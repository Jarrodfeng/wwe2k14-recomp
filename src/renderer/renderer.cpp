#include "renderer.h"
#include "readback_tracker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "device.h"
#include "format_caps.h"
#include "memory.h"
#include "pipeline_store.h"
#include "../perf_file_write.h"  // lane 302: NoteFileWrite
#include "problem.h"
#include "shaders/spv/rdr_present_frag.h"
#include "shaders/spv/rdr_present_sgsr_frag.h"
#include "shaders/spv/rdr_overlay_frag.h"
#include "shaders/spv/rdr_overlay_vert.h"
#include "shaders/spv/rdr_dlss_motion_frag.h"
#include "shaders/spv/rdr_fullscreen_vert.h"
#include "shaders/spv/rdr_present_dlss_frag.h"
#include "streamline.h"
#include "shaders/spv/rdr_present_vert.h"
#include "swapchain.h"
#include "vk_api.h"

namespace rdr {
namespace {

using namespace vk;

constexpr uint32_t kChunkBits = 8;
constexpr uint32_t kChunkSize = 1u << kChunkBits;
constexpr uint32_t kMaxChunks = 256;
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
  bool live = false;
  bool copy_dest = false;      // written by copies, so sampling it needs a layout check
  bool carries_state = false;  // some frame has read it before writing it
  bool ever_read = false;      // some frame has sampled it, loaded it or copied from it (S100: a copy into an image never read yet is kept)
};

struct BufferEntry {
  VkBuffer buffer = VK_NULL_HANDLE;
  Allocation memory;
  size_t bytes = 0;
  bool live = false;
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
};

struct BufferUpload {
  VkBuffer staging;
  Allocation memory;
  VkBuffer dest;
  VkDeviceSize offset, bytes;
};

struct Retired {
  VkImage image = VK_NULL_HANDLE;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  Allocation memory;
  uint32_t free_image = 0, free_buffer = 0;  // ids that become reusable once this slot's GPU work is done
  uint64_t staged_bytes = 0;
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
  std::vector<Retired> retired;
};

struct RenderPassKey {
  uint16_t formats[kAttachmentSlots];
  uint8_t load[kAttachmentSlots];
  uint8_t store[kAttachmentSlots];
  uint8_t stencil_load, stencil_store;
  uint8_t color_count, has_depth, samples, pad;
};

struct PassClassKey {
  uint16_t formats[kAttachmentSlots];
  uint8_t color_count, has_depth, samples, pad;
};

struct FramebufferKey {
  VkRenderPass pass;
  VkImageView views[kAttachmentSlots];
  uint32_t width, height;
};

struct PipelineKey {
  DrawState state;
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
  bool depth_used = false;     // a draw tests or writes depth or stencil, or a clear inside the pass touches them
  bool depth_cleared = false;  // a full depth / stencil clear became the load op
};

enum class StepKind : uint8_t { Pass, Copy, Present };
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
  // Lane 269: the per-frame GPU time without a stall. Each slot's two timestamps are read right after the slot's fence wait (which Render does anyway),
  // frames_in_flight frames after they were written, with no WAIT flag. Results land in rdr::g_gpu_* (overlay.h) and a 600-frame log line.
  uint32_t ts_valid_bits = 0;
  double ts_period = 1.0;         // ns a tick (limits.timestampPeriod)
  std::vector<float> gpu_samples; // ms, this log window
  uint64_t gpu_bad = 0, gpu_not_ready = 0, gpu_bad_total = 0, gpu_not_ready_total = 0;  // window / whole run
  void CollectGpuTime(FrameSlot& slot);
  void LogGpuWindow(const char* why);
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

