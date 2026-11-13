// wwe2k14 - USER PLAYLIST (lane 282, 2026-11-13; Jarrod: "playtesters ... have the music show up in the 'Custom
// Playlist' category as well"). Songs from the mods folder play when a wrestler's entrance music is USER PLAYLIST
// (music id 9998, the first row of Create-an-Entrance's MUSIC list). Default on with the mods folder
// (--wwe2k14_user_playlist=false turns it off); mods off = today's behaviour (the SDK answers the game's play request
// with "unimplemented", the game marks the track failed: silent entrance).
//
// What the game does (docs\systems\music.md "USER PLAYLIST / XMP", all from the recompiled listings): it never asks
// the Xbox for a song list. A track whose music id is 9998 makes the music manager (sub_82ACC890, XMP player object
// at manager +32, vtable 0x82060EC8) call the player's Play (vtable slot 2, sub_82AF6800), which calls the XDK
// wrapper sub_82E1A138 = XamApp 0xFA message 0x0007003B ("play what the Dashboard player has"), then polls the
// status (sub_82E1A6F0, message 0x00070009; 0 idle, 1 playing, 2 paused; the game reads "playing" as bit 8 of its own
// status word, sub_82AF6B90) and stops it with sub_82E1A220 (0x00070004). The SDK's XmpApp handles 0x70002..0x7002E
// but not 0x7003B, so on PC the play request fails.
//
// So: no enumeration to answer. Our side answers the play request itself. Hooks on the XDK wrappers:
//   Play     sub_82E1A138  fully ours when the playlist has a song (the overlapped, if any, is completed by us)
//   Status   sub_82E1A6F0  ours while our playlist is not Idle, else the SDK's
//   Stop     sub_82E1A220  the SDK's, then ours stops
//   Pause    sub_82E1A208 / Continue sub_82E1A1F0   the SDK's, then ours
//   Volume   sub_82E1A7C0  the SDK's, then ours (the game's music volume)
// The songs (the console-wide library): <mods>\music\library\ when it holds a song, else the new entrance songs of
// <mods>\music\new\. The entrance plays ONE song: the wrestler's choice in <library>\choices.txt, else the first (file-name
// order, scanned at every Play). One song after the other, no repeat (the list ends = Idle, as the Dashboard's single
// song playlist does). The core is src\user_playlist_core.h (host-tested); this file is the guest glue and the
// Media Foundation player (its own worker thread; the shared entrance player of src\entrance_music.h is untouched).
// Replays and re-shoots are left alone. Windows only (no player on Android: the hooks pass straight through).
#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "platform/compat.h"
#include "music_slots.h"  // lane 283: Playable() - the cached, normalised copy of a library song
#if defined(_WIN32)
#include <mfplay.h>
#pragma comment(lib, "mfplay.lib")
#endif

#include "frame_stats.h"
#include "guest_read.h"
#include "entrance_music.h"
#include "mods.h"
#include "user_playlist_core.h"

REXCVAR_DEFINE_BOOL(wwe2k14_user_playlist, true, "wwe2k14",
                    "Lane 282: USER PLAYLIST (the first row of the entrance MUSIC list) plays the songs of "
                    "<mods>\\music\\library\\ (else the new entrance songs of <mods>\\music\\new\\), one after the "
                    "other. Needs --wwe2k14_mods. See src/user_playlist.h");

REX_EXTERN(__imp__sub_82E1A138);
REX_EXTERN(__imp__sub_82E1A6F0);
REX_EXTERN(__imp__sub_82E1A220);
REX_EXTERN(__imp__sub_82E1A208);
REX_EXTERN(__imp__sub_82E1A1F0);
REX_EXTERN(__imp__sub_82E1A7C0);

