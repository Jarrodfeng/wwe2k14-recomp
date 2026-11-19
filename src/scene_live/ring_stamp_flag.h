// Bit 6 (ring_rows) stamp switch (Session 103; lanes C3-C7, docs\renderer\STAGE_LIVE_WRITERS.md). Header only, includes nothing (inputs_core.h reads it for `moved`).
// RELEASED by the main session 2026-11-16: run 27 (wwe2k14_7277) whole clean 10596 / unclean 0, skirts none_visible 0; ring_compare on its case (lane C7) differ 0 /
// no_ours 0 in every class (rope / skirt exact, turnbuckle <= tb_fit 1.4e-4, cover 1.5e-4 = the game's VMX sin/cos [L], post 3e-7). Open [U]: the skirt side numbering in
// the three camera quadrants run 27's match did not visit (the next mirror run's compare over an entrance settles it). Only mode 2 reads the bit, and it still needs every
// other required bit. To hold it again: set false; ring_recompute_contrib.h's Take stops stamping and kInputs' `moved` for bit 6 follows.
#pragma once
namespace rdr_scene::live::ringrecompute {
inline constexpr bool kStampRingRows = true;
}
