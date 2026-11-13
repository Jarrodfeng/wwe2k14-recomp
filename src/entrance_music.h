// wwe2k14 - custom entrance music (Session 33 item 5). Default off (--wwe2k14_entrance_music=true; needs the mods
// folder, --wwe2k14_mods=true).
//
// How the game plays an entrance theme (Session 33, coverage build traces runs 023 / 024, Lesnar vs Bryan launch):
// its music player builds the Wwise event name from a template, "Play_MUS_%MUSICID%_%VARIATION%_%...", in
// sub_82ACFDE0 (r3 = the name buffer, filled in place from the current music object: +164 music id, +168 the
// variation, +172 ...). For Brock Lesnar (character 343) that is "Play_MUS_0343_0_0" (frame 2695, the start of the
// in-game sequence). The play method sub_82AF1B80 posts the event only when sub_82ACFDE0 returns non-zero (call at
// 0x82AF1C80); a per-frame poll sub_82AF1990 builds the same name to compare it with its 32 playing-event slots (call
// at 0x82AF1A90). sub_82ACFDE0 itself already blanks the name and returns 0 when the music object's +184 is set: the
// game's own "do not play" path.
//
// So: with a replacement file for music id N in <mods>\music\ (N.mp3 / .m4a / .wav / .flac / .wma / .aac, any file
// Windows' Media Foundation decodes), the hook blanks the name and returns 0 (the game plays nothing for that theme)
// and starts our file instead when the play method asked (return address 0x82AF1C84). Ours stops when another
// music event is played, when the match starts (match clock running), or when the in-game sequence ends.
// Music ids are written with four digits by the game; the roster's theme id is its character id (343 = Lesnar), so a
// file is named after the wrestler's id like the trons (343.mp3). Unconfirmed for every wrestler (alternative themes
// may use other ids): --wwe2k14_music_probe logs every music event name the game builds.
// Replays and re-shoots keep the original theme (the replay modes are left alone).
#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "platform/compat.h"  // <windows.h> on Windows (Session 63)
#if defined(_WIN32)  // Session 63 (Android port): the player is Media Foundation (Windows only; ANDROID_PLAN F9)
#include <mfplay.h>
#pragma comment(lib, "mfplay.lib")
#endif

#include "frame_stats.h"  // g_guest_frame
#include "guest_read.h"
#include "mods.h"
#include "audio_import/audio_import.h"  // lane 283
#include "music_player_app.h"  // Lane 288: --wwe2k14_music_mix_into_game (the song joins the game's own mix)
#include "music_slots.h"  // Session 75: new songs in the MUSIC list (their ids play through this player)
#include "superstars.h"   // Session 98: imported superstars' music ids

REXCVAR_DEFINE_BOOL(wwe2k14_entrance_music, false, "wwe2k14",
                    "Session 33: custom entrance music from the mods folder (<mods>\\music\\ID.mp3, .m4a, .wav, .flac, "
                    ".wma: the wrestler's id as for the trons): the game's own theme for that wrestler is muted and "
                    "yours plays in its place. Needs --wwe2k14_mods. Default off. See src/entrance_music.h");
REXCVAR_DEFINE_DOUBLE(wwe2k14_entrance_music_volume, 1.0, "wwe2k14",
                      "Session 33: the volume of custom entrance music, 0..1, times the game's own EntranceMusicVolume "
                      "option when --wwe2k14_music_game_volume_addr is set (lane 283; unset = 1)");
REXCVAR_DEFINE_STRING(wwe2k14_music_game_volume_addr, "", "wwe2k14",
                      "Lane 283: guest address (hex, e.g. 83xxxxxx) of the byte holding the game's EntranceMusicVolume option (0..10, "
                      "the options screen's slider). Custom songs play at level/10 of --wwe2k14_entrance_music_volume. Empty = "
                      "not applied (the address is not confirmed yet: docs/systems/music.md, Volume option)");
