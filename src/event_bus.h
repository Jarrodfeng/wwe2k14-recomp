#pragma once
// wwe2k14 - Session 91 step 7 (Jarrod: "get started on the event bus within this session"): the game announces its
// transitions, so tools wait on events instead of polling memory.
//
// --wwe2k14_events=true (default off; tools/live/live.py start turns it on) emits, as they happen:
//   seq        the mode changed (sequence id [[0x839400F8]+0]+48: 3 title, 75 main menu, 9 in game, 76 CAW, ...)
//   screen     the main menu's list / screen changed (SmartMenu [0x8392945C] +364 menu id, +11412 > +32 screen mode)
//   exit       a main-menu exit action was requested (0x83929434: 1 launch, 8-11 CAW, 19 logo, 25 shop, ...)
//   msgbox     the game's message box state changed ([0x839294BC]+16: 2 = waiting for an answer, 5 idle)
//   match      the match clock started (0x836F3928 0 -> running); Session 91 step 8 (out/harness/atlas/readers/
//              match_events.md): pin (the referee's pin count started), count 1..3, kickout (the count stopped below
//              3 with no finish), dq_count / count_out_count, eliminated (slot, id, over the top), finish (type + name:
//              1 pinfall, 2 count-out, 3 DQ, 6 submission, 7 last one left, 9 time limit, 10 Inferno; winner / loser
//              slot and character id; emitted once the triple held 2 reads, again when it changes)
//   state      on request (--wwe2k14_match_state_dump N): the match snapshot (clock, referee, finish, wrestlers)
//   entrance   an entrance step changed for a slot ([0x836F38D0 + 4 * slot] + 28 > +2: 0 intro, 5 his segment)
//   load       an archive opened (a *.PAC path; GAME:/D: paths only, deduplicated per frame)
// Each event is one log line `[wwe2k14][event] {json}` and one line in logs/events_NNN.jsonl (NNN = this run's log
// number): {"f": guest frame, "t": seconds since start, "kind": ..., fields}. tools/live/events.py follows the file;
// MCP game_events reads it. The memory words are the atlas's states (tools/input_scripts/states.toml, each with its
// evidence); the per-frame tick reads ~16 words (main thread, the owner update) - negligible.

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_BOOL(wwe2k14_events, false, "wwe2k14",
                    "Session 91: the event bus - log the game's transitions (mode, screen, exit action, message box, "
                    "match start / end, entrance steps, archive loads) as [wwe2k14][event] lines and "
                    "logs/events_NNN.jsonl (src/event_bus.h)");

REXCVAR_DEFINE_INT64(wwe2k14_match_state_dump, 0, "wwe2k14",
                     "Session 91: set to a new value for one 'state' event: the match snapshot (clock, referee mode / "
                     "count, finish, every wrestler's id, team, role, state, health); needs --wwe2k14_events");

namespace wwe2k14::events {

inline int64_t g_state_dump_seen = 0;

inline std::mutex g_mutex;
inline std::FILE* g_file = nullptr;
inline bool g_file_tried = false;
inline const auto g_t0 = std::chrono::steady_clock::now();

inline int RunNumber() {  // this run's log number (wwe2k14_NNN.log), as the snapshots name their files
  std::error_code ec;
  int best = -1;
  for (const auto& e : std::filesystem::directory_iterator(std::filesystem::current_path() / "logs", ec)) {
    const std::string n = e.path().filename().string();
    if (n.rfind("wwe2k14_", 0) == 0 && n.size() > 12 && n.substr(n.size() - 4) == ".log") {
      const int v = std::atoi(n.c_str() + 8);
      if (v > best) best = v;
    }
  }
  return best;
}

inline bool On() { return REXCVAR_GET(wwe2k14_events); }

inline std::string Esc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if (uint8_t(c) >= 0x20) o += c;
  }
  return o;
}

// fields: a JSON fragment (`"a": 1, "b": "x"`) or empty
inline void Emit(uint64_t frame, const char* kind, const std::string& fields) {
  const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
  const std::string line = fmt::format("{{\"f\": {}, \"t\": {:.3f}, \"kind\": \"{}\"{}{}}}", frame, t, kind,
                                       fields.empty() ? "" : ", ", fields);
  REXLOG_INFO("[wwe2k14][event] {}", line);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_file_tried) {
    g_file_tried = true;
    const auto path = std::filesystem::current_path() / "logs" / fmt::format("events_{:03}.jsonl", std::max(0, RunNumber()));
    g_file = std::fopen(path.string().c_str(), "ab");
    REXLOG_INFO("[wwe2k14][event] bus on: {}", path.string());
  }
  if (g_file) {
    std::fputs(line.c_str(), g_file);
    std::fputc('\n', g_file);
    std::fflush(g_file);
  }
}

