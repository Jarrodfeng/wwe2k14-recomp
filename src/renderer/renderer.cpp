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

uint64_t HashBytes(const void* data, size_t bytes, uint64_t seed = 0xcbf29ce484222325ull) {
  const auto* p = static_cast<const uint8_t*>(data);
  uint64_t h = seed;
  for (size_t i = 0; i < bytes; ++i) {
    h ^= p[i];
    h *= 0x100000001b3ull;
  }
  return h;
}

template <class T>
VkDeviceSize AlignUp(T value, VkDeviceSize align) {
  return (VkDeviceSize(value) + align - 1) & ~(align - 1);
}

VkFormat ToVk(Format format, const Caps& caps) {
  switch (format) {
    case Format::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::RGBA8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case Format::BGRA8: return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::R8: return VK_FORMAT_R8_UNORM;
    case Format::R8_SNORM: return VK_FORMAT_R8_SNORM;
    case Format::RG8: return VK_FORMAT_R8G8_UNORM;
    case Format::RGBA4: return VK_FORMAT_B4G4R4A4_UNORM_PACK16;
    case Format::R5G6B5: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case Format::A1RGB5: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    case Format::RGB10A2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case Format::RGBA16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case Format::RGBA16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case Format::RG16_UNORM: return VK_FORMAT_R16G16_UNORM;
    case Format::RG16_SNORM: return VK_FORMAT_R16G16_SNORM;
    case Format::R16_UNORM: return VK_FORMAT_R16_UNORM;
    case Format::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::RG16F: return VK_FORMAT_R16G16_SFLOAT;
    case Format::R16F: return VK_FORMAT_R16_SFLOAT;
    case Format::RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case Format::RG32F: return VK_FORMAT_R32G32_SFLOAT;
    case Format::R32F: return VK_FORMAT_R32_SFLOAT;
    case Format::R32_UINT: return VK_FORMAT_R32_UINT;
    case Format::Depth: return caps.depth_format;
    case Format::BC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case Format::BC2: return VK_FORMAT_BC2_UNORM_BLOCK;
    case Format::BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
    case Format::BC4: return VK_FORMAT_BC4_UNORM_BLOCK;
    case Format::BC5: return VK_FORMAT_BC5_UNORM_BLOCK;
    case Format::ASTC_4x4: return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    case Format::EAC_R11: return VK_FORMAT_EAC_R11_UNORM_BLOCK;
    case Format::EAC_RG11: return VK_FORMAT_EAC_R11G11_UNORM_BLOCK;
    default: return VK_FORMAT_UNDEFINED;
  }
}

// Bytes per texel for formats a raw copy can move between. 0: not copyable that way.
uint32_t BlockBytes(Format format) {
  switch (format) {
    case Format::R8:
    case Format::R8_SNORM: return 1;
    case Format::RG8:
    case Format::RGBA4:
    case Format::R5G6B5:
    case Format::A1RGB5:
    case Format::R16_UNORM:
    case Format::R16F: return 2;
    case Format::RGBA8:
    case Format::RGBA8_SNORM:
    case Format::BGRA8:
    case Format::RGB10A2:
    case Format::RG16_UNORM:
    case Format::RG16_SNORM:
    case Format::RG16F:
    case Format::R32F:
    case Format::R32_UINT: return 4;
    case Format::RGBA16_SNORM:
    case Format::RGBA16_UNORM:
    case Format::RGBA16F:
    case Format::RG32F: return 8;
    case Format::RGBA32F: return 16;
    default: return 0;
  }
}

// S101 lane 200: the names the format-capability log lines use (format_caps.h).
const char* CapFormatName(VkFormat f) {
  switch (f) {
    case VK_FORMAT_R32_SFLOAT: return "R32_SFLOAT";
    case VK_FORMAT_R32G32_SFLOAT: return "R32G32_SFLOAT";
    case VK_FORMAT_R32G32B32A32_SFLOAT: return "R32G32B32A32_SFLOAT";
    case VK_FORMAT_R16_SFLOAT: return "R16_SFLOAT";
    case VK_FORMAT_R16G16_SFLOAT: return "R16G16_SFLOAT";
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
    case VK_FORMAT_R16_UNORM: return "R16_UNORM";
    case VK_FORMAT_R16G16_UNORM: return "R16G16_UNORM";
    case VK_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
    case VK_FORMAT_R16G16_SNORM: return "R16G16_SNORM";
    case VK_FORMAT_R16G16B16A16_SNORM: return "R16G16B16A16_SNORM";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM";
    case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    default: return "format";
  }
}

// What the fallbacks did (logged once at shutdown). With kCapFallbacksOff the "illegal" counters count what the driver was asked to do
// anyway: a linear sample / linear blit of a format that cannot filter, a blend on an attachment that cannot blend.
struct CapStats {
  uint64_t linear_to_nearest = 0;   // texture slots bound with a nearest sampler instead of the linear one the game asked for
  uint64_t blits_to_nearest = 0;    // linear blits made nearest
  uint64_t blend_disabled = 0;      // pipeline attachments built with blending off
  uint64_t illegal_linear = 0;      // fallbacks off: linear sampling / blitting of a format that cannot filter
  uint64_t illegal_blend = 0;       // fallbacks off: blending on an attachment that cannot blend
  uint64_t vertex_unsupported = 0;  // vertex attribute formats the device cannot read as vertex input
};

struct SubresourceState {
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
  VkAccessFlags access = 0;
};

struct ImageEntry {
  VkImage image = VK_NULL_HANDLE;
  Allocation memory;
  VkImageView sampled_view = VK_NULL_HANDLE;
  std::vector<std::pair<uint64_t, VkImageView>> reinterpreted_views;  // other formats and channel orders, by format << 32 | swizzle
  std::vector<std::pair<uint32_t, VkImageView>> target_views;  // by mip << 8 | layer
  std::vector<SubresourceState> states;                        // mips * layers
  ImageDesc desc;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  uint32_t width = 0, height = 0, layers = 1;
  VkImageUsageFlags vk_usage = 0;  // S10b: what it was made with (Streamline's resource description)
  bool depth = false;
  Format decode_from = Format::Undefined;  // S102 U1: a BC image the device cannot sample, made in desc.format (RGBA8 / R8 / RG8) and decoded by UpdateImage
  bool live = false;
  bool copy_dest = false;      // written by copies, so sampling it needs a layout check
  uint8_t gpu_class = 0;       // S102 S0-3: api.h GpuClass, set by Renderer::SetImageClass (the per-role GPU line)
  bool carries_state = false;  // some frame has read it before writing it
  bool ever_read = false;      // some frame has sampled it, loaded it or copied from it (S100: a copy into an image never read yet is kept)
};

struct BufferEntry {
  VkBuffer buffer = VK_NULL_HANDLE;
  Allocation memory;
  size_t bytes = 0;
  bool live = false;
  bool host = false;        // S103 PF2: made by CreateHostBuffer (the host writes it in place)
  uint64_t last_read = 0;   // S103 PF2: (host buffers) the newest Render serial whose draws name this buffer, + 1; 0 = never (atomic_ref: Render writes, the front end reads)
};

// Ids are stable, entries never move, and readers need no lock.
template <class T>
class Table {
 public:
  ~Table() {
    for (T* chunk : chunks_) delete[] chunk;
  }
  uint32_t Add() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!free_.empty()) {
      const uint32_t id = free_.back();
      free_.pop_back();
      return id;
    }
    const uint32_t id = next_++;
    if (id >= kChunkSize * kMaxChunks) return 0;
    T*& chunk = chunks_[id >> kChunkBits];
    if (!chunk) chunk = new T[kChunkSize];
    return id;
  }
  void Remove(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    free_.push_back(id);
  }
  T& operator[](uint32_t id) { return chunks_[id >> kChunkBits][id & (kChunkSize - 1)]; }
  uint32_t high_water() const { return next_.load(std::memory_order_acquire); }

 private:
  T* chunks_[kMaxChunks] = {};
  std::vector<uint32_t> free_;
  std::atomic<uint32_t> next_{1};  // 0 means none
  std::mutex mutex_;
};

struct ImageUpload {
  ImageId image;
  uint32_t mip, layer;
  VkBuffer staging;
  Allocation memory;
  VkDeviceSize bytes = 0;  // S102 M1: the bytes asked for (not the driver's allocation size, which the counts must not follow)
  bool injected = false;   // S102 G5 plants 2 / 3 only: a copy of the first upload added to the frame (it owns no staging: never retired)
  uint8_t pool_class = 0;  // S102 RT1 (render_lean bit 4): the size class of a staging buffer that goes back to the pool once its slot is drained (0 = made for this upload alone)
};

struct BufferUpload {
  VkBuffer staging;
  Allocation memory;
  VkBuffer dest;
  VkDeviceSize offset, bytes;
  uint8_t producer = 0;   // S103 PVR: who asked (rdr::BufferProducer), for the [rdr][gpu][uploads] line's per-producer copy count
};

struct Retired {
  VkImage image = VK_NULL_HANDLE;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  Allocation memory;
  uint32_t free_image = 0, free_buffer = 0;  // ids that become reusable once this slot's GPU work is done
  VkDescriptorSet lean_set = VK_NULL_HANDLE;  // S102 RT1: a kept texture set (render_lean.h) whose image went; freed from the kept pool with the slot's other retired items
  uint64_t staged_bytes = 0;
  uint8_t pool_class = 0;  // S102 RT1: the buffer + memory above go back to the staging pool instead of being destroyed
};

struct PendingDestroy {
  uint32_t image = 0, buffer = 0;
  uint64_t ready_serial = 0;
};

struct FrameSlot {
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkDescriptorPool descriptors = VK_NULL_HANDLE;
  VkSemaphore available = VK_NULL_HANDLE;
  VkQueryPool timestamps = VK_NULL_HANDLE;  // S101 lane22: 2 queries, only when RendererOptions::gpu_timestamps
  bool ts_pending = false;                  // lane 269: this slot's last submit wrote both timestamps and they are not read yet (read after the slot's fence wait, no stall)
  uint64_t ts_submit_ns = 0;                // lane 269: the wall clock at that submit (a frame's GPU time cannot exceed the time since it was submitted)
  // S102 S0-3: the stamps of this slot's last frame. detail: [0] top of the buffer, [1] after the acquire wait (colour-output stage), then one stamp after each step, the
  // LAST after the read-backs (the old bracket's end). Not detail: [0], [1] = end (lane 269's two). ts_class[k] names the stamp k+2's step (detail only).
  uint32_t ts_cap = 0;        // queries in the pool
  uint32_t ts_count = 0;      // queries written this frame
  uint32_t ts_need = 0;       // queries this frame wanted (the pool grows before the slot's next frame when it was short)
  int arm_tag = 0;  // S102 G10: rdr::g_arm_tag when this frame was recorded
  bool sd_arm = false;  // S102 R24-C review F4: stencil_dead when this frame was recorded (the per-step table's arm)
  bool ts_detail = false;     // this frame wrote per-step stamps
  bool ts_has_pass = false;   // at least one pass step was recorded (else no sample)
  bool ts_presented = false;  // a present step was recorded
  std::vector<uint8_t> ts_class;
  std::vector<uint32_t> ts_image;  // S102 G6 (census only): the first target (pass) / destination (copy) image of the step each stamp closes; 0 = the tail / the present
  std::vector<uint8_t> ts_kind;    // S102 G6 (census only): 0 pass, 1 copy, 2 present, 3 tail, 4 a test plant's stamp
  std::vector<uint8_t> ts_src;     // S102 G6 (census only): a copy's SOURCE image's GPU class (255 = not a copy)
  // S102 R24-F (wwe2k14_rdr_gpu_group_stamps): the group stamps inside the colour / frame passes. Their own pool (so the main pool's 1:1 step stamps stay as they are); a pass is a chain of segments
  // from the previous step's stamp to its own closing stamp (group_stamps.h).
  struct GsPass {
    uint32_t begin_stamp = 0, end_stamp = 0;   // main-pool stamps: the previous step's, and this pass's own closing stamp
    uint32_t first_seg = 0, segs = 0, issued = 0;   // its segments in gs_segs; the draws it issued in the main loop (counted apart from the segments)
  };
  VkQueryPool gs_pool = VK_NULL_HANDLE;
  uint32_t gs_count = 0;      // group stamps written this frame
  bool gs_frame = false;      // this frame cuts its colour / frame passes into group segments
  bool gs_overflow = false;   // the pool ran out this frame (the rest of that pass is one mixed segment)
  std::vector<rdr::GroupSeg> gs_segs;
  std::vector<GsPass> gs_passes;
  VkQueryPool occ = VK_NULL_HANDLE;  // S102 G11 (frame trace only): one precise occlusion query per frame-class draw
  uint32_t occ_count = 0;
  std::vector<uint32_t> occ_meta;    // pass, op of each query (pairs)
  uint64_t occ_serial = 0;
  bool occ_pending = false;
  std::vector<uint32_t> ts_pass;   // S102 G11 (frame trace only): a pass step's number in the frame's plan (0xFFFFFFFF = not a pass)
  uint64_t ts_serial = 0;          // S102 G11 (frame trace only): the frame's serial
  bool ts_trace = false;           // S102 G11 (F9): this frame's per-pass numbers are kept (the trace file was open when it began)
  uint32_t ts_up_mid = 0;                   // S103 PVR: the stamp between ApplyUploads' image part and its buffer part (0 = none; written only in the stamp-per-step mode and only when both parts exist): ts_up - 1 is then this stamp
  uint32_t ts_up = 0;                       // S102 G5: the stamp after ApplyUploads' commands (0 = none this frame; needs the outside meter and the stamps)
  uint32_t ts_mark = 0;           // S103 GT round 3: rdr::g_gpu_mark_seq when this frame was recorded (the route window it belongs to)
  uint64_t ts_up_bytes = 0;       // S103 GT round 3: the bytes this frame uploaded (for the route window's steady / loading split)
  uint32_t ts_w_pre = 0, ts_w_closes = 0, ts_w_post = 0;   // S103 GT review: stamps written by kind (the identity ts_count == 2 + pre + closes + post + tail is checked at the end of the frame)
  uint32_t ts_steps_rec = 0, ts_steps_closed = 0;   // S103 GT: steps the frame recorded / steps a stamp of their own closed (stamp-per-step mode only; equal = every pass is bracketed)
  bool ts_census = false;                           // S103 GT: this frame ran in the stamp-per-step mode (the two counts above mean something)
  uint64_t out_wall_ns = 0;                 // S102 G5: the meter's wall time from the submit call to the fence's signal for this slot's last frame (0 = not measured)
  VkCommandBuffer plant_cmd = VK_NULL_HANDLE;  // S102 G5 plant 6: this slot's extra buffer (from its pool, reset with it)
  std::vector<Retired> retired;
};

struct RenderPassKey {
  uint16_t formats[kAttachmentSlots];
  uint8_t load[kAttachmentSlots];
  uint8_t store[kAttachmentSlots];
  uint8_t stencil_load, stencil_store;
  uint8_t color_count, has_depth, samples, pad;
  uint8_t views, pad2;   // S102 C1: > 1 = a multiview pass over that many layers (layered_targets.h)
};

struct PassClassKey {
  uint16_t formats[kAttachmentSlots];
  uint8_t color_count, has_depth, samples, pad;
  uint8_t views, pad2;   // S102 C1: the view count of a multiview class (a pipeline built for it carries the view mask through the render pass)
};

struct FramebufferKey {
  VkRenderPass pass;
  VkImageView views[kAttachmentSlots];
  uint32_t width, height;
};

struct PipelineKey {
  DrawState state{};
  uint32_t pass_class;
};

struct BytesHash {
  template <class T>
  size_t operator()(const T& key) const {
    return size_t(HashBytes(&key, sizeof(T)));
  }
};
struct BytesEqual {
  template <class T>
  bool operator()(const T& a, const T& b) const {
    return std::memcmp(&a, &b, sizeof(T)) == 0;
  }
};

struct Program {
  VkShaderModule vertex = VK_NULL_HANDLE;
  VkShaderModule fragment = VK_NULL_HANDLE;
  const char* name = nullptr;
  uint8_t prepass_class = 0;  // S102 G10: colour_cuts.h ShaderClass of the fragment shader (0 unsafe, 1 plain, 2 only the alpha test discards)
  uint8_t half_ok = 0;        // S102 C4: the fragment shader reads no BuiltIn FragCoord (half_trans.h ReadsFragCoord): its picture does not depend on the target's pixel grid
};

// What the compile step decides for one pass.
struct PassPlan {
  const TargetsOp* targets = nullptr;
  uint32_t first_op = 0, end_op = 0;
  uint32_t first_sampled = 0, sampled_count = 0;
  uint8_t load[kAttachmentSlots] = {};
  uint8_t store[kAttachmentSlots] = {};
  VkClearValue clear[kAttachmentSlots] = {};
  uint32_t width = 0, height = 0;
  uint8_t stencil_load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  uint8_t stencil_store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  bool stencil_used = false;
  bool stencil_tests = false;        // S102 R24-C: a draw of the pass has the stencil test on
  bool stencil_clear_in = false;     // S102 R24-C: a stencil clear inside the pass (not absorbed into the load op)
  bool strip_stencil_clear = false;  // S102 R24-C: stencil_dead: those clears are not issued (poison: issued with 0xA5)
  bool sd_poisoned_load = false;     // S102 R24-C: the poison arm turned a DONT_CARE stencil load into CLEAR 0xA5; the oracle still reads it as undefined
  bool stencil_after = false;  // S102 R24-C: a later pass or copy of this frame reads this depth image's stencil (decide fills it)
  bool depth_used = false;     // a draw tests or writes depth or stencil, or a clear inside the pass touches them
  bool depth_cleared = false;  // a full depth / stencil clear became the load op
};

enum class StepKind : uint8_t { Pass, Copy, Present, Dispatch /* S102 L1b */ };
struct Step {
  StepKind kind;
  uint32_t index;
  bool persistent;  // must run once per game frame even if the frame is never shown
};

struct SampledImage {
  ImageId image;
  bool vertex;
};

enum NextUse : uint8_t { kNextNone, kNextRead, kNextOverwrite };

}  // namespace

struct Renderer::Impl {
  RendererOptions options;
  Device device;
  Allocator allocator;
  Table<ImageEntry> images;
  Table<BufferEntry> buffers;

  std::vector<FrameSlot> slots;
  bool gpu_timestamps = false;    // S101 lane22: on, and the queue family has valid timestamp bits
  uint32_t last_slot = ~0u;       // S101 lane22: the slot of the last frame recorded
  FrameCounts counts_cur;         // S102 M1: this frame's counts (options.counts); counts_plant >= 0: the planted extra draw's pass
  int counts_plant = -1;
  void CountBarrier(uint32_t images) { if (options.counts) ++counts_cur.total.barriers, counts_cur.total.barrier_images += images; }  // a vkCmdPipelineBarrier outside FlushBarriers
  void CountCopy() { if (options.counts) ++counts_cur.total.copies; }  // one vkCmdCopy* / vkCmdBlit* actually recorded
  CallCounts counts_sum;          // the game's 600-frame log
  uint64_t counts_sum_extra = 0, counts_sum_passes = 0, counts_sum_frames = 0;
  // Lane 269: the per-frame GPU time without a stall. Each slot's two timestamps are read right after the slot's fence wait (which Render does anyway),
  // frames_in_flight frames after they were written, with no WAIT flag. Results land in rdr::g_gpu_* (overlay.h) and a 600-frame log line.
  uint32_t ts_valid_bits = 0;
  double ts_period = 1.0;         // ns a tick (limits.timestampPeriod)
  std::vector<float> gpu_samples; // ms, this log window
  uint64_t gpu_bad = 0, gpu_not_ready = 0, gpu_bad_total = 0, gpu_not_ready_total = 0;  // window / whole run
  void CollectGpuTime(FrameSlot& slot);
  void LogGpuWindow(const char* why);
  // S102 S0-3: per-role sums, a window (the 600-frame line) and the whole run.
  struct RoleSums {
    double cls_ms[kGpuClassCount] = {};
    double gap_ms = 0, work_ms = 0, total_ms = 0;  // gap = the start gap [0] -> [1] (NOT the display wait: see docs\renderer\GPU_STAMPS.md)
    double blit_ms = 0;                            // S102 G2: the cap's blit (inside the 'output' sum)
    uint64_t frames = 0, no_pass = 0, no_present = 0, overflow = 0, stamps = 0;
    uint64_t cls_passes[kGpuClassCount] = {};      // pass steps recorded by class (the role mapping's own count)
    uint64_t steps_rec = 0, steps_closed = 0, census_frames = 0;   // S103 GT: the bracket check (stamp-per-step frames only)
    uint64_t ident_frames = 0, ident_bad = 0;      // S103 GT review: stamped frames / frames whose stamp count broke ts_count == 2 + pre + closes + post + tail (independent of the GPU)
  };
  RoleSums role_window, role_run;
  // S103 GT (the stream-free build's per-frame upload view): a frame that uploaded more than kLoadingBytes is a LOADING frame (a burst: new pictures / geometry arriving), the rest are STEADY.
  // The GPU span of ApplyUploads' commands (the stamp pair around it, needs the stamps on) is kept with them. Window = since the last [rdr][gpu] line, run = the whole run.
  static constexpr uint64_t kLoadingBytes = 1ull << 20;
  struct UpSums {
    uint64_t steady_frames = 0, steady_bytes = 0, load_frames = 0, load_bytes = 0, max_bytes = 0;
    uint64_t image_uploads = 0, buffer_uploads = 0, barriers = 0, up_frames = 0, batched_frames = 0, max_barriers = 0, max_images = 0;   // S103 UPL: the census of WHAT the bytes and the fixed cost are (image copies, buffer copies, barriers recorded by ApplyUploads, frames that uploaded anything)
    double gpu_ns = 0;
    uint64_t gpu_frames = 0;
    uint64_t prod_copies[3] = {0, 0, 0};   // S103 PVR: buffer copies by producer (other / posed model vertices / cloth + CPU-skinned), over the frames that uploaded
    double mid_img_ns = 0, mid_buf_ns = 0;   // S103 PVR: the span split at the mid stamp (image part, buffer part) over mid_frames (stamp-per-step mode only)
    uint64_t mid_frames = 0;
    uint64_t ring_peak = 0;   // S103 PVR: the biggest frame ring use (constants + geometry + previous constants, bytes) of the window
    uint64_t ring_ns = 0, ring_bytes = 0;   // S103 PF2: the host's memcpy of the frame's constants + geometry + previous constants into the mapped ring (out_ring_ns, ring_used_cur), summed over the window's frames
  };
  UpSums up_window, up_run;
  // S103 GT round 3: the route window. One per mark (rdr::GpuWindowMark): the frames RECORDED between two marks, summed as they are collected, logged as `[rdr][gpu][window] NAME:` when the first frame of the next window arrives.
  struct MarkWin {
    uint32_t seq = 0;
    double cls_ns[kGpuClassCount] = {};
    double gap_ns = 0, work_ns = 0, bracket_ns = 0, up_gpu_ns = 0;   // bracket = what the csv gpu_ms sums: [0] .. tail of every frame
    uint64_t frames = 0, steady = 0, loading = 0, steady_bytes = 0, loading_bytes = 0, up_frames = 0;
  };
  MarkWin mark_win;
  void FlushMarkWindow(const char* suffix);
  struct CensusRow { double ms = 0; uint64_t steps = 0; };  // S102 G6: the census's rows, keyed (class << 40 | kind << 32 | image), the whole run
  std::map<uint64_t, CensusRow> census_run;
  CensusRow census_copy_src[kGpuClassCount];  // S102 G6: copies (class copies) by the GPU class of the copy's source image
  uint64_t census_frames = 0;
  std::map<uint64_t, std::vector<float>> pass_table;  // S102 R24-B: --wwe2k14_rdr_gpu_pass_stamps: per (class, kind, occurrence of that image in the frame, image) the step's GPU us samples this window
  uint64_t pass_table_frames = 0;
  // S102 R24-F: the group table. Key: the pass step (class, kind, occurrence on its image, image: as pass_table) + group << 56 + the stencil_dead arm in bit 63.
  struct GroupAcc { std::vector<float> us; uint64_t draws = 0; };
  struct GroupPassAcc { double us = 0, direct_us = 0; uint64_t frames = 0, draws = 0, issued = 0, mixed = 0, segs = 0, stamps = 0; };
  std::map<uint64_t, GroupAcc> group_table;
  std::map<uint64_t, GroupPassAcc> group_pass;   // the same key without the group
  uint64_t group_frames = 0, group_not_ready = 0, group_chain_broken = 0, group_cover_bad = 0, group_overflow_frames = 0, group_stamps_total = 0, group_sum_bad = 0;   // S103 GSTP: group_sum_bad = frames whose groups did not add up to the pass's own span
  void CollectGroups(FrameSlot& slot, const uint64_t* ticks, uint32_t count, uint64_t mask, double frame_ns);
  uint64_t pass_table_frames_arm[2] = {0, 0};   // S102 R24-C review F4: frames per arm (stencil_dead off / on): the table key has the arm in bit 63, so an in-run flip is two columns, not an average
  uint32_t CensusImageW(uint32_t id) { return id && id < images.high_water() ? images[id].width : 0; }  // S102 G6
  uint32_t CensusImageH(uint32_t id) { return id && id < images.high_water() ? images[id].height : 0; }
  uint32_t CensusImageFmt(uint32_t id) { return id && id < images.high_water() ? uint32_t(images[id].desc.format) : 0; }
  std::vector<float> work_samples, gap_samples;   // ms, this window
  uint64_t win_acquire_ns = 0, win_present_ns = 0, win_renders = 0, run_acquire_ns = 0, run_present_ns = 0, run_renders = 0;  // per Render: the swapchain acquire and the Present call
  uint32_t stamp_scratch_cap = 0;                 // test plant 4: the scratch buffer's copies (see RecordStampPlant)
  VkBuffer stamp_scratch = VK_NULL_HANDLE;
  Allocation stamp_scratch_memory;
  uint64_t acquire_ns_frame = 0;                  // the swapchain acquire's time inside this Render (CPU wait, not render-thread work)
  void MakeStampPool(FrameSlot& slot, uint32_t cap);
  void RecordStampPlant(VkCommandBuffer cmd);
  // Lane 232 (3d a2): the no-drain read-back. Persistent host-visible staging per picture level; the copy is recorded into the frame's own command buffer (RecordReadbacks) and handed out when
  // that frame's fence has signalled (PollReadbacks, never waits). The bookkeeping is rdr::ReadbackTracker (host-tested).
  rdr::ReadbackTracker readback;
  std::vector<rdr::ReadbackTracker::Request> readback_taking;
  struct ReadbackStaging {
    VkBuffer buffer = VK_NULL_HANDLE;
    Allocation memory;
    VkDeviceSize bytes = 0;
    uint32_t width = 0, height = 0;
    Format format = Format::RGBA8;
  };
  std::unordered_map<uint64_t, ReadbackStaging> readback_staging;
  static uint64_t ReadbackKey(const rdr::ReadbackTracker::Key& k) { return (uint64_t(k.image) << 16) | (uint64_t(k.mip & 0xFF) << 8) | (k.layer & 0xFF); }
  std::vector<char> readback_recorded;  // parallel to readback_taking
  bool RecordOneReadback(VkCommandBuffer cmd, const rdr::ReadbackTracker::Request& req);
  void RecordReadbacksAfterCopy(VkCommandBuffer cmd, const CopyOp& c);
  void RecordReadbacks(VkCommandBuffer cmd);
  uint64_t serial = 0;

  VkBuffer ring = VK_NULL_HANDLE;
  Allocation ring_memory;

  VkDescriptorSetLayout constants_layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout textures_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkDescriptorPool constants_pool = VK_NULL_HANDLE;
  VkDescriptorSet constants_set = VK_NULL_HANDLE;
  VkPipelineCache pipeline_cache = VK_NULL_HANDLE;

  // ---- S102 lane L1b (light_cubes.h): the light cubes' compute pipeline, made on the first dispatch (lc_state 0 untried, 1 ready, 2 failed). One descriptor set per frame slot. Render thread only. ----
  int lc_state = 0;
  VkDescriptorSetLayout lc_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout lc_layout = VK_NULL_HANDLE;
  VkShaderModule lc_module = VK_NULL_HANDLE, lc_dirs_module = VK_NULL_HANDLE;
  VkPipeline lc_pipeline = VK_NULL_HANDLE, lc_dirs_pipeline = VK_NULL_HANDLE, lc_dirs_pipeline_swap = VK_NULL_HANDLE;
  ImageId lc_table = kNoImage;      // review F7: the direction table (64x64x6 RGBA32F), made again only when the texel -> grid mapping changes
  float lc_table_k[4] = {};
  bool lc_table_valid = false, lc_table_swap = false;
  uint64_t lc_table_builds = 0;
  VkDescriptorPool lc_pool = VK_NULL_HANDLE;
  std::vector<VkDescriptorSet> lc_sets;
  bool LightCubesInit();
  void RecordLightCubes(VkCommandBuffer cmd, const DispatchOp& op, uint32_t slot_index, VkDeviceSize ring_base);
  ImageId dummy_image = kNoImage;
  ImageId dummy_cube = kNoImage;
  ImageId dummy_volume = kNoImage;
  VkSampler dummy_sampler = VK_NULL_HANDLE;
  bool filterable[uint32_t(Format::Count)] = {};
  uint32_t pipeline_delay_ms = 0;  // RendererOptions::pipeline_delay_ms (test only)
  uint32_t bc_mode = 1;       // S102 U1: RendererOptions::bc_decode (0 off, 1 auto: only where the device cannot sample BC, 2 force)
  std::atomic<uint64_t> bc_decoded_levels{0}, bc_images{0}, bc_decoded_bytes{0};
  std::atomic<uint64_t> frames_dropped{0};  // S103 CB1: Renderer::FramesDropped
  std::atomic<size_t> render_thread{0};  // S102 U1: hash of the thread that calls Render (WaitForStagingRoom must never wait on it: only its own DrainRetired frees staging)
  uint64_t max_staged = kMaxStagedBytes;  // dev tests may shrink it (RendererOptions::staging_cap_mb)
  bool staging_wait_plant = false;        // dev only: the pre-fix behaviour (the render thread waits too), the planted fault of the stall test  // S102 U1: counters for the verdict / log
  bool caps_fallback = true;  // format_caps.h: false only under kCapFallbacksOff (the re-find)
  CapStats cap_stats;
  VkSampler present_sampler_nearest = VK_NULL_HANDLE;  // lane 200: the present of a source that cannot filter
  // Linear filtering of a renderer Format, from the table made once at Init (no per-call driver query).
  bool CanFilter(Format format) const { return filterable[uint32_t(format)]; }
  std::unordered_set<uint64_t> caps_logged;  // (kind, program, format) already said: each is logged once, however often pipelines are rebuilt
  std::chrono::steady_clock::time_point caps_summary_at{};
  bool caps_summary_dirty = false;
  void CapsSummary() {
    caps_summary_dirty = false;
    // render-clock: perf - log throttle stamp for the format caps summary line
    caps_summary_at = std::chrono::steady_clock::now();
    Log("[rdr] format caps summary: linear->nearest slots %llu, blits->nearest %llu, blend off %llu, vertex unsupported %llu; illegal (fallbacks off) linear %llu, blend %llu",
        static_cast<unsigned long long>(cap_stats.linear_to_nearest), static_cast<unsigned long long>(cap_stats.blits_to_nearest), static_cast<unsigned long long>(cap_stats.blend_disabled),
        static_cast<unsigned long long>(cap_stats.vertex_unsupported), static_cast<unsigned long long>(cap_stats.illegal_linear), static_cast<unsigned long long>(cap_stats.illegal_blend));
  }
  // Review SF1: a counter changed. The summary is written the first time and then at most every 10 s (a phone or a bug-button zip rarely reaches Shutdown).
  void CapsChanged() {
    caps_summary_dirty = true;
    // render-clock: perf - log throttle: the caps summary prints at most every 10 s
    if (caps_summary_at == std::chrono::steady_clock::time_point{} || std::chrono::steady_clock::now() - caps_summary_at >= std::chrono::seconds(10)) CapsSummary();
  }
  bool FirstTime(uint64_t kind, uint64_t a, uint64_t b) { return caps_logged.insert((kind << 56) ^ (a << 24) ^ b).second; }
  VkFilter BlitFilterFor(Format source, bool want_linear) {
    if (!want_linear) return VK_FILTER_NEAREST;
    if (CanFilter(source)) return VK_FILTER_LINEAR;
    if (!caps_fallback) {
      ++cap_stats.illegal_linear;
      CapsChanged();
      return VK_FILTER_LINEAR;
    }
    ++cap_stats.blits_to_nearest;
    if (FirstTime(1, uint64_t(source), 0)) Log("[rdr] format caps: a linear blit from format %u (%s) is made nearest", unsigned(source), CapFormatName(ToVk(source, device.caps())));
    CapsChanged();
    return VK_FILTER_NEAREST;
  }
  VkSampler PresentSamplerFor(const ImageEntry& source) const { return CanFilter(source.desc.format) || !caps_fallback ? present_sampler : present_sampler_nearest; }

  Swapchain swapchain;
  bool windowed = false;
  bool offscreen_out = false;
  uint32_t colour_cuts_ = 0;    // S102 G10: this frame's colour-pass cuts, from the Frame (record time)
  uint32_t arena_fragments_ = 0;  // S102 R18-B3: this frame's arena_fragments.h mask, from the Frame (record time)
  std::vector<std::pair<VkPipeline, uint8_t>> af_pipes;  // S102 R18-B3: per state index, valid for the pass being recorded: (blend-off pipeline, 0 unset / 1 taken / 2 cannot drop)
  std::vector<VkPipeline> cut_pre_pipes, cut_main_pipes;  // S102 G10: per state index, valid for the pass being recorded
  uint32_t out_cap_h_ = 0;      // S102 G2: this frame's output levers, from the Frame (record time); the render thread reads these, never the live switch
  bool out_bilinear_ = false;
  bool present_capped_ = false;  // S102 G2: this frame's present drew at the capped size (set in RecordPresent, read by the submit)  // S102 G2: no window, the present draws into offscreen images (rplay --present-size)
  ImageId display_curve = kNoImage;
  VkSampler present_sampler = VK_NULL_HANDLE;
  uint16_t present_program = 0;
  uint16_t present_sgsr_program = 0;  // settings parity S10a
  uint16_t present_fetch_program = 0;  // S102 R24-A: rdr_present_fetch (the 1:1 present: one texelFetch, no filter)
  bool present_fetch_ = false;        // S102 R24-A: this frame's wwe2k14_rdr_present_fetch, from the Frame (record time)
  // Settings parity S11: the app's overlay.
  uint16_t overlay_program = 0, overlay_layout = 0;
  std::unordered_map<uint32_t, ImageId> overlay_textures;
  std::shared_ptr<const OverlayList> overlay;
  void RecordOverlay(VkCommandBuffer cmd, FrameSlot& slot, VkDeviceSize ring_base, uint32_t present_class);
  // Settings parity S10b: DLSS.
  Renderer* self = nullptr;
  const DlssInputs* frame_dlss = nullptr;  // the frame being recorded's
  uint16_t dlss_motion_program = 0, present_dlss_program = 0;
  uint16_t half_depth_program = 0, half_composite_program = 0;  // S102 C4: the half-size transparents' two internal programs
  ImageId half_color = kNoImage, half_depth_img = kNoImage;      // S102 C4: the half-size target and depth (made again when the size or a format changes)
  uint32_t half_w = 0, half_h = 0;
  ImageId dlss_motion = kNoImage, dlss_out = kNoImage;
  uint32_t dlss_motion_w = 0, dlss_motion_h = 0, dlss_out_w = 0, dlss_out_h = 0;
  uint64_t dlss_count = 0;
  uint64_t last_desired_ns = 0;  // S100: the last desired present time (display timing)
  VkDeviceSize prev_constants_base = 0;  // S10b: where this frame's prev_constants went in the ring
  bool dlss_had_history = false;
  bool RecordDlss(VkCommandBuffer cmd, FrameSlot& slot, VkDeviceSize ring_base, uint32_t out_w, uint32_t out_h);
  uint16_t present_layout = 0;

  StableTable<Program> programs;  // S102 R21-B (STAGE_SPLIT.md hazard 1): append-only, readers by id need no lock; appends under tables_mutex
  VkQueryPool ps_pool = VK_NULL_HANDLE;  // S102 G10: pipeline-statistics queries of the census (vertex + fragment invocations per run of same-program draws)
  uint32_t ps_cap = 0, ps_count = 0;
  std::vector<std::string> ps_names;
  std::vector<uint8_t> ps_afoff;  // S102 R18-B3: parallel to ps_meta: 1 = the draw ran with its blend switched off (the per-draw log's trailing "afoff" field)
  std::vector<std::string> ps_meta;  // S102 C4: the per-draw log's line start for each query (rplay --draw-log); empty otherwise
  std::vector<uint8_t> draw_mod_match;  // S102 G10: per program, 1 = rplay --draw-mod names it
  uint32_t draw_mod_version = ~0u;
  StableTable<VertexLayoutDesc> layouts;  // S102 R21-B: same
  // S102 R21-B (hazards 1, 2): one writer at a time for programs / layouts and the pipeline store's program / layout maps (program_by_hash, program_info,
  // layout_by_hash, hashing_program, store_retry, store_catalog). Held for one add (a shader module) or one map lookup, never across a draw or a pipeline build.
  std::mutex tables_mutex;

  std::unordered_map<RenderPassKey, VkRenderPass, BytesHash, BytesEqual> render_passes;
  std::unordered_map<PassClassKey, uint32_t, BytesHash, BytesEqual> pass_classes;
  std::vector<VkRenderPass> class_passes;
  std::vector<PassClassKey> class_keys;
  std::unordered_map<FramebufferKey, VkFramebuffer, BytesHash, BytesEqual> framebuffers;
  struct PipelineEntry {
    VkPipeline pipeline = VK_NULL_HANDLE;
    int32_t unused_record = -1;  // lane 276: >= 0 = made from the list and not drawn yet this session (the record to stamp at its first draw)
  };
  std::unordered_map<PipelineKey, PipelineEntry, BytesHash, BytesEqual> pipelines;
  std::unordered_map<SamplerDesc, VkSampler, BytesHash, BytesEqual> samplers;

  // ---- lane 276: the pipeline store (pipeline_store.h; SKIP_GAME_WORK_MAP 19.10). All of it is inert while store_on is false. Render thread only. ----
  struct ShaderBlob {
    const uint32_t* words = nullptr;
    size_t bytes = 0;
    const char* name = nullptr;
  };
  struct ProgramInfo {
    uint64_t vhash = 0, fhash = 0;
  };
  struct StoreStats {
    uint64_t bad_recorded = 0, bad_dropped = 0, on_demand = 0, on_demand_ns = 0, prebuilt = 0, prebuilt_ns = 0, prebuilt_used = 0, prebuilt_failed = 0, unresolved_peak = 0;
    uint64_t list_loaded = 0, list_saved = 0, cache_loaded_bytes = 0, cache_saved_bytes = 0, slices = 0, slice_ns = 0, slice_max_ns = 0;
  };
  bool store_on = false;
  std::filesystem::path store_dir;
  pstore::DeviceId store_device;
  std::vector<pstore::Record> store_records;
  uint32_t store_seed_added = 0;                                  // S106 PSEEDF2 N9: seed records merged at boot (pending): StoreNoteBuilt's cap leaves room for them
  std::unordered_map<uint64_t, uint32_t> store_index;             // RecordKey -> record
  std::vector<uint32_t> store_pending;                            // record indices waiting for their prebuild, in the order they were first needed
  size_t store_cursor = 0;
  std::vector<uint32_t> store_unresolved;                         // waiting for a program (the catalog or an AddProgram) that does not exist yet
  bool store_retry = false;                                       // a program or the catalog was added since: try the unresolved again
  std::unordered_map<uint64_t, ShaderBlob> store_catalog;         // SPIR-V hash -> the blob (static arrays owned by the caller)
  std::vector<ProgramInfo> program_info;                          // parallel to programs (filled while store_on)
  std::unordered_map<uint64_t, uint16_t> program_by_hash;         // (vhash, fhash) -> program id
  std::unordered_map<uint64_t, uint16_t> layout_by_hash;          // pstore::HashLayout -> layout id
  ProgramInfo hashing_program;                                    // the hashes StoreFindProgram just made, for StoreProgramAdded
  uint32_t store_session = 1;                                     // the session number this process stamps (the list's + 1)
  bool store_list_dirty = false, store_cache_dirty = false, building_prebuilt = false;
  std::chrono::steady_clock::time_point store_saved_at{};
  uint64_t store_used = 0;                                        // pipelines built or first-used this session (a "substantial" session has >= 20)
  uint32_t store_slice_us = 2000;
  uint32_t store_caps_mask = 0;                                   // RendererOptions::format_caps_mask, written in the list header
  uint32_t store_save_interval_ms = 30000;
  bool prebuild_allowed = true;                                   // false while a scene is live (SetPipelinePrebuildAllowed): no slices then
  std::thread store_worker;                                       // the save's serialise + hash + write, one in flight
  std::atomic<bool> store_worker_busy{false}, store_redo_list{false}, store_redo_cache{false};
  std::atomic<uint64_t> store_saved_list{0}, store_saved_cache{0};
  void StoreJoin() { if (store_worker.joinable()) store_worker.join(); }
  StoreStats store_stats;
  uint64_t last_build_ns = 0;                                     // the vkCreateGraphicsPipelines time of the last BuildPipeline
  std::string store_cache_refusal;
  std::deque<std::string> store_names;                            // stable names of the programs the store made itself
  void StoreInit(const RendererOptions& opts, std::vector<uint8_t>& cache_blob);
  void StoreNoteBuilt(const DrawState& state, uint32_t pass_class, int64_t build_ns);
  bool StoreFindProgram(const ProgramDesc& desc, uint16_t& id);   // store on: the program made from these same SPIR-V words, if there is one (else remembers the hashes for StoreProgramAdded)
  void StoreProgramAdded(uint16_t id);
  void PrebuildSlice(uint64_t budget_ns, bool all);
  // S105 B3: what the store has left, for a thread that is not the render thread (the host's precompile rule): written only by StorePublish on the render thread.
  std::atomic<uint64_t> store_pub_left{0}, store_pub_unresolved{0}, store_pub_total{0}, store_pub_made{0};
  std::atomic<bool> store_pub_valid{false};
  bool store_pub_dirty = true;   // render thread only: a count moved since the last StorePublish (F5)
  void StorePublish();
  bool PrebuildOne(uint32_t index);
  void StoreSave(bool final);
  void StoreMaybeSave();
  void StoreSummary(const char* when);

  std::mutex upload_mutex;
  // S102 R21-B (hazard 4): sealed upload batches. The bind stage seals what is pending when it hands a frame over (SealUploads); that frame's Render
  // takes only the batches sealed up to its Frame::upload_seal, so frame N never applies frame N+1's re-uploads. Seal 0 (every frame off the split)
  // takes every sealed batch and everything pending, as before. Guarded by upload_mutex.
  struct UploadBatch {
    uint64_t seal = 0;
    std::vector<ImageId> fresh;
    std::vector<ImageUpload> images;
    std::vector<BufferUpload> buffers;
    std::vector<std::pair<ImageId, uint8_t>> classes;  // S102 R21-B (hazard 3): SetImageClass changes, applied with the frame that sealed them
  };
  std::deque<UploadBatch> sealed_batches;
  uint64_t next_seal = 0;
  uint64_t upload_seal_now = 0;  // Render: this frame's seal (record thread)
  std::atomic<size_t> producer_thread{0};  // S102 R21-B (hazard 6): the bind stage's thread hash: it never waits for staging room (only Render frees it)
  std::vector<ImageId> new_images;
  std::vector<std::pair<ImageId, uint8_t>> class_changes;  // S102 R21-B (hazard 3): pending SetImageClass (upload_mutex); the class steers what Record does (prepass, half tail, blend-off), so it changes at a frame boundary
  std::vector<ImageUpload> image_uploads;
  std::vector<BufferUpload> buffer_uploads;
  std::atomic<uint64_t> staged_bytes{0};
  std::condition_variable staging_drained;
  std::mutex destroy_mutex;
  std::vector<PendingDestroy> destroy_queue;

  // S102 D1 (breadcrumbs.h, docs\renderer\DEVICE_LOST.md): the ring of the last frames' steps, the marker words, the lost-device handler.
  crumbs::Ring crumb_ring;
  crumbs::Frame* crumb = nullptr;       // this Render's record (null: breadcrumbs off this frame)
  crumbs::Step* crumb_step = nullptr;   // the step being recorded (RecordPass fills its counts)
  VkBuffer marker_buffer = VK_NULL_HANDLE;
  Allocation marker_memory;
  bool marker_failed = false;           // the buffer could not be made: markers stay off for the run
  uint32_t marker_next = 1;             // the next marker of this frame (1 = the uploads)
  uint32_t marker_open = 0;             // the marker whose END is not written yet (0: none)
  int frame_marker = 0;                 // this frame's marker mode (0: no marker words)
  int frame_barrier_mode = 0;           // S102 D1 round 6: this frame's wwe2k14_rdr_frame_top_barrier
  bool frame_barrier_end_done = false;  // mode 2: the barrier before the present was recorded
  void RecordFrameBarrier(VkCommandBuffer cmd, const char* where);
  bool plant5_done = false;             // rplay --crumbs-plant 5 (the GPU-AV vertex-fetch twin)
  uint32_t last_destroys = 0;           // ProcessDestroys: items made ready this Render
  uint64_t lost_serial = 0;             // serial + 1 of the first sign of a lost device (0: fine)
  VkEvent selftest_event = VK_NULL_HANDLE;  // rplay --crumbs-selftest: the stall
  bool EnsureMarker();
  uint16_t MarkBegin(VkCommandBuffer cmd, uint32_t slot_index);   // closes the open marker (its END), writes the next one's BEGIN
  void MarkClose(VkCommandBuffer cmd, uint32_t slot_index);         // the open marker's END: barrier, word, barrier
  bool idle_after_loss = false;         // S105 A9 review F3: the one vkDeviceWaitIdle after a real loss has been done
  bool lost_simulated = false;         // the loss is rplay's selftest (the device is fine: keep submitting)
  void OnDeviceLost(const char* where, int result, bool simulated);
  void DumpCrumbs(bool simulated);

  // Per-frame working storage, reused so a frame allocates nothing once warm.
  std::vector<PassPlan> passes;
  std::vector<Step> steps;
  std::vector<SampledImage> sampled;
  std::vector<uint8_t> next_use;
  std::vector<uint8_t> stencil_needed;
  bool sd_on = false;                                // S102 R24-C: wwe2k14_rdr_stencil_dead, read once per frame at Compile
  std::vector<const CopyOp*> depth_only_copies;      // S102 R24-C: depth copies of this frame that take the depth aspect only (the stencil is dead at their destination)
  std::deque<TargetsOp> trimmed_targets;  // a pass's targets without the depth it never uses (Compile; stable addresses)
  std::vector<uint8_t> state_needed;
  std::vector<uint8_t> clear_absorbed;
  std::vector<uint32_t> image_stamp;
  std::vector<uint32_t> sampled_index;
  std::vector<uint32_t> written_stamp;
  uint32_t stamp = 0;
  uint32_t write_stamp = 0;
  uint32_t last_persistent_count = ~0u;
  struct PooledStaging { VkBuffer buffer = VK_NULL_HANDLE; Allocation memory; };
  rdr::lean::StagingPool<PooledStaging> staging_pool;   // S102 RT1 (bit 4)
  std::vector<VkDescriptorSet> frame_sets;
  // S102 RT1 (render_lean bit 1, Q5): texture sets kept across frames, keyed by the views and samplers they name. They live in their own pool (never reset), are freed only
  // when an image they name is destroyed (ProcessDestroys: the destroy is gated until no frame in flight can name it) or when the cache is full and they were idle.
  VkDescriptorPool lean_pool = VK_NULL_HANDLE;
  rdr::lean::SetCache<size_t(kMaxTextures), VkDescriptorSet> lean_cache{2048};
  std::vector<uint64_t> lean_dying;
  std::vector<VkDescriptorSet> lean_freed;
  uint64_t lean_pool_made_fail = 0;
  void FillTextureInfos(const Frame& frame, uint32_t index, VkDescriptorImageInfo* infos);
  VkDescriptorSet GetTextureSetLean(const Frame& frame, uint32_t index, FrameSlot& slot);
  void LeanCensus();
  void LeanEvictDying(FrameSlot& slot);   // the kept sets that name the views in lean_dying go (retired into this slot)
  void LeanTool(FrameSlot& slot, const Frame& frame);   // tools only (rplay --lean-churn / audit): a view replaced under the cache; the audit of the cache against the live views
  std::vector<std::pair<VkImageView, uint64_t>> lean_churned;   // views replaced by the tool, destroyed once no frame in flight can name them
  std::vector<VkImageMemoryBarrier> barriers;
  VkPipelineStageFlags barrier_src = 0, barrier_dst = 0;
  std::vector<std::pair<VkPipeline, uint32_t>> state_pipelines;  // last pipeline per state index, with its pass class

  float scale = 1.0f;

  // ---- setup ----
  bool Init(const RendererOptions& opts, std::string& error);
  void Shutdown();

  // ---- resources ----
  VkImageView TargetView(ImageEntry& entry, uint32_t mip, uint32_t layer, uint32_t views = 1);   // S102 C1: views > 1 = a 2D-array view of that many layers
  VkImageView SampledView(ImageEntry& entry, const TextureSlot& slot);
  void DrainRetired(FrameSlot& slot);
  void ProcessDestroys(FrameSlot& slot);
  void WaitForStagingRoom();
  ImageId DummyFor(uint8_t kind) const { return kind == 1 ? dummy_cube : kind == 2 ? dummy_volume : dummy_image; }
  void ApplyUploads(VkCommandBuffer cmd, FrameSlot& slot);
  // S102 G5 (docs\renderer\GPU_OUTSIDE.md): the points outside our command buffers. out_window prints every 600 frames (meter on), out_run is the whole run (rplay's line).
  OutsideSums out_window, out_run;
  std::atomic<uint32_t> host_buffers{0};      // S103 PF2: host buffers made (Render scans the draws for them only when there are any)
  std::atomic<uint64_t> direct_overruns{0};   // S103 PF2: frames rendered later than their Record promised
  uint64_t out_collect_ns = 0, out_ring_ns = 0, out_end_ns = 0, out_begin_ns = 0, out_pipe_ns = 0, out_pipe_n = 0;  // this Render's own timers (the rest come from g_render_times' deltas)
  uint64_t out_flushes = 0;                                                                      // FlushBarriers calls that recorded a barrier, whole run
  uint32_t out_uploads = 0, out_up_images = 0, out_scaled = 0, out_up_barriers = 0, out_up_explicit = 0;                                   // this Render's ApplyUploads: uploads, scaled ones (temp image + blit), barriers recorded
  uint64_t out_up_bytes = 0;
  uint64_t ring_used_cur = 0;   // S103 PVR: this Render's ring use (constants + geometry + previous constants)
  uint32_t out_up_by_producer[3] = {0, 0, 0};   // S103 PVR: this Render's buffer copies by producer (0 other, 1 posed model vertices, 2 cloth / CPU-skinned pieces)
  uint64_t up_tot_frames = 0, up_tot_images = 0, up_tot_buffers = 0, up_tot_barriers = 0, up_tot_bytes = 0, up_tot_by_producer[3] = {0, 0, 0};   // S103 UPL (+ PVR by producer): cumulative (LastUploadStats; always counted: a few adds a frame)
  bool out_batched = false;
  void AddOutside(const OutsideSums& frame);

  // ---- frame ----
  void Render(const Frame& frame, RenderMode mode);
  void Compile(const Frame& frame);
  void Transition(ImageEntry& entry, uint32_t first_sub, uint32_t sub_count, VkImageLayout layout, VkPipelineStageFlags stage, VkAccessFlags access, bool discard, bool ranged = false);   // S102 C1: ranged = one barrier for the contiguous layers when their states agree
  const char* LayeredTargetsProblem(const TargetsOp& t);   // S102 C1 review F8: null when the targets are well formed
  void FlushBarriers(VkCommandBuffer cmd);
  void RecordPass(VkCommandBuffer cmd, const Frame& frame, const PassPlan& pass, FrameSlot& slot, VkDeviceSize ring_base);
  struct HalfPrep {  // S102 C4: what the half-size tail needs, made (and checked) before the pass's first render pass begins, so a failure declines the tail instead of dropping it
    VkPipeline depth_pipe = VK_NULL_HANDLE, comp_pipe = VK_NULL_HANDLE;
    RenderPassKey depth_key{}, half_key{}, comp_key{};
    PassClassKey depth_class{}, comp_class{};
    uint32_t hw = 0, hh = 0;
    VkDescriptorSet depth_set = VK_NULL_HANDLE, comp_set = VK_NULL_HANDLE;  // review H1: both sets are allocated (and written) in PrepareHalf: a failure declines the tail, it never drops it
  };
  bool PrepareHalf(const PassPlan& pass, FrameSlot& slot, HalfPrep& prep);
  VkDescriptorSet MakeHalfSet(FrameSlot& slot, ImageEntry* a, ImageEntry* b, ImageEntry* c);
  void RecordHalfTail(VkCommandBuffer cmd, const Frame& frame, const PassPlan& pass, FrameSlot& slot, VkDeviceSize ring_base, const std::vector<uint8_t>& half_skip, uint32_t pass_class, float factor_x, float factor_y, const HalfPrep& prep, CallCounts& pc);
  void RecordCopy(VkCommandBuffer cmd, const CopyOp& copy);
  // S102 G12: where a copy reads and writes, computed once for RecordCopy and for the batch rule (path 0 = unsupported, 1 = vkCmdCopyImage, 2 = vkCmdBlitImage)
  struct CopyGeom {
    ImageEntry* source = nullptr;
    ImageEntry* dest = nullptr;
    uint32_t source_sub = 0, dest_sub = 0, source_w = 0, source_h = 0, dest_w = 0, dest_h = 0;
    int32_t source_x = 0, source_y = 0, dest_x = 0, dest_y = 0;
    bool whole = false;
    bool depth_only = false;   // S102 R24-C: the copy names the depth aspect only (stencil_dead)
    int path = 0;
  };
  CopyGeom GeometryOf(const CopyOp& copy);
  static VkImageCopy RegionOf(const CopyOp& copy, const CopyGeom& g);
  void RecordCopyBatch(VkCommandBuffer cmd, const CopyOp* const* copies, const CopyGeom* geoms, uint32_t count);
  bool RecordPresent(VkCommandBuffer cmd, const PresentOp& present, FrameSlot& slot, VkDeviceSize ring_base, uint32_t& image_index);

  VkRenderPass GetRenderPass(const RenderPassKey& key);
  uint32_t GetPassClass(const PassClassKey& key);
  VkFramebuffer GetFramebuffer(const FramebufferKey& key);
  VkPipeline GetPipeline(const DrawState& state, uint32_t pass_class);
  VkPipeline BuildPipeline(const DrawState& state, uint32_t pass_class);
  VkSampler GetSampler(const SamplerDesc& desc);
  VkDescriptorSet GetTextureSet(const Frame& frame, uint32_t index, FrameSlot& slot);
  void WriteTextureSet(VkDescriptorSet set, const VkDescriptorImageInfo* infos);  // infos: kMaxTextures, pixel slots then vertex slots

  uint32_t ScaledExtent(uint32_t value, const ImageDesc& desc) const {
    if (!(desc.usage & kUsageScaled)) return value;
    return uint32_t(std::ceil(float(value) * scale));
  }
};

// ---- setup ------------------------------------------------------------------------------------------------------------------------

bool Renderer::Impl::Init(const RendererOptions& opts, std::string& error) {
  options = opts;
  scale = opts.render_scale;
  DeviceOptions device_options;
  device_options.validation = opts.validation;
  windowed = opts.window.window != nullptr;
  device_options.windowed = windowed;
  device_options.gpu_index = opts.gpu_index;
  device_options.gpu_name = opts.gpu_name;  // S102 U1
  if (opts.dlss_mode >= 0) device_options.streamline_dir = opts.streamline_dir;  // S10b
  device_options.format_caps_mask = opts.format_caps_mask;
  device_options.pipeline_statistics = opts.instrument_stats;  // S102 G10 (review F11)
  device_options.exclusive_fullscreen = opts.exclusive_fullscreen && windowed;  // S102 G2b
  device_options.robust_buffers = opts.robust_buffers;  // S102 D1
  device_options.occlusion_precise = opts.occlusion_precise;  // S102 G11 (F1)
  device_options.multiview = opts.multiview;  // S102 C1 review F2
  if (!device.Create(device_options, error)) return false;
  allocator.Init(&device);
  VkDevice dev = device.device();
  caps_fallback = !(opts.format_caps_mask & kCapFallbacksOff);
  bc_mode = opts.bc_decode;
  pipeline_delay_ms = opts.pipeline_delay_ms;
#ifdef RDR_DEV
  if (opts.staging_cap_mb) max_staged = uint64_t(opts.staging_cap_mb) << 20;  // S102 U1 (dev tests)
  staging_wait_plant = opts.staging_wait_plant;
#endif
  {
    // S102 U1: said once, because the missing BC formats are the cause of the Mali-G615 black title card (docs\renderer\TITLE_CARD_MALI.md).
    const bool native = device.FormatHas(VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    Log("[rdr] block-compressed textures (BC1-BC5): %s; bc_decode %u -> %s", native ? "the device samples them" : "the device CANNOT sample them", unsigned(bc_mode),
        bc_mode == 2 ? "decoded on the CPU always (diagnosis)" : (bc_mode == 1 && !native) ? "decoded on the CPU at upload" : (!native ? "REFUSED (off: the v0.3.2 behaviour)" : "sampled as they are"));
  }
  for (uint32_t f = 1; f < uint32_t(Format::Count); ++f) {
    const VkFormat format = ToVk(Format(f), device.caps());
    filterable[f] = format != VK_FORMAT_UNDEFINED && device.FormatHas(format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    // One line per format that lacks a feature the renderer would use (docs\renderer\FORMAT_CAPS.md). A full-feature device prints none.
    // Integer (R32_UINT) never filters or blends by design, and a format the device cannot sample at all (ASTC / EAC on a PC) is refused by CreateImage by name.
    if (format == VK_FORMAT_UNDEFINED || format == device.caps().depth_format || format == VK_FORMAT_R32_UINT || !device.FormatHas(format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) continue;
    const bool no_linear = !filterable[f];
    const bool no_blend = !device.FormatHas(format, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) && device.FormatHas(format, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
    if (no_linear || no_blend)
      Log("[rdr] format caps: %s (format %u):%s%s%s", CapFormatName(format), f, no_linear ? " no linear filtering -> sampled and blitted nearest (values unchanged)" : "",
          no_blend ? " no blending as a colour attachment -> blend off on draws into it" : "", caps_fallback ? "" : " [FALLBACKS OFF: asking the driver anyway]");
  }
  if (opts.format_caps_mask) Log("[rdr] format caps: test mask %08x (pretends features are missing)", unsigned(opts.format_caps_mask));

  // The ring: one section per frame in flight, for constants and dynamic geometry.
  VkBufferCreateInfo ring_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  ring_info.size = kRingSection * opts.frames_in_flight;
  ring_info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  vkCreateBuffer(dev, &ring_info, nullptr, &ring);
  VkMemoryRequirements ring_needs{};
  vkGetBufferMemoryRequirements(dev, ring, &ring_needs);
  if (!allocator.Alloc(ring_needs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true, ring_memory)) {
    error = "Not enough graphics memory for the frame ring.";
    return false;
  }
  vkBindBufferMemory(dev, ring, ring_memory.memory, ring_memory.offset);

  // One universal layout: set 0 the two constant blocks, set 1 the textures.
  VkDescriptorSetLayoutBinding constant_bindings[2] = {};
  constant_bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
  constant_bindings[1] = {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};  // S10b: velocity draws read the previous tick's vertex constants here
  VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  layout_info.bindingCount = 2;
  layout_info.pBindings = constant_bindings;
  vkCreateDescriptorSetLayout(dev, &layout_info, nullptr, &constants_layout);
  // Every texture slot is its own binding (binding = slot), so a shader declares each one as the kind it samples and nothing aliases.
  VkDescriptorSetLayoutBinding texture_bindings[kMaxTextures];
  for (uint32_t b = 0; b < uint32_t(kMaxTextures); ++b)
    texture_bindings[b] = {b, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VkShaderStageFlags(b < uint32_t(kFragmentTextures) ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT), nullptr};
  layout_info.bindingCount = kMaxTextures;
  layout_info.pBindings = texture_bindings;
  vkCreateDescriptorSetLayout(dev, &layout_info, nullptr, &textures_layout);
  VkDescriptorSetLayout set_layouts[2] = {constants_layout, textures_layout};
  VkPipelineLayoutCreateInfo pipeline_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pipeline_layout_info.setLayoutCount = 2;
  pipeline_layout_info.pSetLayouts = set_layouts;
  VkPushConstantRange system_constants{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 16};
  pipeline_layout_info.pushConstantRangeCount = 1;
  pipeline_layout_info.pPushConstantRanges = &system_constants;
  vkCreatePipelineLayout(dev, &pipeline_layout_info, nullptr, &pipeline_layout);

  VkDescriptorPoolSize constant_size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &constant_size;
  vkCreateDescriptorPool(dev, &pool_info, nullptr, &constants_pool);
  VkDescriptorSetAllocateInfo set_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  set_alloc.descriptorPool = constants_pool;
  set_alloc.descriptorSetCount = 1;
  set_alloc.pSetLayouts = &constants_layout;
  vkAllocateDescriptorSets(dev, &set_alloc, &constants_set);
  VkDescriptorBufferInfo buffer_infos[2] = {{ring, 0, kConstantRange}, {ring, 0, kConstantRange}};
  VkWriteDescriptorSet writes[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = constants_set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    writes[i].pBufferInfo = &buffer_infos[i];
  }
  vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);

  std::vector<uint8_t> cache_blob;  // lane 276: the driver's cache from the last session (header-checked), else empty
  StoreInit(opts, cache_blob);
  VkPipelineCacheCreateInfo cache_info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  if (!cache_blob.empty()) {
    cache_info.initialDataSize = cache_blob.size();
    cache_info.pInitialData = cache_blob.data();
  }
  if (vkCreatePipelineCache(dev, &cache_info, nullptr, &pipeline_cache) != VK_SUCCESS && !cache_blob.empty()) {
    Log("[rdr][pipestore] cache refused by the driver: starting empty");
    cache_info.initialDataSize = 0;
    cache_info.pInitialData = nullptr;
    store_stats.cache_loaded_bytes = 0;
    vkCreatePipelineCache(dev, &cache_info, nullptr, &pipeline_cache);
  }

  slots.resize(opts.frames_in_flight);
  if (opts.gpu_timestamps) {  // S101 lane22
    uint32_t family_count = 0;
    vk::vkGetPhysicalDeviceQueueFamilyProperties(device.physical(), &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vk::vkGetPhysicalDeviceQueueFamilyProperties(device.physical(), &family_count, families.data());
    gpu_timestamps = device.queue_family() < family_count && families[device.queue_family()].timestampValidBits >= 36;
    ts_valid_bits = device.queue_family() < family_count ? families[device.queue_family()].timestampValidBits : 0;
    ts_period = double(device.properties().limits.timestampPeriod);
    if (!gpu_timestamps) Log("[rdr][gpu] no GPU timestamps: queue family %u has %u valid timestamp bits (need 36)", device.queue_family(), ts_valid_bits);
    else if (opts.gpu_timestamp_plant == 1) ts_period *= 1000.0;  // lane 269 plant: the wrong period, the sanity check must catch it
    if (gpu_timestamps) Log("[rdr][gpu] GPU timestamps on: %u valid bits, period %.3f ns a tick, read %u frames late, no stall", ts_valid_bits, ts_period, opts.frames_in_flight);
  }
  for (FrameSlot& slot : slots) {
    VkCommandPoolCreateInfo command_pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    command_pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    command_pool.queueFamilyIndex = device.queue_family();
    vkCreateCommandPool(dev, &command_pool, nullptr, &slot.pool);
    VkCommandBufferAllocateInfo command_alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_alloc.commandPool = slot.pool;
    command_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_alloc.commandBufferCount = 1;
    vkAllocateCommandBuffers(dev, &command_alloc, &slot.cmd);
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(dev, &fence, nullptr, &slot.fence);
    VkDescriptorPoolSize sampler_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxSetsPerFrame * kMaxTextures};
    VkDescriptorPoolCreateInfo texture_pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    texture_pool.maxSets = kMaxSetsPerFrame;
    texture_pool.poolSizeCount = 1;
    texture_pool.pPoolSizes = &sampler_size;
    vkCreateDescriptorPool(dev, &texture_pool, nullptr, &slot.descriptors);
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(dev, &semaphore, nullptr, &slot.available);
    if (gpu_timestamps) MakeStampPool(slot, rdr::g_gpu_stamps.load(std::memory_order_relaxed) ? 192 : 2);  // S101 lane22; S102 S0-3: two queries until stamps first turn on, then sized for a frame's steps (~130 seen), grown when a frame wanted more
  }
  swapchain.ConfigureExclusive(opts.exclusive_fullscreen && windowed);  // S102 G2b: before the first swapchain
  if (windowed && !swapchain.Create(&device, opts.window, opts.vsync, error)) return false;
  if (!windowed && opts.present_offscreen_w && opts.present_offscreen_h) {  // S102 G2
    if (!swapchain.CreateOffscreen(&device, opts.present_offscreen_w, opts.present_offscreen_h, error)) return false;
    offscreen_out = true;
  }
  return true;
}

void Renderer::Impl::Shutdown() {
  VkDevice dev = device.device();
  if (!dev) return;
  vkDeviceWaitIdle(dev);
  for (FrameSlot& s : slots) CollectGpuTime(s);  // lane 269: the last frames
  LogGpuWindow("last window");
  Log("[rdr][render] bc decoded %llu images, %.1f MB in all (%llu levels) at shutdown", (unsigned long long)bc_images.load(), double(bc_decoded_bytes.load()) / 1048576.0, (unsigned long long)bc_decoded_levels.load());  // S102 U1
  StoreSave(true);  // lane 276 (joins the save worker first)
  StoreJoin();
  StoreSummary("at shutdown");
  swapchain.Destroy();
  if (present_sampler) vkDestroySampler(dev, present_sampler, nullptr);
  if (present_sampler_nearest) vkDestroySampler(dev, present_sampler_nearest, nullptr);
  if (device.format_caps_mask() || caps_summary_dirty || cap_stats.linear_to_nearest || cap_stats.blits_to_nearest || cap_stats.blend_disabled || cap_stats.vertex_unsupported) CapsSummary();
  for (FrameSlot& slot : slots) {
    DrainRetired(slot);
    vkDestroySemaphore(dev, slot.available, nullptr);
    vkDestroyDescriptorPool(dev, slot.descriptors, nullptr);
    if (&slot == &slots.back() && lean_pool) vkDestroyDescriptorPool(dev, lean_pool, nullptr), lean_pool = VK_NULL_HANDLE;   // S102 RT1 (after the last slot drained its retired sets)
    vkDestroyFence(dev, slot.fence, nullptr);
    vkDestroyCommandPool(dev, slot.pool, nullptr);
    if (slot.timestamps) vkDestroyQueryPool(dev, slot.timestamps, nullptr);  // S101 lane22
    if (slot.gs_pool) vkDestroyQueryPool(dev, slot.gs_pool, nullptr);  // S102 R24-F
    if (slot.occ) vkDestroyQueryPool(dev, slot.occ, nullptr);  // S102 G11
  }
  if (stamp_scratch) {  // S102 S0-3 (test plant 4)
    vkDestroyBuffer(dev, stamp_scratch, nullptr);
    allocator.Free(stamp_scratch_memory);
  }
  if (marker_buffer) {  // S102 D1
    vkDestroyBuffer(dev, marker_buffer, nullptr);
    allocator.Free(marker_memory);
    marker_buffer = VK_NULL_HANDLE;
  }
  if (selftest_event) vkDestroyEvent(dev, selftest_event, nullptr), selftest_event = VK_NULL_HANDLE;  // S102 D1
  for (auto& item : readback_staging) {  // lane 232
    vkDestroyBuffer(dev, item.second.buffer, nullptr);
    allocator.Free(item.second.memory);
  }
  readback_staging.clear();
  for (auto& item : pipelines) vkDestroyPipeline(dev, item.second.pipeline, nullptr);
  for (auto& item : framebuffers) vkDestroyFramebuffer(dev, item.second, nullptr);
  for (auto& item : render_passes) vkDestroyRenderPass(dev, item.second, nullptr);
  for (auto& item : samplers) vkDestroySampler(dev, item.second, nullptr);
  if (dummy_sampler) vkDestroySampler(dev, dummy_sampler, nullptr);
  for (Program& program : programs) {
    vkDestroyShaderModule(dev, program.vertex, nullptr);
    if (program.fragment) vkDestroyShaderModule(dev, program.fragment, nullptr);
  }
  for (uint32_t id = 1; id < images.high_water(); ++id) {
    ImageEntry& entry = images[id];
    if (!entry.live) continue;
    for (auto& view : entry.target_views) vkDestroyImageView(dev, view.second, nullptr);
    for (auto& view : entry.reinterpreted_views) vkDestroyImageView(dev, view.second, nullptr);
    if (entry.sampled_view) vkDestroyImageView(dev, entry.sampled_view, nullptr);
    vkDestroyImage(dev, entry.image, nullptr);
  }
  for (uint32_t id = 1; id < buffers.high_water(); ++id) {
    BufferEntry& entry = buffers[id];
    if (entry.live) vkDestroyBuffer(dev, entry.buffer, nullptr);
  }
  staging_pool.Drain([&](const PooledStaging& item) { vkDestroyBuffer(dev, item.buffer, nullptr); });   // S102 RT1
  for (const ImageUpload& upload : image_uploads) vkDestroyBuffer(dev, upload.staging, nullptr);
  for (const BufferUpload& upload : buffer_uploads) vkDestroyBuffer(dev, upload.staging, nullptr);
  for (const UploadBatch& batch : sealed_batches) {  // S102 R21-B: a batch sealed for a frame that was never rendered
    for (const ImageUpload& upload : batch.images) vkDestroyBuffer(dev, upload.staging, nullptr);
    for (const BufferUpload& upload : batch.buffers) vkDestroyBuffer(dev, upload.staging, nullptr);
  }
  if (lc_pipeline) vkDestroyPipeline(dev, lc_pipeline, nullptr);   // S102 L1b
  if (lc_state == 1) Log("[rdr][light_cubes] the direction table was made %llu time(s) in this run", static_cast<unsigned long long>(lc_table_builds));   // S102 L1b review F7
  if (lc_dirs_pipeline) vkDestroyPipeline(dev, lc_dirs_pipeline, nullptr);
  if (lc_dirs_pipeline_swap) vkDestroyPipeline(dev, lc_dirs_pipeline_swap, nullptr);
  if (lc_module) vkDestroyShaderModule(dev, lc_module, nullptr);
  if (lc_dirs_module) vkDestroyShaderModule(dev, lc_dirs_module, nullptr);
  if (lc_pool) vkDestroyDescriptorPool(dev, lc_pool, nullptr);
  if (lc_layout) vkDestroyPipelineLayout(dev, lc_layout, nullptr);
  if (lc_set_layout) vkDestroyDescriptorSetLayout(dev, lc_set_layout, nullptr);
  vkDestroyBuffer(dev, ring, nullptr);
  vkDestroyPipelineCache(dev, pipeline_cache, nullptr);
  vkDestroyDescriptorPool(dev, constants_pool, nullptr);
  vkDestroyPipelineLayout(dev, pipeline_layout, nullptr);
  vkDestroyDescriptorSetLayout(dev, textures_layout, nullptr);
  vkDestroyDescriptorSetLayout(dev, constants_layout, nullptr);
  allocator.Destroy();
  device.Destroy();
}

Renderer::Renderer() : impl_(new Impl) {}

Renderer::~Renderer() { impl_->Shutdown(); }

std::unique_ptr<Renderer> Renderer::Create(const RendererOptions& options, std::string& error) {
  std::unique_ptr<Renderer> renderer(new Renderer);
  if (!renderer->impl_->Init(options, error)) return nullptr;
  // Unused texture slots point at this.
  ImageDesc dummy;
  dummy.width = dummy.height = 1;
  dummy.format = Format::RGBA8;
  renderer->impl_->dummy_image = renderer->CreateImage(dummy);
  const uint32_t black = 0;
  renderer->UpdateImage(renderer->impl_->dummy_image, 0, 0, &black, 4);
  // Empty slots must match what the shader samples them as (a cube sampler needs a cube view).
  ImageDesc dummy_cube = dummy;
  dummy_cube.kind = ImageKind::Cube;
  renderer->impl_->dummy_cube = renderer->CreateImage(dummy_cube);
  for (uint32_t face = 0; face < 6; ++face) renderer->UpdateImage(renderer->impl_->dummy_cube, 0, face, &black, 4);
  ImageDesc dummy_volume = dummy;
  dummy_volume.kind = ImageKind::Volume;
  renderer->impl_->dummy_volume = renderer->CreateImage(dummy_volume);
  renderer->UpdateImage(renderer->impl_->dummy_volume, 0, 0, &black, 4);
  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  vkCreateSampler(renderer->impl_->device.device(), &sampler, nullptr, &renderer->impl_->dummy_sampler);
  sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  vkCreateSampler(renderer->impl_->device.device(), &sampler, nullptr, &renderer->impl_->present_sampler);
  sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;  // lane 200: for a present source that cannot filter
  vkCreateSampler(renderer->impl_->device.device(), &sampler, nullptr, &renderer->impl_->present_sampler_nearest);

  // The display curve starts as the identity.
  ImageDesc curve;
  curve.width = 256;
  curve.height = 1;
  curve.format = Format::RGBA8;
  renderer->impl_->display_curve = renderer->CreateImage(curve);
  uint8_t identity[256 * 4];
  for (int i = 0; i < 256; ++i) identity[i * 4] = identity[i * 4 + 1] = identity[i * 4 + 2] = identity[i * 4 + 3] = uint8_t(i);
  renderer->SetDisplayCurve(identity);

  ProgramDesc present;
  present.name = "rdr_present";
  present.vertex_spirv = kSpv_rdr_present_vert;
  present.vertex_bytes = sizeof(kSpv_rdr_present_vert);
  present.fragment_spirv = kSpv_rdr_present_frag;
  present.fragment_bytes = sizeof(kSpv_rdr_present_frag);
  renderer->impl_->present_program = renderer->AddProgram(present);
  present.name = "rdr_present_sgsr";  // settings parity S10a: the same vertex stage, SGSR 1 then the display curve
  present.fragment_spirv = kSpv_rdr_present_sgsr_frag;
  present.fragment_bytes = sizeof(kSpv_rdr_present_sgsr_frag);
  renderer->impl_->present_sgsr_program = renderer->AddProgram(present);
  present.name = "rdr_present_fetch";  // S102 R24-A: the same vertex stage, one texelFetch then the display curve (exactly 1:1 only)
  present.fragment_spirv = kSpv_rdr_present_fetch_frag;
  present.fragment_bytes = sizeof(kSpv_rdr_present_fetch_frag);
  renderer->impl_->present_fetch_program = renderer->AddProgram(present);
  ProgramDesc overlay;  // settings parity S11
  overlay.name = "rdr_overlay";
  overlay.vertex_spirv = kSpv_rdr_overlay_vert;
  overlay.vertex_bytes = sizeof(kSpv_rdr_overlay_vert);
  overlay.fragment_spirv = kSpv_rdr_overlay_frag;
  overlay.fragment_bytes = sizeof(kSpv_rdr_overlay_frag);
  renderer->impl_->overlay_program = renderer->AddProgram(overlay);
  renderer->impl_->self = renderer.get();  // S10b
  if (options.light_cube_compute && renderer->impl_->device.caps().compute) renderer->impl_->LightCubesInit();   // S102 L1b review F2: at startup, not at the first dispatch
  ProgramDesc dlss_motion;  // S10b: DLSS's motion vectors
  dlss_motion.name = "rdr_dlss_motion";
  dlss_motion.vertex_spirv = kSpv_rdr_fullscreen_vert;
  dlss_motion.vertex_bytes = sizeof(kSpv_rdr_fullscreen_vert);
  dlss_motion.fragment_spirv = kSpv_rdr_dlss_motion_frag;
  dlss_motion.fragment_bytes = sizeof(kSpv_rdr_dlss_motion_frag);
  renderer->impl_->dlss_motion_program = renderer->AddProgram(dlss_motion);
  ProgramDesc half_depth;  // S102 C4: half-size transparents: the half-size depth (nearest of four) and the depth-aware add-back
  half_depth.name = "rdr_half_depth";
  half_depth.vertex_spirv = kSpv_rdr_fullscreen_vert, half_depth.vertex_bytes = sizeof(kSpv_rdr_fullscreen_vert);
  half_depth.fragment_spirv = kSpv_rdr_half_depth_frag, half_depth.fragment_bytes = sizeof(kSpv_rdr_half_depth_frag);
  renderer->impl_->half_depth_program = renderer->AddProgram(half_depth);
  ProgramDesc half_composite = half_depth;
  half_composite.name = "rdr_half_composite";
  half_composite.fragment_spirv = kSpv_rdr_half_composite_frag, half_composite.fragment_bytes = sizeof(kSpv_rdr_half_composite_frag);
  renderer->impl_->half_composite_program = renderer->AddProgram(half_composite);
  present.name = "rdr_present_dlss";  // S10b: DLSS's picture + the HUD, then the display curve
  present.fragment_spirv = kSpv_rdr_present_dlss_frag;
  present.fragment_bytes = sizeof(kSpv_rdr_present_dlss_frag);
  renderer->impl_->present_dlss_program = renderer->AddProgram(present);
  VertexLayoutDesc overlay_vertices{};
  overlay_vertices.attributes[0] = {0, 0, uint16_t(VK_FORMAT_R32G32_SFLOAT), 0};
  overlay_vertices.attributes[1] = {1, 0, uint16_t(VK_FORMAT_R32G32_SFLOAT), 8};
  overlay_vertices.attributes[2] = {2, 0, uint16_t(VK_FORMAT_R8G8B8A8_UNORM), 16};
  overlay_vertices.attribute_count = 3;
  overlay_vertices.strides[0] = sizeof(OverlayVertex);
  renderer->impl_->overlay_layout = renderer->AddVertexLayout(overlay_vertices);
  const uint32_t white = 0xFFFFFFFFu;
  renderer->SetOverlayTexture(0, 1, 1, reinterpret_cast<const uint8_t*>(&white));
  renderer->impl_->present_layout = renderer->AddVertexLayout(VertexLayoutDesc{});
  return renderer;
}

void Renderer::SetDisplayCurve(const uint8_t* rgba256) { UpdateImage(impl_->display_curve, 0, 0, rgba256, 256 * 4); }

void Renderer::SetOverlayTexture(uint32_t id, uint32_t width, uint32_t height, const uint8_t* rgba) { SetOverlayImage(id, MakeOverlayTexture(width, height, rgba)); }
ImageId Renderer::MakeOverlayTexture(uint32_t width, uint32_t height, const uint8_t* rgba) {  // S102 R21-B (review F1): any thread; the upload joins the pending batch
  ImageDesc desc;
  desc.width = uint16_t(width);
  desc.height = uint16_t(height);
  desc.format = Format::RGBA8;
  const ImageId image = CreateImage(desc);
  if (image != kNoImage) UpdateImage(image, 0, 0, rgba, size_t(width) * height * 4);
  return image;
}
void Renderer::SetOverlayImage(uint32_t id, ImageId image) {  // S102 R21-B (review F1): the thread that renders (the map is RecordOverlay's)
  Impl& im = *impl_;
  if (const auto it = im.overlay_textures.find(id); it != im.overlay_textures.end()) DestroyImage(it->second);
  im.overlay_textures[id] = image;
}

void Renderer::RemoveOverlayTexture(uint32_t id) {   // S103 AIM: id 0 (the plain white) is never freed
  Impl& im = *impl_;
  if (!id) return;
  if (const auto it = im.overlay_textures.find(id); it != im.overlay_textures.end()) DestroyImage(it->second), im.overlay_textures.erase(it);
}

void Renderer::SetOverlay(std::shared_ptr<const OverlayList> list) { impl_->overlay = std::move(list); }
ImageId Renderer::dlss_output() const { return impl_->dlss_count ? impl_->dlss_out : kNoImage; }
bool Renderer::dlss_in_last_present() const { return impl_->dlss_count && impl_->dlss_had_history; }
bool Renderer::ColourTargetable(Format format) const { return impl_->device.FormatHas(ToVk(format, impl_->device.caps()), VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT); }   // S102 C1
bool Renderer::compute_supported() const { return impl_->device.caps().compute && impl_->lc_state == 1 && !g_force_no_compute.load(std::memory_order_relaxed); }   // review F2: true only once the pipelines exist   // S102 L1b
Renderer::RawVk Renderer::RawVulkan() const {   // S103 PS1 (tools only): see renderer.h
  const Device& d = impl_->device;
  return RawVk{d.instance(), d.physical(), d.device(), d.queue(), d.queue_family()};
}
bool Renderer::StorageTargetable(Format format) const {   // S102 L1b: the format a compute shader can write AND the readers sample: only 8-bit RGBA (storage support is core for it; BGRA is optional and declined)
  return format == Format::RGBA8 && impl_->device.FormatHas(ToVk(format, impl_->device.caps()), VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
}
bool Renderer::multiview_supported() const { return impl_->device.caps().multiview && !g_force_no_multiview.load(std::memory_order_relaxed); }   // S102 C1

uint64_t Renderer::validation_errors() const { return impl_->device.validation_errors(); }
const char* Renderer::device_name() const { return impl_->device.properties().deviceName; }

// ---- resources --------------------------------------------------------------------------------------------------------------------

ImageId Renderer::CreateImage(const ImageDesc& desc) {
  Impl& im = *impl_;
  const uint32_t id = im.images.Add();
  if (!id) {
    Problem("image_table_full", "more than %u images", kChunkSize * kMaxChunks);
    return kNoImage;
  }
  ImageEntry& entry = im.images[id];
  entry.desc = desc;
  std::atomic_ref<uint8_t>(entry.gpu_class).store(0, std::memory_order_relaxed);  // S102 S0-3 (a recycled id keeps no class); R21-B: atomic like its readers
  // S102 U1: a block-compressed texture the device cannot sample (Mali, Xclipse, Adreno 6xx: no BC) is made in a plain format and decoded on upload
  // (bc_decode.h). Before this every such texture was refused below and the pictures made of them (the title card, the menus) were missing.
  entry.decode_from = Format::Undefined;
  if (bc::IsBc(desc.format) && desc.kind != ImageKind::Volume && im.bc_mode != 0) {
    const VkFormat native = ToVk(desc.format, im.device.caps());
    if (im.bc_mode == 2 || !im.device.FormatHas(native, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
      entry.decode_from = desc.format;
      entry.desc.format = bc::Substitute(desc.format);
      im.bc_images.fetch_add(1, std::memory_order_relaxed);
    }
  }
  entry.format = ToVk(entry.desc.format, im.device.caps());
  entry.depth = entry.desc.format == Format::Depth;
  entry.aspect = entry.depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
  entry.width = im.ScaledExtent(desc.width, desc);
  entry.height = im.ScaledExtent(desc.height, desc);
  entry.layers = desc.kind == ImageKind::Cube ? 6u : desc.layers;

  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.flags = desc.kind == ImageKind::Cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
  if (desc.usage & kUsageMutable) info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  info.imageType = desc.kind == ImageKind::Volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
  info.format = entry.format;
  info.extent = {entry.width, entry.height, desc.kind == ImageKind::Volume ? uint32_t(desc.depth) : 1u};
  info.mipLevels = desc.mips;
  info.arrayLayers = entry.layers;
  info.samples = VkSampleCountFlagBits(desc.samples);
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  const bool target = desc.usage & kUsageTarget;
  const bool transient = desc.usage & kUsageTransient;
  // Refuse by name what the device cannot do, instead of asking the driver for something invalid.
  VkFormatFeatureFlags needs_features = (transient && im.device.caps().lazy_memory) ? 0 : VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  if (target) needs_features |= entry.depth ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
  if (desc.usage & kUsageStorage) needs_features |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
  if (entry.format == VK_FORMAT_UNDEFINED || !im.device.FormatHas(entry.format, needs_features)) {
    Problem("image_format_unsupported", "format %d, usage %d", int(desc.format), int(desc.usage));
    im.images.Remove(id);
    return kNoImage;
  }
  if (target) info.usage |= entry.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  if (desc.usage & kUsageStorage) info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;  // S10b (DLSS writes its output from a compute pass)
  if (transient && im.device.caps().lazy_memory) {
    info.usage |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
  } else {
    info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  }
  VkDevice dev = im.device.device();
  if (vkCreateImage(dev, &info, nullptr, &entry.image) != VK_SUCCESS) {
    Problem("image_create_failed", "%ux%u format %d", entry.width, entry.height, int(desc.format));
    im.images.Remove(id);
    return kNoImage;
  }
  VkMemoryRequirements needs{};
  vkGetImageMemoryRequirements(dev, entry.image, &needs);
  const bool lazy = transient && im.device.caps().lazy_memory;
  if (!(lazy && im.allocator.Alloc(needs, VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, 0, false, entry.memory)) && !im.allocator.Alloc(needs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false, entry.memory)) {
    Problem("image_memory_failed", "%llu bytes", static_cast<unsigned long long>(needs.size));
    vkDestroyImage(dev, entry.image, nullptr);
    entry.image = VK_NULL_HANDLE;
    im.images.Remove(id);
    return kNoImage;
  }
  vkBindImageMemory(dev, entry.image, entry.memory.memory, entry.memory.offset);
  entry.vk_usage = info.usage;
  if (info.usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = entry.image;
    view.viewType = desc.kind == ImageKind::Cube ? VK_IMAGE_VIEW_TYPE_CUBE : desc.kind == ImageKind::Volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    view.format = entry.format;
    view.subresourceRange = {VkImageAspectFlags(entry.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, desc.mips, 0, entry.layers};
    vkCreateImageView(dev, &view, nullptr, &entry.sampled_view);
  }
  entry.states.assign(size_t(desc.mips) * entry.layers, SubresourceState{});
  entry.copy_dest = false;
  entry.carries_state = false;
  entry.ever_read = false;
  entry.live = true;
  {
    std::lock_guard<std::mutex> lock(im.upload_mutex);
    im.new_images.push_back(ImageId(id));
  }
  return ImageId(id);
}

void Renderer::UpdateImage(ImageId image, uint32_t mip, uint32_t layer, const void* data, size_t bytes) {
  Impl& im = *impl_;
  // S102 U1: a decoded BC image takes its level as the game's blocks and stages the plain texels, decoded straight into the staging memory (bc_decode.h).
  Format decode_from = Format::Undefined;
  uint32_t dec_w = 0, dec_h = 0;
  size_t src_bytes = bytes;
  if (image != kNoImage && im.images[image].decode_from != Format::Undefined) {
    const ImageEntry& e = im.images[image];
    decode_from = e.decode_from;
    dec_w = std::max(1u, uint32_t(e.desc.width) >> mip), dec_h = std::max(1u, uint32_t(e.desc.height) >> mip);
    bytes = bc::DecodedBytes(decode_from, dec_w, dec_h);
  }
  im.WaitForStagingRoom();
  VkDevice dev = im.device.device();
  ImageUpload upload{image, mip, layer, VK_NULL_HANDLE, {}, VkDeviceSize(bytes)};  // S102 M1: bytes asked for
  VkMemoryRequirements needs{};
  uint32_t pool_class = 0;   // S102 RT1 (bit 4): a kept staging buffer of this size class, when one is free
  if (rdr::g_render_lean.load(std::memory_order_relaxed) & rdr::kLeanHandoff) {
    if ((pool_class = rdr::lean::StagingClass(bytes)) != 0) {
      Impl::PooledStaging kept;
      if (im.staging_pool.Take(pool_class, kept)) {
        upload.staging = kept.buffer, upload.memory = kept.memory, needs.size = kept.memory.size;
        rdr::LeanAdd(rdr::kLcStageHit);
      }
    }
  }
  if (upload.staging == VK_NULL_HANDLE) {
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = pool_class ? rdr::lean::StagingClassBytes(pool_class) : bytes;   // S102 RT1: a pooled buffer is made at its class size so any upload of the class fits
  info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  vkCreateBuffer(dev, &info, nullptr, &upload.staging);
  vkGetBufferMemoryRequirements(dev, upload.staging, &needs);
  if (!im.allocator.Alloc(needs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, true, upload.memory)) {
    Problem("staging_memory_failed", "%zu bytes", bytes);
    vkDestroyBuffer(dev, upload.staging, nullptr);
    return;
  }
  vkBindBufferMemory(dev, upload.staging, upload.memory.memory, upload.memory.offset);
  if (pool_class) rdr::LeanAdd(rdr::kLcStageMiss);
  }
  upload.pool_class = uint8_t(pool_class);
  if (decode_from != Format::Undefined) {
    if (!bc::Decode(decode_from, static_cast<const uint8_t*>(data), src_bytes, dec_w, dec_h, static_cast<uint8_t*>(upload.memory.mapped)))
      Problem("bc_decode_short", "format %d level %u: %zu bytes of %zu", int(decode_from), mip, src_bytes, bc::EncodedBytes(decode_from, dec_w, dec_h));
    if (im.bc_decoded_levels.fetch_add(1, std::memory_order_relaxed) == 0) Log("[rdr] bc: the first block-compressed level decoded on the CPU (%ux%u, format %d)", dec_w, dec_h, int(decode_from));
    im.bc_decoded_bytes.fetch_add(bytes, std::memory_order_relaxed);
  } else {
    std::memcpy(upload.memory.mapped, data, bytes);
  }
  im.staged_bytes.fetch_add(needs.size, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(im.upload_mutex);
  im.image_uploads.push_back(upload);
}

// S100 (diagnosis of the entrance's slow frames, SESSION100_SUMMARY 04:4x): where a frame's time goes in Render, logged every 600 frames.
struct RenderTimes {
  uint64_t frames = 0, wait = 0, retire = 0, uploads = 0, compile = 0, record = 0, submit = 0, present = 0;
  uint64_t upload_bytes = 0, passes = 0, steps = 0;
  uint64_t drain = 0, destroys = 0, pools = 0;               // retire, split (ns)
  uint64_t pipestore = 0;                                    // lane 276: the listed-pipeline slice + the save's blob copy (printed only when non-zero)
  uint64_t retired_items = 0, destroy_items = 0, new_framebuffers = 0, texture_sets = 0, set_ns = 0, fb_ns = 0;
};
static RenderTimes g_render_times;
static uint64_t RenderNow() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void Renderer::Impl::DrainRetired(FrameSlot& slot) {
  VkDevice dev = device.device();
  uint64_t released = 0;
  for (const Retired& item : slot.retired) {
    if (item.lean_set) vkFreeDescriptorSets(dev, lean_pool, 1, &item.lean_set);   // S102 RT1
    if (item.framebuffer) vkDestroyFramebuffer(dev, item.framebuffer, nullptr);
    if (item.view) vkDestroyImageView(dev, item.view, nullptr);
    if (item.image) vkDestroyImage(dev, item.image, nullptr);
    const bool kept = item.pool_class && item.buffer && staging_pool.Give(item.pool_class, PooledStaging{item.buffer, item.memory});   // S102 RT1: the slot's fence has signalled: its copy is done
    if (item.buffer && !kept) vkDestroyBuffer(dev, item.buffer, nullptr);
    if (item.memory.block >= 0 && !kept) allocator.Free(item.memory);
    if (item.free_image) images.Remove(item.free_image);
    if (item.free_buffer) buffers.Remove(item.free_buffer);
    released += item.staged_bytes;
  }
  slot.retired.clear();
  if (released) {
    staged_bytes.fetch_sub(released, std::memory_order_relaxed);
    staging_drained.notify_all();
  }
}

// Loading a lot of textures at once must not run the host out of memory. Bounded: a game that has stopped presenting must not hang here.
void Renderer::Impl::WaitForStagingRoom() {
  if (staged_bytes.load(std::memory_order_relaxed) < max_staged) return;
  // S102 U1 (review): staging is freed only by DrainRetired, which runs in Render on the render thread: a wait on that thread can only time out (2 s a call).
  // Before the first Render (render_thread 0) nothing has run that could free staging either: no wait then.
  if (!staging_wait_plant) {
    const size_t render = render_thread.load(std::memory_order_relaxed);
    const size_t me = std::hash<std::thread::id>()(std::this_thread::get_id());
    if (render == 0 || render == me) return;
    if (me == producer_thread.load(std::memory_order_relaxed)) return;  // S102 R21-B: the bind stage hands Render its frames; waiting here would wait on itself
  }
  std::unique_lock<std::mutex> lock(upload_mutex);
  staging_drained.wait_for(lock, std::chrono::seconds(2), [&] { return staged_bytes.load(std::memory_order_relaxed) < max_staged; });
}

// Runs on the render thread only, so nothing here races recording.
void Renderer::Impl::ProcessDestroys(FrameSlot& slot) {
  std::vector<PendingDestroy> ready;
  {
    std::lock_guard<std::mutex> lock(destroy_mutex);
    auto split = std::stable_partition(destroy_queue.begin(), destroy_queue.end(), [&](const PendingDestroy& item) { return item.ready_serial > serial; });
    ready.assign(split, destroy_queue.end());
    destroy_queue.erase(split, destroy_queue.end());
  }
  g_render_times.destroy_items += ready.size();
  last_destroys = uint32_t(ready.size());  // S102 D1: the frame's crumb
  for (const PendingDestroy& item : ready) {
    if (item.image) {
      ImageEntry& entry = images[item.image];
      if (!entry.live) continue;
      for (auto it = framebuffers.begin(); it != framebuffers.end();) {
        bool uses = false;
        for (const auto& view : entry.target_views) {
          for (VkImageView key_view : it->first.views) uses |= key_view == view.second;
        }
        if (uses) {
          Retired retired;
          retired.framebuffer = it->second;
          slot.retired.push_back(retired);
          it = framebuffers.erase(it);
        } else {
          ++it;
        }
      }
      const auto retire_views = [&](auto& views) {
        for (auto& view : views) {
          Retired retired;
          retired.view = view.second;
          slot.retired.push_back(retired);
        }
        views.clear();
      };
      if (lean_cache.size()) {  // S102 RT1: the views a kept set may name (a sampled view; the reinterpreted ones)
        lean_dying.push_back(reinterpret_cast<uint64_t>(entry.sampled_view));
        for (auto& view : entry.reinterpreted_views) lean_dying.push_back(reinterpret_cast<uint64_t>(view.second));
      }
      retire_views(entry.target_views);
      retire_views(entry.reinterpreted_views);
      Retired retired;
      retired.image = entry.image;
      retired.view = entry.sampled_view;
      retired.memory = entry.memory;
      retired.free_image = item.image;
      slot.retired.push_back(retired);
      entry.sampled_view = VK_NULL_HANDLE;
      entry.image = VK_NULL_HANDLE;
      entry.memory = Allocation{};
      entry.live = false;
    } else if (item.buffer) {
      BufferEntry& entry = buffers[item.buffer];
      if (!entry.live) continue;
      Retired retired;
      retired.buffer = entry.buffer;
      retired.memory = entry.memory;
      retired.free_buffer = item.buffer;
      slot.retired.push_back(retired);
      entry.buffer = VK_NULL_HANDLE;
      entry.memory = Allocation{};
      entry.live = false;
    }
  }
  LeanEvictDying(slot);   // S102 RT1: every kept set that names a view of a destroyed image goes (into THIS slot's retired list: the destroy was gated until no frame in flight names the image)
}

void Renderer::Impl::LeanEvictDying(FrameSlot& slot) {   // S102 RT1
  if (lean_dying.empty()) return;
  lean_freed.clear();
  const int plant = rdr::g_lean_plant.load(std::memory_order_relaxed);
  const size_t gone = lean_cache.EvictViews(lean_dying, lean_freed, plant == 1);
  for (VkDescriptorSet freed : lean_freed) {
    if (plant == 7 && lean_pool) {   // PLANT 7 (tools only): freed at once, while frames in flight may still use the set
      vkFreeDescriptorSets(device.device(), lean_pool, 1, &freed);
      continue;
    }
    Retired retired;
    retired.lean_set = freed;
    slot.retired.push_back(retired);
  }
  rdr::LeanAdd(rdr::kLcSetEvict, gone);
  lean_dying.clear();
}

// S102 RT1 (tools only, rplay --lean-churn N / --lean-audit N): the proof that eviction runs. Every N frames one live image's sampled view is REPLACED (a view change: a new view of the same image,
// the old one destroyed after the destroy delay, exactly the views a destroyed image would retire); the same happens in the arm with the lever off, so the pictures stay comparable.
// The audit counts kept sets that name a view no live image owns (must be 0).
void Renderer::Impl::LeanTool(FrameSlot& slot, const Frame& frame) {
  using namespace rdr;
  for (auto it = lean_churned.begin(); it != lean_churned.end();) {
    if (it->second <= serial) {
      Retired retired;
      retired.view = it->first;
      slot.retired.push_back(retired);
      it = lean_churned.erase(it);
    } else {
      ++it;
    }
  }
  const int every = g_lean_churn_every.load(std::memory_order_relaxed);
  if (every > 0 && serial % uint64_t(every) == 0) {
    std::vector<uint32_t> sampled;   // the images this frame samples (first seen order): the churn hits views the kept sets name; the same choice in both arms
    for (uint32_t t = 0; t < frame.texture_sets.count; ++t)
      for (const TextureSlot& texture : frame.texture_sets[t].slots)
        if (texture.image != kNoImage && texture.image < images.high_water() && images[texture.image].live && images[texture.image].sampled_view != VK_NULL_HANDLE && images[texture.image].desc.samples == 1 &&
            std::find(sampled.begin(), sampled.end(), uint32_t(texture.image)) == sampled.end())
          sampled.push_back(uint32_t(texture.image));
    if (!sampled.empty()) {
      do {
        const uint32_t id = sampled[uint32_t((serial / uint64_t(every)) * 7) % uint32_t(sampled.size())];
        ImageEntry& entry = images[id];
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = entry.image;
        view.viewType = entry.desc.kind == ImageKind::Cube ? VK_IMAGE_VIEW_TYPE_CUBE : entry.desc.kind == ImageKind::Volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
        view.format = entry.format;
        view.subresourceRange = {VkImageAspectFlags(entry.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, entry.desc.mips, 0, entry.layers};
        VkImageView fresh = VK_NULL_HANDLE;
        if (vkCreateImageView(device.device(), &view, nullptr, &fresh) != VK_SUCCESS) break;
        const VkImageView old = entry.sampled_view;
        entry.sampled_view = fresh;
        lean_dying.push_back(reinterpret_cast<uint64_t>(old));
        lean_churned.push_back({old, serial + kDestroyDelayFrames});
        for (auto& other : entry.reinterpreted_views) {   // the other views of the image are replaced the same way (made again on first use by SampledView)
          lean_dying.push_back(reinterpret_cast<uint64_t>(other.second));
          lean_churned.push_back({other.second, serial + kDestroyDelayFrames});
        }
        entry.reinterpreted_views.clear();
        LeanEvictDying(slot);
        LeanAdd(kLcChurn);
      } while (false);
    }
  }
  const int audit = g_lean_audit_every.load(std::memory_order_relaxed);
  if (audit > 0 && serial % uint64_t(audit) == 0 && lean_cache.size()) {
    std::unordered_set<uint64_t> live_views;
    for (uint32_t id = 1; id < images.high_water(); ++id) {
      const ImageEntry& entry = images[id];
      if (!entry.live) continue;
      live_views.insert(reinterpret_cast<uint64_t>(entry.sampled_view));
      for (const auto& view : entry.reinterpreted_views) live_views.insert(reinterpret_cast<uint64_t>(view.second));
    }
    uint64_t bad = 0;
    lean_cache.ForEach([&](const auto& e) {
      for (size_t i = 0; i < size_t(kMaxTextures); ++i) bad += e.key.view[i] != 0 && !live_views.count(e.key.view[i]);
    });
    LeanAdd(kLcAudit);
    LeanAdd(kLcAuditBad, bad);
  }
}

// S102 D1 (breadcrumbs.h): the marker words, kMarkerWords a frame slot, host-visible so they can be read after the device is lost. Made on first use;
// a failure turns the markers off for the run (one line), never the frame.
bool Renderer::Impl::EnsureMarker() {
  if (marker_buffer) return true;
  if (marker_failed) return false;
  VkDevice dev = device.device();
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = VkDeviceSize(slots.size()) * crumbs::kMarkerWords * 4;
  info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (vkCreateBuffer(dev, &info, nullptr, &marker_buffer) != VK_SUCCESS) {
    marker_buffer = VK_NULL_HANDLE, marker_failed = true;
    Log("[rdr][crumbs] no GPU marker buffer (vkCreateBuffer failed): the breadcrumbs keep the CPU ring only");
    return false;
  }
  VkMemoryRequirements needs{};
  vkGetBufferMemoryRequirements(dev, marker_buffer, &needs);
  if (!allocator.Alloc(needs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, true, marker_memory) || !marker_memory.mapped) {
    vkDestroyBuffer(dev, marker_buffer, nullptr);
    marker_buffer = VK_NULL_HANDLE, marker_failed = true;
    Log("[rdr][crumbs] no GPU marker buffer (no host-visible memory): the breadcrumbs keep the CPU ring only");
    return false;
  }
  vkBindBufferMemory(dev, marker_buffer, marker_memory.memory, marker_memory.offset);
  std::memset(marker_memory.mapped, 0, size_t(info.size));
  return true;
}

// The marker words (breadcrumbs.h). Outside a render pass only. MarkBegin closes the open marker, then writes the next one's BEGIN (no barrier: the
// front end's reach); MarkClose writes the open marker's END after all earlier commands finished and makes it visible to the host before anything after it.
uint16_t Renderer::Impl::MarkBegin(VkCommandBuffer cmd, uint32_t slot_index) {
  if (!marker_buffer) return crumbs::kNoMarker;
  MarkClose(cmd, slot_index);
  if (marker_next > crumbs::kMaxMarker) return crumbs::kNoMarker;
  const uint32_t k = marker_next++;
  const uint32_t value = crumbs::MarkValue(serial);
  vkCmdUpdateBuffer(cmd, marker_buffer, (VkDeviceSize(slot_index) * crumbs::kMarkerWords + crumbs::BeginWord(k)) * 4, 4, &value);
  marker_open = k;
  return uint16_t(k);
}

void Renderer::Impl::MarkClose(VkCommandBuffer cmd, uint32_t slot_index) {
  if (!marker_buffer || !marker_open) return;
  if (frame_marker == 4) {  // mode 4: BEGIN words only, no barriers (the GPU is not serialised; a race the barriers would hide stays possible)
    marker_open = 0;
    return;
  }
  const bool bare = crumbs::g_plant.load(std::memory_order_relaxed) == 4;  // plant 4: the first version's stream (no barriers)
  if (!bare) vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 0, nullptr);
  const uint32_t value = crumbs::MarkValue(serial);
  vkCmdUpdateBuffer(cmd, marker_buffer, (VkDeviceSize(slot_index) * crumbs::kMarkerWords + crumbs::EndWord(marker_open)) * 4, 4, &value);
  if (!bare) {
    VkMemoryBarrier flush{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    flush.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, flush.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &flush, 0, nullptr, 0, nullptr);
  }
  marker_open = 0;
}

// The first sign of a lost device (a fence wait, the submit, the acquire or the present returned VK_ERROR_DEVICE_LOST): one loud line, the accounting
// switched to "nothing presented" (rdr::g_device_lost: the swapchain accepts no present, frames_*.csv device_lost = 1), and the breadcrumbs. Once a run.
// `simulated`: rplay's selftest (the device is fine; the GPU is held at a planted wait).
void Renderer::Impl::OnDeviceLost(const char* where, int result, bool simulated) {
  frames_dropped.fetch_add(1, std::memory_order_relaxed);  // S103 CB1: whatever this frame recorded is gone
  if (lost_serial) return;
  lost_serial = serial + 1;
  g_device_lost.store(serial + 1, std::memory_order_relaxed);
  crumbs::g_presented_at_loss.store(g_presented.load(std::memory_order_relaxed), std::memory_order_relaxed);
  lost_simulated = simulated;
  Log("[rdr][DEVICE_LOST] ===== the Vulkan device is LOST (%s, VkResult %d, frame serial %llu, %s). The picture is frozen from here; presents no longer count. =====", where, result,
      static_cast<unsigned long long>(serial), device.properties().deviceName);
  DumpCrumbs(simulated);
  // Last: a RDR_DEV build (rplay / fview) stops on a Problem, and the dump must be out first (review F4).
  Problem("device_lost", "%s returned VkResult %d at frame serial %llu: the GPU stopped and nothing will be drawn again this run (docs\\renderer\\DEVICE_LOST.md)%s", where, result,
          static_cast<unsigned long long>(serial), simulated ? " [selftest: simulated]" : "");
}

// The dump: per frame slot what the marker words say the GPU reached, then the kept frames (newest last) with their steps around that point, then the last
// pipelines built. Every line starts [rdr][crumbs].
void Renderer::Impl::DumpCrumbs(bool simulated) {
  const auto prog_name = [&](uint16_t p) -> const char* { return p < programs.size() && programs[p].name ? programs[p].name : (p == 0xFFFF ? "-" : "?"); };
  const auto step_line = [&](const crumbs::Frame& f, const crumbs::Step& s, const char* tag) {
    Log("[rdr][crumbs] %s frame %llu step %u: %s %u, class %s, image %u, marker %d, draws %u, binds %u, verts+indices %u, programs %s .. %s", tag,
        static_cast<unsigned long long>(f.serial), s.step, crumbs::StepKindName(s.kind), s.index, GpuClassName(s.cls), s.image, s.marker == crumbs::kNoMarker ? -1 : int(s.marker),
        s.draws, s.binds, s.prims, prog_name(s.first_prog), prog_name(s.last_prog));
  };
  if (!crumbs::g_on.load(std::memory_order_relaxed)) {
    Log("[rdr][crumbs] off (--wwe2k14_rdr_breadcrumbs=false): nothing kept");
    return;
  }
  // 1. The marker words of every slot.
  const uint32_t* words_all = marker_buffer && marker_memory.mapped ? reinterpret_cast<const uint32_t*>(marker_memory.mapped) : nullptr;
  if (!words_all) Log("[rdr][crumbs] no GPU marker words (--wwe2k14_rdr_gpu_marker=0, or no buffer): only the CPU ring below");
  const crumbs::Frame* suspect = nullptr;
  int32_t suspect_marker = -1;
  for (uint32_t s = 0; words_all && s < slots.size(); ++s) {
    const uint32_t* words = words_all + size_t(s) * crumbs::kMarkerWords;
    const crumbs::Frame* f = words[0] ? crumb_ring.FindLow(words[0]) : nullptr;
    crumbs::SlotReach r = crumbs::Reach(words, f ? f->end_marker : crumbs::kNoMarker);
    if (f && f->marker_mode == 4) {  // mode 4 has no END words: the front end's reach names the step (the fault is at or after it)
      r.unfinished = r.begun < 0 ? 1 : r.begun;
      r.done = f->end_marker != crumbs::kNoMarker && r.begun == int32_t(f->end_marker);
    }
    if (!r.any) {
      Log("[rdr][crumbs] GPU slot %u: no marker written yet", s);
      continue;
    }
    const crumbs::Step* at = f ? crumbs::StepOfMarker(*f, r.unfinished) : nullptr;
    const char* where = r.done ? "its END (all its work finished)" : r.unfinished == int32_t(crumbs::kMarkUploads) ? "the uploads" : at ? "a step" :
                        f && r.unfinished == int32_t(f->end_marker) ? "the frame's tail (the read-backs after the last step)" : "a marker the ring does not hold";
    Log("[rdr][crumbs] GPU slot %u: frame %llu (word %08x): finished marker %d, first unfinished %d (%s), front end reached %d, end marker %d%s", s,
        f ? static_cast<unsigned long long>(f->serial) : 0ull, r.serial_lo, r.ended, r.done ? -1 : r.unfinished, where, r.begun, f && f->end_marker != crumbs::kNoMarker ? int(f->end_marker) : -1,
        f ? (f->submitted ? "" : " (its submit FAILED: never ran)") : " (not in the ring)");
    if (at && !r.done) step_line(*f, *at, "  unfinished:");
    if (f && f->submitted && !r.done && (!suspect || f->serial < suspect->serial)) suspect = f, suspect_marker = r.unfinished;  // the oldest submitted frame that did not finish
  }
  if (suspect) {
    const crumbs::Step* at = crumbs::StepOfMarker(*suspect, suspect_marker);
    Log("[rdr][crumbs] VERDICT: the GPU stopped in frame %llu in %s or the steps after it up to the next marked one %s",
        static_cast<unsigned long long>(suspect->serial), at ? "the step below" : suspect_marker == int32_t(crumbs::kMarkUploads) ? "the uploads" : "the frame's tail (read-backs)",
        suspect->marker_mode == 4 ? " (mode 4, BEGIN words only: the front end reached it; it may have run ahead of the shaders, and the newest word can read one short)" : " (every marked work before it FINISHED: its END word was written after a full barrier)");
    if (at) step_line(*suspect, *at, "  VERDICT");
  } else if (words_all) {
    Log("[rdr][crumbs] VERDICT: every submitted frame in the ring finished its end marker: the loss is not inside a frame's commands the markers cover (a present, the driver, or a frame not kept)");
  }
  // 2. The kept frames, oldest first: one summary line each; the suspect frame's steps around the marker (all of them for the newest submitted frame when there is no marker).
  const crumbs::Frame* newest_submitted = nullptr;
  for (const crumbs::Frame& f : crumb_ring.frames)
    if (f.serial != ~0ull && f.submitted && (!newest_submitted || f.serial > newest_submitted->serial)) newest_submitted = &f;
  for (uint64_t back = crumbs::kFrames; back-- > 0;) {
    if (serial < back) continue;
    const crumbs::Frame* f = crumb_ring.Find(serial - back);
    if (!f) continue;
    uint32_t passes_n = 0, copies_n = 0, other_n = 0;
    uint64_t draws = 0, prims = 0;
    for (uint32_t i = 0; i < f->n; ++i) {
      const crumbs::Step& s = f->steps[i];
      (s.kind == crumbs::kPass ? passes_n : s.kind == crumbs::kCopy || s.kind == crumbs::kCopyBatch ? copies_n : other_n) += 1;
      if (s.kind == crumbs::kPass) draws += s.draws, prims += s.prims;
    }
    Log("[rdr][crumbs] frame %llu (slot %u, mode %u, %s): steps %u (kept %u: passes %u, copies %u, other %u), draws %llu, verts+indices %llu, uploads %u, destroys %u, pipelines built %u, markers %s",
        static_cast<unsigned long long>(f->serial), f->slot, unsigned(f->mode), f->submitted ? "submitted" : "NOT submitted", f->steps_total, f->n, passes_n, copies_n, other_n,
        static_cast<unsigned long long>(draws), static_cast<unsigned long long>(prims), f->uploads, f->destroys, f->built, f->marker_mode == 4 ? "every step, BEGIN only" : f->marker_mode >= 3 ? "every step" : f->marker_mode == 2 ? "passes + copy runs" : f->marker_mode == 1 ? "class runs" : "off");
    // without a marker verdict, the steps of the newest TWO submitted frames (frames in flight are 2: either can hold the fault)
    if (f == suspect || (!suspect && (f == newest_submitted || (newest_submitted && f->serial + 1 == newest_submitted->serial && f->submitted)))) {
      const crumbs::Step* at = suspect ? crumbs::StepOfMarker(*f, suspect_marker) : nullptr;
      uint32_t from = 0, to = f->n;
      if (at) {
        const uint32_t i = uint32_t(at - f->steps);
        from = i > 6 ? i - 6 : 0, to = std::min(f->n, i + 10);
      }
      for (uint32_t i = from; i < to; ++i) step_line(*f, f->steps[i], &f->steps[i] == at ? "  >>" : "    ");
    }
  }
  // 3. The last pipelines built (they are made on the render thread between submits: a first-use pipeline was the newest thing in 456's loss frame).
  const uint64_t nb = std::min<uint64_t>(crumb_ring.built_count, crumbs::kBuilt);
  for (uint64_t i = crumb_ring.built_count - nb; i < crumb_ring.built_count; ++i) {
    const crumbs::Built& b = crumb_ring.built[i % crumbs::kBuilt];
    Log("[rdr][crumbs] pipeline built at frame %llu: %s, class %u", static_cast<unsigned long long>(b.serial), prog_name(b.program), unsigned(b.cls));
  }
  Log("[rdr][crumbs] end of the dump%s", simulated ? " (selftest)" : "");
}

// S102 D1 round 6: one full memory barrier (wwe2k14_rdr_frame_top_barrier). Outside a render pass only. The role audit names it (a planted twin that drops it is seen).
void Renderer::Impl::RecordFrameBarrier(VkCommandBuffer cmd, const char* where) {
  VkMemoryBarrier all{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  all.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  all.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &all, 0, nullptr, 0, nullptr);
  crumbs::g_frame_barrier_count.fetch_add(1, std::memory_order_relaxed);
  if (options.counts) ++counts_cur.total.barriers;
  if (std::FILE* ra = crumbs::g_role_audit.load(std::memory_order_relaxed)) std::fprintf(ra, "{\"f\":%llu,\"s\":-1,\"m\":0,\"k\":\"frame_barrier\",\"w\":\"%s\"}\n", static_cast<unsigned long long>(serial), where);
}

void Renderer::DestroyImage(ImageId image) {
  Impl& im = *impl_;
  std::lock_guard<std::mutex> lock(im.destroy_mutex);
  PendingDestroy item;
  item.image = image;
  item.ready_serial = std::atomic_ref<uint64_t>(im.serial).load(std::memory_order_relaxed) + (g_plant_destroy_now.load(std::memory_order_relaxed) ? 0 : kDestroyDelayFrames);  // S102 R21-B: read on the bind stage (plant: no delay)
  im.destroy_queue.push_back(item);
}

BufferId Renderer::CreateBuffer(size_t bytes, const void* initial) {
  Impl& im = *impl_;
  const uint32_t id = im.buffers.Add();
  if (!id) {
    Problem("buffer_table_full", "more than %u buffers", kChunkSize * kMaxChunks);
    return kNoBuffer;
  }
  BufferEntry& entry = im.buffers[id];
  VkDevice dev = im.device.device();
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = bytes;
  info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  vkCreateBuffer(dev, &info, nullptr, &entry.buffer);
  VkMemoryRequirements needs{};
  vkGetBufferMemoryRequirements(dev, entry.buffer, &needs);
  const bool direct = im.device.caps().unified_memory;
  const VkMemoryPropertyFlags required = direct ? (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  if (!im.allocator.Alloc(needs, required, 0, true, entry.memory)) {
    Problem("buffer_memory_failed", "%zu bytes", bytes);
    vkDestroyBuffer(dev, entry.buffer, nullptr);
    entry.buffer = VK_NULL_HANDLE;
    im.buffers.Remove(id);
    return kNoBuffer;
  }
  vkBindBufferMemory(dev, entry.buffer, entry.memory.memory, entry.memory.offset);
  entry.bytes = bytes;
  entry.live = true;
  if (initial && entry.memory.mapped) std::memcpy(entry.memory.mapped, initial, bytes);  // a new buffer: the GPU cannot be using it yet
  else if (initial) UpdateBuffer(BufferId(id), 0, initial, bytes);
  return BufferId(id);
}

void Renderer::UpdateBuffer(BufferId buffer, size_t offset, const void* data, size_t bytes, uint8_t producer) {
  Impl& im = *impl_;
  im.WaitForStagingRoom();
  BufferEntry& entry = im.buffers[buffer];
  VkDevice dev = im.device.device();
  BufferUpload upload{VK_NULL_HANDLE, {}, entry.buffer, offset, bytes, producer};
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = bytes;
  info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  vkCreateBuffer(dev, &info, nullptr, &upload.staging);
  VkMemoryRequirements needs{};
  vkGetBufferMemoryRequirements(dev, upload.staging, &needs);
  if (!im.allocator.Alloc(needs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, true, upload.memory)) {
    Problem("staging_memory_failed", "%zu bytes", bytes);
    vkDestroyBuffer(dev, upload.staging, nullptr);
    return;
  }
  vkBindBufferMemory(dev, upload.staging, upload.memory.memory, upload.memory.offset);
  std::memcpy(upload.memory.mapped, data, bytes);
  im.staged_bytes.fetch_add(needs.size, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(im.upload_mutex);
  im.buffer_uploads.push_back(upload);
}

BufferId Renderer::CreateHostBuffer(size_t bytes, uint8_t** mapped) {   // S103 PF2
  Impl& im = *impl_;
  if (mapped) *mapped = nullptr;
  const uint32_t id = im.buffers.Add();
  if (!id) {
    Problem("buffer_table_full", "more than %u buffers", kChunkSize * kMaxChunks);
    return kNoBuffer;
  }
  BufferEntry& entry = im.buffers[id];
  VkDevice dev = im.device.device();
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = bytes;
  info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (vkCreateBuffer(dev, &info, nullptr, &entry.buffer) != VK_SUCCESS) {   // S103 PF2 (review): a failed create is kNoBuffer (the caller's pcopy_failed route), never a bind of a null buffer
    Problem("buffer_create_failed", "%zu bytes (host-visible)", bytes);
    entry.buffer = VK_NULL_HANDLE;
    im.buffers.Remove(id);
    return kNoBuffer;
  }
  VkMemoryRequirements needs{};
  vkGetBufferMemoryRequirements(dev, entry.buffer, &needs);
  const bool unified = im.device.caps().unified_memory;
  const VkMemoryPropertyFlags visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  const bool got = im.allocator.Alloc(needs, unified ? (visible | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) : visible, unified ? 0 : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true, entry.memory);
  if (!got || !entry.memory.mapped) {
    if (got) im.allocator.Free(entry.memory);
    Problem("buffer_memory_failed", "%zu bytes (host-visible)", bytes);
    vkDestroyBuffer(dev, entry.buffer, nullptr);
    entry.buffer = VK_NULL_HANDLE;
    im.buffers.Remove(id);
    return kNoBuffer;
  }
  vkBindBufferMemory(dev, entry.buffer, entry.memory.memory, entry.memory.offset);
  entry.bytes = bytes;
  entry.live = true;
  entry.host = true, entry.last_read = 0;
  im.host_buffers.fetch_add(1, std::memory_order_relaxed);
  if (static std::atomic<bool> logged{false}; !logged.exchange(true)) {   // once: the memory type the posed copies got (the next Surface log shows whether it is cached or write-combined)
    const VkMemoryPropertyFlags f = im.device.memory().memoryTypes[entry.memory.type].propertyFlags;
    Log("[rdr] host-visible vertex buffers (posed copies): memory type %d flags 0x%x:%s%s%s%s%s (unified device %d)", entry.memory.type, unsigned(f), (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? " DEVICE_LOCAL" : "", (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? " HOST_VISIBLE" : "",
        (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? " HOST_COHERENT" : "", (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? " HOST_CACHED" : " (write-combined: not cached)", "", int(unified));
  }
  if (mapped) *mapped = static_cast<uint8_t*>(entry.memory.mapped);
  return BufferId(id);
}

bool Renderer::HostBufferIdle(BufferId buffer) const {
  const BufferEntry& e = impl_->buffers[buffer];
  if (!e.host) return true;
  const uint64_t lr = std::atomic_ref<uint64_t>(const_cast<uint64_t&>(e.last_read)).load(std::memory_order_relaxed);
  return lr == 0 || lr - 1 + impl_->slots.size() < RenderSerial();   // a Render s waits for frame s - slots before it records: frames with index + slots < serial are finished
}
uint64_t Renderer::DirectOverruns() const { return impl_->direct_overruns.load(std::memory_order_relaxed); }

bool Renderer::UnifiedMemory() const { return impl_->device.caps().unified_memory; }
uint32_t Renderer::FramesInFlight() const { return uint32_t(impl_->slots.size()); }
uint64_t Renderer::RenderSerial() const { return std::atomic_ref<uint64_t>(impl_->serial).load(std::memory_order_relaxed); }

void Renderer::DestroyBuffer(BufferId buffer) {
  Impl& im = *impl_;
  std::lock_guard<std::mutex> lock(im.destroy_mutex);
  PendingDestroy item;
  item.buffer = buffer;
  item.ready_serial = std::atomic_ref<uint64_t>(im.serial).load(std::memory_order_relaxed) + (g_plant_destroy_now.load(std::memory_order_relaxed) ? 0 : kDestroyDelayFrames);  // S102 R21-B: read on the bind stage (plant: no delay)
  im.destroy_queue.push_back(item);
}

uint16_t Renderer::AddProgram(const ProgramDesc& desc) {
  Impl& im = *impl_;
  std::lock_guard<std::mutex> tables(im.tables_mutex);  // S102 R21-B (hazards 1, 2)
  uint16_t same = 0;
  if (im.store_on && im.StoreFindProgram(desc, same)) return same;  // lane 276: the same words are the same program (the prebuilt one)
  Program program;
  program.name = desc.name;
  program.prepass_class = colour_cuts::ClassifyFragment(desc.fragment_spirv, desc.fragment_bytes);  // S102 G10
  program.half_ok = desc.fragment_spirv && !half_trans::ReadsFragCoord(desc.fragment_spirv, desc.fragment_bytes);  // S102 C4
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = desc.vertex_bytes;
  info.pCode = desc.vertex_spirv;
  vkCreateShaderModule(im.device.device(), &info, nullptr, &program.vertex);
  if (desc.fragment_spirv) {  // none: a depth-only program
    info.codeSize = desc.fragment_bytes;
    info.pCode = desc.fragment_spirv;
    vkCreateShaderModule(im.device.device(), &info, nullptr, &program.fragment);
  }
  if (im.programs.size() >= kNoTableId || !im.programs.push_back(program)) {  // S102 R21-B (review F5): full: named, and an id no draw can use (never program 0, a real one)
    Problem("program_table_full", "%u programs", unsigned(kNoTableId));
    vkDestroyShaderModule(im.device.device(), program.vertex, nullptr);
    if (program.fragment) vkDestroyShaderModule(im.device.device(), program.fragment, nullptr);
    return kNoTableId;
  }
  if (im.store_on) im.StoreProgramAdded(uint16_t(im.programs.size() - 1));
  return uint16_t(im.programs.size() - 1);
}

uint16_t Renderer::AddVertexLayout(const VertexLayoutDesc& desc) {
  std::lock_guard<std::mutex> tables(impl_->tables_mutex);  // S102 R21-B (hazards 1, 2)
  // S102 R21-B (review F5): a layout appended, or false (named) when the table is full; the caller returns kNoTableId, never the previous layout's id.
  const auto PushLayout = [](Impl& im, const VertexLayoutDesc& d) {
    if (im.layouts.size() >= kNoTableId || !im.layouts.push_back(d)) {
      Problem("layout_table_full", "%u vertex layouts", unsigned(kNoTableId));
      return false;
    }
    return true;
  };
  if (impl_->store_on) {  // lane 276: the same content is the same layout (the prebuilt one), kept canonical
    const uint64_t hash = pstore::HashLayout(desc);
    const VertexLayoutDesc canon = pstore::Canonical(desc);
    auto found = impl_->layout_by_hash.find(hash);
    if (found != impl_->layout_by_hash.end()) {
      const VertexLayoutDesc& have = impl_->layouts[found->second];
      if (have.attribute_count == canon.attribute_count && std::memcmp(have.attributes, canon.attributes, sizeof(VertexAttribute) * canon.attribute_count) == 0 &&
          std::memcmp(have.strides, canon.strides, sizeof(canon.strides)) == 0 && std::memcmp(have.per_instance, canon.per_instance, sizeof(canon.per_instance)) == 0)
        return found->second;
      if (!PushLayout(*impl_, canon)) return kNoTableId;  // a 64-bit hash collision (never seen): its own id, unindexed
      return uint16_t(impl_->layouts.size() - 1);
    }
    if (!PushLayout(*impl_, canon)) return kNoTableId;
    impl_->layout_by_hash.emplace(hash, uint16_t(impl_->layouts.size() - 1));
    return uint16_t(impl_->layouts.size() - 1);
  }
  if (!PushLayout(*impl_, desc)) return kNoTableId;
  return uint16_t(impl_->layouts.size() - 1);
}

VkImageView Renderer::Impl::TargetView(ImageEntry& entry, uint32_t mip, uint32_t layer, uint32_t views) {
  const uint32_t key = views << 16 | mip << 8 | layer;
  for (const auto& view : entry.target_views) {
    if (view.first == key) return view.second;
  }
  VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  info.image = entry.image;
  info.viewType = views > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
  info.format = entry.format;
  info.subresourceRange = {entry.aspect, mip, 1, layer, views};
  VkImageView view = VK_NULL_HANDLE;
  vkCreateImageView(device.device(), &info, nullptr, &view);
  entry.target_views.push_back({key, view});
  return view;
}

VkImageView Renderer::Impl::SampledView(ImageEntry& entry, const TextureSlot& slot) {
  const bool plain = slot.view_format == Format::Undefined && !slot.swizzle[0] && !slot.swizzle[1] && !slot.swizzle[2] && !slot.swizzle[3];
  if (plain) return entry.sampled_view;
  const uint64_t key = uint64_t(slot.view_format) << 32 | uint64_t(slot.swizzle[0]) << 24 | uint64_t(slot.swizzle[1]) << 16 | uint64_t(slot.swizzle[2]) << 8 | slot.swizzle[3];
  for (const auto& view : entry.reinterpreted_views) {
    if (view.first == key) return view.second;
  }
  VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  info.image = entry.image;
  info.viewType = entry.desc.kind == ImageKind::Cube ? VK_IMAGE_VIEW_TYPE_CUBE : entry.desc.kind == ImageKind::Volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
  info.format = slot.view_format != Format::Undefined ? ToVk(slot.view_format, device.caps()) : entry.format;
  info.components = {VkComponentSwizzle(slot.swizzle[0]), VkComponentSwizzle(slot.swizzle[1]), VkComponentSwizzle(slot.swizzle[2]), VkComponentSwizzle(slot.swizzle[3])};
  info.subresourceRange = {VkImageAspectFlags(entry.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, entry.desc.mips, 0, entry.layers};
  VkImageView view = VK_NULL_HANDLE;
  vkCreateImageView(device.device(), &info, nullptr, &view);
  entry.reinterpreted_views.push_back({key, view});
  return view;
}


void Renderer::Impl::ApplyUploads(VkCommandBuffer cmd, FrameSlot& slot) {
  std::vector<ImageId> fresh;
  std::vector<ImageUpload> image_work;
  std::vector<BufferUpload> buffer_work;
  std::vector<std::pair<ImageId, uint8_t>> class_work;  // S102 R21-B
  {
    std::lock_guard<std::mutex> lock(upload_mutex);
    // S102 R21-B: the sealed batches first, in order (all of them for seal 0, else those up to this frame's seal), then (seal 0 only) what is pending.
    while (!sealed_batches.empty() && (upload_seal_now == 0 || sealed_batches.front().seal <= upload_seal_now)) {
      UploadBatch& b = sealed_batches.front();
      fresh.insert(fresh.end(), b.fresh.begin(), b.fresh.end());
      image_work.insert(image_work.end(), b.images.begin(), b.images.end());
      buffer_work.insert(buffer_work.end(), b.buffers.begin(), b.buffers.end());
      class_work.insert(class_work.end(), b.classes.begin(), b.classes.end());
      sealed_batches.pop_front();
    }
    if (upload_seal_now == 0) {
      fresh.insert(fresh.end(), new_images.begin(), new_images.end()), new_images.clear();
      image_work.insert(image_work.end(), image_uploads.begin(), image_uploads.end()), image_uploads.clear();
      buffer_work.insert(buffer_work.end(), buffer_uploads.begin(), buffer_uploads.end()), buffer_uploads.clear();
      class_work.insert(class_work.end(), class_changes.begin(), class_changes.end()), class_changes.clear();
    }
  }
  for (const auto& [image, cls] : class_work)  // S102 R21-B (hazard 3): the record thread is the only writer of a live image's class
    if (image != kNoImage && image < images.high_water()) images[image].gpu_class = cls;
  // Nothing is ever undefined: new images start cleared to transparent black (TEXTURES_FRAME C22, C32), unless this frame uploads every one of
  // their subresources. Block-compressed formats cannot be cleared; they always come with their upload (a file texture).
  std::unordered_map<ImageId, uint32_t> uploads_per_image;
  for (const ImageUpload& upload : image_work) ++uploads_per_image[upload.image];
  // S102 G5: the barrier count of this call starts here (the clears' barriers count too). The plant (rdr::g_upload_batch_plant, tests only): 1 = the batched groups' last flush is dropped,
  // 2 = a duplicate upload is injected and the duplicate fallback is off, 3 = the same duplicate with the fallback ON (the control: must stay clean).
  const uint64_t flushes_before = out_flushes;
  out_up_explicit = 0;
  const bool want_batch = rdr::g_upload_batch.load(std::memory_order_relaxed);
  const int batch_plant = rdr::g_upload_batch_plant.load(std::memory_order_relaxed);
  if (batch_plant >= 2) {
    for (const ImageUpload& upload : image_work) {
      if (!images[upload.image].live) continue;
      ImageUpload twin = upload;
      twin.injected = true;
      image_work.push_back(twin);
      break;
    }
  }
  // The images to clear (fresh and not fully uploaded). The default: each one a barrier, a clear and a barrier. With the batch switch: every change to copy-destination in ONE
  // barrier, then the clears (the changes to shader-read go out with the flush below).
  std::vector<ImageId> clear_list;
  for (ImageId id : fresh) {
    ImageEntry& entry = images[id];
    if (!entry.live || (entry.desc.usage & kUsageTransient)) continue;
    const bool compressed = entry.desc.format >= Format::BC1 && entry.desc.format <= Format::EAC_RG11;
    const bool fully_uploaded = uploads_per_image[id] >= uint32_t(entry.states.size());
    if ((entry.desc.usage & kUsageTarget) || (!compressed && !fully_uploaded)) clear_list.push_back(id);
  }
  bool clear_batch = want_batch && clear_list.size() > 1;
  if (clear_batch) {
    std::unordered_set<ImageId> listed;
    for (ImageId id : clear_list) clear_batch = listed.insert(id).second && clear_batch;
  }
  if (clear_batch) {
    for (ImageId id : clear_list) {
      ImageEntry& entry = images[id];
      Transition(entry, 0, uint32_t(entry.states.size()), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, true);
    }
    FlushBarriers(cmd);
  }
  for (ImageId id : clear_list) {
    ImageEntry& entry = images[id];
    const uint32_t all = uint32_t(entry.states.size());
    if (!clear_batch) {
      Transition(entry, 0, all, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, true);
      FlushBarriers(cmd);
    }
    VkImageSubresourceRange range{entry.aspect, 0, entry.desc.mips, 0, entry.layers};
    if (entry.depth) {
      const VkClearDepthStencilValue zero{0.0f, 0};
      vkCmdClearDepthStencilImage(cmd, entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    } else {
      const VkClearColorValue zero{};
      vkCmdClearColorImage(cmd, entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    }
    if (options.counts) ++counts_cur.total.clears;  // S102 M1
    // A sampled-only image is never transitioned at draw time: it waits in the layout shaders read (an upload below moves what it writes).
    if (!(entry.desc.usage & kUsageTarget))
      Transition(entry, 0, all, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, false);
    // Compressed, or fully uploaded this frame: left alone (not in the list). Each upload starts its subresource from undefined itself. A compressed image sampled
    // before its contents arrive is then a validation error by layout, which is the right signal (an adapter bug, never a picture to keep).
  }
  FlushBarriers(cmd);
  // S102 G5 (--wwe2k14_rdr_upload_batch): the uploads in three groups: every layout change to copy-destination in ONE barrier, the copies, every change back in ONE barrier (the
  // default is two barriers per upload). Never when one frame uploads the same subresource twice (the second copy must wait for the first), nor for a single upload.
  out_uploads = uint32_t(image_work.size() + buffer_work.size());
  out_up_images = uint32_t(image_work.size());   // S103 UPL
  // S103 UPL (review item 7): the batch is cut at the first upload of a subresource that an earlier upload of the same segment already wrote (the second copy must wait for the first), instead of dropping the batch for
  // the whole frame: each segment is batched on its own (3 groups: changes to copy-destination, copies, changes back). Plant 2 (tests only) keeps one segment through the duplicate.
  std::vector<std::pair<size_t, size_t>> segs;
  {
    size_t begin = 0;
    bool split = false;
    std::unordered_set<uint64_t> seen;
    if (want_batch && image_work.size() > 1) {
      for (size_t i = 0; i < image_work.size(); ++i) {
        const ImageEntry& entry = images[image_work[i].image];
        if (!entry.live) continue;
        const uint64_t key = (uint64_t(image_work[i].image) << 32) | (image_work[i].mip * entry.layers + image_work[i].layer);
        if (seen.insert(key).second || batch_plant == 2) continue;
        segs.push_back({begin, i});
        begin = i, seen.clear(), seen.insert(key);
        if (!split) {
          split = true;
          rdr::g_upload_dup_fallbacks.fetch_add(1, std::memory_order_relaxed);
          static int logged = 0;
          if (logged++ < 8) Log("[rdr][uploads] batch cut at a duplicate subresource: image %u (%ux%u, format %d) mip %u layer %u, upload %zu of %zu in this frame; each segment is batched on its own", unsigned(image_work[i].image),
                                unsigned(entry.desc.width), unsigned(entry.desc.height), int(entry.desc.format), unsigned(image_work[i].mip), unsigned(image_work[i].layer), i, image_work.size());
        }
      }
    }
    segs.push_back({begin, image_work.size()});
  }
  out_batched = false;
  for (const auto& sg : segs) out_batched = out_batched || (want_batch && sg.second - sg.first > 1);
  if (want_batch && image_work.size() > 1) rdr::g_upload_multi_frames.fetch_add(1, std::memory_order_relaxed);
  if (out_batched) rdr::g_upload_batched_frames.fetch_add(1, std::memory_order_relaxed);
  for (const auto& sg : segs) {
  const size_t seg_begin = sg.first, seg_end = sg.second;
  const bool batch = want_batch && seg_end - seg_begin > 1;
  if (batch) {
    for (size_t ui = seg_begin; ui < seg_end; ++ui) {
      const ImageUpload& upload = image_work[ui];
      ImageEntry& entry = images[upload.image];
      if (!entry.live) continue;
      Transition(entry, upload.mip * entry.layers + upload.layer, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, true);
    }
    FlushBarriers(cmd);
  }
  for (size_t ui = seg_begin; ui < seg_end; ++ui) {
    const ImageUpload& upload = image_work[ui];
    ImageEntry& entry = images[upload.image];
    if (!upload.injected) {
      out_up_bytes += upload.bytes;
      Retired retired;
      retired.buffer = upload.staging;
      retired.memory = upload.memory;
      retired.staged_bytes = upload.memory.size;
      retired.pool_class = upload.pool_class;   // S102 RT1: DrainRetired keeps it for the next upload (after this slot's fence)
      if (upload.pool_class && rdr::g_lean_plant.load(std::memory_order_relaxed) == 3) {   // PLANT 3 (tools only): handed back while its copy is still to run
        staging_pool.Give(upload.pool_class, PooledStaging{upload.staging, upload.memory});
        retired.buffer = VK_NULL_HANDLE, retired.memory = Allocation{}, retired.pool_class = 0;
      }
      slot.retired.push_back(retired);
      g_render_times.upload_bytes += upload.memory.size;
      if (options.counts) counts_cur.upload_bytes += upload.bytes;  // S102 M1 (copies are counted where they are recorded)
    }
    if (!entry.live) continue;
    const uint32_t sub = upload.mip * entry.layers + upload.layer;
    if (!batch) {
      Transition(entry, sub, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, true);
      FlushBarriers(cmd);
    }
    VkBufferImageCopy region{};
    region.imageSubresource = {VkImageAspectFlags(entry.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), upload.mip, upload.layer, 1};
    // A scaled image (the render scale) gets the game's contents at the game's own size (S100, the 1.5x check run 7022: a 1280x720 upload
    // copied as 1920x1080 read past its buffer): copied into a temporary image of that size, then blitted up (linear) onto the scaled one.
    const uint32_t guest_w = std::max(1u, uint32_t(entry.desc.width) >> upload.mip), guest_h = std::max(1u, uint32_t(entry.desc.height) >> upload.mip);
    const uint32_t scaled_w = std::max(1u, entry.width >> upload.mip), scaled_h = std::max(1u, entry.height >> upload.mip);
    if (!entry.depth && (guest_w != scaled_w || guest_h != scaled_h)) {
      ++out_scaled;  // S102 G5: a temporary image, its memory and a blit for this upload (render scale other than 1)
      VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      info.imageType = VK_IMAGE_TYPE_2D;
      info.format = entry.format;
      info.extent = {guest_w, guest_h, 1};
      info.mipLevels = 1;
      info.arrayLayers = 1;
      info.samples = VK_SAMPLE_COUNT_1_BIT;
      info.tiling = VK_IMAGE_TILING_OPTIMAL;
      info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      Retired temp;
      if (vkCreateImage(device.device(), &info, nullptr, &temp.image) == VK_SUCCESS) {
        VkMemoryRequirements needs{};
        vkGetImageMemoryRequirements(device.device(), temp.image, &needs);
        if (allocator.Alloc(needs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false, temp.memory)) {
          vkBindImageMemory(device.device(), temp.image, temp.memory.memory, temp.memory.offset);
          const VkImageSubresourceRange whole{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
          VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
          to_dst.srcAccessMask = 0, to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
          to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
          to_dst.srcQueueFamilyIndex = to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          to_dst.image = temp.image, to_dst.subresourceRange = whole;
          vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_dst);
          CountBarrier(1), ++out_up_explicit;  // S102 M1 (+ G5: counted where recorded)
          VkBufferImageCopy guest_region{};
          guest_region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          guest_region.imageExtent = {guest_w, guest_h, 1};
          vkCmdCopyBufferToImage(cmd, upload.staging, temp.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &guest_region);
          CountCopy();  // S102 M1
          VkImageMemoryBarrier to_src = to_dst;
          to_src.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
          to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
          vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_src);
          CountBarrier(1), ++out_up_explicit;  // S102 M1 (+ G5: counted where recorded)
          VkImageBlit blit{};
          blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          blit.srcOffsets[1] = {int32_t(guest_w), int32_t(guest_h), 1};
          blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, upload.mip, upload.layer, 1};
          blit.dstOffsets[1] = {int32_t(scaled_w), int32_t(scaled_h), 1};
          vkCmdBlitImage(cmd, temp.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, BlitFilterFor(entry.desc.format, true));
          CountCopy();  // S102 M1
        } else {
          Problem("scaled_upload_memory", "%ux%u", guest_w, guest_h);
        }
        slot.retired.push_back(temp);  // destroyed (with its memory) once this slot's GPU work is done
      }
    } else {
      region.imageExtent = {scaled_w, scaled_h, 1};
      vkCmdCopyBufferToImage(cmd, upload.staging, entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
      CountCopy();  // S102 M1
    }
    if (!batch) {
      Transition(entry, sub, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, false);
      FlushBarriers(cmd);
    }
  }
  if (batch) {
    for (size_t ui = seg_begin; ui < seg_end; ++ui) {
      const ImageUpload& upload = image_work[ui];
      ImageEntry& entry = images[upload.image];
      if (!entry.live) continue;
      Transition(entry, upload.mip * entry.layers + upload.layer, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, false);
    }
    if (batch_plant == 1) barriers.clear(), barrier_src = barrier_dst = 0;  // plant 1: the last flush is dropped (the recorded state says shader-read, the image stays in copy-destination)
    else FlushBarriers(cmd);
  }
  }
  if (!buffer_work.empty()) {
    // S103 PVR (docs/research/s103/UPF.md section 5): a stamp between the image part and the buffer part, so the [rdr][gpu][uploads] line reads them apart. Only in the stamp-per-step mode
    // (g_gpu_pass_stamps); counted as a post stamp (ts_w_post) so the identity of the stamps written still holds; the post stamp that follows ApplyUploads closes the buffer part.
    if (slot.ts_detail && rdr::g_gpu_pass_stamps.load(std::memory_order_relaxed) && !image_work.empty() && slot.ts_w_pre > 0 && slot.ts_count + 3 < slot.ts_cap) {
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.timestamps, slot.ts_count);
      slot.ts_up_mid = slot.ts_count++;
      ++slot.ts_w_post;
      slot.ts_class.push_back(kGpuOther);
      slot.ts_image.push_back(0), slot.ts_kind.push_back(7), slot.ts_src.push_back(255);
      if (slot.ts_trace) slot.ts_pass.push_back(0xFFFFFFFFu);
      ++slot.ts_need;
    }
    VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    before.srcAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
    CountBarrier(0), ++out_up_explicit;  // S102 M1 (+ G5)
    for (const BufferUpload& upload : buffer_work) {
      VkBufferCopy region{0, upload.offset, upload.bytes};
      vkCmdCopyBuffer(cmd, upload.staging, upload.dest, 1, &region);
      CountCopy();  // S102 M1
      ++out_up_by_producer[std::min<uint8_t>(upload.producer, 2)];   // S103 PVR
      if (options.counts) counts_cur.upload_bytes += upload.bytes;
      Retired retired;
      retired.buffer = upload.staging;
      retired.memory = upload.memory;
      retired.staged_bytes = upload.memory.size;
      slot.retired.push_back(retired);
    }
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    CountBarrier(0), ++out_up_explicit;  // S102 M1 (+ G5)
  }
  // S102 G5: the barriers this call recorded, counted where they are recorded: the flushes (FlushBarriers) and the explicit ones (a scaled upload's temp image, the buffer copies).
  out_up_barriers = uint32_t(out_flushes - flushes_before) + out_up_explicit;
}

// ---- caches -----------------------------------------------------------------------------------------------------------------------

VkRenderPass Renderer::Impl::GetRenderPass(const RenderPassKey& key) {
  auto found = render_passes.find(key);
  if (found != render_passes.end()) return found->second;
  VkAttachmentDescription attachments[kAttachmentSlots] = {};
  VkAttachmentReference color_refs[kMaxColorTargets] = {};
  VkAttachmentReference depth_ref{};
  uint32_t count = 0;
  for (uint32_t i = 0; i < key.color_count; ++i) {
    VkAttachmentDescription& a = attachments[count];
    a.format = VkFormat(key.formats[i]);
    a.samples = VkSampleCountFlagBits(key.samples);
    a.loadOp = VkAttachmentLoadOp(key.load[i]);
    a.storeOp = VkAttachmentStoreOp(key.store[i]);
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_refs[i] = {count, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    ++count;
  }
  if (key.has_depth) {
    VkAttachmentDescription& a = attachments[count];
    a.format = VkFormat(key.formats[kMaxColorTargets]);
    a.samples = VkSampleCountFlagBits(key.samples);
    a.loadOp = VkAttachmentLoadOp(key.load[kMaxColorTargets]);
    a.storeOp = VkAttachmentStoreOp(key.store[kMaxColorTargets]);
    a.stencilLoadOp = VkAttachmentLoadOp(key.stencil_load);
    a.stencilStoreOp = VkAttachmentStoreOp(key.stencil_store);
    a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth_ref = {count, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    ++count;
  }
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = key.color_count;
  subpass.pColorAttachments = color_refs;
  subpass.pDepthStencilAttachment = key.has_depth ? &depth_ref : nullptr;
  VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  info.attachmentCount = count;
  info.pAttachments = attachments;
  info.subpassCount = 1;
  info.pSubpasses = &subpass;
  const uint32_t view_mask = key.views > 1 ? ((1u << key.views) - 1u) & ~(g_layered_plant.load(std::memory_order_relaxed) == 1 ? 1u : 0u) : 0u;   // (plant 1: view 0 is not drawn)   // S102 C1: one pass into several layers (layered_targets.h); the same mask is the correlation mask (the views are one scene)
  VkRenderPassMultiviewCreateInfo multiview{VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
  multiview.subpassCount = 1;
  multiview.pViewMasks = &view_mask;
  multiview.correlationMaskCount = 1;
  multiview.pCorrelationMasks = &view_mask;
  if (key.views > 1) info.pNext = &multiview;
  VkRenderPass pass = VK_NULL_HANDLE;
  vkCreateRenderPass(device.device(), &info, nullptr, &pass);
  render_passes.emplace(key, pass);
  return pass;
}

uint32_t Renderer::Impl::GetPassClass(const PassClassKey& key) {
  auto found = pass_classes.find(key);
  if (found != pass_classes.end()) return found->second;
  RenderPassKey compatible{};
  std::memcpy(compatible.formats, key.formats, sizeof(key.formats));
  std::memset(compatible.load, VK_ATTACHMENT_LOAD_OP_LOAD, sizeof(compatible.load));
  std::memset(compatible.store, VK_ATTACHMENT_STORE_OP_STORE, sizeof(compatible.store));
  compatible.color_count = key.color_count;
  compatible.has_depth = key.has_depth;
  compatible.samples = key.samples;
  compatible.views = key.views;   // S102 C1
  const uint32_t id = uint32_t(class_passes.size());
  class_passes.push_back(GetRenderPass(compatible));
  class_keys.push_back(key);
  pass_classes.emplace(key, id);
  return id;
}

VkFramebuffer Renderer::Impl::GetFramebuffer(const FramebufferKey& key) {
  auto found = framebuffers.find(key);
  if (found != framebuffers.end()) return found->second;
  ++g_render_times.new_framebuffers;
  const uint64_t fb_t0 = RenderNow();
  struct FbTimer {
    uint64_t t0;
    ~FbTimer() { g_render_times.fb_ns += RenderNow() - t0; }
  } fb_timer{fb_t0};
  VkImageView views[kAttachmentSlots];
  uint32_t count = 0;
  for (VkImageView view : key.views) {
    if (view) views[count++] = view;
  }
  VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  info.renderPass = key.pass;
  info.attachmentCount = count;
  info.pAttachments = views;
  info.width = key.width;
  info.height = key.height;
  info.layers = 1;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  vkCreateFramebuffer(device.device(), &info, nullptr, &framebuffer);
  framebuffers.emplace(key, framebuffer);
  return framebuffer;
}

VkPipeline Renderer::Impl::BuildPipeline(const DrawState& state, uint32_t pass_class) {
  if (state.program >= programs.size() || state.vertex_layout >= layouts.size()) {  // S102 R21-B (review F5): kNoTableId from a full table (or any id never made) is refused by name
    Problem("pipeline_bad_ids", "program %u of %zu, layout %u of %zu", unsigned(state.program), programs.size(), unsigned(state.vertex_layout), layouts.size());
    return VK_NULL_HANDLE;
  }
  const Program& program = programs[state.program];
  const VertexLayoutDesc& layout = layouts[state.vertex_layout];
  const PassClassKey& pass_key = class_keys[pass_class];

  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = program.vertex;
  stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = program.fragment;
  stages[1].pName = "main";
  const bool depth_only_twin = state.variant == kVariantDepthOnly;  // S102 G10: the colour pass's depth-only prepass twin (colour_cuts.h): no fragment stage; the state has every write mask 0
  const uint32_t stage_count = program.fragment && !depth_only_twin ? 2 : 1;

  VkVertexInputBindingDescription bindings[2];
  uint32_t binding_count = 0;
  for (uint32_t i = 0; i < 2; ++i) {
    if (!layout.strides[i]) continue;
    bindings[binding_count++] = {i, layout.strides[i], layout.per_instance[i] ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
  }
  VkVertexInputAttributeDescription attributes[kMaxVertexAttributes];
  for (uint32_t i = 0; i < layout.attribute_count; ++i) {
    attributes[i] = {layout.attributes[i].location, layout.attributes[i].binding, VkFormat(layout.attributes[i].format), layout.attributes[i].offset};
    if (!device.VertexFormatReadable(attributes[i].format)) {  // lane 200: a vertex format the device cannot read: said once per pipeline
      if (FirstTime(2, state.program, (uint64_t(layout.attributes[i].location) << 16) ^ uint64_t(attributes[i].format))) {
        ++cap_stats.vertex_unsupported;
        Log("[rdr] format caps: vertex attribute %u of program %s has format %d, which this device cannot read as vertex input", layout.attributes[i].location, program.name ? program.name : "?", int(attributes[i].format));
        CapsChanged();
      }
    }
  }
  VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex_input.vertexBindingDescriptionCount = binding_count;
  vertex_input.pVertexBindingDescriptions = bindings;
  vertex_input.vertexAttributeDescriptionCount = layout.attribute_count;
  vertex_input.pVertexAttributeDescriptions = attributes;

  VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VkPrimitiveTopology(state.topology);
  const bool strip = assembly.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || assembly.topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || assembly.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
  assembly.primitiveRestartEnable = state.primitive_restart && strip;  // not allowed on lists

  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.depthClampEnable = state.depth_clamp && device.caps().depth_clamp;
  if (state.depth_clamp && !device.caps().depth_clamp) Log("[rdr] this device has no depth clamp: draws that rely on it may lose pixels at the depth limits");
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = state.cull;
  raster.frontFace = VkFrontFace(state.front_face);
  raster.depthBiasEnable = state.depth_bias_enable;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VkSampleCountFlagBits(pass_key.samples);

  const auto stencil_face = [](const StencilFace& face) {
    VkStencilOpState op{};
    op.failOp = VkStencilOp(face.fail);
    op.passOp = VkStencilOp(face.pass);
    op.depthFailOp = VkStencilOp(face.depth_fail);
    op.compareOp = VkCompareOp(face.compare);
    return op;
  };
  VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable = state.depth_test;
  depth.depthWriteEnable = state.depth_write;
  depth.depthCompareOp = VkCompareOp(state.depth_compare);
  depth.stencilTestEnable = state.stencil_test;
  depth.front = stencil_face(state.front);
  depth.back = stencil_face(state.back);

  VkPipelineColorBlendAttachmentState blend[kMaxColorTargets] = {};
  for (uint32_t i = 0; i < pass_key.color_count; ++i) {
    const BlendState& b = state.blend[i];
    blend[i].blendEnable = b.enable;
    // A blend that is a plain write (ONE, ZERO, ADD on colour and alpha) needs no blend feature: it is not touched and not counted.
    const bool plain_write = b.src_color == VK_BLEND_FACTOR_ONE && b.dst_color == VK_BLEND_FACTOR_ZERO && b.color_op == VK_BLEND_OP_ADD && b.src_alpha == VK_BLEND_FACTOR_ONE &&
                             b.dst_alpha == VK_BLEND_FACTOR_ZERO && b.alpha_op == VK_BLEND_OP_ADD;
    if (b.enable && !plain_write && pass_key.formats[i] != VK_FORMAT_UNDEFINED && !device.FormatHas(VkFormat(pass_key.formats[i]), VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)) {
      // Lane 200: blending into a format that cannot blend is invalid. The draw writes its colour over the target instead (logged by name).
      if (caps_fallback) {
        blend[i].blendEnable = VK_FALSE;
        if (FirstTime(3, state.program, uint64_t(pass_key.formats[i]))) {
          ++cap_stats.blend_disabled;
          Log("[rdr] format caps: blend off on attachment %u (%s) in program %s", i, CapFormatName(VkFormat(pass_key.formats[i])), program.name ? program.name : "?");
          CapsChanged();
        }
      } else {
        ++cap_stats.illegal_blend;
        CapsChanged();
      }
    }
    blend[i].srcColorBlendFactor = VkBlendFactor(b.src_color);
    blend[i].dstColorBlendFactor = VkBlendFactor(b.dst_color);
    blend[i].colorBlendOp = VkBlendOp(b.color_op);
    blend[i].srcAlphaBlendFactor = VkBlendFactor(b.src_alpha);
    blend[i].dstAlphaBlendFactor = VkBlendFactor(b.dst_alpha);
    blend[i].alphaBlendOp = VkBlendOp(b.alpha_op);
    blend[i].colorWriteMask = b.write_mask;
  }
  VkPipelineColorBlendStateCreateInfo color{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  color.attachmentCount = pass_key.color_count;
  color.pAttachments = blend;

  const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                           VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = uint32_t(std::size(dynamic_states));
  dynamic.pDynamicStates = dynamic_states;

  VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.stageCount = stage_count;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth;
  info.pColorBlendState = &color;
  info.pDynamicState = &dynamic;
  info.layout = pipeline_layout;
  info.renderPass = class_passes[pass_class];
  VkPipeline pipeline = VK_NULL_HANDLE;
  const auto create_start = std::chrono::steady_clock::now();   // lane 271: the line below says what each build cost (a scene start builds ~70 of them in a row)
  if (pipeline_delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(pipeline_delay_ms));   // test only (the release entrance check's slow-compile simulation), counted in the build time below
  if (vkCreateGraphicsPipelines(device.device(), pipeline_cache, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
    if (building_prebuilt) Log("[rdr][pipestore] a listed pipeline was refused by the driver: program %s, class %u (dropped from the list)", program.name ? program.name : "?", pass_class);
    else Problem("pipeline_failed", "program %s, layout %u", program.name ? program.name : "?", unsigned(state.vertex_layout));
    return VK_NULL_HANDLE;
  }
  last_build_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - create_start).count());  // lane 276
  const double build_ms = double(last_build_ns) / 1e6;
  if (building_prebuilt) Log("[rdr] pipeline prebuilt from the list: program %s, class %u, %.2f ms", program.name ? program.name : "?", pass_class, build_ms);
  else Log("[rdr] pipeline built on demand: program %s, class %u, %.2f ms", program.name ? program.name : "?", pass_class, build_ms);
  crumb_ring.NoteBuilt(serial, state.program, uint8_t(pass_class));  // S102 D1: the last pipelines built, named in a lost device's dump
  if (crumb) ++crumb->built;
  if (device.debug_names() && vk::vkSetDebugUtilsObjectNameEXT && program.name) {  // validation on: messages name the program
    VkDebugUtilsObjectNameInfoEXT name{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    name.objectType = VK_OBJECT_TYPE_PIPELINE;
    name.objectHandle = uint64_t(pipeline);
    name.pObjectName = program.name;
    vk::vkSetDebugUtilsObjectNameEXT(device.device(), &name);
  }
  return pipeline;
}

VkPipeline Renderer::Impl::GetPipeline(const DrawState& state, uint32_t pass_class) {
  PipelineKey key{};
  std::memcpy(&key.state, &state, sizeof(DrawState));
  key.pass_class = pass_class;
  auto found = pipelines.find(key);
  if (found != pipelines.end()) {
    if (found->second.unused_record >= 0) {  // lane 276: a prebuilt pipeline drawn for the first time this session
      store_records[size_t(found->second.unused_record)].seen = store_session;
      found->second.unused_record = -1;
      ++store_stats.prebuilt_used;
      ++store_used;
      store_list_dirty = true;
    }
    return found->second.pipeline;
  }
  last_build_ns = 0;
  VkPipeline pipeline = BuildPipeline(state, pass_class);
  out_pipe_ns += last_build_ns, out_pipe_n += pipeline ? 1 : 0;  // S102 G5
  pipelines.emplace(key, PipelineEntry{pipeline, -1});
  if (store_on && pipeline) StoreNoteBuilt(state, pass_class, int64_t(last_build_ns));  // (S102 G10 review F9: the prepass twins are listed too: pstore::Sane admits variant 0 / 1)
  return pipeline;
}

// ---- lane 276: the pipeline store (pipeline_store.h; SKIP_GAME_WORK_MAP 19.10) ----

void Renderer::Impl::StoreInit(const RendererOptions& opts, std::vector<uint8_t>& cache_blob) {
  if (opts.pipeline_store_dir.empty()) return;
  store_dir = pstore::PathOf(opts.pipeline_store_dir);
  std::error_code ec;
  std::filesystem::create_directories(store_dir, ec);
  if (ec) {
    Log("[rdr][pipestore] off: cannot make the folder %s", opts.pipeline_store_dir.c_str());
    return;
  }
  const VkPhysicalDeviceProperties& props = device.properties();
  store_device.vendor = props.vendorID, store_device.device = props.deviceID, store_device.driver = props.driverVersion;
  std::memcpy(store_device.uuid, props.pipelineCacheUUID, 16);
  store_on = true;
  store_slice_us = opts.pipeline_prebuild_slice_us;
  store_caps_mask = opts.format_caps_mask;
  store_save_interval_ms = opts.pipeline_save_interval_ms;
  // render-clock: ctl - save-interval stamp for the shader store file, no picture value
  store_saved_at = std::chrono::steady_clock::now();
  uint32_t list_session = 0;
  std::vector<uint8_t> file;
  if (pstore::ReadFile(store_dir / "pipelines.bin", file, 64ull << 20)) {
    uint32_t dropped = 0;
    const std::string why = pstore::ParseList(file, store_records, list_session, store_device, opts.format_caps_mask, dropped, opts.pipeline_store_plant == 1);
    if (!why.empty()) {
      Log("[rdr][pipestore] list refused: %s (the pipelines are built on demand; a new list is written)", why.c_str());
      store_records.clear();
      list_session = 0;
      store_list_dirty = true;
    } else {
      for (uint32_t i = 0; i < store_records.size(); ++i) {
        if (!store_index.emplace(pstore::RecordKey(store_records[i]), i).second) store_records[i].dead = true;  // a duplicate: dropped
        else store_pending.push_back(i);
      }
      store_stats.list_loaded = store_pending.size();
      store_stats.bad_dropped = dropped;
      if (dropped) store_list_dirty = true;
      Log("[rdr][pipestore] list loaded: %zu pipelines, session %u%s", store_pending.size(), list_session + 1, dropped ? " (some records failed the range checks and were dropped)" : "");
    }
  } else {
    Log("[rdr][pipestore] no list yet (first session on this store)");
  }
  if (!opts.pipeline_seed_path.empty()) {   // S106 PSEED: the shipped seed (R lines of shader_list.txt): records the player's list lacks join the pending ones
    std::vector<uint8_t> seed_file;
    if (pstore::ReadFile(pstore::PathOf(opts.pipeline_seed_path), seed_file, 64ull << 20)) {
      std::vector<pstore::Record> seed;
      // S106 PSEED fix B1: the seed's depth slot is a token, swapped here for THIS device's depth format; a record whose colour format the device cannot render to is dropped
      const pstore::SeedParse sp = pstore::ParseSeedLines(std::string(seed_file.begin(), seed_file.end()), seed, device.caps().multiview, uint16_t(device.caps().depth_format),
                                                          [this](uint16_t f) { return f != 0 && device.FormatHas(VkFormat(f), VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT); });
      const pstore::SeedMerge sm = pstore::MergeSeed(store_records, store_index, store_pending, seed);
      store_seed_added = sm.added;
      Log("[rdr][pipestore] seed: %u records, %u new, %u already in the list, %u refused%s%s, %u multiview skipped, %u colour format unsupported", sp.lines, sm.added, sm.known, sp.refused, sp.refused ? " (" : "", sp.refused ? (sp.first_refusal + ")").c_str() : "", sp.multiview, sp.unsupported);
    } else {
      Log("[rdr][pipestore] seed: %s not readable (no seed)", opts.pipeline_seed_path.c_str());
    }
  }
  store_session = list_session + 1;
  if (pstore::ReadFile(store_dir / "pipeline_cache.bin", file, pstore::kMaxCacheBytes + 64)) {
    const uint8_t* blob = nullptr;
    size_t bytes = 0;
    const std::string why = pstore::ParseCache(file, store_device, blob, bytes, opts.pipeline_store_plant == 2);
    if (!why.empty()) {
      store_cache_refusal = why;
      Log("[rdr][pipestore] cache refused: %s (starting empty)", why.c_str());
      store_cache_dirty = true;
    } else {
      cache_blob = std::move(file);  // (the file is not held twice: the wrapper header is cut off in place)
      cache_blob.erase(cache_blob.begin(), cache_blob.begin() + (cache_blob.size() - bytes));
      store_stats.cache_loaded_bytes = bytes;
      Log("[rdr][pipestore] cache loaded: %zu bytes (device %04X:%04X driver %08X)", bytes, store_device.vendor, store_device.device, store_device.driver);
    }
  } else {
    Log("[rdr][pipestore] no cache yet");
  }
  StorePublish();  // S105 B3: the list's records are counted from the start (a precompile never reads "0 of 0" with records waiting)
}

bool Renderer::Impl::StoreFindProgram(const ProgramDesc& desc, uint16_t& id) {
  hashing_program.vhash = pstore::HashWords(desc.vertex_spirv, desc.vertex_bytes);
  hashing_program.fhash = desc.fragment_spirv ? pstore::HashWords(desc.fragment_spirv, desc.fragment_bytes) : 0;
  auto found = program_by_hash.find(pstore::HashBytes(&hashing_program, sizeof(hashing_program)));
  if (found == program_by_hash.end()) return false;
  if (found->second >= program_info.size() || program_info[found->second].vhash != hashing_program.vhash || program_info[found->second].fhash != hashing_program.fhash) return false;  // a collision of the pair's hash: a new program
  id = found->second;
  return true;
}

void Renderer::Impl::StoreProgramAdded(uint16_t id) {
  if (program_info.size() <= id) program_info.resize(size_t(id) + 1);
  program_info[id] = hashing_program;
  program_by_hash.emplace(pstore::HashBytes(&hashing_program, sizeof(hashing_program)), id);
  store_retry = true;
}

void Renderer::Impl::StoreNoteBuilt(const DrawState& state, uint32_t pass_class, int64_t build_ns) {
  std::lock_guard<std::mutex> tables(tables_mutex);  // S102 R21-B (hazard 2): program_info is written by AddProgram on the bind stage
  ++store_stats.on_demand;
  store_stats.on_demand_ns += uint64_t(build_ns);
  ++store_used;
  const PassClassKey& ck = class_keys[pass_class];
  if (ck.pad != 0 || state.program >= program_info.size() || state.vertex_layout >= layouts.size()) return;  // the swapchain's own class is not kept
  pstore::Record r;
  r.state = state;
  r.state.program = 0;
  r.state.vertex_layout = 0;
  r.vhash = program_info[state.program].vhash;
  r.fhash = program_info[state.program].fhash;
  r.layout = pstore::Canonical(layouts[state.vertex_layout]);
  std::memcpy(r.pass.formats, ck.formats, sizeof(r.pass.formats));
  r.pass.color_count = ck.color_count, r.pass.has_depth = ck.has_depth, r.pass.samples = ck.samples, r.pass.pad = ck.views > 1 ? ck.views : 0;   // S102 C1: a multiview class keeps its view count in `pad` (pipeline_store.h Sane)
  r.seen = store_session;
  r.built = true;
  store_pub_dirty = true;   // S105 B3 F5: the counts changed (published at the next slice call)
  const char* name = programs[state.program].name;
  std::strncpy(r.name, name ? name : "", pstore::kNameBytes - 1);
  if (!pstore::Sane(r)) {  // would be dropped when loaded: not recorded, counted (the pipeline itself was built and works)
    if (++store_stats.bad_recorded == 1) Log("[rdr][pipestore] a pipeline failed the list's range checks and is not recorded (program %s)", r.name);
    return;
  }
  store_cache_dirty = true;
  const uint64_t key = pstore::RecordKey(r);
  auto found = store_index.find(key);
  if (found != store_index.end()) {  // the list already had it (its turn had not come): built now
    pstore::Record& old = store_records[found->second];
    old.built = true, old.seen = store_session;
    store_list_dirty = true;
    return;
  }
  if (store_records.size() >= pstore::kMaxRecords * 2 + store_seed_added) return;  // far past the cap: the save drops the oldest anyway
  store_index.emplace(key, uint32_t(store_records.size()));
  store_records.push_back(r);
  store_list_dirty = true;
}

// true: nothing more to do for this record (built, skipped or dropped). false: its program does not exist yet.
bool Renderer::Impl::PrebuildOne(uint32_t index) {
  pstore::Record& r = store_records[index];
  if (r.built || r.dead) return true;
  if (r.pass.pad > 1 && !device.caps().multiview) {   // S102 C1 review F9: a list from a device that had multiview: nothing to prebuild here (checked before any module is made)
    r.dead = true;
    return true;
  }
  uint16_t program = 0;
  ProgramInfo want{r.vhash, r.fhash};
  // S102 R21-B (hazard 2): the maps are read under tables_mutex, released before AddProgramChecked / AddVertexLayout (they take it) and the build.
  bool have_program = false;
  ShaderBlob vblob, fblob;
  {
    std::lock_guard<std::mutex> tables(tables_mutex);
    auto known = program_by_hash.find(pstore::HashBytes(&want, sizeof(want)));
    if (known != program_by_hash.end()) {
      program = known->second, have_program = true;
    } else {
      auto v = store_catalog.find(r.vhash);
      if (v == store_catalog.end()) return false;
      auto f = r.fhash ? store_catalog.find(r.fhash) : store_catalog.end();
      if (r.fhash && f == store_catalog.end()) return false;
      vblob = v->second;
      if (r.fhash) fblob = f->second;
    }
  }
  if (!have_program) {
    store_names.emplace_back(r.name);
    ProgramDesc desc;
    desc.name = store_names.back().c_str();
    desc.vertex_spirv = vblob.words, desc.vertex_bytes = vblob.bytes;
    if (r.fhash) desc.fragment_spirv = fblob.words, desc.fragment_bytes = fblob.bytes;
    if (!self->AddProgramChecked(desc, program)) {
      r.dead = true;
      ++store_stats.prebuilt_failed;
      return true;
    }
  }
  const uint16_t layout = self->AddVertexLayout(r.layout);
  PassClassKey ck{};
  std::memcpy(ck.formats, r.pass.formats, sizeof(ck.formats));
  ck.color_count = r.pass.color_count, ck.has_depth = r.pass.has_depth, ck.samples = r.pass.samples;
  ck.views = r.pass.pad;   // S102 C1
  const uint32_t pass_class = GetPassClass(ck);
  DrawState state = r.state;
  state.program = program;
  state.vertex_layout = layout;
  PipelineKey key{};
  std::memcpy(&key.state, &state, sizeof(DrawState));
  key.pass_class = pass_class;
  if (pipelines.count(key)) {  // asked for on demand before its turn
    r.built = true;
    return true;
  }
  building_prebuilt = true;
  last_build_ns = 0;
  VkPipeline pipeline = BuildPipeline(state, pass_class);
  building_prebuilt = false;
  if (!pipeline) {
    r.dead = true;
    ++store_stats.prebuilt_failed;
    return true;
  }
  pipelines.emplace(key, PipelineEntry{pipeline, int32_t(index)});
  r.built = true;
  ++store_stats.prebuilt;
  store_stats.prebuilt_ns += last_build_ns;
  store_cache_dirty = true;
  return true;
}

void Renderer::Impl::PrebuildSlice(uint64_t budget_ns, bool all) {
  if (!store_on) return;
  bool retry = false;
  {  // S102 R21-B (hazard 2): store_retry is set by AddProgram / RegisterShaderCatalog on the bind stage
    std::lock_guard<std::mutex> tables(tables_mutex);
    retry = store_retry;
    store_retry = false;
  }
  if (retry && !store_unresolved.empty()) {  // a program or the catalog arrived since: the waiting records get another try
    store_pending.insert(store_pending.end(), store_unresolved.begin(), store_unresolved.end());
    store_unresolved.clear();
  }
  if (store_cursor >= store_pending.size()) {
    if (store_pub_dirty || !store_pub_valid.load(std::memory_order_relaxed)) StorePublish();   // S105 B3 F5: nothing pending: publish only when a count moved (no walk of every record per menu frame)
    return;
  }
  const uint64_t t0 = RenderNow();
  const uint64_t built_before = store_stats.prebuilt;
  while (store_cursor < store_pending.size()) {
    const uint32_t index = store_pending[store_cursor++];
    if (!PrebuildOne(index)) store_unresolved.push_back(index);
    if (!all && store_stats.prebuilt > built_before && RenderNow() - t0 >= budget_ns) break;
  }
  if (store_cursor >= store_pending.size()) {
    store_pending.clear();
    store_cursor = 0;
  }
  store_stats.unresolved_peak = std::max<uint64_t>(store_stats.unresolved_peak, store_unresolved.size());
  const uint64_t spent = RenderNow() - t0;
  ++store_stats.slices;
  store_stats.slice_ns += spent;
  store_stats.slice_max_ns = std::max(store_stats.slice_max_ns, spent);
  StorePublish();
}

// S105 B3: the counts the precompile reads (made / total / left that can be tried / unresolved), computed on the render thread, read by atomics anywhere.
void Renderer::Impl::StorePublish() {
  uint64_t total = 0, made = 0;
  for (const pstore::Record& r : store_records) {
    if (r.dead) continue;
    ++total;
    if (r.built) ++made;
  }
  store_pub_total.store(total, std::memory_order_relaxed);
  store_pub_made.store(made, std::memory_order_relaxed);
  store_pub_left.store(store_pending.size() - store_cursor, std::memory_order_relaxed);
  store_pub_unresolved.store(store_unresolved.size(), std::memory_order_relaxed);
  store_pub_dirty = false;
  store_pub_valid.store(true, std::memory_order_release);
}

// The render thread builds the records to keep and copies the driver blob; the serialise + checksum + write run on ONE worker (at most one in flight; a save asked
// while it runs waits for the next check). `final` (shutdown, the quit flush) joins the worker and writes on this thread.
void Renderer::Impl::StoreSave(bool final) {
  if (!store_on) return;
  if (final) StoreJoin();
  else if (store_worker_busy.load(std::memory_order_acquire)) return;
  if (store_redo_list.exchange(false)) store_list_dirty = true;   // a write that failed is tried again
  if (store_redo_cache.exchange(false)) store_cache_dirty = true;
  if (!store_list_dirty && !store_cache_dirty) return;
  const uint64_t t0 = RenderNow();
  store_saved_at = std::chrono::steady_clock::now();
  std::vector<pstore::Record> keep;
  uint32_t written_session = 0;
  const bool do_list = store_list_dirty;
  if (do_list) {
    const bool substantial = store_used >= 20;  // a launch that only reached the menus ages nothing
    written_session = substantial ? store_session : store_session - 1;
    keep.reserve(store_records.size());
    for (pstore::Record r : store_records) {
      if (!pstore::SaveKeeps(r)) continue;   // S106 PSEED: dead, or a seed record nobody drew
      if (r.seen > written_session) r.seen = written_session;
      if (written_session - r.seen > pstore::kKeepSessions) continue;
      keep.push_back(r);
    }
    if (keep.size() > pstore::kMaxRecords) {  // the oldest go first, the order of the rest is kept
      std::vector<uint32_t> seen;
      for (const pstore::Record& r : keep) seen.push_back(r.seen);
      std::sort(seen.begin(), seen.end());
      const uint32_t cut = seen[keep.size() - pstore::kMaxRecords];
      std::vector<pstore::Record> trimmed;
      for (const pstore::Record& r : keep)
        if (r.seen >= cut && trimmed.size() < pstore::kMaxRecords) trimmed.push_back(r);
      keep.swap(trimmed);
    }
    store_list_dirty = false;
  }
  std::vector<uint8_t> blob;
  bool do_cache = false;
  if (store_cache_dirty && pipeline_cache) {
    VkDevice dev = device.device();
    size_t size = 0;
    if (vkGetPipelineCacheData(dev, pipeline_cache, &size, nullptr) == VK_SUCCESS && size && size <= pstore::kMaxCacheBytes) {
      blob.resize(size);
      if (vkGetPipelineCacheData(dev, pipeline_cache, &size, blob.data()) == VK_SUCCESS && size <= blob.size()) {
        blob.resize(size);
        do_cache = true;
        store_cache_dirty = false;
      } else {
        blob.clear();
      }
    }
  }
  const double copy_ms = double(RenderNow() - t0) / 1e6;
  auto job = [this, keep = std::move(keep), blob = std::move(blob), do_list, do_cache, written_session, final, copy_ms]() {
    const uint64_t w0 = RenderNow();
    if (do_list) {
      std::string why;   // S105 B3: which step failed and the OS error
      if (pstore::WriteFileAtomicWhy(store_dir / "pipelines.bin", pstore::SerialiseList(keep, written_session, store_device, store_caps_mask), why)) {
        store_saved_list.store(keep.size(), std::memory_order_relaxed);
      } else {
        store_redo_list.store(true);
        Log("[rdr][pipestore] could not write the list in %s: %s", store_dir.string().c_str(), why.c_str());
      }
    }
    if (do_cache) {
      std::string why;
      if (pstore::WriteFileAtomicWhy(store_dir / "pipeline_cache.bin", pstore::SerialiseCache(store_device, blob.data(), blob.size()), why)) {
        store_saved_cache.store(blob.size(), std::memory_order_relaxed);
      } else {
        store_redo_cache.store(true);
        Log("[rdr][pipestore] could not write the cache in %s: %s", store_dir.string().c_str(), why.c_str());
      }
    }
    wwe2k14::perf_sys::NoteFileWrite(double(RenderNow() - w0) / 1e6);  // lane 302: the [perf][sys] line's file_write_ms
    Log("[rdr][pipestore] saved%s: list %llu pipelines (session %u), cache %llu bytes; render thread %.2f ms (the blob copy), %s %.1f ms", final ? " at the end" : "",
        static_cast<unsigned long long>(store_saved_list.load()), store_session, static_cast<unsigned long long>(store_saved_cache.load()), copy_ms,
        final ? "serialise + write" : "worker", double(RenderNow() - w0) / 1e6);
  };
  if (final) {
    job();
  } else {
    store_worker_busy.store(true, std::memory_order_release);
    store_worker = std::thread([this, job = std::move(job)]() { wwe2k14::NameThread(L"rdr_store");
      job();
      store_worker_busy.store(false, std::memory_order_release);
    });
  }
}

void Renderer::Impl::StoreMaybeSave() {
  if (!store_on) return;
  if (!store_worker_busy.load(std::memory_order_acquire)) StoreJoin();  // (a finished worker is joined here, never waited for)
  if (!store_list_dirty && !store_cache_dirty && !store_redo_list.load() && !store_redo_cache.load()) return;
  // render-clock: ctl - save-interval gate for the shader store file, no picture value
  if (std::chrono::steady_clock::now() - store_saved_at < std::chrono::milliseconds(store_save_interval_ms)) return;
  if (store_stats.prebuilt + store_stats.on_demand == 0 && !store_list_dirty) return;
  StoreSave(false);
}

void Renderer::Impl::StoreSummary(const char* when) {
  if (!store_on) return;
  const StoreStats& s = store_stats;
  Log("[rdr][pipestore] %s: on-demand builds %llu (%.1f ms), prebuilt from the list %llu (%.1f ms, %llu drawn this session, %llu refused), waiting %zu (%zu unresolved), list loaded %llu, cache loaded %llu bytes%s%s, slices %llu (max %.2f ms)",
      when, static_cast<unsigned long long>(s.on_demand), double(s.on_demand_ns) / 1e6, static_cast<unsigned long long>(s.prebuilt), double(s.prebuilt_ns) / 1e6,
      static_cast<unsigned long long>(s.prebuilt_used), static_cast<unsigned long long>(s.prebuilt_failed), store_pending.size() - store_cursor + store_unresolved.size(), store_unresolved.size(),
      static_cast<unsigned long long>(s.list_loaded), static_cast<unsigned long long>(s.cache_loaded_bytes), store_cache_refusal.empty() ? "" : ", cache refused: ", store_cache_refusal.c_str(),
      static_cast<unsigned long long>(s.slices), double(s.slice_max_ns) / 1e6);
}

void Renderer::RegisterShaderCatalog(const ShaderBlobDesc* blobs, size_t count) {
  Impl& im = *impl_;
  if (!im.store_on) return;
  std::lock_guard<std::mutex> tables(im.tables_mutex);  // S102 R21-B (hazard 2): PrebuildOne reads the catalog on the record stage
  const uint64_t hash_t0 = RenderNow();   // S105 B3 F6: the hash of every blob runs on the boot thread
  for (size_t i = 0; i < count; ++i) {
    if (!blobs[i].words || !blobs[i].bytes) continue;
    Impl::ShaderBlob blob;
    blob.words = blobs[i].words, blob.bytes = blobs[i].bytes, blob.name = blobs[i].name;
    const auto made = im.store_catalog.emplace(pstore::HashWords(blobs[i].words, blobs[i].bytes), blob);
    if (!made.second && (made.first->second.bytes != blob.bytes || std::memcmp(made.first->second.words, blob.words, blob.bytes) != 0))
      Log("[rdr][pipestore] two different shaders share a hash (%s): the second is not in the catalog", blob.name ? blob.name : "?");  // never seen
  }
  im.store_retry = true;
  Log("[rdr][pipestore] shader catalog: %zu shaders (hashed in %.1f ms)", im.store_catalog.size(), double(RenderNow() - hash_t0) / 1e6);
}

bool Renderer::pipeline_store_active() const { return impl_->store_on; }

void Renderer::SetPipelinePrebuildAllowed(bool allowed) { impl_->prebuild_allowed = allowed; }

// S102 R21-B (STAGE_SPLIT.md hazard 4): close what is pending into a batch; the frame handed over with this id applies exactly the batches up to it.
uint64_t Renderer::SealUploads() {
  Impl& im = *impl_;
  std::lock_guard<std::mutex> lock(im.upload_mutex);
  const uint64_t seal = ++im.next_seal;
  if (!im.new_images.empty() || !im.image_uploads.empty() || !im.buffer_uploads.empty() || !im.class_changes.empty()) {
    Impl::UploadBatch batch;
    batch.seal = seal;
    batch.fresh.swap(im.new_images);
    batch.images.swap(im.image_uploads);
    batch.buffers.swap(im.buffer_uploads);
    batch.classes.swap(im.class_changes);
    im.sealed_batches.push_back(std::move(batch));
  }
  return seal;
}
void Renderer::SetRenderThread() { impl_->render_thread.store(std::hash<std::thread::id>()(std::this_thread::get_id()), std::memory_order_relaxed); }  // S102 R21-B (review F1)
void Renderer::SetProducerThread() { if (!g_plant_no_producer.load(std::memory_order_relaxed)) impl_->producer_thread.store(std::hash<std::thread::id>()(std::this_thread::get_id()), std::memory_order_relaxed); }  // S102 R21-B (hazard 6)

// The live game hard-exits (~Renderer never runs): the quit flush saves what is dirty, waits for a save in flight and prints the summary. Render thread; repeatable.
void Renderer::FlushPipelineStore() {
  Impl& im = *impl_;
  if (!im.store_on) return;
  im.StoreSave(true);
  im.StoreJoin();
  im.StoreSummary("at quit");
}

size_t Renderer::PrebuildPipelines(uint32_t max_ms) {
  Impl& im = *impl_;
  if (!im.store_on) return 0;
  const uint64_t t0 = RenderNow();
  im.PrebuildSlice(uint64_t(max_ms) * 1000000ull, max_ms == 0);
  Log("[rdr][pipestore] prebuild pass: %.1f ms", double(RenderNow() - t0) / 1e6);
  return im.store_pending.size() - im.store_cursor + im.store_unresolved.size();
}

void Renderer::SavePipelineStore() {
  impl_->store_list_dirty = impl_->store_cache_dirty = true;
  impl_->StoreSave(false);
}

Renderer::PipelineStoreCounts Renderer::pipeline_store_counts() const {
  const Impl::StoreStats& s = impl_->store_stats;
  PipelineStoreCounts c;
  c.on_demand = s.on_demand, c.prebuilt = s.prebuilt, c.prebuilt_used = s.prebuilt_used, c.on_demand_ns = s.on_demand_ns, c.prebuilt_ns = s.prebuilt_ns;
  c.list_records = impl_->store_records.size(), c.list_loaded = s.list_loaded;
  return c;
}

// S105 B3: only the atomics StorePublish wrote: safe from any thread (the host's precompile rule); the other counts read render-thread tables.
Renderer::PipelineStoreCounts Renderer::published_store_counts() const {
  PipelineStoreCounts c;
  c.valid = impl_->store_pub_valid.load(std::memory_order_acquire);
  c.total = impl_->store_pub_total.load(std::memory_order_relaxed), c.made = impl_->store_pub_made.load(std::memory_order_relaxed);
  c.left_resolvable = impl_->store_pub_left.load(std::memory_order_relaxed), c.unresolved = impl_->store_pub_unresolved.load(std::memory_order_relaxed);
  return c;
}

VkSampler Renderer::Impl::GetSampler(const SamplerDesc& desc) {
  auto found = samplers.find(desc);
  if (found != samplers.end()) return found->second;
  VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  info.magFilter = VkFilter(desc.mag_filter);
  info.minFilter = VkFilter(desc.min_filter);
  info.mipmapMode = VkSamplerMipmapMode(desc.mip_mode);
  // Mirror-clamp is not in the floor: mirrored repeat is the nearest thing every device has.
  const auto address = [](uint8_t mode) { return mode == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT : VkSamplerAddressMode(mode); };
  info.addressModeU = address(desc.address_u);
  info.addressModeV = address(desc.address_v);
  info.addressModeW = address(desc.address_w);
  info.mipLodBias = desc.lod_bias;
  info.anisotropyEnable = desc.max_anisotropy > 1 && device.caps().anisotropy;
  info.maxAnisotropy = std::min(float(desc.max_anisotropy), device.caps().max_anisotropy);
  info.minLod = desc.min_lod;
  info.maxLod = desc.max_lod;
  info.borderColor = VkBorderColor(desc.border);
  VkSampler sampler = VK_NULL_HANDLE;
  vkCreateSampler(device.device(), &info, nullptr, &sampler);
  samplers.emplace(desc, sampler);
  return sampler;
}

void Renderer::Impl::FillTextureInfos(const Frame& frame, uint32_t index, VkDescriptorImageInfo* infos) {   // S102 RT1: the infos loop of GetTextureSet, unchanged
  const TextureSet& textures = frame.texture_sets[index];
  for (uint32_t i = 0; i < kMaxTextures; ++i) {
    const TextureSlot& texture = textures.slots[i];
    ImageEntry* entry = texture.image != kNoImage ? &images[texture.image] : nullptr;
    if (entry && !entry->live) {
      Problem("texture_not_live", "slot %u image %u", i, unsigned(texture.image));
      entry = nullptr;
    }
    if (entry) {
      const uint8_t image_kind = entry->desc.kind == ImageKind::Cube ? 1 : entry->desc.kind == ImageKind::Volume ? 2 : 0;
      if (image_kind != texture.kind) {
        Problem("texture_kind_mismatch", "slot %u image %u is kind %u, the shader samples kind %u", i, unsigned(texture.image), unsigned(image_kind), unsigned(texture.kind));
        entry = nullptr;
      }
    }
    if (!entry) {
      // Empty slots read as transparent black through a view of the kind the shader expects.
      infos[i] = {dummy_sampler, images[DummyFor(texture.kind)].sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      continue;
    }
    SamplerDesc sampler = frame.samplers[texture.sampler];
    const Format read_format = texture.view_format != Format::Undefined ? texture.view_format : entry->desc.format;
    if (!filterable[uint32_t(read_format)]) {
      // Linear filtering of a format that cannot filter is undefined (TEXTURES_FRAME T29, T30). Lane 200 (GitHub #185): decided from the
      // device's format features; the values are untouched, only the smoothing between texels is lost.
      const bool wants_linear = sampler.mag_filter == VK_FILTER_LINEAR || sampler.min_filter == VK_FILTER_LINEAR || sampler.mip_mode == VK_SAMPLER_MIPMAP_MODE_LINEAR;
      if (caps_fallback) {
        cap_stats.linear_to_nearest += wants_linear;
        if (wants_linear) CapsChanged();
        sampler.mag_filter = sampler.min_filter = VK_FILTER_NEAREST;
        sampler.mip_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.max_anisotropy = 1;
      } else {
        cap_stats.illegal_linear += wants_linear;
        if (wants_linear) CapsChanged();
      }
    }
    infos[i].sampler = GetSampler(sampler);
    infos[i].imageView = SampledView(*entry, texture);
    infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
}

VkDescriptorSet Renderer::Impl::GetTextureSetLean(const Frame& frame, uint32_t index, FrameSlot& slot) {   // S102 RT1 (bit 1)
  VkDescriptorSet& set = frame_sets[index];
  VkDescriptorImageInfo infos[kMaxTextures];
  FillTextureInfos(frame, index, infos);
  rdr::lean::SetKey<size_t(kMaxTextures)> key;
  for (uint32_t i = 0; i < kMaxTextures; ++i) key.view[i] = reinterpret_cast<uint64_t>(infos[i].imageView), key.sampler[i] = reinterpret_cast<uint64_t>(infos[i].sampler);
  const uint64_t hash = key.Hash();
  bool collide = false;
  uint64_t prev_use = 0;
  if (auto* hit = lean_cache.Find(key, hash, serial, collide, &prev_use)) {
    set = hit->set;
    rdr::LeanAdd(rdr::kLcSetHit);
    if (prev_use != serial) rdr::LeanAdd(rdr::kLcSetHitOld);   // held from an EARLIER frame (the census the brief asks for); the rest were built earlier in this same frame
    return set;
  }
  ++g_render_times.texture_sets;
  if (options.counts) ++counts_cur.texture_sets;
  struct SetTimer {
    uint64_t t0 = RenderNow();
    ~SetTimer() { g_render_times.set_ns += RenderNow() - t0; }
  } set_timer;
  if (collide) rdr::LeanAdd(rdr::kLcSetCollide);
  if (!lean_pool && !lean_pool_made_fail) {
    if (const int c = rdr::g_lean_capacity.load(std::memory_order_relaxed); c > 0) lean_cache.set_capacity(size_t(c));   // tools only (rplay --lean-capacity)
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, uint32_t(lean_cache.capacity()) * kMaxTextures};
    VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    info.maxSets = uint32_t(lean_cache.capacity());
    info.poolSizeCount = 1;
    info.pPoolSizes = &size;
    if (vkCreateDescriptorPool(device.device(), &info, nullptr, &lean_pool) != VK_SUCCESS) lean_pool = VK_NULL_HANDLE, ++lean_pool_made_fail;
  }
  if (lean_pool && !collide) {
    if (lean_cache.size() >= lean_cache.capacity()) {  // full: sets idle for 600 frames go (they are retired into THIS slot: every frame that could name them is older than this slot's fence)
      lean_freed.clear();
      lean_cache.Sweep(serial, rdr::g_lean_sweep_age.load(std::memory_order_relaxed) ? uint64_t(rdr::g_lean_sweep_age.load(std::memory_order_relaxed)) : 600, lean_freed);
      for (VkDescriptorSet gone : lean_freed) {
        Retired retired;
        retired.lean_set = gone;
        slot.retired.push_back(retired);
      }
      rdr::LeanAdd(rdr::kLcSetEvict, lean_freed.size());
    }
    if (lean_cache.size() < lean_cache.capacity()) {
      VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      alloc.descriptorPool = lean_pool;
      alloc.descriptorSetCount = 1;
      alloc.pSetLayouts = &textures_layout;
      if (vkAllocateDescriptorSets(device.device(), &alloc, &set) == VK_SUCCESS) {
        WriteTextureSet(set, infos);
        lean_cache.Insert(key, hash, set, serial);
        rdr::LeanAdd(rdr::kLcSetMiss);
        return set;
      }
      set = VK_NULL_HANDLE;
    }
  }
  rdr::LeanAdd(rdr::kLcSetFull);  // no room (or a colliding key): this frame's own pool, as before
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = slot.descriptors;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &textures_layout;
  if (vkAllocateDescriptorSets(device.device(), &alloc, &set) != VK_SUCCESS) {
    Problem("descriptor_pool_empty", "more than %u texture sets in one frame", kMaxSetsPerFrame);
    return VK_NULL_HANDLE;
  }
  WriteTextureSet(set, infos);
  return set;
}

void Renderer::Impl::LeanCensus() {   // S102 RT1: every 600 frames while a bit is on
  using namespace rdr;
  Log("[rdr][render_lean] frames %llu, mask %d: sets kept %llu / built %llu / fallback %llu / collide %llu / evicted %llu (cache %zu of %zu); hooks: shadow skip %llu run %llu, wrestler skip %llu run %llu; hand-off: staging pool hit %llu miss %llu, ui record skip %llu run %llu, plane sort skip %llu run %llu, note flat %llu",
      static_cast<unsigned long long>(serial), g_render_lean.load(std::memory_order_relaxed) | g_lean_hooks.load(std::memory_order_relaxed), static_cast<unsigned long long>(LeanGet(kLcSetHit)), static_cast<unsigned long long>(LeanGet(kLcSetMiss)),
      static_cast<unsigned long long>(LeanGet(kLcSetFull)), static_cast<unsigned long long>(LeanGet(kLcSetCollide)), static_cast<unsigned long long>(LeanGet(kLcSetEvict)), lean_cache.size(), lean_cache.capacity(),
      static_cast<unsigned long long>(LeanGet(kLcShadowSkip)), static_cast<unsigned long long>(LeanGet(kLcShadowRun)), static_cast<unsigned long long>(LeanGet(kLcWrestlerSkip)), static_cast<unsigned long long>(LeanGet(kLcWrestlerRun)),
      static_cast<unsigned long long>(LeanGet(kLcStageHit)), static_cast<unsigned long long>(LeanGet(kLcStageMiss)), static_cast<unsigned long long>(LeanGet(kLcSetRecordSkip)), static_cast<unsigned long long>(LeanGet(kLcSetRecordRun)),
      static_cast<unsigned long long>(LeanGet(kLcSortSkip)), static_cast<unsigned long long>(LeanGet(kLcSortRun)), static_cast<unsigned long long>(LeanGet(kLcNoteFlat)));
}

VkDescriptorSet Renderer::Impl::GetTextureSet(const Frame& frame, uint32_t index, FrameSlot& slot) {
  VkDescriptorSet& set = frame_sets[index];
  if (set) return set;
  if (rdr::g_render_lean.load(std::memory_order_relaxed) & rdr::kLeanSets) return GetTextureSetLean(frame, index, slot);   // S102 RT1
  ++g_render_times.texture_sets;
  if (options.counts) ++counts_cur.texture_sets;  // S102 M1
  struct SetTimer {
    uint64_t t0 = RenderNow();
    ~SetTimer() { g_render_times.set_ns += RenderNow() - t0; }
  } set_timer;
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = slot.descriptors;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &textures_layout;
  if (vkAllocateDescriptorSets(device.device(), &alloc, &set) != VK_SUCCESS) {
    Problem("descriptor_pool_empty", "more than %u texture sets in one frame", kMaxSetsPerFrame);
    return VK_NULL_HANDLE;
  }
  VkDescriptorImageInfo infos[kMaxTextures];
  FillTextureInfos(frame, index, infos);
  WriteTextureSet(set, infos);
  return set;
}

void Renderer::Impl::WriteTextureSet(VkDescriptorSet set, const VkDescriptorImageInfo* infos) {
  VkWriteDescriptorSet writes[kMaxTextures];
  for (uint32_t b = 0; b < uint32_t(kMaxTextures); ++b) {
    writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[b].dstSet = set;
    writes[b].dstBinding = b;
    writes[b].descriptorCount = 1;
    writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[b].pImageInfo = infos + b;
  }
  vkUpdateDescriptorSets(device.device(), kMaxTextures, writes, 0, nullptr);
}

// ---- barriers ---------------------------------------------------------------------------------------------------------------------

const char* Renderer::Impl::LayeredTargetsProblem(const TargetsOp& t) {
  bool any = false;
  for (uint32_t c = 0; c < kMaxColorTargets; ++c) {
    const uint16_t pad = t.color[c].pad;
    if (pad == 0) continue;
    if (c >= t.color_count) return "a layered count on an unused colour slot";
    if (pad == 1 || pad > kMaxViews) return "a view count that is not 0 or 2..6";
    if (c != 0) return "only colour 0 may be layered";
    any = true;
  }
  if (t.depth.pad) return "a layered depth target";
  if (!any) return nullptr;
  if (t.color_count != 1) return "a layered colour beside another colour";
  if (t.depth.image != kNoImage) return "a layered colour beside a depth target";
  const ImageRef& ref = t.color[0];
  if (ref.image == kNoImage || ref.image >= images.high_water() || !images[ref.image].live) return "a layered target on a missing image";
  const ImageEntry& entry = images[ref.image];
  if (uint32_t(ref.layer) + ViewCount(ref) > entry.layers) return "layers past the end of the image";
  if (ref.mip >= entry.desc.mips || entry.desc.samples != 1) return "a layered target of a mip that is not there, or a multisampled one";
  return nullptr;
}

void Renderer::Impl::Transition(ImageEntry& entry, uint32_t first_sub, uint32_t sub_count, VkImageLayout layout, VkPipelineStageFlags stage, VkAccessFlags access, bool discard, bool ranged) {
  constexpr VkAccessFlags kWrites = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  if (ranged && sub_count > 1 && (first_sub / entry.layers) == ((first_sub + sub_count - 1) / entry.layers)) {   // S102 C1 review F9: one range barrier for the layers of a layered target when they agree
    bool same = true;
    const SubresourceState first = entry.states[first_sub];
    for (uint32_t sub = first_sub + 1; sub < first_sub + sub_count; ++sub) {
      const SubresourceState& s = entry.states[sub];
      same &= s.layout == first.layout && s.access == first.access && s.stage == first.stage;
    }
    if (same) {
      const bool layout_change = first.layout != layout;
      const bool hazard = (first.access & kWrites) || (access & kWrites);
      if (!layout_change && !hazard) {
        for (uint32_t sub = first_sub; sub < first_sub + sub_count; ++sub) entry.states[sub].stage |= stage;
        return;
      }
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.srcAccessMask = first.access;
      barrier.dstAccessMask = access;
      barrier.oldLayout = discard ? VK_IMAGE_LAYOUT_UNDEFINED : first.layout;
      barrier.newLayout = layout;
      barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = entry.image;
      barrier.subresourceRange = {entry.aspect, first_sub / entry.layers, 1, first_sub % entry.layers, sub_count};
      barriers.push_back(barrier);
      barrier_src |= first.stage;
      barrier_dst |= stage;
      for (uint32_t sub = first_sub; sub < first_sub + sub_count; ++sub) {
        SubresourceState& s = entry.states[sub];
        s.layout = layout, s.stage = stage, s.access = access;
      }
      return;
    }
  }
  for (uint32_t sub = first_sub; sub < first_sub + sub_count; ++sub) {
    SubresourceState& state = entry.states[sub];
    const bool layout_change = state.layout != layout;
    const bool hazard = (state.access & kWrites) || (access & kWrites);
    if (!layout_change && !hazard) {
      state.stage |= stage;  // another reader; a later writer must wait for this stage too
      continue;
    }
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = state.access;
    barrier.dstAccessMask = access;
    barrier.oldLayout = discard ? VK_IMAGE_LAYOUT_UNDEFINED : state.layout;
    barrier.newLayout = layout;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = entry.image;
    barrier.subresourceRange = {entry.aspect, sub / entry.layers, 1, sub % entry.layers, 1};
    barriers.push_back(barrier);
    barrier_src |= state.stage;
    barrier_dst |= stage;
    state.layout = layout;
    state.stage = stage;
    state.access = access;
  }
}

void Renderer::Impl::FlushBarriers(VkCommandBuffer cmd) {
  if (barriers.empty()) return;
  ++out_flushes;  // S102 G5
  vkCmdPipelineBarrier(cmd, barrier_src, barrier_dst, 0, 0, nullptr, 0, nullptr, uint32_t(barriers.size()), barriers.data());
  if (options.counts) ++counts_cur.total.barriers, counts_cur.total.barrier_images += uint32_t(barriers.size());  // S102 M1
  barriers.clear();
  barrier_src = barrier_dst = 0;
}

// ---- compile: the whole frame is known before a single Vulkan call ----------------------------------------------------------------

void Renderer::Impl::Compile(const Frame& frame) {
  passes.clear();
  steps.clear();
  sampled.clear();
  sd_on = g_stencil_dead.load(std::memory_order_relaxed);   // S102 R24-C: read every frame
  depth_only_copies.clear();
  if (sd_on) g_sd_frames.fetch_add(1, std::memory_order_relaxed);
  clear_absorbed.assign(frame.clears.count, 0);
  const uint32_t image_limit = images.high_water();
  // A frame's first part (split at a read-back copy): the rest of the frame reads everything after it.
  next_use.assign(image_limit, frame.keep_contents ? kNextRead : kNextNone);
  stencil_needed.assign(image_limit, frame.keep_contents ? 1 : 0);
  state_needed.assign(image_limit, 0);
  if (image_stamp.size() < image_limit) {
    image_stamp.resize(image_limit, 0);
    sampled_index.resize(image_limit, 0);
    written_stamp.resize(image_limit, 0);
  }
  const uint32_t frame_stamp = ++write_stamp;

  const TargetsOp* current = nullptr;
  int open = -1;  // index into passes
  bool pass_has_work = false;
  trimmed_targets.clear();
  const auto close = [&](uint32_t end_op) {
    if (open >= 0) {
      PassPlan& plan = passes[size_t(open)];
      plan.end_op = end_op;
      // S100: a depth surface the game left bound but no draw or clear of this pass touches is not attached. The 360 never minds a
      // smaller one (EDRAM is addressed by pitch); a Vulkan pass covers only what every attachment covers, so Bobby Heenan's 2048x1024
      // attire build drawn with the scene's 1280x720 depth bound came out cut at 1280x720. Its contents stay as they were either way.
      if (plan.targets->depth.image != kNoImage && plan.targets->color_count && !plan.depth_used && !plan.depth_cleared) {
        trimmed_targets.push_back(*plan.targets);
        trimmed_targets.back().depth = ImageRef{};
        plan.targets = &trimmed_targets.back();
        plan.width = plan.height = 0xFFFFFFFFu;
        for (uint32_t c = 0; c < plan.targets->color_count; ++c) {
          const ImageRef& ref = plan.targets->color[c];
          const ImageEntry& entry = images[ref.image];
          plan.width = std::min(plan.width, std::max(1u, entry.width >> ref.mip));
          plan.height = std::min(plan.height, std::max(1u, entry.height >> ref.mip));
        }
      }
    }
    open = -1;
    pass_has_work = false;
  };
  const auto open_pass = [&](uint32_t op_index) {
    PassPlan plan;
    plan.targets = current;
    plan.first_op = op_index;
    plan.first_sampled = uint32_t(sampled.size());
    for (uint32_t i = 0; i < kAttachmentSlots; ++i) {
      plan.load[i] = VK_ATTACHMENT_LOAD_OP_LOAD;
      plan.store[i] = VK_ATTACHMENT_STORE_OP_STORE;
    }
    // S102 C1 (review of G14: "LOAD makes the cube carry state"; C1 review F3): a layered pass (the pre-light cube bake) loads CLEAR to 0: the cube then depends on no earlier content (no state to keep alive across
    // frames, no read of the old layers), and a texel the draw does not cover (an edge the top-left rule leaves out, a tiler's undefined tile contents) is 0 as the old strip's was, never garbage. The offline sentinel
    // (g_layered_clear_sentinel) clears magenta: a pass that covers every texel gives the same cube either way.
    for (uint32_t c = 0; c < current->color_count; ++c)
      if (ViewCount(current->color[c]) > 1) {
        plan.load[c] = VK_ATTACHMENT_LOAD_OP_CLEAR;
        const bool sentinel = g_layered_clear_sentinel.load(std::memory_order_relaxed);
        plan.clear[c].color.float32[0] = sentinel ? 1.0f : 0.0f, plan.clear[c].color.float32[1] = 0.0f, plan.clear[c].color.float32[2] = sentinel ? 1.0f : 0.0f, plan.clear[c].color.float32[3] = sentinel ? 1.0f : 0.0f;
      }
    // The render area is what every attachment can cover.
    plan.width = plan.height = 0xFFFFFFFFu;
    const auto limit = [&](const ImageRef& ref) {
      const ImageEntry& entry = images[ref.image];
      plan.width = std::min(plan.width, std::max(1u, entry.width >> ref.mip));
      plan.height = std::min(plan.height, std::max(1u, entry.height >> ref.mip));
    };
    for (uint32_t c = 0; c < current->color_count; ++c) limit(current->color[c]);
    if (current->depth.image != kNoImage) limit(current->depth);
    passes.push_back(plan);
    open = int(passes.size()) - 1;
    steps.push_back({StepKind::Pass, uint32_t(open), false});
    ++stamp;
  };

  for (uint32_t i = 0; i < frame.ops.count; ++i) {
    const Op& op = frame.ops[i];
    switch (op.kind) {
      case OpKind::Targets: {
        const TargetsOp* next = &frame.targets[op.index];
        if (const char* why = LayeredTargetsProblem(*next)) {   // S102 C1 review F8: a malformed layered target (layers past the image, beside a depth or another colour, pad garbage) is drawn as ONE view, counted
          Problem("layered_target_bad", "op %u: %s (drawn as an ordinary target)", i, why);
          trimmed_targets.push_back(*next);
          TargetsOp& fixed = trimmed_targets.back();
          for (uint32_t c = 0; c < kMaxColorTargets; ++c) fixed.color[c].pad = 0;
          fixed.depth.pad = 0;
          next = &fixed;
        }
        if (open >= 0 && std::memcmp(next, current, sizeof(TargetsOp)) != 0) close(i);
        current = next;
        break;
      }
      case OpKind::Clear: {
        if (!current) {
          Problem("clear_without_targets", "op %u", i);
          break;
        }
        if (open < 0) open_pass(i);
        const ClearOp& clear = frame.clears[op.index];
        PassPlan& plan = passes[size_t(open)];
        if (g_depth_census_on.load(std::memory_order_relaxed) && current->depth.image != kNoImage && (clear.flags & kClearStencil)) {   // S102 R24-C: the stencil census (fview / rplay only)
          DepthCensusSlot& cs = DepthCensusFor(current->depth.image);
          cs.stencil_clears.fetch_add(1, std::memory_order_relaxed);
          if (clear.stencil != 0) cs.stencil_clears_nonzero.fetch_add(1, std::memory_order_relaxed);
        }
        if (clear.full && !pass_has_work) {
          // A full clear before anything else in the pass becomes the attachment's load op.
          clear_absorbed[op.index] = 1;
          if (clear.flags & kClearColor) {
            for (uint32_t c = 0; c < current->color_count; ++c) {
              if (!(clear.color_mask & (1u << c))) continue;
              plan.load[c] = VK_ATTACHMENT_LOAD_OP_CLEAR;
              std::memcpy(plan.clear[c].color.float32, clear.color, sizeof(clear.color));
            }
          }
          if (current->depth.image != kNoImage) {
            if (clear.flags & kClearDepth) {
              plan.load[kMaxColorTargets] = VK_ATTACHMENT_LOAD_OP_CLEAR;
              plan.clear[kMaxColorTargets].depthStencil.depth = clear.depth;
              plan.depth_cleared = true;
            }
            if (clear.flags & kClearStencil) {
              plan.stencil_load = VK_ATTACHMENT_LOAD_OP_CLEAR;
              plan.clear[kMaxColorTargets].depthStencil.stencil = clear.stencil;
              plan.depth_cleared = true;
            }
          }
        } else {
          pass_has_work = true;
          if (clear.flags & kClearStencil) plan.stencil_clear_in = true;
          if ((clear.flags & kClearStencil) && !sd_on) plan.stencil_used = true;   // S102 R24-C: with stencil_dead a clear is a write, not a use (the pass's load / store and the clear itself die when nothing reads)
          if (clear.flags & (kClearDepth | kClearStencil)) plan.depth_used = true;
        }
        break;
      }
      case OpKind::Draw: {
        if (!current) {
          Problem("draw_without_targets", "op %u", i);
          break;
        }
        if (open < 0) open_pass(i);
        pass_has_work = true;
        const DrawOp& draw = frame.draws[op.index];
        const TextureSet& textures = frame.texture_sets[draw.textures];
        PassPlan& plan = passes[size_t(open)];
        const DrawState& draw_state = frame.states[draw.state];
        if (draw_state.stencil_test) plan.stencil_used = true, plan.stencil_tests = true;
        if (g_depth_census_on.load(std::memory_order_relaxed) && current->depth.image != kNoImage) {   // S102 R24-C: the stencil census (fview / rplay only)
          DepthCensusSlot& cs = DepthCensusFor(current->depth.image);
          const ImageEntry& de = images[current->depth.image];
          cs.width.store(de.desc.width, std::memory_order_relaxed), cs.height.store(de.desc.height, std::memory_order_relaxed);
          cs.draws.fetch_add(1, std::memory_order_relaxed);
          if (draw_state.stencil_test) {
            cs.stencil_test_draws.fetch_add(1, std::memory_order_relaxed);
            const StencilFaceView fv{draw_state.front.fail, draw_state.front.pass, draw_state.front.depth_fail, draw_state.front.compare};
            const StencilFaceView bv{draw_state.back.fail, draw_state.back.pass, draw_state.back.depth_fail, draw_state.back.compare};
            if (DrawNeedsStencil(true, fv, bv)) {
              cs.stencil_effect_draws.fetch_add(1, std::memory_order_relaxed);
              if (FaceWrites(fv) || FaceWrites(bv)) cs.stencil_write_draws.fetch_add(1, std::memory_order_relaxed);
              const uint8_t cls = current->color_count && current->color[0].image != kNoImage ? std::atomic_ref<uint8_t>(images[current->color[0].image].gpu_class).load(std::memory_order_relaxed) : uint8_t(31);
              cs.use_classes.fetch_or(1u << cls, std::memory_order_relaxed);
            }
          }
        }
        if (draw_state.depth_test || draw_state.depth_write || draw_state.stencil_test) plan.depth_used = true;
        for (const TextureSlot& texture : textures.slots) {
          if (texture.image == kNoImage) continue;
          ImageEntry& entry = images[texture.image];
          if (!(entry.desc.usage & kUsageTarget) && !entry.copy_dest) continue;
          if (image_stamp[texture.image] == stamp) {
            sampled[sampled_index[texture.image]].vertex |= texture.vertex != 0;
            continue;
          }
          image_stamp[texture.image] = stamp;
          sampled_index[texture.image] = uint32_t(sampled.size());
          sampled.push_back({texture.image, texture.vertex != 0});
          ++plan.sampled_count;
        }
        break;
      }
      case OpKind::Copy:
        if (const CopyOp& cp = frame.copies[op.index]; (cp.source.image != kNoImage && (cp.source.image >= image_limit || !images[cp.source.image].live)) ||
                                                        (cp.dest.image != kNoImage && (cp.dest.image >= image_limit || !images[cp.dest.image].live))) {
          // S102 R21-B: a copy naming a destroyed image (a release that did not wait for the frames that use it) is dropped whole and named, never recorded
          // with a null VkImage (a driver crash). It must never happen; the stage split's planted twin split_no_release_defer makes it happen.
          Problem("copy_dead_image", "op %u: copy img%u -> img%u names an image that is not there", i, unsigned(cp.source.image), unsigned(cp.dest.image));
          break;
        }
        close(i);
        images[frame.copies[op.index].dest.image].copy_dest = true;
        steps.push_back({StepKind::Copy, op.index, false});
        break;
      case OpKind::Present:
        close(i);
        steps.push_back({StepKind::Present, op.index, false});
        break;
      case OpKind::Dispatch: {   // S102 L1b: a compute dispatch is a step of its own: it ends the open pass (the next draw opens a new one), and the cubes it writes are checked for layout when sampled
        const DispatchOp& d = frame.dispatches[op.index];
        bool bad = d.cube_count == 0 || d.cube_count > uint32_t(kMaxLightCubes);   // review F5: a dispatch naming a missing image is dropped whole, never indexed
        for (uint32_t k = 0; k < d.cube_count && k < uint32_t(kMaxLightCubes) && !bad; ++k) bad = d.cube[k] == kNoImage || d.cube[k] >= image_limit || !images[d.cube[k]].live;
        if (bad) {
          Problem("light_cube_bad", "op %u: a dispatch of %u cubes names an image that is not there", i, d.cube_count);
          break;
        }
        close(i);
        for (uint32_t k = 0; k < d.cube_count; ++k) images[d.cube[k]].copy_dest = true;
        steps.push_back({StepKind::Dispatch, op.index, false});
        break;
      }
    }
  }
  close(frame.ops.count);

  // Walk backwards: what happens next to each image decides what must be kept.
  const auto single = [&](const ImageEntry& entry) { return entry.desc.mips == 1 && entry.layers == 1; };
  for (size_t s = steps.size(); s-- > 0;) {
    const Step& step = steps[s];
    if (step.kind == StepKind::Pass) {
      PassPlan& plan = passes[step.index];
      const auto decide = [&](ImageId id, uint32_t slot_index) {
        const ImageEntry& entry = images[id];
        const uint8_t next = next_use[id];
        const bool dead = (entry.desc.usage & kUsageTransient) || (next == kNextOverwrite && single(entry)) || (next == kNextNone && (entry.desc.usage & kUsageDeadAtFrameEnd));
        plan.store[slot_index] = dead ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
        if ((entry.desc.usage & kUsageTransient) && plan.load[slot_index] == VK_ATTACHMENT_LOAD_OP_LOAD) {
          Problem("transient_needs_contents", "a transient target is loaded instead of cleared (image %u)", unsigned(id));
        }
      };
      for (uint32_t c = 0; c < plan.targets->color_count; ++c) decide(plan.targets->color[c].image, c);
      const ImageId depth = plan.targets->depth.image;
      if (depth != kNoImage) {
        decide(depth, kMaxColorTargets);
        // Stencil is rare: it is kept only if a later pass really uses it.
        plan.stencil_after = stencil_needed[depth] != 0;   // S102 R24-C
        const bool had_abs_clear = plan.stencil_load == VK_ATTACHMENT_LOAD_OP_CLEAR;   // the game's own absorbed stencil clear (the old rule kept it as a CLEAR load, not a LOAD)
        const bool poison = sd_on && g_stencil_dead_poison.load(std::memory_order_relaxed) && (images[depth].aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
        if (sd_on && plan.stencil_clear_in) {   // (b): counted here, once per pass (not while recording: a Persistent + Display frame records a pass twice)
          plan.strip_stencil_clear = !plan.stencil_used && (!plan.stencil_after || g_stencil_dead_plant.load(std::memory_order_relaxed) == 3);
          (plan.strip_stencil_clear ? g_sd_in_stripped : g_sd_in_kept).fetch_add(1, std::memory_order_relaxed);
        }
        if (sd_on && plan.stencil_load == VK_ATTACHMENT_LOAD_OP_CLEAR) {   // S102 R24-C (a): an absorbed stencil clear nothing reads is not done
          const bool wrong = g_stencil_dead_plant.load(std::memory_order_relaxed) == 1;   // plant 1: dropped even before a stencil test
          if (wrong || (!plan.stencil_used && !plan.stencil_after)) {
            plan.stencil_load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            if (poison) plan.stencil_load = VK_ATTACHMENT_LOAD_OP_CLEAR, plan.clear[kMaxColorTargets].depthStencil.stencil = 0xA5, plan.sd_poisoned_load = true;   // poison: the stencil the clear would have made is garbage
            g_sd_abs_dropped.fetch_add(1, std::memory_order_relaxed);
            if (wrong) stencil_needed[depth] = 0;
          } else {
            g_sd_abs_kept.fetch_add(1, std::memory_order_relaxed);
          }
        }
        const bool cleared = plan.stencil_load == VK_ATTACHMENT_LOAD_OP_CLEAR;
        const bool needed_before = !cleared && (plan.stencil_used || stencil_needed[depth]);
        plan.stencil_store = stencil_needed[depth] ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        if (!cleared) plan.stencil_load = needed_before ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        if (sd_on && !had_abs_clear && !cleared && plan.stencil_clear_in && !needed_before) {   // the load the old rule kept (the in-pass clear counted as a use) and this one does not
          g_sd_loads_dead.fetch_add(1, std::memory_order_relaxed);
          if (poison) plan.stencil_load = VK_ATTACHMENT_LOAD_OP_CLEAR, plan.clear[kMaxColorTargets].depthStencil.stencil = 0xA5, plan.sd_poisoned_load = true;
        }
        stencil_needed[depth] = single(images[depth]) ? needed_before : (needed_before || stencil_needed[depth]);   // S102 R24-C review F5: a pass writes one mip / layer; only a 1-mip 1-layer image's need ends here
      }
      for (uint32_t c = 0; c < plan.targets->color_count; ++c) next_use[plan.targets->color[c].image] = plan.load[c] == VK_ATTACHMENT_LOAD_OP_CLEAR ? kNextOverwrite : kNextRead;
      if (depth != kNoImage) next_use[depth] = plan.load[kMaxColorTargets] == VK_ATTACHMENT_LOAD_OP_CLEAR ? kNextOverwrite : kNextRead;
      for (uint32_t k = 0; k < plan.sampled_count; ++k) next_use[sampled[plan.first_sampled + k].image] = kNextRead;
    } else if (step.kind == StepKind::Copy) {
      const CopyOp& copy = frame.copies[step.index];
      const ImageEntry& dest = images[copy.dest.image];
      const bool whole = copy.dest_rect.x == 0 && copy.dest_rect.y == 0 && copy.dest_rect.width >= dest.desc.width && copy.dest_rect.height >= dest.desc.height;
      if (dest.depth && images[copy.source.image].depth) {   // a depth copy (both aspects when the switch is off) carries the stencil: what reads the destination's stencil later needs the source's
        const bool keep = stencil_needed[copy.dest.image] != 0;
        if (sd_on && (g_stencil_dead_plant.load(std::memory_order_relaxed) == 2 || !keep)) {   // S102 R24-C (c): with stencil_dead it takes the depth aspect only when nothing reads the destination's stencil
          depth_only_copies.push_back(&copy);
          g_sd_copy_depth_only.fetch_add(1, std::memory_order_relaxed);
        } else {
          if (sd_on) g_sd_copy_whole.fetch_add(1, std::memory_order_relaxed);
          if (keep) {   // S102 R24-C review F2: also with the switch OFF (the pre-existing walk dropped the source's need: its store was DONT_CARE)
            if (whole && single(dest)) stencil_needed[copy.dest.image] = 0;   // the copy writes the destination's whole stencil: nothing before it matters there
            if (g_stencil_dead_plant.load(std::memory_order_relaxed) != 4) stencil_needed[copy.source.image] = 1;   // ... but the source's stencil is what it carries (plant 4: not marked)
          }
        }
      }
      next_use[copy.dest.image] = whole && single(dest) ? kNextOverwrite : kNextRead;
      next_use[copy.source.image] = kNextRead;
    } else if (step.kind == StepKind::Dispatch) {   // S102 L1b: the dispatch writes every texel of every cube it names
      const DispatchOp& d = frame.dispatches[step.index];
      for (uint32_t k = 0; k < d.cube_count && k < uint32_t(kMaxLightCubes); ++k) next_use[d.cube[k]] = kNextOverwrite;
    } else {
      next_use[frame.presents[step.index].source.image] = kNextRead;
    }
  }

  // S102 R24-C: the forward oracle (census runs only: fview / rplay --depth-census): replay the frame's steps with a "stencil is defined" bit per depth image and count the passes that TEST stencil
  // while it is not (the lever, or the walk before it, left it undefined). A defined stencil is: a CLEAR load op, a LOAD of a defined one, a clear inside the pass that is issued, or a copy
  // that carries both aspects from a defined source. The poison arm's CLEAR 0xA5 stands for "undefined" here (sd_poisoned_load).
  if (g_depth_census_on.load(std::memory_order_relaxed)) {
    std::vector<uint8_t> defined(image_limit, frame.keep_contents ? 1 : 0);
    for (const Step& step : steps) {
      if (step.kind == StepKind::Pass) {
        const PassPlan& p = passes[step.index];
        const ImageId d = p.targets ? p.targets->depth.image : kNoImage;
        if (d == kNoImage || !(images[d].aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) continue;
        const uint8_t load = p.sd_poisoned_load ? uint8_t(VK_ATTACHMENT_LOAD_OP_DONT_CARE) : p.stencil_load;
        bool def = load == VK_ATTACHMENT_LOAD_OP_CLEAR ? true : (load == VK_ATTACHMENT_LOAD_OP_LOAD ? defined[d] != 0 : false);
        if (p.stencil_clear_in && !p.strip_stencil_clear) def = true;
        if (p.stencil_tests) {
          g_sd_oracle_tests.fetch_add(1, std::memory_order_relaxed);
          if (!def) g_sd_oracle_violations.fetch_add(1, std::memory_order_relaxed);
        }
        defined[d] = p.stencil_store == VK_ATTACHMENT_STORE_OP_STORE ? (def ? 1 : 0) : 0;
      } else if (step.kind == StepKind::Copy) {
        const CopyOp& copy = frame.copies[step.index];
        const ImageEntry& dest = images[copy.dest.image];
        if (!dest.depth || !images[copy.source.image].depth) continue;
        const bool whole = copy.dest_rect.x == 0 && copy.dest_rect.y == 0 && copy.dest_rect.width >= dest.desc.width && copy.dest_rect.height >= dest.desc.height;
        const bool depth_only = std::find(depth_only_copies.begin(), depth_only_copies.end(), &copy) != depth_only_copies.end();
        if (depth_only) defined[copy.dest.image] = defined[copy.dest.image];   // (poison: the destination's stencil becomes garbage, but nothing reads it by the lever's rule)
        else if (whole && dest.desc.mips == 1 && dest.layers == 1) defined[copy.dest.image] = defined[copy.source.image];
        else defined[copy.dest.image] = (defined[copy.dest.image] && defined[copy.source.image]) ? 1 : 0;
      }
    }
  }

  // Which images carry state from frame to frame: read before anything in this frame wrote them. Once seen, always.
  const auto read = [&](ImageId id) {
    if (written_stamp[id] != frame_stamp) images[id].carries_state = true;
    images[id].ever_read = true;
  };
  for (const Step& step : steps) {
    if (step.kind == StepKind::Pass) {
      const PassPlan& plan = passes[step.index];
      for (uint32_t k = 0; k < plan.sampled_count; ++k) read(sampled[plan.first_sampled + k].image);
      for (uint32_t c = 0; c < plan.targets->color_count; ++c) {
        if (plan.load[c] == VK_ATTACHMENT_LOAD_OP_LOAD) read(plan.targets->color[c].image);
        written_stamp[plan.targets->color[c].image] = frame_stamp;
      }
      if (plan.targets->depth.image != kNoImage) {
        if (plan.load[kMaxColorTargets] == VK_ATTACHMENT_LOAD_OP_LOAD) read(plan.targets->depth.image);
        written_stamp[plan.targets->depth.image] = frame_stamp;
      }
    } else if (step.kind == StepKind::Copy) {
      const CopyOp& copy = frame.copies[step.index];
      read(copy.source.image);
      written_stamp[copy.dest.image] = frame_stamp;
    } else if (step.kind == StepKind::Dispatch) {   // S102 L1b
      const DispatchOp& d = frame.dispatches[step.index];
      for (uint32_t k = 0; k < d.cube_count && k < uint32_t(kMaxLightCubes); ++k) written_stamp[d.cube[k]] = frame_stamp;
    }
  }

  // Persistent work runs exactly once per game frame, in order, even when the frame is never shown: anything writing an image that carries state, and
  // what that work reads this frame. Passes on the screen-sized images are display work: dropping them leaves at worst a one-frame-old copy of the picture.
  uint32_t screen_width = 0, screen_height = 0;
  if (frame.presents.count) {
    const ImageEntry& screen = images[frame.presents[frame.presents.count - 1].source.image];
    screen_width = screen.desc.width;
    screen_height = screen.desc.height;
  }
  const auto screen_sized = [&](const ImageEntry& entry) { return entry.desc.width == screen_width && entry.desc.height == screen_height; };
  uint32_t persistent_count = 0;
  for (size_t s = steps.size(); s-- > 0;) {
    Step& step = steps[s];
    if (step.kind == StepKind::Pass) {
      const PassPlan& plan = passes[step.index];
      const ImageId depth = plan.targets->depth.image;
      bool on_screen = false, carries = false;
      const auto look = [&](ImageId id) {
        const ImageEntry& entry = images[id];
        on_screen |= screen_sized(entry);
        carries |= entry.carries_state || state_needed[id];
      };
      for (uint32_t c = 0; c < plan.targets->color_count; ++c) look(plan.targets->color[c].image);
      if (depth != kNoImage) look(depth);
      step.persistent = carries && !on_screen;
      if (step.persistent) {
        for (uint32_t k = 0; k < plan.sampled_count; ++k) state_needed[sampled[plan.first_sampled + k].image] = 1;
        for (uint32_t c = 0; c < plan.targets->color_count; ++c) {
          if (plan.load[c] == VK_ATTACHMENT_LOAD_OP_LOAD) state_needed[plan.targets->color[c].image] = 1;
        }
        if (depth != kNoImage && plan.load[kMaxColorTargets] == VK_ATTACHMENT_LOAD_OP_LOAD) state_needed[depth] = 1;
      }
    } else if (step.kind == StepKind::Copy) {
      const CopyOp& copy = frame.copies[step.index];
      const ImageEntry& dest = images[copy.dest.image];
      // S100: also a copy into an image no frame has read yet (an attire packed during a load is first drawn many frames later; the
      // atlas copies of its build): until something reads it, whether it lasts is unknown, so it is kept.
      // S101 names: a destination whose role is declared lasting is kept, whatever has read it (the "ever read" guess stays for the rest).
      // Keeps the !screen_sized(source) guard (S101 review 6: a screen-sized source's pass is not persistent, so its copy would be stale).
      step.persistent = !screen_sized(images[copy.source.image]) &&
                        ((dest.desc.usage & kUsageLasting) || dest.carries_state || state_needed[copy.dest.image] || !dest.ever_read);
      if (step.persistent) state_needed[copy.source.image] = 1;
    } else if (step.kind == StepKind::Dispatch) {   // S102 L1b: as a pass into the same images: it must run once per game frame when a cube carries state from frame to frame (a frame without a bake still reads the last bake)
      const DispatchOp& d = frame.dispatches[step.index];
      bool carries = false;
      // review F1: the copies' own rule (a copy into a lasting destination, one that carries state, one no frame has read yet is persistent; the cubes inherit kUsageLasting from their surface role): the dispatch
      // writes the same destinations the copies did, so it runs once per game frame in a frame that is never shown, and not again in a redraw (DisplayOnly). Plant 4 = the first version's rule.
      const bool old_rule = g_light_cube_plant.load(std::memory_order_relaxed) == 4;
      for (uint32_t k = 0; k < d.cube_count && k < uint32_t(kMaxLightCubes); ++k) {
        const ImageEntry& c = images[d.cube[k]];
        carries |= c.carries_state || state_needed[d.cube[k]] || (!old_rule && ((c.desc.usage & kUsageLasting) || !c.ever_read));
      }
      step.persistent = carries;
    }
    if (step.persistent) ++persistent_count;
  }
#ifdef RDR_DEV
  if (persistent_count != last_persistent_count) {
    last_persistent_count = persistent_count;
    Log("[rdr] frame %llu: %zu steps, %u persistent", static_cast<unsigned long long>(frame.tick), steps.size(), persistent_count);
  }
#endif
}

// ---- record -----------------------------------------------------------------------------------------------------------------------

void Renderer::Impl::RecordPass(VkCommandBuffer cmd, const Frame& frame, const PassPlan& pass, FrameSlot& slot, VkDeviceSize ring_base) {
  const TargetsOp& targets = *pass.targets;
  // S102 M1: this pass's counts (integer increments; the area bound is computed only when counting is on). Barriers are read as the change in the frame total.
  const bool counting = options.counts;
  CallCounts pc;
  const uint32_t barriers_before = counts_cur.total.barriers, barrier_images_before = counts_cur.total.barrier_images;
  const DrawOp* last_draw = nullptr;  // for the planted extra draw
  const bool plant_here = counts_plant >= 0 && uint32_t(&pass - passes.data()) == uint32_t(counts_plant);
  bool plant_done = false;
  uint64_t dyn_area_now = 0;  // the area bound of the last recorded draw (the planted draw repeats it)
  uint32_t plant_op = ~0u;  // the pass's last draw op: the planted draw follows it at once, in the state it left bound
  if (plant_here)
    for (uint32_t i = pass.end_op; i > pass.first_op; --i)
      if (frame.ops[i - 1].kind == OpKind::Draw) { plant_op = i - 1; break; }
  const auto plant_draw = [&](const DrawOp& d) {  // S102 M1 test plant (rplay --counts-plant N): one extra draw, the repeat of the draw before it. The picture is not meant to be judged.
    plant_done = true;
    if (d.index.buffer != kNoBuffer) vkCmdDrawIndexed(cmd, d.count, d.instances, d.first, d.base_vertex, 0), ++pc.indexed_draws, pc.indices += uint64_t(d.count) * d.instances;
    else vkCmdDraw(cmd, d.count, d.instances, d.first, 0), ++pc.draws, pc.vertices += uint64_t(d.count) * d.instances;
    pc.area_px += dyn_area_now;
  };
  uint32_t pass_w = pass.width, pass_h = pass.height;
  // Layouts first: attachments, then anything this pass samples.
  for (uint32_t c = 0; c < targets.color_count; ++c) {
    ImageEntry& entry = images[targets.color[c].image];
    const uint32_t sub = targets.color[c].mip * entry.layers + targets.color[c].layer;
    Transition(entry, sub, g_layered_plant.load(std::memory_order_relaxed) == 2 ? 1u : ViewCount(targets.color[c]) /* S102 C1: a layered target's layers (plant 2: the first only) */, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               pass.load[c] != VK_ATTACHMENT_LOAD_OP_LOAD, true /* S102 C1: ranged */);
  }
  if (targets.depth.image != kNoImage) {
    ImageEntry& entry = images[targets.depth.image];
    const uint32_t sub = targets.depth.mip * entry.layers + targets.depth.layer;
    const bool discard = pass.load[kMaxColorTargets] != VK_ATTACHMENT_LOAD_OP_LOAD && pass.stencil_load != VK_ATTACHMENT_LOAD_OP_LOAD;
    Transition(entry, sub, 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, discard);
  }
  for (uint32_t k = 0; k < pass.sampled_count; ++k) {
    const SampledImage& item = sampled[pass.first_sampled + k];
    bool is_attachment = item.image == targets.depth.image;
    for (uint32_t c = 0; c < targets.color_count; ++c) is_attachment |= item.image == targets.color[c].image;
    if (is_attachment) {
      Problem("sampled_while_attached", "image %u is both a target and a texture of one pass", unsigned(item.image));
      continue;
    }
    ImageEntry& entry = images[item.image];
    const VkPipelineStageFlags stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | (item.vertex ? VK_PIPELINE_STAGE_VERTEX_SHADER_BIT : 0);
    Transition(entry, 0, uint32_t(entry.states.size()), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, stages, VK_ACCESS_SHADER_READ_BIT, false);
  }
  FlushBarriers(cmd);

  RenderPassKey pass_key{};
  PassClassKey class_key{};
  FramebufferKey framebuffer_key{};
  uint32_t samples = 1;
  for (uint32_t c = 0; c < targets.color_count; ++c) {
    ImageEntry& entry = images[targets.color[c].image];
    pass_key.formats[c] = class_key.formats[c] = uint16_t(entry.format);
    pass_key.load[c] = pass.load[c];
    pass_key.store[c] = pass.store[c];
    framebuffer_key.views[c] = TargetView(entry, targets.color[c].mip, targets.color[c].layer, ViewCount(targets.color[c]));   // S102 C1
    samples = entry.desc.samples;
    pass_key.views = class_key.views = uint8_t(ViewCount(targets.color[c]) > 1 ? ViewCount(targets.color[c]) : 0);
  }
  if (targets.depth.image != kNoImage) {
    ImageEntry& entry = images[targets.depth.image];
    pass_key.formats[kMaxColorTargets] = class_key.formats[kMaxColorTargets] = uint16_t(entry.format);
    pass_key.load[kMaxColorTargets] = pass.load[kMaxColorTargets];
    pass_key.store[kMaxColorTargets] = pass.store[kMaxColorTargets];
    pass_key.stencil_load = pass.stencil_load;
    pass_key.stencil_store = pass.stencil_store;
    framebuffer_key.views[kMaxColorTargets] = TargetView(entry, targets.depth.mip, targets.depth.layer);
    samples = entry.desc.samples;
  }
  pass_key.color_count = class_key.color_count = uint8_t(targets.color_count);
  pass_key.has_depth = class_key.has_depth = targets.depth.image != kNoImage;
  pass_key.samples = class_key.samples = uint8_t(samples);
  const uint32_t pass_class = GetPassClass(class_key);
  // S102 C4 (half_trans.h): the tail of additive no-depth-write draws that ends a scene colour pass is drawn at half size (after this pass, below). Decided here, before the pass begins: the planner needs the
  // images and pipelines to exist (PrepareHalf), else the tail stays in this pass. The first part stores colour and depth (the half passes read the depth, the composite adds onto the colour).
  uint32_t tail_begin = pass.end_op;
  std::vector<uint8_t> half_skip;  // S102 C4: per op of the pass, 1 = a half-size draw (set only when the tail is taken)
  HalfPrep half_prep;
  if (frame.half_transparents && !g_draw_mod_mode.load(std::memory_order_relaxed) && targets.color_count == 1 && targets.depth.image != kNoImage && images[targets.color[0].image].gpu_class == kGpuColor &&
      images[targets.color[0].image].desc.samples == 1 && images[targets.depth.image].desc.samples == 1 && images[targets.depth.image].sampled_view != VK_NULL_HANDLE) {
    const uint64_t half_n = ++half_trans::g_frames;
    const int half_plant = half_trans::g_plant.load(std::memory_order_relaxed);
    std::vector<half_trans::OpIn> hin;
    hin.reserve(pass.end_op - pass.first_op);
    for (uint32_t i = pass.first_op; i < pass.end_op; ++i) {
      half_trans::OpIn in;
      const Op& hop = frame.ops[i];
      if (hop.kind == OpKind::Draw) {
        const DrawOp& hd = frame.draws[hop.index];
        const DrawState& st = frame.states[hd.state];
        half_trans::DrawRule rule;
        rule.color_count = uint8_t(targets.color_count), rule.blend_enable = st.blend[0].enable != 0;
        rule.src_color = st.blend[0].src_color, rule.dst_color = st.blend[0].dst_color, rule.color_op = st.blend[0].color_op;
        rule.src_alpha = st.blend[0].src_alpha, rule.dst_alpha = st.blend[0].dst_alpha, rule.alpha_op = st.blend[0].alpha_op, rule.write_mask = st.blend[0].write_mask;
        rule.depth_test = st.depth_test != 0, rule.depth_write = st.depth_write != 0, rule.depth_compare = st.depth_compare, rule.stencil_test = st.stencil_test != 0;
        rule.shader_ok = st.program < programs.size() && programs[st.program].half_ok;
        const half_trans::Reason why = half_trans::Classify(rule, half_plant);
        ++half_trans::g_reasons[why];
        in.is_draw = true, in.kind = why == half_trans::kTaken ? half_trans::kTake : (half_plant != 2 && half_trans::CanHoist(rule)) ? half_trans::kHoist : half_trans::kOther;
      }
      hin.push_back(in);
    }
    const half_trans::Plan hplan = half_trans::PlanTail(hin);
    if (hplan.draws == 0) ++half_trans::g_declined_short;
    else if (!PrepareHalf(pass, slot, half_prep)) ++half_trans::g_declined_target;
    else {
      tail_begin = pass.first_op + hplan.begin;
      half_skip = hplan.flags;  // per op of the pass: 1 = drawn at half size (the main loop leaves it out)
      ++half_trans::g_passes_taken, half_trans::g_draws_halved += hplan.draws;
      pass_key.store[0] = VK_ATTACHMENT_STORE_OP_STORE;
      pass_key.store[kMaxColorTargets] = VK_ATTACHMENT_STORE_OP_STORE;
    }
    if (census_flush::Due(g_cf_half) || half_n % 600 == 0)  // S102 C4: the live census line: a Surface "no gain" is told apart from "took 0 frames"
      Log("[rdr][half_transparents] colour passes seen %llu, taken %llu, declined %llu (no tail of 4 or more %llu, target %llu), draws halved %llu; draws refused by reason: colour targets %llu, not additive %llu, alpha changes %llu, depth state %llu, stencil %llu, shader %llu",
          static_cast<unsigned long long>(half_n), static_cast<unsigned long long>(half_trans::g_passes_taken.load()), static_cast<unsigned long long>(half_trans::g_declined_short.load() + half_trans::g_declined_target.load()),
          static_cast<unsigned long long>(half_trans::g_declined_short.load()), static_cast<unsigned long long>(half_trans::g_declined_target.load()), static_cast<unsigned long long>(half_trans::g_draws_halved.load()),
          static_cast<unsigned long long>(half_trans::g_reasons[1].load()), static_cast<unsigned long long>(half_trans::g_reasons[2].load()), static_cast<unsigned long long>(half_trans::g_reasons[3].load()),
          static_cast<unsigned long long>(half_trans::g_reasons[4].load()), static_cast<unsigned long long>(half_trans::g_reasons[5].load()), static_cast<unsigned long long>(half_trans::g_reasons[6].load()));
  }
  framebuffer_key.pass = GetRenderPass(pass_key);
  framebuffer_key.width = pass.width;
  framebuffer_key.height = pass.height;

  VkClearValue clear_values[kAttachmentSlots];
  uint32_t clear_count = 0;
  bool any_clear = false;
  uint32_t load_clear_mask = 0;  // S102 M1: bit c = colour attachment c's load op is CLEAR
  bool load_clear_depth = false;
  for (uint32_t c = 0; c < targets.color_count; ++c) {
    any_clear |= pass.load[c] == VK_ATTACHMENT_LOAD_OP_CLEAR;
    if (pass.load[c] == VK_ATTACHMENT_LOAD_OP_CLEAR) load_clear_mask |= 1u << c;
    clear_values[clear_count++] = pass.clear[c];
  }
  if (targets.depth.image != kNoImage) {
    load_clear_depth = pass.load[kMaxColorTargets] == VK_ATTACHMENT_LOAD_OP_CLEAR || pass.stencil_load == VK_ATTACHMENT_LOAD_OP_CLEAR;
    any_clear |= load_clear_depth;
    clear_values[clear_count++] = pass.clear[kMaxColorTargets];
  }
  if (!any_clear) clear_count = 0;

  VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  begin.renderPass = framebuffer_key.pass;
  begin.framebuffer = GetFramebuffer(framebuffer_key);
  begin.renderArea.extent = {pass.width, pass.height};
  begin.clearValueCount = clear_count;
  begin.pClearValues = clear_values;
  vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
  pc.clears += LoadClearCount(load_clear_mask, load_clear_depth);  // S102 M1: one per attachment whose load op is CLEAR
  // S102 G11 (offline census, rplay --frame-trace FILE): one line per frame-class pass and per draw / clear in it (numbers only). Nothing is recorded differently.
  std::FILE* trace = nullptr;
  const uint32_t trace_pass = uint32_t(&pass - passes.data());
  if (images[(targets.color_count ? targets.color[0] : targets.depth).image].gpu_class == kGpuFrame) trace = rdr::g_frame_trace.load(std::memory_order_relaxed);
  if (trace)
    std::fprintf(trace, "{\"t\":\"p\",\"f\":%llu,\"p\":%u,\"img\":%u,\"w\":%u,\"h\":%u,\"colors\":%u,\"depth\":%d,\"load\":[%d,%d,%d,%d,%d],\"store\":[%d,%d,%d,%d,%d],\"first_op\":%u,\"end_op\":%u,\"sampled\":%u}\n",
                 static_cast<unsigned long long>(serial), trace_pass, unsigned((targets.color_count ? targets.color[0] : targets.depth).image), pass.width, pass.height, targets.color_count, targets.depth.image != kNoImage ? 1 : 0,
                 int(pass.load[0]), int(pass.load[1]), int(pass.load[2]), int(pass.load[3]), int(pass.load[kMaxColorTargets]), int(pass.store[0]), int(pass.store[1]), int(pass.store[2]), int(pass.store[3]), int(pass.store[kMaxColorTargets]), pass.first_op, pass.end_op, pass.sampled_count);

  const ImageRef& first_target = targets.color_count ? targets.color[0] : targets.depth;
  const bool layered_pass = targets.color_count && ViewCount(targets.color[0]) > 1;   // S102 C1 review F6: a multiview pass makes a query count once per view: no in-pass queries there
  const bool scaled = images[first_target.image].desc.usage & kUsageScaled;
  // S105 A1 review F1: the factor is per axis = the pass image's REAL size / its logical size (exactly `scale` at integer scales; at 0.6667 the image is ceil'd to 854 wide and the viewport must reach it: 853.33 left a column undrawn)
  const float factor_x = scaled ? float(images[first_target.image].width) / float(std::max<uint32_t>(1u, images[first_target.image].desc.width)) : 1.0f;
  const float factor_y = scaled ? float(images[first_target.image].height) / float(std::max<uint32_t>(1u, images[first_target.image].desc.height)) : 1.0f;
  uint64_t dyn_area = 0;  // S102 M1: the current dynamic block's viewport x scissor x pass rectangle, in pixels

  // S102 G10 (colour cut 1, colour_cuts.h): a depth-only prepass of the eligible opaque arena draws, FIRST in the pass; their normal draws then test LESS_OR_EQUAL (below), so the fragment shader
  // runs for the visible surface only. It declines (counted) when a depth clear follows the pass's first draw. Nothing in the pass is reordered.
  const int cut_plant = colour_cuts::g_colour_cuts_plant.load(std::memory_order_relaxed);
  std::vector<uint8_t> cut_eligible;
  bool cut_on = false;
  if ((colour_cuts_ & colour_cuts::kPrepass) && targets.depth.image != kNoImage && images[first_target.image].gpu_class == kGpuColor && !g_draw_mod_mode.load(std::memory_order_relaxed)) {
    cut_eligible.assign(pass.end_op - pass.first_op, 0);
    // Review F8: the inputs for the pure planner (colour_cuts.h PlanPass), one per draw op in order
    std::vector<colour_cuts::DrawIn> plan_in;
    std::vector<uint32_t> plan_op;
    bool depth_clear = false;
    for (uint32_t i = pass.first_op; i < pass.end_op; ++i) {
      const Op& op = frame.ops[i];
      if (op.kind == OpKind::Clear) {
        const ClearOp& clear = frame.clears[op.index];
        if (!clear_absorbed[op.index] && (clear.flags & (kClearDepth | kClearStencil))) depth_clear = true;  // review F7: any depth clear the load op did not absorb
        continue;
      }
      if (op.kind != OpKind::Draw) continue;
      const DrawOp& draw = frame.draws[op.index];
      const DrawState& st = frame.states[draw.state];
      const Program& program = programs[st.program];
      colour_cuts::DrawIn in;
      colour_cuts::DepthRule& rule = in.rule;
      rule.depth_test = st.depth_test != 0, rule.depth_write = st.depth_write != 0, rule.stencil_test = st.stencil_test != 0, rule.depth_compare = st.depth_compare;
      rule.blend_kind = 0;
      for (uint32_t c = 0; c < targets.color_count; ++c) {
        const BlendState& b = st.blend[c];
        rule.blend_kind = std::max<uint8_t>(rule.blend_kind, colour_cuts::BlendKind(b.enable != 0, b.src_color, b.dst_color, b.color_op, b.src_alpha, b.dst_alpha, b.alpha_op, b.write_mask));
        if (b.write_mask) rule.colour_written = true;
      }
      rule.alpha_benign = colour_cuts::AlphaBenign(frame.dynamics[draw.dynamic].system[8], frame.dynamics[draw.dynamic].system[9]);
      in.shader_class = program.prepass_class;
      in.named = colour_cuts::NamedByMask(colour_cuts_, (draw.flags & kDrawOpAlphaOpaque) != 0, cut_plant);  // review F2: the binder's proof, not a name
      if (cut_plant == 7 || cut_plant == 8) {  // diagnosis only (C4 review F4): 7 = only the vertex-lit family (name has "vertex_lm_vertex"), 8 = everything but it
        const bool vlm = program.name && std::strstr(program.name, "vertex_lm_vertex") != nullptr;
        in.named = in.named && (cut_plant == 7 ? vlm : !vlm);
      }
      auto writes = [](const StencilFace& f) { return f.fail != 0 || f.pass != 0 || f.depth_fail != 0; };  // VK_STENCIL_OP_KEEP == 0
      in.stencil_writer = st.stencil_test && (writes(st.front) || writes(st.back));
      plan_in.push_back(in);
      plan_op.push_back(i);
    }
    const colour_cuts::Plan plan = colour_cuts::PlanPass(plan_in, depth_clear, cut_plant);
    for (size_t k = 0; k < plan.flags.size(); ++k) cut_eligible[plan_op[k] - pass.first_op] = plan.flags[k];
    const uint32_t eligible_count = plan.eligible;
    const bool declined = plan.verdict == colour_cuts::kDeclineClear;
    if (plan.verdict == colour_cuts::kDeclineDirection) ++colour_cuts::g_declined_direction;
    else if (plan.verdict == colour_cuts::kDeclineStencil) ++colour_cuts::g_declined_stencil;
    if (declined) ++colour_cuts::g_declined_clear;
    else if (plan.verdict == colour_cuts::kDeclineDirection || plan.verdict == colour_cuts::kDeclineStencil) {}
    else if (!eligible_count) ++colour_cuts::g_declined_none;
    else {
      cut_on = true;
      cut_pre_pipes.assign(frame.states.count, VK_NULL_HANDLE);
      cut_main_pipes.assign(frame.states.count, VK_NULL_HANDLE);
      ++colour_cuts::g_frames_taken;
      VkPipeline pre_pipeline = VK_NULL_HANDLE;
      uint32_t pre_dynamic = ~0u, pre_textures = ~0u;
      BufferRange pre_vertex[2];
      BufferRange pre_index;
      uint8_t pre_index_32 = 0xFF;
      for (uint32_t i = pass.first_op; i < pass.end_op; ++i) {
        if (!cut_eligible[i - pass.first_op]) continue;
        const DrawOp& draw = frame.draws[frame.ops[i].index];
        VkPipeline& twin = cut_pre_pipes[draw.state];
        if (!twin) {
          DrawState ds = frame.states[draw.state];
          if (cut_plant != 6) ds.variant = kVariantDepthOnly;  // plant 6 (S102 C4 review F4, diagnosis only): the twin keeps the NORMAL fragment stage, colour write mask 0: if the stray pixels vanish the cause is the two pipelines' depth variance
          for (uint32_t c = 0; c < kMaxColorTargets; ++c) ds.blend[c].write_mask = 0;
          twin = GetPipeline(ds, pass_class);
        }
        if (!twin) continue;
        if (cut_plant == 3) continue;  // plant 3: the prepass records nothing (the counters below stay as if it had run: the check compares the picture's fragment count)
        if (twin != pre_pipeline) {
          vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, twin);
          pre_pipeline = twin;
          ++pc.pipeline_binds;
        }
        if (draw.dynamic != pre_dynamic) {
          pre_dynamic = draw.dynamic;
          const DynamicState& dyn = frame.dynamics[draw.dynamic];
          VkViewport viewport{dyn.viewport[0] * factor_x, dyn.viewport[1] * factor_y, dyn.viewport[2] * factor_x, dyn.viewport[3] * factor_y, dyn.viewport[4], dyn.viewport[5]};
          vkCmdSetViewport(cmd, 0, 1, &viewport);
          VkRect2D scissor{};
          scissor.offset = {std::max(0, int32_t(float(dyn.scissor.x) * factor_x)), std::max(0, int32_t(float(dyn.scissor.y) * factor_y))};
          scissor.extent = {uint32_t(std::ceil(float(dyn.scissor.width) * factor_x)), uint32_t(std::ceil(float(dyn.scissor.height) * factor_y))};
          if (cut_plant == 4) scissor.offset.x = 8, scissor.extent.width = 0x7FFFFFFF;  // plant 4: offset + extent overflows int32 (VUID-vkCmdSetScissor-offset-00596): only the validation layer sees it; the driver clamps to the render area
          vkCmdSetScissor(cmd, 0, 1, &scissor);
          vkCmdSetDepthBias(cmd, dyn.depth_bias_constant, dyn.depth_bias_clamp, dyn.depth_bias_slope);
          vkCmdSetBlendConstants(cmd, dyn.blend_constants);
          vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dyn.stencil_compare_mask);
          vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dyn.stencil_write_mask);
          vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dyn.stencil_reference);
          vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(dyn.system), dyn.system);
          ++pc.dynamic_sets, ++pc.push_constants;
        }
        const uint32_t offsets[2] = {uint32_t(ring_base + draw.vs_constants),
                                     uint32_t((draw.flags & kDrawOpPrevConstants) ? prev_constants_base + draw.vs_constants : ring_base + draw.fs_constants)};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &constants_set, 2, offsets);
        ++pc.descriptor_binds;
        if (draw.textures != pre_textures) {
          VkDescriptorSet set = GetTextureSet(frame, draw.textures, slot);
          if (!set) continue;
          pre_textures = draw.textures;
          vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 1, 1, &set, 0, nullptr);
          ++pc.descriptor_binds;
        }
        const VkDeviceSize geometry_base = ring_base + AlignUp(frame.constants.count, 16);
        for (uint32_t b = 0; b < 2; ++b) {
          if (draw.vertex[b].buffer == kNoBuffer) continue;
          if (draw.vertex[b].buffer == pre_vertex[b].buffer && draw.vertex[b].offset == pre_vertex[b].offset) continue;
          pre_vertex[b] = draw.vertex[b];
          const bool frame_owned = draw.vertex[b].buffer == kFrameGeometry;
          VkBuffer buffer = frame_owned ? ring : buffers[draw.vertex[b].buffer].buffer;
          VkDeviceSize offset = draw.vertex[b].offset + (frame_owned ? geometry_base : 0);
          vkCmdBindVertexBuffers(cmd, b, 1, &buffer, &offset);
          ++pc.vertex_binds;
        }
        if (draw.index.buffer != kNoBuffer) {
          if (draw.index.buffer != pre_index.buffer || draw.index.offset != pre_index.offset || draw.index_32bit != pre_index_32) {
            pre_index = draw.index;
            pre_index_32 = draw.index_32bit;
            const bool frame_owned = draw.index.buffer == kFrameGeometry;
            VkBuffer buffer = frame_owned ? ring : buffers[draw.index.buffer].buffer;
            vkCmdBindIndexBuffer(cmd, buffer, draw.index.offset + (frame_owned ? geometry_base : 0), draw.index_32bit ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
            ++pc.index_binds;
          }
          vkCmdDrawIndexed(cmd, draw.count, draw.instances, draw.first, draw.base_vertex, 0);
          ++pc.indexed_draws, pc.indices += uint64_t(draw.count) * draw.instances;
        } else {
          vkCmdDraw(cmd, draw.count, draw.instances, draw.first, 0);
          ++pc.draws, pc.vertices += uint64_t(draw.count) * draw.instances;
        }
        ++colour_cuts::g_draws_prepassed;
      }
    }
  }

  // S102 R18-B3 (mask 1): blend off for the draws the binder proved EXACTLY opaque. Per pass: a pipeline per state is made on first use. af_plant 4 = "not applied" (reads the mask, changes nothing: the counts stay 0).
  const int af_plant = arena_fragments::g_plant.load(std::memory_order_relaxed);
  const bool af_blend = (arena_fragments_ & arena_fragments::kBlendOff) && af_plant != 4 && images[first_target.image].gpu_class == kGpuColor && !g_draw_mod_mode.load(std::memory_order_relaxed);
  uint64_t af_off_draws = 0;
  uint64_t af_declined[arena_fragments::kReasonCount] = {};
  std::vector<uint8_t> af_all_arena;  // plant 1 only: per program, 1 = a yBG_* program
  if (af_blend) {
    af_pipes.assign(frame.states.count, {VK_NULL_HANDLE, 0});
    if (af_plant >= 1 && af_plant <= 3) {
      af_all_arena.assign(programs.size(), 0);
      for (size_t p = 0; p < programs.size(); ++p)
        if (programs[p].name && std::strstr(programs[p].name, "yBG_"))
          af_all_arena[p] = std::strstr(programs[p].name, "vertex_lm_vertex") ? arena_fragments::kClassVertexLit : std::strstr(programs[p].name, "yBG_Default_base") ? arena_fragments::kClassDefaultBase : arena_fragments::kClassArena;
    }
  }

  // S102 R24-F (wwe2k14_rdr_gpu_group_stamps): in a colour / frame pass of a frame that carries draw groups, a timestamp (BOTTOM_OF_PIPE, inside the render pass) at each change of group, so the pass's GPU time
  // splits by group. Not in a multiview pass (a stamp there writes one query a view). The chain: previous step's stamp -> group stamps -> this pass's own closing stamp (written by the step loop).
  const bool gs_on = slot.gs_frame && !layered_pass && slot.ts_count >= 2 && !g_draw_mod_mode.load(std::memory_order_relaxed) &&
                     (images[first_target.image].gpu_class == kGpuColor || images[first_target.image].gpu_class == kGpuFrame);
  const int gs_plant = rdr::g_gpu_group_plant.load(std::memory_order_relaxed);
  rdr::SegmentPlanner gs_plan;
  FrameSlot::GsPass gs_pass;
  uint32_t gs_prev = 0, gs_drawn = 0;
  if (gs_on) {
    gs_pass.begin_stamp = slot.ts_count - 1, gs_pass.end_stamp = slot.ts_count, gs_pass.first_seg = uint32_t(slot.gs_segs.size());
    gs_prev = gs_pass.begin_stamp;
    gs_plan.min_run = frame.group_min_run;
    gs_plan.budget = rdr::kGroupPoolCap > slot.gs_count + 1 ? rdr::kGroupPoolCap - slot.gs_count - 1 : 0;
    gs_plan.last = rdr::kGrpIdle;
  }
  const uint64_t gs_issued0 = uint64_t(pc.draws) + pc.indexed_draws;   // (the prepass's draws are counted before this point)
  VkPipeline bound_pipeline = VK_NULL_HANDLE;
  uint32_t bound_dynamic = ~0u, bound_textures = ~0u;
  BufferRange bound_vertex[2];
  BufferRange bound_index;
  uint8_t bound_index_32 = 0xFF;
  const bool draw_census_on = g_draw_census.load(std::memory_order_relaxed) && images[first_target.image].gpu_class == kGpuColor;
  std::map<std::string, DrawCensusRow> census_rows;  // (review F12: only ever filled, and the string keys only built, when draw_census_on)
  const bool ps_on = draw_census_on && ps_pool && g_draw_census_stats.load(std::memory_order_relaxed);
  const char* ps_open_name = nullptr;
  bool ps_open = false;
  // S102 G10 (instrument): rplay --draw-mod: leave out / 1x1-scissor the draws whose program name matches (see overlay.h g_draw_mod_mode).
  const int draw_mod = g_draw_mod_mode.load(std::memory_order_relaxed);
  if (draw_mod) {
    const uint32_t version = g_draw_mod_version.load();
    if (draw_mod_version != version || draw_mod_match.size() != programs.size()) {
      std::string names;
      {
        std::lock_guard<std::mutex> lock(g_draw_mod_mutex);
        names = g_draw_mod_names;
      }
      draw_mod_match.assign(programs.size(), 0);
      for (size_t p = 0; p < programs.size(); ++p) {
        const std::string name = programs[p].name ? programs[p].name : "";
        size_t at = 0;
        while (at <= names.size()) {
          size_t end = names.find(',', at);
          if (end == std::string::npos) end = names.size();
          const std::string part = names.substr(at, end - at);
          if (!part.empty() && (part == "*" || name.find(part) != std::string::npos)) draw_mod_match[p] = 1;
          at = end + 1;
        }
      }
      draw_mod_version = version;
    }
  }

  for (uint32_t i = pass.first_op; i < pass.end_op; ++i) {
    if (!half_skip.empty() && half_skip[i - pass.first_op]) continue;  // S102 C4: a draw of the half-size tail (drawn after this pass's first part, RecordHalfTail)
    const Op& op = frame.ops[i];
    if (op.kind == OpKind::Clear) {
      if (clear_absorbed[op.index]) continue;
      const ClearOp& clear = frame.clears[op.index];
      if (trace)
        std::fprintf(trace, "{\"t\":\"c\",\"f\":%llu,\"p\":%u,\"op\":%u,\"flags\":%u,\"full\":%u,\"w\":%u,\"h\":%u}\n", static_cast<unsigned long long>(serial), trace_pass, i, unsigned(clear.flags), unsigned(clear.full), clear.rect.width, clear.rect.height);  // S102 G11
      VkClearAttachment attachments[kAttachmentSlots];
      uint32_t count = 0;
      if (clear.flags & kClearColor) {
        for (uint32_t c = 0; c < targets.color_count; ++c) {
          if (!(clear.color_mask & (1u << c))) continue;
          attachments[count].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          attachments[count].colorAttachment = c;
          std::memcpy(attachments[count].clearValue.color.float32, clear.color, sizeof(clear.color));
          ++count;
        }
      }
      if ((clear.flags & (kClearDepth | kClearStencil)) && targets.depth.image != kNoImage) {
        VkImageAspectFlags depth_aspects = ((clear.flags & kClearDepth) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | ((clear.flags & kClearStencil) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        uint32_t clear_stencil = clear.stencil;
        if (pass.strip_stencil_clear && (clear.flags & kClearStencil)) {   // S102 R24-C (b): a stencil clear nothing reads is not issued (plant 3: even when a later pass reads it); decided and counted in Compile
          if (g_stencil_dead_poison.load(std::memory_order_relaxed)) clear_stencil = 0xA5;   // poison: issued, with garbage
          else depth_aspects &= ~VkImageAspectFlags(VK_IMAGE_ASPECT_STENCIL_BIT);
        }
        attachments[count].aspectMask = depth_aspects;
        attachments[count].colorAttachment = 0;
        attachments[count].clearValue.depthStencil = {clear.depth, clear_stencil};
        if (depth_aspects) ++count;
      }
      if (count) {
        VkClearRect rect{};
        rect.layerCount = 1;
        if (clear.full) {
          rect.rect.extent = {pass.width, pass.height};
        } else {
          const int64_t left = std::clamp<int64_t>(int64_t(float(clear.rect.x) * factor_x), 0, pass.width);
          const int64_t top = std::clamp<int64_t>(int64_t(float(clear.rect.y) * factor_y), 0, pass.height);
          const int64_t right = std::clamp<int64_t>(int64_t(std::ceil(float(clear.rect.x + int64_t(clear.rect.width)) * factor_x)), left, pass.width);
          const int64_t bottom = std::clamp<int64_t>(int64_t(std::ceil(float(clear.rect.y + int64_t(clear.rect.height)) * factor_y)), top, pass.height);
          rect.rect.offset = {int32_t(left), int32_t(top)};
          rect.rect.extent = {uint32_t(right - left), uint32_t(bottom - top)};
        }
        if (rect.rect.extent.width && rect.rect.extent.height) vkCmdClearAttachments(cmd, count, attachments, 1, &rect), pc.clears += count;  // S102 M1
      }
      continue;
    }
    if (op.kind != OpKind::Draw) continue;
    const DrawOp& draw = frame.draws[op.index];
    bool mod_scissor = false;  // S102 G10
    if (draw_mod) {
      const uint16_t program = frame.states[draw.state].program;
      if (program < draw_mod_match.size() && draw_mod_match[program]) {
        if (draw_mod == 1) continue;
        mod_scissor = true;
      }
    }

    VkPipeline pipe_now = VK_NULL_HANDLE;
    bool af_this = false;  // S102 R18-B3: this draw runs with its blend switched off
    if (cut_on && cut_plant == 1 && cut_eligible[i - pass.first_op]) {  // S102 G10 plant 1 only (review F5: the real cut changes no op: eligible draws already test GREATER_OR_EQUAL)
      VkPipeline& main_pipe = cut_main_pipes[draw.state];
      if (!main_pipe) {
        DrawState ms = frame.states[draw.state];
        ms.depth_compare = colour_cuts::MainCompare(ms.depth_compare, cut_plant);
        main_pipe = GetPipeline(ms, pass_class);
      }
      pipe_now = main_pipe;
    } else if (af_blend && ((draw.flags & kDrawOpBlendOff) || (af_plant >= 1 && af_plant <= 3 && af_all_arena[frame.states[draw.state].program] && (af_plant == 1 || af_all_arena[frame.states[draw.state].program] == (af_plant == 2 ? arena_fragments::kClassDefaultBase : arena_fragments::kClassVertexLit))))) {
      // S102 R18-B3 mask 1: the pipeline of this state without the plain alpha blend (the binder proved the output alpha EXACTLY 1: arena_fragments.h). One pipeline per state a pass, no draw record added.
      auto& slot_af = af_pipes[draw.state];
      if (!slot_af.second) {
        DrawState ds = frame.states[draw.state];
        slot_af.second = 2;
        bool drop = af_plant >= 1 && af_plant <= 3 && (draw.flags & kDrawOpBlendOff) == 0;  // plants 1-3: the planted family, whatever its blend (translucent overlays too)
        if (!drop) {
          arena_fragments::BlendIn in[kMaxColorTargets];
          bool clamps[kMaxColorTargets] = {};
          for (uint32_t c = 0; c < targets.color_count && c < kMaxColorTargets; ++c) {
            const BlendState& b = ds.blend[c];
            in[c] = {b.enable != 0, b.src_color, b.dst_color, b.color_op, b.src_alpha, b.dst_alpha, b.alpha_op, b.write_mask};
            switch (images[targets.color[c].image].desc.format) {  // review F1: fixed-point targets clamp the source alpha to [0,1] before blending; float targets do not
              case Format::RGBA8: case Format::BGRA8: case Format::RGBA8_SNORM: case Format::R8: case Format::RG8: case Format::RGBA4: case Format::R5G6B5: case Format::A1RGB5:
              case Format::RGB10A2: case Format::RGBA16_UNORM: case Format::RG16_UNORM: case Format::R16_UNORM: clamps[c] = true; break;
              default: break;
            }
          }
          int why = 0;
          drop = arena_fragments::CanDropBlend(in, clamps, std::min<uint32_t>(targets.color_count, kMaxColorTargets), &why);
          if (!drop) ++af_declined[why];
        }
        if (drop) {
          for (uint32_t c = 0; c < kMaxColorTargets; ++c) ds.blend[c].enable = 0;
          slot_af.first = GetPipeline(ds, pass_class), slot_af.second = slot_af.first ? 1 : 2;
        }
      }
      if (slot_af.second == 1) {
        pipe_now = slot_af.first;
        ++af_off_draws;
        af_this = true;
      } else {
        auto& cached = state_pipelines[draw.state];
        if (!cached.first || cached.second != pass_class) cached = {GetPipeline(frame.states[draw.state], pass_class), pass_class};
        pipe_now = cached.first;
      }
    } else {
      auto& cached = state_pipelines[draw.state];
      if (!cached.first || cached.second != pass_class) cached = {GetPipeline(frame.states[draw.state], pass_class), pass_class};
      pipe_now = cached.first;
    }
    if (!pipe_now) continue;  // named by BuildPipeline
    if (pipe_now != bound_pipeline) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_now);
      bound_pipeline = pipe_now;
      ++pc.pipeline_binds;  // S102 M1
    }

    if (draw.dynamic != bound_dynamic) {
      bound_dynamic = draw.dynamic;
      const DynamicState& dyn = frame.dynamics[draw.dynamic];
      if (counting) {  // S102 M1: the area bound of this block: viewport (clamped to the pass) intersected with the scissor, integer pixels
        const auto lo = [](float v) { return int64_t(std::floor(v)); };
        const auto hi = [](float v) { return int64_t(std::ceil(v)); };
        const float vx = dyn.viewport[0] * factor_x, vy = dyn.viewport[1] * factor_y, vw = dyn.viewport[2] * factor_x, vh = dyn.viewport[3] * factor_y;
        const int64_t vx0 = std::clamp<int64_t>(lo(vx), 0, pass_w), vy0 = std::clamp<int64_t>(lo(vy), 0, pass_h);
        const int64_t vx1 = std::clamp<int64_t>(hi(vx + vw), vx0, pass_w), vy1 = std::clamp<int64_t>(hi(vy + vh), vy0, pass_h);
        const int64_t sx0 = std::clamp<int64_t>(std::max(0, int32_t(float(dyn.scissor.x) * factor_x)), 0, pass_w), sy0 = std::clamp<int64_t>(std::max(0, int32_t(float(dyn.scissor.y) * factor_y)), 0, pass_h);
        const int64_t sx1 = std::clamp<int64_t>(sx0 + int64_t(std::ceil(float(dyn.scissor.width) * factor_x)), sx0, pass_w), sy1 = std::clamp<int64_t>(sy0 + int64_t(std::ceil(float(dyn.scissor.height) * factor_y)), sy0, pass_h);
        const int64_t ax0 = std::max(vx0, sx0), ay0 = std::max(vy0, sy0), ax1 = std::min(vx1, sx1), ay1 = std::min(vy1, sy1);
        dyn_area = ax1 > ax0 && ay1 > ay0 ? uint64_t(ax1 - ax0) * uint64_t(ay1 - ay0) : 0;
      }
      ++pc.dynamic_sets, ++pc.push_constants;  // S102 M1
      VkViewport viewport{dyn.viewport[0] * factor_x, dyn.viewport[1] * factor_y, dyn.viewport[2] * factor_x, dyn.viewport[3] * factor_y, dyn.viewport[4], dyn.viewport[5]};
      vkCmdSetViewport(cmd, 0, 1, &viewport);
      VkRect2D scissor{};
      scissor.offset = {std::max(0, int32_t(float(dyn.scissor.x) * factor_x)), std::max(0, int32_t(float(dyn.scissor.y) * factor_y))};
      scissor.extent = {uint32_t(std::ceil(float(dyn.scissor.width) * factor_x)), uint32_t(std::ceil(float(dyn.scissor.height) * factor_y))};
      vkCmdSetScissor(cmd, 0, 1, &scissor);
      vkCmdSetDepthBias(cmd, dyn.depth_bias_constant, dyn.depth_bias_clamp, dyn.depth_bias_slope);
      vkCmdSetBlendConstants(cmd, dyn.blend_constants);
      vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dyn.stencil_compare_mask);
      vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dyn.stencil_write_mask);
      vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dyn.stencil_reference);
      vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(dyn.system), dyn.system);
    }

    if (ps_on && !layered_pass) {  // S102 G10: a statistics query per run of same-named programs (S102 C1 review F6: not inside a multiview pass)
      const uint16_t program = frame.states[draw.state].program;
      const char* name = program < programs.size() && programs[program].name ? programs[program].name : "?";
      const bool per_draw = g_draw_log.load(std::memory_order_relaxed) != nullptr;  // S102 C4: one query per draw, with a line of its state
      if (name != ps_open_name || per_draw) {
        if (ps_open) vkCmdEndQuery(cmd, ps_pool, ps_count - 1);
        ps_open = ps_count < ps_cap;
        if (ps_open) vkCmdBeginQuery(cmd, ps_pool, ps_count++, 0), ps_names.push_back(name);
        if (ps_open && per_draw) {
          const DrawState& dst = frame.states[draw.state];
          const BlendState& b0 = dst.blend[0];
          char line[512];
          std::snprintf(line, sizeof line, "f %llu p %u i %u %s | blend %u %u %u %u wm %u dt %u dw %u cmp %u st %u alphaop %u tris %llu area %llu tex %u vb %u",
                        static_cast<unsigned long long>(serial), trace_pass, i, name, unsigned(b0.enable), unsigned(b0.src_color), unsigned(b0.dst_color), unsigned(b0.color_op), unsigned(b0.write_mask), unsigned(dst.depth_test),
                        unsigned(dst.depth_write), unsigned(dst.depth_compare), unsigned(dst.stencil_test), unsigned((draw.flags & kDrawOpAlphaOpaque) != 0), static_cast<unsigned long long>(uint64_t(draw.count) * draw.instances / 3),
                        static_cast<unsigned long long>(dyn_area), unsigned(draw.textures), unsigned(draw.vertex[0].buffer == kFrameGeometry));
          ps_meta.resize(ps_count), ps_meta[ps_count - 1] = line;
          ps_afoff.resize(ps_count), ps_afoff[ps_count - 1] = af_this ? 1 : 0;  // S102 R18-B3
        }
        ps_open_name = name;
      }
    }
    if (mod_scissor) {  // S102 G10: 1x1 scissor for this draw; the next draw re-sets its own block
      const VkRect2D tiny{{0, 0}, {1, 1}};
      vkCmdSetScissor(cmd, 0, 1, &tiny);
      bound_dynamic = ~0u;
    }
    const uint32_t offsets[2] = {uint32_t(ring_base + draw.vs_constants),
                                 uint32_t((draw.flags & kDrawOpPrevConstants) ? prev_constants_base + draw.vs_constants : ring_base + draw.fs_constants)};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &constants_set, 2, offsets);
    ++pc.descriptor_binds;  // S102 M1
    if (draw.textures != bound_textures) {
      VkDescriptorSet set = GetTextureSet(frame, draw.textures, slot);
      if (!set) continue;  // named by GetTextureSet
      bound_textures = draw.textures;
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 1, 1, &set, 0, nullptr);
      ++pc.descriptor_binds;  // S102 M1
    }

    const VkDeviceSize geometry_base = ring_base + AlignUp(frame.constants.count, 16);
    for (uint32_t b = 0; b < 2; ++b) {
      if (draw.vertex[b].buffer == kNoBuffer) continue;
      if (draw.vertex[b].buffer == bound_vertex[b].buffer && draw.vertex[b].offset == bound_vertex[b].offset) continue;
      bound_vertex[b] = draw.vertex[b];
      const bool frame_owned = draw.vertex[b].buffer == kFrameGeometry;
      VkBuffer buffer = frame_owned ? ring : buffers[draw.vertex[b].buffer].buffer;
      VkDeviceSize offset = draw.vertex[b].offset + (frame_owned ? geometry_base : 0);
      vkCmdBindVertexBuffers(cmd, b, 1, &buffer, &offset);
      ++pc.vertex_binds;  // S102 M1
    }
    if (gs_on) {   // S102 R24-F: the draw is issued below: close the segment before it when its group differs (and the segment is long enough, and the pool has room)
      const uint8_t g = frame.draw_groups[op.index];
      if (gs_plan.NeedStamp(g)) {
        const uint32_t q = slot.gs_count++;
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.gs_pool, q);
        const uint32_t enc = rdr::kPoolBit | q;
        rdr::GroupSeg seg;
        seg.begin = gs_prev, seg.end = enc, seg.pass_end_stamp = uint16_t(gs_pass.end_stamp);
        gs_plan.Close(seg);
        if (!(gs_plant == 1 && q == 2)) slot.gs_segs.push_back(seg);   // plant 1: the stamp is written, its segment is not recorded: the chain breaks
        gs_prev = enc;
        --gs_plan.budget;
      } else if (gs_plan.draws > 0 && g != gs_plan.last && gs_plan.draws >= std::max<uint32_t>(1, gs_plan.min_run) && gs_plan.budget == 0) {
        slot.gs_overflow = true;   // out of stamps: the rest of the pass is one mixed segment (labelled by its dominant group)
      }
      if (!(gs_plant == 2 && gs_drawn == 4)) gs_plan.Add(g);   // plant 2: one draw is left out of the segment counts (coverage 100 % breaks)
      else gs_plan.last = g;
      ++gs_drawn;
    }
    const bool occ_on = trace && !layered_pass && slot.occ && slot.occ_count < 512;   // (S102 C1 review F6: not inside a multiview pass)  // S102 G11
    if (occ_on) vkCmdBeginQuery(cmd, slot.occ, slot.occ_count, device.caps().occlusion_precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0), slot.occ_meta.push_back(trace_pass), slot.occ_meta.push_back(i), ++slot.occ_count;
    if (draw.index.buffer != kNoBuffer) {
      if (draw.index.buffer != bound_index.buffer || draw.index.offset != bound_index.offset || draw.index_32bit != bound_index_32) {
        bound_index = draw.index;
        bound_index_32 = draw.index_32bit;
        const bool frame_owned = draw.index.buffer == kFrameGeometry;
        VkBuffer buffer = frame_owned ? ring : buffers[draw.index.buffer].buffer;
        vkCmdBindIndexBuffer(cmd, buffer, draw.index.offset + (frame_owned ? geometry_base : 0), draw.index_32bit ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        ++pc.index_binds;  // S102 M1
      }
      int32_t draw_base_vertex = draw.base_vertex;
      if (crumbs::g_plant.load(std::memory_order_relaxed) == 5 && !plant5_done) plant5_done = true, draw_base_vertex += 1 << 22;  // S102 D1 TEST ONLY (rplay --crumbs-plant 5): one vertex fetch a frame far past its vertex buffer: GPU-assisted validation must flag it
      vkCmdDrawIndexed(cmd, draw.count, draw.instances, draw.first, draw_base_vertex, 0);
      ++pc.indexed_draws, pc.indices += uint64_t(draw.count) * draw.instances;  // S102 M1
    } else {
      vkCmdDraw(cmd, draw.count, draw.instances, draw.first, 0);
      ++pc.draws, pc.vertices += uint64_t(draw.count) * draw.instances;  // S102 M1
    }
    if (occ_on) vkCmdEndQuery(cmd, slot.occ, slot.occ_count - 1);  // S102 G11
    pc.area_px += dyn_area;  // S102 M1
    if (trace) {  // S102 G11: the draw's own state: blend of target 0, depth, the viewport rectangle, the pixels bound (area)
      const DrawState& ds = frame.states[draw.state];
      const DynamicState& dy = frame.dynamics[draw.dynamic];
      std::fprintf(trace, "{\"t\":\"d\",\"f\":%llu,\"p\":%u,\"op\":%u,\"area\":%llu,\"verts\":%llu,\"blend\":[%u,%u,%u,%u,%u,%u,%u,%u],\"dt\":%u,\"dw\":%u,\"st\":%u,\"vp\":[%.1f,%.1f,%.1f,%.1f],\"sc\":[%d,%d,%u,%u],\"tex\":%u,\"prog\":%u}\n",
                   static_cast<unsigned long long>(serial), trace_pass, i, static_cast<unsigned long long>(dyn_area), static_cast<unsigned long long>(uint64_t(draw.count) * draw.instances),
                   unsigned(ds.blend[0].enable), unsigned(ds.blend[0].src_color), unsigned(ds.blend[0].dst_color), unsigned(ds.blend[0].color_op), unsigned(ds.blend[0].src_alpha), unsigned(ds.blend[0].dst_alpha), unsigned(ds.blend[0].alpha_op), unsigned(ds.blend[0].write_mask),
                   unsigned(ds.depth_test), unsigned(ds.depth_write), unsigned(ds.stencil_test), dy.viewport[0], dy.viewport[1], dy.viewport[2], dy.viewport[3], int(dy.scissor.x), int(dy.scissor.y), unsigned(dy.scissor.width), unsigned(dy.scissor.height),
                   unsigned(draw.textures), unsigned(ds.program));
    }
    if (draw_census_on) {  // S102 G10 (rplay --draw-census): the scene colour passes' draws per program name
      const uint16_t program = frame.states[draw.state].program;
      DrawCensusRow& row = census_rows[program < programs.size() && programs[program].name ? programs[program].name : "?"];
      ++row.draws, row.indices += uint64_t(draw.count) * draw.instances, row.area_px += dyn_area;
    }
    last_draw = &draw;
    dyn_area_now = dyn_area;
    if (plant_here && i == plant_op) plant_draw(draw);
  }
  if (plant_here && !plant_done && last_draw) plant_draw(*last_draw);  // (the pass's last draw op was skipped: the last one that was recorded, state still bound)
  if (gs_on) {   // S102 R24-F: the last segment ends at the pass's own closing stamp (the step loop writes it at index end_stamp, right after this pass)
    rdr::GroupSeg seg;
    seg.begin = gs_prev, seg.end = gs_pass.end_stamp, seg.pass_end_stamp = uint16_t(gs_pass.end_stamp);
    gs_plan.Close(seg);
    slot.gs_segs.push_back(seg);
    gs_pass.segs = uint32_t(slot.gs_segs.size()) - gs_pass.first_seg;
    gs_pass.issued = uint32_t(uint64_t(pc.draws) + pc.indexed_draws - gs_issued0);
    slot.gs_passes.push_back(gs_pass);
  }
  if (ps_open) vkCmdEndQuery(cmd, ps_pool, ps_count - 1);
  if (af_blend) {  // S102 R18-B3: the pass's counts, added once (relaxed, fixed slots: nothing per draw)
    if (af_off_draws) arena_fragments::g_blend_off.fetch_add(af_off_draws, std::memory_order_relaxed), arena_fragments::g_passes_taken.fetch_add(1, std::memory_order_relaxed);
    for (int r = 0; r < arena_fragments::kReasonCount; ++r)
      if (af_declined[r]) arena_fragments::g_declined[r].fetch_add(af_declined[r], std::memory_order_relaxed);
  }
  if (draw_census_on) {
    std::lock_guard<std::mutex> lock(g_draw_census_mutex);
    for (const auto& r : census_rows) {
      DrawCensusRow& to = g_draw_census_rows[r.first];
      to.draws += r.second.draws, to.indices += r.second.indices, to.area_px += r.second.area_px;
    }
  }
  if (sd_on && g_stencil_dead_poison.load(std::memory_order_relaxed) && pass.stencil_store == VK_ATTACHMENT_STORE_OP_DONT_CARE && targets.depth.image != kNoImage && (images[targets.depth.image].aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) {
    VkClearAttachment poison{};   // poison: a pass that does not store its stencil leaves garbage in it
    poison.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT, poison.clearValue.depthStencil = {0.0f, 0xA5};
    VkClearRect prect{};
    prect.layerCount = 1, prect.rect.extent = {pass.width, pass.height};
    vkCmdClearAttachments(cmd, 1, &poison, 1, &prect);
  }
  vkCmdEndRenderPass(cmd);
  if (tail_begin < pass.end_op) RecordHalfTail(cmd, frame, pass, slot, ring_base, half_skip, pass_class, factor_x, factor_y, half_prep, pc);  // S102 C4
  if (crumb_step) {  // S102 D1: the pass's counts into its crumb (the counters above, no per-draw work), its first draw op's and last recorded draw's programs
    crumb_step->draws = pc.draws + pc.indexed_draws, crumb_step->binds = pc.pipeline_binds;
    crumb_step->prims = uint32_t(std::min<uint64_t>(pc.vertices + pc.indices, 0xFFFFFFFFull));
    crumb_step->last_prog = last_draw ? frame.states[last_draw->state].program : uint16_t(0xFFFF);
    for (uint32_t i = pass.first_op; i < pass.end_op; ++i)
      if (frame.ops[i].kind == OpKind::Draw) { crumb_step->first_prog = frame.states[frame.draws[frame.ops[i].index].state].program; break; }
  }
  if (counting) {  // S102 M1
    pc.barriers = counts_cur.total.barriers - barriers_before;
    pc.barrier_images = counts_cur.total.barrier_images - barrier_images_before;
    PassCounts record;
    record.index = uint32_t(&pass - passes.data()), record.width = pass.width, record.height = pass.height, record.color_count = targets.color_count, record.first_op = pass.first_op;
    record.target_image = first_target.image, record.target_mip = first_target.mip, record.target_layer = first_target.layer;
    record.target_format = uint32_t(images[first_target.image].desc.format);
    record.calls = pc;
    counts_cur.pass.push_back(record);
    pc.barriers = pc.barrier_images = 0;  // (already in the frame total: FlushBarriers added them there)
    counts_cur.total.Add(pc);
    ++counts_cur.passes;
  }
}

Renderer::Impl::CopyGeom Renderer::Impl::GeometryOf(const CopyOp& copy) {
  CopyGeom g;
  ImageEntry& source = images[copy.source.image];
  ImageEntry& dest = images[copy.dest.image];
  g.source = &source, g.dest = &dest;
  g.source_sub = copy.source.mip * source.layers + copy.source.layer;
  g.dest_sub = copy.dest.mip * dest.layers + copy.dest.layer;
  g.whole = copy.dest_rect.x == 0 && copy.dest_rect.y == 0 && copy.dest_rect.width >= dest.desc.width && copy.dest_rect.height >= dest.desc.height;
  const float source_factor = (source.desc.usage & kUsageScaled) ? scale : 1.0f;
  const float dest_factor = (dest.desc.usage & kUsageScaled) ? scale : 1.0f;
  const auto origin = [](int32_t value, float factor) { return int32_t(float(value) * factor); };
  const auto extent = [](uint32_t value, float factor) { return uint32_t(std::ceil(float(value) * factor)); };
  g.source_x = origin(copy.source_rect.x, source_factor), g.source_y = origin(copy.source_rect.y, source_factor);
  g.dest_x = origin(copy.dest_rect.x, dest_factor), g.dest_y = origin(copy.dest_rect.y, dest_factor);
  const uint32_t source_limit_w = std::max(1u, source.width >> copy.source.mip), source_limit_h = std::max(1u, source.height >> copy.source.mip);
  const uint32_t dest_limit_w = std::max(1u, dest.width >> copy.dest.mip), dest_limit_h = std::max(1u, dest.height >> copy.dest.mip);
  g.source_w = std::min(extent(copy.source_rect.width, source_factor), source_limit_w - uint32_t(g.source_x));
  g.source_h = std::min(extent(copy.source_rect.height, source_factor), source_limit_h - uint32_t(g.source_y));
  g.dest_w = std::min(extent(copy.dest_rect.width, dest_factor), dest_limit_w - uint32_t(g.dest_x));
  g.dest_h = std::min(extent(copy.dest_rect.height, dest_factor), dest_limit_h - uint32_t(g.dest_y));
  if (!depth_only_copies.empty()) g.depth_only = std::find(depth_only_copies.begin(), depth_only_copies.end(), &copy) != depth_only_copies.end();   // S102 R24-C
  const bool same_block = source.format == dest.format || (!source.depth && !dest.depth && BlockBytes(source.desc.format) != 0 && BlockBytes(source.desc.format) == BlockBytes(dest.desc.format));
  if (same_block && g.source_w == g.dest_w && g.source_h == g.dest_h) g.path = 1;
  else if (!source.depth && !dest.depth && device.FormatHas(source.format, VK_FORMAT_FEATURE_BLIT_SRC_BIT) && device.FormatHas(dest.format, VK_FORMAT_FEATURE_BLIT_DST_BIT)) g.path = 2;
  return g;
}

VkImageCopy Renderer::Impl::RegionOf(const CopyOp& copy, const CopyGeom& g) {
  VkImageCopy region{};
  const VkImageAspectFlags aspect_s = g.depth_only ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : g.source->aspect, aspect_d = g.depth_only ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : g.dest->aspect;   // S102 R24-C
  region.srcSubresource = {aspect_s, copy.source.mip, copy.source.layer, 1};
  region.srcOffset = {g.source_x, g.source_y, 0};
  region.dstSubresource = {aspect_d, copy.dest.mip, copy.dest.layer, 1};
  region.dstOffset = {g.dest_x, g.dest_y, 0};
  region.extent = {g.source_w, g.source_h, 1};
  return region;
}

// S102 C4 (half_trans.h): the half-size tail's images, pass keys and the two internal pipelines. false = decline (the tail then stays in the pass, drawn at full size).
VkDescriptorSet Renderer::Impl::MakeHalfSet(FrameSlot& slot, ImageEntry* a, ImageEntry* b, ImageEntry* c) {
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = slot.descriptors;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &textures_layout;
  if (vkAllocateDescriptorSets(device.device(), &alloc, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
  VkDescriptorImageInfo infos[kMaxTextures];
  ImageEntry& dummy = images[dummy_image];
  for (uint32_t i = 0; i < kMaxTextures; ++i) infos[i] = {dummy_sampler, dummy.sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  if (a) infos[0] = {dummy_sampler, a->sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  if (b) infos[1] = {dummy_sampler, b->sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  if (c) infos[2] = {dummy_sampler, c->sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  WriteTextureSet(set, infos);
  return set;
}

bool Renderer::Impl::PrepareHalf(const PassPlan& pass, FrameSlot& slot, HalfPrep& prep) {
  const TargetsOp& targets = *pass.targets;
  ImageEntry& color = images[targets.color[0].image];
  ImageEntry& depth = images[targets.depth.image];
  prep.hw = std::max<uint32_t>(1, (pass.width + 1) / 2), prep.hh = std::max<uint32_t>(1, (pass.height + 1) / 2);
  const bool made = half_color != kNoImage && half_depth_img != kNoImage && half_w == prep.hw && half_h == prep.hh && images[half_color].desc.format == color.desc.format && images[half_depth_img].desc.format == depth.desc.format;
  if (!made) {
    if (half_color != kNoImage) self->DestroyImage(half_color), half_color = kNoImage;
    if (half_depth_img != kNoImage) self->DestroyImage(half_depth_img), half_depth_img = kNoImage;
    ImageDesc desc;
    desc.width = uint16_t(prep.hw), desc.height = uint16_t(prep.hh);
    desc.format = color.desc.format;
    desc.usage = kUsageSampled | kUsageTarget;
    half_color = self->CreateImage(desc);
    desc.format = depth.desc.format;
    half_depth_img = self->CreateImage(desc);
    half_w = prep.hw, half_h = prep.hh;
  }
  if (half_color == kNoImage || half_depth_img == kNoImage) return false;
  ImageEntry& hc = images[half_color];
  ImageEntry& hd = images[half_depth_img];
  if (!hc.sampled_view || !hd.sampled_view || !hd.depth) return false;
  const uint8_t dont_care = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  // (1) the half-size depth: depth only, written by one triangle that reads the full-size depth
  RenderPassKey& dk = prep.depth_key;
  dk = RenderPassKey{};
  dk.formats[kMaxColorTargets] = uint16_t(hd.format), dk.load[kMaxColorTargets] = VK_ATTACHMENT_LOAD_OP_DONT_CARE, dk.store[kMaxColorTargets] = VK_ATTACHMENT_STORE_OP_STORE;
  dk.stencil_load = dont_care, dk.stencil_store = VK_ATTACHMENT_STORE_OP_DONT_CARE, dk.color_count = 0, dk.has_depth = 1, dk.samples = 1;
  prep.depth_class = PassClassKey{};
  prep.depth_class.formats[kMaxColorTargets] = uint16_t(hd.format), prep.depth_class.has_depth = 1, prep.depth_class.samples = 1;
  // (2) the half-size draws: cleared colour, the half-size depth loaded
  RenderPassKey& hk = prep.half_key;
  hk = RenderPassKey{};
  hk.formats[0] = uint16_t(hc.format), hk.load[0] = VK_ATTACHMENT_LOAD_OP_CLEAR, hk.store[0] = VK_ATTACHMENT_STORE_OP_STORE;
  hk.formats[kMaxColorTargets] = uint16_t(hd.format), hk.load[kMaxColorTargets] = VK_ATTACHMENT_LOAD_OP_LOAD, hk.store[kMaxColorTargets] = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  hk.stencil_load = dont_care, hk.stencil_store = VK_ATTACHMENT_STORE_OP_DONT_CARE, hk.color_count = 1, hk.has_depth = 1, hk.samples = 1;
  // (3) the add-back: the full-size colour loaded, no depth attachment (the full-size depth is read as a texture)
  RenderPassKey& ck = prep.comp_key;
  ck = RenderPassKey{};
  ck.formats[0] = uint16_t(color.format), ck.load[0] = VK_ATTACHMENT_LOAD_OP_LOAD, ck.store[0] = pass.store[0];
  ck.stencil_load = dont_care, ck.stencil_store = VK_ATTACHMENT_STORE_OP_DONT_CARE, ck.color_count = 1, ck.has_depth = 0, ck.samples = 1;
  prep.comp_class = PassClassKey{};
  prep.comp_class.formats[0] = uint16_t(color.format), prep.comp_class.color_count = 1, prep.comp_class.samples = 1;
  DrawState depth_state{};
  depth_state.program = half_depth_program;
  depth_state.vertex_layout = present_layout;
  depth_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  depth_state.depth_test = 1, depth_state.depth_write = 1, depth_state.depth_compare = VK_COMPARE_OP_ALWAYS;
  prep.depth_pipe = GetPipeline(depth_state, GetPassClass(prep.depth_class));
  DrawState comp_state{};
  comp_state.program = half_composite_program;
  comp_state.vertex_layout = present_layout;
  comp_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  comp_state.depth_compare = VK_COMPARE_OP_ALWAYS;
  comp_state.blend[0] = {1, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD, 0xF};  // adds rgba: the tail's draws only ever added to the target's alpha, or left it alone (half_trans.h)
  prep.comp_pipe = GetPipeline(comp_state, GetPassClass(prep.comp_class));
  if (prep.depth_pipe == VK_NULL_HANDLE || prep.comp_pipe == VK_NULL_HANDLE) return false;
  prep.depth_set = MakeHalfSet(slot, &depth, nullptr, nullptr);
  prep.comp_set = MakeHalfSet(slot, &hc, &hd, &depth);
  if (!prep.depth_set || !prep.comp_set) {
    Problem("descriptor_pool_empty", "no room for the half-size sets: the tail stays in the pass");
    return false;
  }
  return true;
}

// S102 C4: after the pass's first part (its render pass has ended): (1) the half-size depth from the full-size depth, (2) the tail's draws into a cleared half-size colour against it, (3) the half-size colour added
// onto the full-size colour with a depth-aware upsample. Three render passes, a handful of barriers; the fragment work is a quarter of the tail's (plus one half-size triangle and one full-size one).
void Renderer::Impl::RecordHalfTail(VkCommandBuffer cmd, const Frame& frame, const PassPlan& pass, FrameSlot& slot, VkDeviceSize ring_base, const std::vector<uint8_t>& half_skip, uint32_t pass_class, float factor_x, float factor_y, const HalfPrep& prep, CallCounts& pc) {
  const TargetsOp& targets = *pass.targets;
  ImageEntry& color = images[targets.color[0].image];
  ImageEntry& depth = images[targets.depth.image];
  ImageEntry& hc = images[half_color];
  ImageEntry& hd = images[half_depth_img];
  const bool ps_on = g_draw_census.load(std::memory_order_relaxed) && ps_pool && g_draw_census_stats.load(std::memory_order_relaxed);
  bool ps_open = false;
  const auto query_end = [&]() { if (ps_open) vkCmdEndQuery(cmd, ps_pool, ps_count - 1), ps_open = false; };
  const auto query_begin = [&](const std::string& name) {
    if (!ps_on) return;
    query_end();
    ps_open = ps_count < ps_cap;
    if (ps_open) vkCmdBeginQuery(cmd, ps_pool, ps_count++, 0), ps_names.push_back(name);
  };
  const uint32_t const_offsets[2] = {uint32_t(ring_base), uint32_t(ring_base)};
  const int plant = half_trans::g_plant.load(std::memory_order_relaxed);
  float push[16] = {};
  push[0] = plant == 1 ? 1.0f : 0.0f;  // plant 1: the composite's plain bilinear upsample

  // (1) the half-size depth
  Transition(depth, 0, uint32_t(depth.states.size()), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, false);
  Transition(hd, 0, uint32_t(hd.states.size()), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, true);
  FlushBarriers(cmd);
  {
    VkDescriptorSet set = prep.depth_set;
    FramebufferKey fk{};
    fk.pass = GetRenderPass(prep.depth_key);
    fk.views[kMaxColorTargets] = TargetView(hd, 0, 0);
    fk.width = prep.hw, fk.height = prep.hh;
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = fk.pass;
    begin.framebuffer = GetFramebuffer(fk);
    begin.renderArea.extent = {prep.hw, prep.hh};
    if (plant != 3) {  // plant 3: the half-size depth pass is left out (the depth the half draws test against is whatever the image held)
      vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
      query_begin("half|depth");
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, prep.depth_pipe);
      VkViewport viewport{0, 0, float(prep.hw), float(prep.hh), 0, 1};
      VkRect2D scissor{{0, 0}, {prep.hw, prep.hh}};
      vkCmdSetViewport(cmd, 0, 1, &viewport);
      vkCmdSetScissor(cmd, 0, 1, &scissor);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &constants_set, 2, const_offsets);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 1, 1, &set, 0, nullptr);
      vkCmdDraw(cmd, 3, 1, 0, 0);
      query_end();
      vkCmdEndRenderPass(cmd);
      pc.pipeline_binds += 1, pc.descriptor_binds += 2, pc.dynamic_sets += 1, ++pc.draws, pc.vertices += 3;
    }
  }

  // (2) the tail's draws, half size
  Transition(hc, 0, uint32_t(hc.states.size()), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, true);
  // (the half pass writes no depth, but its storeOp counts as a write for the barriers after it: the access is READ | WRITE, as the first pass's)
  Transition(hd, 0, uint32_t(hd.states.size()), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, false);
  FlushBarriers(cmd);
  {
    FramebufferKey fk{};
    fk.pass = GetRenderPass(prep.half_key);
    fk.views[0] = TargetView(hc, 0, 0);
    fk.views[kMaxColorTargets] = TargetView(hd, 0, 0);
    fk.width = prep.hw, fk.height = prep.hh;
    VkClearValue clears[kAttachmentSlots] = {};
    clears[0].color = {{0, 0, 0, 0}};
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = fk.pass;
    begin.framebuffer = GetFramebuffer(fk);
    begin.renderArea.extent = {prep.hw, prep.hh};
    begin.clearValueCount = 2;
    begin.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    const float half_factor_x = factor_x * 0.5f, half_factor_y = factor_y * 0.5f;
    VkPipeline bound_pipeline = VK_NULL_HANDLE;
    uint32_t bound_dynamic = ~0u, bound_textures = ~0u;
    BufferRange bound_vertex[2];
    BufferRange bound_index;
    uint8_t bound_index_32 = 0xFF;
    const char* open_name = nullptr;
    for (uint32_t i = pass.first_op; i < pass.end_op; ++i) {
      if (!half_skip[i - pass.first_op]) continue;
      const DrawOp& draw = frame.draws[frame.ops[i].index];
      const DrawState& st = frame.states[draw.state];
      VkPipeline pipe = GetPipeline(st, pass_class);
      if (!pipe) continue;
      if (ps_on) {
        const char* name = st.program < programs.size() && programs[st.program].name ? programs[st.program].name : "?";
        if (name != open_name) query_begin(std::string("half|") + name), open_name = name;
      }
      if (pipe != bound_pipeline) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        bound_pipeline = pipe;
        ++pc.pipeline_binds;
      }
      if (draw.dynamic != bound_dynamic) {
        bound_dynamic = draw.dynamic;
        const DynamicState& dyn = frame.dynamics[draw.dynamic];
        VkViewport viewport{dyn.viewport[0] * half_factor_x, dyn.viewport[1] * half_factor_y, dyn.viewport[2] * half_factor_x, dyn.viewport[3] * half_factor_y, dyn.viewport[4], dyn.viewport[5]};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        VkRect2D scissor{};
        scissor.offset = {std::max(0, int32_t(float(dyn.scissor.x) * half_factor_x)), std::max(0, int32_t(float(dyn.scissor.y) * half_factor_y))};
        scissor.extent = {uint32_t(std::ceil(float(dyn.scissor.width) * half_factor_x)), uint32_t(std::ceil(float(dyn.scissor.height) * half_factor_y))};
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdSetDepthBias(cmd, dyn.depth_bias_constant, dyn.depth_bias_clamp, dyn.depth_bias_slope);
        vkCmdSetBlendConstants(cmd, dyn.blend_constants);
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dyn.stencil_compare_mask);
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dyn.stencil_write_mask);
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dyn.stencil_reference);
        vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(dyn.system), dyn.system);
        ++pc.dynamic_sets, ++pc.push_constants;
      }
      const uint32_t offsets[2] = {uint32_t(ring_base + draw.vs_constants), uint32_t((draw.flags & kDrawOpPrevConstants) ? prev_constants_base + draw.vs_constants : ring_base + draw.fs_constants)};
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &constants_set, 2, offsets);
      ++pc.descriptor_binds;
      if (draw.textures != bound_textures) {
        VkDescriptorSet tex = GetTextureSet(frame, draw.textures, slot);
        if (!tex) continue;
        bound_textures = draw.textures;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 1, 1, &tex, 0, nullptr);
        ++pc.descriptor_binds;
      }
      const VkDeviceSize geometry_base = ring_base + AlignUp(frame.constants.count, 16);
      for (uint32_t b = 0; b < 2; ++b) {
        if (draw.vertex[b].buffer == kNoBuffer) continue;
        if (draw.vertex[b].buffer == bound_vertex[b].buffer && draw.vertex[b].offset == bound_vertex[b].offset) continue;
        bound_vertex[b] = draw.vertex[b];
        const bool frame_owned = draw.vertex[b].buffer == kFrameGeometry;
        VkBuffer buffer = frame_owned ? ring : buffers[draw.vertex[b].buffer].buffer;
        VkDeviceSize offset = draw.vertex[b].offset + (frame_owned ? geometry_base : 0);
        vkCmdBindVertexBuffers(cmd, b, 1, &buffer, &offset);
        ++pc.vertex_binds;
      }
      if (draw.index.buffer != kNoBuffer) {
        if (draw.index.buffer != bound_index.buffer || draw.index.offset != bound_index.offset || draw.index_32bit != bound_index_32) {
          bound_index = draw.index;
          bound_index_32 = draw.index_32bit;
          const bool frame_owned = draw.index.buffer == kFrameGeometry;
          VkBuffer buffer = frame_owned ? ring : buffers[draw.index.buffer].buffer;
          vkCmdBindIndexBuffer(cmd, buffer, draw.index.offset + (frame_owned ? geometry_base : 0), draw.index_32bit ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
          ++pc.index_binds;
        }
        vkCmdDrawIndexed(cmd, draw.count, draw.instances, draw.first, draw.base_vertex, 0);
        ++pc.indexed_draws, pc.indices += uint64_t(draw.count) * draw.instances;
      } else {
        vkCmdDraw(cmd, draw.count, draw.instances, draw.first, 0);
        ++pc.draws, pc.vertices += uint64_t(draw.count) * draw.instances;
      }
    }
    query_end();
    vkCmdEndRenderPass(cmd);
  }

  // (3) the add-back onto the full-size colour
  Transition(hc, 0, uint32_t(hc.states.size()), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, false);
  Transition(hd, 0, uint32_t(hd.states.size()), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, false);
  Transition(color, targets.color[0].mip * color.layers + targets.color[0].layer, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
             VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, false);
  FlushBarriers(cmd);
  {
    VkDescriptorSet set = prep.comp_set;
    FramebufferKey fk{};
    fk.pass = GetRenderPass(prep.comp_key);
    fk.views[0] = TargetView(color, targets.color[0].mip, targets.color[0].layer);
    fk.width = pass.width, fk.height = pass.height;
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = fk.pass;
    begin.framebuffer = GetFramebuffer(fk);
    begin.renderArea.extent = {pass.width, pass.height};
    vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    query_begin("half|composite");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, prep.comp_pipe);
    VkViewport viewport{0, 0, float(pass.width), float(pass.height), 0, 1};
    VkRect2D scissor{{0, 0}, {pass.width, pass.height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &constants_set, 2, const_offsets);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 1, 1, &set, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    query_end();
    vkCmdEndRenderPass(cmd);
    pc.pipeline_binds += 1, pc.descriptor_binds += 2, pc.dynamic_sets += 1, ++pc.push_constants, ++pc.draws, pc.vertices += 3;
  }
  if (options.counts) counts_cur.passes += 3;
}

// ---- S102 lane L1b (light_cubes.h, docs\renderer\PRELIGHT_DIRECT.md section 9): the light cubes written by one compute dispatch ----------------------------------------------------------------------
// Made at startup (Renderer::Create, only when RendererOptions::light_cube_compute asks: the start-time value of the switch), never at the first dispatch: a refusal then is known to the binder BEFORE it drops the
// slots' ops (review F2: compute_supported() is false until the pipelines exist), and the first entrance does not pay the compile. lc_state 0 untried, 1 ready, 2 failed.
bool Renderer::Impl::LightCubesInit() {
  if (lc_state != 0) return lc_state == 1;
  lc_state = 2;
  if (g_light_cube_plant.load(std::memory_order_relaxed) == 3) return false;   // planted (review F2): the init fails: every group must be declined, the cubes equal the stream's
  const auto t0 = std::chrono::steady_clock::now();
  VkDevice dev = device.device();
  if (!device.FormatHas(VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return false;
  VkDescriptorSetLayoutBinding bindings[7] = {};
  bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  for (uint32_t b = 1; b <= uint32_t(kMaxLightCubes) + 1; ++b) bindings[b] = {b, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};   // 1..5 the cubes, 6 the direction table
  VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  layout_info.bindingCount = 7;
  layout_info.pBindings = bindings;
  if (vkCreateDescriptorSetLayout(dev, &layout_info, nullptr, &lc_set_layout) != VK_SUCCESS) return false;
  VkPipelineLayoutCreateInfo pl_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl_info.setLayoutCount = 1;
  pl_info.pSetLayouts = &lc_set_layout;
  if (vkCreatePipelineLayout(dev, &pl_info, nullptr, &lc_layout) != VK_SUCCESS) return false;
  const auto make = [&](const uint32_t* words, size_t bytes, VkShaderModule& module, VkPipeline& pipeline) {
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = bytes, mi.pCode = words;
    if (vkCreateShaderModule(dev, &mi, nullptr, &module) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
    ci.layout = lc_layout;
    return vkCreateComputePipelines(dev, pipeline_cache, 1, &ci, nullptr, &pipeline) == VK_SUCCESS;
  };
  VkShaderModule unused_swap_module = VK_NULL_HANDLE;
  if (!make(kSpv_rdr_light_cubes_comp, sizeof(kSpv_rdr_light_cubes_comp), lc_module, lc_pipeline)) return false;
  if (!make(kSpv_rdr_light_dirs_comp, sizeof(kSpv_rdr_light_dirs_comp), lc_dirs_module, lc_dirs_pipeline)) return false;
  if (!make(kSpv_rdr_light_dirs_comp_swap, sizeof(kSpv_rdr_light_dirs_comp_swap), unused_swap_module, lc_dirs_pipeline_swap)) return false;
  vkDestroyShaderModule(dev, unused_swap_module, nullptr);   // (a pipeline keeps what it needs)
  const uint32_t n = uint32_t(slots.size());
  VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, n}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, n * (uint32_t(kMaxLightCubes) + 1)}};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = n, pool_info.poolSizeCount = 2, pool_info.pPoolSizes = sizes;
  if (vkCreateDescriptorPool(dev, &pool_info, nullptr, &lc_pool) != VK_SUCCESS) return false;
  std::vector<VkDescriptorSetLayout> layouts_(n, lc_set_layout);
  lc_sets.assign(n, VK_NULL_HANDLE);
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = lc_pool, alloc.descriptorSetCount = n, alloc.pSetLayouts = layouts_.data();
  if (vkAllocateDescriptorSets(dev, &alloc, lc_sets.data()) != VK_SUCCESS) return false;
  for (uint32_t s = 0; s < n; ++s) {   // the constants binding never changes: the ring, a dynamic offset per frame (five blocks of 18 vec4)
    VkDescriptorBufferInfo info{ring, 0, uint64_t(kMaxLightCubes) * kLightCubeVec4 * 16};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = lc_sets[s], write.dstBinding = 0, write.descriptorCount = 1, write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, write.pBufferInfo = &info;
    vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
  }
  // The direction table (review F7): 64x64 x 6 RGBA32F, written by rdr_light_dirs.comp when the texel -> grid mapping changes, read by every dispatch. A cube-compatible image so its sampled view is legal.
  ImageDesc table;
  table.width = table.height = uint16_t(kLightCubeSize), table.format = Format::RGBA32F, table.kind = ImageKind::Cube, table.usage = uint8_t(kUsageStorage | kUsageSampled);
  lc_table = self->CreateImage(table);
  if (lc_table == kNoImage) return false;
  lc_state = 1;
  Log("[rdr][light_cubes] compute pipelines made at startup in %.2f ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  return true;
}

void Renderer::Impl::RecordLightCubes(VkCommandBuffer cmd, const DispatchOp& op, uint32_t slot_index, VkDeviceSize ring_base) {
  const uint32_t n = std::min<uint32_t>(op.cube_count, uint32_t(kMaxLightCubes));
  if (!n || lc_state != 1 || slot_index >= lc_sets.size()) {
    if (n) Problem("light_cubes_unavailable", "the compute pipeline is not made (%u cubes asked)", n);
    return;
  }
  ImageEntry* cube[kMaxLightCubes] = {};
  for (uint32_t k = 0; k < n; ++k) {
    const ImageId id = op.cube[k];
    if (id == kNoImage || id >= images.high_water() || !images[id].live) { Problem("light_cube_bad", "cube %u is not an image", k); return; }
    ImageEntry& e = images[id];
    if (e.desc.format != Format::RGBA8 || e.width != kLightCubeSize || e.height != kLightCubeSize || e.layers != 6 || e.desc.mips != 1 || !(e.vk_usage & VK_IMAGE_USAGE_STORAGE_BIT)) {
      Problem("light_cube_bad", "cube %u: format %d, %ux%u, %u layers, %u mips, storage %d (a light cube is 64x64 RGBA8 x 6 with storage usage)", k, int(e.desc.format), e.width, e.height, e.layers, unsigned(e.desc.mips), int(!!(e.vk_usage & VK_IMAGE_USAGE_STORAGE_BIT)));
      return;
    }
    cube[k] = &e;
  }
  const int plant = g_light_cube_plant.load(std::memory_order_relaxed);
  ImageEntry& table = images[lc_table];
  // the descriptors of this frame slot: the five cubes (bindings past the cube count repeat the last cube: every statically used binding must be valid) and the table
  VkDescriptorImageInfo infos[kMaxLightCubes + 1];
  VkWriteDescriptorSet writes[kMaxLightCubes + 1];
  for (uint32_t b = 0; b <= uint32_t(kMaxLightCubes); ++b) {
    ImageEntry& target = b < uint32_t(kMaxLightCubes) ? *cube[std::min(b, n - 1)] : table;
    infos[b] = {VK_NULL_HANDLE, TargetView(target, 0, 0, 6), VK_IMAGE_LAYOUT_GENERAL};
    writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[b].dstSet = lc_sets[slot_index], writes[b].dstBinding = 1 + b, writes[b].descriptorCount = 1, writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, writes[b].pImageInfo = &infos[b];
  }
  vkUpdateDescriptorSets(device.device(), uint32_t(kMaxLightCubes) + 1, writes, 0, nullptr);
  const uint32_t offset = uint32_t(ring_base + op.constants);
  // The direction table is made when the texel -> grid mapping (cube 0's K[15], read from this frame's constants) differs from the one it was made with (or the planted face axis changed), not every frame.
  float k15[4];
  std::memcpy(k15, ring_memory.mapped + ring_base + op.constants + 15 * 16, sizeof(k15));
  const bool swap = plant == 2;
  const bool regen = !lc_table_valid || std::memcmp(k15, lc_table_k, sizeof(k15)) != 0 || swap != lc_table_swap;
  if (regen) {
    Transition(table, 0, 6, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, true, true);
    FlushBarriers(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, swap ? lc_dirs_pipeline_swap : lc_dirs_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, lc_layout, 0, 1, &lc_sets[slot_index], 1, &offset);
    vkCmdDispatch(cmd, kLightCubeSize / kLightCubeGroups, kLightCubeSize / kLightCubeGroups, 6);
    if (options.counts) ++counts_cur.total.dispatches, ++counts_cur.total.pipeline_binds, ++counts_cur.total.descriptor_binds;
    std::memcpy(lc_table_k, k15, sizeof(k15));
    lc_table_valid = true, lc_table_swap = swap;
    ++lc_table_builds;
  }
  // ONE barrier flush for the cubes (every layer of every cube to GENERAL for the shader's writes; their old contents are never read: the dispatch writes every texel) and the table's write -> read
  Transition(table, 0, 6, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, false, true);
  for (uint32_t k = 0; k < n; ++k) Transition(*cube[k], 0, 6, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, true, true);
  FlushBarriers(cmd);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, lc_pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, lc_layout, 0, 1, &lc_sets[slot_index], 1, &offset);
  vkCmdDispatch(cmd, kLightCubeSize / kLightCubeGroups, kLightCubeSize / kLightCubeGroups, n * 6);
  if (options.counts) ++counts_cur.total.dispatches, ++counts_cur.total.pipeline_binds, ++counts_cur.total.descriptor_binds;
  if (plant == 1)   // planted: the barrier between the dispatch and the readers is left out (the state says the cubes are readable already): only validation sees it
    for (uint32_t k = 0; k < n; ++k)
      for (SubresourceState& s : cube[k]->states) s.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, s.stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, s.access = VK_ACCESS_SHADER_READ_BIT;
}

void Renderer::Impl::RecordCopy(VkCommandBuffer cmd, const CopyOp& copy) {
  const CopyGeom g = GeometryOf(copy);
  ImageEntry& source = *g.source;
  ImageEntry& dest = *g.dest;

  Transition(source, g.source_sub, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, false);
  Transition(dest, g.dest_sub, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, g.whole && dest.desc.mips == 1 && dest.layers == 1);
  FlushBarriers(cmd);

  if (g.depth_only && g_stencil_dead_poison.load(std::memory_order_relaxed)) {   // S102 R24-C poison: the destination's stencil is garbage after a depth-only copy
    const VkClearDepthStencilValue garbage{0.0f, 0xA5};
    const VkImageSubresourceRange srange{VK_IMAGE_ASPECT_STENCIL_BIT, copy.dest.mip, 1, copy.dest.layer, 1};
    vkCmdClearDepthStencilImage(cmd, dest.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &garbage, 1, &srange);
  }
  if (g.path == 1) {
    // Every copy the game makes (GAME_RULES_TEXTURES_FRAME C4): raw bits, same size. A depth copy stays a depth image and is read as depth.
    const VkImageCopy region = RegionOf(copy, g);
    vkCmdCopyImage(cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    CountCopy();  // S102 M1
  } else if (g.path == 2) {
    VkImageBlit blit{};
    blit.srcSubresource = {source.aspect, copy.source.mip, copy.source.layer, 1};
    blit.srcOffsets[0] = {g.source_x, g.source_y, 0};
    blit.srcOffsets[1] = {g.source_x + int32_t(g.source_w), g.source_y + int32_t(g.source_h), 1};
    blit.dstSubresource = {dest.aspect, copy.dest.mip, copy.dest.layer, 1};
    blit.dstOffsets[0] = {g.dest_x, g.dest_y, 0};
    blit.dstOffsets[1] = {g.dest_x + int32_t(g.dest_w), g.dest_y + int32_t(g.dest_h), 1};
    vkCmdBlitImage(cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, BlitFilterFor(source.desc.format, copy.filter == CopyFilter::Linear));
    CountCopy();  // S102 M1
  } else {
    Problem("copy_shape_unsupported", "format %d to %d, %ux%u to %ux%u", int(source.desc.format), int(dest.desc.format), g.source_w, g.source_h, g.dest_w, g.dest_h);
  }
}

// S102 G12: `count` (>= 2) adjacent plain copies of one source into distinct subresources of one destination (copy_batch.h Compatible): every layout change in ONE barrier, ONE
// vkCmdCopyImage with one region each. The regions are the ones RecordCopy would have recorded one by one (RegionOf), in the same order.
void Renderer::Impl::RecordCopyBatch(VkCommandBuffer cmd, const CopyOp* const* copies, const CopyGeom* geoms, uint32_t count) {
  ImageEntry& source = *geoms[0].source;
  ImageEntry& dest = *geoms[0].dest;
  VkImageCopy regions[rdr::kMaxBatch];
  for (uint32_t i = 0; i < count; ++i) {
    Transition(source, geoms[i].source_sub, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, false);
    Transition(dest, geoms[i].dest_sub, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, geoms[i].whole && dest.desc.mips == 1 && dest.layers == 1);
    regions[i] = RegionOf(*copies[i], geoms[i]);
  }
  const int plant = rdr::g_copy_batch_plant.load(std::memory_order_relaxed);
  if (plant != 2) {
    FlushBarriers(cmd);
  } else {  // plant 2: the barrier is left out (state kept as if it had run): only the validation layers see the layout error
    barriers.clear();
    barrier_src = barrier_dst = 0;
  }
  vkCmdCopyImage(cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, plant == 1 ? count - 1 : count, regions);   // (plant 1: the last region is not copied)
  CountCopy();  // S102 M1: one command, as counted
  rdr::NoteBatch(count);
}

bool Renderer::Impl::RecordPresent(VkCommandBuffer cmd, const PresentOp& present, FrameSlot& slot, VkDeviceSize ring_base, uint32_t& image_index) {
  ImageEntry& source = images[present.source.image];
  // Settings parity S10b: DLSS from the picture before the HUD, to the size the picture is shown at (the fit below, or the window with
  // present_fill); the present then adds the HUD back.
  bool dlss = false;
  if (options.dlss_mode >= 0 && device.dlss() && frame_dlss && frame_dlss->valid) {
    const bool dlss_turned = (swapchain.quarter_turns() & 1) != 0;
    const float shown_w0 = float(dlss_turned ? swapchain.extent().height : swapchain.extent().width);
    const float shown_h0 = float(dlss_turned ? swapchain.extent().width : swapchain.extent().height);
    const float src_w0 = float(present.source_rect.width ? present.source_rect.width : source.desc.width);
    const float src_h0 = float(present.source_rect.height ? present.source_rect.height : source.desc.height);
    const float fit0 = std::min(shown_w0 / src_w0, shown_h0 / src_h0);
    const uint32_t out_w = uint32_t(options.present_fill ? shown_w0 : std::floor(src_w0 * fit0 + 0.5f));
    const uint32_t out_h = uint32_t(options.present_fill ? shown_h0 : std::floor(src_h0 * fit0 + 0.5f));
    dlss = RecordDlss(cmd, slot, ring_base, out_w, out_h);
  }
  dlss_had_history = dlss;
  Transition(source, 0, uint32_t(source.states.size()), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, false);
  if (dlss) {  // DLSS's output and the pre-HUD picture read by the present
    ImageEntry& out = images[dlss_out];
    Transition(out, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, false);
    ImageEntry& before = images[frame_dlss->color];
    Transition(before, 0, uint32_t(before.states.size()), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
               VK_ACCESS_SHADER_READ_BIT, false);
  }
  if (overlay) {  // settings parity S11: the overlay's textures readable before the pass begins
    for (const auto& [id, image] : overlay_textures) {
      ImageEntry& texture = images[image];
      Transition(texture, 0, uint32_t(texture.states.size()), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_SHADER_READ_BIT, false);
    }
  }
  FlushBarriers(cmd);

  // Our own small constant block, at the top of this frame's ring section.
  const VkDeviceSize constants_offset = ring_base + kRingSection - kConstantRange;
  static constexpr float kTurns[4][4] = {{1, 0, 0, 1}, {0, -1, 1, 0}, {-1, 0, 0, -1}, {0, 1, -1, 0}};
  float constants[16] = {};
  std::memcpy(constants, kTurns[swapchain.quarter_turns() & 3], sizeof(float) * 4);
  constants[4] = float(present.source_rect.x) / float(source.desc.width);
  constants[5] = float(present.source_rect.y) / float(source.desc.height);
  constants[6] = float(present.source_rect.width) / float(source.desc.width);
  constants[7] = float(present.source_rect.height) / float(source.desc.height);
  // Settings parity S10a: SGSR's source facts (the image's real texels: scaled images are bigger than their desc) and its sharpness.
  constants[8] = 1.0f / float(source.width), constants[9] = 1.0f / float(source.height);
  constants[10] = float(source.width), constants[11] = float(source.height);
  constants[12] = options.sgsr_sharpness;
  if (overlay && overlay->width > 0 && overlay->height > 0) constants[13] = 2.0f / overlay->width, constants[14] = 2.0f / overlay->height;  // S11
  std::memcpy(ring_memory.mapped + constants_offset, constants, sizeof(constants));

  VkDescriptorSet set = VK_NULL_HANDLE;
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = slot.descriptors;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &textures_layout;
  if (vkAllocateDescriptorSets(device.device(), &alloc, &set) != VK_SUCCESS) {
    Problem("descriptor_pool_empty", "no room for the present set");
    return false;
  }
  VkDescriptorImageInfo infos[kMaxTextures];
  ImageEntry& dummy = images[dummy_image];
  for (uint32_t i = 0; i < kMaxTextures; ++i) infos[i] = {dummy_sampler, dummy.sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  infos[0] = {PresentSamplerFor(source), source.sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  infos[1] = {dummy_sampler, images[display_curve].sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  if (dlss) {  // S10b: rdr_present_dlss.frag's slots
    infos[0] = {PresentSamplerFor(images[dlss_out]), images[dlss_out].sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    infos[2] = {PresentSamplerFor(source), source.sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    infos[3] = {PresentSamplerFor(images[frame_dlss->color]), images[frame_dlss->color].sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  }
  WriteTextureSet(set, infos);

  // Last, so a failure above never leaves an acquired image unpresented.
  const uint32_t out_cap_h = out_cap_h_;  // S102 G2 (b): PerFrame, carried by the frame
  swapchain.NoteOutputCap(out_cap_h);
  const uint64_t acquire_t0 = RenderNow();  // S102 S0-3: a CPU wait on the display, not render work
  const bool acquired = swapchain.Acquire(slot.available, image_index);
  acquire_ns_frame += RenderNow() - acquire_t0;
  if (!acquired) return false;
  const bool out_capped = swapchain.ApplyOutputCap(out_cap_h, rdr::g_out_cap_plant.load(std::memory_order_relaxed));  // S102 G2 (b): extent() / framebuffer() / render_pass() are the capped target's from here
  present_capped_ = out_capped;
  rdr::g_out_frames.fetch_add(1, std::memory_order_relaxed);
  if (out_capped) rdr::g_out_cap_frames.fetch_add(1, std::memory_order_relaxed);

  PassClassKey class_key{};
  class_key.formats[0] = uint16_t(swapchain.format());
  class_key.color_count = 1;
  class_key.samples = 1;
  class_key.pad = out_capped ? 2 : 1;  // S102 G2 review F1: the capped pass (transfer-src final layout, 2 dependencies) is a different pass from the window's: its own class
  // its own class: pipelines here are built against the swapchain's render pass, whose external dependency makes it a different one
  const uint32_t present_class = GetPassClass(class_key);
  class_passes[present_class] = swapchain.render_pass();
  DrawState state{};
  // Settings parity S10a: SGSR only where the picture is made bigger on its way to the screen.
  const bool sgsr_turned = (swapchain.quarter_turns() & 1) != 0;
  const float sgsr_out_w = float(sgsr_turned ? swapchain.extent().height : swapchain.extent().width);
  const float sgsr_out_h = float(sgsr_turned ? swapchain.extent().width : swapchain.extent().height);
  const bool sgsr = options.present_sgsr && !out_bilinear_ /* S102 G2 (c): PerFrame, carried by the frame */ && (sgsr_out_w > constants[6] * float(source.width) * 1.01f ||
                                             sgsr_out_h > constants[7] * float(source.height) * 1.01f);
  {  // S102 S0-1: the swapchain's size, once (the boot's [gfx][applied] line cannot know it): the bench's profile line and a Surface log read it here
    static uint32_t said_w = 0, said_h = 0, said_dw = 0, said_dh = 0;
    if (said_w != swapchain.panel_extent().width || said_h != swapchain.panel_extent().height || said_dw != swapchain.extent().width || said_dh != swapchain.extent().height) {
      said_w = swapchain.panel_extent().width, said_h = swapchain.panel_extent().height, said_dw = swapchain.extent().width, said_dh = swapchain.extent().height;
      Log("[gfx][present] swapchain=%ux%u drawn=%ux%u picture=%ux%u sgsr=%d", said_w, said_h, said_dw, said_dh, uint32_t(constants[6] * float(source.width)),  // S102 G2 F6: the panel, and what the pass draws at (smaller while capped)
          uint32_t(constants[7] * float(source.height)), sgsr ? 1 : 0);
    }
  }
  state.program = dlss ? present_dlss_program : sgsr ? present_sgsr_program : present_program;  // S10b: DLSS replaces SGSR
  if (present_fetch_) {  // S102 R24-A (wwe2k14_rdr_present_fetch): exactly 1:1 reads its pixel with one texelFetch (rdr_present_fetch.frag); everything else keeps the program above
    using namespace rdr;
    const auto count = [](PresentFetchSlot s) { g_present_fetch_count[s].fetch_add(1, std::memory_order_relaxed); };
    const uint64_t frames = g_present_fetch_count[kPcFrames].fetch_add(1, std::memory_order_relaxed) + 1;
    if (census_flush::Due(g_cf_fetch) || frames % 600 == 0) {  // the census line, every 600 frames while the switch is on (strings are made here only)
      const auto get = [](PresentFetchSlot s) { return static_cast<unsigned long long>(g_present_fetch_count[s].load(std::memory_order_relaxed)); };
      Log("[rdr][present_fetch] frames %llu, taken %llu, declined %llu (by reason: fit %llu, sgsr/dlss %llu, cap %llu)", static_cast<unsigned long long>(frames), get(kPcTaken), get(kPcDeclFit) + get(kPcDeclSgsrDlss) + get(kPcDeclCap),
          get(kPcDeclFit), get(kPcDeclSgsrDlss), get(kPcDeclCap));
    }
    // The 1:1 test is made HERE, on the swapchain as this acquire left it (extent() is the capped target's while capped, so a cap declines on its own line first). The fragment at (x, y) reads texel (x, y), so:
    // the picture is the panel's size, the picture is shown whole at the origin (the fit fills the panel, no turn), and nothing replaces the program (SGSR / DLSS). The 'fit' arithmetic is RecordPresent's below.
    const int plant = g_present_fetch_plant.load(std::memory_order_relaxed);  // TEST ONLY (0 in the game): 2 = no fit test
    if (dlss || sgsr) {
      count(kPcDeclSgsrDlss);
    } else if (out_capped) {
      count(kPcDeclCap);
    } else {
      const uint32_t panel_w = swapchain.extent().width, panel_h = swapchain.extent().height;
      const float s_w = float(present.source_rect.width ? present.source_rect.width : source.desc.width);
      const float s_h = float(present.source_rect.height ? present.source_rect.height : source.desc.height);
      const float s_fit = std::min(float(panel_w) / s_w, float(panel_h) / s_h);
      const float v_w = options.present_fill ? float(panel_w) : std::floor(s_w * s_fit + 0.5f), v_h = options.present_fill ? float(panel_h) : std::floor(s_h * s_fit + 0.5f);
      const bool whole = present.source_rect.x == 0 && present.source_rect.y == 0 && (present.source_rect.width == 0 || present.source_rect.width == int32_t(source.desc.width)) &&
                         (present.source_rect.height == 0 || present.source_rect.height == int32_t(source.desc.height));
      const bool plain = present.source.mip == 0 && present.source.layer == 0 && source.layers == 1 && source.desc.samples == 1 && source.desc.kind == ImageKind::Image2D;
      const bool one_to_one = whole && plain && swapchain.quarter_turns() == 0 && source.width == panel_w && source.height == panel_h && v_w == float(panel_w) && v_h == float(panel_h);
      if (one_to_one || plant == 2) state.program = present_fetch_program, count(kPcTaken);
      else count(kPcDeclFit);
    }
  }
  if (sgsr) rdr::g_out_sgsr_frames.fetch_add(1, std::memory_order_relaxed);
  if (sgsr) {
    static bool said = false;
    if (!said) said = true, Log("[rdr] present through SGSR: %ux%u texels shown on %.0fx%.0f", uint32_t(constants[6] * float(source.width)),
                                uint32_t(constants[7] * float(source.height)), sgsr_out_w, sgsr_out_h);
  }
  state.vertex_layout = present_layout;
  state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  state.depth_compare = VK_COMPARE_OP_ALWAYS;
  state.blend[0].write_mask = 0xF;
  VkPipeline pipeline = GetPipeline(state, present_class);

  // Session 100 (Jarrod's phone test: "doesn't seem to be respecting a 16:9 aspect ratio, instead stretching the image to fill the
  // phone"): the frame keeps its own aspect (the source rectangle's: 16:9, or what the game's aspect setting renders), centred, black
  // bars from the pass's clear. The swapchain may be turned (quarter_turns: the phone's native portrait surface): the fit is made in
  // the displayed orientation and the viewport turned back with it.
  const float fb_w = float(swapchain.extent().width), fb_h = float(swapchain.extent().height);
  const bool turned = (swapchain.quarter_turns() & 1) != 0;
  const float shown_w = turned ? fb_h : fb_w, shown_h = turned ? fb_w : fb_h;
  const float src_w = float(present.source_rect.width ? present.source_rect.width : source.desc.width);
  const float src_h = float(present.source_rect.height ? present.source_rect.height : source.desc.height);
  const float fit = std::min(shown_w / src_w, shown_h / src_h);
  // Settings parity S5: with the screen-shape setting the reader drew the scene anamorphic (squeezed by the window's factor): stretched back here.
  const float view_shown_w = options.present_fill ? shown_w : std::floor(src_w * fit + 0.5f);
  const float view_shown_h = options.present_fill ? shown_h : std::floor(src_h * fit + 0.5f);
  const float vw = turned ? view_shown_h : view_shown_w, vh = turned ? view_shown_w : view_shown_h;
  const float vx = std::floor((fb_w - vw) * 0.5f), vy = std::floor((fb_h - vh) * 0.5f);
  VkViewport viewport{vx, vy, vw, vh, 0, 1};
  // S102 G1 lever 2 (present_noclear): the pass is begun here, after the viewport is known. The default clears the whole panel; the lever's pass has
  // no clear and the bars around the viewport (none when it covers the panel) are cleared by rectangles (present_clear.h).
  VkClearValue clear{};
  VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  const bool noclear = options.present_noclear && swapchain.render_pass_noclear() != VK_NULL_HANDLE;  // (the pass is made on this first ask, never for a player without the lever)
  begin.renderPass = noclear ? swapchain.render_pass_noclear() : swapchain.render_pass();
  begin.framebuffer = swapchain.framebuffer(image_index);
  begin.renderArea.extent = swapchain.extent();
  begin.clearValueCount = 1;
  begin.pClearValues = &clear;
  vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
  if (noclear) {
    PresentBar bars[4];
    // No present pipeline: nothing will be drawn into the viewport, so it is cleared too (a viewport of nothing leaves the whole panel to clear).
    const int bar_count = pipeline ? PresentBars(int32_t(fb_w), int32_t(fb_h), int32_t(vx), int32_t(vy), int32_t(vw), int32_t(vh), bars)
                                   : PresentBars(int32_t(fb_w), int32_t(fb_h), 0, 0, 0, 0, bars);
    VkClearAttachment attachment{VK_IMAGE_ASPECT_COLOR_BIT, 0, clear};
    for (int b = 0; b < bar_count; ++b) {
      VkClearRect rect{{{bars[b].x, bars[b].y}, {bars[b].w, bars[b].h}}, 0, 1};
      vkCmdClearAttachments(cmd, 1, &attachment, 1, &rect);
    }
    static bool said = false;
    if (!said) said = true, Log("[rdr] present without the full-panel clear: %d bar rectangle(s) cleared", bar_count);
  }
  if (pipeline) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  VkRect2D scissor{{0, 0}, swapchain.extent()};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  vkCmdSetDepthBias(cmd, 0, 0, 0);
  const float blend[4] = {0, 0, 0, 0};
  vkCmdSetBlendConstants(cmd, blend);
  vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
  vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
  vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
  const uint32_t offsets[2] = {uint32_t(constants_offset), uint32_t(constants_offset)};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &constants_set, 2, offsets);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 1, 1, &set, 0, nullptr);
  if (pipeline) vkCmdDraw(cmd, 3, 1, 0, 0);
  if (overlay && !overlay->draws.empty()) RecordOverlay(cmd, slot, ring_base, present_class);  // settings parity S11
  vkCmdEndRenderPass(cmd);  // (S102 G2: the capped picture's blit is recorded by the step loop, after a stamp of its own)
  return true;
}

// Settings parity S10b: 4x4 helpers (row-major m[r * 4 + c], column vectors: y = M x).
static bool Invert4x4(const float* m, float* out) {
  double inv[16], det;
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (std::abs(det) < 1e-20) return false;
  for (int i = 0; i < 16; ++i) out[i] = float(inv[i] / det);
  return true;
}
static void Mul4x4(const float* a, const float* b, float* out) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
}
static void Transpose4x4(const float* m, float* out) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out[c * 4 + r] = m[r * 4 + c];
}

// Settings parity S10b: DLSS for this present, outside any render pass: the camera's motion vectors (rdr_dlss_motion.frag, a pass into
// an RG16F image of the render size), then Streamline's evaluation into dlss_out (the shown size). False: not done (the present then
// shows the picture as without DLSS).
bool Renderer::Impl::RecordDlss(VkCommandBuffer cmd, FrameSlot& slot, VkDeviceSize ring_base, uint32_t out_w, uint32_t out_h) {
  const DlssInputs& in = *frame_dlss;
  ImageEntry& color = images[in.color];
  ImageEntry& depth = images[in.depth];
  if (!color.live || !depth.live || !color.image || !depth.image || !depth.depth) return false;
  const uint32_t w = color.width, h = color.height;
  if (depth.width != w || depth.height != h || !w || !h) {
    Problem("dlss_size_mismatch", "colour %ux%u, depth %ux%u", w, h, depth.width, depth.height);
    return false;
  }
  int mode = options.dlss_mode;
  if (out_w <= w || out_h <= h) mode = 0, out_w = w, out_h = h;  // not made bigger: DLAA at the render size
  // Our images (render size: the motion vectors; shown size: the output), made again when a size changes.
  if (dlss_motion == kNoImage || dlss_motion_w != w || dlss_motion_h != h) {
    if (dlss_motion != kNoImage) self->DestroyImage(dlss_motion);
    ImageDesc desc;
    desc.width = uint16_t(w), desc.height = uint16_t(h);
    desc.format = Format::RG16F;
    desc.usage = kUsageSampled | kUsageTarget;
    dlss_motion = self->CreateImage(desc);
    dlss_motion_w = w, dlss_motion_h = h;
  }
  if (dlss_out == kNoImage || dlss_out_w != out_w || dlss_out_h != out_h) {
    if (dlss_out != kNoImage) self->DestroyImage(dlss_out);
    ImageDesc desc;
    desc.width = uint16_t(out_w), desc.height = uint16_t(out_h);
    desc.format = Format::RGBA8;
    desc.usage = kUsageSampled | kUsageStorage;
    dlss_out = self->CreateImage(desc);
    dlss_out_w = out_w, dlss_out_h = out_h;
  }
  if (dlss_motion == kNoImage || dlss_out == kNoImage) return false;
  ImageEntry& motion = images[dlss_motion];
  ImageEntry& out = images[dlss_out];
  // Matrices: clip_to_prev = ViewProj(previous) * inverse(ViewProj(this)) (column vectors), for the shader as rows; Streamline wants row
  // vectors (v' = v * M): the transposes.
  float inv_vp[16], clip_to_prev[16], inv_prev[16], prev_to_clip[16];
  if (!Invert4x4(in.view_proj, inv_vp) || !Invert4x4(in.prev_view_proj, inv_prev)) return false;
  Mul4x4(in.prev_view_proj, inv_vp, clip_to_prev);
  Mul4x4(in.view_proj, inv_prev, prev_to_clip);
  // The motion pass's constants (their own block, before the overlay region).
  const VkDeviceSize constants_offset = ring_base + kRingSection - kConstantRange - kOverlayRange - kDlssConstants;
  float constants[24];
  std::memcpy(constants, clip_to_prev, sizeof(clip_to_prev));
  constants[16] = float(w), constants[17] = float(h), constants[18] = 1.0f / float(w), constants[19] = 1.0f / float(h);
  constants[20] = in.jitter[0], constants[21] = in.jitter[1], constants[22] = constants[23] = 0;
  std::memcpy(ring_memory.mapped + constants_offset, constants, sizeof(constants));
  Transition(motion, 0, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, true);
  Transition(depth, 0, uint32_t(depth.states.size()), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
             VK_ACCESS_SHADER_READ_BIT, false);
  ImageEntry* object_motion = nullptr;  // S10b: the wrestlers' own motion (the binder's velocity pass), when this frame has it
  if (in.object_motion != kNoImage && images[in.object_motion].live && images[in.object_motion].width == w && images[in.object_motion].height == h) {
    object_motion = &images[in.object_motion];
    Transition(*object_motion, 0, uint32_t(object_motion->states.size()), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
               VK_ACCESS_SHADER_READ_BIT, false);
  }
  FlushBarriers(cmd);
  RenderPassKey pass_key{};
  pass_key.formats[0] = uint16_t(motion.format);
  pass_key.load[0] = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  pass_key.store[0] = VK_ATTACHMENT_STORE_OP_STORE;
  pass_key.stencil_load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  pass_key.stencil_store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  pass_key.color_count = 1;
  pass_key.samples = 1;
  PassClassKey class_key{};
  class_key.formats[0] = uint16_t(motion.format);
  class_key.color_count = 1;
  class_key.samples = 1;
  FramebufferKey framebuffer_key{};
  framebuffer_key.pass = GetRenderPass(pass_key);
  framebuffer_key.views[0] = TargetView(motion, 0, 0);
  framebuffer_key.width = w, framebuffer_key.height = h;
  DrawState state{};
  state.program = dlss_motion_program;
  state.vertex_layout = present_layout;
  state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  state.depth_compare = VK_COMPARE_OP_ALWAYS;
  state.blend[0].write_mask = 0xF;
  VkPipeline pipeline = GetPipeline(state, GetPassClass(class_key));
  if (!pipeline) return false;
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = slot.descriptors;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &textures_layout;
  if (vkAllocateDescriptorSets(device.device(), &alloc, &set) != VK_SUCCESS) return false;
  VkDescriptorImageInfo infos[kMaxTextures];
  ImageEntry& dummy = images[dummy_image];
  for (uint32_t i = 0; i < kMaxTextures; ++i) infos[i] = {dummy_sampler, dummy.sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  infos[0] = {dummy_sampler, depth.sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  if (object_motion) infos[1] = {dummy_sampler, object_motion->sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};  // the wrestlers'
  constants[22] = object_motion ? 1.0f : 0.0f;
  std::memcpy(ring_memory.mapped + constants_offset, constants, sizeof(constants));
  WriteTextureSet(set, infos);
  VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  begin.renderPass = framebuffer_key.pass;
  begin.framebuffer = GetFramebuffer(framebuffer_key);
  begin.renderArea.extent = {w, h};
  vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  VkViewport viewport{0, 0, float(w), float(h), 0, 1};
  VkRect2D scissor{{0, 0}, {w, h}};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  const uint32_t offsets[2] = {uint32_t(constants_offset), uint32_t(constants_offset)};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &constants_set, 2, offsets);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 1, 1, &set, 0, nullptr);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRenderPass(cmd);
  // DLSS reads the three inputs and writes the output from compute shaders.
  Transition(motion, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, false);
  Transition(color, 0, uint32_t(color.states.size()), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
             VK_ACCESS_SHADER_READ_BIT, false);
  Transition(depth, 0, uint32_t(depth.states.size()), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
             VK_ACCESS_SHADER_READ_BIT, false);
  Transition(out, 0, 1, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, true);
  FlushBarriers(cmd);
  const auto describe = [](ImageEntry& e, VkImageLayout layout, VkImageView view) {
    streamline::Image i;
    i.image = e.image, i.view = view, i.memory = e.memory.memory, i.format = e.format, i.layout = layout, i.usage = e.vk_usage;
    return i;
  };
  streamline::Frame f;
  f.frame_index = uint32_t(dlss_count);
  f.width = w, f.height = h, f.out_width = out_w, f.out_height = out_h;
  f.mode = mode;
  f.color_in = describe(color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color.sampled_view);
  f.depth = describe(depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, depth.sampled_view);
  f.motion = describe(motion, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, motion.sampled_view);
  f.color_out = describe(out, VK_IMAGE_LAYOUT_GENERAL, out.sampled_view);
  Transpose4x4(in.view_proj, f.view_to_clip);
  Transpose4x4(inv_vp, f.clip_to_view);
  Transpose4x4(clip_to_prev, f.clip_to_prev_clip);
  Transpose4x4(prev_to_clip, f.prev_clip_to_clip);
  f.jitter[0] = in.jitter[0], f.jitter[1] = in.jitter[1];
  f.mvec_scale[0] = 1.0f / float(w), f.mvec_scale[1] = 1.0f / float(h);
  f.reset = in.reset || !dlss_had_history;
  std::string reason;
  if (!streamline::Evaluate(cmd, f, &reason)) {
    Problem("dlss_failed", "%s", reason.c_str());
    return false;
  }
  if (dlss_count++ == 0)
    Log("[rdr][dlss] first evaluation: %ux%u -> %ux%u, mode %d (0 DLAA), jitter %.3f %.3f", w, h, out_w, out_h, mode, in.jitter[0], in.jitter[1]);
  return true;
}

// Settings parity S11: the app's overlay over the whole window, in the present pass after the picture. Its vertices and indices go in this
// slot's kOverlayRange; the constant block (written by RecordPresent) carries the turn and 2 / width, 2 / height in sgsr.yz.
void Renderer::Impl::RecordOverlay(VkCommandBuffer cmd, FrameSlot& slot, VkDeviceSize ring_base, uint32_t present_class) {
  const OverlayList& list = *overlay;
  const VkDeviceSize vertex_bytes = list.vertices.size() * sizeof(OverlayVertex);
  const VkDeviceSize index_offset = AlignUp(vertex_bytes, 16);
  const VkDeviceSize index_bytes = list.indices.size() * sizeof(uint16_t);
  if (list.width <= 0 || list.height <= 0 || index_offset + index_bytes > kOverlayRange) {
    Problem("overlay_too_big", "%zu vertices, %zu indices", list.vertices.size(), list.indices.size());
    return;
  }
  const VkDeviceSize base = ring_base + kRingSection - kConstantRange - kOverlayRange;
  std::memcpy(ring_memory.mapped + base, list.vertices.data(), size_t(vertex_bytes));
  std::memcpy(ring_memory.mapped + base + index_offset, list.indices.data(), size_t(index_bytes));
  DrawState state{};
  state.program = overlay_program;
  state.vertex_layout = overlay_layout;
  state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  state.depth_compare = VK_COMPARE_OP_ALWAYS;
  state.blend[0] = {1, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE,
                    VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, 0xF};
  VkPipeline pipeline = GetPipeline(state, present_class);
  if (!pipeline) return;
  static bool said = false;
  if (!said) said = true, Log("[rdr] overlay drawn: %zu draws, %zu vertices over %.0fx%.0f", list.draws.size(), list.vertices.size(), list.width, list.height);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  const VkDeviceSize vertex_at = base;
  vkCmdBindVertexBuffers(cmd, 0, 1, &ring, &vertex_at);
  vkCmdBindIndexBuffer(cmd, ring, base + index_offset, VK_INDEX_TYPE_UINT16);
  const float fb_w = float(swapchain.extent().width), fb_h = float(swapchain.extent().height);
  VkViewport viewport{0, 0, fb_w, fb_h, 0, 1};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  // A clip rectangle in the overlay's space to framebuffer pixels: through clip space and the same turn as the vertices.
  static constexpr float kTurns[4][4] = {{1, 0, 0, 1}, {0, -1, 1, 0}, {-1, 0, 0, -1}, {0, 1, -1, 0}};
  const float* turn = kTurns[swapchain.quarter_turns() & 3];
  const auto to_fb = [&](float x, float y, float& out_x, float& out_y) {
    const float cx = x * 2.0f / list.width - 1.0f, cy = y * 2.0f / list.height - 1.0f;
    out_x = (cx * turn[0] + cy * turn[1] + 1.0f) * 0.5f * fb_w;
    out_y = (cx * turn[2] + cy * turn[3] + 1.0f) * 0.5f * fb_h;
  };
  uint32_t bound_texture = ~0u;
  for (const OverlayDraw& draw : list.draws) {
    if (!draw.count) continue;
    VkRect2D scissor{{0, 0}, swapchain.extent()};
    if (draw.clipped) {
      float x0, y0, x1, y1;
      to_fb(draw.clip[0], draw.clip[1], x0, y0);
      to_fb(draw.clip[2], draw.clip[3], x1, y1);
      const int32_t left = int32_t(std::max(0.0f, std::min(x0, x1))), top = int32_t(std::max(0.0f, std::min(y0, y1)));
      const int32_t right = int32_t(std::min(fb_w, std::max(x0, x1))), bottom = int32_t(std::min(fb_h, std::max(y0, y1)));
      if (right <= left || bottom <= top) continue;
      scissor = {{left, top}, {uint32_t(right - left), uint32_t(bottom - top)}};
    }
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    if (draw.texture != bound_texture) {
      const auto it = overlay_textures.find(draw.texture);
      const ImageId image = it != overlay_textures.end() ? it->second : overlay_textures[0];
      VkDescriptorSet set = VK_NULL_HANDLE;
      VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      alloc.descriptorPool = slot.descriptors;
      alloc.descriptorSetCount = 1;
      alloc.pSetLayouts = &textures_layout;
      if (vkAllocateDescriptorSets(device.device(), &alloc, &set) != VK_SUCCESS) {
        Problem("descriptor_pool_empty", "no room for an overlay set");
        return;
      }
      ImageEntry& texture = images[image];  // (made readable before the pass: RecordPresent)
      VkDescriptorImageInfo infos[kMaxTextures];
      ImageEntry& dummy = images[dummy_image];
      for (uint32_t i = 0; i < kMaxTextures; ++i) infos[i] = {dummy_sampler, dummy.sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      infos[0] = {PresentSamplerFor(texture), texture.sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      WriteTextureSet(set, infos);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 1, 1, &set, 0, nullptr);
      bound_texture = draw.texture;
    }
    vkCmdDrawIndexed(cmd, draw.count, 1, draw.first_index, draw.base_vertex, 0);
  }
}


void Renderer::Impl::Render(const Frame& frame, RenderMode mode) {
  frame_dlss = &frame.dlss;  // S10b
  upload_seal_now = frame.upload_seal;  // S102 R21-B: which sealed upload batches this frame applies (0 = all, as before)
  render_thread.store(std::hash<std::thread::id>()(std::this_thread::get_id()), std::memory_order_relaxed);  // S102 U1
  RenderTimes& rt = g_render_times;
  uint64_t t = RenderNow();
  const uint64_t render_t0 = t;  // S102 S0-3
  const RenderTimes rt0 = rt;    // S102 G5: this Render's own share of the cumulative timers (the 600-frame log resets rt at its end)
  out_collect_ns = out_ring_ns = out_end_ns = out_begin_ns = out_pipe_ns = out_pipe_n = 0;
  out_uploads = out_up_images = out_scaled = out_up_barriers = 0, out_up_bytes = 0, out_batched = false;
  out_up_by_producer[0] = out_up_by_producer[1] = out_up_by_producer[2] = 0;   // S103 PVR
  uint64_t fence_wait_ns = 0;
  auto lap = [&](uint64_t& acc) {
    const uint64_t now = RenderNow();
    acc += now - t;
    t = now;
  };
  const uint32_t slot_index = uint32_t(serial % slots.size());
  FrameSlot& slot = slots[slot_index];
  VkDevice dev = device.device();
  // S105 A9: after a real loss the slot's fence is reset every frame (below) but never submitted again, so it is never signalled: a really lost device returns from the wait at once, a
  // healthy one (the +dev test plant --wwe2k14_rdr_test_device_lost) would wait for ever. Once lost, wait for the device to go idle instead (at once on a lost device; on a healthy one it
  // lets the frames already in flight finish, so the retired resources below are not freed under them).
  // (review F1) the +dev test plant (--wwe2k14_rdr_test_device_lost=N) is set HERE, at the top, before the acquire / present steps: from frame serial N the renderer takes the REAL lost-device path on a
  // healthy device, and this frame's present step is skipped too (a present waiting on a semaphore no submit signals could block the queue for good).
  if (const int64_t plant_at = crumbs::g_test_lost_at.load(std::memory_order_relaxed); plant_at >= 0 && int64_t(serial) >= plant_at && !lost_serial)
    OnDeviceLost("test plant", int(VK_ERROR_DEVICE_LOST), false);
  const bool lost_for_good = lost_serial && !lost_simulated;
  if (lost_for_good) {
    if (!idle_after_loss) idle_after_loss = true, vkDeviceWaitIdle(dev);   // (review F3) once: later frames have nothing in flight
  } else if (rdr::g_fence_friendly.load(std::memory_order_relaxed)) {  // S102 H2: poll + sleep slices; a cap hit falls through to the blocking wait below
    VkFence fence = slot.fence;
    rdr::fence_wait::Wait([&] { return vkGetFenceStatus(dev, fence) != VK_NOT_READY; }, rdr::fence_wait::NowNs, rdr::fence_wait::SleepNs);
  }
  if (const VkResult waited = lost_for_good ? VK_SUCCESS : vkWaitForFences(dev, 1, &slot.fence, VK_TRUE, UINT64_MAX); waited != VK_SUCCESS) {
    if (waited == VK_ERROR_DEVICE_LOST) OnDeviceLost("vkWaitForFences", int(waited), false);  // S102 D1 (before the Problem: a RDR_DEV build stops on it, review F4)
    Problem("frame_fence_wait_failed", "VkResult %d (a lost device shows here first)", int(waited));  // S102 U1 (R9)
  }
  readback.OnSlotWaited(slot_index, serial);  // lane 232: this slot's older read-back copies are complete
  fence_wait_ns = RenderNow() - t;  // S102 S0-3
  lap(rt.wait);
  rt.retired_items += slot.retired.size();
  DrainRetired(slot);
  lap(rt.drain);
  ProcessDestroys(slot);
  if (rdr::g_lean_churn_every.load(std::memory_order_relaxed) | rdr::g_lean_audit_every.load(std::memory_order_relaxed)) LeanTool(slot, frame);   // S102 RT1 (tools only)
  lap(rt.destroys);
  vkResetCommandPool(dev, slot.pool, 0);
  vkResetDescriptorPool(dev, slot.descriptors, 0);
  if ((census_flush::Due(g_cf_lean) || serial % 600 == 0) && (rdr::g_render_lean.load(std::memory_order_relaxed) | rdr::g_lean_hooks.load(std::memory_order_relaxed))) LeanCensus();   // S102 RT1
  lap(rt.pools);
  {  // S102 G5: reading the finished frame's stamps is a driver call too
    const uint64_t collect_t0 = RenderNow();
    CollectGpuTime(slot);  // lane 269
    out_collect_ns = RenderNow() - collect_t0;
  }
  // S102 S0-3: the stamp switch is read ONCE here per frame (rdr::g_gpu_stamps is set from the per-frame switch table, rdr_sw::kGpuStamps, by the live sink). The pool is sized from the
  // pass count: a frame that wanted more grows it before this slot's next frame (its last result is read); a slot that has only the two-query pool grows when stamps first turn on.
  slot.ts_detail = false;
  if (slot.timestamps) {
    const bool want_stamps = rdr::g_gpu_stamps.load(std::memory_order_relaxed) || frame.group_min_run != 0;   // S102 R24-F: the group stamps need the per-step stamps (they chain between them)
    uint32_t cap = slot.ts_cap;
    if (want_stamps && cap < 192) cap = 192;
    if (slot.ts_need > cap) cap = (slot.ts_need + slot.ts_need / 4 + 63) & ~63u;
    if (options.gpu_stamp_plant == 9 && want_stamps) cap = 6;   // S103 GT test plant 9: a pool too small for the frame's steps: stamps are dropped, the bracket check must report it
    if (cap != slot.ts_cap) {
      vkDestroyQueryPool(dev, slot.timestamps, nullptr);
      slot.timestamps = VK_NULL_HANDLE;
      MakeStampPool(slot, cap);
      if (cap > 2) Log("[rdr][gpu] stamp pool %u queries (a frame wanted %u)", slot.ts_cap, slot.ts_need);
      slot.ts_need = 0;
    }
    slot.ts_detail = slot.timestamps && want_stamps && slot.ts_cap >= 4;
    if (frame.group_min_run && slot.timestamps && !slot.gs_pool) {   // S102 R24-F: the group pool, made once per slot (this slot's last frame is finished: its fence was waited)
      VkQueryPoolCreateInfo gq{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      gq.queryType = VK_QUERY_TYPE_TIMESTAMP, gq.queryCount = rdr::kGroupPoolCap;
      if (vkCreateQueryPool(dev, &gq, nullptr, &slot.gs_pool) != VK_SUCCESS) slot.gs_pool = VK_NULL_HANDLE;
    }
  }
  // S102 R24-F: only a frame whose every draw carries a group, with per-step stamps and a pool, is cut (else it is a plain stamped frame)
  slot.gs_frame = frame.group_min_run != 0 && frame.draw_groups.count == frame.draws.count && frame.draws.count != 0 && slot.ts_detail && slot.gs_pool != VK_NULL_HANDLE;
  slot.gs_count = 0, slot.gs_overflow = false, slot.gs_segs.clear(), slot.gs_passes.clear();
  if (store_on) {  // lane 276: a slice of the listed pipelines (menus / loading only), then the 30 s save
    const uint64_t store_t0 = RenderNow();
    if (store_slice_us && prebuild_allowed) PrebuildSlice(uint64_t(store_slice_us) * 1000ull, false);
    StoreMaybeSave();
    const uint64_t store_ns = RenderNow() - store_t0;
    rt.pipestore += store_ns;
    if (store_ns > 8000000ull) Log("[rdr][pipestore] slow frame: %.1f ms in the pipeline store this frame (slice or save)", double(store_ns) / 1e6);
    t = RenderNow();  // (not lapped into another field)
  }

  const VkDeviceSize ring_base = VkDeviceSize(slot_index) * kRingSection;
  const VkDeviceSize geometry_offset = AlignUp(frame.constants.count, 16);
  const VkDeviceSize prev_offset = AlignUp(geometry_offset + frame.geometry.count, kConstantAlign);  // S10b: the previous tick's constants
  prev_constants_base = ring_base + prev_offset;
  if (options.counts) counts_cur.Reset(uint32_t(serial));  // S102 M1: before the early return, so a dropped frame never reads as the frame before it
  if (prev_offset + frame.prev_constants.count + kConstantRange + kOverlayRange + kDlssConstants > kRingSection) {
    Problem("frame_ring_full", "constants %u, geometry %u bytes", frame.constants.count, frame.geometry.count);
    if (options.counts) counts_cur.dropped = 1;  // S102 M1
    frames_dropped.fetch_add(1, std::memory_order_relaxed);  // S103 CB1
    return;
  }
  ring_used_cur = uint64_t(frame.constants.count) + frame.geometry.count + frame.prev_constants.count;   // S103 PVR
  if (options.counts) counts_cur.ring_constants = frame.constants.count, counts_cur.ring_geometry = frame.geometry.count, counts_cur.ring_prev = frame.prev_constants.count;  // S102 M1
  const uint64_t ring_t0 = RenderNow();  // S102 G5
  if (frame.constants.count) std::memcpy(ring_memory.mapped + ring_base, frame.constants.data, frame.constants.count);
  if (frame.geometry.count) std::memcpy(ring_memory.mapped + ring_base + geometry_offset, frame.geometry.data, frame.geometry.count);
  if (frame.prev_constants.count) std::memcpy(ring_memory.mapped + prev_constants_base, frame.prev_constants.data, frame.prev_constants.count);
  out_ring_ns = RenderNow() - ring_t0;
  if (host_buffers.load(std::memory_order_relaxed)) {   // S103 PF2: the real reader record of the host-visible vertex copies: this Render reads every one its draws name (HostBufferIdle)
    for (uint32_t d = 0; d < frame.draws.count; ++d)
      for (int b = 0; b < 2; ++b) {
        const BufferId id = frame.draws[d].vertex[b].buffer;
        if (id == kNoBuffer || id == kFrameGeometry) continue;
        BufferEntry& e = buffers[id];
        if (e.host) std::atomic_ref<uint64_t>(e.last_read).store(serial + 1, std::memory_order_relaxed);
      }
  }
  if (frame.direct_record && serial + 1 > frame.direct_record + (kRendersPerRecord - 1)) {   // recorded at s = direct_record - 1, may be rendered at s .. s + kRendersPerRecord - 1
    direct_overruns.fetch_add(1, std::memory_order_relaxed);
    Problem("posed_direct_overrun", "a frame recorded at Render serial %llu is rendered at %llu (more than %u renders per record): the posed vertex copies it reads may be rewritten", static_cast<unsigned long long>(frame.direct_record - 1), static_cast<unsigned long long>(serial), unsigned(kRendersPerRecord));
  }

  frame_sets.assign(frame.texture_sets.count, VK_NULL_HANDLE);
  state_pipelines.assign(frame.states.count, {VK_NULL_HANDLE, 0});

  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  const uint64_t begin_t0 = RenderNow();  // S102 G5 (review F6): the begin + query reset + first stamps block, timed directly
  vkBeginCommandBuffer(slot.cmd, &begin);
  // S102 D1 round 6: the frame-top barrier (breadcrumbs.h). Before anything else in the buffer: every command submitted earlier on the queue (frame N-1, N-2, the
  // uploads) completes and its writes are visible before any command of this frame runs. Read once a frame; off = nothing recorded.
  frame_barrier_mode = crumbs::g_frame_top_barrier.load(std::memory_order_relaxed);
  if (frame_barrier_mode > 0) {
    if (crumbs::g_frame_barrier_plant.load(std::memory_order_relaxed) != 1) RecordFrameBarrier(slot.cmd, "top");
    const uint64_t n = crumbs::g_frame_barrier_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (census_flush::Due(g_cf_barrier) || n % 600 == 0)
      Log("[rdr][frame_top_barrier] mode %d, frames %llu, barriers %llu (one at the top of every frame%s)", frame_barrier_mode, static_cast<unsigned long long>(n),
          static_cast<unsigned long long>(crumbs::g_frame_barrier_count.load(std::memory_order_relaxed)), frame_barrier_mode >= 2 ? ", one before the present" : "");
  }
  // S102 S0-3. Detail (decided above): [0] top of the buffer, [1] at the colour-output stage, then a stamp after each run of same-class steps and a tail. [1] - [0] is the START GAP: how long
  // the GPU took to get from the top of this buffer to its first colour-output work (earlier submits still running, WDDM holding the batch, a semaphore wait). It is NOT a measure of the
  // display wait (see docs\renderer\GPU_STAMPS.md). Without detail only lane 269's two stamps are written.
  slot.ts_count = 0, slot.ts_need = 0, slot.ts_has_pass = false, slot.ts_presented = false;
  slot.arm_tag = rdr::g_arm_tag.load(std::memory_order_relaxed);  // S102 G10
  slot.ts_class.clear(), slot.ts_image.clear(), slot.ts_kind.clear(), slot.ts_src.clear(), slot.ts_pass.clear();
  slot.ts_serial = serial;  // S102 G11
  slot.ts_trace = rdr::g_frame_trace.load(std::memory_order_relaxed) != nullptr;
  acquire_ns_frame = 0;
  if (rdr::g_frame_trace.load(std::memory_order_relaxed)) {  // S102 G11: the occlusion pool of the frame trace (made once per slot, reset here)
    if (!slot.occ) {
      VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      qi.queryType = VK_QUERY_TYPE_OCCLUSION, qi.queryCount = 512;
      vkCreateQueryPool(dev, &qi, nullptr, &slot.occ);
    }
    if (slot.occ) vkCmdResetQueryPool(slot.cmd, slot.occ, 0, 512);
    slot.occ_count = 0, slot.occ_meta.clear(), slot.occ_serial = serial, slot.occ_pending = slot.occ != VK_NULL_HANDLE;
  }
  if (slot.timestamps) {  // S101 lane22
    vkCmdResetQueryPool(slot.cmd, slot.timestamps, 0, slot.ts_detail ? slot.ts_cap : 2);  // (only what the frame writes)
    if (slot.gs_frame) vkCmdResetQueryPool(slot.cmd, slot.gs_pool, 0, rdr::kGroupPoolCap);   // S102 R24-F
    vkCmdWriteTimestamp(slot.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, slot.timestamps, 0);
    slot.ts_count = 1;
    if (slot.ts_detail) {
      vkCmdWriteTimestamp(slot.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, slot.timestamps, 1);
      slot.ts_count = 2;
    }
  }
  {  // S102 D1: this frame's crumb (the switches read once a frame) and its first marker words: [0] the frame began, [1] the uploads
    crumb = nullptr, crumb_step = nullptr, marker_next = 0;
    plant5_done = false;  // S102 D1 plant 5: once a frame
    const int marker_mode = crumbs::g_marker.load(std::memory_order_relaxed);
    const bool marking = marker_mode > 0 && EnsureMarker();
    frame_marker = marking ? marker_mode : 0;
    if (crumbs::g_on.load(std::memory_order_relaxed)) {
      crumb = &crumb_ring.Begin(serial, slot_index, uint8_t(mode), uint8_t(marking ? marker_mode : 0));
      crumb->destroys = last_destroys;
    }
    last_destroys = 0;
    marker_next = 1, marker_open = 0;
    if (marking) {  // word 0 (the frame began), then marker 1 (the uploads)
      const uint32_t value = crumbs::MarkValue(serial);
      vkCmdUpdateBuffer(slot.cmd, marker_buffer, VkDeviceSize(slot_index) * crumbs::kMarkerWords * 4, 4, &value);
      MarkBegin(slot.cmd, slot_index);
    }
  }
  ps_count = 0, ps_names.clear(), ps_meta.clear(), ps_afoff.clear();  // S102 G10
  if (rdr::g_draw_census_stats.load(std::memory_order_relaxed) && rdr::g_draw_census.load(std::memory_order_relaxed) && options.instrument_stats) {
    if (!ps_pool) {
      VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      qi.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
      qi.queryCount = 4096;
      qi.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
      if (vkCreateQueryPool(dev, &qi, nullptr, &ps_pool) == VK_SUCCESS) ps_cap = 4096;
      else ps_pool = VK_NULL_HANDLE, Log("[rdr][census] no pipeline-statistics query pool on this device");
    }
    if (ps_pool) vkCmdResetQueryPool(slot.cmd, ps_pool, 0, ps_cap);
  }
  out_begin_ns = RenderNow() - begin_t0;
  last_slot = slot_index;
  lap(rt.retire);
  // S102 G5 (review F4): with the meter on a stamp goes JUST BEFORE ApplyUploads and one just after, so the uploads' span is the commands between them (not measured from [1], the colour-output stamp,
  // which would also hold anything the GPU does between the top of the buffer and the first upload). Both are class `other`: without the meter the uploads span falls in the FIRST RUN of steps (its class); with it, the span is split out into `other` (the roles then differ by that amount).
  slot.ts_up = 0, slot.ts_up_mid = 0;
  slot.ts_mark = rdr::g_gpu_mark_seq.load(std::memory_order_acquire);
  slot.ts_steps_rec = slot.ts_steps_closed = slot.ts_w_pre = slot.ts_w_closes = slot.ts_w_post = 0, slot.ts_census = false;   // S103 GT
  // S103 GT: the pair of stamps around ApplyUploads (their GPU span: the upload line of [rdr][gpu][uploads]) is also written in the stamp-per-step mode (--wwe2k14_rdr_gpu_pass_stamps), not only with the outside meter (which waits on fences)
  // S103 GT review: written whenever the frame is stamped (ts_detail), not only with the meter / pass stamps: the Surface bench sets gpu_stamps alone, and without the pair the uploads' GPU time was billed to the frame's first role.
  // A deliberate baseline change for every stamped run (both builds). The pair has its own kind (6, "uploads") so the tail row (kind 3, #0) keeps one meaning.
  const bool meter_stamps = slot.ts_detail;
  if (meter_stamps && slot.ts_count + 2 < slot.ts_cap) {
    vkCmdWriteTimestamp(slot.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.timestamps, slot.ts_count++);
    ++slot.ts_w_pre;
    slot.ts_class.push_back(kGpuOther);
    slot.ts_image.push_back(0), slot.ts_kind.push_back(6), slot.ts_src.push_back(255);  // S102 G11 (item 11): G5's meter stamps keep the per-stamp vectors the same length as ts_class (the census and the trace read them together)
    if (slot.ts_trace) slot.ts_pass.push_back(0xFFFFFFFFu);
    ++slot.ts_need;
  }
  ApplyUploads(slot.cmd, slot);
  if (crumb) crumb->uploads = out_uploads;  // S102 D1
  if (meter_stamps && out_uploads && slot.ts_count + 1 < slot.ts_cap && slot.ts_count > 2) {  // S102 G5: the uploads' own span on the GPU
    vkCmdWriteTimestamp(slot.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.timestamps, slot.ts_count);
    slot.ts_up = slot.ts_count++;
    ++slot.ts_w_post;
    slot.ts_class.push_back(kGpuOther);
    slot.ts_image.push_back(0), slot.ts_kind.push_back(6), slot.ts_src.push_back(255);  // S102 G11 (item 11): G5's meter stamps keep the per-stamp vectors the same length as ts_class (the census and the trace read them together)
    if (slot.ts_trace) slot.ts_pass.push_back(0xFFFFFFFFu);
    ++slot.ts_need;
  }
  lap(rt.uploads);

  Compile(frame);
  slot.sd_arm = sd_on;   // S102 R24-C review F4
  lap(rt.compile);
  rt.passes += passes.size();
  rt.steps += steps.size();
  if (options.counts) counts_cur.steps = uint32_t(steps.size());  // S102 M1
  // Lane 232: the read-back requests recorded into THIS frame. (The frame_ring_full early return above happens before this: a request queued for such a frame stays pending and lands in a later frame.)
  readback.TakePending(readback_taking);
  readback_recorded.assign(readback_taking.size(), 0);
  out_cap_h_ = frame.out_cap_h, out_bilinear_ = frame.out_bilinear;  // S102 G2: this frame's output levers, decided at record time
  present_fetch_ = frame.present_fetch;  // S102 R24-A
  if (frame.arena_overdraw) {  // S102 C4: the live census line (a Surface "no gain" is told apart from "took 0 frames"): every 600 frames while the switch is on
    const uint64_t n = ++colour_cuts::g_overdraw_frames;
    if (census_flush::Due(g_cf_overdraw) || n % 600 == 0)
      Log("[rdr][arena_overdraw] frames %llu, colour passes taken %llu, declined %llu (depth clear %llu, nothing eligible %llu, mixed direction %llu, stencil %llu), draws pre-drawn %llu, texture-alpha proof admitted %llu refused %llu, texture scans %llu images %.2f MB %.2f ms",
          static_cast<unsigned long long>(n), static_cast<unsigned long long>(colour_cuts::g_frames_taken.load()),
          static_cast<unsigned long long>(colour_cuts::g_declined_clear.load() + colour_cuts::g_declined_none.load() + colour_cuts::g_declined_direction.load() + colour_cuts::g_declined_stencil.load()),
          static_cast<unsigned long long>(colour_cuts::g_declined_clear.load()), static_cast<unsigned long long>(colour_cuts::g_declined_none.load()), static_cast<unsigned long long>(colour_cuts::g_declined_direction.load()),
          static_cast<unsigned long long>(colour_cuts::g_declined_stencil.load()), static_cast<unsigned long long>(colour_cuts::g_draws_prepassed.load()),
          static_cast<unsigned long long>(colour_cuts::g_overdraw_texture_taken.load()), static_cast<unsigned long long>(colour_cuts::g_overdraw_texture_refused.load()),
          static_cast<unsigned long long>(colour_cuts::g_scan_images.load()), double(colour_cuts::g_scan_bytes.load()) / 1048576.0, double(colour_cuts::g_scan_ns.load()) / 1e6);
  }
  arena_fragments_ = frame.arena_fragments;  // S102 R18-B3
  if (arena_fragments_) {  // the live census line (a Surface "no gain" is told apart from "took 0 frames"): every 600 frames while a bit is on
    const uint64_t n = ++arena_fragments::g_frames;
    if (census_flush::Due(g_cf_frag) || n % 600 == 0)
      Log("[rdr][arena_fragments] mask %u, frames %llu, colour passes taken %llu, draws blend-off %llu (%.1f a frame), draws widened %llu, declined %llu states (by reason: not a plain alpha blend %llu, other target blended %llu, float target %llu), exact proofs %llu inexact %llu",
          arena_fragments_, static_cast<unsigned long long>(n), static_cast<unsigned long long>(arena_fragments::g_passes_taken.load()), static_cast<unsigned long long>(arena_fragments::g_blend_off.load()),
          double(arena_fragments::g_blend_off.load()) / double(n),
          static_cast<unsigned long long>(arena_fragments::g_widened.load()), static_cast<unsigned long long>(arena_fragments::g_declined[0].load() + arena_fragments::g_declined[1].load() + arena_fragments::g_declined[2].load()),
          static_cast<unsigned long long>(arena_fragments::g_declined[0].load()), static_cast<unsigned long long>(arena_fragments::g_declined[1].load()), static_cast<unsigned long long>(arena_fragments::g_declined[2].load()),
          static_cast<unsigned long long>(arena_fragments::g_proof_exact.load()), static_cast<unsigned long long>(arena_fragments::g_proof_inexact.load()));
  }
  colour_cuts_ = frame.colour_cuts | (frame.arena_overdraw ? colour_cuts::kPrepass : 0u);  // S102 G10; S102 C4: the arena overdraw switch runs the same prepass planner (its rule adds the texture-alpha proof in the binder)
  bool presenting = false;
  uint32_t image_index = 0;
  // S102 S0-3: one stamp per RUN of steps of the same class (a stamp drains the pipe, so a stamp after every copy would cost real time): a run is closed (stamped) when the
  // next step has another class, after the first step when a plant is on, and after the last step. Its span goes to the run's class.
  bool run_open = false, planted = false;
  // S102 D1: the step loop's breadcrumb state; rplay --crumbs-selftest stalls this frame's GPU at a step (breadcrumbs.h)
  const int crumb_plant = crumbs::g_plant.load(std::memory_order_relaxed);
  const bool selftest_here = int64_t(serial) == crumbs::g_selftest_frame.load(std::memory_order_relaxed) && frame_marker > 0;
  const int64_t selftest_step = crumbs::g_selftest_step.load(std::memory_order_relaxed);
  if (selftest_here && !selftest_event) {
    VkEventCreateInfo ei{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    vkCreateEvent(dev, &ei, nullptr, &selftest_event);
  }
  bool selftest_stalled = false, crumb_prev_copy = false;
  uint8_t crumb_prev_class = 0xFF;
  uint32_t selftest_at_step = 0;
  uint16_t selftest_marker = crumbs::kNoMarker;
  uint8_t run_class = kGpuOther;
  const bool census = slot.ts_detail && (rdr::g_gpu_census.load(std::memory_order_relaxed) || rdr::g_gpu_pass_stamps.load(std::memory_order_relaxed) || slot.gs_frame);  // S102 G6; S102 R24-B: the live per-pass stamps take the census's stamp-per-step mode; S102 R24-F: so do the group stamps
  uint32_t run_image = 0;
  uint8_t run_kind = 3, run_src = 255;
  uint32_t run_pass = 0xFFFFFFFFu;  // S102 G11
  auto close_run = [&](uint8_t cls, bool plant_stamp = false) -> bool {   // S103 GT review: true when the stamp was written (false = the pool overflowed and the stamp was dropped)
    ++slot.ts_need;
    if (slot.ts_count + 1 < slot.ts_cap) {  // (the last query is the tail's)
      vkCmdWriteTimestamp(slot.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.timestamps, slot.ts_count++);
      ++slot.ts_w_closes;
      slot.ts_class.push_back(cls);
      slot.ts_image.push_back(plant_stamp ? 0 : run_image), slot.ts_kind.push_back(plant_stamp ? 4 : run_kind), slot.ts_src.push_back(plant_stamp ? 255 : run_src);
      if (slot.ts_trace) slot.ts_pass.push_back(plant_stamp ? 0xFFFFFFFFu : run_pass);  // S102 G11 (F9): only while the frame trace is open  // S102 G6 (kind 4 = a test plant's stamp)
    } else {
      ++role_window.overflow, ++role_run.overflow;  // (the pool grows before this slot's next frame)
      return false;
    }
    return true;
  };
  // S103 GT: a stamp that closes a STEP (the stamp-per-step mode counts them against the steps recorded: every pass bracketed = equal). Test plant 8 leaves the 2nd closing stamp of each frame out: that step's
  // time falls into the next step's span, the chain still adds up, and only the count (steps closed < steps recorded) shows it.
  slot.ts_census = census;
  uint32_t plant8_closes = 0;
  auto close_step = [&](uint8_t cls) {
    if (options.gpu_stamp_plant == 8 && census && ++plant8_closes == 2) return;
    if (close_run(cls) && census) ++slot.ts_steps_closed;   // counted only when the stamp was really written
  };
  auto class_of = [&](ImageId image) -> uint8_t { return image != kNoImage && image < images.high_water() ? std::atomic_ref<uint8_t>(images[image].gpu_class).load(std::memory_order_relaxed) : uint8_t(kGpuOther); };  // S102 R21-B (hazard 3)
  const bool copy_batch_on = rdr::g_copy_batch.load(std::memory_order_relaxed);  // S102 G12: read every frame
  for (size_t step_i = 0; step_i < steps.size(); ++step_i) {
    const Step& step = steps[step_i];
    if (step.kind != StepKind::Present) {
      if (mode == RenderMode::PersistentOnly && !step.persistent) continue;
      if (mode == RenderMode::DisplayOnly && step.persistent) continue;
      if (mode == RenderMode::NoPasses) continue;  // S102 lane KO (ko_draw_none): no pass, no copy; the present below still runs
    }
    // The class of this step (a pass: its first colour target, else its depth target; a copy: its destination; the present: output) and whether it records at all.
    uint8_t step_class = kGpuOther;
    bool records = true;
    if (step.kind == StepKind::Pass) {
      const PassPlan& plan = passes[step.index];
      step_class = class_of(plan.targets ? (plan.targets->color_count ? plan.targets->color[0].image : plan.targets->depth.image) : kNoImage);
    } else if (step.kind == StepKind::Copy) {
      step_class = class_of(frame.copies[step.index].dest.image);
    } else if (step.kind == StepKind::Dispatch) {   // S102 L1b: the first cube's class (the binder names the light cubes pre-light)
      step_class = class_of(frame.dispatches[step.index].cube[0]);
    } else {
      step_class = kGpuOutput;
      records = (windowed || offscreen_out) && !presenting && mode != RenderMode::PersistentOnly;
      if (lost_serial && !lost_simulated) records = false;  // S102 D1 (review F5): no acquire / present on a lost device (the acquire blocked 0.5-1.9 s on the Surface)
    }
    if (!records) continue;
    if (slot.ts_detail) ++slot.ts_steps_rec;   // S103 GT: the bracket check counts every recorded step
    if (slot.ts_detail) {
      uint32_t step_image = 0;  // S102 G6 (review: only while stamping): what the census names the step by
      uint8_t step_src = 255;
      if (step.kind == StepKind::Pass) {
        const PassPlan& tp = passes[step.index];
        step_image = tp.targets ? (tp.targets->color_count ? tp.targets->color[0].image : tp.targets->depth.image) : 0;
      } else if (step.kind == StepKind::Copy) {
        step_image = frame.copies[step.index].dest.image;
        step_src = class_of(frame.copies[step.index].source.image);
      } else if (step.kind == StepKind::Dispatch) {
        step_image = frame.dispatches[step.index].cube[0];   // S102 L1b
      }
      const uint8_t step_kind = step.kind == StepKind::Pass ? 0 : step.kind == StepKind::Copy ? 1 : step.kind == StepKind::Dispatch ? 5 : 2;   // (5 = a dispatch, S102 L1b)
      if (run_open && (step_class != run_class || census)) close_step(run_class);  // S102 G6: the census closes a run after every step
      run_class = step_class, run_open = true, run_image = step_image, run_kind = step_kind, run_src = step_src;
      run_pass = step.kind == StepKind::Pass ? step.index : 0xFFFFFFFFu;  // S102 G11
    }
    // S102 D1: the step's crumb and its marker word (breadcrumbs.h). Mode 1 marks the first step of each run of one GPU class and the present; mode 2 every
    // pass / dispatch / present and the first copy of a run of copies; mode 3 every step.
    crumbs::Step* cs = crumb && !(crumb_plant == 2 && step_i == 3) ? crumbs::Ring::Add(*crumb) : nullptr;  // (plant 2: the 4th step's crumb is left out)
    uint16_t step_marker = crumbs::kNoMarker;
    if (frame_marker >= 3 || (frame_marker == 2 && !(step.kind == StepKind::Copy && crumb_prev_copy)) ||
        (frame_marker == 1 && (step_class != crumb_prev_class || step.kind == StepKind::Present))) {
      step_marker = MarkBegin(slot.cmd, slot_index);
    }
    crumb_prev_copy = step.kind == StepKind::Copy, crumb_prev_class = step_class;
    if (cs) {
      cs->kind = step.kind == StepKind::Pass ? crumbs::kPass : step.kind == StepKind::Copy ? crumbs::kCopy : step.kind == StepKind::Dispatch ? crumbs::kDispatch : crumbs::kPresent;
      cs->cls = step_class, cs->step = uint32_t(step_i), cs->index = step.index, cs->marker = step_marker;
      if (step.kind == StepKind::Pass) {
        const PassPlan& cp = passes[step.index];
        cs->image = cp.targets ? (cp.targets->color_count ? cp.targets->color[0].image : cp.targets->depth.image) : 0;
      } else if (step.kind == StepKind::Copy) {
        cs->image = frame.copies[step.index].dest.image;
      } else if (step.kind == StepKind::Dispatch) {
        cs->image = frame.dispatches[step.index].cube[0];
      }
    }
    if (selftest_here && !selftest_stalled && int64_t(step_i) >= selftest_step && selftest_event && step_marker != crumbs::kNoMarker) {  // rplay --crumbs-selftest: the GPU stops here until the host sets the event
      // TEST ONLY, never in a player's frame: a host-set event waited on by a pending command buffer is invalid usage under today's spec (VUID-vkSetEvent-event-09543,
      // VUID-vkCmdWaitEvents-srcStageMask-01158), so the selftest arms run without validation. No flush of its own (review F1): it records the production marker stream.
      vkCmdWaitEvents(slot.cmd, 1, &selftest_event, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, nullptr, 0, nullptr, 0, nullptr);
      selftest_stalled = true, selftest_at_step = uint32_t(step_i), selftest_marker = step_marker;
      if (crumb_plant == 1 && cs) cs->marker = uint16_t(step_marker + 1);  // plant 1: the crumb names the wrong marker word (the decode must miss the step)
    }
    if (std::FILE* ra = crumbs::g_role_audit.load(std::memory_order_relaxed)) {  // S102 D1 offline instrument (rplay --role-audit): the step's images and their state before it
      const auto img = [&](ImageId id) {
        if (id == kNoImage || id >= images.high_water()) { std::fprintf(ra, "[0,0,0,0]"); return; }
        const ImageEntry& e = images[id];
        bool undef = !e.states.empty();
        for (const SubresourceState& st : e.states) undef &= st.layout == VK_IMAGE_LAYOUT_UNDEFINED;
        std::fprintf(ra, "[%u,%u,%u,%d]", unsigned(id), e.width, e.height, undef ? 1 : 0);
      };
      std::fprintf(ra, "{\"f\":%llu,\"s\":%u,\"m\":%d,", static_cast<unsigned long long>(serial), unsigned(step_i), int(mode));
      if (step.kind == StepKind::Pass) {
        const PassPlan& ap = passes[step.index];
        std::fprintf(ra, "\"k\":\"pass\",\"i\":%u,\"t\":[", step.index);
        for (uint32_t c = 0; ap.targets && c < ap.targets->color_count; ++c) { if (c) std::fputc(',', ra); img(ap.targets->color[c].image); }
        std::fprintf(ra, "],\"d\":");
        img(ap.targets ? ap.targets->depth.image : kNoImage);
        std::fprintf(ra, ",\"sl\":%d,\"ss\":%d,\"st\":%d,\"sc\":%d", int(ap.stencil_load), int(ap.stencil_store), ap.stencil_tests ? 1 : 0, ap.strip_stencil_clear ? 1 : 0);   // S102 R24-C: stencil load / store ops, a draw tests stencil, the in-pass clears are stripped
        std::fprintf(ra, ",\"load\":%d,\"r\":[", ap.targets && ap.targets->color_count ? int(ap.load[0]) : -1);
        for (uint32_t q = 0; q < ap.sampled_count; ++q) { if (q) std::fputc(',', ra); img(sampled[ap.first_sampled + q].image); }
        std::fprintf(ra, "]}\n");
      } else if (step.kind == StepKind::Copy) {
        const CopyOp& ac = frame.copies[step.index];
        std::fprintf(ra, "\"k\":\"copy\",\"i\":%u,\"src\":", step.index);
        img(ac.source.image);
        std::fprintf(ra, ",\"dst\":");
        img(ac.dest.image);
        std::fprintf(ra, ",\"asp\":%d,\"rect\":[%d,%d,%u,%u]}\n", std::find(depth_only_copies.begin(), depth_only_copies.end(), &ac) != depth_only_copies.end() ? 1 : 3, int(ac.dest_rect.x), int(ac.dest_rect.y), unsigned(ac.dest_rect.width), unsigned(ac.dest_rect.height));   // S102 R24-C: asp 1 = depth only, 3 = depth + stencil (colour copies say 3 too)
      } else if (step.kind == StepKind::Dispatch) {
        std::fprintf(ra, "\"k\":\"dispatch\",\"i\":%u,\"t\":[", step.index);
        img(frame.dispatches[step.index].cube[0]);
        std::fprintf(ra, "]}\n");
      } else {
        std::fprintf(ra, "\"k\":\"present\",\"i\":%u,\"src\":", step.index);
        img(frame.presents[step.index].source.image);
        std::fprintf(ra, "}\n");
      }
    }
    crumb_step = cs && step.kind == StepKind::Pass ? cs : nullptr;
    if (step.kind == StepKind::Pass) {
      RecordPass(slot.cmd, frame, passes[step.index], slot, ring_base);
      crumb_step = nullptr;  // S102 D1
      slot.ts_has_pass = true;
      if (slot.ts_detail) ++role_run.cls_passes[step_class], ++role_window.cls_passes[step_class];
    } else if (step.kind == StepKind::Copy) {
      const CopyOp& copy = frame.copies[step.index];
      // S102 G12 (copy_batch.h): adjacent plain copies of one source into distinct subresources of one destination go as one command with one barrier flush. Not while a read-back is
      // being taken (it reads after each copy), and a step this mode would not record ends the batch.
      uint32_t taken = 1;
      if (copy_batch_on && readback_taking.empty()) {
        CopyGeom geoms[rdr::kMaxBatch];
        rdr::BatchKey keys[rdr::kMaxBatch];
        const CopyOp* ops[rdr::kMaxBatch];
        const auto key_of = [](const CopyOp& c, const CopyGeom& g) {
          rdr::BatchKey k;
          k.source_image = c.source.image, k.source_mip = c.source.mip, k.source_layer = c.source.layer;
          k.dest_image = c.dest.image, k.dest_sub = g.dest_sub;
          k.plain = g.path == 1 && c.source.image != c.dest.image;
          return k;
        };
        rdr::StepView views[rdr::kMaxBatch];
        uint32_t cand = 0;
        for (size_t j = step_i; j < steps.size() && cand < rdr::kMaxBatch; ++j) {   // the candidates: this copy and the steps after it (rdr::BatchSpan decides where the batch ends)
          const Step& cs = steps[j];
          views[cand].is_copy = cs.kind == StepKind::Copy;
          views[cand].records = !((mode == RenderMode::PersistentOnly && !cs.persistent) || (mode == RenderMode::DisplayOnly && cs.persistent));
          if (views[cand].is_copy) {
            const CopyOp& cc = frame.copies[cs.index];
            geoms[cand] = GeometryOf(cc), keys[cand] = key_of(cc, geoms[cand]), ops[cand] = &cc;
            views[cand].key = keys[cand];
          }
          ++cand;
          if (!views[cand - 1].is_copy) break;
        }
        const uint32_t n = rdr::BatchSpan(views, cand);
        if (n >= 2) RecordCopyBatch(slot.cmd, ops, geoms, n), taken = n;
      }
      if (cs && taken >= 2) cs->kind = crumbs::kCopyBatch, cs->draws = taken;  // S102 D1: one crumb for the batch
      if (taken < 2) {
        RecordCopy(slot.cmd, copy);
        if (!readback_taking.empty()) RecordReadbacksAfterCopy(slot.cmd, copy);
      }
      step_i += taken - 1;
    } else if (step.kind == StepKind::Dispatch) {   // S102 L1b
      RecordLightCubes(slot.cmd, frame.dispatches[step.index], slot_index, ring_base);
    } else {
      if (frame_barrier_mode >= 2 && crumbs::g_frame_barrier_plant.load(std::memory_order_relaxed) != 1) RecordFrameBarrier(slot.cmd, "present"), frame_barrier_end_done = true;  // S102 D1 round 6 (mode 2)
      presenting = RecordPresent(slot.cmd, frame.presents[step.index], slot, ring_base, image_index);
      slot.ts_presented = presenting;
      if (presenting && present_capped_) {  // S102 G2 (b), review F5: a stamp before and after the blit, so its ms shows apart (it is part of 'output' too)
        if (slot.ts_detail) close_step(kGpuOutput), run_open = false;
        swapchain.RecordCapBlit(slot.cmd, image_index);
        if (slot.ts_detail) close_run(kBlitTag);
      }
    }
    if (slot.ts_detail && (options.gpu_stamp_plant == 3 || options.gpu_stamp_plant == 4) && !planted) {  // test plants, once a frame after the first step: 3 an empty pass (plane role), 4 a slow one (rim role)
      planted = true;
      close_step(run_class);   // (a real step's closing stamp: counted as one)
      run_open = false;
      if (options.gpu_stamp_plant == 4) RecordStampPlant(slot.cmd);
      close_run(options.gpu_stamp_plant == 4 ? kGpuRim : kGpuPlane, true);  // (labelled as a plant in the census)
    }
  }
  if (slot.ts_detail && run_open) close_step(run_class);
  if (options.gpu_stamp_plant == 10 && slot.ts_detail && slot.ts_count + 1 < slot.ts_cap) {   // S103 GT test plant 10: a stamp written outside every counted site: the stamp-count identity must break
    vkCmdWriteTimestamp(slot.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.timestamps, slot.ts_count++);
    slot.ts_class.push_back(kGpuOther), slot.ts_image.push_back(0), slot.ts_kind.push_back(4), slot.ts_src.push_back(255);
    if (slot.ts_trace) slot.ts_pass.push_back(0xFFFFFFFFu);
    ++slot.ts_need;
  }
  RecordReadbacks(slot.cmd);  // lane 232: after every step, so the copy sees this frame's result
  if (options.gpu_stamp_plant == 5 && slot.timestamps) RecordStampPlant(slot.cmd);  // S102 S0-3 plant 5: a slow copy loop at the END of the frame: the NEXT frame's start gap must rise with no window
  if (slot.timestamps) {  // S101 lane22, lane 269; S102 S0-3: the tail stamp is the last query ([1] when not detailed); a render with no pass is no sample
    const uint32_t tail = slot.ts_detail ? slot.ts_count : 1;
    vkCmdWriteTimestamp(slot.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.timestamps, tail);
    if (slot.ts_detail) {
      slot.ts_class.push_back(kGpuOther);
      slot.ts_image.push_back(0), slot.ts_kind.push_back(3), slot.ts_src.push_back(255);  // S102 G6
      if (slot.ts_trace) slot.ts_pass.push_back(0xFFFFFFFFu);  // S102 G11 (F9)
      ++slot.ts_need;
    }
    if (slot.ts_detail) {   // S103 GT review: the stamps written must be exactly [0] + [1] + the upload pair + every closing stamp + the tail (a stamp written outside the counted sites breaks it); no GPU involved
      const bool ok = tail == 2 + slot.ts_w_pre + slot.ts_w_closes + slot.ts_w_post;
      for (RoleSums* sums : {&role_window, &role_run}) {
        ++sums->ident_frames, sums->ident_bad += ok ? 0 : 1;
        if (slot.ts_census) sums->steps_rec += slot.ts_steps_rec, sums->steps_closed += slot.ts_steps_closed, ++sums->census_frames;
      }
    }
    slot.ts_count = tail + 1;
    slot.ts_need += 2;  // [0] and [1]
    slot.ts_pending = true;
    slot.ts_submit_ns = RenderNow();
  }
  if (frame_barrier_mode >= 2 && !frame_barrier_end_done && crumbs::g_frame_barrier_plant.load(std::memory_order_relaxed) != 1) RecordFrameBarrier(slot.cmd, "end");  // S102 D1 round 6: no present this frame
  frame_barrier_end_done = false;
  if (frame_marker) {  // S102 D1: the frame's end marker (after the read-backs): the front end got through the whole buffer
    const uint16_t end_marker = MarkBegin(slot.cmd, slot_index);  // (closes the last step's marker)
    MarkClose(slot.cmd, slot_index);
    if (crumb) crumb->end_marker = end_marker;
  }
  const uint64_t end_t0 = RenderNow();  // S102 G5
  if (const VkResult ended = vkEndCommandBuffer(slot.cmd); ended != VK_SUCCESS) Problem("frame_record_failed", "vkEndCommandBuffer VkResult %d", int(ended));  // S102 U1 (R9)
  out_end_ns = RenderNow() - end_t0;
  lap(rt.record);

  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &slot.cmd;
  const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | (present_capped_ ? VK_PIPELINE_STAGE_TRANSFER_BIT : 0);  // S102 G2: the cap's blit writes the swapchain image at the transfer stage
  VkSemaphore finished = VK_NULL_HANDLE;
  if (presenting && !swapchain.offscreen()) {  // S102 G2: an offscreen output has no display to wait for or to signal
    finished = swapchain.finished_semaphore(image_index);
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &slot.available;
    submit.pWaitDstStageMask = &wait_stage;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &finished;
  }
  vkResetFences(dev, 1, &slot.fence);
  const bool meter_outside = rdr::g_gpu_outside.load(std::memory_order_relaxed) && slot.timestamps;  // S102 G5: read once a frame
  const uint64_t meter_t0 = meter_outside ? RenderNow() : 0;
  uint64_t meter_wait_ns = 0;  // S102 G5 (review F3): the instrument's own fence wait: idle time, like the slot's fence wait and the acquire
  if (options.gpu_stamp_plant == 6) {  // S102 G5 plant 6: GPU work OUTSIDE the stamped buffer: an extra submit of its own small buffer before the frame's (its copy loop is on the same queue)
    if (!slot.plant_cmd) {
      VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      alloc.commandPool = slot.pool, alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, alloc.commandBufferCount = 1;
      vkAllocateCommandBuffers(dev, &alloc, &slot.plant_cmd);
    }
    VkCommandBufferBeginInfo plant_begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    plant_begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(slot.plant_cmd, &plant_begin);
    RecordStampPlant(slot.plant_cmd);
    vkEndCommandBuffer(slot.plant_cmd);
    VkSubmitInfo plant_submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    plant_submit.commandBufferCount = 1, plant_submit.pCommandBuffers = &slot.plant_cmd;
    vkQueueSubmit(device.queue(), 1, &plant_submit, VK_NULL_HANDLE);
  }
  // S102 D1 (review F5): once the device is really lost nothing more is submitted (the fence stays unsignalled: a lost device's waits return at once).
  if (lost_serial && !lost_simulated) {
    if (crumb) crumb->submitted = false;
  } else if (const VkResult submitted = vkQueueSubmit(device.queue(), 1, &submit, slot.fence); submitted != VK_SUCCESS) {
    if (submitted == VK_ERROR_DEVICE_LOST) OnDeviceLost("vkQueueSubmit", int(submitted), false);  // S102 D1 (before the Problem, review F4)
    Problem("frame_submit_failed", "VkResult %d", int(submitted));  // S102 U1 (R9)
    frames_dropped.fetch_add(1, std::memory_order_relaxed);  // S103 CB1
  } else if (crumb) {
    crumb->submitted = true;
  }
  if (selftest_stalled) {  // S102 D1 (rplay --crumbs-selftest): wait until the marker words say the GPU reached the stalled step, run the lost-device handler as if lost, release the GPU
    const uint32_t* words = reinterpret_cast<const uint32_t*>(marker_memory.mapped) + size_t(slot_index) * crumbs::kMarkerWords;
    const uint32_t want = crumbs::MarkValue(serial);
    // render-clock-begin: dev - the stall self-test waits on a marker, never a picture value
    const auto t0 = std::chrono::steady_clock::now();  // the marker before the stalled one has its END written once everything before the stall finished
    while (selftest_marker >= 2 && selftest_marker <= crumbs::kMaxMarker && words[crumbs::EndWord(selftest_marker - 1u)] != want && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3))
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // render-clock-end
    std::this_thread::sleep_for(std::chrono::milliseconds(30));  // the front end is at the wait: nothing after it may be written
    if (!g_problem_nonfatal.load(std::memory_order_relaxed)) vkSetEvent(dev, selftest_event);  // --crumbs-fatal: the process stops at the Problem: release the GPU first (never leave it waiting)
    OnDeviceLost("selftest", int(VK_ERROR_DEVICE_LOST), true);
    const crumbs::SlotReach reach = crumbs::Reach(words, crumb ? crumb->end_marker : crumbs::kNoMarker);
    const crumbs::Step* named = crumb ? crumbs::StepOfMarker(*crumb, reach.unfinished) : nullptr;
    // The ring against the frame's own plan: every recorded step once, in order, with its kind, index and image.
    uint32_t recorded = 0, mismatched = 0, k = 0;
    for (size_t si = 0; si < steps.size(); ++si) {
      const Step& st = steps[si];
      if (st.kind != StepKind::Present) {
        if (mode == RenderMode::PersistentOnly && !st.persistent) continue;
        if (mode == RenderMode::DisplayOnly && st.persistent) continue;
        if (mode == RenderMode::NoPasses) continue;
      } else if (!((windowed || offscreen_out) && mode != RenderMode::PersistentOnly)) {
        continue;
      }
      ++recorded;
      const crumbs::Step* c = crumb && k < crumb->n ? &crumb->steps[k] : nullptr;
      const uint8_t want_kind = st.kind == StepKind::Pass ? crumbs::kPass : st.kind == StepKind::Copy ? crumbs::kCopy : st.kind == StepKind::Dispatch ? crumbs::kDispatch : crumbs::kPresent;
      if (!c || c->step != si || c->index != st.index || !(c->kind == want_kind || (want_kind == crumbs::kCopy && c->kind == crumbs::kCopyBatch))) { ++mismatched; continue; }
      if (c->kind == crumbs::kCopyBatch) si += c->draws - 1;
      ++k;
    }
    const bool ok = named && named->step == selftest_at_step && reach.serial_lo == want && !reach.done && crumb && mismatched == 0 && k == crumb->n;
    Log("[rdr][crumbs][selftest] frame %llu stalled at step %u (marker %u); decoded: slot %u frame word %08x, marker reached %d, step %d; ring: steps recorded %u, crumbs %u, mismatched %u; %s",
        static_cast<unsigned long long>(serial), selftest_at_step, unsigned(selftest_marker), slot_index, reach.serial_lo, reach.unfinished, named ? int(named->step) : -1, recorded,
        crumb ? crumb->n : 0, mismatched, ok ? "PASS" : "FAIL");
    Log("[rdr][crumbs][selftest] words: finished marker %d, front end reached %d", reach.ended, reach.begun);
    vkSetEvent(dev, selftest_event);
  }
  readback.MarkRecorded(slot_index, serial);  // lane 232: the recorded read-back copies are in flight in this slot
  if (ps_pool && ps_count) {  // S102 G10 (instrument): wait for this frame, read the statistics, fold them into the census rows by program name
    if (!(lost_serial && !lost_simulated)) vkWaitForFences(dev, 1, &slot.fence, VK_TRUE, UINT64_MAX);   // S105 A9 review F2: not on the fence a real loss leaves unsubmitted
    std::vector<uint64_t> values(size_t(ps_count) * 2);
    if (!(lost_serial && !lost_simulated) && vkGetQueryPoolResults(dev, ps_pool, 0, ps_count, values.size() * 8, values.data(), 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
      std::lock_guard<std::mutex> lock(g_draw_census_mutex);
      for (uint32_t q = 0; q < ps_count; ++q) {
        DrawCensusRow& row = g_draw_census_rows[ps_names[q]];
        row.vs_invocations += values[q * 2], row.fs_invocations += values[q * 2 + 1];
        if (q < ps_meta.size() && !ps_meta[q].empty())
          if (std::FILE* log = g_draw_log.load(std::memory_order_relaxed)) std::fprintf(log, "%s vs %llu fs %llu afoff %u\n", ps_meta[q].c_str(), static_cast<unsigned long long>(values[q * 2]), static_cast<unsigned long long>(values[q * 2 + 1]), unsigned(q < ps_afoff.size() ? ps_afoff[q] : 0));  // S102 C4; S102 R18-B3: afoff
      }
    }
    ps_count = 0;
  }
  if (options.gpu_timestamp_plant == 2) CollectGpuTime(slot);  // lane 269 plant: read at once, before the GPU is done; must count as not ready
  lap(rt.submit);
  slot.out_wall_ns = 0;
  if (meter_outside) {  // S102 G5 (the instrument): wait for this frame's fence now, so the submit-to-signal wall time is known; collected with the stamps a frame later (CollectGpuTime)
    const uint64_t wait_t0 = RenderNow();
    if (!(lost_serial && !lost_simulated)) vkWaitForFences(dev, 1, &slot.fence, VK_TRUE, UINT64_MAX);   // S105 A9 review F2: not on the fence a real loss leaves unsubmitted
    meter_wait_ns = RenderNow() - wait_t0;
    slot.out_wall_ns = RenderNow() - meter_t0;
    t = RenderNow();  // (the meter's wait is not lapped into any field: it is the instrument's own cost)
  }
  uint64_t present_call_ns = 0;
  if (presenting) {
    // S100 phone 120 fix: with display timing, each picture asks for the refresh after the last one asked for (or now): the halfway
    // picture and its tick land a refresh apart, as the old presenter paced Android (desired present times).
    // Fix 2 (run s100ph4): the 120 mode spaces them at its own rate (options.present_hz): Jarrod's phone reports a 60 Hz refresh while it
    // runs at 120, so the reported refresh held it at 60. Those ask half a slot early, so each lands on the refresh nearest its slot
    // (a slot right on a refresh would otherwise slip one refresh now and then).
    uint64_t desired = 0;
    const uint64_t spacing = options.present_hz ? 1000000000ull / options.present_hz : swapchain.refresh_ns();
    if (device.caps().display_timing && spacing) {
      const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
      const uint64_t slot = std::max(now, last_desired_ns + spacing);
      last_desired_ns = slot;
      desired = options.present_hz ? slot - spacing / 2 : slot;
    }
    const uint64_t present_t0 = RenderNow();  // S102 S0-3: the time the Present call itself takes (it can block on the display's queue)
    const bool accepted = swapchain.Present(image_index, finished, desired);
    present_call_ns = RenderNow() - present_t0;
    if (accepted || (crumb_plant == 3 && DeviceLost())) {  // lane 256: only an accepted present counts (S102 D1: none after a lost device; plant 3 = the old count, rplay's twin)
      g_presented.fetch_add(1, std::memory_order_relaxed);  // the overlay's shown rate
    }
  }
  if (const char* at = swapchain.lost_at(); at && !lost_serial) OnDeviceLost(at, int(VK_ERROR_DEVICE_LOST), false);  // S102 D1: the acquire (in the present step) or the present saw it first
  crumb = nullptr, crumb_step = nullptr;  // S102 D1: the frame's record is complete
  lap(rt.present);
  {  // S102 S0-3: what this Render spent in Renderer::Render alone (rdr_submit: all of it but the slot's fence wait and the swapchain acquire), and the acquire and Present calls apart.
    // (The live render THREAD's own CPU time, which also holds the bind and the parse, is counted in live.cpp's loop: g_render_thread_ns_total.)
    const uint64_t total = RenderNow() - render_t0;
    const uint64_t idle = fence_wait_ns + acquire_ns_frame + (options.gpu_stamp_plant == 7 ? 0 : meter_wait_ns);  // S102 G5 (F3): the meter's own wait is idle too (plant 7: it is left in the busy time)
    rdr::g_submit_busy_ns_total.fetch_add(total > idle ? total - idle : 0, std::memory_order_relaxed);
    rdr::g_submit_busy_calls.fetch_add(1, std::memory_order_relaxed);
    rdr::g_acquire_ns_total.fetch_add(acquire_ns_frame, std::memory_order_relaxed);
    rdr::g_present_call_ns_total.fetch_add(present_call_ns, std::memory_order_relaxed);
    win_acquire_ns += acquire_ns_frame, win_present_ns += present_call_ns, ++win_renders;
    run_acquire_ns += acquire_ns_frame, run_present_ns += present_call_ns, ++run_renders;
  }
  {  // S102 G5: this Render's CPU time at every point outside the command buffer (outside.h). The cumulative timers of g_render_times give the deltas; the rest are timed above.
    OutsideSums f;
    f.frames = 1;
    auto sub = [](uint64_t a, uint64_t b) { return a > b ? a - b : uint64_t(0); };
    f.ns[kOutFenceWait] = fence_wait_ns;
    f.ns[kOutDrain] = sub(rt.drain, rt0.drain);
    f.ns[kOutDestroys] = sub(rt.destroys, rt0.destroys);
    f.ns[kOutPools] = sub(rt.pools, rt0.pools);
    f.ns[kOutCollect] = out_collect_ns;
    f.ns[kOutPipeStore] = sub(rt.pipestore, rt0.pipestore);
    f.ns[kOutRingCopy] = out_ring_ns;
    f.ns[kOutBegin] = out_begin_ns;  // (timed directly; the stamp-pool regrow and the frame's set-up in the retire lap are not named)
    f.ns[kOutUploads] = sub(rt.uploads, rt0.uploads);
    f.ns[kOutCompile] = sub(rt.compile, rt0.compile);
    f.ns[kOutTextureSets] = sub(rt.set_ns, rt0.set_ns);
    f.ns[kOutFramebuffers] = sub(rt.fb_ns, rt0.fb_ns);
    f.ns[kOutPipelines] = out_pipe_ns;
    f.ns[kOutRecord] = sub(sub(sub(sub(sub(rt.record, rt0.record), f.ns[kOutTextureSets]), f.ns[kOutFramebuffers]), out_pipe_ns), out_end_ns + acquire_ns_frame);
    f.ns[kOutEnd] = out_end_ns;
    f.ns[kOutSubmit] = sub(rt.submit, rt0.submit);
    f.ns[kOutAcquire] = acquire_ns_frame;
    f.ns[kOutPresent] = present_call_ns;
    slot.ts_up_bytes = out_up_bytes;
    if (out_uploads) ++up_tot_frames, up_tot_images += out_up_images, up_tot_buffers += out_uploads - out_up_images, up_tot_barriers += out_up_barriers, up_tot_bytes += out_up_bytes, up_tot_by_producer[0] += out_up_by_producer[0], up_tot_by_producer[1] += out_up_by_producer[1], up_tot_by_producer[2] += out_up_by_producer[2];   // S103 UPL (+ PVR)
    if (slot.ts_detail) for (UpSums* u : {&up_window, &up_run}) {   // S103 GT: steady vs loading frames (stamped runs only: nothing is counted for a player)
      u->max_bytes = std::max<uint64_t>(u->max_bytes, out_up_bytes);
      u->ring_peak = std::max<uint64_t>(u->ring_peak, ring_used_cur);   // S103 PVR
      u->ring_ns += out_ring_ns, u->ring_bytes += ring_used_cur;   // S103 PF2
      if (out_uploads) {   // S103 UPL: calls and barriers of the frames that uploaded (a frame with no upload records none)
        for (int pk = 0; pk < 3; ++pk) u->prod_copies[pk] += out_up_by_producer[pk];   // S103 PVR
        ++u->up_frames, u->image_uploads += out_up_images, u->buffer_uploads += out_uploads - out_up_images, u->barriers += out_up_barriers, u->batched_frames += out_batched ? 1 : 0;
        u->max_barriers = std::max<uint64_t>(u->max_barriers, out_up_barriers), u->max_images = std::max<uint64_t>(u->max_images, out_up_images);
      }
      if (out_up_bytes > kLoadingBytes) ++u->load_frames, u->load_bytes += out_up_bytes;
      else ++u->steady_frames, u->steady_bytes += out_up_bytes;
    }
    f.uploads = out_uploads, f.scaled_uploads = out_scaled, f.upload_bytes = out_up_bytes, f.upload_barriers = out_up_barriers, f.pipelines_built = out_pipe_n;
    f.upload_batched_frames = out_batched ? 1 : 0;
    AddOutside(f);
  }
  if (options.counts) {  // S102 M1: the game's switch (--wwe2k14_rdr_counts) logs the mean counts a frame every 600 frames
    counts_sum.Add(counts_cur.total);
    counts_sum_extra += counts_cur.ring_constants + counts_cur.ring_geometry + counts_cur.ring_prev;
    counts_sum_passes += counts_cur.passes;
    if (++counts_sum_frames == 600) {
      const double n = 600.0;
      Log("[rdr][counts] 600 frames, a frame: %.1f passes, %.1f draws (%.1f indexed), %.1f pipeline binds, %.1f descriptor binds, %.1f dynamic sets, %.1f clears, %.1f copies, %.1f barriers, "
          "%.0f KB into the ring, %.0f vertices, %.0f indices, %.2f M pixels of area bound",
          counts_sum_passes / n, (counts_sum.draws + counts_sum.indexed_draws) / n, counts_sum.indexed_draws / n, counts_sum.pipeline_binds / n, counts_sum.descriptor_binds / n,
          counts_sum.dynamic_sets / n, counts_sum.clears / n, counts_sum.copies / n, counts_sum.barriers / n, counts_sum_extra / n / 1024.0, counts_sum.vertices / n, counts_sum.indices / n,
          counts_sum.area_px / n / 1e6);
      counts_sum = CallCounts{};
      counts_sum_extra = counts_sum_passes = counts_sum_frames = 0;
    }
  }
  std::atomic_ref<uint64_t>(serial).fetch_add(1, std::memory_order_relaxed);  // S102 R21-B (hazard 5): DestroyImage / DestroyBuffer read it on the bind stage
  if (++rt.frames == 600) {
    const double f = 600.0 * 1e6;
    Log("[rdr][render] 600 frames, ms a frame: wait for the slot %.3f, retire %.3f (drain %.3f, destroys %.3f, pool resets %.3f), uploads %.3f, "
        "compile %.3f, record %.3f (texture sets %.3f, new framebuffers %.3f), submit %.3f, present %.3f; a frame: %.0f passes, %.0f steps, %.0f KB "
        "uploaded, %.0f retired items, %.0f destroys, %.1f new framebuffers (%zu cached), %.0f texture sets",
        rt.wait / f, (rt.retire + rt.drain + rt.destroys + rt.pools) / f, rt.drain / f, rt.destroys / f, rt.pools / f, rt.uploads / f,
        rt.compile / f, rt.record / f, rt.set_ns / f, rt.fb_ns / f, rt.submit / f, rt.present / f, rt.passes / 600.0, rt.steps / 600.0,
        rt.upload_bytes / 600.0 / 1024.0, rt.retired_items / 600.0, rt.destroy_items / 600.0, rt.new_framebuffers / 600.0, framebuffers.size(),
        rt.texture_sets / 600.0);
    Log("[rdr][render] bc decoded %llu images, %.1f MB so far (%llu levels)%s", (unsigned long long)bc_images.load(), double(bc_decoded_bytes.load()) / 1048576.0,
        (unsigned long long)bc_decoded_levels.load(), bc_images.load() ? "" : " (none: the device samples BC, or bc_decode is off)");  // S102 U1
    if (rt.pipestore) Log("[rdr][render] pipeline store: %.3f ms a frame in the listed-pipeline slices and saves", rt.pipestore / f);
    rt = RenderTimes{};
  }
}

void Renderer::RenderFrame(const Frame& frame, RenderMode mode) { impl_->Render(frame, mode); }

// S102 G5: add one Render's points to the window and the run; with the meter on the window prints every 600 frames (the line GPU_OUTSIDE.md reads).
void Renderer::Impl::AddOutside(const OutsideSums& f) {
  for (OutsideSums* sums : {&out_window, &out_run}) {
    sums->frames += f.frames;
    for (uint32_t p = 0; p < kOutPointCount; ++p) sums->ns[p] += f.ns[p];
    sums->uploads += f.uploads, sums->scaled_uploads += f.scaled_uploads, sums->upload_bytes += f.upload_bytes, sums->upload_barriers += f.upload_barriers;
    sums->pipelines_built += f.pipelines_built, sums->upload_batched_frames += f.upload_batched_frames;
  }
  if (out_window.frames >= 600 || (census_flush::Due(g_cf_outside) && out_window.frames)) {
    if (rdr::g_gpu_outside.load(std::memory_order_relaxed)) Log("%s", OutsideLine("[rdr][outside] window:", out_window).c_str());
    out_window = OutsideSums{};
  }
}
const OutsideSums& Renderer::OutsideRun() const { return impl_->out_run; }  // S102 G5
uint64_t Renderer::FramesDropped() const { return impl_->frames_dropped.load(std::memory_order_relaxed); }  // S103 CB1
void Renderer::NoteFrameDropped() { impl_->frames_dropped.fetch_add(1, std::memory_order_relaxed); }

void Renderer::Impl::MakeStampPool(FrameSlot& slot, uint32_t cap) {  // S102 S0-3
  VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  query.queryType = VK_QUERY_TYPE_TIMESTAMP;
  query.queryCount = cap;
  slot.timestamps = VK_NULL_HANDLE;
  vkCreateQueryPool(device.device(), &query, nullptr, &slot.timestamps);
  slot.ts_cap = slot.timestamps ? cap : 0;
}

// S102 S0-3 test plant 4: a slow "pass" for the per-role line's twin check: 256 copies of 16 MB inside one scratch buffer (about 8 GB of traffic), each fenced from the
// last. Its stamp is added to the rim class, so it must show there and nowhere else.
void Renderer::Impl::RecordStampPlant(VkCommandBuffer cmd) {
  VkDevice dev = device.device();
  constexpr VkDeviceSize kHalf = 16ull << 20;
  if (!stamp_scratch) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = kHalf * 2;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    vkCreateBuffer(dev, &info, nullptr, &stamp_scratch);
    VkMemoryRequirements needs{};
    vkGetBufferMemoryRequirements(dev, stamp_scratch, &needs);
    if (!allocator.Alloc(needs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, true, stamp_scratch_memory)) {
      vkDestroyBuffer(dev, stamp_scratch, nullptr);
      stamp_scratch = VK_NULL_HANDLE;
      Log("[rdr][gpu] stamp plant 4: no memory for the scratch buffer");
      return;
    }
    vkBindBufferMemory(dev, stamp_scratch, stamp_scratch_memory.memory, stamp_scratch_memory.offset);
  }
  const VkBufferCopy region{0, kHalf, kHalf};
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  for (int i = 0; i < 256; ++i) {
    vkCmdCopyBuffer(cmd, stamp_scratch, stamp_scratch, 1, &region);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
  }
}

// Lane 269, S102 S0-3. Called after a slot's fence wait, so its last submit is complete and every result is available without waiting; a not-ready result
// is counted and dropped, never waited for. A render that recorded no pass is not a drawn frame and gives no sample (counted apart). With per-step stamps the
// GPU time splits: [0] -> [1] is the wait for the display, [1] -> the tail is the work, cut at each step's stamp into its class.
void Renderer::Impl::CollectGpuTime(FrameSlot& slot) {
  if (slot.occ_pending && slot.occ) {  // S102 G11: the pixels each frame-class draw wrote (read after the slot's fence wait, so never waits)
    slot.occ_pending = false;
    std::FILE* ft = rdr::g_frame_trace.load(std::memory_order_relaxed);
    uint64_t px[512];
    if (ft && slot.occ_count && vkGetQueryPoolResults(device.device(), slot.occ, 0, slot.occ_count, sizeof(px), px, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
      for (uint32_t q = 0; q < slot.occ_count; ++q)
        std::fprintf(ft, "{\"t\":\"o\",\"f\":%llu,\"p\":%u,\"op\":%u,\"px\":%llu}\n", static_cast<unsigned long long>(slot.occ_serial), slot.occ_meta[2 * q], slot.occ_meta[2 * q + 1], static_cast<unsigned long long>(px[q]));
  }
  if (!slot.ts_pending || !slot.timestamps) return;
  slot.ts_pending = false;
  if (slot.ts_detail && !slot.ts_has_pass) {  // (with stamps on only: the default gpu_ms counts every render as before)
    ++role_window.no_pass, ++role_run.no_pass;
    return;
  }
  const uint32_t count = slot.ts_count;
  uint64_t ticks[2 + 2 * 1024];
  if (count < 2 || count > sizeof(ticks) / sizeof(ticks[0])) {
    ++gpu_bad, ++gpu_bad_total;
    return;
  }
  const VkResult result = vkGetQueryPoolResults(device.device(), slot.timestamps, 0, count, count * sizeof(uint64_t), ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
  if (result != VK_SUCCESS) {
    ++gpu_not_ready, ++gpu_not_ready_total;
    return;
  }
  const uint64_t mask = ts_valid_bits >= 64 ? ~0ull : ((1ull << ts_valid_bits) - 1);
  const uint32_t last = count - 1;
  auto span = [&](uint32_t i, uint32_t j) { return double((ticks[j] - ticks[i]) & mask) * ts_period; };
  const double ns = span(0, last);
  const double wall_ns = double(RenderNow() - slot.ts_submit_ns);
  if (!(ns > 0.0) || ns > wall_ns) {  // sanity: (0, time since the submit]
    ++gpu_bad, ++gpu_bad_total;
    return;
  }
  if (slot.ts_detail && slot.ts_class.size() + 2 == count) {
    double cls_ns[kGpuClassCount] = {};
    double blit_ns = 0;  // S102 G2: the cap's blit span (also counted in cls_ns[kGpuOutput])
    bool sane = true;
    for (uint32_t k = 2; k <= last && sane; ++k) {
      const double step_ns = span(k - 1, k);
      if (step_ns > ns) sane = false;  // a stamp out of order shows as a huge wrapped span
      if (slot.ts_class[k - 2] == kBlitTag) cls_ns[kGpuOutput] += step_ns, blit_ns += step_ns;  // S102 G2
      else cls_ns[std::min<uint32_t>(slot.ts_class[k - 2], kGpuClassCount - 1)] += step_ns;
    }
    if (std::FILE* ft = rdr::g_frame_trace.load(std::memory_order_relaxed); ft && slot.ts_pass.size() == slot.ts_class.size()) {  // S102 G11: the GPU ms of each frame-class pass (needs the census: a stamp after every step)
      for (uint32_t k = 2; k <= last; ++k)
        if (slot.ts_class[k - 2] == kGpuFrame && slot.ts_kind[k - 2] == 0 && slot.ts_pass[k - 2] != 0xFFFFFFFFu)
          std::fprintf(ft, "{\"t\":\"g\",\"f\":%llu,\"p\":%u,\"ms\":%.5f}\n", static_cast<unsigned long long>(slot.ts_serial), slot.ts_pass[k - 2], span(k - 1, k) / 1e6);
    }
    if (slot.ts_image.size() == slot.ts_class.size() && rdr::g_gpu_pass_stamps.load(std::memory_order_relaxed)) {  // S102 R24-B: the per-step table (mean / p90 per step over the window)
      std::map<uint64_t, uint32_t> seen;  // occurrences of (class, kind, image) so far this frame = the step's index among its image's steps
      for (uint32_t k = 2; k <= last; ++k) {
        const uint64_t base = (uint64_t(slot.ts_class[k - 2]) << 48) | (uint64_t(slot.ts_kind[k - 2]) << 40) | slot.ts_image[k - 2];
        const uint32_t occ = std::min<uint32_t>(seen[base]++, 255);
        const bool plant5 = slot.sd_arm && k == 2 && g_stencil_dead_plant.load(std::memory_order_relaxed) == 5;   // plant 5: +1 ms on the first step of every ON frame (the ON column must show it, the OFF column not)
        pass_table[base | (uint64_t(occ) << 32) | (slot.sd_arm ? (1ull << 63) : 0)].push_back(float(span(k - 1, k) / 1e3 + (plant5 ? 1000.0 : 0.0)));
      }
      ++pass_table_frames;
      ++pass_table_frames_arm[slot.sd_arm ? 1 : 0];
    }
    if (slot.gs_frame && slot.ts_image.size() == slot.ts_class.size()) CollectGroups(slot, ticks, count, mask, ns);   // S102 R24-F: the group stamps of the colour / frame passes
    if (slot.ts_image.size() == slot.ts_class.size() && slot.ts_src.size() == slot.ts_class.size() && rdr::g_gpu_census.load(std::memory_order_relaxed)) {  // S102 G6
      for (uint32_t k = 2; k <= last; ++k) {
        CensusRow& row = census_run[(uint64_t(slot.ts_class[k - 2]) << 40) | (uint64_t(slot.ts_kind[k - 2]) << 32) | slot.ts_image[k - 2]];
        row.ms += span(k - 1, k) / 1e6, ++row.steps;
        if (slot.ts_kind[k - 2] == 1 && slot.ts_class[k - 2] == kGpuCopies && slot.ts_src[k - 2] < kGpuClassCount) {  // S102 G6 (item 7): the copies by what they copy FROM
          CensusRow& by_src = census_copy_src[slot.ts_src[k - 2]];
          by_src.ms += span(k - 1, k) / 1e6, ++by_src.steps;
        }
      }
      ++census_frames;
    }
    const double gap_ns = span(0, 1);
    if (!sane || gap_ns > ns) {
      ++gpu_bad, ++gpu_bad_total;
      return;
    }
    const double work_ns = ns - gap_ns;
    static uint64_t plant12_n = 0;
    const bool plant12_skip = options.gpu_stamp_plant == 12 && (++plant12_n & 1);   // S103 GT test plant 12: every other frame is left out of the role sums (the csv's GPU time still counts it): the rig's work-vs-csv check must fail
    if (slot.ts_mark != mark_win.seq) {   // S103 GT round 3: the first frame of a new route window: the finished one is logged
      FlushMarkWindow("");
      mark_win = MarkWin{};
      mark_win.seq = slot.ts_mark;
    }
    if (!plant12_skip) {
      MarkWin& w = mark_win;
      for (int c = 0; c < kGpuClassCount; ++c) w.cls_ns[c] += cls_ns[c];
      w.gap_ns += gap_ns, w.work_ns += work_ns, w.bracket_ns += ns, ++w.frames;
      if (slot.ts_up_bytes > kLoadingBytes) ++w.loading, w.loading_bytes += slot.ts_up_bytes;
      else ++w.steady, w.steady_bytes += slot.ts_up_bytes;
      if (slot.ts_up && slot.ts_up <= last) {
        const double u = span(slot.ts_up - 1 - (slot.ts_up_mid ? 1u : 0u), slot.ts_up);   // S103 PVR
        if (u < ns) w.up_gpu_ns += u, ++w.up_frames;
      }
    }
    if (!plant12_skip) for (RoleSums* sums : {&role_window, &role_run}) {
      for (int c = 0; c < kGpuClassCount; ++c) sums->cls_ms[c] += cls_ns[c] / 1e6;
      sums->blit_ms += blit_ns / 1e6;  // S102 G2
      sums->gap_ms += gap_ns / 1e6, sums->work_ms += work_ns / 1e6, sums->total_ms += ns / 1e6;
      ++sums->frames;
      sums->stamps += count;
      if ((windowed || offscreen_out) && !slot.ts_presented) ++sums->no_present;
    }
    if (rdr::g_arms_on.load(std::memory_order_relaxed)) {  // S102 G10 (review F1): kept per arm tag, only when rplay --arms asked (the game never grows this)
      std::lock_guard<std::mutex> lock(rdr::g_arm_mutex);
      ArmSamples& arm = rdr::g_arm_samples[slot.arm_tag];
      rdr::g_arm_kept.fetch_add(1, std::memory_order_relaxed);
      arm.colour.push_back(float(cls_ns[kGpuColor] / 1e6)), arm.work.push_back(float(work_ns / 1e6)), arm.total.push_back(float(ns / 1e6));
      arm.copies.push_back(float(cls_ns[kGpuCopies] / 1e6)), arm.shadow.push_back(float((cls_ns[kGpuShadow] + cls_ns[kGpuPlane] + cls_ns[kGpuRim]) / 1e6)), arm.other.push_back(float((cls_ns[kGpuPost] + cls_ns[kGpuCards] + cls_ns[kGpuFrame] + cls_ns[kGpuUi] + cls_ns[kGpuOutput] + cls_ns[kGpuPrelight] + cls_ns[kGpuFeedback] + cls_ns[kGpuOther]) / 1e6));
    }
    work_samples.push_back(float(work_ns / 1e6));
    gap_samples.push_back(float(gap_ns / 1e6));
    if (slot.ts_up && slot.ts_up <= last) {  // S102 G5: the uploads' own span (the stamp after ApplyUploads minus the one before it)
      const uint32_t up_pre = slot.ts_up - 1 - (slot.ts_up_mid ? 1u : 0u);   // S103 PVR: the mid stamp sits between the pair
      const double up_ns = span(up_pre, slot.ts_up);
      if (up_ns < ns && slot.ts_up_mid) {
        const double img_ns = span(up_pre, slot.ts_up_mid), buf_ns = span(slot.ts_up_mid, slot.ts_up);
        for (UpSums* u : {&up_window, &up_run}) u->mid_img_ns += img_ns, u->mid_buf_ns += buf_ns, ++u->mid_frames;
      }
      if (up_ns < ns) {
        for (OutsideSums* o : {&out_window, &out_run}) o->upload_gpu_ns += uint64_t(up_ns), ++o->upload_gpu_samples;
        for (UpSums* u : {&up_window, &up_run}) u->gpu_ns += up_ns, ++u->gpu_frames;   // S103 GT
      }
    }
    rdr::g_gpu_work_ns_total.fetch_add(uint64_t(work_ns), std::memory_order_relaxed);
  }
  if (slot.out_wall_ns) {  // S102 G5: the meter: submit-to-signal wall time less the stamped bracket = the GPU's time outside what the stamps cover
    for (OutsideSums* o : {&out_window, &out_run}) {
      o->gpu_wall_ns += slot.out_wall_ns, o->gpu_stamped_ns += uint64_t(ns), ++o->gpu_samples;
      o->gpu_outside_ns += slot.out_wall_ns > uint64_t(ns) ? slot.out_wall_ns - uint64_t(ns) : 0;
    }
    slot.out_wall_ns = 0;
  }
  gpu_samples.push_back(float(ns / 1e6));
  rdr::g_gpu_ns_total.fetch_add(uint64_t(ns), std::memory_order_relaxed);
  rdr::g_gpu_last_us.store(uint32_t(std::min(ns / 1000.0, 4.0e9)), std::memory_order_relaxed);
  rdr::g_gpu_frames.fetch_add(1, std::memory_order_relaxed);
  if (gpu_samples.size() >= 600) LogGpuWindow("600 frames");
}

// S102 R24-F: one frame's group stamps. `ticks` are the main pool's results (count of them), the group pool's are read here (never waited for: a not-ready result drops the frame's groups). Each instrumented pass is a chain of
// segments from its begin stamp to its closing stamp; the chain is checked (continuous, no wrapped span), its spans are added per dominant group, the draws per group come from the exact counts, and the draws the segments hold are
// compared with the draws the pass issued (coverage). Any failed check is counted and the frame's groups are not added to the table.
void Renderer::Impl::CollectGroups(FrameSlot& slot, const uint64_t* ticks, uint32_t count, uint64_t mask, double frame_ns) {
  uint64_t gt[rdr::kGroupPoolCap];
  const uint32_t n = std::min<uint32_t>(slot.gs_count, rdr::kGroupPoolCap);
  if (n && vkGetQueryPoolResults(device.device(), slot.gs_pool, 0, n, n * sizeof(uint64_t), gt, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
    ++group_not_ready;
    return;
  }
  const auto raw = [&](uint32_t id) { return (id & rdr::kPoolBit) ? gt[id & ~rdr::kPoolBit] : ticks[id]; };
  const auto span = [&](uint32_t a, uint32_t b) { return double((raw(b) - raw(a)) & mask) * ts_period; };
  std::map<uint32_t, const FrameSlot::GsPass*> by_end;
  for (const FrameSlot::GsPass& p : slot.gs_passes)
    if (p.end_stamp >= 2 && p.end_stamp < count && p.begin_stamp < p.end_stamp && p.end_stamp - 2 < slot.ts_kind.size() && slot.ts_kind[p.end_stamp - 2] == 0) by_end[p.end_stamp] = &p;
  if (by_end.size() != slot.gs_passes.size()) {   // a pass whose closing stamp was not written (the main pool ran out): not a frame to read
    ++group_chain_broken;
    return;
  }
  struct Row { uint64_t key; rdr::PassGroups g; const FrameSlot::GsPass* p; double direct_ns; };   // S103 GSTP: direct_ns = ticks[end] - ticks[begin] of the pass, read straight from the stamps
  std::vector<Row> rows;
  std::map<uint64_t, uint32_t> seen;
  bool ok = true;
  for (uint32_t k = 2; k < count; ++k) {   // the pass table's occurrence rule: the step's index among the steps of its image
    const uint64_t base = (uint64_t(slot.ts_class[k - 2]) << 48) | (uint64_t(slot.ts_kind[k - 2]) << 40) | slot.ts_image[k - 2];
    const uint32_t occ = std::min<uint32_t>(seen[base]++, 255);
    const auto it = by_end.find(k);
    if (it == by_end.end()) continue;
    const FrameSlot::GsPass& p = *it->second;
    const rdr::ChainResult chain = rdr::CheckChain(slot.gs_segs.data() + p.first_seg, p.segs, p.begin_stamp, p.end_stamp, frame_ns, span);
    Row row{base | (uint64_t(occ) << 32) | (slot.sd_arm ? (1ull << 63) : 0), {}, &p, span(p.begin_stamp, p.end_stamp)};
    for (uint32_t s = 0; s < p.segs; ++s) {
      const rdr::GroupSeg& seg = slot.gs_segs[p.first_seg + s];
      rdr::Aggregate(seg, span(seg.begin, seg.end), row.g);
    }
    const uint32_t held = row.g.total_draws;
    if (chain.broken) ++group_chain_broken, ok = false;
    if (held != p.issued) ++group_cover_bad, ok = false;
    {   // S103 GSTP (review fix 2): the groups' sum is ADDED UP from the segments' spans, the pass's own span is read straight from its two end stamps: independent readings of one pass, equal to rounding (a segment the chain lost, a stamp out of order or a wrapped span shows here)
      double sum = 0;
      for (uint8_t g = 0; g < rdr::kGrpCount; ++g) sum += row.g.ns[g];
      if (std::fabs(sum - row.direct_ns) > 2.0 * ts_period) ++group_sum_bad, ok = false;
    }
    rows.push_back(row);
  }
  if (!ok) return;
  for (const Row& row : rows) {
    GroupPassAcc& pa = group_pass[row.key];
    double pass_ns = 0;
    for (uint8_t g = 0; g < rdr::kGrpCount; ++g) pass_ns += row.g.ns[g];
    pa.direct_us += row.direct_ns / 1e3;   // S103 GSTP
    pa.us += pass_ns / 1e3, ++pa.frames, pa.draws += row.g.total_draws, pa.issued += row.p->issued, pa.mixed += row.g.mixed_draws, pa.segs += row.p->segs, pa.stamps += row.p->segs - 1;
    for (uint8_t g = 0; g < rdr::kGrpCount; ++g) {
      if (!row.g.draws[g] && !(row.g.ns[g] > 0)) continue;
      GroupAcc& a = group_table[row.key | (uint64_t(g) << 56)];
      a.us.push_back(float(row.g.ns[g] / 1e3));
      a.draws += row.g.draws[g];
    }
  }
  ++group_frames;
  group_stamps_total += slot.gs_count;
  if (slot.gs_overflow) ++group_overflow_frames;
}

void Renderer::Impl::FlushMarkWindow(const char* suffix) {   // S103 GT round 3
  const MarkWin& w = mark_win;
  if (!w.seq || !w.frames) return;
  std::string name;
  {
    std::lock_guard<std::mutex> lock(rdr::g_gpu_mark_mu);
    if (w.seq <= rdr::g_gpu_mark_names.size()) name = rdr::g_gpu_mark_names[w.seq - 1];
  }
  const double f = double(w.frames) * 1e6;   // ns -> ms a frame
  const double up = w.up_gpu_ns / f;   // ms a frame: the upload span summed over the frames that had one, spread over ALL the window's frames
  double other_net = w.cls_ns[kGpuOther] / f - up;
  if (other_net < 0) other_net = 0;
  const double stamped = w.work_ns + w.gap_ns;
  Log("[rdr][gpu][window] %s%s: GPU frames %llu; by role, mean ms a frame: colour %.3f, shadow %.3f, rim %.3f, plane %.3f, cards %.3f, post %.3f, output %.3f, ui %.3f, frame %.3f, copies %.3f, prelight %.3f, feedback %.3f, "
      "other %.3f, uploads %.3f = work %.3f; start gap [0]-[1] %.3f; bracket %.3f (the csv gpu_ms basis: [0] to the tail stamp of every frame); unstamped %.3f (bracket - work - gap); "
      "uploads steady (<= %llu KB) frames %llu = %.3f MB/frame, loading frames %llu = %.3f MB/frame",
      name.c_str(), suffix, static_cast<unsigned long long>(w.frames), w.cls_ns[kGpuColor] / f, w.cls_ns[kGpuShadow] / f, w.cls_ns[kGpuRim] / f, w.cls_ns[kGpuPlane] / f, w.cls_ns[kGpuCards] / f, w.cls_ns[kGpuPost] / f,
      w.cls_ns[kGpuOutput] / f, w.cls_ns[kGpuUi] / f, w.cls_ns[kGpuFrame] / f, w.cls_ns[kGpuCopies] / f, w.cls_ns[kGpuPrelight] / f, w.cls_ns[kGpuFeedback] / f, other_net, up, w.work_ns / f, w.gap_ns / f, w.bracket_ns / f,
      (w.bracket_ns - stamped) / f, static_cast<unsigned long long>(kLoadingBytes >> 10), static_cast<unsigned long long>(w.steady), w.steady ? double(w.steady_bytes) / 1048576.0 / double(w.steady) : 0.0,
      static_cast<unsigned long long>(w.loading), w.loading ? double(w.loading_bytes) / 1048576.0 / double(w.loading) : 0.0);
}

void Renderer::Impl::LogGpuWindow(const char* why) {
  if (!std::strcmp(why, "last window") || !std::strcmp(why, "summary")) FlushMarkWindow(" (open at exit)");   // S103 GT round 3: the window that was still open
  if (gpu_samples.empty() && !gpu_bad && !gpu_not_ready && !role_window.no_pass) return;
  std::vector<float> v = gpu_samples;
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  double sum = 0;
  for (float x : v) sum += x;
  Log("[rdr][gpu] %s: frames %zu, gpu ms median %.3f / mean %.3f / p95 %.3f / max %.3f; dropped %llu implausible (outside 0..wall time), %llu not ready",
      why, n, n ? v[n / 2] : 0.0f, n ? sum / double(n) : 0.0, n ? v[std::min(n - 1, size_t(double(n) * 0.95))] : 0.0f, n ? v[n - 1] : 0.0f,
      static_cast<unsigned long long>(gpu_bad), static_cast<unsigned long long>(gpu_not_ready));
  if (role_window.frames) {  // S102 S0-3: by role, the start gap apart; the means are over the frames with stamps, absent roles count 0, so the roles add up to the work
    auto median = [](std::vector<float>& s) {
      if (s.empty()) return 0.0;
      std::sort(s.begin(), s.end());
      return double(s[s.size() / 2]);
    };
    const double f = double(role_window.frames);
    const RoleSums& r = role_window;
    Log("[rdr][gpu] %s by role, mean ms a frame over %llu frames: colour %.3f, shadow %.3f, rim %.3f, plane %.3f, cards %.3f, post %.3f, output %.3f, ui %.3f, frame %.3f, copies %.3f, prelight %.3f, feedback %.3f, other %.3f = work %.3f (median %.3f)"
        " + start gap [0]-[1] %.3f (median %.3f) = %.3f (the bracket above); a render spends %.3f ms in vkAcquireNextImage and %.3f ms in the Present call; renders with no pass %llu,"
        " with no present (a window is up) %llu, stamp overflows %llu, %.1f stamps a frame; passes a frame by class: colour %.1f, shadow %.1f, rim %.1f, plane %.1f, cards %.1f, post %.1f, output %.1f, ui %.1f, frame %.1f, copies %.1f, prelight %.1f, feedback %.1f, other %.1f; of 'output' the capped blit %.3f ms",  // S102 G2 F5 + G6
        why, static_cast<unsigned long long>(r.frames), r.cls_ms[kGpuColor] / f, r.cls_ms[kGpuShadow] / f, r.cls_ms[kGpuRim] / f, r.cls_ms[kGpuPlane] / f, r.cls_ms[kGpuCards] / f,
        r.cls_ms[kGpuPost] / f, r.cls_ms[kGpuOutput] / f, r.cls_ms[kGpuUi] / f, r.cls_ms[kGpuFrame] / f, r.cls_ms[kGpuCopies] / f, r.cls_ms[kGpuPrelight] / f, r.cls_ms[kGpuFeedback] / f, r.cls_ms[kGpuOther] / f, r.work_ms / f, median(work_samples), r.gap_ms / f, median(gap_samples), r.total_ms / f,
        win_renders ? double(win_acquire_ns) / 1e6 / double(win_renders) : 0.0, win_renders ? double(win_present_ns) / 1e6 / double(win_renders) : 0.0,
        static_cast<unsigned long long>(r.no_pass), static_cast<unsigned long long>(r.no_present), static_cast<unsigned long long>(r.overflow), double(r.stamps) / f,
        double(r.cls_passes[kGpuColor]) / f, double(r.cls_passes[kGpuShadow]) / f, double(r.cls_passes[kGpuRim]) / f, double(r.cls_passes[kGpuPlane]) / f, double(r.cls_passes[kGpuCards]) / f,
        double(r.cls_passes[kGpuPost]) / f, double(r.cls_passes[kGpuOutput]) / f, double(r.cls_passes[kGpuUi]) / f, double(r.cls_passes[kGpuFrame]) / f, double(r.cls_passes[kGpuCopies]) / f, double(r.cls_passes[kGpuPrelight]) / f,
        double(r.cls_passes[kGpuFeedback]) / f, double(r.cls_passes[kGpuOther]) / f, r.blit_ms / f);
  }
  if (rdr::g_gpu_stamps.load(std::memory_order_relaxed)) {   // S103 GT: the upload view (steady vs loading frames), the same window as the lines above; the format is read by tools\surface\bench_line.py --roles (stamped runs only)
    const UpSums& u = up_window;
    const uint64_t n = u.steady_frames + u.load_frames;
    if (n) {
      const double mb = 1.0 / 1048576.0;
      Log("[rdr][gpu][uploads] %s: frames %llu; steady (<= %llu KB uploaded) frames %llu = %.3f MB/frame; loading (> %llu KB) frames %llu = %.3f MB/frame (%.1f %% of the frames, %.1f %% of the bytes); all frames %.3f MB/frame; biggest frame %.2f MB; "
          "GPU ms of the uploads' commands %.3f a frame over %llu frames with the stamps (a copy is its own role: see 'copies' above); "
          "calls (S103 UPL): %llu frames uploaded (%.1f %%), a frame that uploaded: %.2f image copies (biggest %llu), %.2f buffer copies, %.2f barriers (biggest %llu), batched in %.1f %% of them (frames with 2+ image copies %llu, of them left unbatched by a duplicate subresource %llu); a frame (all): %.2f image copies, %.2f barriers; upload batch switch %s; "
          "S103 PVR: buffer copies a frame that uploaded by producer: posed models %.2f, cloth / CPU-skinned %.2f, other %.2f; span split at the mid stamp (stamp-per-step mode only): image part %.3f ms, buffer part %.3f ms over %llu frames; peak frame ring use (constants + geometry + previous constants) %.2f MB of %.2f MB; S103 PF2: the host's ring memcpy %.3f ms a frame (mean %.2f MB a frame)",
          why, static_cast<unsigned long long>(n), static_cast<unsigned long long>(kLoadingBytes >> 10), static_cast<unsigned long long>(u.steady_frames), u.steady_frames ? double(u.steady_bytes) * mb / double(u.steady_frames) : 0.0,
          static_cast<unsigned long long>(kLoadingBytes >> 10), static_cast<unsigned long long>(u.load_frames), u.load_frames ? double(u.load_bytes) * mb / double(u.load_frames) : 0.0,
          100.0 * double(u.load_frames) / double(n), (u.steady_bytes + u.load_bytes) ? 100.0 * double(u.load_bytes) / double(u.steady_bytes + u.load_bytes) : 0.0,
          double(u.steady_bytes + u.load_bytes) * mb / double(n), double(u.max_bytes) * mb, u.gpu_frames ? u.gpu_ns / 1e6 / double(u.gpu_frames) : 0.0, static_cast<unsigned long long>(u.gpu_frames),
          static_cast<unsigned long long>(u.up_frames), 100.0 * double(u.up_frames) / double(n), u.up_frames ? double(u.image_uploads) / double(u.up_frames) : 0.0, static_cast<unsigned long long>(u.max_images),
          u.up_frames ? double(u.buffer_uploads) / double(u.up_frames) : 0.0, u.up_frames ? double(u.barriers) / double(u.up_frames) : 0.0, static_cast<unsigned long long>(u.max_barriers),
          u.up_frames ? 100.0 * double(u.batched_frames) / double(u.up_frames) : 0.0, static_cast<unsigned long long>(rdr::g_upload_multi_frames.load(std::memory_order_relaxed)), static_cast<unsigned long long>(rdr::g_upload_dup_fallbacks.load(std::memory_order_relaxed)), double(u.image_uploads) / double(n), double(u.barriers) / double(n), rdr::g_upload_batch.load(std::memory_order_relaxed) ? "ON" : "off",
          u.up_frames ? double(u.prod_copies[1]) / double(u.up_frames) : 0.0, u.up_frames ? double(u.prod_copies[2]) / double(u.up_frames) : 0.0, u.up_frames ? double(u.prod_copies[0]) / double(u.up_frames) : 0.0,
          u.mid_frames ? u.mid_img_ns / 1e6 / double(u.mid_frames) : 0.0, u.mid_frames ? u.mid_buf_ns / 1e6 / double(u.mid_frames) : 0.0, static_cast<unsigned long long>(u.mid_frames),
          double(u.ring_peak) * mb, double(kRingSection - kOverlayRange - kConstantRange - kDlssConstants) * mb, double(u.ring_ns) / 1e6 / double(n), double(u.ring_bytes) * mb / double(n));
    }
    up_window = UpSums{};
  }
  if (role_window.ident_frames) {   // S103 GT: the bracket line: in the stamp-per-step frames every recorded step is closed by a stamp that was written, and in every stamped frame the stamp count is exactly [0] + [1] + uploads pair + closes + tail
    const RoleSums& r = role_window;
    Log("[rdr][gpu][bracket] %s: %llu stamped frames, stamp count identity broken in %llu; %llu stamp-per-step frames, steps recorded %llu, steps closed by a written stamp %llu (%s), stamp pool overflows %llu",
        why, static_cast<unsigned long long>(r.ident_frames), static_cast<unsigned long long>(r.ident_bad), static_cast<unsigned long long>(r.census_frames), static_cast<unsigned long long>(r.steps_rec),
        static_cast<unsigned long long>(r.steps_closed), r.steps_rec == r.steps_closed && !r.ident_bad ? "all bracketed" : "UNBRACKETED", static_cast<unsigned long long>(r.overflow));
  }
  if (!pass_table.empty() && pass_table_frames) {  // S102 R24-B: one row per step (role, kind, occurrence on that image, image, size, format): mean and p90 GPU us over the frames that had the step
    static const char* const kinds[] = {"pass", "copy", "present", "tail", "plant", "dispatch", "uploads", "uploads_img"};   // S103 GT: 6 = the pair of stamps around ApplyUploads, 7 (S103 PVR) = the mid stamp between its image part and its buffer part
    struct Row { uint64_t key; double mean, p90, sum; size_t n; };
    std::vector<Row> rows;
    double table_sum = 0;
    for (auto& [key, v] : pass_table) {
      std::sort(v.begin(), v.end());
      double sum = 0;
      for (float x : v) sum += x;
      rows.push_back({key, sum / double(v.size()), double(v[std::min(v.size() - 1, size_t(double(v.size()) * 0.9))]), sum, v.size()});
      table_sum += sum;
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.sum > b.sum; });
    const double f = double(pass_table_frames);
    const double fa[2] = {double(std::max<uint64_t>(1, pass_table_frames_arm[0])), double(std::max<uint64_t>(1, pass_table_frames_arm[1]))};   // S102 R24-C review F4: per-arm frame counts
    Log("[rdr][gpu][passes] %s: per-step GPU table over %llu frames (a stamp after EVERY step: it drains the pipe, read as a ranking); %zu steps; the steps add up to %.3f ms a frame (= the work); columns: role kind #occurrence-on-that-image image WxH fmt: mean us / p90 us over the frames that had it, frames that had it, ms a frame (over all frames)",
        why, static_cast<unsigned long long>(pass_table_frames), rows.size(), table_sum / 1e3 / f);
    double rest = 0;
    size_t shown = 0;
    std::set<uint64_t> want_rows;   // S103 R24S: a printed row's other-arm twin (same key, bit 63 flipped) prints too, under the floor or past the cap, so an in-run flip always shows both columns
    {
      size_t n_want = 0;
      for (const Row& r : rows) {
        if (n_want >= 70 || r.sum / 1e3 / f < 0.004) continue;
        ++n_want;
        want_rows.insert(r.key), want_rows.insert(r.key ^ (1ull << 63));
      }
    }
    for (const Row& r : rows) {
      if (!want_rows.count(r.key)) { rest += r.sum; continue; }
      ++shown;
      const uint32_t img = uint32_t(r.key & 0xFFFFFFFFu);
      Log("[rdr][gpu][passes] %s %s #%u image %u %ux%u fmt %u: %.0f us mean / %.0f us p90, %zu of %llu frames, %.4f ms a frame [stencil_dead %s: %llu frames]",
          GpuClassName(uint32_t((r.key >> 48) & 0x7FFF)), kinds[std::min<uint32_t>(uint32_t((r.key >> 40) & 0xFF), 7)], uint32_t((r.key >> 32) & 0xFF), img,
          uint32_t(CensusImageW(img)), uint32_t(CensusImageH(img)), uint32_t(CensusImageFmt(img)), r.mean, r.p90, r.n, static_cast<unsigned long long>(pass_table_frames), r.sum / 1e3 / fa[r.key >> 63], (r.key >> 63) ? "ON" : "OFF", static_cast<unsigned long long>(pass_table_frames_arm[r.key >> 63]));
    }
    if (rest > 0) Log("[rdr][gpu][passes] the %zu smaller steps together: %.4f ms a frame", rows.size() - shown, rest / 1e3 / f);
    pass_table.clear(), pass_table_frames = 0, pass_table_frames_arm[0] = pass_table_frames_arm[1] = 0;
  }
  if (group_frames || group_not_ready || group_chain_broken || group_cover_bad || group_sum_bad) {   // S102 R24-F: the group table: per instrumented pass and group, mean / p90 GPU us over the frames that had the group
    static const char* const kinds[] = {"pass", "copy", "present", "tail", "plant", "dispatch", "uploads", "uploads_img"};
    const double f = double(std::max<uint64_t>(1, group_frames));
    uint64_t pass_draws = 0, held_draws = 0;
    for (const auto& [pk, pa] : group_pass) pass_draws += pa.issued, held_draws += pa.draws;
    Log("[rdr][gpu][groups] %s: group GPU table over %llu frames (a stamp at each change of draw group inside the colour / frame passes, min run %d draws; a stamp inside a pass drains it, read as a ranking, not as the shipped cost); %.1f group stamps a frame;"
        " frames dropped: group results not ready %llu, chain broken %llu, coverage short %llu; frames that ran out of stamps %llu; draws held by the segments %llu of %llu the passes issued (%.2f %%); frames where the groups did not add up to the pass's own stamp span %llu",
        why, static_cast<unsigned long long>(group_frames), std::max(1, rdr::g_gpu_group_stamps.load(std::memory_order_relaxed)), double(group_stamps_total) / f,
        static_cast<unsigned long long>(group_not_ready), static_cast<unsigned long long>(group_chain_broken), static_cast<unsigned long long>(group_cover_bad), static_cast<unsigned long long>(group_overflow_frames),
        static_cast<unsigned long long>(held_draws), static_cast<unsigned long long>(pass_draws), pass_draws ? 100.0 * double(held_draws) / double(pass_draws) : 0.0, static_cast<unsigned long long>(group_sum_bad));
    struct GRow { uint64_t key, pkey; double mean, p90, sum; size_t n; uint64_t draws; };
    std::vector<GRow> grows;
    for (auto& [key, a] : group_table) {
      std::sort(a.us.begin(), a.us.end());
      double sum = 0;
      for (float x : a.us) sum += x;
      grows.push_back({key, key & ~(0x3Full << 56), sum / double(a.us.size()), double(a.us[std::min(a.us.size() - 1, size_t(double(a.us.size()) * 0.9))]), sum, a.us.size(), a.draws});
    }
    std::sort(grows.begin(), grows.end(), [](const GRow& a, const GRow& b) { return a.sum > b.sum; });
    // per pass first: the pass line (its own span, the draws, the purity), then its groups biggest first
    std::vector<std::pair<uint64_t, GroupPassAcc>> passes_sorted(group_pass.begin(), group_pass.end());
    std::sort(passes_sorted.begin(), passes_sorted.end(), [](const auto& a, const auto& b) { return a.second.us > b.second.us; });
    for (const auto& [pk, pa] : passes_sorted) {
      const uint32_t img = uint32_t(pk & 0xFFFFFFFFu);
      const double pf = double(std::max<uint64_t>(1, pa.frames));
      Log("[rdr][gpu][groups] %s %s #%u image %u %ux%u fmt %u [stencil_dead %s]: pass %.4f ms a frame (its own stamp span %.4f ms) over %llu frames, %.1f draws a frame (segments hold %.1f), %.1f segments a frame, %.2f %% of its draws sit in a segment labelled with another group",
          GpuClassName(uint32_t((pk >> 48) & 0x7FFF)), kinds[std::min<uint32_t>(uint32_t((pk >> 40) & 0xFF), 7)], uint32_t((pk >> 32) & 0xFF), img,
          uint32_t(CensusImageW(img)), uint32_t(CensusImageH(img)), uint32_t(CensusImageFmt(img)), (pk >> 63) ? "ON" : "OFF", pa.us / 1e3 / pf, pa.direct_us / 1e3 / pf, static_cast<unsigned long long>(pa.frames),
          double(pa.issued) / pf, double(pa.draws) / pf, double(pa.segs) / pf, pa.draws ? 100.0 * double(pa.mixed) / double(pa.draws) : 0.0);
      for (const GRow& r : grows) {
        if (r.pkey != pk) continue;
        Log("[rdr][gpu][groups]   %s: %.0f us mean / %.0f us p90, %zu of %llu frames, %.1f draws a frame, %.4f ms a frame = %.1f %% of the pass", GroupName(uint32_t((r.key >> 56) & 0x3F)), r.mean, r.p90, r.n,
            static_cast<unsigned long long>(pa.frames), double(r.draws) / pf, r.sum / 1e3 / pf, pa.us > 0 ? 100.0 * r.sum / pa.us : 0.0);
      }
    }
    group_table.clear(), group_pass.clear();
    group_frames = group_not_ready = group_chain_broken = group_cover_bad = group_overflow_frames = group_stamps_total = group_sum_bad = 0;
  }
  if (!std::strcmp(why, "summary") && !census_run.empty() && census_frames) {  // S102 G6: the census, ranked by ms a frame (kind: pass / copy / present / tail)
    std::vector<std::pair<uint64_t, CensusRow>> rows(census_run.begin(), census_run.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
    static const char* const kinds[] = {"pass", "copy", "present", "tail", "plant", "dispatch", "uploads", "uploads_img"};
    for (const auto& r : rows) {
      const double f = double(census_frames);
      Log("[rdr][gpu][census] class %s kind %s image %u (%ux%u fmt %u): %.4f ms a frame, %.2f steps a frame",
          GpuClassName(uint32_t(r.first >> 40)), kinds[std::min<uint32_t>(uint32_t((r.first >> 32) & 0xFF), 7)], uint32_t(r.first & 0xFFFFFFFFu),
          uint32_t(CensusImageW(uint32_t(r.first & 0xFFFFFFFFu))), uint32_t(CensusImageH(uint32_t(r.first & 0xFFFFFFFFu))), uint32_t(CensusImageFmt(uint32_t(r.first & 0xFFFFFFFFu))),
          r.second.ms / f, double(r.second.steps) / f);
    }
    for (int c = 0; c < kGpuClassCount; ++c)  // S102 G6 (item 7): what the copies copy from, so lanes can rank them
      if (census_copy_src[c].steps)
        Log("[rdr][gpu][census] copies by source class %s: %.4f ms a frame, %.2f steps a frame", GpuClassName(uint32_t(c)), census_copy_src[c].ms / double(census_frames), double(census_copy_src[c].steps) / double(census_frames));
  }
  win_acquire_ns = win_present_ns = win_renders = 0;
  gpu_samples.clear();
  work_samples.clear(), gap_samples.clear();
  role_window = RoleSums{};
  gpu_bad = gpu_not_ready = 0;
}

void Renderer::LogGpuSummary() { impl_->LogGpuWindow("summary"); }
void Renderer::GpuTotals(uint64_t& implausible, uint64_t& not_ready) const { implausible = impl_->gpu_bad_total, not_ready = impl_->gpu_not_ready_total; }
void Renderer::SetImageClass(ImageId image, uint8_t gpu_class) {  // S102 S0-3
  // S102 R21-B (STAGE_SPLIT.md hazard 3): the class steers Record (the colour prepass, the half-size tail, the arena blend-off), so a change made by the bind
  // stage waits for the frame that sealed it (ApplyUploads), never lands under a frame being recorded. Seal 0 (one thread): applied at the next Render, as before.
  Impl& im = *impl_;
  if (image == kNoImage || image >= im.images.high_water()) return;
  if (im.options.gpu_stamp_plant == 11) return;   // S103 GT test plant 11: no image is named (every pass is `other`): the role checks must fail
  std::lock_guard<std::mutex> lock(im.upload_mutex);
  im.class_changes.emplace_back(image, gpu_class < kGpuClassCount ? gpu_class : uint8_t(kGpuOther));
}
Renderer::GpuRoleTotals Renderer::GpuRoles() const {  // S102 S0-3
  const Impl::RoleSums& r = impl_->role_run;
  GpuRoleTotals out;
  for (int c = 0; c < kGpuClassCount; ++c) out.cls_ms[c] = r.frames ? r.cls_ms[c] / double(r.frames) : 0.0;
  out.gap_ms = r.frames ? r.gap_ms / double(r.frames) : 0.0;
  out.blit_ms = r.frames ? r.blit_ms / double(r.frames) : 0.0;  // S102 G2
  for (int c = 0; c < kGpuClassCount; ++c) out.cls_passes_a_frame[c] = r.frames ? double(r.cls_passes[c]) / double(r.frames) : 0.0;
  out.acquire_ms = impl_->run_renders ? double(impl_->run_acquire_ns) / 1e6 / double(impl_->run_renders) : 0.0;
  out.present_call_ms = impl_->run_renders ? double(impl_->run_present_ns) / 1e6 / double(impl_->run_renders) : 0.0;
  out.work_ms = r.frames ? r.work_ms / double(r.frames) : 0.0;
  out.total_ms = r.frames ? r.total_ms / double(r.frames) : 0.0;
  out.frames = r.frames, out.no_pass = r.no_pass, out.no_present = r.no_present, out.overflow = r.overflow;
  out.stamps_a_frame = r.frames ? double(r.stamps) / double(r.frames) : 0.0;
  out.census_frames = r.census_frames, out.steps_recorded = r.steps_rec, out.steps_closed = r.steps_closed;   // S103 GT
  out.ident_frames = r.ident_frames, out.ident_bad = r.ident_bad;
  return out;
}
uint32_t Renderer::UnnamedColourTargets(std::vector<std::string>* what) const {   // S103 GT round 3
  uint32_t n = 0;
  Impl& im = *impl_;
  for (uint32_t id = 1; id < im.images.high_water(); ++id) {
    const ImageEntry& e = im.images[id];
    if (!e.live || e.depth || !(e.desc.usage & kUsageTarget) || e.gpu_class != kGpuOther) continue;
    ++n;
    if (what) what->push_back(std::to_string(id) + " " + std::to_string(e.width) + "x" + std::to_string(e.height) + " fmt " + std::to_string(unsigned(e.desc.format)));
  }
  return n;
}
Renderer::UploadTotals Renderer::UploadRun() const {   // S103 GT
  const Impl::UpSums& u = impl_->up_run;
  UploadTotals t;
  t.steady_frames = u.steady_frames, t.steady_bytes = u.steady_bytes, t.loading_frames = u.load_frames, t.loading_bytes = u.load_bytes, t.max_bytes = u.max_bytes;
  t.frames_with_span = u.gpu_frames, t.gpu_ms_sum = u.gpu_ns / 1e6;
  return t;
}

const FrameCounts& Renderer::LastFrameCounts() const { return impl_->counts_cur; }  // S102 M1
uint32_t Renderer::RingRoom() const { return uint32_t(kRingSection - kOverlayRange - kConstantRange - kDlssConstants - 1024); }   // S103 PVR (the frame_ring_full test in Render adds the section alignments: 1 KB of slack)
Renderer::UploadStats Renderer::LastUploadStats() const {   // S103 UPL
  const Impl& im = *impl_;
  UploadStats s;
  s.images = im.out_up_images, s.buffers = im.out_uploads - im.out_up_images, s.barriers = im.out_up_barriers, s.bytes = im.out_up_bytes, s.batched = im.out_batched;
  for (int pk = 0; pk < 3; ++pk) s.by_producer[pk] = im.out_up_by_producer[pk], s.total_by_producer[pk] = im.up_tot_by_producer[pk];   // S103 PVR
  s.total_frames = im.up_tot_frames, s.total_images = im.up_tot_images, s.total_buffers = im.up_tot_buffers, s.total_barriers = im.up_tot_barriers, s.total_bytes = im.up_tot_bytes;
  return s;
}
void Renderer::SetCountsPlant(int pass) { impl_->counts_plant = pass; }

uint64_t Renderer::LastGpuNs() {  // S101 lane22
  Impl& im = *impl_;
  if (!im.gpu_timestamps || im.last_slot == ~0u) return 0;
  FrameSlot& slot = im.slots[im.last_slot];
  vkWaitForFences(im.device.device(), 1, &slot.fence, VK_TRUE, UINT64_MAX);
  uint64_t ticks[2] = {0, 0};
  if (slot.ts_count < 2) return 0;
  // S102 S0-3: the bracket is query 0 to the LAST query written (the tail), whichever stamp mode the frame ran in.
  if (vkGetQueryPoolResults(im.device.device(), slot.timestamps, 0, 1, sizeof(uint64_t), &ticks[0], sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) return 0;
  if (vkGetQueryPoolResults(im.device.device(), slot.timestamps, slot.ts_count - 1, 1, sizeof(uint64_t), &ticks[1], sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) return 0;
  const uint64_t mask = im.ts_valid_bits >= 64 ? ~0ull : ((1ull << im.ts_valid_bits) - 1);
  return uint64_t(double((ticks[1] - ticks[0]) & mask) * double(im.device.properties().limits.timestampPeriod));
}

void Renderer::WaitIdle() { vkDeviceWaitIdle(impl_->device.device()); }

static float HalfToFloat(uint16_t half) {
  const int exponent = (half >> 10) & 31, mantissa = half & 1023;
  const float sign = (half >> 15) ? -1.0f : 1.0f;
  if (exponent == 31) return 0.0f;  // infinity or NaN: nothing to show
  if (exponent == 0) return sign * float(mantissa) / (1024.0f * 16384.0f);
  return sign * std::ldexp(1.0f + float(mantissa) / 1024.0f, exponent - 15);
}

// The staging bytes of a read-back as tightly packed RGBA8 (BGRA swapped; R8 as grey; 16F scaled by the peak: display only).
static void ConvertReadback(Format format, uint32_t texel, const uint8_t* mapped, uint32_t width, uint32_t height, std::vector<uint8_t>& rgba) {
  const size_t pixels = size_t(width) * height;
  rgba.resize(pixels * 4);
  if (format == Format::RGBA8 || format == Format::BGRA8) {
    std::memcpy(rgba.data(), mapped, pixels * 4);
    if (format == Format::BGRA8) {
      for (size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
    }
  } else if (format == Format::Depth) {   // S102 G11: raw depth bits, a hash is all they are read for
    std::memcpy(rgba.data(), mapped, pixels * 4);
  } else if (format == Format::R8) {
    for (size_t i = 0; i < pixels; ++i) {
      rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = mapped[i];
      rgba[i * 4 + 3] = 255;
    }
  } else {
    const uint32_t channels = texel / 2;
    const uint16_t* halves = reinterpret_cast<const uint16_t*>(mapped);
    float peak = 1e-6f;
    for (size_t i = 0; i < pixels * channels; ++i) peak = std::max(peak, std::fabs(HalfToFloat(halves[i])));
    for (size_t i = 0; i < pixels; ++i) {
      for (uint32_t c = 0; c < 3; ++c) rgba[i * 4 + c] = c < channels ? uint8_t(std::min(255.0f, std::fabs(HalfToFloat(halves[i * channels + c])) / peak * 255.0f)) : 0;
      rgba[i * 4 + 3] = 255;
    }
  }
}

// Colour surfaces come back as RGBA8 for looking at. A float surface is scaled so its largest value is white.
bool Renderer::ReadPresent(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) { return impl_->swapchain.ReadLast(rgba, width, height); }

bool Renderer::ImageIs8Bit(ImageId image) const {
  if (image == kNoImage) return false;
  const ImageEntry& entry = impl_->images[image];
  return entry.live && entry.desc.samples == 1 && (entry.desc.format == Format::RGBA8 || entry.desc.format == Format::BGRA8);
}

float Renderer::render_scale() const { return impl_->scale; }   // S105 A1

std::vector<Renderer::LiveImageInfo> Renderer::LiveImageInfos() const {   // S105 A1 (test use)
  std::vector<LiveImageInfo> out;
  for (uint32_t id = 1; id < impl_->images.high_water(); ++id) {
    const ImageEntry& entry = impl_->images[id];
    if (!entry.live) continue;
    LiveImageInfo info;
    info.id = id, info.width = entry.width, info.height = entry.height, info.format = uint32_t(entry.desc.format), info.scaled = (entry.desc.usage & kUsageScaled) != 0;
    out.push_back(info);
  }
  return out;
}

bool Renderer::ImageExtent(ImageId image, uint32_t& width, uint32_t& height, bool& scaled) const {   // S105 A1
  width = height = 0, scaled = false;
  if (image == kNoImage) return false;
  const ImageEntry& entry = impl_->images[image];
  if (!entry.live) return false;
  width = entry.width, height = entry.height, scaled = (entry.desc.usage & kUsageScaled) != 0;
  return true;
}

bool Renderer::ReadImage(ImageRef image, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
  Impl& im = *impl_;
  ImageEntry& entry = im.images[image.image];
  if (!entry.live || entry.desc.samples != 1) return false;
  uint32_t texel = 0;
  switch (entry.desc.format) {
    case Format::R8: texel = 1; break;
    case Format::RGBA8: case Format::BGRA8: case Format::RG16F: texel = 4; break;
    case Format::RGBA16F: texel = 8; break;
    case Format::Depth: texel = 4; break;   // S102 G11: the depth aspect, 4 bytes a texel (the sweep hashes the frame buffer's depth)
    default: return false;
  }
  VkDevice dev = im.device.device();
  vkDeviceWaitIdle(dev);
  width = std::max(1u, entry.width >> image.mip);
  height = std::max(1u, entry.height >> image.mip);
  const VkDeviceSize bytes = VkDeviceSize(width) * height * texel;
  VkBuffer staging = VK_NULL_HANDLE;
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = bytes;
  info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  vkCreateBuffer(dev, &info, nullptr, &staging);
  VkMemoryRequirements needs{};
  vkGetBufferMemoryRequirements(dev, staging, &needs);
  Allocation memory;
  if (!im.allocator.Alloc(needs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT, true, memory)) {
    vkDestroyBuffer(dev, staging, nullptr);
    return false;
  }
  vkBindBufferMemory(dev, staging, memory.memory, memory.offset);

  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.queueFamilyIndex = im.device.queue_family();
  vkCreateCommandPool(dev, &pool_info, nullptr, &pool);
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = pool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  vkAllocateCommandBuffers(dev, &alloc, &cmd);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin);
  const uint32_t sub = image.mip * entry.layers + image.layer;
  const VkImageLayout layout_before = entry.states[sub].layout;   // S102 P1 (review F6): a sampled image goes back to SHADER_READ_ONLY after the read (else the next frame that reads it without rewriting it trips validation)
  im.Transition(entry, sub, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, false);
  im.FlushBarriers(cmd);
  VkBufferImageCopy region{};
  region.imageSubresource = {entry.desc.format == Format::Depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : entry.aspect, image.mip, image.layer, 1};   // S102 G11: depth only
  region.imageExtent = {width, height, 1};
  vkCmdCopyImageToBuffer(cmd, entry.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &region);
  if (layout_before == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    im.Transition(entry, sub, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, false);
    im.FlushBarriers(cmd);
  }
  VkMemoryBarrier to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, nullptr, 0, nullptr);
  vkEndCommandBuffer(cmd);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  vkQueueSubmit(im.device.queue(), 1, &submit, VK_NULL_HANDLE);
  vkQueueWaitIdle(im.device.queue());

  ConvertReadback(entry.desc.format, texel, static_cast<const uint8_t*>(memory.mapped), width, height, rgba);
  vkDestroyCommandPool(dev, pool, nullptr);
  vkDestroyBuffer(dev, staging, nullptr);
  im.allocator.Free(memory);
  return true;
}

// ---- lane 232 (3d a2): the no-drain read-back ----
static uint32_t ReadbackTexelBytes(Format f) {
  switch (f) {
    case Format::R8: return 1;
    case Format::RGBA8: case Format::BGRA8: case Format::RG16F: return 4;
    case Format::RGBA16F: return 8;
    default: return 0;
  }
}

// Records ONE request's copy into `cmd` (false: cancelled: the image is gone, unreadable or out of range, or no staging memory).
bool Renderer::Impl::RecordOneReadback(VkCommandBuffer cmd, const rdr::ReadbackTracker::Request& req) {
  VkDevice dev = device.device();
  ImageEntry& entry = images[req.key.image];
  const uint32_t texel = ReadbackTexelBytes(entry.desc.format);
  if (!entry.live || entry.desc.samples != 1 || !texel || req.key.mip >= entry.desc.mips || req.key.layer >= entry.layers) return false;
  const uint32_t width = std::max(1u, entry.width >> req.key.mip), height = std::max(1u, entry.height >> req.key.mip);
  const VkDeviceSize bytes = VkDeviceSize(width) * height * texel;
  ReadbackStaging& st = readback_staging[ReadbackKey(req.key)];
  if (st.buffer == VK_NULL_HANDLE || st.bytes != bytes) {  // a new level, or the picture was recreated at another size (never in flight: Queue refuses a busy key)
    if (st.buffer != VK_NULL_HANDLE) {
      vkDestroyBuffer(dev, st.buffer, nullptr);
      allocator.Free(st.memory);
      st = ReadbackStaging{};
    }
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = bytes;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    vkCreateBuffer(dev, &info, nullptr, &buffer);
    VkMemoryRequirements needs{};
    vkGetBufferMemoryRequirements(dev, buffer, &needs);
    Allocation memory;
    if (!allocator.Alloc(needs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT, true, memory)) {
      vkDestroyBuffer(dev, buffer, nullptr);
      readback_staging.erase(ReadbackKey(req.key));
      return false;
    }
    vkBindBufferMemory(dev, buffer, memory.memory, memory.offset);
    st.buffer = buffer, st.memory = memory, st.bytes = bytes;
  }
  st.width = width, st.height = height, st.format = entry.desc.format;
  const uint32_t sub = req.key.mip * entry.layers + req.key.layer;
  Transition(entry, sub, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, false);
  FlushBarriers(cmd);
  VkBufferImageCopy region{};
  region.imageSubresource = {entry.depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : entry.aspect, req.key.mip, req.key.layer, 1};
  region.imageExtent = {width, height, 1};
  vkCmdCopyImageToBuffer(cmd, entry.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, st.buffer, 1, &region);
  CountCopy();  // S102 M1: the in-frame read-back (the ReadImage-style calls below own their command buffer and are not part of any frame)
  VkMemoryBarrier to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, nullptr, 0, nullptr);
  CountBarrier(0);
  return true;
}

// SF-2: a request with a source is recorded right after the copy step that writes its level from THAT source (the frame may hold other copies into the same image, from other targets).
void Renderer::Impl::RecordReadbacksAfterCopy(VkCommandBuffer cmd, const CopyOp& c) {
  for (size_t i = 0; i < readback_taking.size(); ++i) {
    const rdr::ReadbackTracker::Request& req = readback_taking[i];
    if (readback_recorded[i] || !req.source || req.key.image != uint32_t(c.dest.image) || req.key.mip != c.dest.mip || req.key.layer != c.dest.layer || req.source != uint32_t(c.source.image)) continue;
    readback_recorded[i] = 1;
    if (!RecordOneReadback(cmd, req)) readback.Cancel(req.key);
  }
}

// At the end of the steps: requests without a source are recorded now (the frame's final picture); a request whose source copy never ran in this frame is cancelled (never another target's picture).
void Renderer::Impl::RecordReadbacks(VkCommandBuffer cmd) {
  for (size_t i = 0; i < readback_taking.size(); ++i) {
    if (readback_recorded[i]) continue;
    readback_recorded[i] = 1;
    const rdr::ReadbackTracker::Request& req = readback_taking[i];
    if (req.source || !RecordOneReadback(cmd, req)) readback.Cancel(req.key);
  }
}

bool Renderer::QueueReadback(ImageRef image, uint64_t tag, ImageId source) {
  Impl& im = *impl_;
  if (image.image == kNoImage) return false;
  const ImageEntry& entry = im.images[image.image];
  if (!entry.live || entry.desc.samples != 1 || !ReadbackTexelBytes(entry.desc.format) || image.mip >= entry.desc.mips || image.layer >= entry.layers) return false;
  return im.readback.Queue({image.image, image.mip, image.layer}, tag, uint32_t(source));
}

void Renderer::PollReadbacks(std::vector<ReadbackResult>& out) {
  Impl& im = *impl_;
  if (!im.readback.InFlight() && !im.readback.HasCancelled()) return;
  VkDevice dev = im.device.device();
  std::vector<rdr::ReadbackTracker::Done> done;
  im.readback.TakeCancelled(done);  // SF-1: reported with cancelled = true so the caller forgets the tag
  const size_t cancelled = done.size();
  im.readback.Poll([&](uint32_t slot) { return vkGetFenceStatus(dev, im.slots[slot].fence) == VK_SUCCESS; }, done);
  for (size_t di = 0; di < done.size(); ++di) {
    const rdr::ReadbackTracker::Done& d = done[di];
    if (di < cancelled) {
      ReadbackResult r;
      r.tag = d.tag, r.cancelled = true;
      out.push_back(std::move(r));
      continue;
    }
    const auto it = im.readback_staging.find(Impl::ReadbackKey(d.key));
    if (it == im.readback_staging.end()) {
      ReadbackResult gone;
      gone.tag = d.tag, gone.cancelled = true;
      out.push_back(std::move(gone));
      continue;
    }
    const Impl::ReadbackStaging& st = it->second;
    ReadbackResult r;
    r.tag = d.tag;
    r.image = ImageRef{ImageId(d.key.image), uint8_t(d.key.mip), uint8_t(d.key.layer), 0};
    r.width = st.width, r.height = st.height;
    ConvertReadback(st.format, ReadbackTexelBytes(st.format), static_cast<const uint8_t*>(st.memory.mapped), st.width, st.height, r.rgba);
    out.push_back(std::move(r));
  }
}

// S101 fview: one texel of a colour image as the GPU holds it (raw bytes of the image's own format), plus a display RGBA8 of it. Equality of two raw reads is
// exact for every readable format (the 16F display copy of ReadImage is scaled by the image's peak, so it cannot be compared). Test and viewer use only.
bool Renderer::ReadTexelRaw(ImageRef image, int x, int y, uint8_t raw[8], uint32_t& raw_bytes, uint8_t rgba[4]) {
  Impl& im = *impl_;
  ImageEntry& entry = im.images[image.image];
  if (!entry.live || entry.desc.samples != 1) return false;
  uint32_t texel = 0;
  switch (entry.desc.format) {
    case Format::R8: texel = 1; break;
    case Format::RGBA8: case Format::BGRA8: case Format::RG16F: texel = 4; break;
    case Format::RGBA16F: texel = 8; break;
    default: return false;
  }
  const uint32_t width = std::max(1u, entry.width >> image.mip), height = std::max(1u, entry.height >> image.mip);
  if (x < 0 || y < 0 || uint32_t(x) >= width || uint32_t(y) >= height) return false;
  VkDevice dev = im.device.device();
  vkDeviceWaitIdle(dev);
  VkBuffer staging = VK_NULL_HANDLE;
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = 16;
  info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  vkCreateBuffer(dev, &info, nullptr, &staging);
  VkMemoryRequirements needs{};
  vkGetBufferMemoryRequirements(dev, staging, &needs);
  Allocation memory;
  if (!im.allocator.Alloc(needs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT, true, memory)) {
    vkDestroyBuffer(dev, staging, nullptr);
    return false;
  }
  vkBindBufferMemory(dev, staging, memory.memory, memory.offset);
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.queueFamilyIndex = im.device.queue_family();
  vkCreateCommandPool(dev, &pool_info, nullptr, &pool);
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = pool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  vkAllocateCommandBuffers(dev, &alloc, &cmd);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin);
  const uint32_t sub = image.mip * entry.layers + image.layer;
  im.Transition(entry, sub, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, false);
  im.FlushBarriers(cmd);
  VkBufferImageCopy region{};
  region.imageSubresource = {entry.depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : entry.aspect, image.mip, image.layer, 1};   // S102 R24-C review: one region may not name depth AND stencil (Vulkan: separate regions per aspect); a depth texel is the depth aspect
  region.imageOffset = {x, y, 0};
  region.imageExtent = {1, 1, 1};
  vkCmdCopyImageToBuffer(cmd, entry.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &region);
  VkMemoryBarrier to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, nullptr, 0, nullptr);
  vkEndCommandBuffer(cmd);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  vkQueueSubmit(im.device.queue(), 1, &submit, VK_NULL_HANDLE);
  vkQueueWaitIdle(im.device.queue());
  const uint8_t* mapped = static_cast<const uint8_t*>(memory.mapped);
  std::memset(raw, 0, 8);
  std::memcpy(raw, mapped, texel);
  raw_bytes = texel;
  if (entry.desc.format == Format::RGBA8 || entry.desc.format == Format::BGRA8) {
    std::memcpy(rgba, mapped, 4);
    if (entry.desc.format == Format::BGRA8) std::swap(rgba[0], rgba[2]);
  } else if (entry.desc.format == Format::R8) {
    rgba[0] = rgba[1] = rgba[2] = mapped[0], rgba[3] = 255;
  } else {  // 16F: the magnitude clamped to 0..1, for display only
    const uint32_t channels = texel / 2;
    const uint16_t* halves = reinterpret_cast<const uint16_t*>(mapped);
    for (uint32_t c = 0; c < 3; ++c) rgba[c] = c < channels ? uint8_t(std::min(1.0f, std::fabs(HalfToFloat(halves[c]))) * 255.0f) : 0;
    rgba[3] = 255;
  }
  vkDestroyCommandPool(dev, pool, nullptr);
  vkDestroyBuffer(dev, staging, nullptr);
  im.allocator.Free(memory);
  return true;
}

// S101 lane 20: ReadTexelRaw for many texels at once: one idle wait, one staging buffer, one command buffer, one submit.
bool Renderer::ReadTexelsRaw(std::vector<TexelRead>& reads) {
  Impl& im = *impl_;
  std::vector<uint32_t> texel(reads.size(), 0);
  size_t wanted = 0;
  for (size_t i = 0; i < reads.size(); ++i) {
    TexelRead& r = reads[i];
    r.ok = false;
    ImageEntry& entry = im.images[r.image.image];
    if (!entry.live || entry.desc.samples != 1) continue;
    switch (entry.desc.format) {
      case Format::R8: texel[i] = 1; break;
      case Format::RGBA8: case Format::BGRA8: case Format::RG16F: texel[i] = 4; break;
      case Format::RGBA16F: texel[i] = 8; break;
      default: continue;
    }
    const uint32_t width = std::max(1u, entry.width >> r.image.mip), height = std::max(1u, entry.height >> r.image.mip);
    if (r.x < 0 || r.y < 0 || uint32_t(r.x) >= width || uint32_t(r.y) >= height) {
      texel[i] = 0;
      continue;
    }
    ++wanted;
  }
  if (!wanted) return true;
  VkDevice dev = im.device.device();
  vkDeviceWaitIdle(dev);
  VkBuffer staging = VK_NULL_HANDLE;
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = 16 * reads.size();
  info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  vkCreateBuffer(dev, &info, nullptr, &staging);
  VkMemoryRequirements needs{};
  vkGetBufferMemoryRequirements(dev, staging, &needs);
  Allocation memory;
  if (!im.allocator.Alloc(needs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT, true, memory)) {
    vkDestroyBuffer(dev, staging, nullptr);
    return false;
  }
  vkBindBufferMemory(dev, staging, memory.memory, memory.offset);
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.queueFamilyIndex = im.device.queue_family();
  vkCreateCommandPool(dev, &pool_info, nullptr, &pool);
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = pool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  vkAllocateCommandBuffers(dev, &alloc, &cmd);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin);
  for (size_t i = 0; i < reads.size(); ++i) {
    if (!texel[i]) continue;
    const TexelRead& r = reads[i];
    ImageEntry& entry = im.images[r.image.image];
    const uint32_t sub = r.image.mip * entry.layers + r.image.layer;
    im.Transition(entry, sub, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, false);
    im.FlushBarriers(cmd);
    VkBufferImageCopy region{};
    region.bufferOffset = 16 * i;
    region.imageSubresource = {entry.depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : entry.aspect, r.image.mip, r.image.layer, 1};
    region.imageOffset = {r.x, r.y, 0};
    region.imageExtent = {1, 1, 1};
    vkCmdCopyImageToBuffer(cmd, entry.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &region);
  }
  VkMemoryBarrier to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, nullptr, 0, nullptr);
  vkEndCommandBuffer(cmd);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  vkQueueSubmit(im.device.queue(), 1, &submit, VK_NULL_HANDLE);
  vkQueueWaitIdle(im.device.queue());
  for (size_t i = 0; i < reads.size(); ++i) {
    if (!texel[i]) continue;
    TexelRead& r = reads[i];
    ImageEntry& entry = im.images[r.image.image];
    const uint8_t* mapped = static_cast<const uint8_t*>(memory.mapped) + 16 * i;
    std::memset(r.raw, 0, 8);
    std::memcpy(r.raw, mapped, texel[i]);
    r.raw_bytes = texel[i];
    if (entry.desc.format == Format::RGBA8 || entry.desc.format == Format::BGRA8) {
      std::memcpy(r.rgba, mapped, 4);
      if (entry.desc.format == Format::BGRA8) std::swap(r.rgba[0], r.rgba[2]);
    } else if (entry.desc.format == Format::R8) {
      r.rgba[0] = r.rgba[1] = r.rgba[2] = mapped[0], r.rgba[3] = 255;
    } else {
      const uint32_t channels = texel[i] / 2;
      const uint16_t* halves = reinterpret_cast<const uint16_t*>(mapped);
      for (uint32_t c = 0; c < 3; ++c) r.rgba[c] = c < channels ? uint8_t(std::min(1.0f, std::fabs(HalfToFloat(halves[c]))) * 255.0f) : 0;
      r.rgba[3] = 255;
    }
    r.ok = true;
  }
  vkDestroyCommandPool(dev, pool, nullptr);
  vkDestroyBuffer(dev, staging, nullptr);
  im.allocator.Free(memory);
  return true;
}

// S101 fview: AddProgram for live reload: checks vkCreateShaderModule and the 16-bit program id; false = nothing was added (a shader the driver refused).
bool Renderer::AddProgramChecked(const ProgramDesc& desc, uint16_t& id) {
  Impl& im = *impl_;
  std::lock_guard<std::mutex> tables(im.tables_mutex);  // S102 R21-B (hazards 1, 2)
  if (im.store_on && im.StoreFindProgram(desc, id)) return true;  // lane 276: the same words are the same program
  if (im.programs.size() >= 65535) {
    Problem("program_table_full", "65535 programs: restart the viewer");
    return false;
  }
  Program program;
  program.name = desc.name;
  program.prepass_class = colour_cuts::ClassifyFragment(desc.fragment_spirv, desc.fragment_bytes);  // S102 G10
  program.half_ok = desc.fragment_spirv && !half_trans::ReadsFragCoord(desc.fragment_spirv, desc.fragment_bytes);  // S102 C4
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = desc.vertex_bytes;
  info.pCode = desc.vertex_spirv;
  if (vkCreateShaderModule(im.device.device(), &info, nullptr, &program.vertex) != VK_SUCCESS) return false;
  if (desc.fragment_spirv) {
    info.codeSize = desc.fragment_bytes;
    info.pCode = desc.fragment_spirv;
    if (vkCreateShaderModule(im.device.device(), &info, nullptr, &program.fragment) != VK_SUCCESS) {
      vkDestroyShaderModule(im.device.device(), program.vertex, nullptr);
      return false;
    }
  }
  if (im.programs.size() >= kNoTableId || !im.programs.push_back(program)) {  // S102 R21-B (review F5)
    Problem("program_table_full", "%u programs: restart the viewer", unsigned(kNoTableId));
    vkDestroyShaderModule(im.device.device(), program.vertex, nullptr);
    if (program.fragment) vkDestroyShaderModule(im.device.device(), program.fragment, nullptr);
    return false;
  }
  id = uint16_t(im.programs.size() - 1);
  if (im.store_on) im.StoreProgramAdded(id);
  return true;
}

bool Renderer::validation_active() const { return impl_->device.debug_names(); }  // S101 fview

bool VertexFormatIsInteger(uint16_t vk_format) {
  switch (VkFormat(vk_format)) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_A8B8G8R8_UINT_PACK32: case VK_FORMAT_A8B8G8R8_SINT_PACK32:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_A2B10G10R10_SINT_PACK32:
    case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
      return true;
    default:
      return false;
  }
}

}  // namespace rdr
