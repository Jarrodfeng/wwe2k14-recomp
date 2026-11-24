// The ring ropes' shadow proxy in the plane mask (Session 103, lane B2F; live_map_rope.h has the whole story). Two parts:
//  * (BuildRopeInput, the record side, is in live_map_rope_input.cpp: no renderer, so the offline units link it.)
//  * LiveMapsHook::DrawRopeProxies: the proxy mesh (ring entry 956 of the player's own bgNN.pac, loaded once per arena on a worker) drawn once per rope and part through OUR mask program
//    (rdr_plane_mask), the palette = the rope's node rows by the mesh's palette (slot k = pool row palette[k]), the eye = the ring kind on the rope's side.
// Everything here is read from the record and the player's files; nothing from the stream.
#include "../thread_name.h"  // S103 H1
#include "rows_plane.h"   // S106 G4-2
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../assets/ring_game_space.h"
#include "../assets/shadow_caster_vertex.h"
#include "../renderer/problem.h"   // rdr::Log
#include "char_effects.h"   // SkinVertex
#include "live_maps.h"
#include "live_maps_internal.h"

namespace rdr_scene {

// ---- the proxy mesh -------------------------------------------------------------------------------------------------------------------------------------------------------------------------

namespace {

constexpr int kProxyEntry = 956;

// The mesh of one proxy part in OUR vertex streams: stream 0 = float3 position, stream 1 = SkinVertex (the one skin pair: weight 1 on the slot). The slot comes from the validated shadow-vertex writer
// (assets::shadow_vertex, the ring proxy's layout: pos @0, fill @16, ids @20, xdword @24, stride 28), so the slot rule is the one proved byte for byte on every vertex of the stream's own draws.
bool BuildPart(const assets::ring_gs::RingMesh& m, std::vector<float>& pos, std::vector<SkinVertex>& skin, std::vector<uint32_t>& list, std::string& why) {
  using namespace assets::shadow_vertex;
  if (m.pairs != 1) return why = "the proxy mesh has " + std::to_string(m.pairs) + " skin pairs (one expected)", false;
  Layout lay;
  lay.host_stride = 28;
  lay.r = {{Role::Pos, 0}, {Role::Fill, 16}, {Role::Ids, 20}, {Role::XDword, 24}};
  Options opt;
  opt.fill = FillMode::Weight255;
  std::vector<uint8_t> bytes;
  if (!Write(m.data, lay, opt, Faults(), bytes) || bytes.size() != size_t(m.vertices) * 28) return why = "the proxy vertices could not be written", false;
  pos.resize(size_t(m.vertices) * 3);
  skin.resize(m.vertices);
  for (uint32_t v = 0; v < m.vertices; ++v) {
    const uint8_t* d = bytes.data() + size_t(v) * 28;
    std::memcpy(&pos[size_t(v) * 3], d, 12);
    const uint32_t slot = d[20];
    if (slot >= m.data.palette_count) return why = "a proxy vertex names palette slot " + std::to_string(slot) + " of " + std::to_string(m.data.palette_count), false;
    SkinVertex sv{};
    for (int k = 0; k < 4; ++k) sv.ids[k] = slot;
    sv.w[0] = 1.f;
    skin[v] = sv;
  }
  // the strip groups (joined by one restart) -> a triangle list (no culling in the mask draw: the winding does not matter)
  const std::vector<uint16_t> strip = assets::ring_gs::BuildIndices(m);
  list.clear();
  uint32_t run = 0;
  uint16_t a = 0, b = 0;
  for (uint16_t ix : strip) {
    if (ix == 0xFFFF) {
      run = 0;
      continue;
    }
    if (ix >= m.vertices) return why = "a proxy index is past the vertices", false;
    if (run >= 2 && a != b && b != ix && a != ix) {
      if (run & 1) list.insert(list.end(), {b, a, uint32_t(ix)});
      else list.insert(list.end(), {a, b, uint32_t(ix)});
    }
    a = b, b = ix;
    ++run;
  }
  return true;
}

}  // namespace

uint32_t LiveMapsHook::DrawRopeProxies(rdr::FrameBuilder& fb, const E2Input& in, const float* head, uint32_t dyn_id, uint32_t tex_id, uint32_t fs_const) {
  E2State& s = *e2_;
  if (!s.rope) s.rope = std::make_unique<RopeProxyState>();
  RopeProxyState& rp = *s.rope;
  rp.draws = rp.ropes_drawn = rp.ropes_dropped = 0;
  rp.note = "";
  const RopeInput& ri = in.rope;
  if (e2_faults_.plane_rope_none) return rp.note = "planted: no rope proxy", 0;
  if (!ri.count) return rp.note = ri.calls_seen ? "the record's rope rows are unusable (no nodes / out of range)" : "the record has no rope rows (kScRingRows)", 0;
  if (ri.arena < 0) return rp.note = "the record names no arena pac (the proxy is read from it)", 0;
  uint32_t eyes = 0;   // the ring kinds' eyes the record carries
  for (uint32_t k = kPeRing0; k <= kPeRing3; ++k)
    if (in.plane_eye_have & (1u << k)) eyes |= 1u << k;
  if (!eyes) return rp.note = "no ring eye points in the record (kScPlaneEyes kinds 4-7)", 0;

  // ---- the mesh: one worker load per arena, uploaded on the render thread
  if (rp.arena != ri.arena) {   // a new arena: forget the old proxy (the buffers go to the graveyard)
    if (rp.load_state.load() == 1) return rp.note = "the previous arena's proxy is still loading (reset when it is done)", 0;   // review 4: never join on the render thread
    if (rp.worker.joinable()) rp.worker.join();   // (state 0 or 2: the worker is done)
    Retire(rp.vb), Retire(rp.sv), Retire(rp.ib);
    rp.vb = rp.sv = rp.ib = rdr::kNoBuffer;
    rp.parts.clear();
    rp.loaded.reset();
    rp.error.clear();
    rp.uploaded = rp.failed = false;
    rp.load_state = 0;
    rp.arena = ri.arena;
  }
  if (rp.failed) return rp.note = "the proxy mesh could not be made (see the log)", 0;
  if (rp.load_state.load() == 0) {
    if (game_root_.empty()) return rp.note = "no game folder to read the arena's proxy from", 0;
    rp.load_state = 1;
    const int arena = rp.arena;
    const std::string root = game_root_;
    RopeProxyState* st = &rp;
    rp.worker = std::thread([st, arena, root] { wwe2k14::NameThread(L"live_rope");
      auto ring = std::make_shared<assets::ring_gs::RingArena>();
      std::string err;
      if (!assets::ring_gs::Init(root)) err = "no game files at " + root;
      else if (!assets::ring_gs::Load(arena, *ring, err)) ring.reset();
      st->error = err;
      st->loaded = ring;
      st->load_state = 2;
    });
  }
  if (rp.load_state.load() == 1 && blocking_loads_ && rp.worker.joinable()) rp.worker.join();
  if (rp.load_state.load() == 1) return rp.note = "the proxy mesh is loading", 0;
  if (!rp.uploaded) {
    if (rp.worker.joinable()) rp.worker.join();
    rp.uploaded = true;
    std::vector<uint8_t> vb, sv, ib;
    std::string why;
    if (!rp.loaded) why = rp.error.empty() ? "the arena has no ring" : rp.error;
    else {
      for (int part = 0; part < 4 && why.empty(); ++part) {
        const assets::ring_gs::RingMesh* m = rp.loaded->Find(kProxyEntry, part);
        if (!m) continue;
        std::vector<float> pos;
        std::vector<SkinVertex> skin;
        std::vector<uint32_t> list;
        if (!BuildPart(*m, pos, skin, list, why)) break;
        RopeProxyState::Part p;
        p.vb = uint32_t((vb.size() + 15) & ~size_t(15)), vb.resize(p.vb);
        vb.insert(vb.end(), reinterpret_cast<const uint8_t*>(pos.data()), reinterpret_cast<const uint8_t*>(pos.data() + pos.size()));
        p.sv = uint32_t((sv.size() + 15) & ~size_t(15)), sv.resize(p.sv);
        sv.insert(sv.end(), reinterpret_cast<const uint8_t*>(skin.data()), reinterpret_cast<const uint8_t*>(skin.data() + skin.size()));
        p.ib = uint32_t(ib.size());
        ib.insert(ib.end(), reinterpret_cast<const uint8_t*>(list.data()), reinterpret_cast<const uint8_t*>(list.data() + list.size()));
        p.index_count = uint32_t(list.size());
        p.palette_count = m->data.palette_count;
        std::memcpy(p.palette, m->data.palette, sizeof p.palette);
        rp.parts.push_back(p);
      }
      if (why.empty() && rp.parts.empty()) why = "the arena's ring has no proxy entry 956";
    }
    if (!why.empty()) {
      rp.failed = true;
      rdr::Log("[scene][maps] rope proxy (arena %d): %s", rp.arena, why.c_str());
      return rp.note = "the proxy mesh could not be made", 0;
    }
    rp.vb = renderer_.CreateBuffer(vb.size(), vb.data());
    rp.sv = renderer_.CreateBuffer(sv.size(), sv.data());
    rp.ib = renderer_.CreateBuffer(ib.size(), ib.data());
    rp.loaded.reset();   // the file meshes are no longer needed
    rdr::Log("[scene][maps] rope proxy (arena %d): %zu parts, %zu / %zu / %zu bytes", rp.arena, rp.parts.size(), vb.size(), sv.size(), ib.size());
  }
  if (!rp.layout_ready) {
    constexpr uint16_t kRGB32F = 106, kRGBA32UI = 107;
    rdr::VertexLayoutDesc l{};
    l.strides[0] = 12, l.strides[1] = uint32_t(sizeof(SkinVertex));
    l.attributes[0] = {0, 0, kRGB32F, 0};                                        // in_position (w = 1)
    l.attributes[1] = {1, 1, kRGB32F, uint32_t(offsetof(SkinVertex, w))};         // in_weights
    l.attributes[2] = {2, 1, kRGBA32UI, uint32_t(offsetof(SkinVertex, ids))};     // in_bone_ids (integer)
    l.attributes[3] = {3, 0, uint16_t(100), 0};                                   // in_bone_id4 (float; unused by the four-bone path, declared because the shader reads location 3)
    l.attribute_count = 4;
    rp.layout = renderer_.AddVertexLayout(l);
    rp.layout_ready = true;
  }
  if (rp.vb == rdr::kNoBuffer || rp.sv == rdr::kNoBuffer || rp.ib == rdr::kNoBuffer) return rp.note = "the proxy buffers could not be made", 0;

  // ---- which ring kind lights which rope: the eye on the rope's own side (largest dot of the eye's x z with the rope's centre)
  int kind_of[kRopeMax];
  for (uint32_t i = 0; i < ri.count; ++i) {
    int best = -1;
    float best_dot = -1e30f;
    for (uint32_t k = kPeRing0; k <= kPeRing3; ++k) {
      if (!(eyes & (1u << k))) continue;
      const float d = in.plane_eye_kind[k][0] * ri.rope[i].cx + in.plane_eye_kind[k][2] * ri.rope[i].cz;
      if (d > best_dot) best_dot = d, best = int(k);
    }
    // review 5: a ring kind missing from the record must not leave a rope with the wrong side's eye: the best eye must point the rope's way (dot > half the product of the lengths)
    const float el = best >= 0 ? std::sqrt(in.plane_eye_kind[best][0] * in.plane_eye_kind[best][0] + in.plane_eye_kind[best][2] * in.plane_eye_kind[best][2]) : 0.f;
    const float cl = std::sqrt(ri.rope[i].cx * ri.rope[i].cx + ri.rope[i].cz * ri.rope[i].cz);
    kind_of[i] = (best >= 0 && best_dot > 0.5f * el * cl && cl > 0.f) ? best : -1;
  }
  const int first_kind = int(__builtin_ctz(eyes));
  int opposite = -1;   // the kind whose direction is the most against the first one's
  {
    float lo = 1e30f;
    for (uint32_t k = kPeRing0; k <= kPeRing3; ++k) {
      if (!(eyes & (1u << k)) || int(k) == first_kind) continue;
      const float d = in.plane_eye_kind[k][0] * in.plane_eye_kind[first_kind][0] + in.plane_eye_kind[k][2] * in.plane_eye_kind[first_kind][2];
      if (d < lo) lo = d, opposite = int(k);
    }
  }
  static const bool log_on = std::getenv("WWE_RDR_B2F_LOG") != nullptr;

  alignas(16) float block[308];
  uint32_t draws = 0;
  for (uint32_t i = 0; i < ri.count; ++i) {
    int kind = kind_of[i];
    if (kind < 0) { ++rp.ropes_dropped; continue; }
    if (e2_faults_.plane_rope_drop_side && kind == first_kind) { ++rp.ropes_dropped; continue; }   // planted: one side's three ropes
    if (e2_faults_.plane_rope_swap_eyes && opposite >= 0) {                                         // planted: two opposite sides' eyes exchanged
      if (kind == first_kind) kind = opposite;
      else if (kind == opposite) kind = first_kind;
    }
    const RopeInput::Rope& r = ri.rope[i];
    bool drew = false;
    for (const RopeProxyState::Part& p : rp.parts) {
      std::memset(block, 0, sizeof block);
      std::memcpy(block, head, 56 * sizeof(float));
      std::memcpy(block + 0, in.plane_eye_kind[kind], 16);
      bool ok = true;
      for (uint32_t k = 0; k < p.palette_count && k < 21; ++k) {
        const uint32_t node = p.palette[k];   // [C: run 29 f500, every slot of both parts: the stream's row of slot k = the record's pool row palette[k] itself (pool rows 0 and 1 are the unfilled identity rows), NOT palette[k] - 1]
        if (node >= r.n) { ok = false; break; }
        std::memcpy(block + 56 + 12 * k, ri.rows.data() + size_t(r.first + node) * 12, 12 * sizeof(float));   // rows = (m[k], m[4 + k], m[8 + k], m[12 + k]): the shader's three rows of a bone
      }
      if (!ok) continue;
      rdr::DrawState state{};
      state.program = s.plane_prog;
      state.vertex_layout = rp.layout;
      state.topology = 3;
      state.cull = 0;
      state.front_face = 1;
      state.depth_test = 1, state.depth_write = 1, state.depth_compare = 3;
      state.blend[0].write_mask = 0xF;
      rdr::DrawOp op{};
      op.state = fb.State(state);
      op.dynamic = dyn_id;
      op.textures = tex_id;
      op.vs_constants = fb.PushConstants(block, sizeof block);
      op.fs_constants = fs_const;
      op.vertex[0] = {rp.vb, p.vb};
      op.vertex[1] = {rp.sv, p.sv};
      op.index = {rp.ib, p.ib};
      op.index_32bit = 1;
      op.count = p.index_count;
      op.instances = 1;
      fb.Draw(op, rdr::kGrpRing);   // S103 GSTP (rope proxies: the ring's ropes)
      ++draws, drew = true;
    }
    if (drew) ++rp.ropes_drawn;
    else ++rp.ropes_dropped;
    if (log_on && i == 6) {
      for (const RopeProxyState::Part& p : rp.parts) {
        std::fprintf(stderr, "[B2F]   palette (%u):", p.palette_count);
        for (uint32_t k = 0; k < p.palette_count; ++k) std::fprintf(stderr, " %u", p.palette[k]);
        std::fprintf(stderr, "\n");
      }
      std::fprintf(stderr, "[B2F]   node y/x:");
      for (uint32_t k = 0; k < r.n; ++k) std::fprintf(stderr, " %u:%.3f/%.3f", k, ri.rows[size_t(r.first + k) * 12 + 7], ri.rows[size_t(r.first + k) * 12 + 3]);
      std::fprintf(stderr, "\n");
    }
    if (log_on && (i % 3) == 0)
      for (uint32_t k = 0; k < r.n && k < 6; ++k) {
        const float* q = ri.rows.data() + size_t(r.first + k) * 12;
        std::fprintf(stderr, "[B2F]   rope %u node %u rows %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f\n", i, k, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10], q[11]);
      }
    if (log_on)
      std::fprintf(stderr, "[B2F] rope %u: nodes %u centre (%.2f %.2f) kind %d eye (%.0f %.0f %.0f) node0 t (%.3f %.3f %.3f) %s\n", i, r.n, r.cx, r.cz, kind, in.plane_eye_kind[kind][0], in.plane_eye_kind[kind][1], in.plane_eye_kind[kind][2],
                   ri.rows[size_t(r.first) * 12 + 3], ri.rows[size_t(r.first) * 12 + 7], ri.rows[size_t(r.first) * 12 + 11], drew ? "drawn" : "DROPPED");
  }
  rp.draws = draws;
  if (!draws) rp.note = "no rope proxy could be drawn (kind / palette)";
  return draws;
}

}  // namespace rdr_scene