  ImageId dummy_image = kNoImage;
  ImageId dummy_cube = kNoImage;
  ImageId dummy_volume = kNoImage;
  VkSampler dummy_sampler = VK_NULL_HANDLE;
  bool filterable[uint32_t(Format::Count)] = {};
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
    caps_summary_at = std::chrono::steady_clock::now();
    Log("[rdr] format caps summary: linear->nearest slots %llu, blits->nearest %llu, blend off %llu, vertex unsupported %llu; illegal (fallbacks off) linear %llu, blend %llu",
        static_cast<unsigned long long>(cap_stats.linear_to_nearest), static_cast<unsigned long long>(cap_stats.blits_to_nearest), static_cast<unsigned long long>(cap_stats.blend_disabled),
        static_cast<unsigned long long>(cap_stats.vertex_unsupported), static_cast<unsigned long long>(cap_stats.illegal_linear), static_cast<unsigned long long>(cap_stats.illegal_blend));
  }
  // Review SF1: a counter changed. The summary is written the first time and then at most every 10 s (a phone or a bug-button zip rarely reaches Shutdown).
  void CapsChanged() {
    caps_summary_dirty = true;
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
  ImageId display_curve = kNoImage;
  VkSampler present_sampler = VK_NULL_HANDLE;
  uint16_t present_program = 0;
  uint16_t present_sgsr_program = 0;  // settings parity S10a
  // Settings parity S11: the app's overlay.
  uint16_t overlay_program = 0, overlay_layout = 0;
  std::unordered_map<uint32_t, ImageId> overlay_textures;
  std::shared_ptr<const OverlayList> overlay;
  void RecordOverlay(VkCommandBuffer cmd, FrameSlot& slot, VkDeviceSize ring_base, uint32_t present_class);
  // Settings parity S10b: DLSS.
  Renderer* self = nullptr;
  const DlssInputs* frame_dlss = nullptr;  // the frame being recorded's
  uint16_t dlss_motion_program = 0, present_dlss_program = 0;
  ImageId dlss_motion = kNoImage, dlss_out = kNoImage;
  uint32_t dlss_motion_w = 0, dlss_motion_h = 0, dlss_out_w = 0, dlss_out_h = 0;
  uint64_t dlss_count = 0;
  uint64_t last_desired_ns = 0;  // S100: the last desired present time (display timing)
  VkDeviceSize prev_constants_base = 0;  // S10b: where this frame's prev_constants went in the ring
  bool dlss_had_history = false;
  bool RecordDlss(VkCommandBuffer cmd, FrameSlot& slot, VkDeviceSize ring_base, uint32_t out_w, uint32_t out_h);
  uint16_t present_layout = 0;

  std::vector<Program> programs;
  std::vector<VertexLayoutDesc> layouts;

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
  bool PrebuildOne(uint32_t index);
  void StoreSave(bool final);
  void StoreMaybeSave();
  void StoreSummary(const char* when);

  std::mutex upload_mutex;
  std::vector<ImageId> new_images;
  std::vector<ImageUpload> image_uploads;
  std::vector<BufferUpload> buffer_uploads;
  std::atomic<uint64_t> staged_bytes{0};
  std::condition_variable staging_drained;
  std::mutex destroy_mutex;
  std::vector<PendingDestroy> destroy_queue;

  // Per-frame working storage, reused so a frame allocates nothing once warm.
  std::vector<PassPlan> passes;
  std::vector<Step> steps;
  std::vector<SampledImage> sampled;
  std::vector<uint8_t> next_use;
  std::vector<uint8_t> stencil_needed;
  std::deque<TargetsOp> trimmed_targets;  // a pass's targets without the depth it never uses (Compile; stable addresses)
  std::vector<uint8_t> state_needed;
  std::vector<uint8_t> clear_absorbed;
  std::vector<uint32_t> image_stamp;
  std::vector<uint32_t> sampled_index;
  std::vector<uint32_t> written_stamp;
  uint32_t stamp = 0;
  uint32_t write_stamp = 0;
  uint32_t last_persistent_count = ~0u;
  std::vector<VkDescriptorSet> frame_sets;
  std::vector<VkImageMemoryBarrier> barriers;
  VkPipelineStageFlags barrier_src = 0, barrier_dst = 0;
  std::vector<std::pair<VkPipeline, uint32_t>> state_pipelines;  // last pipeline per state index, with its pass class

  float scale = 1.0f;

  // ---- setup ----
  bool Init(const RendererOptions& opts, std::string& error);
  void Shutdown();

  // ---- resources ----
  VkImageView TargetView(ImageEntry& entry, uint32_t mip, uint32_t layer);
  VkImageView SampledView(ImageEntry& entry, const TextureSlot& slot);
  void DrainRetired(FrameSlot& slot);
  void ProcessDestroys(FrameSlot& slot);
  void WaitForStagingRoom();
  ImageId DummyFor(uint8_t kind) const { return kind == 1 ? dummy_cube : kind == 2 ? dummy_volume : dummy_image; }
  void ApplyUploads(VkCommandBuffer cmd, FrameSlot& slot);

  // ---- frame ----
  void Render(const Frame& frame, RenderMode mode);
  void Compile(const Frame& frame);
  void Transition(ImageEntry& entry, uint32_t first_sub, uint32_t sub_count, VkImageLayout layout, VkPipelineStageFlags stage, VkAccessFlags access, bool discard);
  void FlushBarriers(VkCommandBuffer cmd);
  void RecordPass(VkCommandBuffer cmd, const Frame& frame, const PassPlan& pass, FrameSlot& slot, VkDeviceSize ring_base);
  void RecordCopy(VkCommandBuffer cmd, const CopyOp& copy);
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
  if (opts.dlss_mode >= 0) device_options.streamline_dir = opts.streamline_dir;  // S10b
  device_options.format_caps_mask = opts.format_caps_mask;
  if (!device.Create(device_options, error)) return false;
  allocator.Init(&device);
  VkDevice dev = device.device();
  caps_fallback = !(opts.format_caps_mask & kCapFallbacksOff);
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
    if (gpu_timestamps) {  // S101 lane22
      VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      query.queryType = VK_QUERY_TYPE_TIMESTAMP;
      query.queryCount = 2;
      vkCreateQueryPool(dev, &query, nullptr, &slot.timestamps);
    }
  }
  if (windowed && !swapchain.Create(&device, opts.window, opts.vsync, error)) return false;
  return true;
}

void Renderer::Impl::Shutdown() {
  VkDevice dev = device.device();
  if (!dev) return;
  vkDeviceWaitIdle(dev);
  for (FrameSlot& s : slots) CollectGpuTime(s);  // lane 269: the last frames
  LogGpuWindow("last window");
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
    vkDestroyFence(dev, slot.fence, nullptr);
    vkDestroyCommandPool(dev, slot.pool, nullptr);
    if (slot.timestamps) vkDestroyQueryPool(dev, slot.timestamps, nullptr);  // S101 lane22
  }
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
  for (const ImageUpload& upload : image_uploads) vkDestroyBuffer(dev, upload.staging, nullptr);
  for (const BufferUpload& upload : buffer_uploads) vkDestroyBuffer(dev, upload.staging, nullptr);
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
  ProgramDesc overlay;  // settings parity S11
  overlay.name = "rdr_overlay";
  overlay.vertex_spirv = kSpv_rdr_overlay_vert;
  overlay.vertex_bytes = sizeof(kSpv_rdr_overlay_vert);
  overlay.fragment_spirv = kSpv_rdr_overlay_frag;
  overlay.fragment_bytes = sizeof(kSpv_rdr_overlay_frag);
  renderer->impl_->overlay_program = renderer->AddProgram(overlay);
  renderer->impl_->self = renderer.get();  // S10b
  ProgramDesc dlss_motion;  // S10b: DLSS's motion vectors
  dlss_motion.name = "rdr_dlss_motion";
  dlss_motion.vertex_spirv = kSpv_rdr_fullscreen_vert;
  dlss_motion.vertex_bytes = sizeof(kSpv_rdr_fullscreen_vert);
  dlss_motion.fragment_spirv = kSpv_rdr_dlss_motion_frag;
  dlss_motion.fragment_bytes = sizeof(kSpv_rdr_dlss_motion_frag);
  renderer->impl_->dlss_motion_program = renderer->AddProgram(dlss_motion);
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

void Renderer::SetOverlayTexture(uint32_t id, uint32_t width, uint32_t height, const uint8_t* rgba) {
  Impl& im = *impl_;
  if (const auto it = im.overlay_textures.find(id); it != im.overlay_textures.end()) DestroyImage(it->second);
  ImageDesc desc;
  desc.width = width;
  desc.height = height;
  desc.format = Format::RGBA8;
  const ImageId image = CreateImage(desc);
  UpdateImage(image, 0, 0, rgba, size_t(width) * height * 4);
  im.overlay_textures[id] = image;
}

void Renderer::SetOverlay(std::shared_ptr<const OverlayList> list) { impl_->overlay = std::move(list); }
ImageId Renderer::dlss_output() const { return impl_->dlss_count ? impl_->dlss_out : kNoImage; }
bool Renderer::dlss_in_last_present() const { return impl_->dlss_count && impl_->dlss_had_history; }

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
  entry.format = ToVk(desc.format, im.device.caps());
  entry.depth = desc.format == Format::Depth;
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
  im.WaitForStagingRoom();
  VkDevice dev = im.device.device();
  ImageUpload upload{image, mip, layer, VK_NULL_HANDLE, {}};
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
    if (item.framebuffer) vkDestroyFramebuffer(dev, item.framebuffer, nullptr);
    if (item.view) vkDestroyImageView(dev, item.view, nullptr);
    if (item.image) vkDestroyImage(dev, item.image, nullptr);
    if (item.buffer) vkDestroyBuffer(dev, item.buffer, nullptr);
    if (item.memory.block >= 0) allocator.Free(item.memory);
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
  if (staged_bytes.load(std::memory_order_relaxed) < kMaxStagedBytes) return;
  std::unique_lock<std::mutex> lock(upload_mutex);
  staging_drained.wait_for(lock, std::chrono::seconds(2), [&] { return staged_bytes.load(std::memory_order_relaxed) < kMaxStagedBytes; });
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
}