namespace wwe2k14::user_playlist {

// The machine is allocated once (TheMachine) and never destroyed: the player thread and the hooks may touch it at exit.
// Null = the playlist never started. An atomic pointer with the pointer's syntax.
struct MachinePtr {
  std::atomic<Machine*> p{nullptr};
  Machine* operator->() const { return p.load(std::memory_order_acquire); }
  explicit operator bool() const { return p.load(std::memory_order_acquire) != nullptr; }
  MachinePtr& operator=(Machine* m) {
    p.store(m, std::memory_order_release);
    return *this;
  }
};
inline MachinePtr g_machine;
// The game's music volume (XMPSetVolume, sub_82E1A7C0). The XMP player constructor sets it before the first Play, when
// no machine exists yet: always kept here, and it is the machine's starting volume (lane 291, review S2).
inline std::atomic<float> g_volume{1.0f};
// The in-game sequence number at the last Play (-1 = unknown): OnFrame stops when it leaves (review S1).
inline std::atomic<int32_t> g_play_seq{-1};

#if !defined(_WIN32)
// Android: no player of its own (lane 297 owns Android music). Machine and hooks exist so the file compiles; Active() is false, so every
// hook passes through.
class NullPlayer : public PlayerIf {
 public:
  bool Start(const std::filesystem::path&, uint32_t, float) override { return false; }
  void Stop() override {}
  void Pause(bool) override {}
  void SetVolume(float) override {}
};
#else

// One worker thread owns the COM apartment and the MFPlay player (as src\entrance_music.h: MFPlay needs a message
// loop on the creating thread). The game threads and the machine only post commands.
class MfPlayer : public PlayerIf {
 public:
  bool Start(const std::filesystem::path& file, uint32_t token, float volume) override {
    Post({Cmd::Start, file, token, volume, false});
    return true;  // a song the decoder refuses is reported later (OnSongEnded(token, false))
  }
  void Stop() override { Post({Cmd::Stop, {}, 0, 0.0f, false}); }
  void Pause(bool paused) override { Post({Cmd::Pause, {}, 0, 0.0f, paused}); }
  void SetVolume(float v) override { Post({Cmd::Volume, {}, 0, v, false}); }

 private:
  enum class Cmd { Start, Stop, Pause, Volume };
  struct Command {
    Cmd cmd;
    std::filesystem::path file;
    uint32_t token;
    float volume;
    bool paused;
  };

  // The MFPlay event sink of one song start.
  class Sink : public IMFPMediaPlayerCallback {
   public:
    explicit Sink(uint32_t token) : token_(token) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
      if (riid == __uuidof(IMFPMediaPlayerCallback) || riid == __uuidof(IUnknown)) {
        *out = static_cast<IMFPMediaPlayerCallback*>(this);
        AddRef();
        return S_OK;
      }
      *out = nullptr;
      return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ULONG(InterlockedIncrement(&ref_)); }
    STDMETHODIMP_(ULONG) Release() override {
      const ULONG r = ULONG(InterlockedDecrement(&ref_));
      if (!r) delete this;
      return r;
    }
    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* h) override {
      if (!h || !g_machine || dead_.load()) return;
      if (h->eEventType == MFP_EVENT_TYPE_PLAYBACK_ENDED) {
        dead_ = true;
        g_machine->OnSongEnded(token_, true);
      } else if (FAILED(h->hrEvent) && h->eEventType != MFP_EVENT_TYPE_POSITION_SET &&
                 h->eEventType != MFP_EVENT_TYPE_RATE_SET) {
        dead_ = true;
        REXLOG_WARN("[wwe2k14][user_playlist] a song could not be played (event {}, 0x{:08X}): next one",
                    int(h->eEventType), uint32_t(h->hrEvent));
        g_machine->OnSongEnded(token_, false);
      }
    }
    void Kill() { dead_ = true; }

