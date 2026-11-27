#include "../thread_name.h"  // S102 H1
#include "renderer.h"
#include "readback_tracker.h"
#include "stable_table.h"  // S102 R21-B: programs / layouts never move under Render
#include "pipeline_seed.h"  // S106 PSEED
#include "stage_plants.h"  // S102 R21-B: test plants (rplay only)
#include "present_clear.h"  // S102 G1
#include "../renderer_adapter/census_flush.h"  // S102 CEN1: a flip phase end prints every lever's census now
namespace {
static census_flush::Site g_cf_half, g_cf_fetch, g_cf_lean, g_cf_barrier, g_cf_overdraw, g_cf_frag, g_cf_outside;  // S102 CEN1: one per census line below
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include "colour_cuts.h"  // S102 G10
#include "arena_fragments.h"  // S102 R18-B3
#include "half_trans.h"  // S102 C4
#include "shaders/spv/rdr_half_depth_frag.h"  // S102 C4
#include "shaders/spv/rdr_half_composite_frag.h"  // S102 C4
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "bc_decode.h"  // S102 U1
#include "device.h"
#include "dyn_state_vk.h"  // S111 DYNSTATE: draw state set per draw where the device can (the pipeline key holds the rest)
#include "fence_wait.h"  // S102 H2
#include "format_caps.h"
#include "memory.h"
#include "pipeline_store.h"
#include "../perf_file_write.h"  // lane 302: NoteFileWrite
#include "problem.h"
#include "shaders/spv/rdr_present_frag.h"
#include "shaders/spv/rdr_present_sgsr_frag.h"
#include "shaders/spv/rdr_present_fetch_frag.h"  // S102 R24-A
#include "output_switches.h"  // S102 R24-A: the present fetch census
#include "shaders/spv/rdr_overlay_frag.h"
#include "shaders/spv/rdr_overlay_vert.h"
#include "shaders/spv/rdr_dlss_motion_frag.h"
#include "shaders/spv/rdr_fullscreen_vert.h"
#include "shaders/spv/rdr_present_dlss_frag.h"
#include "streamline.h"
#include "shaders/spv/rdr_present_vert.h"
#include "swapchain.h"
#include "output_switches.h"  // S102 G2
#include "copy_batch.h"  // S102 G12: consecutive copies as one command
#include "depth_nostencil.h"  // S102 R24-C: stencil liveness + the stencil census
#include "layered_targets.h"  // S102 C1: one render pass into several layers (multiview)
#include "light_cubes.h"  // S102 L1b: the light cubes written by one compute dispatch
#include "group_stamps.h"  // S102 R24-F: GPU stamps at the change of draw group inside a colour / frame pass
#include "breadcrumbs.h"  // S102 D1: the last frames' steps and the GPU marker words, dumped on a lost device
#include "render_lean.h"  // S102 RT1: texture sets kept across frames (wwe2k14_rdr_render_lean bit 1)
#include "shaders/spv/rdr_light_cubes_comp.h"  // S102 L1b
#include "shaders/spv/rdr_light_dirs_comp.h"  // S102 L1b review F7: the direction table
#include "shaders/spv/rdr_light_dirs_comp_swap.h"  // S102 L1b (the planted twin)
namespace rdr { constexpr uint8_t kBlitTag = 0xFE; }  // S102 G2: a stamp class of its own for the cap's blit (summed into output AND reported apart)
#include "vk_api.h"

namespace rdr {
namespace {

using namespace vk;

constexpr uint32_t kChunkBits = 8;
constexpr uint32_t kChunkSize = 1u << kChunkBits;
constexpr uint32_t kMaxChunks = 256;
constexpr uint16_t kNoTableId = 0xFFFF;  // S102 R21-B (review F5): what AddProgram / AddVertexLayout return when their table is full (never a real id: the tables hold 0..0xFFFE)
constexpr VkDeviceSize kRingSection = 24ull << 20;
constexpr uint32_t kConstantRange = 8192;
constexpr VkDeviceSize kOverlayRange = 1ull << 20;  // (S10b: the DLSS motion pass's constants take the kConstantRange before it)
constexpr VkDeviceSize kDlssConstants = kConstantRange;  // settings parity S11: the overlay's vertices and indices, before the constants
constexpr uint32_t kMaxSetsPerFrame = 2048;
constexpr int kAttachmentSlots = kMaxColorTargets + 1;
constexpr uint64_t kDestroyDelayFrames = 12;  // frames still waiting in the pump can name an image the game has already released (TEXTURES_FRAME L10)
constexpr uint64_t kMaxStagedBytes = 256ull << 20;

#include "renderer_parts/types.inc"  // split: types

}  // namespace

#include "renderer_parts/impl_state.inc"  // split: impl_state
#include "renderer_parts/setup_api.inc"  // split: setup_api
#include "renderer_parts/lifetime_buffers.inc"  // split: lifetime_buffers
#include "renderer_parts/uploads.inc"  // split: uploads
#include "renderer_parts/passes_pipelines.inc"  // split: passes_pipelines
#include "renderer_parts/pipeline_store.inc"  // split: pipeline_store
#include "renderer_parts/texture_sets_barriers.inc"  // split: texture_sets_barriers
#include "renderer_parts/compile.inc"  // split: compile
#include "renderer_parts/record_pass.inc"  // split: record_pass
#include "renderer_parts/half_cubes_copy.inc"  // split: half_cubes_copy
#include "renderer_parts/present_dlss_overlay.inc"  // split: present_dlss_overlay
#include "renderer_parts/render.inc"  // split: render
#include "renderer_parts/gpu_time.inc"  // split: gpu_time
#include "renderer_parts/readback.inc"  // split: readback
}  // namespace rdr