void Renderer::DestroyImage(ImageId image) {
  Impl& im = *impl_;
  std::lock_guard<std::mutex> lock(im.destroy_mutex);
  PendingDestroy item;
  item.image = image;
  item.ready_serial = im.serial + kDestroyDelayFrames;
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

void Renderer::UpdateBuffer(BufferId buffer, size_t offset, const void* data, size_t bytes) {
  Impl& im = *impl_;
  im.WaitForStagingRoom();
  BufferEntry& entry = im.buffers[buffer];
  VkDevice dev = im.device.device();
  BufferUpload upload{VK_NULL_HANDLE, {}, entry.buffer, offset, bytes};
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

void Renderer::DestroyBuffer(BufferId buffer) {
  Impl& im = *impl_;
  std::lock_guard<std::mutex> lock(im.destroy_mutex);
  PendingDestroy item;
  item.buffer = buffer;
  item.ready_serial = im.serial + kDestroyDelayFrames;
  im.destroy_queue.push_back(item);
}

uint16_t Renderer::AddProgram(const ProgramDesc& desc) {
  Impl& im = *impl_;
  uint16_t same = 0;
  if (im.store_on && im.StoreFindProgram(desc, same)) return same;  // lane 276: the same words are the same program (the prebuilt one)
  Program program;
  program.name = desc.name;
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = desc.vertex_bytes;
  info.pCode = desc.vertex_spirv;
  vkCreateShaderModule(im.device.device(), &info, nullptr, &program.vertex);
  if (desc.fragment_spirv) {  // none: a depth-only program
    info.codeSize = desc.fragment_bytes;
    info.pCode = desc.fragment_spirv;
    vkCreateShaderModule(im.device.device(), &info, nullptr, &program.fragment);
  }
  im.programs.push_back(program);
  if (im.store_on) im.StoreProgramAdded(uint16_t(im.programs.size() - 1));
  return uint16_t(im.programs.size() - 1);
}

uint16_t Renderer::AddVertexLayout(const VertexLayoutDesc& desc) {
  if (impl_->store_on) {  // lane 276: the same content is the same layout (the prebuilt one), kept canonical
    const uint64_t hash = pstore::HashLayout(desc);
    const VertexLayoutDesc canon = pstore::Canonical(desc);
    auto found = impl_->layout_by_hash.find(hash);
    if (found != impl_->layout_by_hash.end()) {
      const VertexLayoutDesc& have = impl_->layouts[found->second];
      if (have.attribute_count == canon.attribute_count && std::memcmp(have.attributes, canon.attributes, sizeof(VertexAttribute) * canon.attribute_count) == 0 &&
          std::memcmp(have.strides, canon.strides, sizeof(canon.strides)) == 0 && std::memcmp(have.per_instance, canon.per_instance, sizeof(canon.per_instance)) == 0)
        return found->second;
      impl_->layouts.push_back(canon);  // a 64-bit hash collision (never seen): its own id, unindexed
      return uint16_t(impl_->layouts.size() - 1);
    }
    impl_->layouts.push_back(canon);
    impl_->layout_by_hash.emplace(hash, uint16_t(impl_->layouts.size() - 1));
    return uint16_t(impl_->layouts.size() - 1);
  }
  impl_->layouts.push_back(desc);
  return uint16_t(impl_->layouts.size() - 1);
}

VkImageView Renderer::Impl::TargetView(ImageEntry& entry, uint32_t mip, uint32_t layer) {
  const uint32_t key = mip << 8 | layer;
  for (const auto& view : entry.target_views) {
    if (view.first == key) return view.second;
  }
  VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  info.image = entry.image;
  info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  info.format = entry.format;
  info.subresourceRange = {entry.aspect, mip, 1, layer, 1};
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
  {
    std::lock_guard<std::mutex> lock(upload_mutex);
    fresh.swap(new_images);
    image_work.swap(image_uploads);
    buffer_work.swap(buffer_uploads);
  }
  // Nothing is ever undefined: new images start cleared to transparent black (TEXTURES_FRAME C22, C32), unless this frame uploads every one of
  // their subresources. Block-compressed formats cannot be cleared; they always come with their upload (a file texture).
  std::unordered_map<ImageId, uint32_t> uploads_per_image;
  for (const ImageUpload& upload : image_work) ++uploads_per_image[upload.image];
  for (ImageId id : fresh) {
    ImageEntry& entry = images[id];
    if (!entry.live || (entry.desc.usage & kUsageTransient)) continue;
    const uint32_t all = uint32_t(entry.states.size());
    const bool compressed = entry.desc.format >= Format::BC1 && entry.desc.format <= Format::EAC_RG11;
    const bool fully_uploaded = uploads_per_image[id] >= all;
    if ((entry.desc.usage & kUsageTarget) || (!compressed && !fully_uploaded)) {
      Transition(entry, 0, all, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, true);
      FlushBarriers(cmd);
      VkImageSubresourceRange range{entry.aspect, 0, entry.desc.mips, 0, entry.layers};
      if (entry.depth) {
        const VkClearDepthStencilValue zero{0.0f, 0};
        vkCmdClearDepthStencilImage(cmd, entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
      } else {
        const VkClearColorValue zero{};
        vkCmdClearColorImage(cmd, entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
      }
      // A sampled-only image is never transitioned at draw time: it waits in the layout shaders read (an upload below moves what it writes).
      if (!(entry.desc.usage & kUsageTarget))
        Transition(entry, 0, all, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, false);
    }
    // Compressed, or fully uploaded this frame: left alone. Each upload starts its subresource from undefined itself. A compressed image sampled
    // before its contents arrive is then a validation error by layout, which is the right signal (an adapter bug, never a picture to keep).
  }
  FlushBarriers(cmd);
  for (const ImageUpload& upload : image_work) {
    ImageEntry& entry = images[upload.image];
    Retired retired;
    retired.buffer = upload.staging;
    retired.memory = upload.memory;
    retired.staged_bytes = upload.memory.size;
    slot.retired.push_back(retired);
    g_render_times.upload_bytes += upload.memory.size;
    if (!entry.live) continue;
    const uint32_t sub = upload.mip * entry.layers + upload.layer;
    Transition(entry, sub, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, true);
    FlushBarriers(cmd);
    VkBufferImageCopy region{};
    region.imageSubresource = {VkImageAspectFlags(entry.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), upload.mip, upload.layer, 1};
    // A scaled image (the render scale) gets the game's contents at the game's own size (S100, the 1.5x check run 7022: a 1280x720 upload
    // copied as 1920x1080 read past its buffer): copied into a temporary image of that size, then blitted up (linear) onto the scaled one.
    const uint32_t guest_w = std::max(1u, uint32_t(entry.desc.width) >> upload.mip), guest_h = std::max(1u, uint32_t(entry.desc.height) >> upload.mip);
    const uint32_t scaled_w = std::max(1u, entry.width >> upload.mip), scaled_h = std::max(1u, entry.height >> upload.mip);
    if (!entry.depth && (guest_w != scaled_w || guest_h != scaled_h)) {
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
          VkBufferImageCopy guest_region{};
          guest_region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          guest_region.imageExtent = {guest_w, guest_h, 1};
          vkCmdCopyBufferToImage(cmd, upload.staging, temp.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &guest_region);
          VkImageMemoryBarrier to_src = to_dst;
          to_src.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
          to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
          vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_src);
          VkImageBlit blit{};
          blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          blit.srcOffsets[1] = {int32_t(guest_w), int32_t(guest_h), 1};
          blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, upload.mip, upload.layer, 1};
          blit.dstOffsets[1] = {int32_t(scaled_w), int32_t(scaled_h), 1};
          vkCmdBlitImage(cmd, temp.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, BlitFilterFor(entry.desc.format, true));
        } else {
          Problem("scaled_upload_memory", "%ux%u", guest_w, guest_h);
        }
        slot.retired.push_back(temp);  // destroyed (with its memory) once this slot's GPU work is done
      }
    } else {
      region.imageExtent = {scaled_w, scaled_h, 1};
      vkCmdCopyBufferToImage(cmd, upload.staging, entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    Transition(entry, sub, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, false);
    FlushBarriers(cmd);
  }
  if (!buffer_work.empty()) {
    VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    before.srcAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
    for (const BufferUpload& upload : buffer_work) {
      VkBufferCopy region{0, upload.offset, upload.bytes};
      vkCmdCopyBuffer(cmd, upload.staging, upload.dest, 1, &region);
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
  }
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
  const uint32_t stage_count = program.fragment ? 2 : 1;

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
  if (vkCreateGraphicsPipelines(device.device(), pipeline_cache, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
    if (building_prebuilt) Log("[rdr][pipestore] a listed pipeline was refused by the driver: program %s, class %u (dropped from the list)", program.name ? program.name : "?", pass_class);
    else Problem("pipeline_failed", "program %s, layout %u", program.name ? program.name : "?", unsigned(state.vertex_layout));
    return VK_NULL_HANDLE;
  }
  last_build_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - create_start).count());  // lane 276
  const double build_ms = double(last_build_ns) / 1e6;
  if (building_prebuilt) Log("[rdr] pipeline prebuilt from the list: program %s, class %u, %.2f ms", program.name ? program.name : "?", pass_class, build_ms);
  else Log("[rdr] pipeline built on demand: program %s, class %u, %.2f ms", program.name ? program.name : "?", pass_class, build_ms);
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
  pipelines.emplace(key, PipelineEntry{pipeline, -1});
  if (store_on && pipeline) StoreNoteBuilt(state, pass_class, int64_t(last_build_ns));
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
  r.pass.color_count = ck.color_count, r.pass.has_depth = ck.has_depth, r.pass.samples = ck.samples, r.pass.pad = 0;
  r.seen = store_session;
  r.built = true;
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
  if (store_records.size() >= pstore::kMaxRecords * 2) return;  // far past the cap: the save drops the oldest anyway
  store_index.emplace(key, uint32_t(store_records.size()));
  store_records.push_back(r);
  store_list_dirty = true;
}

// true: nothing more to do for this record (built, skipped or dropped). false: its program does not exist yet.
bool Renderer::Impl::PrebuildOne(uint32_t index) {
  pstore::Record& r = store_records[index];
  if (r.built || r.dead) return true;
  uint16_t program = 0;
  ProgramInfo want{r.vhash, r.fhash};
  auto known = program_by_hash.find(pstore::HashBytes(&want, sizeof(want)));
  if (known != program_by_hash.end()) {
    program = known->second;
  } else {
    auto v = store_catalog.find(r.vhash);
    if (v == store_catalog.end()) return false;
    auto f = r.fhash ? store_catalog.find(r.fhash) : store_catalog.end();
    if (r.fhash && f == store_catalog.end()) return false;
    store_names.emplace_back(r.name);
    ProgramDesc desc;
    desc.name = store_names.back().c_str();
    desc.vertex_spirv = v->second.words, desc.vertex_bytes = v->second.bytes;
    if (r.fhash) desc.fragment_spirv = f->second.words, desc.fragment_bytes = f->second.bytes;
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
  if (store_retry && !store_unresolved.empty()) {  // a program or the catalog arrived since: the waiting records get another try
    store_pending.insert(store_pending.end(), store_unresolved.begin(), store_unresolved.end());
    store_unresolved.clear();
  }
  store_retry = false;
  if (store_cursor >= store_pending.size()) return;
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
      if (r.dead) continue;
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
      if (pstore::WriteFileAtomic(store_dir / "pipelines.bin", pstore::SerialiseList(keep, written_session, store_device, store_caps_mask))) {
        store_saved_list.store(keep.size(), std::memory_order_relaxed);
      } else {
        store_redo_list.store(true);
        Log("[rdr][pipestore] could not write the list in %s", store_dir.string().c_str());
      }
    }
    if (do_cache) {
      if (pstore::WriteFileAtomic(store_dir / "pipeline_cache.bin", pstore::SerialiseCache(store_device, blob.data(), blob.size()))) {
        store_saved_cache.store(blob.size(), std::memory_order_relaxed);
      } else {
        store_redo_cache.store(true);
        Log("[rdr][pipestore] could not write the cache in %s", store_dir.string().c_str());
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
    store_worker = std::thread([this, job = std::move(job)]() {
      job();
      store_worker_busy.store(false, std::memory_order_release);
    });
  }
}

void Renderer::Impl::StoreMaybeSave() {
  if (!store_on) return;
  if (!store_worker_busy.load(std::memory_order_acquire)) StoreJoin();  // (a finished worker is joined here, never waited for)
  if (!store_list_dirty && !store_cache_dirty && !store_redo_list.load() && !store_redo_cache.load()) return;
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
  for (size_t i = 0; i < count; ++i) {
    if (!blobs[i].words || !blobs[i].bytes) continue;
    Impl::ShaderBlob blob;
    blob.words = blobs[i].words, blob.bytes = blobs[i].bytes, blob.name = blobs[i].name;
    const auto made = im.store_catalog.emplace(pstore::HashWords(blobs[i].words, blobs[i].bytes), blob);
    if (!made.second && (made.first->second.bytes != blob.bytes || std::memcmp(made.first->second.words, blob.words, blob.bytes) != 0))
      Log("[rdr][pipestore] two different shaders share a hash (%s): the second is not in the catalog", blob.name ? blob.name : "?");  // never seen
  }
  im.store_retry = true;
  Log("[rdr][pipestore] shader catalog: %zu shaders", im.store_catalog.size());
}

bool Renderer::pipeline_store_active() const { return impl_->store_on; }

void Renderer::SetPipelinePrebuildAllowed(bool allowed) { impl_->prebuild_allowed = allowed; }

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

VkDescriptorSet Renderer::Impl::GetTextureSet(const Frame& frame, uint32_t index, FrameSlot& slot) {
  VkDescriptorSet& set = frame_sets[index];
  if (set) return set;
  ++g_render_times.texture_sets;
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

void Renderer::Impl::Transition(ImageEntry& entry, uint32_t first_sub, uint32_t sub_count, VkImageLayout layout, VkPipelineStageFlags stage, VkAccessFlags access, bool discard) {
  constexpr VkAccessFlags kWrites = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
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
  vkCmdPipelineBarrier(cmd, barrier_src, barrier_dst, 0, 0, nullptr, 0, nullptr, uint32_t(barriers.size()), barriers.data());
  barriers.clear();
  barrier_src = barrier_dst = 0;
}

// ---- compile: the whole frame is known before a single Vulkan call ----------------------------------------------------------------

void Renderer::Impl::Compile(const Frame& frame) {
  passes.clear();
  steps.clear();
  sampled.clear();
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
          if (clear.flags & kClearStencil) plan.stencil_used = true;
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
        if (draw_state.stencil_test) plan.stencil_used = true;
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
        close(i);
        images[frame.copies[op.index].dest.image].copy_dest = true;
        steps.push_back({StepKind::Copy, op.index, false});
        break;
      case OpKind::Present:
        close(i);
        steps.push_back({StepKind::Present, op.index, false});
        break;
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
        const bool cleared = plan.stencil_load == VK_ATTACHMENT_LOAD_OP_CLEAR;
        const bool needed_before = !cleared && (plan.stencil_used || stencil_needed[depth]);
        plan.stencil_store = stencil_needed[depth] ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        if (!cleared) plan.stencil_load = needed_before ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        stencil_needed[depth] = needed_before;
      }
      for (uint32_t c = 0; c < plan.targets->color_count; ++c) next_use[plan.targets->color[c].image] = plan.load[c] == VK_ATTACHMENT_LOAD_OP_CLEAR ? kNextOverwrite : kNextRead;
      if (depth != kNoImage) next_use[depth] = plan.load[kMaxColorTargets] == VK_ATTACHMENT_LOAD_OP_CLEAR ? kNextOverwrite : kNextRead;
      for (uint32_t k = 0; k < plan.sampled_count; ++k) next_use[sampled[plan.first_sampled + k].image] = kNextRead;
    } else if (step.kind == StepKind::Copy) {
      const CopyOp& copy = frame.copies[step.index];
      const ImageEntry& dest = images[copy.dest.image];
      const bool whole = copy.dest_rect.x == 0 && copy.dest_rect.y == 0 && copy.dest_rect.width >= dest.desc.width && copy.dest_rect.height >= dest.desc.height;
      next_use[copy.dest.image] = whole && single(dest) ? kNextOverwrite : kNextRead;
      next_use[copy.source.image] = kNextRead;
    } else {
      next_use[frame.presents[step.index].source.image] = kNextRead;
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
  // Layouts first: attachments, then anything this pass samples.
  for (uint32_t c = 0; c < targets.color_count; ++c) {
    ImageEntry& entry = images[targets.color[c].image];
    const uint32_t sub = targets.color[c].mip * entry.layers + targets.color[c].layer;
    Transition(entry, sub, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               pass.load[c] != VK_ATTACHMENT_LOAD_OP_LOAD);
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
    framebuffer_key.views[c] = TargetView(entry, targets.color[c].mip, targets.color[c].layer);
    samples = entry.desc.samples;
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
  framebuffer_key.pass = GetRenderPass(pass_key);
  framebuffer_key.width = pass.width;
  framebuffer_key.height = pass.height;

  VkClearValue clear_values[kAttachmentSlots];
  uint32_t clear_count = 0;
  bool any_clear = false;
  for (uint32_t c = 0; c < targets.color_count; ++c) {
    any_clear |= pass.load[c] == VK_ATTACHMENT_LOAD_OP_CLEAR;
    clear_values[clear_count++] = pass.clear[c];
  }
  if (targets.depth.image != kNoImage) {
    any_clear |= pass.load[kMaxColorTargets] == VK_ATTACHMENT_LOAD_OP_CLEAR || pass.stencil_load == VK_ATTACHMENT_LOAD_OP_CLEAR;
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

  const ImageRef& first_target = targets.color_count ? targets.color[0] : targets.depth;
  const bool scaled = images[first_target.image].desc.usage & kUsageScaled;
  const float factor = scaled ? scale : 1.0f;

  VkPipeline bound_pipeline = VK_NULL_HANDLE;
  uint32_t bound_dynamic = ~0u, bound_textures = ~0u;
  BufferRange bound_vertex[2];
  BufferRange bound_index;
  uint8_t bound_index_32 = 0xFF;

  for (uint32_t i = pass.first_op; i < pass.end_op; ++i) {
    const Op& op = frame.ops[i];
    if (op.kind == OpKind::Clear) {
      if (clear_absorbed[op.index]) continue;
      const ClearOp& clear = frame.clears[op.index];
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
        attachments[count].aspectMask = ((clear.flags & kClearDepth) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | ((clear.flags & kClearStencil) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        attachments[count].colorAttachment = 0;
        attachments[count].clearValue.depthStencil = {clear.depth, clear.stencil};
        ++count;
      }
      if (count) {
        VkClearRect rect{};
        rect.layerCount = 1;
        if (clear.full) {
          rect.rect.extent = {pass.width, pass.height};
        } else {
          const int64_t left = std::clamp<int64_t>(int64_t(float(clear.rect.x) * factor), 0, pass.width);
          const int64_t top = std::clamp<int64_t>(int64_t(float(clear.rect.y) * factor), 0, pass.height);
          const int64_t right = std::clamp<int64_t>(int64_t(std::ceil(float(clear.rect.x + int64_t(clear.rect.width)) * factor)), left, pass.width);
          const int64_t bottom = std::clamp<int64_t>(int64_t(std::ceil(float(clear.rect.y + int64_t(clear.rect.height)) * factor)), top, pass.height);
          rect.rect.offset = {int32_t(left), int32_t(top)};
          rect.rect.extent = {uint32_t(right - left), uint32_t(bottom - top)};
        }
        if (rect.rect.extent.width && rect.rect.extent.height) vkCmdClearAttachments(cmd, count, attachments, 1, &rect);
      }
      continue;
    }
    if (op.kind != OpKind::Draw) continue;
    const DrawOp& draw = frame.draws[op.index];

    auto& cached = state_pipelines[draw.state];
    if (!cached.first || cached.second != pass_class) cached = {GetPipeline(frame.states[draw.state], pass_class), pass_class};
    if (!cached.first) continue;  // named by BuildPipeline
    if (cached.first != bound_pipeline) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cached.first);
      bound_pipeline = cached.first;
    }

    if (draw.dynamic != bound_dynamic) {
      bound_dynamic = draw.dynamic;
      const DynamicState& dyn = frame.dynamics[draw.dynamic];
      VkViewport viewport{dyn.viewport[0] * factor, dyn.viewport[1] * factor, dyn.viewport[2] * factor, dyn.viewport[3] * factor, dyn.viewport[4], dyn.viewport[5]};
      vkCmdSetViewport(cmd, 0, 1, &viewport);
      VkRect2D scissor{};
      scissor.offset = {std::max(0, int32_t(float(dyn.scissor.x) * factor)), std::max(0, int32_t(float(dyn.scissor.y) * factor))};
      scissor.extent = {uint32_t(std::ceil(float(dyn.scissor.width) * factor)), uint32_t(std::ceil(float(dyn.scissor.height) * factor))};
      vkCmdSetScissor(cmd, 0, 1, &scissor);
      vkCmdSetDepthBias(cmd, dyn.depth_bias_constant, dyn.depth_bias_clamp, dyn.depth_bias_slope);
      vkCmdSetBlendConstants(cmd, dyn.blend_constants);
      vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dyn.stencil_compare_mask);
      vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dyn.stencil_write_mask);
      vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dyn.stencil_reference);
      vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(dyn.system), dyn.system);
    }

    const uint32_t offsets[2] = {uint32_t(ring_base + draw.vs_constants),
                                 uint32_t((draw.flags & kDrawOpPrevConstants) ? prev_constants_base + draw.vs_constants : ring_base + draw.fs_constants)};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &constants_set, 2, offsets);
    if (draw.textures != bound_textures) {
      VkDescriptorSet set = GetTextureSet(frame, draw.textures, slot);
      if (!set) continue;  // named by GetTextureSet
      bound_textures = draw.textures;
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 1, 1, &set, 0, nullptr);
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
    }
    if (draw.index.buffer != kNoBuffer) {
      if (draw.index.buffer != bound_index.buffer || draw.index.offset != bound_index.offset || draw.index_32bit != bound_index_32) {
        bound_index = draw.index;
        bound_index_32 = draw.index_32bit;
        const bool frame_owned = draw.index.buffer == kFrameGeometry;
        VkBuffer buffer = frame_owned ? ring : buffers[draw.index.buffer].buffer;
        vkCmdBindIndexBuffer(cmd, buffer, draw.index.offset + (frame_owned ? geometry_base : 0), draw.index_32bit ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
      }
      vkCmdDrawIndexed(cmd, draw.count, draw.instances, draw.first, draw.base_vertex, 0);
    } else {
      vkCmdDraw(cmd, draw.count, draw.instances, draw.first, 0);
    }
  }
  vkCmdEndRenderPass(cmd);
}