REXCVAR_DEFINE_BOOL(wwe2k14_music_probe, false, "wwe2k14",
                    "Session 33 (debug): log every music event name the game builds (Play_MUS_...), with the caller");

namespace wwe2k14::music_app {
// ONE volume rule (lane 288): the game's music volume x the player's cvar (see src\music_player_app.h).
inline float EffectiveVolume() {
  return g_game_volume.load() * float(std::clamp(REXCVAR_GET(wwe2k14_entrance_music_volume), 0.0, 1.0));
}
}  // namespace wwe2k14::music_app

REX_EXTERN(__imp__sub_82ACFDE0);

namespace wwe2k14::entrance_music {

// Lane 283: every common audio format is accepted (audio_import::kExts); what plays is always the cached, loudness-normalised
// copy (music_slots::Playable), never the player's own file.
// S101 2026-11-13 (Jarrod, hands-on 7221: a USER PLAYLIST wrestler walked out in silence). At the entrance the game posts the
// Wwise event 'Play_MUS_9998_0_0' (log 7221 21:13:53, not the XMP route), which has no sound. Id 9998 = USER PLAYLIST: the song
// picked in our picker, given by src\user_playlist.h (EntranceFile: the choice under '*', which every pick also writes; the
// entrance cannot name its wrestler yet). Then it plays like any custom theme (Entrance owner, arena room).
inline constexpr int kUserPlaylistId = 9998;
inline std::filesystem::path (*g_user_playlist_file)() = nullptr;
inline std::filesystem::path FindMusicUncached(int id) {
  if (id == kUserPlaylistId) return g_user_playlist_file ? g_user_playlist_file() : std::filesystem::path{};
  std::error_code ec;
  for (const char* ext : audio_import::kExts) {
    auto p = mods::g_dir / "music" / (std::to_string(id) + ext);
    if (std::filesystem::is_regular_file(p, ec)) return music_slots::Playable(p);
  }
  return {};
}
// The hook runs on every poll (each frame): the folder is looked at again for an id at most every 2 s (review fix).
inline std::filesystem::path FindMusic(int id) {
#if !defined(_WIN32)
  if (!music_app::Active()) return {};  // Session 63 (Android port): no MFPlay there, so no theme is muted (ANDROID_PLAN F9)
#endif
  if (!mods::g_on || id < 0) return {};
#if !defined(_WIN32)
  // Android (lane 297, B1): FileFor / FindMusicUncached return the launcher's cached, normalised WAV (music_slots::Playable ->
  // PlayableNow; the game never converts). The player reads WAV only, so a song that is not converted yet (it would play as the
  // source) keeps the game's own theme rather than playing silence.
  if (music_slots::IsSlotId(id)) {
    auto f = music_slots::FileFor(id);
    return !f.empty() && music::WavPcmSource::Sniff(f) ? f : std::filesystem::path{};
  }
#else
  if (music_slots::IsSlotId(id)) return music_slots::FileFor(id);  // Session 75: a new song's own id
#endif
  static std::mutex m;
  static int last_id = -1;
  static std::filesystem::path last_path;
  static std::chrono::steady_clock::time_point last_time;
  std::lock_guard lock(m);
  const auto now = std::chrono::steady_clock::now();
  if (id != last_id || now - last_time > std::chrono::seconds(2)) {
    last_id = id, last_path = FindMusicUncached(id), last_time = now;
  }
#if !defined(_WIN32)
  if (!last_path.empty() && !music::WavPcmSource::Sniff(last_path)) return {};
#endif
  return last_path;
}

// "Play_MUS_0343_0_0" -> 343; -1 for anything else.
inline int MusicIdOf(const char* name) {
  if (std::strncmp(name, "Play_MUS_", 9) != 0) return -1;
  int id = 0, digits = 0;
  for (const char* p = name + 9; *p >= '0' && *p <= '9' && digits < 6; ++p, ++digits) id = id * 10 + (*p - '0');
  return digits ? id : -1;
}

// ---- the player: one worker thread owns the COM apartment and the MFPlay player; the game's threads only post ----
struct Command {
  int id = -1;  // -1 = stop
  std::filesystem::path file;
};
inline std::mutex g_mutex;
inline std::deque<Command> g_queue;
inline bool g_worker_started = false;
inline std::atomic<int> g_playing_id{-1};
inline std::atomic<bool> g_preview{false};  // lane 281: the playing file is a Create-an-Entrance MUSIC preview (src\music_preview.h)

// MFPlay talks to its player through a hidden window on the creating thread: the worker is a single-threaded
// apartment with a message loop (without one the player is created but never starts: run 2523, Session 33).
inline HANDLE g_wake = nullptr;

#if !defined(_WIN32)
inline void Post(Command) {}  // Session 63 (Android port): never reached (FindMusic finds nothing there)
#else
inline void Worker() {
  const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  IMFPMediaPlayer* player = nullptr;
  int playing = -1;
  for (;;) {
    const DWORD r = MsgWaitForMultipleObjects(1, &g_wake, FALSE, INFINITE, QS_ALLINPUT);
    if (r == WAIT_OBJECT_0 + 1) {
      MSG m;
      while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) TranslateMessage(&m), DispatchMessageW(&m);
      continue;
    }
    for (;;) {
      Command c;
      {
        std::lock_guard lock(g_mutex);
        if (g_queue.empty()) break;
        c = std::move(g_queue.front());
        g_queue.pop_front();
      }
      if (player) {
        player->Stop();
        player->Shutdown();
        player->Release();
        player = nullptr;
        REXLOG_INFO("[wwe2k14][music] custom theme {} stopped", playing);
        playing = -1;
      }
      if (c.id < 0) continue;
      const HRESULT hr = MFPCreateMediaPlayer(c.file.c_str(), TRUE, 0, nullptr, nullptr, &player);
      if (FAILED(hr) || !player) {
        player = nullptr;
        g_playing_id = -1;
        REXLOG_WARN("[wwe2k14][music] could not play {} (0x{:08X}): the game's theme stays muted", mods::U8(c.file),
                    uint32_t(hr));
        continue;
      }
      player->SetVolume(music_app::EffectiveVolume());
      playing = c.id;
      REXLOG_INFO("[wwe2k14][music] custom theme {} playing: {}", c.id, mods::U8(c.file));
    }
  }
  (void)co;
}

