// The wrestlers' shadow map from the scene record (Session 103, lane E; live_maps.h). Our pass: every caster of the record (a record object whose model has character
// meshes) is drawn from the shadow light into an RGBA16F map with the generated yShadowMap effect (the same shader the game's T_ShadowMap pass runs), storing the view
// distance scaled to the light's range: (d, d, 1, 1), cleared to white. The light comes from the render camera record the MAP pass entry read (MapsInput::rows); the
// palettes are the record's skin palettes (Object::bones_*), the geometry is the file's (the front-end's assets::Model), skinned with the same chunking and the same
// bone rows as the colour pass's character path (char_effects.cpp ResolveChar). Nothing here reads the stream.
//
// What this first version does NOT do (stated, MapsCounts / the page LIVE_MAPS.md): the game's map-A collapse rule (the bones map A leaves out) and the blur / min-pair chain
// that turns the raw distance map into the readers' texture (the readers get the raw map; the chain is a second piece), the props map's own caster pass (the floor props light).
#include <algorithm>
#include <cstring>

#include "char_effects.h"
#include "bone_tag_rows.h"   // S106 G4-C: SkinChunkAbsent
#include "front_end.h"
#include "live_maps.h"
#include "live_maps_internal.h"
#include "props_map_switch.h"   // S103 MBC
#include "shadow_fit.h"   // S103 E3
#include "../start_settings.h"   // S103 SPB: PlayerQuality (shadows, shadow_half)