   private:
    LONG ref_ = 1;
    uint32_t token_;
    std::atomic<bool> dead_{false};
  };

  void Post(Command c) {
    std::lock_guard lock(m_);
    if (!started_) {
      started_ = true;
      wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      std::thread([this] { Worker(); }).detach();
    }
    q_.push_back(std::move(c));
    SetEvent(wake_);
  }

  void Worker() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IMFPMediaPlayer* player = nullptr;
    Sink* sink = nullptr;
    auto drop = [&] {
      if (sink) sink->Kill();
      if (player) {
        player->Stop();
        player->Shutdown();
        player->Release();
        player = nullptr;
      }
      if (sink) sink->Release(), sink = nullptr;
    };
    for (;;) {
      const DWORD r = MsgWaitForMultipleObjects(1, &wake_, FALSE, INFINITE, QS_ALLINPUT);
      if (r == WAIT_OBJECT_0 + 1) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) TranslateMessage(&msg), DispatchMessageW(&msg);
        continue;
      }
      for (;;) {
        Command c;
        {
          std::lock_guard lock(m_);
          if (q_.empty()) break;
          c = std::move(q_.front());
          q_.pop_front();
        }
        switch (c.cmd) {
          case Cmd::Start: {
            drop();
            sink = new Sink(c.token);
            const HRESULT hr = MFPCreateMediaPlayer(c.file.c_str(), TRUE, 0, sink, nullptr, &player);
            if (FAILED(hr) || !player) {
              player = nullptr;
              sink->Kill();
              sink->Release(), sink = nullptr;
              REXLOG_WARN("[wwe2k14][user_playlist] could not play {} (0x{:08X}): next one", U8(c.file),
                          uint32_t(hr));
              if (g_machine) g_machine->OnSongEnded(c.token, false);
              break;
            }
            player->SetVolume(c.volume);
            REXLOG_INFO("[wwe2k14][user_playlist] playing {}", U8(c.file));
            break;
          }
          case Cmd::Stop:
            drop();
            break;
          case Cmd::Pause:
            if (player) c.paused ? player->Pause() : player->Play();
            break;
          case Cmd::Volume:
            if (player) player->SetVolume(c.volume);
            break;
        }
      }
    }
    (void)co;
  }

  std::mutex m_;
  std::deque<Command> q_;
  bool started_ = false;
  HANDLE wake_ = nullptr;
};
#endif  // _WIN32

// Lane 288 (the shared player, src\music_player_app.h): with --wwe2k14_music_mix_into_game the playlist's song is decoded by the shared player
// (Kind::Playlist) and mixed into the game's own audio by the SDK inject hook: one output, one voice (a new owner's Start pre-empts the
// current song), the game's volume x the player's cvar. Off (the default) = the Media Foundation player above, as before. The end of a song
// comes back through music_app::Pump (entrance_music.h OnFrame -> g_playlist_event below).
class MixPlayer : public PlayerIf {
 public:
  bool Start(const std::filesystem::path& file, uint32_t token, float) override {
    wwe2k14::music_app::Start(wwe2k14::music::Kind::Playlist, file, token, wwe2k14::music_app::EffectiveVolume());
    return true;  // a song the decoder refuses comes back as a Failed event
  }
  void Stop() override {
    if (wwe2k14::music_app::g_created.load()) wwe2k14::music_app::Stop(wwe2k14::music::Kind::Playlist);
  }
  // MusicPlayer::Pause / SetVolume act on whatever voice is playing: only when that voice is ours (an entrance theme or the editor's preview
  // that pre-empted the playlist is not paused or re-volumed by the playlist's Pause / the game's volume calls).
  void Pause(bool paused) override {
    if (OursPlaying()) wwe2k14::music_app::Pause(paused);
  }
  void SetVolume(float v) override {
    wwe2k14::music_app::SetGameVolume(v);
    if (OursPlaying()) wwe2k14::music_app::SetVolume(wwe2k14::music_app::EffectiveVolume());
  }

 private:
  static bool OursPlaying() {
    return wwe2k14::music_app::g_created.load() && wwe2k14::music_app::Player().CurrentKind() == wwe2k14::music::Kind::Playlist;
  }
};

// Picks the player per call by the cvar; when it changed since the last Start the other player is stopped first (never two voices).
class SwitchPlayer : public PlayerIf {
 public:
  bool Start(const std::filesystem::path& file, uint32_t token, float v) override {
    const bool mix = wwe2k14::music_app::Active();
    if (mix != mixing_) Cur(mixing_).Stop(), mixing_ = mix;
    // lane 283: the loudness-normalised copy the launcher made (the song itself when it is not converted yet; empty = refused, or a
    // format this player cannot open unconverted: the machine goes on to the next song)
    return StartVia(Cur(mix), [](const std::filesystem::path& f) { return music_slots::Playable(f); }, file, token, v);
  }
  void Stop() override { Cur(mixing_).Stop(); }
  void Pause(bool p) override { Cur(mixing_).Pause(p); }
  void SetVolume(float v) override { Cur(mixing_).SetVolume(v); }

