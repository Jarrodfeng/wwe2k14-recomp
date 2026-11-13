// wwe2k14 - Session 100, strip-360 tier C step 1 (STRIP360_PLAN.md part 2; docs\native\mem_tier_c_flat_arena.md section 2): the 360's rule
// for physical memory seen through the CPU's views, in one place of ours (it was spelled out in 16 places in 7 files).
//
// The console reaches physical memory through three CPU views: 0xA0000000 (64 KB pages), 0xC0000000 (16 MB pages) and 0xE0000000 (4 KB
// pages). The physical (GPU) address of a view address is its low 29 bits, plus 4 KB for the 0xE0000000 view: the console's own layout,
// which the game's D3D library applies when it forms GPU addresses (a CPU pointer 0xFC545000 is GPU 0x1C546000). The SDK maps the views
// so that host = membase + guest (S95), with the 0xE0000000 view 4 KB further into the physical mapping. Everything of ours that turns a
// CPU view address into a GPU address calls this; removing the rule itself needs the game's D3D library out of the path (tier F).
#pragma once

#include <cstdint>

namespace wwe2k14::guest_view {

// 0x1000 for an address in the 0xE0000000 view, 0 for any other (the library's ((a >> 20) + 512) & 0x1000).
constexpr uint32_t ViewOffset(uint32_t va) { return (((va >> 20) & 0xFFFu) + 512u) & 0x1000u; }
// A CPU view address as the GPU address of the same byte (the low 29 bits plus the view's offset).
constexpr uint32_t ToPhysical(uint32_t va) { return ViewOffset(va) + (va & 0x1FFFFFFFu); }

static_assert(ToPhysical(0xFC545000u) == 0x1C546000u, "the 0xE0000000 view is 4 KB higher");
static_assert(ToPhysical(0xBC546000u) == 0x1C546000u && ToPhysical(0xDC546000u) == 0x1C546000u, "the 0xA / 0xC views are not");

}  // namespace wwe2k14::guest_view
