// The consumer of the GuestModel rows: bytes by hash -> an assets::Model the front end draws (Session 103, lanes CW-2 / CW-2b; docs\research\s103\PCW_adversary.md, PCW_caw_wrestler.md P2, CW2_review.md).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only; no game, SDK or GPU includes (the unit builds it).
//
// What it does: (1) Store keeps the vertex / index / texture bytes by content hash: what the records carried, and, for the in-process consumer, whatever the writer's process byte store holds under the hash
// (scene_live\guest_model_store.h: a record that names bytes it does not carry is still answered; a hash neither holds is a HOLE: counted, never a guess). The store is PER ModelCache (CacheState), absorbed at
// the top of every BuildFrame (a late or skipping consumer misses no record's bytes). (2) BuildModel(record, store, model address) makes the CPU model of one guest model: every part's vertices DECODED (and mirrored into viewer space, as every loaded model) from the
// game's own vertex flag word F (the packer sub_82BD4BE0's switch: scene_live\guest_model_reader.h LayoutOfFlags, the ONE declaration; never from the stride), its palette slots mapped to skeleton nodes through
// the part's own palette, its strip indices converted to triangles (0xFFFF restarts honoured, both ranges), one material per distinct (effect, pictures): the effect NAME as the game stores it ([desc+124], the
// jboy record's +0x7C read rule) and the sampler pictures by the jboy parameter names. rf_wrestlers draws a visible actor whose asset the record cannot name (asset pac 0) from it, taking the skeleton palette from
// the same record rows the named wrestlers use (actor bones).
// [U] (flagged for the live check): the override block's value layout and that CEditEx composite textures live in guest memory at all (the writer's census line `[rdr][caw] textures: ... from override O` answers it),
// the material parameters other than the pictures (spec level, tint ...: not read, the defaults stand).
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../assets/asset_model.h"
#include "../scene_live/guest_model_reader.h"
#include "../scene_live/guest_model_store.h"
#include "record_io.h"
#include "../scene_live/draw_camera_core.h"   // S103 CAWB: Cam().NoteBodyDraw (the draw-camera census carries last_body_draw)

