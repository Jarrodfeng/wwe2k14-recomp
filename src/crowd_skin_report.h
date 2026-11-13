// wwe2k14 - Session 100, strip-360 E3 (STRIP360_PLAN.md part 2): the crowd's two other CPU skinning loops report what they write.
//
// crowd_manager_update (0x8250BAF0) calls four skinning variants in a row (listing 0x8250BCEC-0x8250BD1C), each r3 = the manager, r4 = a
// descriptor (vertex count at +0), r5 = the bone matrices, r6 = the output (28-byte vertex records: position, normal, half-float u / v):
// sub_825085F8 (one bone), sub_82508738 (two; native, src\native_skin.h), sub_82508958 (three; native, src\nf\skin_3bone.h), sub_82508C08
// (four influences). The two natives report through native_fn.h; these two run the game's own code and report after it (listings read
// 2026-11-11: count `lwz 0(r4)`, output `addi 12(r6)` advancing 28 a vertex at 0x8250871C / 0x82508F0C). Census 6976 / 6984: crowd
// streams changed with no report before these hooks (0x1679B000 / 0x1679C000 3,276 B, 0x167E2000 / 0x167E5000 8,540 B).
#pragma once

#include <cstdint>

#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/system/xmemory.h>

#include "host_write.h"
#include "scene_live/skin_skip_hook.h"  // S101 lane 50

REX_EXTERN(__imp__sub_825085F8);
REX_EXTERN(__imp__sub_82508C08);

namespace wwe2k14::crowd_skin_report {

inline uint32_t Count(uint8_t* base, uint32_t descriptor) {
  const uint8_t* q = rex::memory::GuestPtr(base, descriptor);
  return (uint32_t(q[0]) << 24) | (uint32_t(q[1]) << 16) | (uint32_t(q[2]) << 8) | uint32_t(q[3]);
}

template <void (*Original)(PPCContext&, uint8_t*)>
inline void Run(PPCContext& ctx, uint8_t* base) {
  if (wwe2k14::crowd_skip::SkinLoopSkipped(ctx, base)) return;  // S101 lane 50 (P2c): the loop is not run while ours draws and nothing reads its group; nothing written, nothing reported
  const uint32_t count = Count(base, ctx.r4.u32), out = ctx.r6.u32;
  Original(ctx, base);
  if (int32_t(count) > 0) host_write::NoteBy(out, count * 28u, host_write::Writer::CrowdSkin);  // S101 A1c
}

}  // namespace wwe2k14::crowd_skin_report

REX_HOOK_RAW(sub_825085F8) { wwe2k14::crowd_skin_report::Run<__imp__sub_825085F8>(ctx, base); }
REX_HOOK_RAW(sub_82508C08) { wwe2k14::crowd_skin_report::Run<__imp__sub_82508C08>(ctx, base); }
