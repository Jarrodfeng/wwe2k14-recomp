// S102 RT1: the cvar of wwe2k14_rdr_render_lean (docs\renderer\RENDER_LEAN.md), in its own header so reader.h stays untouched (other lanes edit it). Included once, by live.cpp.
#pragma once
#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(wwe2k14_rdr_render_lean, 0, "wwe2k14",
                     "S102 RT1 (docs\\renderer\\RENDER_LEAN.md): per-frame work on our render thread whose answer does not change, as a bit mask: 1 = the texture descriptor sets are kept across frames "
                     "(keyed by the views and samplers they name; freed when an image they name is destroyed), 2 = the scene record's arena is found once per record on the hooks side, "
                     "4 = hand-off pieces (a staging buffer pool for UpdateImage, the UI hook's same-record skip, the plane shadow sort only where needed, the lasting-prune Note without per-frame hash nodes). "
                     "0 = today's code. Read every frame");
