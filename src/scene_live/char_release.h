// S111 FREEHOOK (docs\research\s111\FREEHOOK.md): what our side does when the game destroys a CHARACTER. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// No SDK include: the hook (model_release_hook.h, REX_HOOK_RAW on sub_825A1510) calls OnCharacterDestroyed, and tools\scene\inputs_unit.cpp drives OnCharacterFreed (freehook.* and its twins).
//
// The game's order for a slot character it frees in a match [C, listings]: the release (sub_8251ECA0 / sub_8251F150, an update task of period X) clears the table word and queues the character;
// the walk of period X may still draw it (its own draw node, not the table, draws it: drawn_slots.h); the post-barrier tick of period X (sub_8225FE68 -> sub_8251E928 +84, before our latch X) runs
// slot 0 of its vtable, sub_825A2A30: the destructor body sub_825A1510 (+1C), then the free of the block (+34). Inside the body the base sub_823CCDD0 +100 deletes the model [char+48], whose deleting
// destructor sub_82BD99F8 runs the model release sub_82BD91B0 (+28: frees [model+200], the palette, at +1E8) BEFORE the model hook sub_82BD3B40 (+30). So the model hook sees no palette and no
// character; this hook, at the ENTRY of sub_825A1510, sees both while nothing is freed:
//   1. the departure reader (drawn_slots.h Departed, the InputsContributor's) forgets the character: nothing of ours reads the block after the free. (S111 PIECECARRY: the draw-ask list needs no forget:
//      latch X republishes it from its own table before any ask of period X + 1, and NoteDrawAsked's reads are page-checked and owner-tested, wrestler_contrib.h NoteDrawAsked read_flag.)
//   2. latch X's reads of the body, made here (the tick runs after the barrier wait 0x82BB5F5C, every writer parked: what the walk of X drew), all labelled as latch X labels them (hook =
//      HookFrameOfRecord(X); its reads are tagged hook + kPaletteLead = X): the palette [[char+48]+200]; and (S111 PIECECARRY, docs\research\s111\PIECECARRY.md) the descriptor check that names it,
//      its SUBM pieces with their spring-hair rows (WrestlerContributor::NoteFreedLast: the body's last record) and the palettes of the pieces that latch would have read. The model hooks that follow
//      (ForgetModel) move them into the freed copies, so record X + 1, which pairs with the walk of X and takes latch X's links, shows the body WHOLE from one read (posed, named, its pieces and rows;
//      FreedRevive serves it only when that walk asked for it).
// Thread: the tick runs on the swap thread, before OnPresent's latch on the same thread; another caller may run on another thread: every call below takes its own lock or is atomic.
#pragma once
#include <cstdint>
#include <vector>

#include "inputs_contrib.h"
#include "inputs_core.h"
#include "wrestler_contrib.h"

namespace wwe2k14::char_release {

inline constexpr uint32_t kPlantNoPose = 1;     // test only: the old hook (no palette kept at the destructor): record X + 1 has no pose (counted FreedUnposed)
inline constexpr uint32_t kPlantNoForget = 2;   // test only: the departure reader not told: the next latch reads the freed block
inline constexpr uint32_t kPlantNoLast = 4;     // test only (S111 PIECECARRY): the FREEHOOK hook (the body's palette only): record X + 1 shows the body with no pieces, no spring-hair rows and no name

// `clock` = wwe2k14::g_guest_frame when the game destroys `ch` (period X); `read` = the guest reader (page-checked). `in` may be null (no departure reader, no piece read: the palette only).
inline void OnCharacterFreed(rdr_scene::live::WrestlerContributor& w, rdr_scene::live::inputs::InputsContributor* in, uint32_t ch, uint64_t clock, scene_inputs::ReadFn read, uint32_t plants = 0) {
  if (ch < 0x10000) return;
  if (in && !(plants & kPlantNoForget)) in->ForgetCharacter(ch);
  if ((plants & kPlantNoPose) || !read || scene_frame::CurrentMode() == scene_frame::Mode::Off) return;   // mode Off: the latch reads nothing either (inputs_contrib.h LatchPalettes)
  uint32_t skel = 0;
  if (!scene_inputs::ReadWord(read, ch + scene_inputs::drawn_slots::kCharSkeleton, skel) || skel < 0x10000) return;   // no body model: nothing to pose
  const uint32_t count = w.WantCount(skel);   // the update hook's node count; 0 = a model we never met (not a body of ours)
  if (!count) return;
  const uint64_t hook = scene_inputs::HookFrameOfRecord(clock);   // latch X's hook frame: its reads are tagged hook + kPaletteLead = X
  static thread_local std::vector<rdr_scene::Mat4> pal;   // the caller's thread's scratch
  static thread_local std::vector<uint32_t> raw;
  static thread_local std::vector<rdr_scene::live::WrestlerContributor::PaletteTarget> piece_targets;
  static thread_local rdr_scene::live::inputs::InputsContributor::SlotTable tbl;
  if (!scene_inputs::ReadPalette(read, skel, count, pal, raw)) return;   // no palette of this read: no last record either (the copy keeps latch X - 1's, as before)
  w.SubmitLatchConsumer(skel, count, pal.data(), hook);
  piece_targets.clear();
  if (in && !(plants & kPlantNoLast)) {   // S111 PIECECARRY: the rest of latch X's reads of this body, in ReadSlotSkeletons' order (the check first: the pieces' build-path proof asks for this latch's verdict)
    in->VerifyBodyIdentity(skel);
    tbl.pieces.clear();
    in->ReadPiecesOf(ch, skel, -1, tbl, false);   // the slot is the body's Known::slot (NoteFreedLast); no census (the latch's own)
    w.NoteFreedLast(skel, hook, tbl.pieces, piece_targets);
  }
  for (const auto& pt : piece_targets)
    if (scene_inputs::ReadPalette(read, pt.skel, pt.count, pal, raw)) w.SubmitLatchConsumer(pt.skel, pt.count, pal.data(), hook);
}

// The hook's call: the process singletons.
inline void OnCharacterDestroyed(uint32_t ch, uint64_t clock, scene_inputs::ReadFn read) {
  OnCharacterFreed(rdr_scene::live::Wrestlers(), &rdr_scene::live::inputs::Inputs(), ch, clock, read);
}

}  // namespace wwe2k14::char_release
