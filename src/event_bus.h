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
//              3 with no finish; the kickout line also has stopped_f = the frame the count stopped, emitted late: S101 lane 102),
//              dq_count / count_out_count, eliminated (slot, id, over the top), finish (type + name:
//              1 pinfall, 2 count-out (or table: FinishName), 3 DQ, 6 submission, 7 last one left, 9 time limit, 10 Inferno; winner / loser
//              slot and character id; emitted once the triple held 2 reads, again when it changes)
//   state      on request (--wwe2k14_match_state_dump N): the match snapshot (clock, referee, finish, wrestlers)
//   entrance   an entrance step changed for a slot ([0x836F38D0 + 4 * slot] + 28 > +2: 0 intro, 5 his segment)
//   render     lane 147: the renderer announced something; what = roster_ready (pacs, models, seconds: the wrestler roster is built) or
//              roster_failed. Queued by the render thread (src/render_signal.h), emitted on the guest frame
//   load       an archive opened (a *.PAC path; GAME:/D: paths only, deduplicated per frame)
// Each event is one log line `[wwe2k14][event] {json}` and one line in logs/events_NNN.jsonl (NNN = this run's log
// number): {"f": guest frame, "t": seconds since start, "kind": ..., fields}. tools/live/events.py follows the file;
// MCP game_events reads it. The memory words are the atlas's states (tools/input_scripts/states.toml, each with its
// evidence); the per-frame tick reads ~16 words (main thread, the owner update) - negligible.
//
// S110 BUSWAIT: the vocabulary (docs/research/s110/BUSWAIT.md 3). A route waits on these with `wait_event KIND [WORD] [KEY=VALUE ...]`
// (src/input_script.h, src/event_wait_core.h); a route that does turns the bus on (On()); every Emit is also kept for it (event_wait::Global()).
//   kind      what / fields                                              emitted at                      game state
//   seq       seq, from                                                  Tick                            [[0x839400F8]+0]+48
//   screen    menu, screen_mode, from_menu, from_mode                    Tick (seq 75)                   [0x8392945C]+364, [[0x8392945C]+11412]+32
//   exit      action                                                     Tick                            0x83929434 (non-zero)
//   msgbox    state, from                                                Tick                            [0x839294BC]+16
//   match     what=clock_started: clock                                  Tick                            0x836F3928 0 -> > 0
//   match     what=finish: finish_type, finish_name, winner_slot,        Tick (held 2 reads)             0x8376ACCC, 0x8376E3EC / 0x8376E3ED
//             winner_id, loser_slot, loser_id, clock, table_rule,                                        table_rule: rule bytes G+18 / G+39 (bits 0 / 1)
//             count_out_rule                                                                             count_out_rule: rule byte G+26
//   match     what=scene: g472, clock                                    Tick                            0x8376ACCC = 15 (a 30 Years scene)
//   match     what=fall: scorer_slot, scorer_id, falls, then the         Tick (the tally rose;           rule byte G+13 / G+22 (0x8376AB01 / 0x8376AB0A)
//             finish's fields as read then; a fall scored in a rule the  S110 ELIMFIN fix 2,             != 0; the tally int16 G+14588 + 2*slot; the finish
//             game may go on past (falls); that rule's finish is said    event_bus_finish_core.h)        then waits for the finish time G+14580 = 0x8376E3E8
//             once the game commits the end (+ winner_falls, loser_falls)                                != 0
//   match     what=pin: pinning_slot                                     MatchTick (seq 9)               judge [0x83767B6C]+72 -> 1, +92
//   match     what=count: count (1..3)                                   MatchTick                       judge +80 in mode 1
//   match     what=kickout: at_count, stopped_f (20 frames late)         MatchTick                       mode 1 -> 0 below 3, no finish
//   match     what=dq_count | count_out_count                            MatchTick                       judge mode 3 | 5
//   match     what=eliminated: slot, id, over_the_top                    MatchTick (not a Rumble)        char+1020 -> 4 | 5
//   match     what=eliminated (Rumble): n, slot, id, over_the_top,       event_bus_rumble.h Tick         0x836F3924
//             by_idx, victim_idx, remaining [, same_frame]
//   match     what=entered: n, slot, id [, starter | same_frame];        event_bus_rumble.h Tick         G+81 + [0x835E1928]
//             what=entered_id: n, slot, id; what=resync: counter, from, to
//   move      slot, id, waza, move, class (signature | finisher;         move_log_hook.h, at each motion char+2490..+2494 latched
//             normal too with --wwe2k14_events_moves=all): one line a    start that latches a logged     by sub_823CA4B0 from the
//             logged move whose motion starts (S110 BUSMOVE fix 2: not   move; the rule is               log sub_825A14A8 writes
//             what the grapple-start offer loop left, not a continuation event_bus_rumble.h MoveLatch
//             of the move the character is in)
//   entrance  slot, step, from                                           Tick (seq 9)                    [[0x836F38D0+4*slot]+28]+2
//   state     clock, referee_mode, referee_count, finish, finish_name,   Tick, on request                SnapshotJson (--wwe2k14_match_state_dump)
//             winner, loser, wrestlers[] (or in_match: false)
//   render    what=roster_ready (pacs, models, seconds) | roster_failed  Tick (render_signal.h queue)    the renderer's posts
//             | background_ended (ms)
//   load      file                                                       OnFileOpened                    a *.PAC opened
//   break     what=ring, byte                                            MatchTick, once a match         0x837632FA bit 0 (sub_825957A8)
//   break     what=table: obj, act, previous, track, weapon_kind, on_fire, via   table_break.h's hooks of the    [w+1464] 0 -> 1 on a table
//             (hit | other) [, body_slot, body_id when via=hit]           latch writers, once a table     (kind 5 / 9) across one write
//             per break: slot +88 sub_825DDEC0 / slot +84 sub_825DDFA0 (the broken-model swap), whatever calls them (a motion start,
//             sub_825DF030's mid-motion break, sub_82684948's 15378 / 15379). act / previous / track = the table's at the write.
//             Not a break: sub_825E0B38's direct 1 (an out-of-bounds removal: the table shows broken while it fades out) and the
//             replay's restores. A second table that only plays a motion (15229 / 15356) writes no latch.
//   replay    state=start                                                Replay(): hook sub_8271A630     the PRTI track begins playback
//             state=skipped                                              Replay(): hook sub_826071F8     [ctrl+48] = 4 after the skip
//             state=end, skipped (true | false)                          Replay(): hook sub_8271AA48     the PRTI track ends playback
// S110 TABLEEVENT / TABLELATCH (docs/research/s110/TABLELATCH.md): the table break is the weapon's latch, emitted where it is written (replaces BUSWAIT.md 4a).

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

