// wwe2k14 - S110 REPLAN (docs\research\s110\REPLAN.md): ONE boot plan for the game's fixed per-slot memory budgets a wrestler's model
// passes through, inside the console's 512 MB layout. Pure logic, no SDK includes (unit test tools\tests\mem_plan_test.cpp); the boot
// hook, the cvar and the log lines are in src\mem_plan.h. Replaces the two per-budget fixes of S110 (PAC176CRASH: data heaps 22..33 at
// 3264 KB in the boot table only; POOLWIN: vertex-buffer windows 34..42 at 2048 KB).
//
// How the game sizes them (all [C] from the listings; addresses in REPLAN.md "What I read"):
// - CPU heaps (CYMem): sub_8225F0F0 takes one block of [cfg + 28] bytes (cfg = 0x8392479C, the word 0x0BD00000 stored at
//   sub_827FE8A8 +12C) directly below the window span; sub_82C2E2F8 (boot, sub_827FE8A8 +1CC) cuts heaps 0..54 DOWNWARD from the
//   block's top in id order from a 56-word KB table scattered from the (id, KB) pairs at 0x83625BF8; heap 55 ('Debug') is the rest at
//   the bottom. Later re-cuts read other pair tables from the image each time they run: sub_827FED60 (Create a Superstar, the
//   two-wrestler layout: 0x83625F78, or 0x83625EE0 when r3 = 0) and sub_827FEE88 (the boot table again) re-cut ids 14..53 downward
//   from heap 14's top through sub_82C2EA70, and heap 54 ('common', [0x8392FFD8] = 54) takes the rest down to heap 55's top;
//   sub_827FEFD8 (0x83625D70; from game_main_sequence_ctor sub_824F6130 +44 when sub_822BA5A8 is true) and sub_827FF088 (0x83625E28)
//   re-cut ids 22..43 only, downward from heap 22's top (sub_82C2F000), with no bound against heap 54 below them.
// - GPU windows (physical): sub_8225F0F0 sets the span [0x1FC00000 - [cfg + 24], 0x1FC00000) (the word 0x10400000, sub_827FE8A8
//   +108); sub_8225F2F8 (sub_827FE8A8 +278) cuts windows 0..54 UPWARD from the span's base from the pairs at 0x83626020, window 55 the
//   rest; sub_8225F410 re-cuts from window 14 (sub_827FED60: 0x83626190; sub_827FEE88: 0x83626020) or 44, window 54 the rest.
// - Match slot s (0..8) loads its model data with current id 22 + s, builds its objects and vertex / index buffers with id 34 + s;
//   entrance slots 9..11 use 31..33 (sub_827DBF20 / sub_827DBF30, sub_82314128, sub_82311D30). One id names both a heap and a window.
// The sizes are the 360's memory plan (a 360 habit, invisible on screen); an allocation that does not fit returns 0, unchecked
// (runs 216 / 219: the BPE decode wrote through NULL; run 220: a vertex buffer locked at NULL).
//
// The plan (kBudgets): the model data heaps 22..33 get 2560 KB and the vertex-buffer windows 34..42 2048 KB in EVERY image table that
// gives the slot room (a row the game holds at 0 or leaves out stays so). Run 222 (the modder's import, 2048 KB heaps): heap 22 peaked
// at 1,363 KB (2560 = 1.88x), heap 23 679 KB. Kept at the game's sizes: the instance heaps 34..42 (1294 / 644 / 492, 1152 in Create a
// Superstar: run 222 used 121 KB of heaps 34 / 35, no need measured, and sub_829E21E0 picks a heap by the free room of 22..30 / 37..42,
// so raising them would change where the game puts things) and the texture windows 22..33 (sub_825AB5E0 sizes the attire composite to
// the window's free room, so a bigger window changes what the game builds; no texture failure seen). The heap block grows by what the
// boot heap table gains and the window span by what the boot window table gains (each rounded up to 64 KB), so heap 55 and window 55
// keep their sizes; every mode table gains no more than the boot table of its kind (checked), so heap 54 / window 54 (the rest in the
// re-cut layouts) never shrink and the 22..43 re-cuts never reach heap 54. Both grow DOWNWARD into the guest physical memory left
// unreserved below the game's block (0x01000000-0x03B00000, 44,032 KB: the SDK's GPU writeback block ends at 0x01000000): block
// +19,904 KB, span +11,904 KB, 12,224 KB of it left free. That room is HELD for the budgets not yet seen to fail (the texture windows
// 22..33, the loader's heap 13), not spent; kReserveKb is the floor any plan must leave.
// The game's exact values are checked first (both config words, every planned row of all seven tables); anything else: nothing written.
#pragma once