 private:
  PlayerIf& Cur(bool mix) {
    if (mix) return mix_;
#if defined(_WIN32)
    return mf_;
#else
    return null_;
#endif
  }
  bool mixing_ = false;
  MixPlayer mix_;
#if defined(_WIN32)
  MfPlayer mf_;
#else
  NullPlayer null_;
#endif
};
inline SwitchPlayer g_player;

inline Machine& TheMachine() {
  static Machine* m = [] {
    auto* mm = new Machine(g_player, [](const std::filesystem::path& p) {
      std::error_code ec;
      return std::filesystem::is_regular_file(p, ec);
    });
    mm->SetVolume(g_volume.load());  // the volume the game set before any Play (review S2)
    g_machine = mm;
    // Lane 288: end-of-song events of the shared player (Pump runs in entrance_music.h OnFrame).
    wwe2k14::entrance_music::g_playlist_event = [](const wwe2k14::music::Event& e) {
      if (!g_machine) return;
      if (e.type == wwe2k14::music::EventType::Stopped) {
        // Another owner pre-empted the playlist's song (an entrance theme, the editor's preview): the machine is not Playing any more. A
        // Stopped for an older token (the machine's own Stop / next song) does not match and is ignored.
        if (uint32_t(e.tag) == g_machine->token() && g_machine->state() != State::Idle) g_machine->Stop();
        return;
      }
      if (e.type != wwe2k14::music::EventType::Ended && e.type != wwe2k14::music::EventType::Failed) return;
      g_machine->OnSongEnded(uint32_t(e.tag), e.type == wwe2k14::music::EventType::Ended);
    };
    return mm;
  }();
  return *m;
}

inline bool ReplayLike() {
  return !rex::cvar::GetFlagByName("wwe2k14_replay_play").empty() ||
         !rex::cvar::GetFlagByName("wwe2k14_reshoot").empty() && rex::cvar::GetFlagByName("wwe2k14_reshoot") != "false";
}

inline bool Enabled() {
#if defined(__ANDROID__)  // lane 297 (B1): the phone has the lane 288 player (the launcher passes the mix flag with USE MY MUSIC)
  return REXCVAR_GET(wwe2k14_user_playlist) && mods::g_on && music_slots::MixOn() && !ReplayLike();
#elif !defined(_WIN32)
  return false;
#else
  return REXCVAR_GET(wwe2k14_user_playlist) && mods::g_on && !ReplayLike();
#endif
}

// The console-wide music library (Jarrod, 2026-11-13): <mods>\music\library\. Falls back to <mods>\music\new\ when empty.
inline std::filesystem::path LibraryDir() { return mods::g_dir / "music" / "library"; }

// Which wrestler a choice belongs to (lane 309). EDITOR side [C = read in the listings, [U] until the next hands-on run prints it]:
// the Create-an-Entrance screen object S (sub_83355A30's r3 = the screen sub_833564F0 drives) keeps the edited character's index at
// S+84 (set by the constructors sub_83356BF0 / sub_83356DC0 from their argument block +16; sub_83355A30 reads the entrance record
// as sub_822C0888(S+84) + 780 = sub_827DA658(index): < 100 a Create-a-Wrestler slot, else the roster record). The picker and the
// editor preview's Play (the two things the editor does with USER PLAYLIST) read it here. It is only trusted while the editor's
// preview object M (global 0x839BFC3C, music_preview.h kGlobalM) exists: M is destroyed when the editor is left.
// ENTRANCE side (a match): the wrestler is NOT readable yet (the music object carries only the id 9998); an unreadable key = "" =
// the kAnyKey line, then the first song. The trace (--wwe2k14_music_picker_trace) logs what the next run needs to name it.
constexpr uint32_t kEditorPreviewGlobal = 0x839BFC3C;  // the same global as music_preview.h kGlobalM (pointer to M, 0 = no editor)
constexpr uint32_t kEditorCharOffset = 84;             // S+84: the edited character's index (see above)
inline std::atomic<uint32_t> g_cae_screen{0};          // the latest Create-an-Entrance screen object S (hook below); 0 = none seen