inline void Post(Command c) {
  std::lock_guard lock(g_mutex);
  if (!g_worker_started) {
    g_worker_started = true;
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::thread(Worker).detach();
  }
  g_queue.push_back(std::move(c));
  SetEvent(g_wake);
}
#endif  // _WIN32
// Lane 283: the optional address of the game's EntranceMusicVolume byte (0..10) feeds the ONE volume rule (music_app::SetGameVolume).
inline void UpdateGameVolumeFromAddr(uint8_t* base) {
  static const uint32_t addr = [] {
    const std::string t = REXCVAR_GET(wwe2k14_music_game_volume_addr);
    return t.empty() ? 0u : uint32_t(std::strtoul(t.c_str(), nullptr, 16));
  }();
  if (!addr || addr < 0x1000 || !GuestWordReadableFast(addr & ~3u)) return;
  const int level = std::clamp(int(*rex::memory::GuestPtr(base, addr)), 0, 10);  // sub_82B20328: byte at +48 + 8 * index
  music_app::SetGameVolume(float(level) / 10.0f);  // linear: the game's curve is unconfirmed
}

// Lane 292 / 288: the USER PLAYLIST machine's end-of-song handler (src\user_playlist.h sets it when the machine is made).
inline void (*g_playlist_event)(const music::Event&) = nullptr;
inline float Volume() { return music_app::EffectiveVolume(); }  // the game's music volume x --wwe2k14_entrance_music_volume

