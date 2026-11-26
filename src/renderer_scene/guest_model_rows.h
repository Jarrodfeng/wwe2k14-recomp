// GuestModel rows: the scene record's carrier for a model the game built at run time that no asset name can reach (Session 103, lanes CW-2 / CW-2b; docs\research\s103\PCW_caw_wrestler.md C2 / P2,
// PCW_adversary.md, CW2_review.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only; no game, SDK or asset includes.
//
// Hard rule 1: the bytes in these sections are geometry and pictures read from the running game's memory. They exist only in the process and in local scene records under out\ (never committed, never shipped).
// Four sections, all optional (a record without them is byte-identical to today's; ParseRecord reads old records):
//   62 kScGuestModelParts = GuestModelPart[] (layout 2): one row per readable part of each model read this frame, ALWAYS with its hashes (the identity), the game's own vertex flag word F
//      ([[m+360]+8p], the packer's switch: the vertex is decoded from F, never from the stride), the part's palette (slot -> skeleton node), its effect name and its sampler textures.
//   63 kScGuestModelBlobs = uint8_t[]: the bytes the rows point at (vtx_off / idx_off / texture rows' off, 8-byte aligned), present for a hash NEW to the record session and resent every kGmResendFrames (ODD:
//      a consumer that takes every second record still sees every resend) frames. A row whose offset is kGmNoBytes names bytes an earlier record carried: the in-process consumer asks the writer's byte store
//      (scene_live\guest_model_store.h) by hash, a file consumer waits for the resend, and a hash neither holds is a HOLE (counted, never a guess).
//   64 kScGuestModelTextures = GuestModelTexture[]: one row per distinct texture a part names (hash of the decoded mip-0 blocks, size, game format, where the bytes are).
//   71 kScGuestModelParams = GuestModelParam[] (S111 GUESTHAIR): one row per float parameter of a part that the material table takes (assets\material_floats.h; guest_model_float_rows.h), every record.
//   72 kScGuestModelMats = GuestModelMat[] (S111 EYECUBE; UNIFYMAT step 3's shape): one row per sampler the reader kept (EVERY one: no cap of this section's; the part row's tex[] keeps <= kGmMaxTex),
//      each with the part's technique / cull words, its parameter name and its own table index t; a part with no sampler gets one row of its words alone (sampler = kGmMatNoSampler). Joined to the
//      part row's tex[i] by the parameter NAME, never by place (guest_model_mat_rows.h NameOf).
//   73 kScGuestModelTexNames = GuestModelTexName[] (S111 EYECUBE): the model's texture name of each t a carried sampler names ([m+268]+16t: the name sub_82BD9A60 resolves). Both every record; both optional.
// Join: a row's `model` = the guest address of the model object = the engine object address kScEngineObjects holds for the actor that is the same object (actor:slot:N with no asset link).
// Vertex bytes: vertex_count * stride bytes, dwords in HOST order (the endian-2 swap already applied). Indices: n0 (+ n1) host u16, a triangle strip with 0xFFFF restarts; range 1 = [0, n0), range 2 = [n0, n0 + n1).
#pragma once
#include <cstdint>
#include <vector>