// Words of guest memory, big-endian. False = unreadable (never an access violation: GuestWordReadableFast).
inline bool RdWord(uint8_t* base, uint32_t a, uint32_t* out) {
  if (a < 0x1000 || (a & 3) || !GuestWordReadableFast(a)) return false;
  uint32_t v;
  std::memcpy(&v, rex::memory::GuestPtr(base, a), 4);
  *out = ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
  return true;
}
// The edited character's index while the editor is alive, -1 otherwise (any unreadable link).
inline int64_t EditorCharacter(uint8_t* base) {
  uint32_t m = 0, id = 0;
  const uint32_t s = g_cae_screen.load();
  if (!s || !RdWord(base, kEditorPreviewGlobal, &m) || m == 0 || !RdWord(base, s + kEditorCharOffset, &id)) return -1;
  return int64_t(int32_t(id));
}
// "" = unknown (no editor, or an id outside 0..4095): the callers use the kAnyKey line.
inline std::string CurrentKey(uint8_t* base) {
  try {
    return KeyForCharacter(EditorCharacter(base));
  } catch (...) {
    return {};
  }
}

// File names as UTF-8 (U8: path::string() throws on a name the ANSI code page cannot hold, review B2). Anything that goes wrong in the
// folder walk = what was read so far (the caller shows "no songs", the game's own behaviour).
inline std::vector<std::string> NamesIn(const std::filesystem::path& dir) {
  std::vector<std::string> out;
  try {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
      if (e.is_regular_file(ec)) out.push_back(U8(e.path().filename()));
    }
  } catch (...) {
    REXLOG_WARN("[wwe2k14][user_playlist] could not list {}", U8(dir));
  }
  return out;
}

// choices.txt, at most 64 KB (a hand-edited or damaged file cannot make the hooks read megabytes).
inline std::string ReadChoices() {
  std::string text;
  try {
    std::ifstream in(LibraryDir() / "choices.txt", std::ios::binary | std::ios::ate);
    constexpr std::streamoff kMax = 64 << 10;
    std::streamoff size = in ? std::streamoff(in.tellg()) : 0;
    if (size <= 0) return text;
    // Lines are appended / rewritten at the END and the last line of a key wins: a file over the cap keeps its newest 64 KB (from the next
    // whole line), never its oldest.
    const std::streamoff start = size > kMax ? size - kMax : 0;
    in.seekg(start);
    text.resize(size_t(size - start));
    in.read(text.data(), std::streamsize(text.size()));
    text.resize(size_t(std::max<std::streamsize>(0, in.gcount())));
    if (start > 0) {
      const size_t nl = text.find('\n');
      text.erase(0, nl == std::string::npos ? text.size() : nl + 1);
    }
  } catch (...) {
    text.clear();
  }
  return text;
}

// Looks at the folders again and hands the machine the list. Returns the number of songs.
inline size_t Rescan() {
  const auto playlist_dir = LibraryDir();
  const auto new_dir = mods::g_dir / "music" / "new";
  std::error_code mk;
  std::filesystem::create_directories(playlist_dir, mk);  // so the launcher's MUSIC tab can open it
  const auto p = NamesIn(playlist_dir), n = NamesIn(new_dir);
  const int which = ChooseFolder(p, n);
  std::vector<std::filesystem::path> songs;
  if (which >= 0) {
    // The list keeps the SOURCE paths (the picker names rows by file name, reads their tags, and choices.txt matches by file name); the
    // converted copy is substituted only where the machine hands a song to the player (SwitchPlayer::Start).
    for (const auto& name : PickSongs(which == 0 ? p : n)) songs.push_back((which == 0 ? playlist_dir : new_dir) / FromU8(name));
  }
  TheMachine().SetSongs(songs);
  static int last = -2;
  static size_t last_count = size_t(-1);
  if (which != last || songs.size() != last_count) {
    last = which, last_count = songs.size();
    REXLOG_INFO("[wwe2k14][user_playlist] {} song(s) from {}", songs.size(),
                which == 0 ? U8(playlist_dir) : which == 1 ? U8(new_dir) : std::string("(no folder with songs)"));
  }
  return songs.size();
}

