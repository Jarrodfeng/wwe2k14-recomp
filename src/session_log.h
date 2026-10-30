// wwe2k14 - the session timeline for problem reports (Session 58, Jarrod: "as much a complete understanding of a user's
// last session as possible"; the performance test plays the same match every time). Default on, players too.
//
//   [wwe2k14][scene] lines: every change of the game's sequence (main menu and setup screens, 30 Years of WrestleMania,
//     the create modes, in-game), of the in-game scene word (4 = the match load and the entrances, 1 = in the ring) and
//     the match clock starting, with the guest frame, the seconds since the game started and the frames / fps of the
//     part before.
//   [wwe2k14][match] card lines: when the in-game sequence starts and when the match clock starts: mode, match type,
//     arena, the extra ids and each participant's id and slot word, from the match description M (src/scenario.h,
//     docs/systems/match_setup.md). The launcher's report names the ids (data\ids\characters.json).
//   Snapshots: a small JPEG (captures/NNN_scene_K.jpg, --wwe2k14_session_snapshot_width wide) 90 frames after each of
//     those changes and every 3600 frames in the ring, at most --wwe2k14_session_snapshots per run (0 = none). Off in
//     harness runs (an input script or the live channel) unless set on the command line. At start, the snapshots of
//     all but the 5 newest earlier runs are deleted.
//
// Addresses: the sequence id chain and the scene word / match clock words are tools/input_scripts/states.toml's
// (seq = 0x839400F8>0>48, scene_word = 0x836F3940, match_clock = 0x836F3928); M is scenario::kMatchDescPtr.
#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "input_script.h"  // g_session_frame_hook, SaveSnapshot
#include "load_probe.h"    // CurrentSequence, SafeRead32
#include "scenario.h"      // kMatchDescPtr

REXCVAR_DEFINE_BOOL(wwe2k14_session_log, true, "wwe2k14",
                    "Session 58: [scene] lines for every sequence / scene change and [match] card lines (who, where, "
                    "which match) in the log, for Report a problem. See src/session_log.h");
REXCVAR_DEFINE_INT32(wwe2k14_session_snapshots, 60, "wwe2k14",
                     "Session 58: at most this many small JPEG snapshots per run (captures/NNN_scene_K.jpg): after each "
                     "scene change and every 3600 frames in the ring; 0 = none (off in harness runs unless set)")
    .range(0, 1000);
REXCVAR_DEFINE_INT32(wwe2k14_session_snapshot_width, 640, "wwe2k14",
                     "Session 58: the scene snapshots' largest width in pixels")
    .range(160, 3840);

