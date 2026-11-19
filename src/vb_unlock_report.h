// wwe2k14 - Session 100, strip-360 E3 (STRIP360_PLAN.md part 2): the game's vertex-buffer unlock reports the buffer's bytes to the new
// renderer's reader (src\host_write.h): the game's own "the CPU finished writing this buffer", like the texture unlock flush (0x82CA6BE0).
//
// Found by watch run 6989: three shadow-pass vertex streams rewritten in place during WM30's match load (memory reuse; census 6976 / 6987:
// the only geometry changes no report announced) were written by the vertex packer sub_82BD4BE0 (pack_float3_to_10_11_11 sub_82C14BC0
// and its siblings) inside sub_82BD7790, which creates the buffer (sub_82CACD88), locks it (sub_82CADF68), fills it and unlocks it with
// sub_82CACE50: r3 = the buffer object; it tail-calls the generic resource flush sub_82CACA40 with the data address (listing). The object
// (sub_82CACD88's stores, listing 0x82CACE0C-0x82CACE40): +24 = the data's CPU virtual address | 3, +28 = the byte size in bits 6-29
// (& 0x03FFFFFC) with flag bits around it. 18 call sites in 15 functions call this unlock.
#pragma once

#include <cstdint>

#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/system/xmemory.h>

#include "host_write.h"
#include "scene_live/buffer_unlock_log.h"   // S103 CW-2b: the unlock ring the guest model reader asks (a read whose buffers were unlocked since is re-read)

REX_EXTERN(__imp__sub_82CACE50);
REX_EXTERN(__imp__sub_82CACF10);

namespace wwe2k14::vb_unlock_report {

inline uint32_t Ld32(uint8_t* base, uint32_t at) {
  const uint8_t* q = rex::memory::GuestPtr(base, at);
  return (uint32_t(q[0]) << 24) | (uint32_t(q[1]) << 16) | (uint32_t(q[2]) << 8) | uint32_t(q[3]);
}

}  // namespace wwe2k14::vb_unlock_report

REX_HOOK_RAW(sub_82CACE50) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t data = object ? wwe2k14::vb_unlock_report::Ld32(base, object + 24) & ~3u : 0;
  const uint32_t bytes = object ? wwe2k14::vb_unlock_report::Ld32(base, object + 28) & 0x03FFFFFCu : 0;
  __imp__sub_82CACE50(ctx, base);
  if (data && bytes) wwe2k14::host_write::NoteBy(data, bytes, wwe2k14::host_write::Writer::VertexUnlock);  // S101 A1c
  if (data) rdr_scene::live::unlocklog::Note(data);   // S103 CW-2b: after the original ran (its bytes are final)
}

// S103 CW-2b (CW2_review bug 1): the index buffer's unlock. sub_82CACF10 (12 bytes: r4 = [r3+24], tail-calls sub_82CACA40; q decomp) is the unlock the index fill ends with (sub_82CADFB8 lock -> fill ->
// unlock, PCW_adversary.md section 1 item 7): r3 = the buffer object, +24 = its data address (an index buffer's has no fetch-type bits). Only the guest model reader's ring is told: the reader's host-write
// marks (host_write) are a different consumer's and are not changed by this lane.
REX_HOOK_RAW(sub_82CACF10) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t data = object ? wwe2k14::vb_unlock_report::Ld32(base, object + 24) & ~3u : 0;
  __imp__sub_82CACF10(ctx, base);
  if (data) rdr_scene::live::unlocklog::Note(data);
}