void Renderer::Impl::RecordCopy(VkCommandBuffer cmd, const CopyOp& copy) {
  ImageEntry& source = images[copy.source.image];
  ImageEntry& dest = images[copy.dest.image];
  const uint32_t source_sub = copy.source.mip * source.layers + copy.source.layer;
  const uint32_t dest_sub = copy.dest.mip * dest.layers + copy.dest.layer;
  const bool whole = copy.dest_rect.x == 0 && copy.dest_rect.y == 0 && copy.dest_rect.width >= dest.desc.width && copy.dest_rect.height >= dest.desc.height;
  const float source_factor = (source.desc.usage & kUsageScaled) ? scale : 1.0f;
  const float dest_factor = (dest.desc.usage & kUsageScaled) ? scale : 1.0f;
  const auto origin = [](int32_t value, float factor) { return int32_t(float(value) * factor); };
  const auto extent = [](uint32_t value, float factor) { return uint32_t(std::ceil(float(value) * factor)); };
  const int32_t source_x = origin(copy.source_rect.x, source_factor), source_y = origin(copy.source_rect.y, source_factor);
  const int32_t dest_x = origin(copy.dest_rect.x, dest_factor), dest_y = origin(copy.dest_rect.y, dest_factor);
  const uint32_t source_limit_w = std::max(1u, source.width >> copy.source.mip), source_limit_h = std::max(1u, source.height >> copy.source.mip);
  const uint32_t dest_limit_w = std::max(1u, dest.width >> copy.dest.mip), dest_limit_h = std::max(1u, dest.height >> copy.dest.mip);
  uint32_t source_w = std::min(extent(copy.source_rect.width, source_factor), source_limit_w - uint32_t(source_x));
  uint32_t source_h = std::min(extent(copy.source_rect.height, source_factor), source_limit_h - uint32_t(source_y));
  uint32_t dest_w = std::min(extent(copy.dest_rect.width, dest_factor), dest_limit_w - uint32_t(dest_x));
  uint32_t dest_h = std::min(extent(copy.dest_rect.height, dest_factor), dest_limit_h - uint32_t(dest_y));

  Transition(source, source_sub, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, false);
  Transition(dest, dest_sub, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, whole && dest.desc.mips == 1 && dest.layers == 1);
  FlushBarriers(cmd);

  const bool same_block = source.format == dest.format || (!source.depth && !dest.depth && BlockBytes(source.desc.format) != 0 && BlockBytes(source.desc.format) == BlockBytes(dest.desc.format));
  if (same_block && source_w == dest_w && source_h == dest_h) {
    // Every copy the game makes (GAME_RULES_TEXTURES_FRAME C4): raw bits, same size. A depth copy stays a depth image and is read as depth.
    VkImageCopy region{};
    region.srcSubresource = {source.aspect, copy.source.mip, copy.source.layer, 1};
    region.srcOffset = {source_x, source_y, 0};
    region.dstSubresource = {dest.aspect, copy.dest.mip, copy.dest.layer, 1};
    region.dstOffset = {dest_x, dest_y, 0};
    region.extent = {source_w, source_h, 1};
    vkCmdCopyImage(cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  } else if (!source.depth && !dest.depth && device.FormatHas(source.format, VK_FORMAT_FEATURE_BLIT_SRC_BIT) && device.FormatHas(dest.format, VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
    VkImageBlit blit{};
    blit.srcSubresource = {source.aspect, copy.source.mip, copy.source.layer, 1};
    blit.srcOffsets[0] = {source_x, source_y, 0};
    blit.srcOffsets[1] = {source_x + int32_t(source_w), source_y + int32_t(source_h), 1};
    blit.dstSubresource = {dest.aspect, copy.dest.mip, copy.dest.layer, 1};
    blit.dstOffsets[0] = {dest_x, dest_y, 0};
    blit.dstOffsets[1] = {dest_x + int32_t(dest_w), dest_y + int32_t(dest_h), 1};
    vkCmdBlitImage(cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, BlitFilterFor(source.desc.format, copy.filter == CopyFilter::Linear));
  } else {
    Problem("copy_shape_unsupported", "format %d to %d, %ux%u to %ux%u", int(source.desc.format), int(dest.desc.format), source_w, source_h, dest_w, dest_h);
  }
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
  if (!swapchain.Acquire(slot.available, image_index)) return false;

  PassClassKey class_key{};
  class_key.formats[0] = uint16_t(swapchain.format());
  class_key.color_count = 1;
  class_key.samples = 1;
  class_key.pad = 1;  // its own class: pipelines here are built against the swapchain's render pass, whose external dependency makes it a different one
  const uint32_t present_class = GetPassClass(class_key);
  class_passes[present_class] = swapchain.render_pass();
  DrawState state{};
  // Settings parity S10a: SGSR only where the picture is made bigger on its way to the screen.
  const bool sgsr_turned = (swapchain.quarter_turns() & 1) != 0;
  const float sgsr_out_w = float(sgsr_turned ? swapchain.extent().height : swapchain.extent().width);
  const float sgsr_out_h = float(sgsr_turned ? swapchain.extent().width : swapchain.extent().height);
  const bool sgsr = options.present_sgsr && (sgsr_out_w > constants[6] * float(source.width) * 1.01f ||
                                             sgsr_out_h > constants[7] * float(source.height) * 1.01f);
  state.program = dlss ? present_dlss_program : sgsr ? present_sgsr_program : present_program;  // S10b: DLSS replaces SGSR
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

  VkClearValue clear{};
  VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  begin.renderPass = swapchain.render_pass();
  begin.framebuffer = swapchain.framebuffer(image_index);
  begin.renderArea.extent = swapchain.extent();
  begin.clearValueCount = 1;
  begin.pClearValues = &clear;
  vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
  if (pipeline) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
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
  vkCmdEndRenderPass(cmd);
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
  RenderTimes& rt = g_render_times;
  uint64_t t = RenderNow();
  auto lap = [&](uint64_t& acc) {
    const uint64_t now = RenderNow();
    acc += now - t;
    t = now;
  };
  const uint32_t slot_index = uint32_t(serial % slots.size());
  FrameSlot& slot = slots[slot_index];
  VkDevice dev = device.device();
  vkWaitForFences(dev, 1, &slot.fence, VK_TRUE, UINT64_MAX);
  readback.OnSlotWaited(slot_index, serial);  // lane 232: this slot's older read-back copies are complete
  lap(rt.wait);
  rt.retired_items += slot.retired.size();
  DrainRetired(slot);
  lap(rt.drain);
  ProcessDestroys(slot);
  lap(rt.destroys);
  vkResetCommandPool(dev, slot.pool, 0);
  vkResetDescriptorPool(dev, slot.descriptors, 0);
  lap(rt.pools);
  CollectGpuTime(slot);  // lane 269
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
  if (prev_offset + frame.prev_constants.count + kConstantRange + kOverlayRange + kDlssConstants > kRingSection) {
    Problem("frame_ring_full", "constants %u, geometry %u bytes", frame.constants.count, frame.geometry.count);
    return;
  }
  if (frame.constants.count) std::memcpy(ring_memory.mapped + ring_base, frame.constants.data, frame.constants.count);
  if (frame.geometry.count) std::memcpy(ring_memory.mapped + ring_base + geometry_offset, frame.geometry.data, frame.geometry.count);
  if (frame.prev_constants.count) std::memcpy(ring_memory.mapped + prev_constants_base, frame.prev_constants.data, frame.prev_constants.count);

  frame_sets.assign(frame.texture_sets.count, VK_NULL_HANDLE);
  state_pipelines.assign(frame.states.count, {VK_NULL_HANDLE, 0});

  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(slot.cmd, &begin);
  if (slot.timestamps) {  // S101 lane22
    vkCmdResetQueryPool(slot.cmd, slot.timestamps, 0, 2);
    vkCmdWriteTimestamp(slot.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, slot.timestamps, 0);
  }
  last_slot = slot_index;
  lap(rt.retire);
  ApplyUploads(slot.cmd, slot);
  lap(rt.uploads);

  Compile(frame);
  lap(rt.compile);
  rt.passes += passes.size();
  rt.steps += steps.size();
  // Lane 232: the read-back requests recorded into THIS frame. (The frame_ring_full early return above happens before this: a request queued for such a frame stays pending and lands in a later frame.)
  readback.TakePending(readback_taking);
  readback_recorded.assign(readback_taking.size(), 0);
  bool presenting = false;
  uint32_t image_index = 0;
  for (const Step& step : steps) {
    if (step.kind != StepKind::Present) {
      if (mode == RenderMode::PersistentOnly && !step.persistent) continue;
      if (mode == RenderMode::DisplayOnly && step.persistent) continue;
    }
    if (step.kind == StepKind::Pass) {
      RecordPass(slot.cmd, frame, passes[step.index], slot, ring_base);
    } else if (step.kind == StepKind::Copy) {
      RecordCopy(slot.cmd, frame.copies[step.index]);
      if (!readback_taking.empty()) RecordReadbacksAfterCopy(slot.cmd, frame.copies[step.index]);
    } else if (windowed && !presenting && mode != RenderMode::PersistentOnly) {
      presenting = RecordPresent(slot.cmd, frame.presents[step.index], slot, ring_base, image_index);
    }
  }
  RecordReadbacks(slot.cmd);  // lane 232: after every step, so the copy sees this frame's result
  if (slot.timestamps) {  // S101 lane22, lane 269
    vkCmdWriteTimestamp(slot.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.timestamps, 1);
    slot.ts_pending = true;
    slot.ts_submit_ns = RenderNow();
  }
  vkEndCommandBuffer(slot.cmd);
  lap(rt.record);

  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &slot.cmd;
  const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSemaphore finished = VK_NULL_HANDLE;
  if (presenting) {
    finished = swapchain.finished_semaphore(image_index);
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &slot.available;
    submit.pWaitDstStageMask = &wait_stage;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &finished;
  }
  vkResetFences(dev, 1, &slot.fence);
  vkQueueSubmit(device.queue(), 1, &submit, slot.fence);
  readback.MarkRecorded(slot_index, serial);  // lane 232: the recorded read-back copies are in flight in this slot
  if (options.gpu_timestamp_plant == 2) CollectGpuTime(slot);  // lane 269 plant: read at once, before the GPU is done; must count as not ready
  lap(rt.submit);
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
    if (swapchain.Present(image_index, finished, desired)) {  // lane 256: only an accepted present counts
      g_presented.fetch_add(1, std::memory_order_relaxed);  // the overlay's shown rate
    }
  }
  lap(rt.present);
  ++serial;
  if (++rt.frames == 600) {
    const double f = 600.0 * 1e6;
    Log("[rdr][render] 600 frames, ms a frame: wait for the slot %.3f, retire %.3f (drain %.3f, destroys %.3f, pool resets %.3f), uploads %.3f, "
        "compile %.3f, record %.3f (texture sets %.3f, new framebuffers %.3f), submit %.3f, present %.3f; a frame: %.0f passes, %.0f steps, %.0f KB "
        "uploaded, %.0f retired items, %.0f destroys, %.1f new framebuffers (%zu cached), %.0f texture sets",
        rt.wait / f, (rt.retire + rt.drain + rt.destroys + rt.pools) / f, rt.drain / f, rt.destroys / f, rt.pools / f, rt.uploads / f,
        rt.compile / f, rt.record / f, rt.set_ns / f, rt.fb_ns / f, rt.submit / f, rt.present / f, rt.passes / 600.0, rt.steps / 600.0,
        rt.upload_bytes / 600.0 / 1024.0, rt.retired_items / 600.0, rt.destroy_items / 600.0, rt.new_framebuffers / 600.0, framebuffers.size(),
        rt.texture_sets / 600.0);
    if (rt.pipestore) Log("[rdr][render] pipeline store: %.3f ms a frame in the listed-pipeline slices and saves", rt.pipestore / f);
    rt = RenderTimes{};
  }
}

void Renderer::RenderFrame(const Frame& frame, RenderMode mode) { impl_->Render(frame, mode); }

// Lane 269. Called after a slot's fence wait, so its last submit is complete and both results are available without waiting; a not-ready result
// is counted and dropped, never waited for.
void Renderer::Impl::CollectGpuTime(FrameSlot& slot) {
  if (!slot.ts_pending || !slot.timestamps) return;
  slot.ts_pending = false;
  uint64_t ticks[2] = {0, 0};
  const VkResult result = vkGetQueryPoolResults(device.device(), slot.timestamps, 0, 2, sizeof(ticks), ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
  if (result != VK_SUCCESS) {
    ++gpu_not_ready, ++gpu_not_ready_total;
    return;
  }
  const uint64_t mask = ts_valid_bits >= 64 ? ~0ull : ((1ull << ts_valid_bits) - 1);
  const double ns = double((ticks[1] - ticks[0]) & mask) * ts_period;
  const double wall_ns = double(RenderNow() - slot.ts_submit_ns);
  if (!(ns > 0.0) || ns > wall_ns) {  // sanity: (0, time since the submit]
    ++gpu_bad, ++gpu_bad_total;
    return;
  }
  gpu_samples.push_back(float(ns / 1e6));
  rdr::g_gpu_ns_total.fetch_add(uint64_t(ns), std::memory_order_relaxed);
  rdr::g_gpu_last_us.store(uint32_t(std::min(ns / 1000.0, 4.0e9)), std::memory_order_relaxed);
  rdr::g_gpu_frames.fetch_add(1, std::memory_order_relaxed);
  if (gpu_samples.size() >= 600) LogGpuWindow("600 frames");
}

void Renderer::Impl::LogGpuWindow(const char* why) {
  if (gpu_samples.empty() && !gpu_bad && !gpu_not_ready) return;
  std::vector<float> v = gpu_samples;
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  double sum = 0;
  for (float x : v) sum += x;
  Log("[rdr][gpu] %s: frames %zu, gpu ms median %.3f / mean %.3f / p95 %.3f / max %.3f; dropped %llu implausible (outside 0..wall time), %llu not ready",
      why, n, n ? v[n / 2] : 0.0f, n ? sum / double(n) : 0.0, n ? v[std::min(n - 1, size_t(double(n) * 0.95))] : 0.0f, n ? v[n - 1] : 0.0f,
      static_cast<unsigned long long>(gpu_bad), static_cast<unsigned long long>(gpu_not_ready));
  gpu_samples.clear();
  gpu_bad = gpu_not_ready = 0;
}

void Renderer::LogGpuSummary() { impl_->LogGpuWindow("summary"); }
void Renderer::GpuTotals(uint64_t& implausible, uint64_t& not_ready) const { implausible = impl_->gpu_bad_total, not_ready = impl_->gpu_not_ready_total; }

uint64_t Renderer::LastGpuNs() {  // S101 lane22
  Impl& im = *impl_;
  if (!im.gpu_timestamps || im.last_slot == ~0u) return 0;
  FrameSlot& slot = im.slots[im.last_slot];
  vkWaitForFences(im.device.device(), 1, &slot.fence, VK_TRUE, UINT64_MAX);
  uint64_t ticks[2] = {0, 0};
  if (vkGetQueryPoolResults(im.device.device(), slot.timestamps, 0, 2, sizeof(ticks), ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) return 0;
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
bool Renderer::ReadImage(ImageRef image, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
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
  im.Transition(entry, sub, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, false);
  im.FlushBarriers(cmd);
  VkBufferImageCopy region{};
  region.imageSubresource = {entry.aspect, image.mip, image.layer, 1};
  region.imageExtent = {width, height, 1};
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
  region.imageSubresource = {entry.aspect, req.key.mip, req.key.layer, 1};
  region.imageExtent = {width, height, 1};
  vkCmdCopyImageToBuffer(cmd, entry.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, st.buffer, 1, &region);
  VkMemoryBarrier to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, nullptr, 0, nullptr);
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
  region.imageSubresource = {entry.aspect, image.mip, image.layer, 1};
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
    region.imageSubresource = {entry.aspect, r.image.mip, r.image.layer, 1};
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
  if (im.store_on && im.StoreFindProgram(desc, id)) return true;  // lane 276: the same words are the same program
  if (im.programs.size() >= 65535) {
    Problem("program_table_full", "65535 programs: restart the viewer");
    return false;
  }
  Program program;
  program.name = desc.name;
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
  im.programs.push_back(program);
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
