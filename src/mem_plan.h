// wwe2k14 - S110 REPLAN (docs\research\s110\REPLAN.md): the boot hook, the cvar and the log lines of src\mem_plan_core.h (the why,
// the addresses and the plan table are there). sub_8225F0F0 (r3 = the memory config block 0x8392479C, r4 = the out block: +0 the
// heap block's address, +4 its size granted) has one caller, sub_827FE8A8 +16C, at boot. It reads the window span word (cfg + 24,
// +13C) and the heap block word (cfg + 28); every pair table is read after it returns (sub_827FE8A8 +1A0 the heaps, +1EC..+278 the
// windows) or later (the mode re-cuts read their tables from the image each time they run), so the whole plan is written here,
// before the original, and put back whole when either grant is short (the game's own budget in the room it got).
// --wwe2k14_mem_plan (default on); off: the game's sizes.
#pragma once

#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "mem_plan_core.h"
#include "native_mem_probe.h"  // wwe2k14::native_mem_probe::Ld32 / St32 (guest big-endian words)

REXCVAR_DEFINE_BOOL(wwe2k14_mem_plan, true, "wwe2k14",
                    "S110 REPLAN: one boot plan for the game's per-slot memory budgets (model data heaps 22..33 -> 2560 KB, "
                    "vertex-buffer windows 34..42 -> 2048 KB in every layout table; the heap block and the window span grown by the "
                    "same amount), so heavier imported models load; off: the game's sizes");

REX_EXTERN(__imp__sub_8225F0F0);

REX_HOOK_RAW(sub_8225F0F0) {
  namespace plan_ns = wwe2k14::mem_plan;
  namespace mp = wwe2k14::native_mem_probe;
  if (!REXCVAR_GET(wwe2k14_mem_plan)) {
    __imp__sub_8225F0F0(ctx, base);
    return;
  }
  const uint32_t out = ctx.r4.u32;
  auto rd = [&](uint32_t a, uint32_t* v) {
    *v = mp::Ld32(base, a);
    return true;
  };
  auto wr = [&](uint32_t a, uint32_t v) { mp::St32(base, a, v); };
  const plan_ns::Plan plan = plan_ns::Apply(rd, wr, ctx.r3.u32);
  __imp__sub_8225F0F0(ctx, base);
  if (!plan.ok) {
    REXLOG_WARN("[wwe2k14][memplan] every budget left at the game's sizes: {}", plan.why);
    return;
  }
  const uint32_t block_granted = mp::Ld32(base, out + 4);
  const uint32_t span_granted = mp::Ld32(base, plan_ns::kSpanGranted);
  if (block_granted < plan.block_to || span_granted < plan.span_to) {
    plan_ns::Restore(wr, plan);
    REXLOG_WARN("[wwe2k14][memplan] heap block asked 0x{:08X} granted 0x{:08X}, window span asked 0x{:08X} granted 0x{:08X}: every "
                "budget put back at the game's sizes",
                plan.block_to, block_granted, plan.span_to, span_granted);
    return;
  }
  REXLOG_INFO("[wwe2k14][memplan] {}; {} words in {} tables; heap block 0x{:08X} -> 0x{:08X} at 0x{:08X} (+{} KB), window span "
              "0x{:08X} -> 0x{:08X} at 0x{:08X} (+{} KB); {} KB left free above 0x01000000",
              plan_ns::Describe(), plan.words, plan_ns::kTableCount, plan.block_from, plan.block_to, mp::Ld32(base, out),
              plan.heap_added_kb, plan.span_from, plan.span_to, mp::Ld32(base, plan_ns::kSpanBase), plan.window_added_kb,
              plan.free_kb);
}