#include "event_bus_rumble.h"  // S101 lane 120
#include "render_signal.h"  // lane 147
#include "event_wait_core.h"  // S110 BUSWAIT: every event kept for the route's wait_event (no SDK)
#include "table_break_core.h"  // S110 TABLEEVENT: finish type 2's name by the rule bits (no SDK)
#include "event_bus_finish_core.h"  // S110 ELIMFIN: which decision line a read makes: finish | fall | scene (no SDK)

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>

REXCVAR_DECLARE(bool, wwe2k14_events);

REXCVAR_DECLARE(int64_t, wwe2k14_match_state_dump);

// S101 lane 113 / lane 120: "" = the move event emits signature + finisher moves only; "all" adds the normal moves.
REXCVAR_DECLARE(std::string, wwe2k14_events_moves);

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

// S110 BUSWAIT: also on when the route parser saw a bus wait (wait_event / log_event), so a route never needs the flag.
inline bool On() { return REXCVAR_GET(wwe2k14_events) || ::wwe2k14::event_wait::g_wanted.load(std::memory_order_relaxed); }

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
  ::wwe2k14::event_wait::Global().Add(frame, kind, fields);  // S110 BUSWAIT: the route's record (its own mutex; any emitting thread)
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
// S101 lane 113: a big-endian halfword
inline uint16_t R16(uint32_t a) {
  return rex::memory::load_and_swap<uint16_t>(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(a));
}
// S101 lane 120: the guest memory as src/event_bus_rumble.h reads it
struct GuestMem {
  uint8_t r8(uint32_t a) const { return R8(a); }
  uint16_t r16(uint32_t a) const { return R16(a); }
  uint32_t r32(uint32_t a) const { return R32(a); }
};
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
  finish_core::State fin;  // S110 ELIMFIN: the decision record's lines (src/event_bus_finish_core.h): held 2 reads; fall / finish
  std::array<int64_t, 6> part{-2, -2, -2, -2, -2, -2};  // char+1020 per slot: 0 active, 1 apron, 4 out, 5 over the top
};
inline Last g_last;
inline std::atomic<uint64_t> g_frame_now{0};  // S110 BUSWAIT: Tick's frame, the stamp of the events emitted off the owner update (Replay)
inline int g_ring_break = 0;                  // S110 BUSWAIT: 0 not armed, 1 armed (bit 0 seen clear in this match), 2 said
inline rumble::State g_rumble;          // S101 lane 120
inline rumble::MoveState g_moves;       // S101 lane 113; S110 BUSMOVE: written by src/move_log_hook.h (each move-log call) and Tick (the match leave)
inline std::mutex g_moves_mutex;        // S110 BUSMOVE review: guards g_moves (the move hook and Tick's match leave)

