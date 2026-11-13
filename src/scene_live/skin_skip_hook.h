// A seam for the crowd's CPU skin loops (Session 101, lane 50): sub_825085F8 / 82508738 / 82508958 / 82508C08 already have hooks of their own
// (src\native_skin.h and src\nf\skin_3bone.h through native_fn.h Dispatch; src\crowd_skin_report.h), and a function takes one hook. Those files
// ask SkinLoopSkipped first; scene_live\crowd_skip.h installs the decision. The switch is tested here, before the indirect call, so an off switch
// costs one cvar read. Null (nothing installed) = the loop runs as before.
#pragma once

#include <rex/cvar.h>
#include <rex/ppc/context.h>

#include <cstdint>

REXCVAR_DEFINE_BOOL(wwe2k14_rdr_crowd_skip_skin, false, "wwe2k14",
                    "Session 101 lane 50 (P2c of the crowd's promotion): with --wwe2k14_rdr_crowd_ours drawing the crowd, the game's CPU skin loops of the crowd manager do not run "
                    "for a group nothing else reads (the palette copy ours reads stays). Only after 3 frames in a row in which ours drew with no stream draw kept. Off by default");

namespace wwe2k14::crowd_skip {
// r3 = the manager, r6 = the destination buffer of the call. True = do not run the loop (and report no writes).
inline bool (*g_skip_skin_loop)(PPCContext&, uint8_t*) = nullptr;
inline bool SkinLoopSkipped(PPCContext& ctx, uint8_t* base) {
  return REXCVAR_GET(wwe2k14_rdr_crowd_skip_skin) && g_skip_skin_loop && g_skip_skin_loop(ctx, base);
}
}  // namespace wwe2k14::crowd_skip