// Lane 288 stop rule: ours stops by the sequence the game was in WHEN IT STARTED, not by "seq != 9" (a song started in
// another sequence, e.g. a preview in the editor, would otherwise stop at once). -1 = unknown.
inline std::atomic<uint8_t*> g_base{nullptr};  // the guest memory base, set by OnFrame every frame
inline std::atomic<int64_t> g_start_seq{-1};
inline bool ReadSeq(uint8_t* base, uint32_t* seq) {
  auto rd = [base](uint32_t a, uint32_t* out) {
    if (a < 0x1000 || (a & 3) || !GuestWordReadableFast(a)) return false;
    uint32_t v;
    std::memcpy(&v, rex::memory::GuestPtr(base, a), 4);
    *out = _byteswap_ulong(v);
    return true;
  };
  uint32_t a = 0, b = 0;
  return rd(0x839400F8, &a) && rd(a, &b) && rd(b + 48, seq);  // states.toml seq
}
inline void NoteStart() {
  uint32_t seq = 0;
  uint8_t* base = g_base.load();
  g_start_seq = base && ReadSeq(base, &seq) ? int64_t(seq) : -1;
}
// Is the playing song the editor preview? With the mix-in player the player itself knows (one voice, one owner).
inline bool PreviewIsCurrent() {
  return music_app::g_created.load() && music_app::Active() ? music_app::Player().CurrentKind() == music::Kind::Preview : g_preview.load();
}

inline void Stop() {
  g_playing_id = -1;
  g_preview = false;
  if (music_app::Active()) {  // lane 288: the music player + SDK inject hook (the game's own mix); not the Playlist's song
    if (music_app::g_created.load()) {
      music_app::Stop(music::Kind::Entrance);
      music_app::Stop(music::Kind::Preview);
    }
    return;
  }
  Post({});
}
inline void Play(int id, std::filesystem::path file) {
  g_playing_id = id;
  g_preview = false;
  NoteStart();
  if (music_app::Active()) {  // a new owner's Start pre-empts the current song, whoever owned it
    music_app::Start(music::Kind::Entrance, file, uint32_t(id), Volume());
    return;
  }
  Post({id, std::move(file)});
}
// Lane 281: the editor's MUSIC preview. Same player and volume; the match / in-game-sequence stop rules of OnFrame do not
// apply (the editor is not in a match); src\music_preview.h stops it on the game's own preview stops.
inline void PlayPreview(int id, std::filesystem::path file) {
  g_playing_id = id;
  g_preview = true;
  NoteStart();
  if (music_app::Active()) {
    music_app::Start(music::Kind::Preview, file, uint32_t(id), Volume());
    return;
  }
  Post({id, std::move(file)});
}

// Defined in src\music_preview.h (it knows the preview object).
inline void OnPreviewFrame(uint8_t* base);

// Called from the game's frame (VdSwap): stop ours when the match starts or the in-game sequence ends.
inline void OnFrame(uint8_t* base) {
  g_base = base;
  UpdateGameVolumeFromAddr(base);
  if (music_app::g_created.load()) {  // lane 288: end-of-song events (the song finished by itself, or could not be decoded)
    music_app::Pump([](const music::Event& ev) {
      if (ev.kind == music::Kind::Playlist && g_playlist_event) g_playlist_event(ev);
      if ((ev.kind == music::Kind::Entrance || ev.kind == music::Kind::Preview) &&
          (ev.type == music::EventType::Ended || ev.type == music::EventType::Failed) && ev.tag == g_playing_id.load()) {
        g_playing_id = -1;
        g_preview = false;
      }
    });
    if (g_playing_id.load() >= 0) music_app::SetVolume(Volume());
  }
  if (g_playing_id.load() < 0) return;
  if (g_preview.load()) {  // lane 281: the preview has its own frame check
    OnPreviewFrame(base);
    return;
  }
  uint32_t clock = 0, seq = 0;
  const bool seq_known = ReadSeq(base, &seq);
  bool clock_known = false;
  if (GuestWordReadableFast(0x836F3928)) {
    uint32_t v;
    std::memcpy(&v, rex::memory::GuestPtr(base, 0x836F3928), 4);
    clock = _byteswap_ulong(v), clock_known = true;
  }
  const bool mix_on = music_app::Active();  // OFF = exactly the old rule (seq != 9 or the clock runs)
  const bool stop = music::EntranceShouldStop(mix_on, seq_known, seq, g_start_seq.load(), clock_known, clock);
  const bool match_started = clock_known && clock > 0;
  if (stop) {  // states.toml seq / match_clock
    REXLOG_INFO("[wwe2k14][music] {}: custom theme stops", match_started ? "the match started" : "the in-game sequence ended");
    Stop();
  }
}