// Match state words (match_events.md): the referee ("judge") [0x83767B6C]: +72 mode (0 watch, 1 pin count, 3 5-count
// to a DQ, 4 a count over all slots, 5 10-count count-out), +80 count, +92 the pinning slot (inferred); the finish
// type G+472 = 0x8376ACCC (mapper sub_825B0EA0's table 0x8201EB58), winner / loser 0x8376E3EC / 0x8376E3ED (signed
// bytes), finish time 0x8376E3E8; the slot table 0x836F38D0 (9 character pointers; char+1220 = its slot, +1480 team,
// +3720 role (1 referee), +1020 participation, +1021 the decision kind, +3756 the participant index, [+3068] the health
// block: int16 current[4] at +52, max[4] at +60).
constexpr uint32_t kJudge = 0x83767B6C, kFinish = 0x8376ACCC, kWinner = 0x8376E3EC, kLoser = 0x8376E3ED;
constexpr uint32_t kSlots = 0x836F38D0, kClock = 0x836F3928, kMatchDesc = 0x836F2D88;

constexpr uint32_t kFinishTime = 0x8376E3E8;  // G+14580: the clock, written when the game commits the match end (S110 ELIMFIN, finish_core)
// S110 TABLEEVENT: finish type 2 is decision kind 1 (the table 0x8201EB58 read by sub_825B0EA0). The referee's 10-count (sub_825B5780,
// mode 5) records kind 1, and so does the table-hit decision sub_827B5B98 (+188 / +1AC, against the hit command's [cmd+36]) when rule
// byte G+39 or G+18 is set (G = 0x8376AAF4, the match's rule bytes G+0..63). The 10-count runs only under the count-out rule byte G+26
// (sub_825B3CF0 +414): type 2 is named "table" under a table rule with G+26 clear, else "count-out" (table_break_core.h Kind1Name).
constexpr uint32_t kRules = 0x8376AAF4;
inline int TableRules() {
  return table_break::TableRuleBits(R8(kRules + 18), R8(kRules + 39)) | (R8(kRules + 26) ? table_break::kCountOutRule : 0);
}
inline const char* FinishName(int64_t t, int table_rules = 0) {
  switch (t) {
    case 1: return "pinfall";
    case 2: return table_break::Kind1Name(table_rules);
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
                     Readable(judge) ? int32_t(R32(judge + 80)) : -1, fin, fin ? FinishName(fin, TableRules()) : "",
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
      // S101 lane 102: stopped_f = the frame the count stopped (this line is emitted kKickoutHold frames later); the moment grammar
      // (tools/goto/when.py @kickout~late) measures how late in the count interval the near-fall ended from it.
      Emit(frame, "match", fmt::format("\"what\": \"kickout\", \"at_count\": {}, \"stopped_f\": {}", g_kickout.count, g_kickout.frame));
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
  // S101 lane 120: a Rumble (G byte 0 in 20..23) is driven by the counter 0x836F3924 (src/event_bus_rumble.h); the per-slot path below
  // serves the other types, and no longer emits for id 255 (the non-wrestler seats start at state 4: the 3 false lines about 460
  // frames before clock_started in every match). No clock gate: elimination tag / chamber eliminations must still emit (review item 8).
  GuestMem mem;
  const bool is_rumble = rumble::Tick(g_rumble, mem, frame, g_last.clock_running == 1, [&](const std::string& f) { Emit(frame, "match", f); });
  for (uint32_t s = 0; s < 6; ++s) {
    const uint32_t c = Char(s);
    const int64_t st = c ? int64_t(R8(c + 1020)) : -1;
    const int64_t prev = g_last.part[s];
    g_last.part[s] = st;
    if (prev >= 0 && st != prev && (st == 4 || st == 5) && !is_rumble) {  // S101 lane 120
      const int64_t id = c ? CharacterId(c) : -1;
      if (id == 255) continue;
      Emit(frame, "match", fmt::format("\"what\": \"eliminated\", \"slot\": {}, \"id\": {}, \"over_the_top\": {}", s, id,
                                       st == 5 ? "true" : "false"));
    }
  }
  // S101 lane 113 / S110 BUSMOVE: the move line is emitted at each call of the move log sub_825A14A8 (src/move_log_hook.h), not polled here
  // S110 BUSWAIT: the OMG ring break. sub_825957A8 writes byte 0x837632FA = 3 at 0x825957E8 when the RIDS rig starts the break; the rest
  // value is 0 or 2 (sub_82597120 0x8259715C). A replay saves the byte and forces 2 (sub_82711CA8 +20..+38) and puts it back at its end
  // (sub_82711D38 +1C), so bit 0 rises again after a replay of the break: one line a match, armed once bit 0 was seen clear in it.
  constexpr uint32_t kRingBreak = 0x837632FA;
  const uint8_t ring = R8(kRingBreak);
  if (g_ring_break == 0 && !(ring & 1)) {
    g_ring_break = 1;
  } else if (g_ring_break == 1 && (ring & 1)) {
    g_ring_break = 2;
    Emit(frame, "break", fmt::format("\"what\": \"ring\", \"byte\": {}", int(ring)));
  }
}


// The owner update, once per guest frame (src/scenario.h's hook).
inline void Tick(uint64_t frame) {
  if (!On() || frame == g_last.frame) return;
  g_last.frame = frame;
  g_frame_now.store(frame, std::memory_order_relaxed);
  render_signal::Drain([&](const std::string& what, const std::string& fields) {   // lane 147: the render side's ready signals, queued by its thread, emitted here on the guest frame
    Emit(frame, "render", fmt::format("\"what\": \"{}\"{}{}", what, fields.empty() ? "" : ", ", fields));
  });
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
  // The decision record: the type, winner and loser are written in different orders by different paths, multi-fall rules reset
  // them, and the time limit fills the winner a frame later (match_events.md section 5): a line once the triple has held for 2
  // reads, again whenever it changes to another non-zero triple. S110 ELIMFIN (docs/research/s110/ELIMFIN.md): in a rule the game
  // may go on past a decision (rule byte G+13 or G+22, sub_827C3660 +A8..+18C) that line is `fall`, and `finish` is said once the
  // game commits the end (G+14580 written: sub_827C30B0 +BC, sub_82679710). src/event_bus_finish_core.h decides which.
  const int64_t finish = int64_t(int32_t(R32(kFinish)));
  const int64_t win = int8_t(R8(kWinner)), lose = int8_t(R8(kLoser));
  finish_core::Read fr;
  fr.type = finish;
  fr.win = win;
  fr.lose = lose;
  fr.multi = finish_core::MultiDecision(R8(kRules + 13), R8(kRules + 22));
  fr.ended = R32(kFinishTime) != 0;
  for (int s = 0; s < finish_core::kTallySlots; ++s) fr.falls[s] = int16_t(R16(kRules + finish_core::kTallyOffset + 2 * uint32_t(s)));
  const finish_core::Scored scored = finish_core::Falls(g_last.fin, fr);  // S110 ELIMFIN fix 2: a fall is a tally rise (run 293)
  const finish_core::Line fin_line = finish_core::Step(g_last.fin, fr);
  auto who = [](int64_t slot) {
    const uint32_t c = slot >= 0 && slot < 9 ? Char(uint32_t(slot)) : 0;
    return c ? CharacterId(c) : -1;
  };
  auto record = [&]() {  // the decision record's fields as this read sees them (the finish line's text since Session 91)
    return fmt::format("\"finish_type\": {}, \"finish_name\": \"{}\", \"winner_slot\": {}, \"winner_id\": {}, \"loser_slot\": {}, "
                       "\"loser_id\": {}, \"clock\": {}, \"table_rule\": {}, \"count_out_rule\": {}",
                       finish, FinishName(finish, TableRules()), win, who(win), lose, who(lose), clock, TableRules() & 3,
                       int(R8(kRules + 26)));
  };
  for (int i = 0; i < scored.n; ++i) {  // before the finish: a fall that ends the match rises in the read the end may also show
    const int s = scored.slot[size_t(i)];
    Emit(frame, "match", fmt::format("\"what\": \"fall\", \"scorer_slot\": {}, \"scorer_id\": {}, \"falls\": {}, {}", s, who(s),
                                     fr.falls[size_t(s)], record()));
  }
  if (fin_line == finish_core::Line::kScene) {  // lane 150: G+472 = 15 is the value a playing 30 Years scene leaves there (run 7092), not a match end: never a `finish`
    Emit(frame, "match", fmt::format("\"what\": \"scene\", \"g472\": {}, \"clock\": {}", finish, clock));
  } else if (fin_line != finish_core::Line::kNone) {
    auto tally = [&](int64_t slot) { return slot >= 0 && slot < finish_core::kTallySlots ? fr.falls[size_t(slot)] : int64_t(-1); };
    // S110 ELIMFIN fix 2: a multi-decision rule's finish carries the falls (an Iron Man time-limit win is type 1 by the tally,
    // sub_827C2BE8 +2C8); a one-fall line keeps its text
    const std::string falls = fr.multi ? fmt::format(", \"winner_falls\": {}, \"loser_falls\": {}", tally(win), tally(lose)) : "";
    Emit(frame, "match", fmt::format("\"what\": \"{}\", {}{}", finish_core::What(fin_line), record(), falls));
  }
  if (seq == 9) {
    MatchTick(frame);
  } else {  // S101 lane 120: leaving the match ends the Rumble / move tracking
    g_rumble.Reset();
    {  // S110 BUSMOVE review: the move hook (src/move_log_hook.h) runs only on move-log calls, which a menu never makes: the reset is here
      std::lock_guard<std::mutex> lock(g_moves_mutex);
      g_moves = {};
    }
    g_ring_break = 0;  // S110 BUSWAIT: the next match arms again
  }
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

// S110 BUSWAIT: the replay's moments, from the existing hooks in src/replay_fx_skip.h (a guest thread; Emit is thread-safe). The frame is
// the owner update's last one (g_frame_now). extra = more fields (a JSON fragment) or "".
inline void Replay(const char* state, const char* extra = "") {
  if (!On()) return;
  Emit(g_frame_now.load(std::memory_order_relaxed), "replay",
       fmt::format("\"state\": \"{}\"{}{}", state, extra[0] ? ", " : "", extra));
}

}  // namespace wwe2k14::events
