// wwe2k14 - S103 VTG2: ONE switch for the retro arenas' vintage look. The sepia is the post pass (already follows PlayerQuality::film_grain in both builds); the darkening, grain and
// vignette are the stage component VTGE's draw method sub_82594808 (vtable 0x8201E20C slot 1, r3 = component + 48; docs\research\s103\VTG.md "VTG2"). With the setting off the method is
// skipped. The flag is read every call, but PlayerQuality::film_grain is written only at start (ApplyStartSettings, before the guest runs; and only
// with rdr_owner, so a stream build with the old renderer keeps the look): a live toggle would need a writer of g_quality.film_grain (Opus review). Do NOT clear the stage record's bytes +41 / +42 / +43 instead:
// sub_82A19A18 (cutscene_program_build) also reads +42 / +43.
#pragma once

#include <rex/hook.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <cstring>

#include "frame_word.h"
#include "lever_scene.h"
#include "start_settings.h"
#include "step_rate.h"
#include "vintage_grain_rate_core.h"
#include "vintage_look_core.h"

REX_EXTERN(__imp__sub_82594808);

namespace wwe2k14::vintage {
inline std::atomic<uint32_t> g_grain_period_unsane{0};   // calls where the period word was 0 or above 64 (not swapped)
}

REX_HOOK_RAW(sub_82594808) {
  namespace wv = wwe2k14::vintage;
  if (!wwe2k14::start::Quality().film_grain) return;
  // S110 GRAINRATE (note GRAINCAP): the game's 30 fps scene shows each grain picture 66.7 ms (timer += step, reset at the period 3); at our 60 it is 50 ms.
  // With k > 1, swap the period word ([this+1920], this = comp+48) for the one that keeps 66.7 ms for the one call and put it back; k == 1 is today's call.
  const bool lever = wwe2k14::g_lever_scene.load(std::memory_order_relaxed);
  const uint32_t gw = wwe2k14::step_rate::g_game_word.load(std::memory_order_relaxed);
  const int32_t hz = wwe2k14::step_rate::g_applied_hz.load(std::memory_order_relaxed);
  const uint32_t k = wwe2k14::HoldFactor(lever, gw, wwe2k14::g_frame_word.load(std::memory_order_relaxed), hz);
  if (k <= 1) {
    wv::Run(ctx, base, true, __imp__sub_82594808);
    return;
  }
  const uint32_t self = ctx.r3.u32;
  uint32_t* period = reinterpret_cast<uint32_t*>(rex::memory::GuestPtr(base, self + 1920));
  const uint32_t saved = rex::memory::load_and_swap<uint32_t>(period);
  if (!wv::GrainPeriodSane(saved)) {
    wv::g_grain_period_unsane.fetch_add(1, std::memory_order_relaxed);
    wv::Run(ctx, base, true, __imp__sub_82594808);
    return;
  }
  const bool scaled_low = hz != 0 && (gw == 30 || gw == 25);   // the game's own word, as HoldFactor reads it
  const uint32_t own = lever ? 30u : scaled_low ? gw : 60u;
  const uint32_t stepbits = rex::memory::load_and_swap<uint32_t>(rex::memory::GuestPtr(base, 0x8392DC74u));
  float step_ours;
  std::memcpy(&step_ours, &stepbits, sizeof step_ours);
  rex::memory::store_and_swap<uint32_t>(period, wv::GrainPeriod(saved, k, own, step_ours));
  wv::Run(ctx, base, true, __imp__sub_82594808);
  rex::memory::store_and_swap<uint32_t>(period, saved);   // the game always sees its own period outside the call
}
