// The renderer's public vocabulary. Everything here is in host terms: no 360 address, format or register number ever appears.
// A Frame is flat POD arrays, so writing a frame to disk is writing the arrays out.
// Small enums hold Vulkan's own numeric values (VkCompareOp, VkBlendFactor...), which saves a mapping layer without pulling vulkan.h into clients.
#pragma once
#include <cstddef>
#include <cstdint>

namespace rdr {

enum ImageId : uint32_t { kNoImage = 0 };
enum BufferId : uint32_t { kNoBuffer = 0, kFrameGeometry = 0xFFFFFFFFu };  // kFrameGeometry: the frame's own dynamic geometry blob
enum BufferProducer : uint8_t { kBufOther = 0, kBufPosed = 1, kBufCloth = 2 };   // S103 PVR: who asked for a buffer copy (Renderer::UpdateBuffer, the uploads line)

enum class Format : uint8_t {
  Undefined,
  RGBA8,
  RGBA8_SNORM,
  BGRA8,
  R8,
  R8_SNORM,
  RG8,
  RGBA4,
  R5G6B5,
  A1RGB5,
  RGB10A2,
  RGBA16_SNORM,
  RGBA16_UNORM,
  RG16_UNORM,
  RG16_SNORM,
  R16_UNORM,
  RGBA16F,
  RG16F,
  R16F,
  RGBA32F,
  RG32F,
  R32F,
  R32_UINT,
  Depth,  // the device's depth + stencil format, sampled as depth
  BC1,
  BC2,
  BC3,
  BC4,
  BC5,
  ASTC_4x4,
  EAC_R11,
  EAC_RG11,
  Count
};

enum ImageUsage : uint8_t {
  kUsageSampled = 1,
  kUsageTarget = 2,
  kUsageTransient = 4,        // a target whose contents are never read after its pass
  kUsageDeadAtFrameEnd = 8,   // a written game fact: contents are not needed after the frame (dev poison mode proves it)
  kUsageScaled = 16,          // follows the render scale
  kUsageMutable = 32,         // read through another format of the same size. Only when needed: it turns off framebuffer compression on most phones
  kUsageStorage = 64,         // settings parity S10b: written by a compute pass (DLSS's output)
  kUsageLasting = 128,        // S101 names: its role says a later frame reads it (docs\renderer\NAMES.md surface roles): the IMAGE rule of the skip classifier never drops a copy into it (lasting_prune.h PerFrame); the exact-rectangle streak rule still does (a copy of the same rectangle in each of the last 3 frames is per-frame work the drawn frame redoes: img388), and the run-end keep (wwe2k14_rdr_run_end_keep) is what saves the last copy of such a run
};

// S102 S0-3: the "surface role" group a pass's GPU milliseconds are added to (the [rdr][gpu] per-role line). A pass takes its class from its first colour
// target, or its depth target when it has no colour; a copy from its destination; the present step is kGpuOutput. Set with Renderer::SetImageClass.
// S102 G6: ui (menu / tron / ui pictures), frame (the frame buffer the post chain, HUD, text, particles and the movie draw into), copies (resolve copies into textures: 97 a frame in Bryan),
// prelight (the light-cube strips) and feedback (the sweat / damage maps) were all `other`; `other` is now what no role names (a gate holds it under 10 % on Bryan).
enum GpuClass : uint8_t { kGpuOther = 0, kGpuColor, kGpuShadow, kGpuRim, kGpuPlane, kGpuCards, kGpuPost, kGpuOutput, kGpuUi, kGpuCopies, kGpuPrelight, kGpuFeedback, kGpuFrame, kGpuClassCount };
inline const char* GpuClassName(uint32_t c) {
  static const char* const k[] = {"other", "colour", "shadow", "rim", "plane", "cards", "post", "output", "ui", "copies", "prelight", "feedback", "frame"};
  static_assert(sizeof(k) / sizeof(k[0]) == kGpuClassCount, "GpuClassName: one name per GpuClass");  // S102 G6
  return c < kGpuClassCount ? k[c] : "?";
}

enum class ImageKind : uint8_t { Image2D, Cube, Volume };

struct ImageDesc {
  uint16_t width = 0, height = 0, depth = 1;
  uint8_t mips = 1;
  uint8_t layers = 1;
  Format format = Format::Undefined;
  ImageKind kind = ImageKind::Image2D;
  uint8_t usage = kUsageSampled;
  uint8_t samples = 1;
};

struct ImageRef {
  ImageId image = kNoImage;
  uint8_t mip = 0;
  uint8_t layer = 0;
  uint16_t pad = 0;
};

struct Rect {
  int32_t x = 0, y = 0;
  uint32_t width = 0, height = 0;
};

enum class OpKind : uint8_t { Targets, Clear, Draw, Copy, Present, Dispatch /* S102 L1b */ };

struct Op {
  OpKind kind;
  uint32_t index;  // into the array for that kind
};

constexpr int kMaxColorTargets = 4;
// TextureSet slots 0-15 are the pixel stage's, 16-19 the vertex stage's (a vertex shader's textures[i] is slot 16 + i). Each slot is its own binding
// (set 1, binding = slot), declared by each shader as the kind it samples: a vertex texture never aliases a pixel one (S100: the crowd's light map
// read the diffuse texture), and a 2D slot never aliases a cube one.
constexpr int kFragmentTextures = 16;
constexpr int kVertexTextures = 4;
constexpr int kMaxTextures = kFragmentTextures + kVertexTextures;

struct TargetsOp {
  ImageRef color[kMaxColorTargets];
  ImageRef depth;
  uint32_t color_count = 0;
};

enum ClearFlags : uint8_t { kClearColor = 1, kClearDepth = 2, kClearStencil = 4 };

struct ClearOp {
  uint8_t flags = 0;
  uint8_t color_mask = 0xF;  // which of the colour targets
  uint8_t stencil = 0;
  uint8_t full = 0;          // covers the whole of every target it touches
  float color[4] = {};
  float depth = 0;
  Rect rect;                 // ignored when full
};

struct BlendState {
  uint8_t enable, src_color, dst_color, color_op, src_alpha, dst_alpha, alpha_op, write_mask;
};

struct StencilFace {
  uint8_t fail, pass, depth_fail, compare;
};

// Everything baked into a pipeline. All bytes: compared and hashed as raw memory.
constexpr uint8_t kVariantDepthOnly = 1;  // S102 G10: DrawState::variant
struct DrawState {
  uint16_t program;
  uint16_t vertex_layout;
  uint8_t topology;       // VkPrimitiveTopology
  uint8_t cull;           // VkCullModeFlags
  uint8_t front_face;     // VkFrontFace
  uint8_t depth_clamp;
  uint8_t depth_test, depth_write, depth_compare, stencil_test;
  uint8_t depth_bias_enable, primitive_restart;
  uint8_t variant;  // S102 G10 (review F10): 0 = the draw as asked, 1 = the colour pass's depth-only prepass twin (no fragment stage). Zero unless a planner sets it (DrawState values are built with {})
  uint8_t pad;
  StencilFace front, back;
  BlendState blend[kMaxColorTargets];
};
static_assert(offsetof(DrawState, variant) == 14 && offsetof(DrawState, pad) == 15, "S102 G10 review F10: DrawState::variant sits where pad[0] was (the pipeline store hashes raw bytes)");

// Everything set dynamically.
struct DynamicState {
  float viewport[6];  // x, y, width, height, min depth, max depth
  Rect scissor;
  float depth_bias_constant, depth_bias_clamp, depth_bias_slope;
  float blend_constants[4];
  uint8_t stencil_reference, stencil_compare_mask, stencil_write_mask, pad;
  float system[16];   // pushed to every shader: ndc scale xyz_, ndc offset xyz_, alpha test (reference, function bits, -, -), options
};

struct SamplerDesc {
  uint8_t mag_filter, min_filter, mip_mode;     // VkFilter, VkFilter, VkSamplerMipmapMode
  uint8_t address_u, address_v, address_w;      // VkSamplerAddressMode
  uint8_t max_anisotropy;                       // 1 = off
  uint8_t border;                               // VkBorderColor
  float lod_bias, min_lod, max_lod;
};

struct TextureSlot {
  ImageId image = kNoImage;
  uint32_t sampler = 0;                     // index into Frame::samplers
  Format view_format = Format::Undefined;   // Undefined: the image's own. Otherwise a same-size reinterpretation (signed vs unsigned)
  uint8_t swizzle[4] = {0, 0, 0, 0};        // VkComponentSwizzle, 0 = identity. How a copy's channel order and one-channel textures are read
  uint8_t kind = 0;                         // 0 2D, 1 cube, 2 volume: what the shader samples this slot as, even when it is empty
  uint8_t vertex = 0;                       // a vertex shader reads it
  uint8_t pad = 0;
};

struct NativeWindow {
  void* instance = nullptr;  // Windows: HINSTANCE. Android: unused.
  void* window = nullptr;    // Windows: HWND. Android: ANativeWindow*.
  // Lane 235 (Android): the window's handle right now, asked every frame by the swapchain's surface lifecycle (the ANativeWindow is
  // destroyed in the background and a new one made on return). Null function: `window` is the only window. May return null (none).
  void* (*current)(void* user) = nullptr;
  void* current_user = nullptr;
};

struct TextureSet {
  TextureSlot slots[kMaxTextures];
};

struct BufferRange {
  BufferId buffer = kNoBuffer;
  uint32_t offset = 0;
};

// DrawOp::flags. S10b: the draw's binding 1 is the previous tick's vertex constants (Frame::prev_constants at vs_constants): a velocity draw.
enum DrawOpFlags : uint8_t { kDrawOpPrevConstants = 1, kDrawOpAlphaOpaque = 2 /* S102 G10 review F2: the binder proved this draw's output alpha is 1 (colour_cuts.h) */, kDrawOpBlendOff = 4 /* S102 R18-B3: the binder proved the output alpha EXACTLY 1 (arena_fragments.h): the plain alpha blend may be switched off */ };

struct DrawOp {
  uint32_t state;         // Frame::states
  uint32_t dynamic;       // Frame::dynamics
  uint32_t textures;      // Frame::texture_sets
  uint32_t vs_constants;  // byte offset into Frame::constants
  uint32_t fs_constants;
  BufferRange vertex[2];
  BufferRange index;      // kNoBuffer: not indexed
  uint32_t count;         // indices or vertices
  uint32_t first;         // first index or vertex
  int32_t base_vertex;
  uint32_t instances;
  uint8_t index_32bit;
  uint8_t flags;           // kDrawOpPrevConstants
  uint16_t blend_count;   // how many Frame::blend_ranges this draw's vertex constants have
  uint32_t blend_first;
  uint32_t blend_key_low; // stable identity of this draw between game ticks; 0 = none (never interpolated)
  uint32_t blend_key_high;
};

// A run of vertex constants (matrices, bones) that may be interpolated between game ticks. Relative to the draw's vs_constants.
struct BlendRange {
  uint32_t offset;
  uint32_t bytes;
};

enum class CopyFilter : uint8_t { Nearest, Linear };

struct CopyOp {
  ImageRef source;
  ImageRef dest;
  Rect source_rect;
  Rect dest_rect;
  CopyFilter filter = CopyFilter::Nearest;
};

struct PresentOp {
  ImageRef source;
  Rect source_rect;
};

// S102 lane L1b (docs/renderer/PRELIGHT_DIRECT.md section 9): ONE compute dispatch that writes the game's light cubes (64x64, six layers, RGBA8, each given kUsageStorage) from their baked-lights constants:
// no strip, no render pass, no copy. `constants` is a byte offset into Frame::constants of `cube_count` blocks of kLightCubeVec4 vec4 (the PreLightK layout of fx_prelight.glsl; [15] holds the
// texel -> grid mapping). The readers sample the cubes as they always did.
constexpr int kMaxLightCubes = 5;
constexpr uint32_t kLightCubeVec4 = 18;
struct DispatchOp {
  ImageId cube[kMaxLightCubes];
  uint32_t cube_count = 0;
  uint32_t constants = 0;
};

template <class T>
struct Span {
  const T* data = nullptr;
  uint32_t count = 0;
  const T& operator[](uint32_t i) const { return data[i]; }
};

// Settings parity S10b: what DLSS needs from a frame (the binder fills it at the present; Frame::dlss.valid false = none).
struct DlssInputs {
  bool valid = false;
  bool reset = false;              // no history to use (first frame, a camera cut)
  ImageId color = kNoImage;        // the picture before the HUD (render size)
  ImageId depth = kNoImage;        // the scene's depth (render size)
  ImageId object_motion = kNoImage;  // the wrestlers' own motion (yVelocity.frag: RGBA32F, render size); kNoImage = the camera's only
  float view_proj[16] = {};        // the game's ViewProj this frame, row k = register c56 + k (clip = M * world)
  float prev_view_proj[16] = {};   // the last frame's
  float jitter[2] = {};            // render pixels (dlss_jitter.h)
};

// S103 PF2: the most Renders one recorded frame may get (the host's tween: the half-way picture, then the tick's own). A direct-path (host-visible vertex copy) frame is recorded at Render serial s and may be rendered at
// serials s .. s + kRendersPerRecord - 1 only: the front end frees a copy on that promise (FrontEnd::PosedDirectWrite) and Render raises a Problem when it is broken (Frame::direct_record).
constexpr uint32_t kRendersPerRecord = 2;

struct Frame {
  uint64_t tick = 0;
  uint64_t direct_record = 0;   // S103 PF2: 0 = no draw reads a host-visible vertex copy; else the Render serial the front end saw at Record, + 1
  Span<Op> ops;
  Span<TargetsOp> targets;
  Span<ClearOp> clears;
  Span<DrawOp> draws;
  Span<CopyOp> copies;
  Span<PresentOp> presents;
  Span<DispatchOp> dispatches;   // S102 L1b
  Span<DrawState> states;
  Span<DynamicState> dynamics;
  Span<SamplerDesc> samplers;
  Span<TextureSet> texture_sets;
  Span<BlendRange> blend_ranges;
  Span<uint8_t> constants;
  Span<uint8_t> geometry;
  // S100: the first part of a frame split at a copy the game reads back with the CPU; the rest of the frame comes next, so every image
  // (and stencil) is kept at the end of this part.
  bool keep_contents = false;
  DlssInputs dlss;  // settings parity S10b
  uint32_t out_cap_h = 0;      // S102 G2 (b): this frame's output cap (0 = none), decided at record time (live.cpp, the S0-4 table); the render thread reads only this
  bool out_bilinear = false;   // S102 G2 (c): this frame's present is plain bilinear even when SGSR is on
  bool present_fetch = false;   // S102 R24-A: this frame's wwe2k14_rdr_present_fetch (output_switches.h), decided at record time; the render thread reads only this
  bool arena_overdraw = false;     // S102 C4: this frame's wwe2k14_rdr_arena_overdraw (colour_cuts.h g_arena_overdraw): the prepass also takes draws with a texture-alpha proof
  bool half_transparents = false;  // S102 C4: this frame's wwe2k14_rdr_low_half_transparents (half_trans.h), only under LOW
  uint32_t arena_fragments = 0;  // S102 R18-B3: this frame's wwe2k14_rdr_arena_fragments bit mask (arena_fragments.h Bits), decided at record time; the render thread reads only this
  uint32_t colour_cuts = 0;    // S102 G10: this frame's colour-pass cuts (colour_cuts.h Bits), decided at record time (live.cpp, the S0-4 table); the render thread reads only this
  Span<uint8_t> prev_constants;  // S10b: the previous tick's constants, laid out as `constants` (velocity draws); empty = none
  Span<uint8_t> draw_groups;     // S102 R24-F: one group id (group_stamps.h GroupId) per draw, parallel to `draws`; empty unless wwe2k14_rdr_gpu_group_stamps is on (the binder writes it)
  uint8_t group_min_run = 0;     // S102 R24-F: this frame's wwe2k14_rdr_gpu_group_stamps (0 = off), decided at record time; the render thread reads only this
  uint64_t upload_seal = 0;      // S102 R21-B: the upload batches this frame applies (Renderer::SealUploads); 0 = every upload made so far (the one-thread path)
};

struct ProgramDesc {
  const char* name = nullptr;
  const uint32_t* vertex_spirv = nullptr;
  size_t vertex_bytes = 0;
  const uint32_t* fragment_spirv = nullptr;
  size_t fragment_bytes = 0;
};

constexpr int kMaxVertexAttributes = 16;

struct VertexAttribute {
  uint8_t location, binding;
  uint16_t format;  // VkFormat
  uint32_t offset;
};

struct VertexLayoutDesc {
  VertexAttribute attributes[kMaxVertexAttributes];
  uint32_t attribute_count = 0;
  uint32_t strides[2] = {0, 0};
  uint8_t per_instance[2] = {0, 0};
};

constexpr uint32_t kConstantAlign = 256;  // the largest minUniformBufferOffsetAlignment the spec allows

}  // namespace rdr
