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
#include <windows.h>
#include <mfplay.h>
#pragma comment(lib, "mfplay.lib")

#include "frame_stats.h"  // g_guest_frame
#include "guest_read.h"
#include "mods.h"

REXCVAR_DEFINE_BOOL(wwe2k14_entrance_music, false, "wwe2k14",
                    "Session 33: custom entrance music from the mods folder (<mods>\\music\\ID.mp3, .m4a, .wav, .flac, "
                    ".wma: the wrestler's id as for the trons): the game's own theme for that wrestler is muted and "
                    "yours plays in its place. Needs --wwe2k14_mods. Default off. See src/entrance_music.h");
REXCVAR_DEFINE_DOUBLE(wwe2k14_entrance_music_volume, 1.0, "wwe2k14",
                      "Session 33: the volume of custom entrance music, 0..1 (the game's own music volume setting is "
                      "not applied yet)");
REXCVAR_DEFINE_BOOL(wwe2k14_music_probe, false, "wwe2k14",
                    "Session 33 (debug): log every music event name the game builds (Play_MUS_...), with the caller");

REX_EXTERN(__imp__sub_82ACFDE0);

namespace wwe2k14::entrance_music {

inline constexpr const char* kExts[] = {".mp3", ".m4a", ".wav", ".flac", ".wma", ".aac"};

inline std::filesystem::path FindMusicUncached(int id) {
  std::error_code ec;
  for (const char* ext : kExts) {
    auto p = mods::g_dir / "music" / (std::to_string(id) + ext);
    if (std::filesystem::is_regular_file(p, ec)) return p;
  }
  return {};
}
// The hook runs on every poll (each frame): the folder is looked at again for an id at most every 2 s (review fix).
inline std::filesystem::path FindMusic(int id) {
  if (!mods::g_on || id < 0) return {};
  static std::mutex m;
  static int last_id = -1;
  static std::filesystem::path last_path;
  static std::chrono::steady_clock::time_point last_time;
  std::lock_guard lock(m);
  const auto now = std::chrono::steady_clock::now();
  if (id != last_id || now - last_time > std::chrono::seconds(2)) {
    last_id = id, last_path = FindMusicUncached(id), last_time = now;
  }
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

// MFPlay talks to its player through a hidden window on the creating thread: the worker is a single-threaded
// apartment with a message loop (without one the player is created but never starts: run 2523, Session 33).
inline HANDLE g_wake = nullptr;

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
        REXLOG_WARN("[wwe2k14][music] could not play {} (0x{:08X}): the game's theme stays muted", c.file.string(),
                    uint32_t(hr));
        continue;
      }
      player->SetVolume(float(std::clamp(REXCVAR_GET(wwe2k14_entrance_music_volume), 0.0, 1.0)));
      playing = c.id;
      REXLOG_INFO("[wwe2k14][music] custom theme {} playing: {}", c.id, c.file.string());
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
inline void Stop() {
  g_playing_id = -1;
  Post({});
}
inline void Play(int id, std::filesystem::path file) {
  g_playing_id = id;
  Post({id, std::move(file)});
}

// Called from the game's frame (VdSwap): stop ours when the match starts or the in-game sequence ends.
inline void OnFrame(uint8_t* base) {
  if (g_playing_id.load() < 0) return;
  auto rd = [base](uint32_t a, uint32_t* out) {
    if (a < 0x1000 || (a & 3) || !GuestWordReadableFast(a)) return false;
    uint32_t v;
    std::memcpy(&v, rex::memory::GuestPtr(base, a), 4);
    *out = _byteswap_ulong(v);
    return true;
  };
  uint32_t clock = 0, a = 0, b = 0, seq = 0;
  const bool in_game = rd(0x839400F8, &a) && rd(a, &b) && rd(b + 48, &seq) && seq == 9;
  if (!in_game || (rd(0x836F3928, &clock) && clock > 0)) {  // states.toml seq / match_clock
    REXLOG_INFO("[wwe2k14][music] {}: custom theme stops", in_game ? "the match started" : "the in-game sequence ended");
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
  if ((!on && !probe) || !name || ctx.r3.u32 == 0) return;
  // All four call sites pass a 256-byte stack buffer; 48 bytes are plenty for "Play_MUS_0343_0_0" (review fix).
  char buf[48] = {};
  std::memcpy(buf, base + name, sizeof(buf) - 1);
  const bool play = lr == 0x82AF1C84;
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
  if (!on) return;
  const int id = MusicIdOf(buf);
  if (play && id != g_playing_id.load() && g_playing_id.load() >= 0) Stop();  // the next theme: ours ends
  if (id < 0 || ReplayLike()) return;
  const auto file = FindMusic(id);
  if (file.empty()) return;
  base[name] = 0;  // as the game's own "do not play" path: no name, return 0
  ctx.r3.u64 = 0;
  if (play) {
    REXLOG_INFO("[wwe2k14][music] theme {} ('{}') muted: {} instead", id, buf, file.filename().string());
    Play(id, file);
  }
}
