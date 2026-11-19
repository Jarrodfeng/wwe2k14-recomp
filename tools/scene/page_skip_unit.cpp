// S103 EPS: the unused stage-page pass rule (src\scene_live\page_skip_core.h, hooked on sub_825BB0E0 in arena_hooks.h). Compile only, no game. Exit 1 on a failure.
// Cases: a page-less arena in the editor (BG66 + sequence 76, with and without the renderer's flag) is skipped; an arena WITH a page (an entrance, a match) is never skipped, whatever the phase / sequence;
// the switch off, the stream build, an unknown stage, a flag of another arena all keep the game's pass; the flag is one word (arena + answer).
// `--plant fake_pageless_entrance`: a copy of the rule that trusts "page-less" from the phase word (4) alone, as the first probe's sketch would: an entrance (phase 4, seq 9) is then skipped. It MUST fail.
// Call: page_skip_unit.exe [--plant fake_pageless_entrance]
#include <cstdio>
#include <cstring>

#include "../../src/scene_live/page_skip_core.h"

namespace ps = wwe2k14::pageskip;

static int g_fail = 0;
static void Expect(bool ok, const char* what) { if (!ok) std::printf("FAIL  %s\n", what), ++g_fail; }
static bool g_plant = false;

// the planted twin: the page-less verdict is read from the phase word (4 = "not the match") instead of the arena's own flag. Phase 4 is also the entrance, which needs the page.
static bool PlantShouldSkip(const ps::Inputs& in, uint32_t phase) {
  if (!in.stream_free || !in.cvar_on || !in.stage_known) return false;
  return phase == 4;
}
static bool Decide(const ps::Inputs& in, uint32_t phase) { return g_plant ? PlantShouldSkip(in, phase) : ps::ShouldSkip(in); }

static ps::Inputs Base(int32_t arena, uint32_t seq, ps::PageFlag flag) {
  ps::Inputs in;
  in.stream_free = true, in.cvar_on = true, in.stage_known = true, in.stage_arena = arena;
  in.seq_known = true, in.seq_id = seq;
  in.flag = flag;
  in.entrance_word_known = true, in.entrance_word = 0;
  return in;
}

int main(int argc, char** argv) {
  g_plant = argc == 3 && std::strcmp(argv[1], "--plant") == 0 && std::strcmp(argv[2], "fake_pageless_entrance") == 0;
  // the editor: BG66, sequence 76, phase 4
  Expect(Decide(Base(66, 76, ps::PageFlag::Pageless), 4), "editor, flag page-less: skipped");
  Expect(Decide(Base(66, 76, ps::PageFlag::Unknown), 4), "editor, no flag yet (fallback 66 + 76): skipped");
  Expect(!Decide(Base(66, 9, ps::PageFlag::Unknown), 4), "arena 66 outside sequence 76, no flag: not skipped");
  // an entrance: a page-ful arena (4) in sequence 9 with phase 4 (the entrance word) - the planted twin skips it
  Expect(!Decide(Base(4, 9, ps::PageFlag::HasPage), 4), "entrance (arena 4, seq 9, phase 4, has page): NOT skipped");
  Expect(!Decide(Base(4, 9, ps::PageFlag::HasPage), 1), "match (arena 4, seq 9, phase 1, has page): NOT skipped");
  Expect(!Decide(Base(4, 9, ps::PageFlag::Unknown), 4), "entrance, flag not published yet: fail closed, NOT skipped");
  Expect(!Decide(Base(66, 76, ps::PageFlag::HasPage), 4), "a flag that says has-page beats the fallback");
  {  // EPS review 2: a set entrance word ([this + 72] != 0: the branch that runs sub_825BACE0) is never skipped, even for a page-less flag in the editor's own arena
    ps::Inputs ent = Base(66, 76, ps::PageFlag::Pageless);
    ent.entrance_word = 1;
    Expect(!Decide(ent, 4), "entrance word set, page-less flag: NOT skipped");
  }
  if (!g_plant) {   // the rest are the real rule's own edges (the plant has no notion of them)
    ps::Inputs off = Base(66, 76, ps::PageFlag::Pageless);
    off.cvar_on = false;
    Expect(!ps::ShouldSkip(off), "switch off: not skipped");
    ps::Inputs stream = Base(66, 76, ps::PageFlag::Pageless);
    stream.stream_free = false;
    Expect(!ps::ShouldSkip(stream), "stream build: not skipped");
    ps::Inputs nostage = Base(66, 76, ps::PageFlag::Pageless);
    nostage.stage_known = false;
    Expect(!ps::ShouldSkip(nostage), "stage unknown: not skipped");
    ps::Inputs unread = Base(66, 76, ps::PageFlag::Pageless);
    unread.entrance_word_known = false;
    Expect(!ps::ShouldSkip(unread), "entrance word unreadable: not skipped (fail closed)");
    ps::Inputs noseq = Base(66, 76, ps::PageFlag::Unknown);
    noseq.seq_known = false;
    Expect(!ps::ShouldSkip(noseq), "fallback with an unreadable sequence: not skipped");
    // the published flag: one word per arena; another arena's answer is no answer
    ps::ResetPageFlag();
    Expect(ps::PageFlagFor(66) == ps::PageFlag::Unknown, "nothing published: unknown");
    ps::PublishPageFlag(66, false);
    Expect(ps::PageFlagFor(66) == ps::PageFlag::Pageless, "66 published page-less");
    Expect(ps::PageFlagFor(4) == ps::PageFlag::Unknown, "66's answer is not arena 4's");
    ps::PublishPageFlag(4, true);
    Expect(ps::PageFlagFor(4) == ps::PageFlag::HasPage, "4 published has-page");
    Expect(ps::PageFlagFor(66) == ps::PageFlag::Unknown, "the newer arena replaced 66's answer (the stage is one at a time)");
    // the whole chain: an entrance on arena 4 after the editor left a page-less flag behind
    ps::PublishPageFlag(66, false);
    ps::Inputs ent = Base(4, 9, ps::PageFlagFor(4));
    Expect(!ps::ShouldSkip(ent), "entrance after the editor: arena 4 has no flag of its own: not skipped");
    ps::Inputs ed = Base(66, 76, ps::PageFlagFor(66));
    Expect(ps::ShouldSkip(ed), "editor after an entrance: skipped");
  }
  std::printf("page_skip_unit: %s\n", g_fail ? "FAIL" : "PASS");
  return g_fail ? 1 : 0;
}
