// wwe2k14 - Session 95: the two vector x matrix functions on Android (NEON through simde): the lean version only, the
// recompiled original for non-finite inputs and an unaligned r1 (the strict versions in nf/vec4_mat4_*.h are x86-64).
// Same operations as nf/vec4_lean.h on x86-64. Not yet run on a phone.
// --wwe2k14_nf_vec4_mat4_inplace, --wwe2k14_nf_vec4_mat4_row = off | verifylean | lean, or --wwe2k14_nf_lean.

#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>

#include "../native_fn.h"
#include "vec4_lean.h"

REXCVAR_DEFINE_STRING(wwe2k14_nf_vec4_mat4_inplace, "off", "wwe2k14",
                      "Session 95: native sub_82C32A88 (vec4 times mat4, in place): off | verifylean | lean");
REXCVAR_DEFINE_STRING(wwe2k14_nf_vec4_mat4_row, "off", "wwe2k14",
                      "Session 95: native sub_82C32BC0 (row vector times mat4): off | verifylean | lean");

REX_EXTERN(__imp__sub_82C32A88);
REX_EXTERN(__imp__sub_82C32BC0);

namespace wwe2k14::nf_vec4_arm {

struct Inplace {
  static constexpr const char* kName = "vec4_mat4_inplace";
  static constexpr uint32_t kGuest = 0x82C32A88;
  static constexpr uint32_t kRegs = native_fn::kR3;
  static native_fn::Mode GetMode() { return native_fn::ModeFromCvar(REXCVAR_GET(wwe2k14_nf_vec4_mat4_inplace)); }
  static void Writes(const PPCContext& ctx, uint8_t*, native_fn::Ranges& out) { out.Add(ctx.r3.u32, 16); }
  static void Native(PPCContext& ctx, uint8_t* base) {
    if (!nf_vec4_lean::Run<false>(ctx, base)) __imp__sub_82C32A88(ctx, base);
  }
  static void Lean(PPCContext& ctx, uint8_t* base) { Native(ctx, base); }
};
struct Row {
  static constexpr const char* kName = "vec4_mat4_row";
  static constexpr uint32_t kGuest = 0x82C32BC0;
  static constexpr uint32_t kRegs = native_fn::kR3;
  static native_fn::Mode GetMode() { return native_fn::ModeFromCvar(REXCVAR_GET(wwe2k14_nf_vec4_mat4_row)); }
  static void Writes(const PPCContext& ctx, uint8_t*, native_fn::Ranges& out) { out.Add(ctx.r3.u32, 16); }
  static void Native(PPCContext& ctx, uint8_t* base) {
    if (!nf_vec4_lean::Run<true>(ctx, base)) __imp__sub_82C32BC0(ctx, base);
  }
  static void Lean(PPCContext& ctx, uint8_t* base) { Native(ctx, base); }
};

}  // namespace wwe2k14::nf_vec4_arm

WWE2K14_NATIVE_FN(sub_82C32A88, wwe2k14::nf_vec4_arm::Inplace)
WWE2K14_NATIVE_FN(sub_82C32BC0, wwe2k14::nf_vec4_arm::Row)