namespace rdr_scene::guestmodel {

// Test only: 1 = the 10:11:11 fields are read x-high (the wrong end); 2 = partial weights are dropped (only a 4-pair vertex gets 1 - sum: the CW-2 fault); 3 = a part's second range is dropped (strip stats only: the
// reader's plant 5 is the live fault); 4 = the uv halves are exchanged (u low, v high: the CW-2 fault); 5 = the palette slot is used as the node (the CW-2 fault); 6 = the vertex is decoded from the STRIDE (CW-2's
// LayoutOfStride: F ignored); 7 = an unknown F bit is accepted (no refusal); 11 = the file-space -> viewer-space mirror of the built vertices is skipped (CAWH: the strands).
// S105 A5c: 14 = the logo pictures are not put on the material (the old black layers).
inline int g_draw_plant = 0;

inline float HalfToFloat(uint16_t h) {
  const uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u;
  float f;
  if (e == 0) f = std::ldexp(float(m), -24);
  else if (e == 31) f = m ? NAN : INFINITY;
  else f = std::ldexp(float(m | 1024u), int(e) - 25);
  return s ? -f : f;
}
// A signed field of `bits` bits, normalised (the game's float -> integer of value * max: 511 for 10 bits, 1023 for 11).
inline float SignedField(uint32_t v, int bits) {
  const int32_t sv = int32_t(v << (32 - bits)) >> (32 - bits);
  const float mx = float((1 << (bits - 1)) - 1);
  return std::max(-1.0f, float(sv) / mx);
}
// One 10:11:11 word (x:10 low, y:11, z:11 high [C: sub_82C14BC0]; g_draw_plant 1 reverses the field order).
inline void Unpack1011(uint32_t w, float out[3]) {
  if (g_draw_plant == 1) {
    out[0] = SignedField(w >> 22, 10), out[1] = SignedField(w >> 11, 11), out[2] = SignedField(w, 11);
    return;
  }
  out[0] = SignedField(w & 0x3FFu, 10), out[1] = SignedField((w >> 10) & 0x7FFu, 11), out[2] = SignedField((w >> 21) & 0x7FFu, 11);
}

struct DecodeStats { uint32_t bad_slots = 0, bad_nodes = 0, unskinned = 0, max_node = 0; };   // max_node = (the highest skeleton node a vertex uses) + 1, 0 = none (S103 CAWS)
// Vertex bytes (host-order dwords, `stride` bytes a vertex, declared by the game's flag word F) -> assets::Vertex. False for an F this decoder does not know or that disagrees with the stride.
// `nodes` = the part's palette (slot -> skeleton node, 0xFFFF none), `node_count` its size.
inline bool DecodeVertices(const uint8_t* bytes, uint32_t count, uint32_t stride, uint32_t F, const uint16_t* nodes, uint32_t node_count, std::vector<assets::Vertex>& out, DecodeStats& st) {
  using live::guest_model::SkinKind;
  live::guest_model::VLayout lay = live::guest_model::LayoutOfFlags(F, g_draw_plant != 7);
  if (g_draw_plant == 6) {   // the stride-only decode (CW-2's fault): the dword count alone picks the skin / tangent / colour fields
    lay = live::guest_model::VLayout{};
    lay.ok = true;
    const uint32_t dw = stride / 4;
    if (dw == 11 || dw == 12) lay.skin = SkinKind::Pairs4, lay.weights_at = 4, lay.slots_at = 7, lay.dwords = 8;
    else lay.skin = SkinKind::Rigid, lay.slots_at = 4, lay.dwords = 5;
    lay.normal_at = int(lay.dwords++);
    if (dw == 9 || dw == 12) lay.tangent_at = int(lay.dwords++);
    lay.colour_at = int(lay.dwords++);
    lay.uv_at = int(lay.dwords++), lay.uv_sets = 1;
  }
  if (!lay.ok || lay.Stride() != stride || stride > 256) return false;
  const uint32_t dw = stride / 4;
  out.assign(count, assets::Vertex{});
  for (uint32_t v = 0; v < count; ++v) {
    uint32_t d[64];
    std::memcpy(d, bytes + size_t(v) * stride, size_t(dw) * 4);
    assets::Vertex& o = out[v];
    float pos[4];
    std::memcpy(pos, d, 16);
    o.pos[0] = pos[0], o.pos[1] = pos[1], o.pos[2] = pos[2];
    if (lay.normal_at >= 0) {
      float n[3];
      Unpack1011(d[lay.normal_at], n);
      o.normal[0] = n[0], o.normal[1] = n[1], o.normal[2] = n[2];
    }
    if (lay.tangent_at >= 0) {
      float t[3];
      Unpack1011(d[lay.tangent_at], t);
      o.tangent[0] = t[0], o.tangent[1] = t[1], o.tangent[2] = t[2];
    }
    o.color = lay.colour_at >= 0 ? d[lay.colour_at] : 0xFFFFFFFFu;
    if (lay.uv_at >= 0) {   // the packer's half2: half(u) at +0, half(v) at +2 -> the host dword is (u << 16) | v (sub_82C148F8; CW2_review bug 2)
      const uint32_t uv = d[lay.uv_at];
      const uint16_t hu = g_draw_plant == 4 ? uint16_t(uv & 0xFFFFu) : uint16_t(uv >> 16), hv = g_draw_plant == 4 ? uint16_t(uv >> 16) : uint16_t(uv & 0xFFFFu);
      o.uv0[0] = HalfToFloat(hu), o.uv0[1] = HalfToFloat(hv);
    }
    o.uv1[0] = o.uv1[1] = 0.f;
    // skin: pairs of (slot, weight); a slot is the part's palette index, the node is palette[slot] - 1 (CW2_review bug 4)
    const auto node_of = [&](uint32_t slot, bool& bad) -> uint16_t {
      if (g_draw_plant == 5) return uint16_t(slot);
      if (slot >= node_count || nodes[slot] == 0xFFFF) return bad = true, ++st.bad_nodes, uint16_t(0);
      const uint16_t node = g_draw_plant == 10 ? uint16_t(nodes[slot] + 200u) : nodes[slot];   // plant 10 (S103 CAWS P3): every node 200 too high = past any palette; nodes_ge_palette must trip
      st.max_node = std::max<uint32_t>(st.max_node, uint32_t(node) + 1u);
      return node;
    };
    for (int k = 0; k < 4; ++k) o.bones[k] = 0, o.weights[k] = 0.f;
    if (lay.skin == SkinKind::None) {
      o.weights[0] = 1.f;
      ++st.unskinned;
    } else if (lay.skin == SkinKind::Rigid) {
      const uint32_t s0 = d[lay.slots_at] & 255u;
      bool bad = false;
      o.bones[0] = s0 == 255 ? uint16_t(0) : node_of(s0, bad), o.weights[0] = 1.f;
      if (s0 == 255) ++st.bad_slots;
    } else {
      const uint32_t slots = d[lay.slots_at];
      float w3[3];
      for (int k = 0; k < 3; ++k) std::memcpy(&w3[k], &d[lay.weights_at + k], 4);
      int n = 0;
      while (n < 4 && ((slots >> (8 * n)) & 255u) != 255u) ++n;   // the leading slots that are not 255 (the packer writes 255 into the absent ones)
      if (n == 0) {
        o.weights[0] = 1.f;
        ++st.bad_slots;
      } else {
        float sum = 0;
        for (int k = 0; k < n; ++k) {
          bool bad = false;
          o.bones[k] = node_of((slots >> (8 * k)) & 255u, bad);
          if (k < n - 1) {   // the stored weights of all but the LAST present bone; that one takes 1 - sum (the shader's rule, docs\formats\wrestler_host_vertex.md)
            o.weights[k] = k < 3 ? w3[k] : 0.f;
            sum += o.weights[k];
          }
        }
        if (g_draw_plant == 2 && n < 4) o.weights[n - 1] = 0.f;   // the CW-2 fault: partial weights are not completed
        else o.weights[n - 1] = std::max(0.f, 1.f - sum);
      }
    }
  }
  return true;
}

// The material slot a sampler parameter fills (the jboy read rule, src\assets\jboy.h 645-653; `skinned`: a skinned model's light-map slot is ignored there too).
enum class TexSlot : uint8_t { None = 0, Diffuse, Normal, Mask, Specular, Detail };
// CTF: the game's own names are lower case in memory ("ych_skin", log run 011), the file stores mixed case: compare ASCII case-insensitively (g_draw_plant 8 = the old case-sensitive compare).
inline char AsciiLower(char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; }
inline bool NameEq16(const char* a, const char* b) {
  for (int i = 0; i < 16; ++i) {
    const char x = g_draw_plant == 8 ? a[i] : AsciiLower(a[i]), y = g_draw_plant == 8 ? b[i] : AsciiLower(b[i]);
    if (x != y) return false;
    if (!x) return true;
  }
  return true;
}
// The pipeline's effect name for the game's [desc+124] name (case-insensitive; kFx = the 13 yCh_* pipelines of src\renderer\shaders\spv\fx_registry.h = char_effects.cpp kEffects, held equal by the unit's lint L10).
// The game's own literal list (sub_82BDA038) is NOT the same set: it has "ych_trans_tiling" (kAlias below), the misspellings "ych_grar" / "ych_grar_flex" (not mapped: unconfirmed that any data file carries them) and no "ych_cloth_trans" (we keep the pipeline). nullptr = not a character effect we have.
inline const char* PipelineEffect(const char* name) {
  static const char* const kFx[] = {"yCh_skin", "yCh_skin_flex", "yCh_gear", "yCh_gear_flex", "yCh_trans", "yCh_Reflect", "yCh_Ls", "yCh_cloth", "yCh_cloth_flex", "yCh_cloth_trans", "yCh_TransTiling", "yCh_Anisotropic", "yCh_Aniso_trans"};
  static const char* const kAlias[][2] = {{"ych_trans_tiling", "yCh_TransTiling"}};
  for (const char* fx : kFx)
    if (NameEq16(name, fx)) return fx;
  for (const auto& al : kAlias)
    if (NameEq16(name, al[0])) return al[1];
  return nullptr;
}
inline TexSlot SlotOfParam(const char* name) {
  const auto eq = [&](const char* n) { return NameEq16(name, n); };
  if (eq("g_mDiffusePrm") || eq("sampDiffusePrm")) return TexSlot::Diffuse;
  if (eq("g_mNormalFxPrm") || eq("sampNormalPrm")) return TexSlot::Normal;
  if (eq("g_mMaskPrm")) return TexSlot::Mask;
  if (eq("sampSpecularPrm")) return TexSlot::Specular;
  if (eq("g_mNoiseTilePrm")) return TexSlot::Detail;
  return TexSlot::None;
}

// Bytes by content hash, PER ModelCache. A hash the records carried is kept (shared pointers: a hash absorbed twice is one copy); one a record names without carrying is asked of the writer's process byte
// store at lookup time (the in-process consumer). Entries a record has not named for kKeepRecords absorbs are dropped.
class Store {
 public:
  using Bytes = live::guest_model::ByteStore::Bytes;
  static constexpr uint32_t kKeepRecords = 600;
  static constexpr size_t kCapBytes = size_t(256) << 20;   // S110 STOREABSENT: a cap on HISTORY: over it the least recently named entries go after each record (MakeRoom), never the bytes a record carries
  struct Counts { uint64_t absorbed_vtx = 0, absorbed_idx = 0, absorbed_tex = 0, dropped = 0, full = 0, records = 0, from_process = 0; };   // (full: carried bytes taken over the cap)
  Store() = default;
  explicit Store(size_t cap) : cap_(cap) {}   // (the unit's small stores)
  void Absorb(const SceneTables& t) {
    ++now_, ++counts_.records;
    const auto& g = t.guest_models;
    for (const GuestModelPart& r : g.parts) {
      Put(vtx_, r.vtx_hash, r.vtx_off, size_t(r.vertex_count) * r.stride, g.blobs, counts_.absorbed_vtx);
      Put(idx_, r.idx_hash, r.idx_off, size_t(r.index_count) * 2, g.blobs, counts_.absorbed_idx);
    }
    for (const GuestModelTexture& r : g.textures) Put(tex_, r.hash, r.off, r.nbytes, g.blobs, counts_.absorbed_tex);
    if ((now_ & 63) == 0) Sweep();
    if (bytes_ > cap_) MakeRoom();   // S110 STOREABSENT: after the whole record, so every entry it names has this record's age (last == now_) and none of them goes
  }
  Bytes Vtx(uint64_t h) { return Get(vtx_, h); }
  Bytes Idx(uint64_t h) { return Get(idx_, h); }
  Bytes Tex(uint64_t h) { return Get(tex_, h); }
  const Counts& counts() const { return counts_; }
  size_t bytes() const { return bytes_; }
  bool Empty() const { return vtx_.empty() && idx_.empty() && tex_.empty(); }
 private:
  struct E { Bytes b; uint64_t last = 0; };
  using Map = std::unordered_map<uint64_t, E>;
  void Put(Map& m, uint64_t h, uint32_t off, size_t n, const std::vector<uint8_t>& blobs, uint64_t& counter) {
    auto it = m.find(h);
    if (it != m.end()) { it->second.last = now_; return; }   // known: nothing to copy (the bytes are content-addressed)
    if (off == kGmNoBytes) return;                           // named, not carried: the process byte store answers at lookup (or it is a hole until the writer resends)
    if (uint64_t(off) + n > blobs.size()) return;
    if (bytes_ + n > cap_) { ++counts_.full; if (g_draw_plant == 17) return; }   // S110 STOREABSENT: over the cap the carried bytes are still taken (this record names them); Absorb's MakeRoom drops the least recently named. Plant 17 (unit 63) = the old refusal
    E e;
    e.b = std::make_shared<const std::vector<uint8_t>>(blobs.begin() + off, blobs.begin() + off + n);
    e.last = now_;
    bytes_ += n;
    m.emplace(h, std::move(e));
    ++counter;
  }
  Bytes Get(Map& m, uint64_t h) {
    auto it = m.find(h);
    if (it != m.end()) { it->second.last = now_; return it->second.b; }
    Bytes p = live::guest_model::ByteStore::Process().Peek(h);   // (no touch: this store's clock is not the writer's; the writer keeps what it still emits alive)
    if (p) ++counts_.from_process;
    return p;
  }
  void Sweep() {
    for (Map* m : {&vtx_, &idx_, &tex_})
      for (auto it = m->begin(); it != m->end();) {
        if (now_ - it->second.last > kKeepRecords) { bytes_ -= it->second.b->size(), ++counts_.dropped, it = m->erase(it); }
        else ++it;
      }
  }
  // S110 STOREABSENT: over the cap, the entries named longest ago go first (never one the record just absorbed named: last == now_) until the store fits; what one record names stays even over the cap
  // (it is drawn now). Runs only over the cap (one sort of the entries then; nothing in the normal case).
  void MakeRoom() {
    struct Old { uint64_t last; Map* m; uint64_t h; };
    std::vector<Old> old;
    for (Map* m : {&vtx_, &idx_, &tex_})
      for (const auto& kv : *m)
        if (kv.second.last < now_) old.push_back({kv.second.last, m, kv.first});
    std::sort(old.begin(), old.end(), [](const Old& a, const Old& b) { return a.last < b.last; });
    for (const Old& o : old) {
      if (bytes_ <= cap_) break;
      const auto it = o.m->find(o.h);
      bytes_ -= it->second.b->size(), ++counts_.dropped;
      o.m->erase(it);
    }
  }
  Map vtx_, idx_, tex_;
  uint64_t now_ = 0;
  size_t bytes_ = 0;
  size_t cap_ = kCapBytes;
  Counts counts_;
};

struct BuildStats {
  uint32_t parts = 0, parts_in_record = 0, hole_bytes = 0, hole_layout = 0, hole_parts = 0, extra_streams = 0, effect_fallback = 0, bad_triangles = 0, degenerate = 0, bad_slots = 0, bad_nodes = 0, unskinned = 0;
  uint32_t blended_no_normal = 0;   // S105 A5b F1: trans parts left opaque because their normal picture is not bound
  uint32_t blended = 0;    // S105 A5b: parts built with alpha_blend (an effect whose name holds "trans": yCh_trans eyelid layer, yCh_Aniso_trans hair ...)
  uint32_t max_node = 0;   // S103 CAWS: (highest node any vertex uses) + 1, over all parts; compared with the actor's palette size (NoteActorPalette)
  uint32_t tex_bound = 0, tex_missing = 0, tex_override = 0, no_diffuse = 0, tex_default_cube = 0;
  uint32_t tex_absent = 0;        // S110 STOREABSENT: of tex_missing, the samplers whose row NAMES a picture (state bound / override, a hash) with no bytes in reach (no row in the record, or in neither store)
  std::vector<uint64_t> absent;   // S110 STOREABSENT: those pictures' hashes and absent logo rows', each once; non-empty = the build is PROVISIONAL (guest_model_absent.h: rebuilt when one arrives)
  uint32_t logo_parts = 0, logo_missing = 0, diffuse_t0 = 0;   // S105 A5c: parts that carry a logo picture / whose logo row could not be built (a hole in the record or the store) / with the game's bool 150
  uint64_t vertices = 0, triangles = 0;
  std::string part_palettes;   // S103 CAWS (CAWG row 6): per part "p<i>: <palette size>, nodes <palette[slot]-1 ...>" (<= 8 listed), so a run says whether the values are skeleton nodes (up to ~130) or a piece's own small table (<= ~20)
  std::string why;   // empty = whole
};

// The per-ModelCache state of the actor path (what used to be function-local statics shared by the live and the mirror cache: CW2_review bug 7).
struct CacheState {
  Store store;
  std::unordered_map<std::string, uint64_t> hashes;   // key -> the model hash it was built for
  std::string why;                                    // the last refusal (the skip line's tail)
  std::unordered_map<int, uint32_t> max_node_of;      // S103 CAWS: model id -> BuildStats::max_node (set where the model is put; the per-actor palette check reads it)
  std::unordered_set<std::string> join_seen;          // CTX2: the `guest join miss` pairs already printed (capped at kMaxJoinLines)
  static constexpr uint32_t kMaxJoinLines = 16;
  std::unordered_map<std::string, std::vector<uint64_t>> absent_of;   // S110 STOREABSENT: model key -> the pictures its PROVISIONAL build lacked (guest_model_absent.h); a whole build has no entry
};
// The state in a ModelCache's slot (shared_ptr<void>& from ModelCache::GuestStateSlot).
inline CacheState& StateIn(std::shared_ptr<void>& slot) {
  if (!slot) slot = std::make_shared<CacheState>();
  return *static_cast<CacheState*>(slot.get());
}
// The top of every BuildFrame, warm or drawing (CW2_review bug 6): every record's bytes are taken, whether or not an actor asks for a guest model in it.
inline void AbsorbRecord(CacheState& s, const SceneTables& t) {
  if (t.guest_models.Empty() && s.store.Empty()) return;   // nothing carried, nothing kept: free
  s.store.Absorb(t);
}

// CAWD site 5: format 6 = k_8_8_8_8 read as linear BGRA8 (the front end's MakeTexture builds BGRA8 at mips = 1: front_end.cpp ~576, site 7).
inline assets::TexFormat TexFormatOf(uint32_t game_format) {
  return game_format == 6 ? assets::TexFormat::kB8G8R8A8 : game_format == 18 ? assets::TexFormat::kBC1 : game_format == 19 ? assets::TexFormat::kBC2 : assets::TexFormat::kBC3;
}
// CAWD debug plant (WWE_RDR_GUEST_FLAT=1, read once by rf_wrestlers.cpp): a part with no diffuse picture is given a flat bright magenta material colour, so "drawn black" and "not drawn" can be told apart in one shot.
// (BuildModel's `flat_diffuseless` parameter: the caller reads the switch once; no global is written per call.)

// S105 A5b: the file loader's blend rule (jboy.h: `*trans*` effect -> alpha_blend), applied to a guest part's material. The game blends every yCh_trans mesh (normal-map w x tint = the
// layer's opacity, arena_look.md item 12); built opaque, the created superstar's eyelid layer (guest part 11) showed as solid dark rims. Case-insensitive, on the pipeline's own name.
// g_draw_plant 12 = the old rule (no part blends).
inline bool EffectBlends(const std::string& shader) {
  if (g_draw_plant == 12) return false;
  std::string l = shader;
  for (char& c : l) c = char(AsciiLower(c));
  return l.find("trans") != std::string::npos;
}

// Builds the CPU model of the guest model at `model` from the record's rows + the store's bytes. False (stats.why says why) = a hole: never half a body.
inline bool BuildModel(const SceneTables& t, Store& store, uint32_t model, assets::Model& out, BuildStats& st, bool flat_diffuseless = false) {
  st = BuildStats{};
  const GuestModelPart* by_part[64] = {};
  uint32_t parts = 0;
  for (const GuestModelPart& r : t.guest_models.parts)
    if (r.model == model && r.part < 64) {
      by_part[r.part] = &r, parts = r.parts;
    }
  if (!parts) return st.why = "no guest-model rows for this object", false;
  st.parts = parts;
  out = assets::Model{};
  out.label = "guest model";
  std::unordered_map<std::string, int> mat_of;
  std::unordered_map<uint64_t, int> tex_of;
  bool first = true;
  for (uint32_t p = 0; p < parts; ++p) {
    const GuestModelPart* r = by_part[p];
    if (!r) { ++st.hole_parts; continue; }
    ++st.parts_in_record;
    const Store::Bytes vb = store.Vtx(r->vtx_hash), ib = store.Idx(r->idx_hash);
    if (!vb || !ib || vb->size() != size_t(r->vertex_count) * r->stride || ib->size() != size_t(r->index_count) * 2) { ++st.hole_bytes; continue; }
    assets::Mesh mesh;
    DecodeStats ds;
    if (!DecodeVertices(vb->data(), r->vertex_count, r->stride, r->vflags, r->nodes, r->node_count, mesh.vertices, ds)) { ++st.hole_layout; continue; }
    {
      char pp[200];
      int n = std::snprintf(pp, sizeof pp, "%sp%u: %u slots, nodes", st.part_palettes.empty() ? "" : "; ", p, r->node_count);
      for (uint32_t k = 0; k < r->node_count && k < 8 && n > 0 && n < 170; ++k) n += std::snprintf(pp + n, sizeof pp - size_t(n), " %d", r->nodes[k] == 0xFFFF ? -1 : int(r->nodes[k]));
      st.part_palettes += pp;
    }
    // CAWH: the game's vertices are in FILE space (-Y up); every loaded model is mirrored into viewer space by FinishModel (jboy.h) and the live palette is mirrored S.M.S (rf_wrestlers), so the bytes are mirrored here once
    // (g_draw_plant 11 = skipped: every vertex lands about 2|y_bind| from its bone). Winding is untouched, as in FinishModel (the viewer draws both faces).
    if (g_draw_plant != 11)
      for (assets::Vertex& v : mesh.vertices) v.pos[1] = -v.pos[1], v.normal[1] = -v.normal[1], v.tangent[1] = -v.tangent[1];
    st.bad_slots += ds.bad_slots, st.bad_nodes += ds.bad_nodes, st.unskinned += ds.unskinned, st.max_node = std::max(st.max_node, ds.max_node);
    std::vector<uint16_t> idx(r->index_count);
    std::memcpy(idx.data(), ib->data(), ib->size());
    const live::guest_model::StripStats ss = live::guest_model::StripsToList(idx, r->vertex_count, r->n0, g_draw_plant == 3 ? 0u : r->n1, mesh.indices);
    st.bad_triangles += ss.bad, st.degenerate += ss.degenerate, st.triangles += ss.triangles;
    // the material: the game's effect name (the jboy record's +0x7C), the sampler pictures by parameter name
    char eff[kGmNameBytes + 1] = {};
    std::memcpy(eff, r->effect, kGmNameBytes);
    std::string shader(eff);
    if (g_draw_plant == 8) {
      if (shader.compare(0, 4, "yCh_") != 0) shader = "yCh_skin", ++st.effect_fallback;   // (plant 8: the old case-sensitive test)
    } else if (const char* fx = PipelineEffect(eff)) {
      shader = fx;   // CTF: the game's lower-case name -> the pipeline's mixed-case one; a gear part keeps the gear pipeline
    } else {
      shader = "yCh_skin", ++st.effect_fallback;   // an effect the character path does not know (or an unread name): the plain skin
    }
    assets::Material mat;
    mat.shader = shader;
    std::string mkey = shader;
    bool have_diffuse = false;
    for (uint32_t i = 0; i < r->tex_count && i < kGmMaxTex; ++i) {
      const GuestModelTex& tx = r->tex[i];
      const TexSlot slot = SlotOfParam(tx.name);
      if (tx.state == kGtEngineDefault) { ++st.tex_default_cube; continue; }   // CTX2: the reader saw a cube dimension (3): the engine's default stands, not a missing picture (by the row's state, never by name)
      if (tx.state == kGtMissing || !tx.hash) { ++st.tex_missing; continue; }
      const GuestModelTexture* tr = t.guest_models.FindTexture(tx.hash);
      const Store::Bytes tb = tr ? store.Tex(tx.hash) : nullptr;
      if (!tr || !tb || tb->size() != tr->nbytes) {   // S110 STOREABSENT: named (state bound / override, a hash) but no bytes in reach: the build is provisional
        ++st.tex_missing, ++st.tex_absent;
        if (std::find(st.absent.begin(), st.absent.end(), tx.hash) == st.absent.end()) st.absent.push_back(tx.hash);
        continue;
      }
      int ti;
      auto f = tex_of.find(tx.hash);
      if (f != tex_of.end()) ti = f->second;
      else {
        assets::Texture tex;
        tex.name = "guest " + std::to_string(tx.hash);
        tex.format = TexFormatOf(tr->format), tex.width = tr->width, tex.height = tr->height, tex.mips = 1;
        tex.data.assign(tb->begin(), tb->end());
        ti = int(out.textures.size());
        out.textures.push_back(std::move(tex));
        tex_of[tx.hash] = ti;
      }
      if (tx.state == kGtOverride) ++st.tex_override;
      else ++st.tex_bound;
      mkey += "|" + std::to_string(int(slot)) + ":" + std::to_string(ti);
      switch (slot) {
        case TexSlot::Diffuse: mat.diffuse = ti, have_diffuse = true; break;
        case TexSlot::Normal: mat.normal = ti; break;
        case TexSlot::Mask: mat.mask = ti; break;
        case TexSlot::Specular: mat.specular = ti; break;
        case TexSlot::Detail: mat.detail = ti; break;
        default: break;
      }
    }
    // S105 A5c (A5c.md items 2 / 3): the model's logo pictures (the row indices the writer put in logo7_tex / logo4_tex) go on the material, and the game's bool-150 rule (the diffuse sampler's table index is 0) rides with it.
    if (r->flags & kGmDiffuseT0) mat.diffuse_t0 = true, ++st.diffuse_t0, mkey += "|T0";
    if (g_draw_plant != 14)
      for (int li = 0; li < 2; ++li) {
        const uint32_t rowi = li == 0 ? r->logo7_tex : r->logo4_tex;
        if (!rowi) continue;
        const GuestModelTexture* tr = rowi <= t.guest_models.textures.size() ? &t.guest_models.textures[rowi - 1] : nullptr;
        const Store::Bytes tb = tr ? store.Tex(tr->hash) : nullptr;
        if (!tr || !tb || tb->size() != tr->nbytes) {
          ++st.logo_missing;
          if (tr && std::find(st.absent.begin(), st.absent.end(), tr->hash) == st.absent.end()) st.absent.push_back(tr->hash);   // S110 STOREABSENT: a logo row whose bytes are in neither store: provisional too
          continue;
        }
        int ti;
        const auto f = tex_of.find(tr->hash);
        if (f != tex_of.end()) ti = f->second;
        else {
          assets::Texture tex;
          tex.name = "guest " + std::to_string(tr->hash);
          tex.format = TexFormatOf(tr->format), tex.width = tr->width, tex.height = tr->height, tex.mips = 1;
          tex.data.assign(tb->begin(), tb->end());
          ti = int(out.textures.size());
          out.textures.push_back(std::move(tex));
          tex_of[tr->hash] = ti;
        }
        (li == 0 ? mat.logo7 : mat.logo4) = ti;
        mkey += "|L" + std::to_string(li == 0 ? 7 : 4) + ":" + std::to_string(ti);
        ++st.logo_parts;
      }
    // S105 A5b (review F1): the layer's opacity IS the normal map's w (fx_trans_ps.glsl): with no normal picture bound the stand-in is opaque (grey_: the old dark rims) or half transparent (neutral_normal_: a garment
    // showing the body), so a trans part blends only when its normal texture is bound; one without is counted (blended_no_normal) and keeps the old opaque build. The material key holds the normal slot: no dedup change.
    if (EffectBlends(shader)) {
      if (mat.normal >= 0 || g_draw_plant == 13) mat.alpha_blend = true, ++st.blended;   // (plant 13 = the first rule: blend with no normal picture)
      else ++st.blended_no_normal;
    }
    if (!have_diffuse) {
      ++st.no_diffuse;
      if (flat_diffuseless) {   // CAWD debug plant: flat bright magenta (whether the yCh shader multiplies MatDifCol with its white stand-in is read off the shot)
        for (int k = 0; k < 4; ++k) mat.diffuse_color[k] = mat.ambient[k] = (k == 1 ? 0.f : 1.f);
        mkey += "|flat";
      }
    }
    auto mi = mat_of.find(mkey);
    if (mi == mat_of.end()) {
      mi = mat_of.emplace(mkey, int(out.materials.size())).first;
      out.materials.push_back(mat);
    }
    mesh.material = mi->second;
    mesh.skinned = true;
    mesh.part = "guest part " + std::to_string(p);
    mesh.src_kind = 2, mesh.src_pac = "guest", mesh.src_key = int(p);
    mesh.strip_indices = r->index_count;
    st.vertices += r->vertex_count;
    if (r->flags & kGmExtraStreams) ++st.extra_streams;
    for (const assets::Vertex& v : mesh.vertices)
      for (int k = 0; k < 3; ++k) {
        if (first) out.bounds_min[k] = out.bounds_max[k] = v.pos[k];
        out.bounds_min[k] = std::min(out.bounds_min[k], v.pos[k]), out.bounds_max[k] = std::max(out.bounds_max[k], v.pos[k]);
        if (k == 2) first = false;
      }
    out.meshes.push_back(std::move(mesh));
  }
  if (st.hole_parts || st.hole_bytes || st.hole_layout) {
    st.why = std::to_string(st.hole_parts) + " parts not in the record, " + std::to_string(st.hole_bytes) + " without bytes, " + std::to_string(st.hole_layout) + " with a vertex flag word / stride the decoder does not accept";
    return false;
  }
  return true;
}

// Process-wide counters of the actor path (rf_wrestlers), printed as the `[rdr][caw] guest draw` line the editor capture run reads (open question C3 is answered by the writer's `[rdr][caw] textures:` line).
// CTX2 (CAWD Q1): the fields are atomics (the live_scene census thread reads what the drawing thread counts), and the line is re-printed every census window with the window's DELTA (WindowCensusLine), so `actors drawn`
// is read at the END of a window, never at print time of the first model build (the old one-shot line printed `actors drawn 0` before any actor was drawn).
struct DrawCounters {
  std::atomic<uint64_t> models_built{0}, models_holes{0}, actors_drawn{0}, tex_bound{0}, tex_override{0}, tex_missing{0}, no_diffuse{0}, extra_streams{0}, bad_nodes{0}, effect_fallback{0};
  std::atomic<uint64_t> logo_parts{0}, logo_missing{0};                    // S105 A5c: guest parts built with a logo picture / whose logo row was a hole
  std::atomic<uint64_t> blended{0}, blended_no_normal{0};                  // S105 A5b: guest parts built with alpha_blend (a "trans" effect)
  std::atomic<uint64_t> bad_slots{0}, unskinned{0};  // S103 CAWS (CAWG row 5): vertices with an absent slot (255 -> bone 0) / with no skin at all, summed at model build
  std::atomic<uint64_t> nodes_ge_palette{0};         // S103 CAWS (CAWG rows 3-4): actor draws whose model uses a node >= the actor's palette size (the front end draws those with an identity matrix)
  std::atomic<uint64_t> actors_skipped_palette{0};   // a guest-model actor refused: palette size != the model's nodes (rf_wrestlers.cpp skip)
  std::atomic<uint64_t> skel_holes{0};               // own-skeleton pieces ("skel:" objects) that were holes this window (rf_wrestlers.cpp hole path)
  std::atomic<uint64_t> engine_default_cube{0};      // g_mSpecPowPrm samplers: the engine's default cube stands, not a missing picture (CAWD kind 1)
  std::atomic<uint64_t> last_draw_frame{0};          // the guest frame of the last actor drawn from a guest model
};
inline DrawCounters& Counters() { static DrawCounters c; return c; }
// The one place an actor drawn from a guest model is counted (g_draw_plant 9 = the increment is left out: the window line then disagrees with `[rf][wrestlers] drawn`).
// S103 CAWS (CAWG row 3): the real palette-size check for a guest model. `max_node_p1` = BuildStats::max_node of the model, `palette` = the actor's bone count. True (and counted) = a node outside the palette.
inline bool NoteActorPalette(uint32_t max_node_p1, uint32_t palette) {
  const bool bad = palette != 0 && max_node_p1 > palette;
  if (bad) ++Counters().nodes_ge_palette;
  return bad;
}
inline void NoteActorDrawn(uint64_t guest_frame) {
  if (g_draw_plant == 9) return;
  DrawCounters& c = Counters();
  ++c.actors_drawn;
  c.last_draw_frame.store(guest_frame, std::memory_order_relaxed);
  rdr_scene::live::drawcam::Cam().NoteBodyDraw(guest_frame);   // S103 CAWB (CAWV Q3)
}
// A plain copy of the totals (for deltas).
struct DrawTotals {
  uint64_t models_built = 0, models_holes = 0, actors_drawn = 0, tex_bound = 0, tex_override = 0, tex_missing = 0, no_diffuse = 0, extra_streams = 0, bad_nodes = 0, effect_fallback = 0, actors_skipped_palette = 0, skel_holes = 0, bad_slots = 0, unskinned = 0, nodes_ge_palette = 0,
           engine_default_cube = 0, last_draw_frame = 0, blended = 0, blended_no_normal = 0, logo_parts = 0, logo_missing = 0;   // (S105 A5c: the last two)
  // (skel_holes is NOT here: it counts every wrestler's own-skeleton piece holes, stock ones too; it must not start the CAW lines in a match)
  bool Any() const { return models_built || models_holes || actors_drawn || actors_skipped_palette || tex_bound || tex_missing; }
};
inline DrawTotals Totals() {
  const DrawCounters& c = Counters();
  DrawTotals t;
  t.models_built = c.models_built, t.models_holes = c.models_holes, t.actors_drawn = c.actors_drawn, t.tex_bound = c.tex_bound, t.tex_override = c.tex_override, t.tex_missing = c.tex_missing, t.no_diffuse = c.no_diffuse,
  t.extra_streams = c.extra_streams, t.bad_nodes = c.bad_nodes, t.effect_fallback = c.effect_fallback, t.actors_skipped_palette = c.actors_skipped_palette, t.skel_holes = c.skel_holes, t.engine_default_cube = c.engine_default_cube,
  t.last_draw_frame = c.last_draw_frame, t.bad_slots = c.bad_slots, t.unskinned = c.unskinned, t.nodes_ge_palette = c.nodes_ge_palette, t.blended = c.blended, t.blended_no_normal = c.blended_no_normal, t.logo_parts = c.logo_parts, t.logo_missing = c.logo_missing;
  return t;
}
inline std::string CensusLine(const DrawTotals& c) {
  return "[rdr][caw] guest draw: models built " + std::to_string(c.models_built) + ", holes " + std::to_string(c.models_holes) + ", actors drawn from them " + std::to_string(c.actors_drawn) + " (last at frame " + std::to_string(c.last_draw_frame) +
         ", skipped: palette mismatch " + std::to_string(c.actors_skipped_palette) + ", own-skeleton piece holes of ALL wrestlers " + std::to_string(c.skel_holes) + "), textures bound " + std::to_string(c.tex_bound) + " from override " + std::to_string(c.tex_override) +
         " missing " + std::to_string(c.tex_missing) + " engine_default_cube " + std::to_string(c.engine_default_cube) + " (parts without a diffuse " + std::to_string(c.no_diffuse) + "), effect fallbacks " + std::to_string(c.effect_fallback) + ", blended parts " + std::to_string(c.blended) + " (trans parts left opaque for a missing normal picture " + std::to_string(c.blended_no_normal) + ")" +
         ", bad palette nodes " + std::to_string(c.bad_nodes) + ", bad slots " + std::to_string(c.bad_slots) + ", unskinned vertices " + std::to_string(c.unskinned) + ", actor draws with a node >= the palette " + std::to_string(c.nodes_ge_palette) + ", parts with extra streams (HOLE) " + std::to_string(c.extra_streams) + ", parts with a logo picture " + std::to_string(c.logo_parts) + " (logo rows missing " + std::to_string(c.logo_missing) + ")";
}
inline std::string CensusLine() { return CensusLine(Totals()); }
// The periodic line (the `[scene][live] frames N-M` owner calls it once a window): this window's DELTAS first, then the totals. "" while nothing was ever counted (no CAW editor in the run). `prev` moves to now.
inline std::string WindowCensusLine(DrawTotals& prev, uint64_t from, uint64_t to) {
  const DrawTotals now = Totals();
  if (!now.Any()) return std::string();
  const auto d = [](uint64_t a, uint64_t b) { return std::to_string(a >= b ? a - b : a); };
  std::string s = "[rdr][caw] guest draw window frames " + std::to_string(from) + "-" + std::to_string(to) + ": actors drawn +" + d(now.actors_drawn, prev.actors_drawn) + ", models built +" + d(now.models_built, prev.models_built) + ", holes +" +
                  d(now.models_holes, prev.models_holes) + ", skipped palette mismatch +" + d(now.actors_skipped_palette, prev.actors_skipped_palette) + ", own-skeleton piece holes +" + d(now.skel_holes, prev.skel_holes) + ", last draw frame " +
                  std::to_string(now.last_draw_frame) + "; " + CensusLine(now);
  prev = now;
  return s;
}
inline std::string WindowCensusLine(uint64_t from, uint64_t to) {
  static DrawTotals prev;
  return WindowCensusLine(prev, from, to);
}

}  // namespace rdr_scene::guestmodel
