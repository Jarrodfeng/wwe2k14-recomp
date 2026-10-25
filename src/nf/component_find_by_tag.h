// wwe2k14 - Session 66 (native function factory, track F2): native version of sub_82C31CB0 (db component_find_by_tag).
//
// What it does (listing: tools\analysis\list_function.py 0x82C31CB0; a leaf with no frame and no calls): r3 = a node,
// r4 = a 32-bit tag; returns in r3 the node found, or 0. Nodes keep a tag word at +4, a "next child" link at +20 and a
// "next sibling" link at +16. The search order, mirrored exactly:
//   1. r3 == 0 (low 32 bits)                  -> 0.
//   2. Follow the +20 chain from the node: up to 255 links; for each link that is non-zero, its tag word (+4) is compared
//      with r4 and a match returns that node. When the 255th link has been looked at without a match the result is 0 (the
//      sibling chain is NOT tried in that case).
//   3. When the +20 chain ends (a link is 0) before the cap: the node itself is tested (tag at node+4; a match returns the
//      node, r3 left exactly as it came in), then the +16 chain from the node: each link is tested (0 ends the search with
//      0, a match returns it); this chain has no cap.
// Tag compares are 32-bit unsigned equality; every load is a plain big-endian word load. Nothing is written to memory.
//
// Registers on return: r3 only (the zero-extended link that matched, 0, or the unchanged input when the node itself
// matched). The original also leaves cr6, r9, r10, r11 changed (volatile; callers do not read them).
//
// Could still differ: a chain that loops through the +16 links forever hangs both versions the same way (no cap there);
// another thread rewriting a link between the two verify runs is classified a race by the verify mode.
//
// --wwe2k14_nf_component_find_by_tag = off | verify | on (default off), or the global --wwe2k14_nf_all.

#pragma once

#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../native_fn.h"

REXCVAR_DEFINE_STRING(wwe2k14_nf_component_find_by_tag, "off", "wwe2k14",
                      "Session 66: native version of sub_82C31CB0 (find a component node by tag): off | verify | on");

namespace wwe2k14::nf_component_find_by_tag {

inline uint32_t Ld(const uint8_t* base, uint32_t a) {
  uint32_t v;
  __builtin_memcpy(&v, rex::memory::GuestPtr(const_cast<uint8_t*>(base), a), 4);
  return __builtin_bswap32(v);
}

inline void Run(PPCContext& ctx, uint8_t* base) {
  const uint32_t node = ctx.r3.u32, tag = ctx.r4.u32;
  if (node == 0) {
    ctx.r3.u64 = 0;
    return;
  }
  uint32_t cur = node;
  for (int i = 0; i < 255; ++i) {
    cur = Ld(base, cur + 20);
    if (cur == 0) {
      // The child chain ended early: the node itself, then its sibling chain.
      if (Ld(base, node + 4) == tag) return;  // r3 unchanged
      uint32_t s = node;
      for (;;) {
        s = Ld(base, s + 16);
        if (s == 0) {
          ctx.r3.u64 = 0;
          return;
        }
        if (Ld(base, s + 4) == tag) {
          ctx.r3.u64 = s;
          return;
        }
      }
    }
    if (Ld(base, cur + 4) == tag) {
      ctx.r3.u64 = cur;
      return;
    }
  }
  ctx.r3.u64 = 0;
}

struct Spec {
  static constexpr const char* kName = "component_find_by_tag";
  static constexpr uint32_t kGuest = 0x82C31CB0;
  static constexpr uint32_t kRegs = native_fn::kR3;
  static native_fn::Mode GetMode() {
    return native_fn::ModeFromCvar(REXCVAR_GET(wwe2k14_nf_component_find_by_tag));
  }
  static void Writes(const PPCContext& ctx, uint8_t* base, native_fn::Ranges& out) {
    (void)ctx;
    (void)base;
    (void)out;  // reads only
  }
  static void Native(PPCContext& ctx, uint8_t* base) { Run(ctx, base); }
};

}  // namespace wwe2k14::nf_component_find_by_tag

WWE2K14_NATIVE_FN(sub_82C31CB0, wwe2k14::nf_component_find_by_tag::Spec)