namespace rdr_scene {
namespace md = maps_detail;

const LiveMapsHook::CasterModel* LiveMapsHook::CasterOf(uint32_t asset, bool prop) {
  // S103 MBC: `prop` = a tagged weapon / event prop / structure piece (RMP.md 2.3 site 3): its posed CPU copy comes from the prop provider (with a pose serial), every opaque mesh of any effect is a caster, drawn rigid
  // (the posed vertices are the world's: palette slot 0 = identity). A re-pose of the same shape rewrites the vertex buffer in place; a different shape builds again.
  auto& cache = prop ? prop_casters_ : casters_;
  uint32_t serial = 0;
  const assets::Model* model = prop ? (prop_provider_ ? prop_provider_(asset, serial) : nullptr) : (provider_ ? provider_(asset) : nullptr);
  if (!model) {   // not resident (a live load is asynchronous): nothing is remembered, the next frame asks again
    const auto stale = cache.find(asset);
    if (stale != cache.end()) RetireModel(*stale->second), cache.erase(stale);
    return nullptr;
  }
  std::unique_ptr<CasterModel> old;
  const auto found = cache.find(asset);
  if (found != cache.end()) {
    if (found->second->source == model && (!prop || found->second->serial == serial)) {
      found->second->last_used = frames_;
      return found->second.get();
    }
    old = std::move(found->second);   // the front-end replaced the model under this index, or a prop was re-posed: built again (a prop of the same shape keeps its buffers)
    cache.erase(found);
  }
  auto cm = std::make_unique<CasterModel>();
  cm->source = model;
  cm->serial = serial;
  cm->last_used = frames_;
  {
    std::vector<uint8_t> vb, ib, svb;
    int src_index = -1;
    for (const assets::Mesh& me : model->meshes) {   // the same walk as FrontEnd::AddModel, so Object::mesh_mask bits mean the same meshes
      if (me.vertices.empty() || me.indices.size() < 3) continue;
      bool bad = false;
      for (uint32_t ix : me.indices) bad = bad || ix >= me.vertices.size();
      if (bad) continue;
      if (me.part.rfind("crowd flash", 0) == 0) continue;
      ++src_index;
      if (me.material < 0 || size_t(me.material) >= model->materials.size()) continue;
      const assets::Material& mat = model->materials[size_t(me.material)];
      if (prop ? (mat.alpha_blend || mat.additive) : (!IsCharEffect(mat.shader) || mat.alpha_blend)) continue;   // a wrestler's opaque pieces cast; hair / translucent layers do not. S103 MBC: a prop's opaque meshes of any effect cast (a glow / additive layer does not)
      const auto add_piece = [&](const std::vector<assets::Vertex>& verts, const std::vector<uint32_t>& indices, const SkinChunk* chunk) {
        CasterMesh cmesh;
        cmesh.vb_offset = uint32_t((vb.size() + 15) & ~size_t(15));
        vb.resize(cmesh.vb_offset);
        vb.insert(vb.end(), reinterpret_cast<const uint8_t*>(verts.data()), reinterpret_cast<const uint8_t*>(verts.data() + verts.size()));
        cmesh.ib_offset = uint32_t(ib.size());
        ib.insert(ib.end(), reinterpret_cast<const uint8_t*>(indices.data()), reinterpret_cast<const uint8_t*>(indices.data() + indices.size()));
        cmesh.index_count = uint32_t(indices.size());
        cmesh.src = src_index;
        cmesh.sample_first = uint32_t(cm->samples.size());   // S103 E3: every 8th vertex, for the bounds
        for (size_t vi = 0; vi < verts.size(); vi += 8) {
          SamplePt sp;
          sp.p[0] = verts[vi].pos[0], sp.p[1] = verts[vi].pos[1], sp.p[2] = verts[vi].pos[2];
          uint32_t heavy = 0;   // review 5: the heaviest of the vertex's bones
          if (chunk)
            for (uint32_t b = 1; b < 4; ++b)
              if (chunk->skin[vi].w[b] > chunk->skin[vi].w[heavy]) heavy = b;
          sp.slot = chunk ? uint16_t(chunk->skin[vi].ids[heavy]) : uint16_t(0);
          cm->samples.push_back(sp);
        }
        cmesh.sample_count = uint32_t(cm->samples.size()) - cmesh.sample_first;
        if (chunk) {
          cmesh.chunk = true;
          cmesh.nodes = chunk->nodes;
          cmesh.sv_offset = uint32_t((svb.size() + 15) & ~size_t(15));
          svb.resize(cmesh.sv_offset);
          svb.insert(svb.end(), reinterpret_cast<const uint8_t*>(chunk->skin.data()), reinterpret_cast<const uint8_t*>(chunk->skin.data() + chunk->skin.size()));
        }
        cm->meshes.push_back(std::move(cmesh));
      };
      if (me.skinned && !prop) {
        std::vector<SkinChunk> chunks;
        if (!BuildSkinChunks(me, chunks)) continue;
        for (const SkinChunk& c : chunks) add_piece(c.vertices, c.indices, &c);
      } else {
        add_piece(me.vertices, me.indices, nullptr);
      }
    }
    cm->vb_bytes = uint32_t(vb.size()), cm->ib_bytes = uint32_t(ib.size());
    if (prop && old && old->vb != rdr::kNoBuffer && old->vb_bytes == cm->vb_bytes && old->ib_bytes == cm->ib_bytes && old->meshes.size() == cm->meshes.size() && !vb.empty()) {
      renderer_.UpdateBuffer(old->vb, 0, vb.data(), vb.size());   // the same shape re-posed: the vertex bytes are rewritten, ordered with the frames; the indices are the model's own
      old->last_used = frames_;
      old->source = model, old->serial = serial, old->samples = std::move(cm->samples), old->meshes = std::move(cm->meshes);
      return cache.emplace(asset, std::move(old)).first->second.get();
    }
    if (old) RetireModel(*old);
    if (!vb.empty()) cm->vb = renderer_.CreateBuffer(vb.size(), vb.data());
    if (!ib.empty()) cm->ib = renderer_.CreateBuffer(ib.size(), ib.data());
    if (!svb.empty()) cm->svb = renderer_.CreateBuffer(svb.size(), svb.data());
  }
  return cache.emplace(asset, std::move(cm)).first->second.get();
}

// S103 E3: the casters' bounding sphere, in the game's space, from the same objects / pieces / palettes the draw loop uses. Every piece's sampled vertices are skinned with their dominant bone
// (the shader's blend moves a vertex by less than a limb's thickness; the fit pads for it) and put through the same y mirror as the shader rows. The sphere is the box's half diagonal.
bool LiveMapsHook::MeasureCasters(const SceneFrame& frame, float center[3], float& radius, uint32_t& samples_out) {
  const float mirror = frame.viewer_space ? -1.f : 1.f;
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  uint32_t n = 0;
  for (const Object& obj : frame.objects) {
    if (!(obj.flags & kObjVisible)) continue;
    const CasterModel* cm = CasterOf(obj.asset);
    if (!cm || cm->meshes.empty()) continue;
    assets::Matrix world;
    std::memcpy(world.data(), obj.world.m, sizeof(float) * 16);
    const Mat4* skin = nullptr;
    uint32_t skin_count = 0;
    if ((obj.flags & kObjSkinned) && obj.bones_count && size_t(obj.bones_first) + obj.bones_count <= frame.bones.size()) skin = &frame.bones[obj.bones_first], skin_count = obj.bones_count;
    for (const CasterMesh& m : cm->meshes) {
      if (m.src < 32 ? !((obj.mesh_mask_lo >> m.src) & 1u) : (m.src < 64 && !((obj.mesh_mask_hi >> (m.src - 32)) & 1u))) continue;
      if (m.chunk && rdr_scene::SkinChunkAbsent(m.nodes, skin, skin_count)) continue;   // S103 W2B (review C): a chunk that skins to an absent spring-hair piece (zero rows) is not part of the fit / mask
      std::vector<assets::Matrix>& slots = slots_scratch_;
      slots.clear();
      if (m.chunk) {
        for (uint16_t node : m.nodes) {
          assets::Matrix sk = assets::Identity();
          if (skin && node < skin_count) std::memcpy(sk.data(), skin[node].m, sizeof(float) * 16);
          slots.push_back(assets::Mul(sk, world));
        }
      }
      if (slots.empty()) slots.push_back(world);
      for (uint32_t i = 0; i < m.sample_count; ++i) {
        const SamplePt& sp = cm->samples[size_t(m.sample_first) + i];
        const assets::Matrix& mt = slots[std::min<size_t>(sp.slot, slots.size() - 1)];
        float q[3];
        for (int k = 0; k < 3; ++k) q[k] = sp.p[0] * mt[size_t(k)] + sp.p[1] * mt[size_t(4 + k)] + sp.p[2] * mt[size_t(8 + k)] + mt[size_t(12 + k)];
        q[1] *= mirror;
        if (!std::isfinite(q[0]) || !std::isfinite(q[1]) || !std::isfinite(q[2])) continue;
        for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], q[k]), hi[k] = std::max(hi[k], q[k]);
        ++n;
      }
    }
  }
  samples_out = n;
  if (n == 0) return false;
  float r2 = 0.f;
  for (int k = 0; k < 3; ++k) {
    center[k] = 0.5f * (lo[k] + hi[k]);
    const float h = 0.5f * (hi[k] - lo[k]);
    r2 += h * h;
  }
  radius = std::sqrt(r2);
  for (int k = 0; k < 3; ++k) counts_.fit_center[k] = center[k], counts_.fit_lo[k] = lo[k], counts_.fit_hi[k] = hi[k];
  return true;
}

