// wwe2k14 - Session 66 (track F2): the native function factory's functions (src/native_fn.h). Each has its own cvar
// wwe2k14_nf_<name> = off | verify | on (default off until its verify runs show 0 mismatches);
// --wwe2k14_nf_all=verify checks them all (and native skinning) in one run.
#pragma once

// Session 66: the helpers are x86-64 only for now (SSE intrinsics; verified on PC only); skinning is portable.
#if defined(__x86_64__) || defined(_M_X64)
#include "nf/atan2_d.h"            // 0x82C7A898 double atan2
#include "nf/frustum_cull.h"       // 0x8250E110 per-object view-volume classification (inferred)
#include "nf/memcpy_fwd.h"         // 0x82C79000 forward memcpy (prefetch hints)
#include "nf/memcpy_vmx.h"         // 0x82261340 memcpy with VMX blocks and dcbzl
#include "nf/sincos_vmx.h"         // 0x825D0618 float sine and cosine (VMX polynomial)
#include "nf/vec4_mat4_inplace.h"  // 0x82C32A88 vector x matrix, in place
#include "nf/vec4_mat4_row.h"      // 0x82C32BC0 vector x matrix into an output
#include "nf/component_find_by_tag.h"  // 0x82C31CB0 component lookup by tag (two chains)
// (nf/render_cmd_records.h, render command list appends 0x82C1A998 / 0x82C28798: not included - lists another thread
// consumes; verify mode's write-then-put-back is visible to it (run 4217 stalled in a load with them in verify).)
#include "nf/d3d_bind_fetch_slot.h"          // 0x82CA8B40 D3D: bind a texture / vertex fetch slot (render thread)
#include "nf/d3d_flush_fetch_constants.h"    // 0x82CBD668 D3D: dirty fetch constants -> packets
// (nf/d3d_flush_float_constants.h, 0x82CBD7C0: not included - src\half2_hooks.h already hooks it; chain it there first)
#include "nf/d3d_flush_register_runs.h"      // 0x82CBD428 D3D: dirty register runs -> packets
#include "nf/d3d_set_vs_float_constants.h"   // 0x82CB0658 D3D: SetVertexShaderConstantF
// (nf/gpu_wait_poll_step.h, 0x82CB1F50: written, not included - a GPU wait spin step, no frame time to gain)
#endif
#include "nf/skin_3bone.h"         // 0x82508958 the skinning loop with up to three bones
