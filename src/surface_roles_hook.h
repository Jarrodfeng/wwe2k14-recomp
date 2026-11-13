// wwe2k14 - Session 101 (renderer arc A1b, docs\renderer\NAMES.md "Surface roles"): remember which game function created each render-target
// header, so the renderer gives each surface the role its creator declares instead of guessing from its shape (the 16:9 rule).
// The surface creator sub_82CA88E0 takes r3 width, r4 height, r5 format word and returns the 48-byte header it allocated (listing read
// S101; docs\systems\render_targets.md). Creates happen at loads, not per frame: always recorded (the reader may start later).
#pragma once

#include <rex/hook.h>
#include <rex/ppc/context.h>

#include <cstdint>

#include "renderer_adapter/surface_creators.h"

REX_HOOK_RAW(sub_82CA88E0) {
  rdr_adapter::roles::Created c;
  c.call_site = uint32_t(ctx.lr) - 4;
  c.width = ctx.r3.u32;
  c.height = ctx.r4.u32;
  c.format_word = ctx.r5.u32;
  __imp__sub_82CA88E0(ctx, base);
  if (const uint32_t header = ctx.r3.u32) rdr_adapter::roles::Global().Note(header, c);
}
