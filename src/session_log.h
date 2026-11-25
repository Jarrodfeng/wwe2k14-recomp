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

#include "sequence_name.h"  // S105 E: SequenceName
#include "input_script.h"  // g_session_frame_hook, SaveSnapshot
#include "load_probe.h"    // CurrentSequence, SafeRead32
#include "scenario.h"      // kMatchDescPtr
#include "renderer_adapter/announce.h"  // S101 lane 271: g_announced_arena
#include "renderer_game/fallback_census.h"  // S103 L2b: SceneChange (the census flushes its window under the old scene)

REXCVAR_DECLARE(bool, wwe2k14_session_log);
REXCVAR_DECLARE(int32_t, wwe2k14_session_snapshots);
REXCVAR_DECLARE(int32_t, wwe2k14_session_snapshot_width);

namespace wwe2k14::session_log {

constexpr uint32_t kSceneWord = 0x836F3940;   // states.toml scene_word
constexpr uint32_t kMatchClock = 0x836F3928;  // states.toml match_clock

// SequenceName: src/sequence_name.h (S105 E).

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
  // S106 C3: the title bytes of the same seats (slot +41 / +42 of the participant word at e + 40), so "the champion carried his title into the match" is a line.
  std::string titles;
  for (uint32_t i = 0; i < 43; ++i) {
    const uint32_t e = m + 724 + 3580 * i;
    const uint32_t id = Read(e + 56);
    if (id >= 500) continue;
    const uint32_t w = Read(e + 40);
    titles += fmt::format("{}{}:{} t{}/{}", titles.empty() ? "" : ", ", i, id, (w >> 16) & 0xFF, (w >> 8) & 0xFF);
  }
  REXLOG_INFO("[wwe2k14][match] titles ({}): {} frame={}", when, titles.empty() ? "(none)" : titles, frame);
}

// S108 RPT (docs\systems\problem_report.md): what a problem report needs to rebuild this match on our PC (its REPRODUCE block prints a
// `--wwe2k14_launch=@quick ...` line from it). Ids and option values only, read from M like the card, once per entry into the in-game sequence:
//   [wwe2k14][match] setup: mode M type T pace P arena A; rules HEX; seats i:ID/PAD/aN[/sS], ... frame=F
//   [wwe2k14][match] setup+: seats ... frame=F      (the seats after the first 12, at most 24 per line: no line over ~600 characters)
// pace = M+528 (setter sub_822C6668); rules = the 64 rule bytes M+12..75 (what --wwe2k14_quick_rules writes, scenario.h); per seat (entry
// e = M+724+3580 i, seats 0..39, id = e+56 < 500): PAD = `cpu` when byte e+12 is 1, else `p` + byte e+4 (sub_822C6BD8 +CC..+D4 / +110); aN = the
// attire digit of the slot key u32 e+16 = id*100 + attire*10 + 2 (sub_822C2B20; `a?` when the key is not this id's); /sS = the colour set,
// signed byte e+23, when the set mode byte e+22 is 1 (sub_822C6808).
inline void LogMatchSetup(uint64_t frame) {
  const uint32_t m = Read(scenario::kMatchDescPtr);
  if (!m) return;
  std::string rules;
  for (uint32_t o = 12; o < 76; o += 4) rules += fmt::format("{:08X}", Read(m + o));
  std::vector<std::string> seats;
  for (uint32_t i = 0; i < 40; ++i) {
    const uint32_t e = m + 724 + 3580 * i;
    const uint32_t id = Read(e + 56);
    if (id >= 500) continue;
    const uint32_t key = Read(e + 16), w20 = Read(e + 20);
    const bool cpu = (Read(e + 12) >> 24) == 1;
    std::string s = fmt::format("{}:{}/{}/a{}", i, id, cpu ? std::string("cpu") : fmt::format("p{}", Read(e + 4) >> 24),
                                key / 100 == id ? std::to_string((key / 10) % 10) : std::string("?"));
    if (((w20 >> 8) & 0xFF) == 1) s += fmt::format("/s{}", int(int8_t(w20 & 0xFF)));
    seats.push_back(std::move(s));
  }
  auto list = [&](size_t from, size_t to) {
    std::string t;
    for (size_t k = from; k < to && k < seats.size(); ++k) t += (t.empty() ? "" : ", ") + seats[k];
    return t.empty() ? std::string("(none)") : t;
  };
  REXLOG_INFO("[wwe2k14][match] setup: mode {} type {} pace {} arena {}; rules {}; seats {} frame={}", int32_t(Read(m + 4)), int32_t(Read(m + 8)),
              int32_t(Read(m + 528)), int32_t(Read(m + 532)), rules, list(0, 12), frame);
  for (size_t k = 12; k < seats.size(); k += 24) REXLOG_INFO("[wwe2k14][match] setup+: seats {} frame={}", list(k, k + 24), frame);
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
  { const size_t lp = what.rfind('('), rp = what.rfind(')'); fallback_census::SceneChange(lp != std::string::npos && rp != std::string::npos && rp > lp ? what.substr(lp + 1, rp - lp - 1) : what); }   // S103 L2b: the fallback census window ends with the scene (its line names it)
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
    if (seq == 9) {
      LogMatchCard("in-game sequence started", frame);
      LogMatchSetup(frame);  // S108 RPT: the report's REPRODUCE line (one setup line per match, + setup+ lines for seats past 12)
      // S101 lane 271: the live renderer's per-arena loads start now, not at the first record that draws the arena (renderer_adapter/announce.h)
      const uint32_t m = Read(scenario::kMatchDescPtr);
      int people = 0;   // a card with fewer than two participants is not a match yet (a stale or half-built description): no early load
      for (uint32_t i = 0; m && i < 43; ++i) people += Read(m + 724 + 3580 * i + 56) < 500;
      const int arena = m && people >= 2 ? int32_t(Read(m + 532)) : -1;
      g_announced_arena.store(arena >= 0 && arena < 200 ? arena : -1, std::memory_order_relaxed);
    } else {
      g_announced_arena.store(-1, std::memory_order_relaxed);
    }
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