namespace wwe2k14::session_log {

constexpr uint32_t kSceneWord = 0x836F3940;   // states.toml scene_word
constexpr uint32_t kMatchClock = 0x836F3928;  // states.toml match_clock

// Session 61: every id with the name the game registers it under (the second argument of the sequence base
// constructor sub_82C3A298 at each of its 27 call sites; docs\systems\menu_ui.md), our own description first where
// we have one.
inline const char* SequenceName(uint32_t seq) {
  switch (seq) {
    case 3: return "startup / title: CStartupCtrlSequence";
    case 4: return "OverlaySequence";
    case 5: return "CInGameStoreSequence";
    case 7: return "CExhibitionEndSequence";
    case 9: return "in-game: CGameMainSequence";
    case 10: return "CCutScenePreviewSequence";
    case 11: return "story / cutscenes: CCutSceneStartupSequence";
    case 14: return "SequenceSelectSequence";
    case 20: return "MovesMenuSequence";
    case 21: return "COnlineMenuSequence";
    case 22: return "COMS_ResultMenuSequence";
    case 23: return "CUCC_ResultMenuSequence";
    case 25: return "Create an Entrance: CreateEntranceSequence";
    case 26: return "Create a Finisher: CCafMenuSequence";
    case 27: return "CCahSequenceBase";
    case 28: return "Create an Arena: Create a Ring Menu Sequence";
    case 29: return "Create a Title: CAB MENU";
    case 30: return "CTitleMatchResultSequence / CKotRResultSequence";
    case 31: return "30 Years of WrestleMania: CRTWBeginSequence";
    case 35: return "CWM30MeunSequence";
    case 36: return "CStreakMeunSequence";
    case 75: return "main menu / match setup: CSmartMenuSequence";
    case 76: return "Create a Superstar: Create a Superstar Ex Menu Sequence";
    case 77: return "SuperstarThreads Menu Sequence";
    case 78: return "Create a Logo: PaintTool Sequence";
    case 82: return "CWWEUniverseNextMatchSequence";
    default: return "?";
  }
}

struct State {
  uint32_t seq = 0, scene = 0;
  bool clock_running = false;
  uint64_t part_frame = 0;  // the frame the current part began
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now(), part_time = start;
  uint64_t snapshot_due = 0, next_ring_snapshot = 0;
  int snapshots = 0;
};
inline State g_state;

inline uint32_t Read(uint32_t address) {
  uint32_t v = 0;
  return load_probe::SafeRead32(address, &v) ? v : 0;
}

inline void LogMatchCard(const char* when, uint64_t frame) {
  const uint32_t m = Read(scenario::kMatchDescPtr);
  if (!m) return;
  std::string who;
  for (uint32_t i = 0; i < 43; ++i) {
    const uint32_t e = m + 724 + 3580 * i;
    const uint32_t id = Read(e + 56);
    if (id >= 500) continue;
    who += fmt::format("{}{}:{} (slot {})", who.empty() ? "" : ", ", i, id, Read(e + 16));
  }
  REXLOG_INFO("[wwe2k14][match] card ({}): mode {} type {} arena {} extras [{}, {}, {}]; participants {} frame={}", when,
              int32_t(Read(m + 4)), int32_t(Read(m + 8)), int32_t(Read(m + 532)), int32_t(Read(m + 544)),
              int32_t(Read(m + 548)), int32_t(Read(m + 552)), who.empty() ? "(none)" : who, frame);
}

inline void Snapshot() {
  State& s = g_state;
  if (s.snapshots >= REXCVAR_GET(wwe2k14_session_snapshots)) return;
  ++s.snapshots;
  SaveSnapshot(fmt::format("scene_{}", s.snapshots), uint32_t(REXCVAR_GET(wwe2k14_session_snapshot_width)));
}

// Ends the current part (logged with its frames and fps) and starts the next.
inline void Change(const std::string& what, uint64_t frame) {
  State& s = g_state;
  const auto now = std::chrono::steady_clock::now();
  const double part_s = std::chrono::duration<double>(now - s.part_time).count();
  const uint64_t frames = frame - s.part_frame;
  NoteScene(what);  // Session 73 (frame_stats.h: the CSV's scene marker)
  REXLOG_INFO("[wwe2k14][scene] {} at frame {}, {:.1f} s after start (the part before: {} frames in {:.1f} s, {:.1f} fps)",
              what, frame, std::chrono::duration<double>(now - s.start).count(), frames, part_s,
              part_s > 0 ? double(frames) / part_s : 0.0);
  s.part_frame = frame, s.part_time = now;
  s.snapshot_due = frame + 90;
}

// VdSwap, every guest frame (the game thread).
inline void OnFrame(uint64_t frame) {
  State& s = g_state;
  const uint32_t seq = load_probe::CurrentSequence();
  if (seq != 0 && seq != s.seq) {
    Change(fmt::format("sequence {} -> {} ({})", s.seq, seq, SequenceName(seq)), frame);
    if (seq == 9) LogMatchCard("in-game sequence started", frame);
    s.seq = seq;
    s.scene = 0, s.clock_running = false;
  }
  if (seq == 9) {
    const uint32_t scene = Read(kSceneWord);
    if (scene != 0 && scene != s.scene) {
      if (s.scene != 0) {
        Change(fmt::format("scene word {} -> {} ({})", s.scene, scene,
                           scene == 4 ? "match load / entrances" : scene == 1 ? "in the ring" : "?"),
               frame);
      }
      s.scene = scene;
    }
    const bool running = Read(kMatchClock) != 0;
    if (running && !s.clock_running) {
      Change("match clock started", frame);
      LogMatchCard("match clock started", frame);
      s.next_ring_snapshot = frame + 3600;
    }
    s.clock_running = running;
    if (running && frame >= s.next_ring_snapshot) {
      s.next_ring_snapshot = frame + 3600;
      Snapshot();
    }
  }
  if (s.snapshot_due && frame >= s.snapshot_due) {
    s.snapshot_due = 0;
    Snapshot();
  }
}

// The scene snapshots of all but the newest `keep` earlier runs are deleted (F11 marks are the player's: kept).
inline void PruneSnapshots(size_t keep) {
  const auto dir = std::filesystem::current_path() / "captures";
  std::error_code ec;
  std::vector<std::pair<int, std::filesystem::path>> files;
  std::vector<int> runs;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    const std::string n = e.path().filename().string();
    const size_t at = n.find("_scene_");
    if (at == std::string::npos || at == 0 || e.path().extension() != ".jpg") continue;
    const int run = std::atoi(n.substr(0, at).c_str());
    files.emplace_back(run, e.path());
    if (std::find(runs.begin(), runs.end(), run) == runs.end()) runs.push_back(run);
  }
  if (runs.size() <= keep) return;
  std::sort(runs.begin(), runs.end());
  const int oldest_kept = runs[runs.size() - keep];
  for (const auto& [run, path] : files) {
    if (run < oldest_kept) std::filesystem::remove(path, ec);
  }
}

// OnPostSetup. `harness`: snapshots off unless --wwe2k14_session_snapshots was given.
inline void Install(bool harness) {
  if (!REXCVAR_GET(wwe2k14_session_log)) return;
  PruneSnapshots(5);
  if (harness && rex::cvar::GetFlagSource("wwe2k14_session_snapshots") == rex::cvar::Source::kDefault) {
    rex::cvar::SetFlagByName("wwe2k14_session_snapshots", "0");
  }
  g_session_frame_hook = OnFrame;
  REXLOG_INFO("[wwe2k14][session] timeline on: [scene] / [match] lines, {} snapshot(s) at most",
              REXCVAR_GET(wwe2k14_session_snapshots));
}

}  // namespace wwe2k14::session_log