bool LiveMapsHook::MeasuredOnce(const SceneFrame& frame, float center[3], float& radius, uint32_t& samples_out) {
  if (!meas_.tried) meas_.tried = true, meas_.ok = MeasureCasters(frame, meas_.c, meas_.r, meas_.n);
  std::memcpy(center, meas_.c, sizeof meas_.c);
  radius = meas_.r, samples_out = meas_.n;
  return meas_.ok;
}

// S103 PM2: the yShadowMap programs and vertex layouts, made once (moved out of RecordShadow: the props map's caster pass shares them). False = the effect is not in the registry.
bool LiveMapsHook::EnsureShadowPrograms() {
  if (!shadow_ready_) {
    vs_skinned_ = md::FindVariant("yShadowMap_skinned_vertex");
    vs_single_ = md::FindVariant("yShadowMap_single_bone_vertex");
    fs_shadow_ = md::FindVariant("yShadowMap_base_fragment");
    if (vs_skinned_ < 0 || vs_single_ < 0 || fs_shadow_ < 0) return false;
    const auto make_program = [&](int vs, uint16_t& program) {
      rdr::ProgramDesc d;
      d.name = rdr::fx::kVariants[vs].name;
      d.vertex_spirv = rdr::fx::kVariants[vs].spirv, d.vertex_bytes = rdr::fx::kVariants[vs].bytes;
      d.fragment_spirv = rdr::fx::kVariants[fs_shadow_].spirv, d.fragment_bytes = rdr::fx::kVariants[fs_shadow_].bytes;
      program = renderer_.AddProgram(d);
    };
    make_program(vs_skinned_, prog_skinned_);
    make_program(vs_single_, prog_single_);
    // vertex layouts over the front-end's buffers: stream 0 = assets::Vertex, stream 1 = SkinVertex (char_effects.h)
    constexpr uint16_t kRGB32F = 106, kRGBA32UI = 107, kR16UI = 74;
    {
      rdr::VertexLayoutDesc d{};
      d.strides[0] = uint32_t(sizeof(assets::Vertex)), d.strides[1] = uint32_t(sizeof(SkinVertex));
      d.attributes[0] = {0, 0, kRGB32F, uint32_t(offsetof(assets::Vertex, pos))};          // inPosition (vec4: w = 1)
      d.attributes[1] = {1, 1, kRGB32F, uint32_t(offsetof(SkinVertex, w))};                // inWeights
      d.attributes[2] = {2, 1, kRGBA32UI, uint32_t(offsetof(SkinVertex, ids))};            // inBoneIds (integer)
      d.attribute_count = 3;
      layout_skinned_ = renderer_.AddVertexLayout(d);
    }
    {
      rdr::VertexLayoutDesc d{};
      d.strides[0] = uint32_t(sizeof(assets::Vertex));
      d.attributes[0] = {0, 0, kRGB32F, uint32_t(offsetof(assets::Vertex, pos))};
      d.attributes[1] = {1, 0, kR16UI, uint32_t(offsetof(assets::Vertex, bones))};         // inBoneId (integer; unused: the bool constant picks bone 0)
      d.attribute_count = 2;
      layout_single_ = renderer_.AddVertexLayout(d);
    }
    shadow_ready_ = true;
  }
  return true;
}

