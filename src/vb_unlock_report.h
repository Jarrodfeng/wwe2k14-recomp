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

REX_EXTERN(__imp__sub_82CACE50);

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
}
