// S102 RT1: the cvar of wwe2k14_rdr_render_lean (docs\renderer\RENDER_LEAN.md), in its own header so reader.h stays untouched (other lanes edit it). Included once, by live.cpp.
#pragma once
#include <rex/cvar.h>

REXCVAR_DECLARE(int32_t, wwe2k14_rdr_render_lean);
