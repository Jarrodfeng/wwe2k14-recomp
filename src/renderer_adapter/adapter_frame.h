// The adapter's description of one whole frame: flat POD tables in host terms, the only guest things left being the game's shader
// hashes and its raw constant registers (the binder packs constants per shader, using each shader's mapping).
// This file is the on-disk layout of a capture frame too (docs\renderer\CAPTURE_FORMAT.md). It has no game or SDK includes.
#pragma once
#include <cstddef>
#include <cstdint>

#include "../renderer/api.h"

namespace rdr_adapter {

inline constexpr uint32_t kFrameMagic = 0x52464352;    // "RCFR"
inline constexpr uint32_t kImagesMagic = 0x4D494352;   // "RCIM"
inline constexpr uint32_t kTextureMagic = 0x58544352;  // "RCTX"
inline constexpr uint32_t kFormatVersion = 1;

// ---- images ---------------------------------------------------------------------------------------------------------------------

enum ImageSource : uint8_t { kSourceSurface = 0, kSourceTexture = 1 };
enum ImageFlags : uint8_t {
  kImageTiled = 1,
  kImageCopyDest = 2,     // written by a Copy: storage follows the source surface (C4), never read from guest memory
  kImageMoviePlane = 4,   // linear k_8: re-read each frame it is bound
  kImageCopySwap = 8,     // the game's red / blue swap on copies into this image (C8); already folded into each slot's swizzle
  kImageBorder = 16,      // the fetch constant's border_size bit is set (size carries +3, not +1)
};

// One line per image of the whole capture (images.bin): both registries (surfaces and textures) share one id space. Id 0 = nothing.
struct ImageRecord {
  uint32_t id = 0;
  uint16_t width = 0, height = 0, depth = 1;
  uint8_t mips = 1, layers = 1;
  uint8_t format = 0;       // rdr::Format of the storage (a copy destination keeps the source's: k_8 from 8888 is RGBA8, read through RRRR)
  uint8_t kind = 0;         // rdr::ImageKind
  uint8_t usage = 0;        // rdr::ImageUsage bits
  uint8_t source = 0;       // ImageSource
  uint8_t flags = 0;        // ImageFlags
  uint8_t game_format = 0;  // for reports only: the fetch constant format, or the colour surface format (depth surfaces: 0xD0 | fixed-vs-float)
  uint8_t role = 0;         // S101 names (names.h SurfaceRole): the role its creator declares; 0 unnamed (older captures: always 0)
  uint8_t role_flags = 0;   // S101 names (names.h SurfaceRoleFlags): scales / lasting / declared / guessed
  uint8_t pad = 0;
};

struct ImageTableHeader {
  uint32_t magic = kImagesMagic;
  uint32_t version = kFormatVersion;
  uint32_t count = 0;
  uint32_t pad = 0;
};

// ---- the frame's tables -----------------------------------------------------------------------------------------------------------

enum class OpKind : uint32_t { SetTargets, Clear, Draw, Copy, Present };

struct Op {
  OpKind kind;
  uint32_t index;  // into the table for that kind
};

// Pipeline-level state. rdr::DrawState holds Vulkan's numbers already; program and vertex_layout stay 0 here (the binder fills them).
struct DrawFixed {
  rdr::DrawState state;
  uint8_t alpha_test = 0;     // A1: compare the shader's raw colour-0 alpha
  uint8_t alpha_func = 7;     // VkCompareOp (the Xenos bits are the same numbers, A2)
  uint8_t alpha_to_coverage = 0;
  uint8_t clip_off = 0;       // C14: pixel-space draw, depth clamp on (C15)
  int8_t exp_bias[4] = {};    // O10: RB_COLOR_INFO color_exp_bias per target
  float alpha_ref = 0;        // A4: the register float, unscaled
  uint32_t pad = 0;
};

enum DynamicFlags : uint32_t {
  kDynClipOff = 1,        // C14: rebuild ndc_scale / ndc_offset for another resolution scale from guest_scale / guest_offset
  kDynEmptyViewport = 2,  // C9: covers nothing
  kDynHalfPixel = 4,      // C6 applied: +0.5 guest pixels are inside guest_offset
};

// Viewport and scissor at 1x, in guest pixels. Clip-on draws: multiply viewport and scissor by the whole scale N and leave ndc_* alone
// (S1, S4). Clip-off draws: E = min(8192, maxViewport / N); host viewport (0, 0, E N, E N); ndc_scale = guest_scale * 2 / E;
// ndc_offset = (guest_offset - E / 2) * 2 / E (C14, S10); z scale and offset as given.
// In the shader: pos.xyz = xyz * ndc_scale + ndc_offset * w, pos.w = w (C2), after C1's decode (vtx_fmt bits).
struct DrawDynamic {
  rdr::DynamicState host;
  float ndc_scale[3] = {1, 1, 1};
  float ndc_offset[3] = {0, 0, 0};
  float guest_scale[3] = {1, 1, 1};    // viewport scale registers with C4's per-axis enables applied
  float guest_offset[3] = {0, 0, 0};   // the offsets with C4, C6 and C7 applied
  uint32_t flags = 0;                  // DynamicFlags
  uint8_t vtx_xy_fmt = 0, vtx_z_fmt = 0, vtx_w0_fmt = 0, pad = 0;  // C1
};

enum Target16Read : uint8_t { kTarget16None = 0, kTarget16Unsigned = 1, kTarget16Signed = 2, kTarget16Mixed = 3 };

// Parallel to the frame's copies: what rdr::CopyOp has no field for.
struct CopyExtra {
  uint8_t target16 = 0;       // the source is a k_16_16 / k_16_16_16_16 target (RGBA16F storage)
  uint8_t dest_sign = 0;      // the destination texture's own fetch sign, channel x: the sign the library's copy number format follows (C24, C28)
  uint8_t dest_format = 0;    // the destination's fetch format code
  uint8_t swap = 0;           // the red / blue swap of this copy (C8); folded into each reading slot's swizzle
};

// A texture a shader of this draw reads.
struct TextureSlot {
  uint32_t image = 0;        // 0: a fetch of nothing, all channels read 0 (T14)
  uint32_t sampler = 0;      // into Frame samplers
  uint8_t slot = 0;          // the game's fetch slot (the shader's index, conventions: textures[slot])
  uint8_t view_format = 0;   // rdr::Format of the view: a same-size reinterpretation, signed vs unsigned. 0 for every read of a 16-bit target's copy (RGBA16F storage)
  uint8_t swizzle[4] = {};   // VkComponentSwizzle: fetch swizzle composed with the format's host swizzle (T15) and any copy swap (C8)
  uint8_t sign[4] = {};      // per channel, the fetch constant's sign: 0 unsigned, 1 signed, 2 unsigned biased, 3 gamma (T16-T19)
  uint8_t dimension = 1;     // 0 1D, 1 2D or stacked, 2 3D, 3 cube
  uint8_t min_level = 0;     // L12
  uint8_t max_level = 0;
  uint8_t target16_read = 0; // Target16Read: this slot reads a copy of a k_16_16 / k_16_16_16_16 target, and how the game's fetch takes it (T7, T8, T16, C28)
  float fetch_lod_bias = 0;  // word 4 bits 12-21 signed / 32: goes into the shader's LOD, not the sampler (S7)
  int32_t t21_exp = 0;       // T21 as today's picture has it: word 4 bits 13-18 signed; result x 2^this. 0 for almost everything
  int32_t word3_exp = 0;     // the header's true exp_adjust (word 3 bits 13-18): today nothing applies it, kept for the census
};

// One vertex fetch of the shader, in program order (conventions: `fetch` = this index).
enum ElementFlags : uint8_t { kElementSigned = 1, kElementInteger = 2 };
struct VertexElement {
  uint16_t vk_format = 0;    // VkFormat. Missing components read 0 (V13), including w: the shader's job
  uint16_t host_offset = 0;  // bytes, in the converted vertex
  uint8_t source_format = 0; // Xenos vertex format code
  uint8_t components = 0;
  uint8_t flags = 0;         // ElementFlags
  uint8_t mini = 0;          // a mini fetch in the microcode
  int32_t guest_offset = 0;  // dwords, for reference
};

struct VertexLayout {
  uint32_t element_first = 0;
  uint32_t element_count = 0;
  uint32_t host_stride = 0;       // bytes
  uint32_t guest_stride = 0;      // dwords, for reference
};

enum DrawFlags : uint8_t {
  kDrawInline = 1,          // from BeginVertices / EndVertices
  kDrawQuadsToList = 2,     // P6: quads became lists with the 0,1,2 / 0,2,3 diagonal
  kDrawRectsExpanded = 4,   // P3 applied in input space (fourth vertex extrapolated from the fetched attributes)
  kDrawNoGeometry = 8,      // not capturing, or the vertices could not be described (census names it)
  kDrawThin = 16,           // S101 lane 122 (D1 thin draws, thin_draw.h): the reader did not build this draw's geometry and shader facts; the binder draws the key's template. Older captures never carry it
};

struct Draw {
  uint64_t vs_hash = 0;          // XXH3 of the vertex shader's ORIGINAL microcode, the key of db\shaders\inventory.json
  uint64_t ps_hash = 0;          // 0: no pixel shader (depth-only draw, D12)
  uint64_t vs_patched_hash = 0;  // XXH3 of the patched copy, the shader_list.txt key; for cross-checks
  uint32_t fixed = 0;            // into fixeds
  uint32_t dynamic = 0;          // into dynamics
  uint32_t texture_first = 0, texture_count = 0;  // into texture_slots
  uint32_t vs_bank = 0, ps_bank = 0;              // into constant_banks: 256 vec4 each, already little-endian
  uint32_t bool_loop = 0;        // into bool_loops
  uint32_t layout = 0xFFFFFFFFu; // into layouts (0xFFFFFFFF: no vertex fetch)
  uint64_t vertex_offset = 0;    // into geometry.bin
  uint64_t index_offset = 0;
  uint32_t vertex_bytes = 0;
  uint32_t index_bytes = 0;      // 0: not indexed, draw vertex_count vertices
  uint32_t vertex_count = 0;
  uint32_t index_count = 0;
  uint8_t index_32bit = 0;
  uint8_t source_prim = 0;       // the guest's primitive code (P1), for reports
  uint8_t flags = 0;             // DrawFlags
  uint8_t pad[5] = {};
};

// Bool and loop constant registers (0x4900..): 8 bool words, 32 loop words, little-endian.
struct BoolLoop {
  uint32_t words[40] = {};
};

struct ConstantBank {
  uint32_t dwords[1024] = {};  // 256 vec4, little-endian floats, literal constants overlaid
};

// Constant banks as changes (S100 speed: two full 4 KB banks a draw cost the game's render thread ~2 us a draw). When a frame carries
// kSecBankDefs, a draw's vs_bank / ps_bank index the defs: def i is constant_banks[full] (base == kBankNoBase) or def `base` (an earlier one)
// with the patches [patch_first, patch_first + patch_count) applied. Readers expand the defs into full banks (CaptureReader::ParseFrame).
inline constexpr uint32_t kBankNoBase = 0xFFFFFFFFu;
struct BankDef {
  uint32_t base = kBankNoBase;
  uint32_t full = 0;
  uint32_t patch_first = 0;
  uint32_t patch_count = 0;
};
struct BankPatch {
  uint32_t block = 0;          // 64-byte block 0-63: vec4 registers 4 * block .. 4 * block + 3 (dwords 16 * block ..)
  uint32_t dwords[16] = {};
};

// "Image X takes this content before this frame's first op". Offset and size into textures.bin.
struct ContentRef {
  uint32_t image = 0;
  uint32_t pad = 0;
  uint64_t offset = 0;
  uint64_t size = 0;
};

// ---- frame file ---------------------------------------------------------------------------------------------------------------------

enum SectionId : uint32_t {
  kSecOps = 1,
  kSecTargets,
  kSecClears,
  kSecDraws,
  kSecCopies,
  kSecPresents,
  kSecFixeds,
  kSecDynamics,
  kSecSamplers,
  kSecTextureSlots,
  kSecLayouts,
  kSecElements,
  kSecConstantBanks,
  kSecBoolLoops,
  kSecContents,
  kSecCopyExtras,
  kSecBankDefs,
  kSecBankPatches,
  kSecCount
};

// FrameFileHeader::flags.
enum FrameFlags : uint32_t {
  // Vertex elements follow the ORIGINAL microcode's fetch order (S100, reader matched the patched copy's fetches by destination register).
  // Frames without it list them in the patched copy's order, which differs for rigid props and shadows (binder.h applies the one known fix-up).
  kFrameFetchOrderOriginal = 1,
  // 16-bit component pairs (k_16_16[_FLOAT], k_16_16_16_16[_FLOAT]) converted with the high half first (S100: uvs were swapped on every
  // model). Frames without it get the swap in the binder.
  kFrameSixteenPairsHighFirst = 2,
  // Settings parity S10b (DLSS): the scene's draws carry a jitter; its phase (dlss_jitter.h, 0-255) is in bits 24-31, so the binder tells
  // DLSS the same value the reader moved them by.
  kFrameDlssJitter = 4,
  // S101 lane 11: the first part of a frame split at a copy the game reads back (LiveSink::DrawAndRead): its contents must survive the draw
  // (rplay renders it with keep_contents, as the live renderer does). Set only by the case ring, on its own copy.
  kFramePart = 8,
  // S101 lane 59 (review): the stage view's "a `!scene play` is playing", stamped on the guest thread when the frame is captured (reader.h FrameBytes), so the picture of THAT frame
  // is chosen by that frame's own state, not by whatever the director says when the render thread gets to it.
  kFrameStagePlaying = 16,
  // S101 lane 122 (review 5): this frame holds thin draws (Draw::flags kDrawThin): the reader built no geometry for them, so a capture / case zip made from it cannot show the stream's arena. The
  // binder (and rplay / fview) replays them from the templates it learns from the capture's own full frames; a draw met before its key is learned is "not learned", not a hole. Set with names version 3.
  kFrameHasThin = 32,
};

struct FrameFileHeader {
  uint32_t magic = kFrameMagic;
  uint32_t version = kFormatVersion;
  uint32_t capture_frame = 0;      // 0, 1, 2 ... in this capture
  uint32_t section_count = 0;
  uint64_t guest_frame = 0;        // the game's frame counter when it was presented
  uint32_t present_interval = 0;   // device render state 94: 1 or 2 (30 fps entrances)
  uint32_t flags = 0;
};

struct SectionEntry {
  uint32_t id = 0;
  uint32_t element_size = 0;
  uint64_t count = 0;
  uint64_t offset = 0;  // from the start of the file
};

// One texture content blob in textures.bin: this header, `subresource_count` Subresource records, then the data.
struct TextureBlobHeader {
  uint32_t magic = kTextureMagic;
  uint32_t image = 0;
  uint32_t subresource_count = 0;
  uint32_t format = 0;  // rdr::Format
  uint64_t data_bytes = 0;
};

struct Subresource {
  uint32_t mip = 0, layer = 0;
  uint32_t width = 0, height = 0;  // texels
  uint32_t row_bytes = 0;          // tightly packed block row
  uint32_t rows = 0;               // block rows
  uint64_t offset = 0;             // from the start of the data
};

}  // namespace rdr_adapter
