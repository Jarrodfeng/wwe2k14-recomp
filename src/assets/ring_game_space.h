// The ring's pieces from the files, in the GAME's space, written in the stream draw's host vertex layout (Session 101, lane 70 = RO; docs\renderer\RING_PROMOTION_MAP.md sections 1.3 / 3,
// corrected by docs\systems\ring.md Part B). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Reads the player's own bgNN.pac at run time; nothing is kept on disk.
//
// The ring is one model object per piece (post 951, turnbuckle 952, canvas 953, proxy 956, high rope 960, cover 964, skirts 973 / 974, low rope 980, low post 981, low cover 982) drawn many times
// a frame with a different palette each time (sub_825CCFE0). This file knows only the FILE side: the pac entries' mesh records as they are stored (jboy_parts: no conversion, no Y mirror),
// their effect names and the textures their materials name (the same loader the viewer uses, only for the textures), and the vertex / index writer that produces the stream draw's bytes.
//
// The vertex writer (WriteVertices) is a pure function of the file's four streams and the layout the stream draw names (wrestler_geom::Classify names each element's role), with the rules
// measured by tools\ring\ring_census.py on every vertex of every ring draw of three captures (docs\systems\ring.md B2.3):
//   position  bit-equal file x y z, w = 1 (no scale)          fill word (rigid 1-pair meshes) (0, 0, 0, 255)         weights (2-pair rope) (w0, 0, 0)
//   bone ids  (slot 0, slot 1 or 255, 255, 255)               normal    the game's 10:11:11 pack, expanded (wrestler_geom::PackVec3)
//   colour    the file's A R G B word as stream bytes R G B A  uv       halves truncated toward zero of (|u|, v): the stream holds |u| (the SDK's vpkd3d128 codegen drops u's sign)
// The single-pair words come from prop_host_vertex.h (lane 49's SinglePairWords: shared with the arena's slots 390-397); its planted faults (colour_order, uv_halves_swapped, bone_zero,
// weight_word) act here too. The index buffer is the stream's: the file's strip groups joined by one 0xFFFF restart.
//
// Threads: Load on a worker (it takes the loader's process-wide mutex); everything else is plain data.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "asset_model.h"
#include "wrestler_geom.h"   // (first: it declares the zlib decoder pac.h needs)
#include "jboy_parts.h"
#include "prop_host_vertex.h"