// S101 2026-11-13: the file a USER PLAYLIST (9998) entrance plays through src\entrance_music.h (the game posts the Wwise event
// Play_MUS_9998_0_0 at the entrance: run 7221). The '*' choice (written with every pick; the entrance cannot name its wrestler yet),
// else the first song, as the XMP path would choose; the converted copy as the machine plays it (music_slots::Playable). Empty =
// none, and on a phone only a converted WAV (as FindMusic's other ids). entrance_music caches the answer for 2 s.
inline std::filesystem::path EntranceFile() {
  if (!Enabled()) return {};
  try {
    if (Rescan() == 0) return {};
    const auto songs = TheMachine().Songs();
    const size_t i = ChooseSong(ParseChoices(ReadChoices()), "", songs);
    if (i == size_t(-1)) return {};
    auto f = music_slots::Playable(songs[i]);
#if !defined(_WIN32)
    if (f.empty() || !music::WavPcmSource::Sniff(f)) return {};
#endif
    static std::filesystem::path last;  // the poll asks every 2 s: log a change only
    if (f != last) {
      REXLOG_INFO("[wwe2k14][user_playlist] entrance: USER PLAYLIST plays {}", U8(songs[i].filename()));
      last = f;
    }
    return f;
  } catch (...) {
    return {};
  }
}
inline const bool g_entrance_file_registered = (entrance_music::g_user_playlist_file = &EntranceFile, true);

// XOVERLAPPED (28 bytes): +0 InternalLow (997 = pending), +24 dwExtendedError. The SDK path would complete it; ours does.
inline void CompleteOverlapped(uint8_t* base, uint32_t ovl) {
  if (ovl < 0x1000 || (ovl & 3) || !GuestWordReadableFast(ovl) || !GuestWordReadableFast(ovl + 24)) return;
  std::memset(rex::memory::GuestPtr(base, ovl), 0, 8);
  std::memset(rex::memory::GuestPtr(base, ovl + 24), 0, 4);
}

inline void PutBe32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }

// Called from the game's frame: the Dashboard's playlist is the game's to stop (it calls Stop), but a sequence that ends
// without a Stop must not leave our songs running into the match (same reads as entrance_music::OnFrame).
inline void OnFrame(uint8_t* base) {
  if (!g_machine || g_machine->state() == State::Idle) return;
  auto rd = [base](uint32_t a, uint32_t* out) {
    if (a < 0x1000 || (a & 3) || !GuestWordReadableFast(a)) return false;
    uint32_t v;
    std::memcpy(&v, rex::memory::GuestPtr(base, a), 4);
    *out = ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
    return true;
  };
  uint32_t clock = 0, a = 0, b = 0, seq = 0;
  const bool seq_ok = rd(0x839400F8, &a) && rd(a, &b) && rd(b + 48, &seq);
  const bool clock_ok = rd(0x836F3928, &clock);
  const StopWhy why = ShouldStop(g_play_seq.load(), seq_ok ? int32_t(seq) : -1, clock_ok ? clock : 0);
  if (why != StopWhy::None) {
    REXLOG_INFO("[wwe2k14][user_playlist] {}: stops", why == StopWhy::MatchStarted ? "the match started" : "the sequence ended");
    g_machine->Stop();
  }
}

// The in-game sequence number now (-1 = unreadable). Same reads as OnFrame.
inline int32_t ReadSeq(uint8_t* base) {
  auto rd = [base](uint32_t a, uint32_t* out) {
    if (a < 0x1000 || (a & 3) || !GuestWordReadableFast(a)) return false;
    uint32_t v;
    std::memcpy(&v, rex::memory::GuestPtr(base, a), 4);
    *out = ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
    return true;
  };
  uint32_t a = 0, b = 0, seq = 0;
  return rd(0x839400F8, &a) && rd(a, &b) && rd(b + 48, &seq) ? int32_t(seq) : -1;
}

}  // namespace wwe2k14::user_playlist