#include <cstdint>
#include <string>

namespace wwe2k14::mem_plan {

enum class Kind : uint8_t { kHeap = 0, kWindow = 1 };

// One row per budget. plan_kb 0: the game's sizes kept. The basis of each number is in REPLAN.md section 4.
struct Budget {
  const char* name;
  Kind kind;
  uint32_t first, count;  // ids first .. first + count - 1
  uint32_t plan_kb;
};
inline constexpr Budget kBudgets[] = {
    // game 1088 (slots 0-5) / 416 (6-8) / 1024 (entrance 9-11); the import's heap 22 peaked at 1,363 KB (run 222): 1.88x that
    {"model data heaps 22-33", Kind::kHeap, 22, 12, 2560},
    // game 1294 (0-5) / 644 (6) / 492 (7-8), 1152 in Create a Superstar: kept (run 222: 121 KB used; header comment)
    {"instance heaps 34-42", Kind::kHeap, 34, 9, 0},
    // game 6656 / 3584 / 6656: kept (header comment)
    {"texture windows 22-33", Kind::kWindow, 22, 12, 0},
    // game 864 (0-5) / 448 (6-8); the import asked 980 KB and more (run 220): 2.09x that
    {"vertex-buffer windows 34-42", Kind::kWindow, 34, 9, 2048},
};
inline constexpr uint32_t kBudgetCount = sizeof(kBudgets) / sizeof(kBudgets[0]);

// Every image pair table that sizes ids 22..42, with the game's KB of each of those ids (0: the id is absent or 0 there).
inline constexpr uint32_t kIdFirst = 22, kIdCount = 21;  // ids 22..42
struct Table {
  Kind kind;
  uint32_t addr, pairs;  // (id, KB) pairs
  bool boot;             // the boot layout: the block / the span grow by what it gains
  const char* who;
  uint32_t game_kb[kIdCount];
};
inline constexpr Table kTables[] = {
    {Kind::kHeap, 0x83625BF8u, 46, true, "boot heaps (sub_827FE8A8, sub_827FEE88)",
     {1088, 1088, 1088, 1088, 1088, 1088, 416, 416, 416, 1024, 1024, 1024, 1294, 1294, 1294, 1294, 1294, 1294, 644, 492, 492}},
    {Kind::kHeap, 0x83625D70u, 23, false, "heaps 22-43 (sub_827FEFD8)",
     {1088, 1088, 1088, 1088, 1088, 1088, 0, 0, 0, 0, 0, 0, 1294, 1294, 1294, 1294, 1294, 1294, 0, 0, 0}},
    {Kind::kHeap, 0x83625E28u, 23, false, "heaps 22-43 (sub_827FF088)",
     {1088, 1088, 1088, 1088, 1088, 1088, 416, 416, 416, 1024, 1024, 1024, 1294, 1294, 1294, 1294, 1294, 1294, 644, 492, 492}},
    {Kind::kHeap, 0x83625EE0u, 19, false, "Create a Superstar heaps (sub_827FED60, r3 0)",
     {1088, 1088, 0, 0, 0, 1088, 0, 0, 0, 0, 0, 0, 1152, 1152, 0, 0, 0, 1152, 0, 0, 0}},
    {Kind::kHeap, 0x83625F78u, 19, false, "Create a Superstar heaps (sub_827FED60)",
     {1088, 1088, 0, 0, 0, 1088, 0, 0, 0, 0, 0, 0, 1152, 1152, 0, 0, 0, 1152, 0, 0, 0}},
    {Kind::kWindow, 0x83626020u, 45, true, "boot windows (sub_827FE8A8, sub_827FEE88)",
     {6656, 6656, 6656, 6656, 6656, 6656, 3584, 3584, 3584, 6656, 6656, 6656, 864, 864, 864, 864, 864, 864, 448, 448, 448}},
    {Kind::kWindow, 0x83626190u, 17, false, "Create a Superstar windows (sub_827FED60)",
     {6656, 6656, 0, 0, 0, 6656, 0, 0, 0, 0, 0, 0, 864, 864, 0, 0, 0, 864, 0, 0, 0}},
};
inline constexpr uint32_t kTableCount = sizeof(kTables) / sizeof(kTables[0]);

inline constexpr uint32_t kSpanWord = 24;               // cfg + 24 (0x839247B4): the window span
inline constexpr uint32_t kBlockWord = 28;              // cfg + 28 (0x839247B8): the heap block
inline constexpr uint32_t kGameSpan = 0x10400000u;      // sub_827FE8A8 +108
inline constexpr uint32_t kGameBlock = 0x0BD00000u;     // sub_827FE8A8 +12C
inline constexpr uint32_t kSpanTop = 0x1FC00000u;       // the span's end, a constant in sub_8225F0F0 (+170)
inline constexpr uint32_t kSpanBase = 0x836EE410u;      // written by sub_8225F0F0 +180
inline constexpr uint32_t kSpanGranted = 0x836EE418u;   // written by sub_8225F0F0 +184 (the span's size granted)
inline constexpr uint32_t kGuestFloor = 0x01000000u;    // the SDK's GPU writeback block ends here (xmemory.cpp:533-535)
inline constexpr uint32_t kReserveKb = 8192;            // the plan must leave at least this much free above kGuestFloor
inline constexpr uint32_t kMaxWords = 96;

inline uint32_t PlanKb(Kind kind, uint32_t id) {
  for (const Budget& b : kBudgets)
    if (b.kind == kind && id >= b.first && id < b.first + b.count) return b.plan_kb;
  return 0;
}

// The block and the span are allocated in 64 KB steps.
inline uint32_t RoundUp64KbInKb(uint32_t kb) { return (kb + 63u) / 64u * 64u; }

struct Plan {
  bool ok = false;
  const char* why = "";
  uint32_t block_from = 0, block_to = 0;  // cfg + 28 asked: before / after
  uint32_t span_from = 0, span_to = 0;    // cfg + 24 asked: before / after
  uint32_t heap_added_kb = 0;             // what the boot heap table gained (before rounding)
  uint32_t window_added_kb = 0;           // what the boot window table gained (before rounding)
  uint32_t free_kb = 0;                   // guest physical left free between kGuestFloor and the planned block's bottom
  uint32_t words = 0;                     // KB words raised over all tables
  uint32_t word_addr[kMaxWords] = {};
  uint32_t game_kb[kMaxWords] = {};
  uint32_t plan_kb[kMaxWords] = {};
};

// The address of id `id`'s KB word in a pair table: 0 when the id is absent; ~0u when it appears twice or a word is unreadable.
template <class Rd>
uint32_t FindKbWord(Rd& rd, uint32_t pairs, uint32_t count, uint32_t id) {
  uint32_t at = 0;
  for (uint32_t k = 0; k < count; ++k) {
    uint32_t idx = 0;
    if (!rd(pairs + 8 * k, &idx)) return ~0u;
    if (idx != id) continue;
    if (at) return ~0u;
    at = pairs + 8 * k + 4;
  }
  return at;
}

// rd(address, &word) / wr(address, word): guest big-endian words; cfg = sub_8225F0F0's r3. Writes nothing unless every check holds
// (a second call, or words that are not the game's, are left as they are), then every planned word and both config words.
template <class Rd, class Wr>
Plan Apply(Rd&& rd, Wr&& wr, uint32_t cfg) {
  Plan p;
  if (!rd(cfg + kBlockWord, &p.block_from) || !rd(cfg + kSpanWord, &p.span_from)) return p.why = "config words unreadable", p;
  if (p.block_from != kGameBlock || p.span_from != kGameSpan) return p.why = "config words are not the game's (changed already?)", p;
  uint32_t boot_gain[2] = {0, 0}, mode_gain[2] = {0, 0};  // by Kind
  for (const Table& t : kTables) {
    uint32_t gain = 0;
    for (uint32_t i = 0; i < kIdCount; ++i) {
      const uint32_t id = kIdFirst + i;
      const uint32_t at = FindKbWord(rd, t.addr, t.pairs, id);
      if (at == ~0u) return p.why = "a pair table is not the game's (an id twice, or unreadable)", p;
      uint32_t kb = 0;
      if (at && !rd(at, &kb)) return p.why = "a pair table is unreadable", p;
      if (kb != t.game_kb[i]) return p.why = "a pair table is not the game's (changed already?)", p;
      const uint32_t plan = PlanKb(t.kind, id);
      if (kb == 0 || plan <= kb) continue;
      if (p.words == kMaxWords) return p.why = "too many words", p;
      p.word_addr[p.words] = at;
      p.game_kb[p.words] = kb;
      p.plan_kb[p.words++] = plan;
      gain += plan - kb;
    }
    const int k = t.kind == Kind::kHeap ? 0 : 1;
    if (t.boot)
      boot_gain[k] = gain;
    else if (gain > mode_gain[k])
      mode_gain[k] = gain;
  }
  if (mode_gain[0] > boot_gain[0] || mode_gain[1] > boot_gain[1]) return p.why = "a mode table would gain more than the boot layout", p;
  p.heap_added_kb = boot_gain[0];
  p.window_added_kb = boot_gain[1];
  p.block_to = p.block_from + RoundUp64KbInKb(boot_gain[0]) * 1024u;
  p.span_to = p.span_from + RoundUp64KbInKb(boot_gain[1]) * 1024u;
  const uint64_t used = uint64_t(p.span_to) + p.block_to + kGuestFloor;
  if (used + uint64_t(kReserveKb) * 1024u > kSpanTop) return p.why = "the plan leaves less than the reserve above 0x01000000", p;
  p.free_kb = uint32_t((kSpanTop - used) >> 10);
  for (uint32_t w = 0; w < p.words; ++w) wr(p.word_addr[w], p.plan_kb[w]);
  wr(cfg + kBlockWord, p.block_to);
  wr(cfg + kSpanWord, p.span_to);
  p.ok = true;
  return p;
}

// Puts the game's sizes back in every table (a grant was smaller than asked). The config words stay as asked (sub_8225F0F0 has read
// them); heap 55 / window 55, the rest, then hold what was added.
template <class Wr>
void Restore(Wr&& wr, const Plan& p) {
  for (uint32_t w = 0; w < p.words; ++w) wr(p.word_addr[w], p.game_kb[w]);
}

// "model data heaps 22-33 2560 KB, instance heaps 34-42 the game's, ..." (the boot log line).
inline std::string Describe() {
  std::string s;
  for (const Budget& b : kBudgets) {
    if (!s.empty()) s += ", ";
    s += b.name;
    s += b.plan_kb ? " " + std::to_string(b.plan_kb) + " KB" : std::string(" the game's");
  }
  return s;
}

}  // namespace wwe2k14::mem_plan
