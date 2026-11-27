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