namespace rdr_scene {

inline constexpr uint32_t kScGuestModelParts = 62;   // 59-61 are the weapon lanes' (weapon_rows.h)
inline constexpr uint32_t kScGuestModelBlobs = 63;
inline constexpr uint32_t kScGuestModelTextures = 64;
inline constexpr uint32_t kScGuestModelParams = 71;     // S111 GUESTHAIR (68-70 are the snapshot sections, rows_snapshot.h)
inline constexpr uint32_t kScGuestModelMats = 72;       // S111 EYECUBE (guest_model_mat_rows.h)
inline constexpr uint32_t kScGuestModelTexNames = 73;   // S111 EYECUBE
inline constexpr uint32_t kGuestModelLayout = 2;     // 2: F, palette nodes, effect name, sampler textures (CW-2b); 1 (CW-2) is refused (never produced outside the lane's branch)
inline constexpr uint32_t kGmNoBytes = 0xFFFFFFFFu;
inline constexpr uint32_t kGmResendFrames = 61;      // odd (CW2_review bug 6)
inline constexpr uint32_t kGmMaxStride = 256, kGmMaxVertices = 1u << 20, kGmMaxIndices = 1u << 20;
inline constexpr uint32_t kGmMaxNodes = 20, kGmMaxTex = 6, kGmNameBytes = 16;   // the descriptor's palette holds <= 20 slots; a part keeps <= 6 sampler textures (more are counted, not carried)
inline constexpr uint32_t kGmMaxTexBytes = 4u << 20;                              // one texture's blocks (a 2048 x 1024 BC3 is 2 MB; a 1024 x 1024 k_8_8_8_8 is exactly 4 MB, accepted: every test is `>`)
enum GuestModelFlags : uint32_t {
  kGmN1 = 1,             // the part has a second draw range
  kGmExtraStreams = 2,   // the game binds extra 12-byte streams for it that this carrier does not hold: a HOLE, counted (the part is drawn without them; CW2_review item 8)
  kGmDiffuseT0 = 4,      // S105 A5c: the part's diffuse sampler names texture index 0 of the model's table: the game sets pixel bool 150 for it ([C] sub_82C04DF0 0x82C096F8-0x82C0971C), which picks the logo layer 7 over 4 in an FX_LOGO fragment
  kGmDrawWords = 8,      // S111 LAYERS: the reader read the part's draw words (src\assets\char_draw_layer.h): kGmCreated and the tag below are valid (the row layout is unchanged: these were zero bits)
  kGmCreated = 16,       // S111 LAYERS: the model's dim predicate [m+620] is 1 (a created superstar's model)
  kGmTagMask = 0xF00,    // S111 LAYERS: the part's tag [[m+368]+4p] (0..7) in bits 8..11
};
inline constexpr uint32_t kGmTagShift = 8;   // S111 LAYERS
enum GuestTexState : uint32_t { kGtMissing = 0, kGtBound = 1, kGtOverride = 2, kGtEngineDefault = 3 };   // a sampler's picture: not read / the model's own texture table / the override block (CEditEx composite textures, [L]) / CTX2: the sampler names a CUBE (fetch dimension 3): the engine supplies its default, no picture carried (hash 0)

struct GuestModelTex {   // 32 bytes: one sampler of the part
  char name[kGmNameBytes] = {};   // the param's name as stored ("sampDiffusePrm", "g_mNormalFxPrm", ...), NUL padded
  uint32_t key = 0;               // the texture key's first word (the registry handle; sub_82294F20 resolves it), 0 = none
  uint32_t state = kGtMissing;    // GuestTexState
  uint64_t hash = 0;              // content hash of the decoded blocks, 0 = missing
};
static_assert(sizeof(GuestModelTex) == 32, "GuestModelTex layout");

struct GuestModelPart {   // 344 bytes
  uint32_t version = kGuestModelLayout;
  uint32_t model = 0;        // guest address of the model object (the join key)
  uint32_t part = 0;
  uint32_t parts = 0;        // the model's part count
  uint64_t model_hash = 0;   // over every part's hashes (a changed model = a new hash)
  uint64_t vtx_hash = 0, idx_hash = 0;   // content hashes of the part's vertex / index bytes
  uint32_t vertex_count = 0, stride = 0;   // stride in bytes
  uint32_t index_count = 0;   // n0 + n1
  uint32_t n0 = 0, n1 = 0;
  uint32_t flags = 0;         // GuestModelFlags
  uint32_t vtx_off = kGmNoBytes, idx_off = kGmNoBytes;   // offsets into the blob section of THIS record, or kGmNoBytes
  uint32_t vflags = 0;        // the game's vertex flag word F ([[m+360]+8p]); the decode follows it
  uint32_t node_count = 0;    // the part's palette size ([desc+8], <= 20)
  uint16_t nodes[kGmMaxNodes] = {};   // slot k -> skeleton node = [desc+12+4k] - 1 (0xFFFF = the palette value was 0: no node)
  char effect[kGmNameBytes] = {};     // the material class / effect name ([desc+124]), NUL padded; empty = not read
  uint32_t tex_count = 0;
  uint32_t logo7_tex = 0;     // S105 A5c: the model's logo picture bound at fetch slot 7: 1 + the index of its row in GuestModelTables::textures, 0 = none (the row layout is unchanged: this was padding)
  GuestModelTex tex[kGmMaxTex] = {};
  uint32_t tex_overflow = 0;   // sampler params beyond kGmMaxTex (counted)
  uint32_t logo4_tex = 0;      // S105 A5c: the same for the logo picture at fetch slot 4 ("logo2")
};
static_assert(sizeof(GuestModelPart) == 344, "GuestModelPart layout: 16 + 24 + 32 + 8 + 40 + 16 + 8 + 6 * 32 + 8");

struct GuestModelTexture {   // 32 bytes: a picture some part names
  uint64_t hash = 0;           // content hash of the blocks (what GuestModelTex::hash names)
  uint32_t width = 0, height = 0;
  uint32_t format = 0;         // the game's format code (18 = BC1, 19 = BC2, 20 = BC3, 6 = k_8_8_8_8 read as linear BGRA8, CAWD)
  uint32_t nbytes = 0;         // blocks: ((width + 3) / 4) * ((height + 3) / 4) * (8 or 16); 8888: width * height * 4
  uint32_t off = kGmNoBytes;   // into the blob section of THIS record, or kGmNoBytes (then the writer's store has it)
  uint32_t pad = 0;
};
static_assert(sizeof(GuestModelTexture) == 32, "GuestModelTexture layout");

struct GuestModelParam {   // 40 bytes: one float parameter of a part (S111 GUESTHAIR), as the game holds it (the part's record: name, kind, value(s) at +20)
  uint32_t model = 0;             // the part row's model (the join: model + part)
  uint16_t part = 0;
  uint16_t kind = 0;              // 0x0A = a scalar (v[0]) / 0x0D = a vec4
  char name[kGmNameBytes] = {};   // NUL padded (lower case in memory); a 16-character name has no NUL
  float v[4] = {0, 0, 0, 0};
};
static_assert(sizeof(GuestModelParam) == 40, "GuestModelParam layout");

inline constexpr uint16_t kGmMatNoSampler = 0xFFFF;   // S111 EYECUBE review: the GuestModelMat row of a part with no sampler (its record words alone)
struct GuestModelMat {   // 36 bytes: ONE sampler of a part, with the part's record words (S111 EYECUBE; UNIFYMAT step 3). One row per sampler the reader kept, any number (UNIFY2 lifts the reader's cap
                         // with no change here); a part with none has one row with sampler = kGmMatNoSampler. The words repeat on every row of the part.
  uint32_t model = 0;               // the part row's model (the join: model + part)
  uint16_t part = 0;
  uint16_t sampler = kGmMatNoSampler;   // the sampler's place in the reader's list (PartRead::samplers): informational, the consumer joins by `param`
  uint32_t technique = 0;           // [desc+144] (the jboy record's +0x90; BuildModel puts it on the Material, S111 UNIFY3 guest_model_mat_rows.h WordsOf)
  uint32_t cull = 0;                // [desc+140] (the jboy record's +0x8C; the same)
  int32_t t = -1;                   // the sampler's texture table index ([param+20]); section 73 holds the model's name of it
  char param[kGmNameBytes] = {};    // the sampler's parameter name, NUL padded: the same bytes as the part row's tex[i].name (both are copies of the one SamplerRead::name)
};
static_assert(sizeof(GuestModelMat) == 36, "GuestModelMat layout: 4 + 2 + 2 + 4 + 4 + 4 + 16");

struct GuestModelTexName {   // 24 bytes: one entry of a model's texture-name table (S111 EYECUBE)
  uint32_t model = 0;
  uint32_t t = 0;
  char name[kGmNameBytes] = {};     // as the game holds it, NUL padded; a 16-character name has no NUL
};
static_assert(sizeof(GuestModelTexName) == 24, "GuestModelTexName layout");

// The record's tables (one member of SceneTables: the smallest addition). The size is part of SceneTables' (tools\swap\swap_once_test.cpp): the merged size is the session base + the sum of the new members' sizes.
struct GuestModelTables {
  std::vector<GuestModelPart> parts;
  std::vector<uint8_t> blobs;
  std::vector<GuestModelTexture> textures;
  std::vector<GuestModelParam> params;   // S111 GUESTHAIR (section 71)
  std::vector<GuestModelMat> mats;            // S111 EYECUBE (section 72)
  std::vector<GuestModelTexName> tex_names;   // S111 EYECUBE (section 73)
  uint32_t errors = 0;   // ParseRecord dropped these sections (unknown layout / element size / a bad row / an offset past the blob)
  bool Empty() const { return parts.empty() && blobs.empty() && textures.empty() && params.empty() && mats.empty() && tex_names.empty(); }
  void Clear() { parts.clear(), blobs.clear(), textures.clear(), params.clear(), mats.clear(), tex_names.clear(), errors = 0; }
  // S105 A5c: the row index of the picture with `hash` (-1 = not in this record).
  int IndexOfTexture(uint64_t hash) const {
    for (size_t i = 0; i < textures.size(); ++i)
      if (textures[i].hash == hash) return int(i);
    return -1;
  }
  const GuestModelTexture* FindTexture(uint64_t hash) const {
    for (const GuestModelTexture& t : textures)
      if (t.hash == hash) return &t;
    return nullptr;
  }
};
static_assert(sizeof(GuestModelTables) == 152, "GuestModelTables: 6 vectors (144) + errors (4) + padding; SceneTables' size in swap_once_test.cpp counts it (CW2_review lint: a new SceneTables member is static_assert-ed)");

inline uint32_t GuestTexBytes(uint32_t width, uint32_t height, uint32_t format) {
  if (format == 6) return width * height * 4;   // CAWD: k_8_8_8_8 (linear BGRA8 once read), 4 bytes a texel; the CAW model's 1024 x 1024 / 512 x 1024 / 256 x 512 pictures
  const uint32_t bpb = format == 18 ? 8u : 16u;
  return ((width + 3) / 4) * ((height + 3) / 4) * bpb;
}

// Why a row is refused (empty = fine). Used by ParseRecord and the unit.
inline const char* GuestModelRowProblem(const GuestModelPart& r, size_t blob_size) {
  if (r.version != kGuestModelLayout) return "unknown layout version";
  if (r.vertex_count == 0 || r.vertex_count > kGmMaxVertices) return "vertex count out of range";
  if (r.stride < 4 || r.stride > kGmMaxStride || (r.stride & 3)) return "stride out of range";
  if (r.index_count == 0 || r.index_count > kGmMaxIndices || r.index_count != uint64_t(r.n0) + r.n1) return "index ranges do not add up";
  if (r.n1 && !(r.flags & kGmN1)) return "second range without the flag";
  if (r.part >= r.parts || r.parts > 64) return "part number out of range";
  if (r.node_count > kGmMaxNodes) return "palette size out of range";
  if (r.tex_count > kGmMaxTex) return "texture count out of range";
  for (uint32_t i = 0; i < r.tex_count; ++i)
    if (r.tex[i].state > kGtEngineDefault) return "texture state out of range";
  const uint64_t vb = uint64_t(r.vertex_count) * r.stride, ib = uint64_t(r.index_count) * 2;
  if (r.vtx_off != kGmNoBytes && ((r.vtx_off & 7) || uint64_t(r.vtx_off) + vb > blob_size)) return "vertex bytes past the blob section";
  if (r.idx_off != kGmNoBytes && ((r.idx_off & 7) || uint64_t(r.idx_off) + ib > blob_size)) return "index bytes past the blob section";
  return "";
}
inline const char* GuestModelTextureProblem(const GuestModelTexture& t, size_t blob_size) {
  if (t.format != 18 && t.format != 19 && t.format != 20 && t.format != 6) return "texture format is not BC1 / BC2 / BC3 / 8888";
  if (!t.width || !t.height || t.width > 4096 || t.height > 4096) return "texture size out of range";
  if (t.nbytes != GuestTexBytes(t.width, t.height, t.format) || t.nbytes > kGmMaxTexBytes) return "texture byte count does not match its size";
  if (t.off != kGmNoBytes && ((t.off & 7) || uint64_t(t.off) + t.nbytes > blob_size)) return "texture bytes past the blob section";
  return "";
}
inline const char* GuestModelParamProblem(const GuestModelParam& p) {   // S111 GUESTHAIR
  if (p.kind != 0x0A && p.kind != 0x0D) return "parameter kind is not a scalar / vec4";
  if (p.part >= 64) return "parameter part out of range";
  return "";
}
inline const char* GuestModelMatProblem(const GuestModelMat& m) {   // S111 EYECUBE
  if (m.part >= 64) return "material part out of range";
  if (m.sampler != kGmMatNoSampler && m.t < 0) return "material sampler without a texture index";   // the reader keeps no sampler with t < 0
  return "";
}
inline const char* GuestModelTexNameProblem(const GuestModelTexName& n) {   // S111 EYECUBE
  if (n.t > 0xFFFFu) return "texture name index out of range";
  return "";
}

}  // namespace rdr_scene
