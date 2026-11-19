// S103 EPS (360 HABIT row 10, docs\renderer\360_HABITS.md; probe docs\research\s103\PHS.md "BG66"): skip the game's own stage-page pass sub_825BB0E0 (it records a 512 x 512 pass + a copy every frame and replays them: GUEST-CPU work only, the stream-free build runs no GPU work for it)
// while the live stage has no page consumer. Pure rule, no game / renderer dependency, so a unit compiles it (tools\scene\page_skip_unit.cpp). The hook is in arena_hooks.h.
//
// THE RULE (all must hold): the stream-free build; the cvar wwe2k14_rdr_skip_unused_page on; the stage is known (its arena id [stage + 1216]); and that arena has NO page consumer:
//   - the flag the renderer publishes when it builds the arena's set (rf_c_core.cpp: page_base empty = the pac has no fade_lightmap00 + live page texture) says "page-less" FOR THIS ARENA ID; or
//   - FALLBACK while no flag has been published for this arena yet: arena 66 (the CAW editor's BG66, whose two pieces sample no page: PHS BG66 B11) AND the sequence id is 76 (the editor).
// The object's entrance word [this + 72] must read 0 (a set word = an entrance branch: never skipped, whatever the flag).
// An arena that HAS a page (flag "has page") is never skipped; an arena with no flag and not (66 + 76) is never skipped (fail closed). NEVER by phase: phase 4 is also the entrance, which needs the page.
//
// What a skip leaves out [C, q decomp sub_825BB0E0, docs\research\s103\PHS.md + the EPS adversary in the hand-back]: the recorded pass, the device-state save / restore around it (identity: every word it sets it
// restores from the getters read at its start), the 0x8392DD80 record's save + restore (sub_82274678 saves it and resets it, the loop at 0x825BB37C puts it back: identity) and ONE write that is not identity
// across threads: sub_82BCC510([0x8392DDA4], [obj + 64]) at 0x825BB358 stores the page texture handle at word +1180 of THIS thread's 2208-byte element (read back by sub_827F22C8's LightMapParam upload, 0x827F2434..).
// The hook replays exactly that write (arena_hooks.h), so the game state after a skipped call equals the state after a full one.
#pragma once

#include <atomic>
#include <cstdint>

namespace wwe2k14::pageskip {

inline constexpr int32_t kEditorArena = 66;      // the CAW editor's own stage (D:\PAC\BG\BG66.PAC; docs\systems\arena_look.md "Which arena is live")
inline constexpr uint32_t kEditorSequence = 76;  // scene sequence 76 = CAS (the editor), scene_kind.h

enum class PageFlag : uint32_t { Unknown = 0, HasPage = 1, Pageless = 2 };

inline std::atomic<bool> g_on{false};            // wwe2k14_rdr_skip_unused_page (stored once at start by start_settings.cpp; the stream-free build only)
inline std::atomic<uint64_t> g_flag{0};          // (arena + 1) << 8 | PageFlag; 0 = nothing published. ONE word: the arena and its answer are never read torn.
inline std::atomic<uint64_t> g_skipped{0}, g_called{0};   // census (arena_hooks.h prints them every 600 frames)

// Published by the renderer thread when it builds an arena's set (the arena's own answer; re-published when the set is rebuilt).
inline void PublishPageFlag(int32_t arena, bool has_page) {
  if (arena < 0) return;
  g_flag.store((uint64_t(uint32_t(arena) + 1u) << 8) | uint64_t(has_page ? PageFlag::HasPage : PageFlag::Pageless), std::memory_order_release);
}
inline PageFlag PageFlagFor(int32_t arena) {
  const uint64_t v = g_flag.load(std::memory_order_acquire);
  if (v == 0 || arena < 0 || (v >> 8) != uint64_t(uint32_t(arena) + 1u)) return PageFlag::Unknown;   // another arena's answer is no answer
  return PageFlag(uint32_t(v & 0xFFu));
}
inline void ResetPageFlag() { g_flag.store(0, std::memory_order_release); }   // tests / an arena unload

struct Inputs {
  bool stream_free = false;   // !WWE2K14_STREAM()
  bool cvar_on = false;
  bool stage_known = false;   // StageIdRead succeeded
  int32_t stage_arena = -1;   // [stage + 1216] as a signed id
  bool seq_known = false;     // the sequence-id chain read
  uint32_t seq_id = 0;
  PageFlag flag = PageFlag::Unknown;   // PageFlagFor(stage_arena)
  bool entrance_word_known = false;    // [this + 72] (= object + 88) read
  uint32_t entrance_word = 0;          // non-zero = the entrance branch runs sub_825BACE0 (writes obj + 416 + 16 i): never skipped
};

// true = the page pass is skipped.
inline bool ShouldSkip(const Inputs& in) {
  if (!in.stream_free || !in.cvar_on || !in.stage_known || in.stage_arena < 0) return false;
  if (!in.entrance_word_known || in.entrance_word != 0) return false;   // EPS review 2: skip only on the plain fade branch, so "identity" holds by construction; an unreadable word fails closed
  if (in.flag == PageFlag::HasPage) return false;
  if (in.flag == PageFlag::Pageless) return true;
  return in.stage_arena == kEditorArena && in.seq_known && in.seq_id == kEditorSequence;   // the documented fallback (no flag for this arena yet)
}

}  // namespace wwe2k14::pageskip