bool LiveMapsHook::RecordShadow(const StageCtx& ctx, rdr::FrameBuilder& fb) {
  const SceneFrame& frame = *ctx.frame;
  const MapsInput& in = *input_;
  // S103 SPBF review L4: every `return false` of this function (a hole, SHADOWS off, no casters) leaves the image without a map worth showing again: shadow_drawn_w_ / h_ go to 0, so a half-rate skip frame
  // after a hole never republishes an old map. Only the final `return true` keeps them (published = true).
  struct DrawnGuard {
    uint32_t& w;
    uint32_t& h;
    bool published = false;
    ~DrawnGuard() { if (!published) w = h = 0; }
  } drawn_guard{shadow_drawn_w_, shadow_drawn_h_};
  // S103 SPB (SP2 M3): the player's SHADOWS setting. Off = no dynamic caster is drawn and no map is published (the readers take their plain, lit variants: the same picture the stream binder's g_shadows=false gives,
  // where the maps keep their clears). Not a hole: the player asked.
  const wwe2k14::start::PlayerQuality& quality = wwe2k14::start::Quality();
  if (!quality.shadows) {
    counts_.shadow_skip = "SHADOWS off (the player's setting)";
    shadow_drawn_w_ = shadow_drawn_h_ = 0;
    return false;
  }
  // S103 E3: where the light camera comes from. The game's caster pass entry (kScCasterStates) when the record has one and the fit is not forced; otherwise OUR fit (shadow_fit.h) from the key
  // light and the casters' bounds. No source = a counted hole with its reason, never an empty map.
  CasterRows fit_rows = {};
  WrestlerCameraRows fit_cam = {};
  const CasterRows* rows_p = &in.rows;
  const WrestlerCameraRows* cam_p = &in.cam;
  if (in.force_fit || (!in.have_light && !in.has_caster_records)) {   // review 7: never override the game's deliberate refusals (a record with caster entries but no usable light)
    if (!in.fit_key_valid) {
      counts_.shadow_skip = in.have_light ? in.fit_key_why : (in.light_why[0] && in.has_caster_records ? in.light_why : in.fit_key_why);
      ++counts_.holes;
      return false;
    }
    float center[3];
    float radius = 0.f;
    if (!MeasuredOnce(frame, center, radius, counts_.fit_samples)) {
      counts_.shadow_skip = "no caster is resident to measure (the models are not loaded yet)";
      ++counts_.holes;
      return false;
    }
    ShadowFitInput fi;
    std::memcpy(fi.key_vec, in.fit_key, sizeof fi.key_vec);
    std::memcpy(fi.center, center, sizeof center);
    fi.radius = radius;
    fi.map_w = in.map_w, fi.map_h = in.map_h;
    ShadowFitFaults ff;
    ff.view_not_light = faults_.fit_view_not_light, ff.key_not_negated = faults_.fit_key_not_negated, ff.no_bounds = faults_.fit_no_bounds;
    ShadowFit fit;
    const char* why = "";
    if (!FitShadowLight(fi, fit, why, ff, in.fit_main_forward)) {
      counts_.shadow_skip = why;
      ++counts_.holes;
      return false;
    }
    fit_rows = BuildCasterRows(fit.state);
    fit_cam = BuildWrestlerCameraRows(fit.state);
    if (!fit_cam.valid) {
      counts_.shadow_skip = "the fitted light has no readers' projection";
      ++counts_.holes;
      return false;
    }
    rows_p = &fit_rows, cam_p = &fit_cam;
    counts_.shadow_fitted = true;
    counts_.fit_radius = radius, counts_.fit_scale = fit.scale;
  } else if (!in.have_light) {
    counts_.shadow_skip = in.light_why;
    ++counts_.holes;   // review 8
    return false;
  }
  const CasterRows& crows = *rows_p;
  if (!EnsureImages(in.map_w, in.map_h, false)) {
    counts_.shadow_skip = "could not create the map images";
    ++counts_.holes;
    return false;
  }
  if (!EnsureShadowPrograms()) {
    counts_.shadow_skip = "the yShadowMap effect is not in the registry";
    ++counts_.holes;
    return false;
  }

  // S103 SPB (SP2 M3): LOW's half-rate shadows (the stream binder's g_shadow_half: the maps every other frame, the odd frames keep the previous map, one frame behind). The image keeps its contents between frames;
  // the odd frame leaves the clear and the casters out and publishes the same image again with this frame's own light matrix. Only after a drawn map of the same size exists (never an empty image).
  const bool skip_draw = quality.shadow_half && shadow_drawn_w_ == in.map_w && shadow_drawn_h_ == in.map_h && (++shadow_half_n_ & 1u) != 0;
  if (!skip_draw) {
    rdr::TargetsOp targets;
    targets.color[0].image = shadow_color_;
    targets.color_count = 1;
    targets.depth.image = shadow_depth_;
    fb.SetTargets(targets);
    rdr::ClearOp clear;
    clear.flags = rdr::kClearColor | rdr::kClearDepth | rdr::kClearStencil;
    clear.full = 1;
    clear.color[0] = clear.color[1] = clear.color[2] = clear.color[3] = 1.0f;   // the game's map starts white: nothing between the light and the receiver
    clear.depth = 1.0f;
    fb.Clear(clear);
  }

  rdr::DynamicState dynamic{};
  dynamic.viewport[2] = float(in.map_w), dynamic.viewport[3] = float(in.map_h), dynamic.viewport[5] = 1.0f;
  dynamic.scissor.width = in.map_w, dynamic.scissor.height = in.map_h;
  dynamic.system[0] = 1.f, dynamic.system[1] = -1.f, dynamic.system[2] = 1.f;   // as the front-end: clip y is up, the viewport is Vulkan's
  dynamic.system[9] = 7.f;
  const uint32_t dyn_id = fb.Dynamic(dynamic);

  // the light's rows (the transposed-light plant hands the shader the matrix the wrong way round)
  float view_proj[16], trapezoid[16], view_depth[4];
  std::memcpy(view_proj, crows.view_proj, sizeof view_proj);
  std::memcpy(trapezoid, crows.trapezoid, sizeof trapezoid);
  std::memcpy(view_depth, crows.view_depth, sizeof view_depth);
  if (faults_.transpose_light) {
    float t[16];
    md::Transpose4(view_proj, t), std::memcpy(view_proj, t, sizeof t);
  }
  rdr::SamplerDesc unused_sampler{};
  (void)unused_sampler;
  rdr::TextureSet no_textures{};
  const uint32_t tex_id = fb.Textures(no_textures);

  std::vector<uint8_t>& block = block_scratch_;
  std::vector<uint8_t> fs_block;
  const rdr::fx::Variant& fvar = rdr::fx::kVariants[fs_shadow_];
  fs_block.assign(std::max<uint32_t>(fvar.block_bytes, 16), 0);
  float prm[4];
  std::memcpy(prm, crows.shadow_prm[0], sizeof prm);
  if (faults_.scale_prm) prm[0] *= 2.f, prm[1] *= 2.f;   // plant: the light's range is wrong
  md::PutRegisters(fs_block, fvar, 168, prm, 1);
  const uint32_t fs_const = fb.PushConstants(fs_block.data(), uint32_t(fs_block.size()));

  const float mirror = frame.viewer_space ? -1.f : 1.f;   // the viewer's tables have Y mirrored once; the shader wants the game's file space (ResolveChar)
  // S103 MBC (RMP.md 2): the game's map B mode (sub_827E8080). Mode 0 = wrestlers, then categories 26 / 41 (every CWeapon-class caster: weapons, event props, ladder pieces; the cast tag), then 33 / 43 (pieces: not built here) unless the scene word is 1.
  // Mode 1 = the scene word (0x836F3940) is 1 AND there are >= 3 participants: wrestlers only. A scene word that is not 1 (entrances, cutscenes) is mode 0 whatever the count. Unknown word, or word 1 with an unknown count = a counted hole:
  // wrestlers only, never a guess. Category 24 (the ring) casts only while [0x836F39A0] is set: no ring caster is built, the gate is counted.
  bool props_cast = false;
  if (!mapb_props::On() || faults_.mapb_wrestlers_only) {
    counts_.mapb_mode = "off";
  } else if (frame.phase_word < 0) {
    counts_.mapb_mode = "hole: the scene word is not in the record";
  } else if (frame.phase_word == 1 && frame.participants < 0) {
    counts_.mapb_mode = "hole: scene word 1 and the participant count is unknown";
  } else if (frame.phase_word == 1 && frame.participants >= 3) {
    counts_.mapb_mode = "1";
  } else {
    counts_.mapb_mode = "0";
    props_cast = true;
  }
  counts_.mapb_ring_gate = frame.ring_gate > 0;
  // S103 MBC: one mesh drawn into map B (the wrestlers' and the props' share it): `slots` = the palette slot -> world rows; a chunk mesh is the skinned program, anything else the rigid one
  const auto draw_mesh = [&](const CasterModel& cm_, const CasterMesh& m, const std::vector<assets::Matrix>& slots, bool is_prop) {
    const bool skinned = m.chunk;
    const int vs = skinned ? vs_skinned_ : vs_single_;
    const rdr::fx::Variant& vvar = rdr::fx::kVariants[vs];
    block.assign(vvar.block_bytes, 0);
    md::PutRegisters(block, vvar, 56, view_proj, 4);
    md::PutRegisters(block, vvar, 64, trapezoid, 4);
    md::PutRegisters(block, vvar, 54, view_depth, 1);
    {
      std::vector<float>& rows = rows_scratch_;
      rows.assign(63 * 4, 0.f);
      for (uint32_t s = 0; s < 21; ++s) {
        const assets::Matrix& mt = slots[std::min<size_t>(s, slots.size() - 1)];
        for (uint32_t k = 0; k < 3; ++k)
          for (int i = 0; i < 4; ++i) rows[size_t(4 * (3 * s + k) + uint32_t(i))] = (k == 1 ? mirror : 1.f) * mt[size_t(i * 4) + k];
      }
      md::PutRegisters(block, vvar, 76, rows.data(), 63);
    }
    if (vvar.bools_offset != 0xFFFF && size_t(vvar.bools_offset) + 4 <= block.size()) {
      const uint32_t bits = skinned ? 0xEu : 0u;   // four bones (bits 1-3); the single-bone twin uses bone 0 with bit 0 off
      std::memcpy(block.data() + vvar.bools_offset, &bits, 4);
    }
    rdr::DrawState state{};
    state.program = skinned ? prog_skinned_ : prog_single_;
    state.vertex_layout = skinned ? layout_skinned_ : layout_single_;
    state.topology = 3;
    state.cull = 0;
    state.front_face = 1;
    state.depth_test = 1, state.depth_write = 1, state.depth_compare = 3;
    state.depth_clamp = is_prop ? 1 : 0;   // S103 MBC review 4: a prop between the light and the near plane is clamped onto it, not clipped away (caps_.depth_clamp is on); wrestlers keep the game's state
    state.blend[0].write_mask = 0xF;
    rdr::DrawOp op{};
    op.state = fb.State(state);
    op.dynamic = dyn_id;
    op.textures = tex_id;
    op.vs_constants = fb.PushConstants(block.data(), uint32_t(block.size()));
    op.fs_constants = fs_const;
    op.vertex[0] = {cm_.vb, m.vb_offset};
    if (skinned) op.vertex[1] = {cm_.svb, m.sv_offset};
    op.index = {cm_.ib, m.ib_offset};
    op.index_32bit = 1;
    op.count = m.index_count;
    op.instances = 1;
    fb.Draw(op, rdr::kGrpShadow);   // S103 GSTP
  };
  uint32_t draws = 0;
  if (!faults_.no_casters && !skip_draw)
    for (const Object& obj : frame.objects) {
      if (!(obj.flags & kObjVisible)) {
        ++counts_.casters_skipped_hidden;
        continue;
      }
      // S103 MBC: a tagged prop goes the prop way; it never counts in the wrestler skip counters
      const bool tagged = (obj.flags & kObjCastsMapB) != 0;
      const bool prop = tagged;   // (the mapb_casts_gm plant acts at tag time: rf_props.cpp, mapb_props::g_plant_cast_gm)
      if (prop) {
        if (!props_cast) {
          if (tagged) ++(counts_.mapb_mode[0] == '1' ? counts_.mapb_skipped_mode1 : counts_.mapb_skipped_hole);
          continue;
        }
        if (tagged && (obj.flags & kObjMapBPiece43) && frame.phase_word == 1) {   // category 43 casts unless the scene word is 1 (RMP 2.1)
          ++counts_.mapb_skipped_piece43;
          continue;
        }
        const CasterModel* pm = CasterOf(obj.asset, true);
        if (!pm) {
          if (tagged) ++counts_.mapb_skipped_no_model;
          continue;
        }
        if (pm->meshes.empty()) continue;
        if (tagged) {   // casters by family; the fit is wrestlers-only (the crop is not widened on a guess): a prop wholly outside the fitted sphere is counted
          ++(obj.flags & kObjMapBEvent ? counts_.mapb_fam_event : obj.flags & kObjMapBStage ? counts_.mapb_fam_stage : obj.flags & kObjMapBUnsure ? counts_.mapb_fam_unsure : counts_.mapb_fam_weapon);
          {   // review 4: the nearest sample's light depth (the readers' register row 3, game space) against the map's near distance (ShadowPrm c168.x)
            float md = 1e30f;
            for (const SamplePt& sp : pm->samples) md = std::min(md, sp.p[0] * cam_p->tex_proj_light[3][0] + sp.p[1] * mirror * cam_p->tex_proj_light[3][1] + sp.p[2] * cam_p->tex_proj_light[3][2] + cam_p->tex_proj_light[3][3]);
            if (md < crows.shadow_prm[0][0]) ++counts_.mapb_near_clip;
          }
          if (counts_.shadow_fitted && !pm->samples.empty()) {
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (const SamplePt& sp : pm->samples) {
              const float q[3] = {sp.p[0], sp.p[1] * mirror, sp.p[2]};   // the posed copy is in the object's frame with an identity world, then the same y mirror as the shader rows
              for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], q[k]), hi[k] = std::max(hi[k], q[k]);
            }
            float d2 = 0.f, r2 = 0.f;
            for (int k = 0; k < 3; ++k) {
              const float c = 0.5f * (lo[k] + hi[k]), h = 0.5f * (hi[k] - lo[k]);
              d2 += (c - counts_.fit_center[k]) * (c - counts_.fit_center[k]), r2 += h * h;
            }
            if (std::sqrt(d2) - std::sqrt(r2) > counts_.fit_radius) ++counts_.mapb_out_of_fit;
          }
        }
        const uint32_t before = draws;
        assets::Matrix world = assets::Identity();   // the posed copy's vertices are the world's already (rf_props.cpp: world = identity); the y mirror is applied by the rows below
        for (const CasterMesh& m : pm->meshes) {
          if (m.src < 32 ? !((obj.mesh_mask_lo >> m.src) & 1u) : (m.src < 64 && !((obj.mesh_mask_hi >> (m.src - 32)) & 1u))) continue;
          std::vector<assets::Matrix>& slots = slots_scratch_;
          slots.assign(1, world);
          draw_mesh(*pm, m, slots, true);
          ++draws, ++counts_.casters_meshes;
          if (tagged) ++counts_.mapb_props_meshes;
        }
        if (draws != before) {
          ++counts_.casters_objects;
          if (tagged) ++counts_.mapb_props_cast;
        }
        continue;
      }
      const CasterModel* cm = CasterOf(obj.asset);
      if (!cm) {
        ++counts_.casters_skipped_no_model;   // no model (not resident yet, or none): asked again next frame
        continue;
      }
      if (cm->meshes.empty()) {
        ++counts_.casters_skipped_not_char;
        continue;
      }
      assets::Matrix world;
      std::memcpy(world.data(), obj.world.m, sizeof(float) * 16);
      const Mat4* skin = nullptr;
      uint32_t skin_count = 0;
      if ((obj.flags & kObjSkinned) && obj.bones_count && size_t(obj.bones_first) + obj.bones_count <= frame.bones.size()) skin = &frame.bones[obj.bones_first], skin_count = obj.bones_count;
      bool any = false;
      for (const CasterMesh& m : cm->meshes) {
        if (m.src < 32 ? !((obj.mesh_mask_lo >> m.src) & 1u) : (m.src < 64 && !((obj.mesh_mask_hi >> (m.src - 32)) & 1u))) continue;
        // the piece's bone rows: palette slot -> (skin, then the object's place), Y row mirrored for a viewer-space record (char_effects.cpp ResolveChar WorldMatrixArray)
        if (m.chunk && rdr_scene::SkinChunkAbsent(m.nodes, skin, skin_count)) continue;   // S103 W2B (review C): a chunk that skins to an absent spring-hair piece (zero rows) is not part of the fit / mask
        std::vector<assets::Matrix>& slots = slots_scratch_;
        slots.clear();
        if (m.chunk) {
          for (uint16_t node : m.nodes) {
            assets::Matrix sk = assets::Identity();
            if (skin && node < skin_count) std::memcpy(sk.data(), skin[node].m, sizeof(float) * 16);
            slots.push_back(assets::Mul(sk, world));
          }
        }
        if (slots.empty()) slots.push_back(world);
        draw_mesh(*cm, m, slots, false);
        ++draws, ++counts_.casters_meshes;
        any = true;
      }
      if (any) {
        ++counts_.casters_objects;
        if (guest_pred_ && guest_pred_(obj.asset)) ++counts_.guest_casters;   // S103 CS1B
      }
    }
  caster_draws_ += draws;
  guest_casters_last_ = counts_.guest_casters;   // S103 CS1B review F5
  if (draws == 0 && !skip_draw) {   // S103 L7 (review 5): nothing cast: no map is published (the readers take their plain variants) and no chain runs over it. Not a hole: no wrestler is in the frame.
    counts_.shadow_skip = "no caster meshes drawn";
    shadow_drawn_w_ = shadow_drawn_h_ = 0;   // S103 SPB: the image holds an empty map now: nothing to show again
    return false;
  }
  if (!skip_draw) shadow_drawn_w_ = in.map_w, shadow_drawn_h_ = in.map_h;

  MapImage mi;
  mi.role = MapRole::ShadowMap;
  mi.image = shadow_color_;
  // matrix = TexProjLight, VS c68-c71 (bias and the half texel INCLUDED), rows as the register file holds them; in.have_light already implies cam.valid and a checked size
  std::memcpy(mi.matrix, cam_p->tex_proj_light, sizeof mi.matrix);
  mi.params[0] = float(in.map_w), mi.params[1] = float(in.map_h), mi.params[2] = crows.shadow_prm[0][0], mi.params[3] = crows.shadow_prm[0][1];
  std::memcpy(mi.prm, crows.shadow_prm, sizeof mi.prm);   // S103 L6: the readers' c168-c170, from the record (stream route) or the fit (shadow_fit.h)
  mi.raw = true;   // S103 L6: no blur / min-pair chain behind this map (header, LIVE_MAPS.md): the readers get the raw map
  maps_.maps.push_back(mi);
  drawn_guard.published = true;
  return true;
}

}  // namespace rdr_scene