// Guest reads that never fault on a null pointer in a chain (reads below 0x40000000 are treated as unreadable).
inline bool Readable(uint32_t a) { return a >= 0x40000000u && a < 0xF0000000u; }
inline uint32_t R32(uint32_t a) {
  return rex::memory::load_and_swap<uint32_t>(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(a));
}
inline uint8_t R8(uint32_t a) { return *REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(a); }
// [[base]+o1]+o2...: -1 when a pointer on the way is null / out of range
inline int64_t Chain(uint32_t base, std::initializer_list<uint32_t> offs, bool byte = false) {
  uint32_t a = base;
  size_t i = 0;
  for (uint32_t o : offs) {
    if (!Readable(a)) return -1;
    const uint32_t p = R32(a);
    if (!Readable(p)) return -1;
    a = p + o;
    ++i;
  }
  if (!Readable(a)) return -1;
  return byte ? int64_t(int8_t(R8(a))) : int64_t(int32_t(R32(a)));
}

struct Last {
  int64_t seq = -2, menu = -2, mode = -2, exit = -2, box = -2, clock_running = -2, finish = -2;
  std::array<int64_t, 6> entrance{-2, -2, -2, -2, -2, -2};
  uint64_t frame = 0;
  // Session 91 step 8 (match state; out/harness/atlas/readers/match_events.md, listings cited there)
  int64_t ref_mode = -2, ref_count = -2, ref_last_count = 0;
  int64_t fin_seen = 0, fin_type = 0, fin_win = -1, fin_lose = -1, fin_emitted = 0;  // the finish held 2 reads
  int64_t fin_emitted_key = -1;
  std::array<int64_t, 6> part{-2, -2, -2, -2, -2, -2};  // char+1020 per slot: 0 active, 1 apron, 4 out, 5 over the top
};
inline Last g_last;

// Match state words (match_events.md): the referee ("judge") [0x83767B6C]: +72 mode (0 watch, 1 pin count, 3 5-count
// to a DQ, 4 a count over all slots, 5 10-count count-out), +80 count, +92 the pinning slot (inferred); the finish
// type G+472 = 0x8376ACCC (mapper sub_825B0EA0's table 0x8201EB58), winner / loser 0x8376E3EC / 0x8376E3ED (signed
// bytes), finish time 0x8376E3E8; the slot table 0x836F38D0 (9 character pointers; char+1220 = its slot, +1480 team,
// +3720 role (1 referee), +1020 participation, +1021 the decision kind, +3756 the participant index, [+3068] the health
// block: int16 current[4] at +52, max[4] at +60).
constexpr uint32_t kJudge = 0x83767B6C, kFinish = 0x8376ACCC, kWinner = 0x8376E3EC, kLoser = 0x8376E3ED;
constexpr uint32_t kSlots = 0x836F38D0, kClock = 0x836F3928, kMatchDesc = 0x836F2D88;

inline const char* FinishName(int64_t t) {
  switch (t) {
    case 1: return "pinfall";
    case 2: return "count-out";
    case 3: return "disqualification";
    case 6: return "submission";
    case 7: return "last one left";
    case 9: return "time limit";
    case 10: return "inferno";
    default: return "unknown";
  }
}

inline uint32_t Char(uint32_t slot) {  // the character in slot (0..8), 0 when absent / not this slot
  const uint32_t c = R32(kSlots + 4 * slot);
  if (!Readable(c)) return 0;
  return R32(c + 1220) == slot ? c : 0;
}

inline int64_t CharacterId(uint32_t c) {  // via the participant index into the match description (inferred)
  const uint32_t m = R32(kMatchDesc), e = R32(c + 3756);
  if (!Readable(m) || e > 42) return -1;
  return int64_t(R32(m + 724 + 3580 * e + 56));
}

inline void Health(uint32_t c, int64_t& cur, int64_t& max) {
  cur = max = -1;
  const uint32_t h = R32(c + 3068);
  if (!Readable(h)) return;
  cur = max = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    cur += int16_t(rex::memory::load_and_swap<uint16_t>(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(h + 52 + 2 * i)));
    max += int16_t(rex::memory::load_and_swap<uint16_t>(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(h + 60 + 2 * i)));
  }
}

inline std::string WrestlerJson(uint32_t slot) {
  const uint32_t c = Char(slot);
  if (!c) return "";
  int64_t cur = 0, max = 0;
  Health(c, cur, max);
  return fmt::format("{{\"slot\": {}, \"id\": {}, \"team\": {}, \"role\": {}, \"state\": {}, \"health\": {}, "
                     "\"health_max\": {}}}",
                     slot, CharacterId(c), int32_t(R32(c + 1480)), int32_t(R32(c + 3720)), int(R8(c + 1020)), cur, max);
}