namespace assets::ring_gs {

// The ring's pac entries (docs\systems\ring.md B1). 956 is the rope's plane-shadow proxy: loaded so the join sees every candidate, never claimed (its pass is not the colour pass).
inline constexpr int kEntries[] = {951, 952, 953, 956, 960, 964, 973, 974, 980, 981, 982};
inline bool IsRopeEntry(int entry) { return entry == 960 || entry == 980; }   // the rope colour (ar_rop01 / _red / _blue) is chosen per instance at run time (ring.md B2.3)

// Writer faults (the unit test's twins: each must fail its exact check). Zero = none.
struct WriteFaults {
  bool colour_order = false;       // R: the colour bytes in the file's A R G B order (Session 100's vertex-colour swizzle class)
  bool uv_halves_swapped = false;  // R: the 16-bit uv halves exchanged (Session 100's "uvs swapped on every model")
  bool uv_sign = false;            // R: the u sign rule written the WRONG way round for the stream at hand (signed where the stream holds |u|, |u| where it holds signed u): the SDK's vpkd3d128 defect, patch 109
  bool bone_zero = false;          // bone id 0 for every vertex
  bool weight_word = false;        // the fill / weight word (255, 0, 0, 0) instead of (0, 0, 0, 255)
};

struct RingMesh {
  int entry = -1;                  // the pac key (951 ..)
  int part = -1;                   // the mesh record's index in its JBOY = the game's DrawOrigin::part
  std::string effect;              // the material class as stored ("yProp_BumpMap", "yProp_DefaultLM", ...)
  std::shared_ptr<const Bytes> blob;   // the decoded entry (the Part's pointers point into it)
  jboy_parts::Part data;
  uint32_t vertices = 0, pairs = 0;    // pairs: skin pairs stored a vertex (S1 stride / 8)
  int first_strict_neg_u = -1;     // S101 lane 220 follow-up: the first vertex whose file u is STRICTLY below zero (-0.0 excluded): a clear stream sign bit proves |u| only there
  int first_neg_u = -1;            // the first vertex whose file u is negative (-1: none): only such meshes depend on the stream's u-sign rule
  int diffuse = -1, normal_map = -1, specular = -1;   // indices into RingArena::textures; -1 = the mesh names none (or the directories do not hold it)
  // The index count the game's draw has: the strip groups' sum, one more per extra group (the restart between them).
  uint32_t StreamIndexCount() const { return data.index_count + (data.index_groups > 1 ? data.index_groups - 1 : 0); }
};

struct RingArena {
  int arena = -1;
  std::vector<RingMesh> meshes;
  std::vector<Texture> textures;   // only what the ring meshes name
  size_t Bytes() const {
    size_t n = 0;
    for (const RingMesh& m : meshes) n += m.blob ? m.blob->size() : 0;
    for (const Texture& t : textures) n += t.data.size();
    return n;
  }
  const RingMesh* Find(int entry, int part) const {
    for (const RingMesh& m : meshes)
      if (m.entry == entry && m.part == part) return &m;
    return nullptr;
  }
};

bool Init(const std::string& game_path);
// Loads arena `arena`'s ring entries. False + err when the pac cannot be read or holds no ring entry (the 8 ringless arenas: bg66-68, 76-80).
bool Load(int arena, RingArena& out, std::string& err);

// ---- the writer (header only: the unit test and the hook share it) ---------------------------------------------------------------------------------------------------

// The stream's index buffer for a mesh: all strip groups in order, joined by one 0xFFFF restart.
inline std::vector<uint16_t> BuildIndices(const RingMesh& m) {
  std::vector<uint16_t> r;
  bool first = true;
  for (const jboy_parts::Part::Group& g : m.data.groups) {
    if (!first) r.push_back(0xFFFF);
    first = false;
    for (uint32_t i = 0; i < g.count; ++i) r.push_back(Be16(g.data + 2ull * i));
  }
  return r;
}

// The stream's u-sign rule from two halves of its u (S101 lane 220 follow-up): 2 signed, 1 |u|, 0 unknown. A SET sign bit at the first sign-bit vertex proves signed (a -0.0 vertex whose bit
// the game cleared must never teach |u|); a clear bit teaches |u| only at a STRICTLY negative vertex (`have_strict`).
inline int LearnUvMode(uint16_t half_at_first_sign_bit, bool have_strict, uint16_t half_at_strict) {
  if (half_at_first_sign_bit & 0x8000u) return 2;
  if (!have_strict) return 0;
  return (half_at_strict & 0x8000u) ? 2 : 1;
}

// The whole vertex buffer in the classified layout (`out` = m.vertices * roles.host_stride bytes). False: the mesh does not fit the single-pair / two-pair rules (a slot of 255 or more, a
// weight that is not 1.0 on a one-pair mesh): nothing is written, the draw stays the stream's.
// `signed_u`: the stream holds the file's signed u (post SDK patch 109) rather than |u| (captures before it). The caller learns which from the stream's own bytes (RingCore): never assumed. The planted
// uv_sign fault flips it.
inline bool WriteVertices(const RingMesh& m, const wrestler_geom::Roles& roles, const WriteFaults& faults, bool signed_u, std::vector<uint8_t>& out) {
  using wrestler_geom::Role;
  const jboy_parts::Part& p = m.data;
  const uint32_t N = p.vertices, stride = roles.host_stride, pairs = p.stride[1] / 8;
  out.assign(size_t(N) * stride, 0);
  prop_host::Faults pf;
  pf.colour_order = faults.colour_order, pf.uv_halves_swapped = faults.uv_halves_swapped, pf.bone_zero = faults.bone_zero, pf.weight_word = faults.weight_word;
  const prop_host::ColourOrder order{};   // identity on our R G B A
  const prop_host::ColourOrder use = faults.colour_order ? prop_host::WrongOrder() : order;
  for (uint32_t v = 0; v < N; ++v) {
    uint8_t* dst = out.data() + size_t(v) * stride;
    const uint8_t* s0 = p.s[0] + 28ull * v;
    const uint8_t* s1 = p.s[1] + size_t(p.stride[1]) * v;
    const uint8_t* s2 = p.s[2] + size_t(p.stride[2]) * v;
    uint8_t slot_bytes[4] = {255, 255, 255, 255};
    float pair_w[4] = {0, 0, 0, 0};
    for (uint32_t k = 0; k < 4 && k < pairs; ++k) {
      uint32_t slot;
      std::memcpy(&slot, s1 + 8 * k, 4);
      slot_bytes[k] = uint8_t(slot > 255 ? 255 : slot);
      pair_w[k] = BeF(s1 + 8 * k + 4);
    }
    prop_host::SkinWords words{};
    const bool single = pairs == 1;
    if (single && !prop_host::SinglePairWords(1, slot_bytes, pair_w, pf, words)) return false;
    for (const wrestler_geom::Roles::R& r : roles.r) {
      uint8_t* d = dst + r.offset;
      switch (r.role) {
        case Role::Pos: {
          float pos[4] = {BeF(s0), BeF(s0 + 4), BeF(s0 + 8), 1.0f};
          std::memcpy(d, pos, 16);
          break;
        }
        case Role::Weights: {   // three floats: the first (pairs - 1) pairs' weights, 0 beyond (docs\formats\wrestler_host_vertex.md; ring.md B2.3: (w0, 0, 0) on the high rope)
          for (uint32_t k = 0; k < 3; ++k) {
            const float w = (k + 1 < pairs && slot_bytes[k] != 255) ? pair_w[k] : 0.0f;
            std::memcpy(d + 4 * k, &w, 4);
          }
          break;
        }
        case Role::Slots: {
          for (uint32_t k = 0; k < 4; ++k) d[k] = single ? words.ids[k] : (faults.bone_zero && k == 0 ? uint8_t(0) : slot_bytes[k]);
          break;
        }
        case Role::Fill0: {   // the rigid-part dummy (0, 0, 0, 255); the writer fault flips it
          const uint8_t w[4] = {0, 0, 0, 255}, wf[4] = {255, 0, 0, 0};
          std::memcpy(d, single ? words.weights : (faults.weight_word ? wf : w), 4);
          break;
        }
        case Role::Normal: {
          float n[3] = {BeF(s0 + 12), BeF(s0 + 16), BeF(s0 + 20)};
          wrestler_geom::PackVec3(n, d);
          break;
        }
        case Role::Tangent: {   // the ring has no S3; a layout that fetches one is not ours
          return false;
        }
        case Role::Colour: {
          const uint8_t rgba[4] = {s0[25], s0[26], s0[27], s0[24]};   // the file word is A R G B
          for (int k = 0; k < 4; ++k) d[k] = rgba[use.perm[k]];
          break;
        }
        case Role::Uv: {
          const float u = BeF(s2), vv = BeF(s2 + 4);
          const uint16_t hu = prop_host::FloatToHalfTruncate((signed_u != faults.uv_sign) ? u : std::fabs(u)), hv = prop_host::FloatToHalfTruncate(vv);
          const uint16_t h[2] = {faults.uv_halves_swapped ? hv : hu, faults.uv_halves_swapped ? hu : hv};
          std::memcpy(d, h, 4);
          break;
        }
      }
    }
  }
  return true;
}

}  // namespace assets::ring_gs
