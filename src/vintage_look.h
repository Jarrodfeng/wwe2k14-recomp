// wwe2k14 - S103 VTG2: ONE switch for the retro arenas' vintage look. The sepia is the post pass (already follows PlayerQuality::film_grain in both builds); the darkening, grain and
// vignette are the stage component VTGE's draw method sub_82594808 (vtable 0x8201E20C slot 1, r3 = component + 48; docs\research\s103\VTG.md "VTG2"). With the setting off the method is
// skipped. The flag is read every call, but PlayerQuality::film_grain is written only at start (ApplyStartSettings, before the guest runs; and only
// with rdr_owner, so a stream build with the old renderer keeps the look): a live toggle would need a writer of g_quality.film_grain (Opus review). Do NOT clear the stage record's bytes +41 / +42 / +43 instead:
// sub_82A19A18 (cutscene_program_build) also reads +42 / +43.
#pragma once

#include <rex/hook.h>
#include <rex/ppc/context.h>

#include "start_settings.h"
#include "vintage_look_core.h"

REX_EXTERN(__imp__sub_82594808);

REX_HOOK_RAW(sub_82594808) {
  wwe2k14::vintage::Run(ctx, base, wwe2k14::start::Quality().film_grain, __imp__sub_82594808);
}