// The on-demand snapshot (--wwe2k14_match_state_dump N, a new N each time): one event "state".
inline std::string SnapshotJson() {
  const uint32_t judge = R32(kJudge);
  std::string ws;
  for (uint32_t s = 0; s < 9; ++s) {
    const std::string w = WrestlerJson(s);
    if (!w.empty()) ws += (ws.empty() ? "" : ", ") + w;
  }
  const int64_t fin = int64_t(int32_t(R32(kFinish)));
  return fmt::format("\"clock\": {}, \"referee_mode\": {}, \"referee_count\": {}, \"finish\": {}, \"finish_name\": "
                     "\"{}\", \"winner\": {}, \"loser\": {}, \"wrestlers\": [{}]",
                     int32_t(R32(kClock)), Readable(judge) ? int32_t(R32(judge + 72)) : -1,
                     Readable(judge) ? int32_t(R32(judge + 80)) : -1, fin, fin ? FinishName(fin) : "",
                     int(int8_t(R8(kWinner))), int(int8_t(R8(kLoser))), ws);
}

// A kickout is the pin count stopping below 3 with no finish - but on a real three-count the referee leaves the count
// at 2 in the same frame and the finish type lands ~2 frames later (run 5910: "kickout at 2", then the pinfall 2
// frames on): a kickout is emitted only when no finish followed within kKickoutHold frames.
constexpr uint64_t kKickoutHold = 20;
struct PendingKickout {
  uint64_t frame = 0;
  int64_t count = -1;
};
inline PendingKickout g_kickout;

inline void MatchTick(uint64_t frame) {
  const uint32_t judge = R32(kJudge);
  if (!Readable(judge)) return;
  if (g_kickout.count >= 0) {
    if (R32(kFinish) != 0) {
      g_kickout = {};  // it was the three-count
    } else if (frame >= g_kickout.frame + kKickoutHold) {
      Emit(frame, "match", fmt::format("\"what\": \"kickout\", \"at_count\": {}", g_kickout.count));
      g_kickout = {};
    }
  }
  // the referee: pin started / counts / kickout (mode 1 -> 0 below 3 with no finish), other counts
  const int64_t mode = int32_t(R32(judge + 72)), count = int32_t(R32(judge + 80));
  const int64_t prev_mode = g_last.ref_mode;
  if (mode != prev_mode) {
    g_last.ref_mode = mode;
    if (prev_mode != -2) {
      if (mode == 1)
        Emit(frame, "match", fmt::format("\"what\": \"pin\", \"pinning_slot\": {}", int32_t(R32(judge + 92))));
      else if (prev_mode == 1 && g_last.ref_last_count < 3 && R32(kFinish) == 0)
        g_kickout = {frame, g_last.ref_last_count};  // held: the 3 and the finish can land a few frames later (run 5910)
      else if (mode == 3 || mode == 5)
        Emit(frame, "match", fmt::format("\"what\": \"{}\"", mode == 3 ? "dq_count" : "count_out_count"));
    }
    g_last.ref_last_count = 0;
  }
  if (mode == 1 && count != g_last.ref_count && count > 0 && count <= 3) {
    Emit(frame, "match", fmt::format("\"what\": \"count\", \"count\": {}", count));
    g_last.ref_last_count = count;
  }
  g_last.ref_count = count;
  // eliminations: char+1020 becomes 4 (eliminated) or 5 (over the top)
  for (uint32_t s = 0; s < 6; ++s) {
    const uint32_t c = Char(s);
    const int64_t st = c ? int64_t(R8(c + 1020)) : -1;
    const int64_t prev = g_last.part[s];
    g_last.part[s] = st;
    if (prev >= 0 && st != prev && (st == 4 || st == 5))
      Emit(frame, "match", fmt::format("\"what\": \"eliminated\", \"slot\": {}, \"id\": {}, \"over_the_top\": {}", s,
                                       c ? CharacterId(c) : -1, st == 5 ? "true" : "false"));
  }
}


