// Which arena the scene is in: the stage object's own id, G+64 only while no stage is known (S103 lane CAE, docs\research\s103\LDP.md section 1 + its adversary). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Pure: no game, SDK or renderer includes; arena_hooks.h reads the two words and calls ResolveArenaId, the unit (tools\scene\stage_fx_unit.cpp, run by stage_fx_check.py) forges them.
//
// Why: the game builds its stage with the arena id stored at stage + 1216 (constructor sub_824E55B0, 0x824E560C; its only writer). The CAW editor builds its own stage BG66 while G+64 (0x8376AB34, the match settings'
// arena) stays at the stale 0, so the stream-free build published arena 0 (bg00, 40 moving slots, screen movies 800..809) for a stage that is bg66. In a match both words are the same arena.
// Ids 200..249 (Create-an-Arena venues, read by the game through [stage + 21192], sub_822A7ED8) have no bgNN.pac: S103 CAR loads them from the part pacs of the selection the latch publishes
// (stage_fx_contrib.h Take -> assets\car_sel.h; arena_hooks.h waits for it, arena_provider.h SlotsFromCustom); until the selection is read the take stays not-clean (fail closed);
// the census prints the id so a run shows it. Lodged: docs\systems\arena_look.md "Which arena is live".
// THE constants for these two numbers: no other file in src\scene_live carries a literal 1216 or a `< 400` (tools\scene\cae_literals_lint.py).
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace rdr_scene::live::arena {

inline constexpr uint32_t kMaxArenaId = 400;            // an id below this is an arena
inline constexpr uint32_t kStageArenaIdOffset = 1216;   // stage + 1216 = the arena id the stage was built for [C: q decomp sub_824E55B0 +5C, 0x824E560C]

struct ArenaIdCensus {
  std::atomic<uint64_t> resolved{0}, from_stage{0}, from_word{0}, unknown{0}, disagree{0};   // disagree: the stage's id and G+64 both read and differ
  // The window between G+64 changing and the next stage set-up (CStage::MotionSetup): a clean stage_fx take inside it would come from the OLD stage (the match-load hole of review F1).
  std::atomic<uint32_t> last_word{0xFFFFFFFFu};
  std::atomic<bool> win_open{false};
  std::atomic<uint64_t> win_base{0}, word_changes{0}, windows_closed{0}, clean_in_windows{0};
  // Called every light-set frame with G+64 and the stage_fx clean-take counter. True = G+64 changed (a window opened).
  bool NoteWord(bool word_ok, uint32_t word, uint64_t clean_takes) {
    if (!word_ok) return false;
    const uint32_t prev = last_word.exchange(word, std::memory_order_relaxed);
    if (prev == 0xFFFFFFFFu || prev == word) return false;
    word_changes.fetch_add(1, std::memory_order_relaxed);
    win_base.store(clean_takes, std::memory_order_relaxed), win_open.store(true, std::memory_order_relaxed);
    return true;
  }
  // Called at CStage::MotionSetup exit. Returns true when a window was open; `clean_in_window` = clean takes since G+64 changed.
  bool NoteSetStage(uint64_t clean_takes, uint64_t& clean_in_window) {
    if (!win_open.exchange(false, std::memory_order_relaxed)) return false;
    const uint64_t base = win_base.load(std::memory_order_relaxed);
    clean_in_window = clean_takes >= base ? clean_takes - base : 0;
    windows_closed.fetch_add(1, std::memory_order_relaxed), clean_in_windows.fetch_add(clean_in_window, std::memory_order_relaxed);
    return true;
  }
  // The one line: `stage id N, published word M, disagreements K` (+ the counts of each source). `stage_ok` / `word_ok` = the word was readable.
  std::string Text(bool stage_ok, uint32_t stage_id, bool word_ok, uint32_t word) const {
    char b[384];
    char s[16], w[16];
    if (stage_ok) std::snprintf(s, sizeof s, "%u", stage_id); else std::snprintf(s, sizeof s, "none");
    if (word_ok) std::snprintf(w, sizeof w, "%u", word); else std::snprintf(w, sizeof w, "unread");
    std::snprintf(b, sizeof b, "stage id %s, published word %s, disagreements %llu (from stage %llu, from word %llu, unknown %llu; G+64 changes %llu, windows closed %llu, clean takes in them %llu)", s, w,
                  (unsigned long long)disagree.load(), (unsigned long long)from_stage.load(), (unsigned long long)from_word.load(), (unsigned long long)unknown.load(), (unsigned long long)word_changes.load(),
                  (unsigned long long)windows_closed.load(), (unsigned long long)clean_in_windows.load());
    return b;
  }
  void Reset() { resolved = from_stage = from_word = unknown = disagree = 0, last_word = 0xFFFFFFFFu, win_open = false, win_base = word_changes = windows_closed = clean_in_windows = 0; }
};
inline ArenaIdCensus& ArenaIdCensusOf() {
  static ArenaIdCensus c;
  return c;
}

// Test only (tools\scene\stage_fx_unit.cpp): 1 = G+64 only (the old rule; the stage's id is never asked).
inline int g_arena_id_plant = 0;   // 2: SlotsReady keyed on the published arena only (the pre-F2 rule); 3: the game's stage holder is never asked (the pre-CACBG rule)

// The slot table counts as ready for the stage's take only when it is the table of THIS stage's arena: the stage's own arena equals the published one, and that table is loaded (review F2: with the editor's stage
// 66 and a stale published 0, "the published arena's table is loaded" said ready for the wrong arena). Plant 2 (unit only): the old rule, the published arena only.
inline bool SlotsReady(int32_t stage_arena, int32_t published, bool needs_slots_for_published, bool needs_slots_for_stage) {
  if (g_arena_id_plant == 2) return published >= 0 && !needs_slots_for_published;
  return stage_arena >= 0 && stage_arena == published && !needs_slots_for_stage;
}

// stage_ok: [stage + 1216] was read; word_ok: G+64 was read. Returns the arena id or -1 (unknown).
inline int32_t ResolveArenaId(bool stage_ok, uint32_t stage_id, bool word_ok, uint32_t word, ArenaIdCensus& c) {
  int32_t id = -1;
  const bool use_stage = stage_ok && stage_id < kMaxArenaId && g_arena_id_plant != 1;
  if (use_stage) {
    id = int32_t(stage_id);
    c.from_stage.fetch_add(1, std::memory_order_relaxed);
    if (word_ok && word != stage_id) c.disagree.fetch_add(1, std::memory_order_relaxed);
  } else if (word_ok && word < kMaxArenaId) {
    id = int32_t(word);
    c.from_word.fetch_add(1, std::memory_order_relaxed);
  } else {
    c.unknown.fetch_add(1, std::memory_order_relaxed);
  }
  c.resolved.fetch_add(1, std::memory_order_relaxed);
  return id;
}

// S111 CACBG (docs\research\s111\CACBG.md): the record names an arena only while the GAME holds a stage. [0x83763544] is the game's stage holder: sub_825E6078 (+100) and the
// created-arena loader sub_8342A598 store a new stage there; the stage loader zeroes it when it frees one (sub_827E4FC8, 0x827E51CC, right after the deleting destructor) [C: its three
// writers]. Create a Title's state 1 (sub_83328CA8 -> sub_827E41E0, loader step 2) frees the stage the previous creation mode left and builds none: without this rule the record kept
// naming that arena (the last published id, or the stale G+64), our cached copy was READY and the belt's walk made the frame OURS (the CAS editor's set behind the title editor).
// `holder_ok`: the word was readable (unreadable = not known: the old rule applies). The G+64 fallback of ResolveArenaId stays for a stage the game holds that StageFx has not seen set up.
// Plant 3 (unit only): the holder is never asked.
inline bool GameHoldsNoStage(bool holder_ok, uint32_t holder) {
  if (g_arena_id_plant == 3) return false;
  return holder_ok && holder == 0;
}
inline int32_t ArenaWhileStageHeld(bool no_stage, int32_t resolved) { return no_stage ? -1 : resolved; }

}  // namespace rdr_scene::live::arena