// ---- the hooks (see the top of this file) ----

// The Create-an-Entrance screen's menu build (lane 309): r3 = the screen object S, whose +84 is the edited character (CurrentKey).
REX_EXTERN(__imp__sub_83355A30);
REX_HOOK_RAW(sub_83355A30) {
  wwe2k14::user_playlist::g_cae_screen.store(ctx.r3.u32);
  __imp__sub_83355A30(ctx, base);
}

// S101 2026-11-13 (Jarrod, phone, log 061: after the USER PLAYLIST pick the editor preview stayed silent; SetSong(9998) logged, no
// "play asked"). Listings read (0 runs): before Play both the editor's sub-player tick (sub_8334B4B0 0x8334B508) and the entrance's
// music manager (sub_82ACDAC8 0x82ACDBFC) call sub_82AF6730(xmp, name), which calls sub_82AF6538(name, xmp+32): it ENUMERATES the
// Xbox playlists (sub_82E1A458, 25 at a time) and compares their names. No Xbox playlists exist here, so it returns 0, sub_82AF6730
// returns 1, the editor's tick goes back to idle and the entrance marks its 9998 track failed (state 8): Play is never asked.
// (This corrects "the game never enumerates" above: sub_82E1A458 was not in that list.) While our library serves USER PLAYLIST,
// the lookup says "found" (1) without enumerating; sub_82AF6730 then sets xmp+860 = 1 and returns 0, and the game goes on to
// Play (vtable 8 -> sub_82AF6800 -> sub_82E1A138, the hook below picks the song). sub_82AF6538 has one caller, sub_82AF6730.
REX_EXTERN(__imp__sub_82AF6538);
REX_HOOK_RAW(sub_82AF6538) {
  if (!wwe2k14::user_playlist::Enabled()) return __imp__sub_82AF6538(ctx, base);
  static std::atomic<int> n{0};
  if (n.fetch_add(1) < 8) REXLOG_INFO("[wwe2k14][user_playlist] playlist lookup answered 'found' (our library serves USER PLAYLIST)");
  ctx.r3.u64 = 1;
}

// XMPPlay (0x7003B): r3 = the 820-byte playlist block (zero-filled by the game), r4 = overlapped or 0. 0 = ok.
REX_HOOK_RAW(sub_82E1A138) {
  using namespace wwe2k14::user_playlist;
  if (!Enabled()) return __imp__sub_82E1A138(ctx, base);
  const uint32_t ovl = ctx.r4.u32;
  size_t first = size_t(-1);
  try {
    if (Rescan() != 0) {  // the machine's list = what Rescan chose; the sidecar sits in the library folder
      const std::string key = CurrentKey(base);
      first = ChooseSong(ParseChoices(ReadChoices()), key, TheMachine().Songs());
      // Lane 309: which wrestler's line decided (the next hands-on run reads it: editor preview = the edited character, entrance = '' until
      // the entrance side can name its wrestler).
      REXLOG_INFO("[wwe2k14][user_playlist] play asked: key '{}' -> song {} (seq {})", key,
                  first != size_t(-1) ? U8(TheMachine().Songs()[first].filename()) : std::string("none"), ReadSeq(base));
    }
  } catch (...) {  // never an exception out of a guest hook: the game's own behaviour (the SDK answers)
    REXLOG_WARN("[wwe2k14][user_playlist] could not read the library: the SDK answers");
    return __imp__sub_82E1A138(ctx, base);
  }
  // Two songs at once (review S3): a custom theme of the previous wrestler is never stopped by a 9998 track.
  if (first != size_t(-1) && wwe2k14::entrance_music::g_playing_id.load() >= 0) wwe2k14::entrance_music::Stop();
  if (first == size_t(-1) || TheMachine().Play(first, true) != 0) {
    REXLOG_INFO("[wwe2k14][user_playlist] play asked, no playable song in the mods folder: the SDK answers");
    return __imp__sub_82E1A138(ctx, base);
  }
  g_play_seq = ReadSeq(base);  // OnFrame stops when this sequence is left (the editor's 25 or the entrance's 9)
  CompleteOverlapped(base, ovl);
  ctx.r3.u64 = 0;
}