inline bool ReplayLike() {
  return !rex::cvar::GetFlagByName("wwe2k14_replay_play").empty() ||
         !rex::cvar::GetFlagByName("wwe2k14_reshoot").empty() && rex::cvar::GetFlagByName("wwe2k14_reshoot") != "false";
}

}  // namespace wwe2k14::entrance_music

// The music event name builder (see the top of this file).
REX_HOOK_RAW(sub_82ACFDE0) {
  using namespace wwe2k14::entrance_music;
  const uint32_t name = ctx.r3.u32, lr = uint32_t(ctx.lr);
  __imp__sub_82ACFDE0(ctx, base);
  const bool on = REXCVAR_GET(wwe2k14_entrance_music), probe = REXCVAR_GET(wwe2k14_music_probe);
  const bool slots = wwe2k14::music_slots::Active();  // Session 75: the new songs play without --wwe2k14_entrance_music
  const bool stars = wwe2k14::superstars::g_active;   // Session 98: imported superstars play their entrance wrestler's theme
  if ((!on && !probe && !slots && !stars) || !name || ctx.r3.u32 == 0) return;
  // All four call sites pass a 256-byte stack buffer; 48 bytes are plenty for "Play_MUS_0343_0_0" (review fix).
  char buf[48] = {};
  std::memcpy(buf, base + name, sizeof(buf) - 1);
  const bool play = lr == 0x82AF1C84;
  if (stars) {  // "Play_MUS_0397_0_0" -> "Play_MUS_0100_0_0" in the game's own buffer (same length; src\superstars.h)
    const int from = MusicIdOf(buf), to = wwe2k14::superstars::RemapMusic(buf, sizeof(buf));
    if (to >= 0) {
      std::memcpy(base + name + 9, buf + 9, 4);
      if (play) REXLOG_INFO("[wwe2k14][superstars] theme {} -> {} ('{}')", from, to, buf);
    }
  }
  if (probe) {
    static std::mutex probe_mutex;
    std::lock_guard probe_lock(probe_mutex);
    static std::string last;
    if (play || last != buf) {
      last = buf;
      REXLOG_INFO("[wwe2k14][music] event '{}' ({} from 0x{:08X}) frame {}", buf, play ? "play" : "poll", lr,
                  wwe2k14::g_guest_frame.load());
    }
  }
  if (!on && !slots && !g_user_playlist_file) return;
  const int id = MusicIdOf(buf);
  if (play && id != g_playing_id.load() && g_playing_id.load() >= 0) Stop();  // the next theme: ours ends
  if (id < 0 || ReplayLike()) return;
  if (!on && !wwe2k14::music_slots::IsSlotId(id) && id != kUserPlaylistId) return;  // slots alone: only the new songs' ids (+ USER PLAYLIST)
  const auto file = FindMusic(id);
  if (file.empty()) return;
  base[name] = 0;  // as the game's own "do not play" path: no name, return 0
  ctx.r3.u64 = 0;
  if (play) {
    REXLOG_INFO("[wwe2k14][music] theme {} ('{}') muted: {} instead", id, buf, wwe2k14::mods::U8(file.filename()));
    Play(id, file);
  }
}
