#pragma once
// Pure inline: no hook, no cvar, no static (tools/lint/one_definition_lint.py). Moved verbatim from src/scene_director.h (EXEB-B1) so a game-fix unit can use it
// without pulling in the scene director.
#include <cstddef>
#include <cstdint>
#include <initializer_list>

#include <rex/ppc/context.h>

namespace wwe2k14::scene_director {

// Calls guest functions on the owner update's context, saving and restoring every register around each call (the launcher's recipe,
// src/scenario.h FireBuild): lr is 0 during the calls, so observe hooks show the caller as 00000000.
struct Caller {
  PPCContext& ctx;
  uint8_t* base;
  PPCContext saved;
  Caller(PPCContext& c, uint8_t* b) : ctx(c), base(b), saved(c) {}
  uint32_t operator()(void (*fn)(PPCContext&, uint8_t*), std::initializer_list<uint64_t> args) {
    ctx = saved;
    ctx.lr = 0;
    uint64_t* regs[] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64};
    size_t i = 0;
    for (uint64_t a : args) *regs[i++] = a;
    fn(ctx, base);
    const uint32_t r3 = ctx.r3.u32;
    ctx = saved;
    return r3;
  }
  // lane 161: a call that also passes a float in f1 (the PowerPC float argument register; sub_825F1F10 does `fmr f31,f1`). Same save / restore.
  uint32_t withf(void (*fn)(PPCContext&, uint8_t*), std::initializer_list<uint64_t> args, double f1) {
    ctx = saved;
    ctx.lr = 0;
    ctx.f1.f64 = f1;
    uint64_t* regs[] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64};
    size_t i = 0;
    for (uint64_t a : args) *regs[i++] = a;
    fn(ctx, base);
    const uint32_t r3 = ctx.r3.u32;
    ctx = saved;
    return r3;
  }
  // S110 PLACE: a call whose float RESULT (f1) is wanted (sub_82612BF8: the floor height); read before the restore.
  double retf(void (*fn)(PPCContext&, uint8_t*), std::initializer_list<uint64_t> args) {
    ctx = saved;
    ctx.lr = 0;
    uint64_t* regs[] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64};
    size_t i = 0;
    for (uint64_t a : args) *regs[i++] = a;
    fn(ctx, base);
    const double f1 = ctx.f1.f64;
    ctx = saved;
    return f1;
  }
  // S110 PLACE: a call that passes three floats in f1, f2, f3 (sub_829C0EE8: x, y, z).
  uint32_t withf3(void (*fn)(PPCContext&, uint8_t*), std::initializer_list<uint64_t> args, double f1, double f2, double f3) {
    ctx = saved;
    ctx.lr = 0;
    ctx.f1.f64 = f1;
    ctx.f2.f64 = f2;
    ctx.f3.f64 = f3;
    uint64_t* regs[] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64};
    size_t i = 0;
    for (uint64_t a : args) *regs[i++] = a;
    fn(ctx, base);
    const uint32_t r3 = ctx.r3.u32;
    ctx = saved;
    return r3;
  }
};

}  // namespace wwe2k14::scene_director