// XMPGetStatus (0x70009): r3 = client, r4 = out (u32 BE).
REX_HOOK_RAW(sub_82E1A6F0) {
  using namespace wwe2k14::user_playlist;
  const uint32_t out = ctx.r4.u32;
  if (g_machine && g_machine->state() != State::Idle && out >= 0x1000 && !(out & 3) &&
      wwe2k14::GuestWordReadableFast(out)) {
    PutBe32(rex::memory::GuestPtr(base, out), uint32_t(g_machine->state()));
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82E1A6F0(ctx, base);
}

// XMPStop (0x70004): r3 = client, r5 = overlapped or 0 (sub_82E1A2C0(ovl) tail-calls here with r3 = 2, r4 = 0, r5 = ovl).
// S101 2026-11-13 (hands-on run 7220: leaving Create-an-Entrance after a USER PLAYLIST preview hung on the loading screen). Since
// the playlist lookup answers "found", the game's Play succeeds and sets xmp+860; on leaving, the editor's sub-player stops it
// (sub_82AF67B8 -> Stop sub_82AF6DF8 -> sub_82E1A2C0 with the overlapped at xmp+4, xmp+852 = 1) and waits until
// sub_82AF6C70 sees that overlapped done. The SDK never played anything (our Play hook answers it), so its stop was never seen to
// complete [cause read from the listings + the log going silent after 20:44:21; the fix is checked by the next hands-on run].
// While our library serves USER PLAYLIST the stop is ours, like Play: stop our player, complete the overlapped, answer 0.
REX_HOOK_RAW(sub_82E1A220) {
  using namespace wwe2k14::user_playlist;
  if (!Enabled()) {
    __imp__sub_82E1A220(ctx, base);
    if (g_machine) g_machine->Stop();
    return;
  }
  if (g_machine) g_machine->Stop();
  CompleteOverlapped(base, ctx.r5.u32);
  static std::atomic<int> n{0};
  if (n.fetch_add(1) < 8) REXLOG_INFO("[wwe2k14][user_playlist] stop answered by us (overlapped {:08X})", ctx.r5.u32);
  ctx.r3.u64 = 0;
}
// XMPPause (0x70005) / XMPContinue (0x70003)
REX_HOOK_RAW(sub_82E1A208) {
  __imp__sub_82E1A208(ctx, base);
  if (wwe2k14::user_playlist::g_machine) wwe2k14::user_playlist::g_machine->Pause();
}
REX_HOOK_RAW(sub_82E1A1F0) {
  __imp__sub_82E1A1F0(ctx, base);
  if (wwe2k14::user_playlist::g_machine) wwe2k14::user_playlist::g_machine->Continue();
}
// XMPSetVolume (0x7000C): r3 = client, f1 = the volume (the game's music volume times its constant).
REX_HOOK_RAW(sub_82E1A7C0) {
  const float v = float(ctx.f1.f64);
  {  // S101: the loudness check reads the volume the game asks for (its music option x the constant sub_82ACD288 stores at 0x83628CC0)
    static std::atomic<int> n{0};
    if (n.fetch_add(1) < 12) REXLOG_INFO("[wwe2k14][user_playlist] the game sets the user-music volume {:.4f}", v);
  }
  wwe2k14::user_playlist::g_volume = v;  // kept even before the machine exists (review S2)
  if (wwe2k14::music_app::Active()) wwe2k14::music_app::SetGameVolume(v);  // lane 288: one volume rule for entrance, preview and playlist
  __imp__sub_82E1A7C0(ctx, base);
  if (wwe2k14::user_playlist::g_machine) wwe2k14::user_playlist::g_machine->SetVolume(v);
}