// The owner update, once per guest frame (src/scenario.h's hook).
inline void Tick(uint64_t frame) {
  if (!On() || frame == g_last.frame) return;
  g_last.frame = frame;
  auto changed = [&](int64_t& last, int64_t now) {
    const bool c = last != -2 && now != last;
    const bool first = last == -2;
    last = now;
    return c || (first && now >= 0);
  };
  const int64_t seq = Chain(0x839400F8, {0, 48});
  if (int64_t prev = g_last.seq; changed(g_last.seq, seq))
    Emit(frame, "seq", fmt::format("\"seq\": {}, \"from\": {}", seq, prev < -1 ? -1 : prev));
  const bool menus = seq == 75 && Readable(R32(0x8392945C));
  const int64_t menu = menus ? Chain(0x8392945C, {364}) : -1;
  const int64_t mode = menus ? Chain(0x8392945C, {11412, 32}) : -1;
  const int64_t prev_menu = g_last.menu, prev_mode = g_last.mode;
  const bool menu_c = changed(g_last.menu, menu), mode_c = changed(g_last.mode, mode);
  if (menus && (menu_c || mode_c))
    Emit(frame, "screen", fmt::format("\"menu\": {}, \"screen_mode\": {}, \"from_menu\": {}, \"from_mode\": {}", menu,
                                      mode, prev_menu, prev_mode));
  const int64_t exit = int64_t(int32_t(R32(0x83929434)));
  if (changed(g_last.exit, exit) && exit != 0) Emit(frame, "exit", fmt::format("\"action\": {}", exit));
  const int64_t box = Chain(0x839294BC, {16});
  if (int64_t prev = g_last.box; changed(g_last.box, box) && prev != -2)
    Emit(frame, "msgbox", fmt::format("\"state\": {}, \"from\": {}", box, prev));
  const int64_t clock = int64_t(int32_t(R32(0x836F3928)));
  if (changed(g_last.clock_running, clock > 0 ? 1 : 0) && clock > 0)
    Emit(frame, "match", fmt::format("\"what\": \"clock_started\", \"clock\": {}", clock));
  // The finish: the type, winner and loser are written in different orders by different paths, multi-fall rules reset
  // them, and the time limit fills the winner a frame later (match_events.md section 5): emit once the triple has held
  // for 2 reads, again whenever it changes to another non-zero triple.
  const int64_t finish = int64_t(int32_t(R32(kFinish)));
  const int64_t win = int8_t(R8(kWinner)), lose = int8_t(R8(kLoser));
  const int64_t key = finish ? (finish << 16) | ((win & 0xFF) << 8) | (lose & 0xFF) : 0;
  if (key != g_last.fin_seen) {
    g_last.fin_seen = key;
  } else if (key != 0 && key != g_last.fin_emitted_key) {
    g_last.fin_emitted_key = key;
    auto who = [](int64_t slot) {
      const uint32_t c = slot >= 0 && slot < 9 ? Char(uint32_t(slot)) : 0;
      return c ? CharacterId(c) : -1;
    };
    Emit(frame, "match", fmt::format("\"what\": \"finish\", \"finish_type\": {}, \"finish_name\": \"{}\", \"winner_slot\": "
                                     "{}, \"winner_id\": {}, \"loser_slot\": {}, \"loser_id\": {}, \"clock\": {}",
                                     finish, FinishName(finish), win, who(win), lose, who(lose), clock));
  } else if (key == 0) {
    g_last.fin_emitted_key = -1;  // reset (a new fall / match): the next finish is emitted again
  }
  if (seq == 9) MatchTick(frame);
  if (const int64_t ask = REXCVAR_GET(wwe2k14_match_state_dump); ask != g_state_dump_seen) {
    g_state_dump_seen = ask;
    Emit(frame, "state", seq == 9 ? SnapshotJson() : "\"in_match\": false");
  }
  if (seq == 9) {
    for (uint32_t slot = 0; slot < 6; ++slot) {
      const int64_t step = Chain(0x836F38D0 + 4 * slot, {28, 2}, true);
      if (int64_t prev = g_last.entrance[slot]; changed(g_last.entrance[slot], step) && step >= 0 && prev != -2)
        Emit(frame, "entrance", fmt::format("\"slot\": {}, \"step\": {}, \"from\": {}", slot, step, prev));
    }
  }
}

inline std::atomic<uint64_t> g_load_frame{~0ull};
inline std::string g_load_last;

// kernel_overrides.h's file-open path: archives only.
inline void OnFileOpened(const std::string& path, uint64_t frame) {
  if (!On() || path.size() < 5) return;
  std::string low = path;
  for (auto& c : low) c = char(std::tolower(uint8_t(c)));
  if (low.compare(low.size() - 4, 4, ".pac") != 0) return;
  if (frame == g_load_frame.load() && low == g_load_last) return;  // the game opens some archives twice in a row
  g_load_frame = frame;
  g_load_last = low;
  Emit(frame, "load", fmt::format("\"file\": \"{}\"", Esc(path)));
}

}  // namespace wwe2k14::events
