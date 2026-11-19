// S103 RPF (docs\research\s103\RPL.md, docs\systems\wrestler_pose.md "S103 RPL"): the game's REPLAY poses the wrestler bodies with its CHAR track
// (vtable 0x82028970, slot 3 = sub_8270ECE8), writing [[char+48]+180/192/200] directly and never calling the skeleton update sub_82BD30E8. The contributor's liveness
// (wrestler_contrib.cpp Collect) knew only the update hook and the model-draw walk (never set in the stream-free build), so both bodies went quiet_unseen and were not drawn.
// This hook is READ ONLY: it calls the original unchanged, then reports [[track+136]+48] (track+136 = the character, char+48 = the body skeleton) to Wrestlers().NoteReplayPose.
// Review (Opus, ec06f1e01): the track HIDES the character when the keyframe record's bit 0 is clear (loc_8270EE10 -> sub_82271FE0(char+224, 0): stw r4, 8(r3) = the word [char+232] = 0, no pose); only
// loc_8270EE44 sets it to 1 and poses. So after the original [char+232] != 0 = posed and shown (stamp), 0 = hidden (the pose stamp is cleared: the old rule applies at once).
// Included from draw_origin.h (one TU), both builds. Guarded reads: null / unreadable = nothing reported. Not registered / scene renderer off = the original only (as wrestler_hook.h OnSkelIn).
#pragma once
#include <rex/hook.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <cstdint>

#include "../frame_stats.h"  // g_guest_frame
#include "wrestler_contrib.h"
#include "wrestler_hook.h"   // ReadWord (page-checked off Windows)

REX_EXTERN(__imp__sub_8270ECE8);
REX_HOOK_RAW(sub_8270ECE8) {
  const uint32_t track = ctx.r3.u32;   // saved before the call: the original clobbers r3
  __imp__sub_8270ECE8(ctx, base);
  if (!rdr_scene::live::WrestlersRegistered() || !rdr_scene::live::Enabled()) return;
  uint32_t ch = 0, skel = 0, shown = 0;
  if (track && wwe2k14::wrestlers::ReadWord(track + 136, ch) && ch && wwe2k14::wrestlers::ReadWord(ch + 48, skel) && skel && wwe2k14::wrestlers::ReadWord(ch + 232, shown))
    rdr_scene::live::Wrestlers().NoteReplayPose(skel, wwe2k14::g_guest_frame.load(std::memory_order_relaxed), shown != 0);
}
