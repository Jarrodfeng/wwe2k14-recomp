// Session 98 (phone port, stage A): the backend-neutral half of the Arena Viewer renderer (renderer.h). Everything here moved out of
// the D3D11 renderer unchanged in what it computes; the GPU work is in renderer_d3d11.cpp / renderer_vk.cpp.
#include "renderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "../crowd_flash.h"
#include "json_lite.h"

namespace view {

#ifdef _WIN32
std::unique_ptr<Renderer> MakeRendererD3D11();  // renderer_d3d11.cpp
#endif
#ifdef WWE_VIEWER_VK
std::unique_ptr<Renderer> MakeRendererVk();  // renderer_vk.cpp
#endif

std::unique_ptr<Renderer> MakeRenderer(const std::string& backend, std::string& err) {
#ifdef _WIN32
  if (backend == "d3d11") return MakeRendererD3D11();
#endif
#ifdef WWE_VIEWER_VK
  if (backend == "vk") return MakeRendererVk();
#endif
  err = "no such renderer backend in this build (d3d11 on Windows; vk when built with WWE_VIEWER_VK)";
  return nullptr;
}

// Session 98 (replay S3): DrawCB float4 slot of a bank_dump field name (idx = the light for ldir / lcol); -1 unknown.
int Renderer::CbSlot(const std::string& f, int idx) {
  static const char* const kNames[] = {"offset", "dif", "flags", "flags2", "mat_amb", "mat_dif", "spec", "misc"};
  for (int i = 0; i < 8; i++) if (f == kNames[i]) return i;
  if (f == "ldir") return idx >= 0 && idx < 4 ? 8 + idx : -1;
  if (f == "lcol") return idx >= 0 && idx < 4 ? 12 + idx : -1;
  static const char* const kRest[] = {"sky", "ground", "limit", "xform", "hl_dir", "hl_col", "sss1", "sss2", "refc", "wspec", "wmisc",
                                      "wnrm", "wdet", "wdet_a", "wdet_b"};
  for (int i = 0; i < 15; i++) if (f == kRest[i]) return 16 + i;
  if (f == "plcol") return idx >= 0 && idx < 4 ? 31 + idx : -1;
  if (f == "plscale") return 35;
  return -1;
}

// ---- models ----------------------------------------------------------------------------------------------------------

// Session 98: is this mesh part of a wrestler (a yCh_* material)?
static bool gm_wrestler_mesh(const assets::Model& m, const assets::Mesh& me) {
  if (me.material < 0 || me.material >= (int)m.materials.size()) return false;
  if (m.materials[me.material].crowd) return false;  // (an audience member is never a shadow caster)
  std::string lc = m.materials[me.material].shader;
  for (char& ch : lc) ch = (char)tolower((unsigned char)ch);
  return lc.rfind("ych_", 0) == 0;
}

bool Renderer::AddModel(const assets::Model& m, const float offset[3], std::vector<std::string>& warnings, std::string& err) {
  GpuModel g;
  g.gpu = NewModelData();
  memcpy(g.offset, offset, sizeof g.offset);
  float wbmin[3] = {1e30f, 1e30f, 1e30f}, wbmax[3] = {-1e30f, -1e30f, -1e30f};
  for (auto& t : m.textures) g.tex_ok.push_back(UploadTexture(*g.gpu, t, warnings) ? 1 : 0);
  // a material's texture: its slot when that upload worked, else none
  auto slot = [&](int i) { return i >= 0 && i < (int)g.tex_ok.size() && g.tex_ok[(size_t)i] ? i : -1; };
  g.label = m.label;  // Session 98 (UI): what the Assets tab lists
  for (auto& t : m.textures) g.tex_info.push_back({t.name, t.width, t.height, t.mips, t.format, t.live});
  for (auto& mt : m.materials) {
    GpuMaterial gm;
    gm.diffuse = slot(mt.diffuse);
    gm.lightmap = slot(mt.lightmap);
    gm.normal = slot(mt.normal);
    memcpy(gm.diffuse_color, mt.diffuse_color, sizeof gm.diffuse_color);
    memcpy(gm.amb, mt.ambient, sizeof gm.amb); memcpy(gm.spec_col, mt.spec_col, sizeof gm.spec_col);
    memcpy(gm.limit, mt.light_limit, sizeof gm.limit);
    gm.spec_lev = mt.spec_lev; gm.spec_pow = mt.spec_pow;
    gm.spec_lv_base = mt.spec_lv_base; gm.spec_pw_base = mt.spec_pw_base; gm.fairy = mt.fairy; gm.fairy_edge = mt.fairy_edge;
    memcpy(gm.ref_col, mt.ref_col, sizeof gm.ref_col); memcpy(gm.sss_col, mt.sss_col, sizeof gm.sss_col);
    memcpy(gm.sss_col2, mt.sss_col2, sizeof gm.sss_col2); gm.sss = mt.has_sss;
    {  // class: the wrestler recipe for the yCh_* family (the arena and ring pieces use the arena recipe)
      std::string lc = mt.shader; for (char& ch : lc) ch = (char)tolower((unsigned char)ch);
      gm.wrestler = lc.rfind("ych_", 0) == 0;
      gm.hair = lc.find("aniso") != std::string::npos || lc.find("trans") != std::string::npos;
      gm.spec_mask = !gm.wrestler ? 0 : gm.hair ? 0 : (mt.normal >= 0 ? 1 : 2);
      gm.stage_lm = !gm.wrestler && mt.has_lm_slot && mt.lightmap < 0;
      gm.crowd = mt.crowd;
      // Session 98 (flash fix): renderer.h GpuMaterial::ent_lm (the entrance's LM shader variants)
      gm.ent_lm = !gm.wrestler && !mt.crowd && mt.lightmap < 0 &&
                  (lc == "yprop_default" || lc == "yprop_psm" || lc == "ybg_default" || lc == "ybg_uvscroll");
      gm.ent_lm_bg = lc.rfind("ybg_", 0) == 0;
      gm.own_lm_bg = !gm.wrestler && !mt.crowd && mt.lightmap >= 0 && lc.rfind("ybg_", 0) == 0;
      gm.ring_mat = lc == "yprop_psm";  // Session 98 (shadow / match light): the ring mat, the plane shadow's only receiver
      // Session 98 (wrestler fix 3): the shader family (renderer.h GpuMaterial::wclass). Skin: yCh_skin / yCh_skin_flex (body and head
      // draws: wrestler_skin*_entrance.frag, wrestler_head_entrance.frag); hair / translucent: yCh_trans, yCh_Aniso_trans,
      // yCh_Anisotropic (wrestler_trans*.frag, wrestler_hair.frag); the rest of the yCh_* classes take the gear / reflect frame
      // (wrestler_body_entrance.frag, wrestler_reflect_entrance.frag). [U: yCh_cloth / yCh_Ls not seen in a probe.]
      if (gm.wrestler) gm.wclass = gm.hair ? 3 : lc.rfind("ych_skin", 0) == 0 ? 1 : 2;
    }
    gm.detail = slot(mt.detail);
    gm.detail_mask = slot(mt.mask);
    memcpy(gm.noise_near, mt.noise_near, sizeof gm.noise_near); memcpy(gm.noise_far, mt.noise_far, sizeof gm.noise_far);
    memcpy(gm.noise_dist, mt.noise_dist, sizeof gm.noise_dist);
    gm.alpha_blend = mt.alpha_blend; gm.alpha_test = mt.alpha_test; gm.additive = mt.additive; gm.cull_back = mt.cull_back;
    gm.blend_inline = mt.blend_inline;
    g.materials.push_back(gm);
  }
  g.materials.push_back(GpuMaterial());  // fallback for a bad material index (last entry)
  // every bone index a vertex holds must have a palette entry (SetSkin pads with identity): at least 256 (the old 8-bit range), more for a
  // big rig (Session 99: Fandango's 659-node stage set; Vertex::bones is 16-bit)
  g.model_bones = 256;
  for (const auto& me : m.meshes)
    for (const auto& v : me.vertices)
      for (int k = 0; k < 4; ++k) g.model_bones = std::max<size_t>(g.model_bones, size_t(v.bones[k]) + 1);
  g.lm_xy[0] = m.lm_scale[0]; g.lm_xy[1] = m.lm_scale[1]; g.lm_gain = m.lm_gain;
  g.lights = m.light_sets;
  g.lights.push_back(assets::DefaultRig());  // last entry: the default rig (wrestlers, meshes with no set)
  g.stage_lm = slot(m.stage_lightmap);
  for (auto& pl : m.placements) {
    GpuPlacement gp;
    gp.count = uint32_t(pl.transforms.size());
    if (!UploadPlacement(*g.gpu, pl.transforms, err)) return false;
    g.placements.push_back(gp);
  }
  for (auto& pl : m.placements) {  // Session 98 (step 16): the copies' positions and turns, for the INSPECT panel
    PlacementMeta pm;
    pm.label = pl.label;
    for (const assets::Matrix& t : pl.transforms) pm.mats.insert(pm.mats.end(), t.begin(), t.end());
    g.pmeta.push_back(std::move(pm));
  }
  for (auto& me : m.meshes) {
    if (me.vertices.empty() || me.indices.size() < 3) continue;
    bool bad = false;
    for (uint32_t ix : me.indices) if (ix >= me.vertices.size()) { bad = true; break; }
    if (bad) { warnings.push_back("mesh '" + me.part + "': index out of range, skipped"); continue; }
    GpuMesh gm;
    if (!UploadMesh(*g.gpu, me, gm.buf, err)) return false;
    gm.index_count = (uint32_t)me.indices.size();
    gm.vertex_count = (uint32_t)me.vertices.size();
    gm.placement = (me.placement >= 0 && me.placement < (int)g.placements.size()) ? me.placement : -1;
    gm.skinned = me.skinned;
    gm.screen = me.part == "slot 800";
    {  // Session 98: ring pieces (slots 951-999) cast the props map
      int key = 0;
      gm.caster = sscanf(me.part.c_str(), "slot %d", &key) == 1 && key >= 951 && key <= 999;
    }
    gm.light = (me.light_set >= 0 && me.light_set < (int)m.light_sets.size()) ? me.light_set : (int)g.lights.size() - 1;
    gm.material = (me.material >= 0 && me.material < (int)m.materials.size()) ? me.material : (int)g.materials.size() - 1;
    {  // Session 98 (flash fix): the stage movie surfaces keep their unlit look (no entrance LM); the flash sprite goes to the flash pass only
      int key = 0;
      gm.movie = sscanf(me.part.c_str(), "slot %d", &key) == 1 && key >= 800 && key <= 809;
      if (me.part.rfind("crowd flash", 0) == 0) { g.flash_meshes.push_back(std::move(gm)); continue; }
    }
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (auto& v : me.vertices)
      for (int i = 0; i < 3; i++) {
        if (!std::isfinite(v.pos[i])) continue;
        mn[i] = std::min(mn[i], v.pos[i]); mx[i] = std::max(mx[i], v.pos[i]);
      }
    for (int i = 0; i < 3; i++) gm.center[i] = (mn[i] <= mx[i]) ? (mn[i] + mx[i]) * 0.5f : 0.f;
    if (gm_wrestler_mesh(m, me)) {  // Session 98: the model's wrestler bounds (the shadow fit)
      g.has_wrestler = true;
      for (int i = 0; i < 3; i++)
        if (mn[i] <= mx[i]) { wbmin[i] = std::min(wbmin[i], mn[i]); wbmax[i] = std::max(wbmax[i], mx[i]); }
    }
    {  // Session 98 (step 16): where this mesh came from
      PartMeta pm;
      pm.kind = me.src_kind; pm.pac = me.src_pac; pm.key = me.src_key; pm.part = me.part; pm.node = me.node; pm.palette = me.palette;
      pm.skinned = me.skinned; pm.placement = gm.placement;
      pm.light_id = (me.light_set >= 0 && me.light_set < (int)m.light_sets.size()) ? m.light_sets[(size_t)me.light_set].id : -1;
      if (me.material >= 0 && me.material < (int)m.materials.size()) {
        const assets::Material& mt = m.materials[(size_t)me.material];
        pm.shader = mt.shader; pm.diffuse_name = mt.diffuse_name; pm.additive = mt.additive; pm.blended = mt.alpha_blend || mt.alpha_test;
        const std::pair<const char*, int> slots[] = {{"Diffuse", mt.diffuse}, {"Normal", mt.normal}, {"Lightmap", mt.lightmap}, {"Mask", mt.mask}, {"Specular", mt.specular}};
        for (auto& s : slots)
          if (s.second >= 0 && s.second < (int)g.tex_info.size()) pm.tex.push_back({s.first, s.second});
        // Session 98 (replay S3/S4, the black eyes): a wrestler's eyelid layer = an extra piece (20000-39999) drawn translucent whose
        // palette holds the eyelid bones (`mabuta`). The game never draws it in an entrance [C: probe runs 6816 + 6818, 12 frames: no draw
        // with its vertex count for Orton (161, 258 vertices) or Cena (166, 323); capture rp_ringin: every pixel around the eye comes from
        // the eye / head draws]. The viewer drew it on top of the eyeball: the black ring. When the game shows it (blinks / KO?) is [U].
        gm.lid_layer = me.src_kind == 2 && me.src_key >= 20000 && me.src_key < 40000 && mt.shader.find("trans") != std::string::npos &&
                       me.palette.find("mabuta") != std::string::npos;
      }
      g.parts.push_back(std::move(pm));
    }
    g.meshes.push_back(std::move(gm));
  }
  if (!FinishModel(*g.gpu, err)) return false;
  for (int i = 0; i < 3; i++) { g.bmin[i] = wbmin[i] <= wbmax[i] ? wbmin[i] : 0.f; g.bmax[i] = wbmin[i] <= wbmax[i] ? wbmax[i] : 0.f; }
  // bounds: the model's own, with the offset (falls back to mesh data when the model left them zero)
  float mn[3] = {m.bounds_min[0], m.bounds_min[1], m.bounds_min[2]}, mx[3] = {m.bounds_max[0], m.bounds_max[1], m.bounds_max[2]};
  memcpy(g.lmin, mn, sizeof g.lmin); memcpy(g.lmax, mx, sizeof g.lmax);  // (model space; world bounds in RecomputeBounds)
  models_.push_back(std::move(g));
  models_gen_++;
  RecomputeBounds();
  if (!m.skin.empty() && !SetSkin(models_.size() - 1, m.skin, err)) return false;
  return true;
}

bool Renderer::SetSkin(size_t model, const std::vector<assets::Matrix>& skin, std::string& err) {
  if (model >= models_.size()) { err = "no such model"; return false; }
  GpuModel& g = models_[model];
  if (skin.empty()) { DropBones(g); g.has_bones = false; g.bone_count = 0; g.skin_cpu.clear(); return true; }
  // (Session 97 review: never smaller than the model's bone list - a shorter palette would read past the buffer; pad
  // with identity. A failed resize leaves the model unskinned, not with a stale smaller view.)
  std::vector<assets::Matrix> full(skin);
  g.skin_cpu = full;  // (Session 98, replay S0: before the padding below; bank_dump reads it)
  if (full.size() < g.model_bones) full.resize(g.model_bones, assets::Identity());
  if (!UploadBones(g, full, err)) {
    if (!g.has_bones) g.bone_count = 0;
    return false;
  }
  return true;
}

// Session 98 (scenes): the union of the models' bounds. A turned model counts with its horizontal radius.
void Renderer::RecomputeBounds() {
  memset(bmin_, 0, sizeof bmin_); memset(bmax_, 0, sizeof bmax_);
  bool first = true;
  for (const auto& g : models_) {
    float mn[3], mx[3];
    const bool turned = g.rot_sin != 0.f || g.rot_cos != 1.f;
    for (int i = 0; i < 3; i++) { mn[i] = g.lmin[i]; mx[i] = g.lmax[i]; }
    if (turned) {
      const float r = std::sqrt(std::max(g.lmin[0] * g.lmin[0], g.lmax[0] * g.lmax[0]) + std::max(g.lmin[2] * g.lmin[2], g.lmax[2] * g.lmax[2]));
      mn[0] = mn[2] = -r; mx[0] = mx[2] = r;
    }
    for (int i = 0; i < 3; i++) { mn[i] += g.offset[i]; mx[i] += g.offset[i]; }
    if (first) { memcpy(bmin_, mn, sizeof bmin_); memcpy(bmax_, mx, sizeof bmax_); first = false; }
    else for (int i = 0; i < 3; i++) { bmin_[i] = std::min(bmin_[i], mn[i]); bmax_[i] = std::max(bmax_[i], mx[i]); }
  }
}

void Renderer::SetModelFeet(size_t model, float cx, float cz, float y) {
  if (model >= models_.size()) return;
  GpuModel& g = models_[model];
  g.feet_set = std::isfinite(cx) && std::isfinite(cz) && std::isfinite(y);
  g.feet_c[0] = cx; g.feet_c[1] = cz; g.feet_y = y;
}

void Renderer::SetTransform(size_t model, const float pos[3], float turn) {
  if (model >= models_.size()) return;
  memcpy(models_[model].offset, pos, sizeof models_[model].offset);
  models_[model].rot_sin = std::sin(turn); models_[model].rot_cos = std::cos(turn);
  RecomputeBounds();
}

void Renderer::RemoveModel(size_t model) {
  if (model >= models_.size()) return;
  BeforeModelsDrop();
  models_.erase(models_.begin() + (std::ptrdiff_t)model);
  models_gen_++;
  AfterModelsDrop();  // (release the GPU's references; the scene pass rebinds all)
  RecomputeBounds();
}

bool Renderer::LiveTextureSize(const std::string& name, int& w, int& h) const {
  for (const GpuModel& g : models_)
    for (size_t i = 0; i < g.tex_info.size() && i < g.tex_ok.size(); i++)
      if (g.tex_info[i].live && g.tex_info[i].name == name && g.tex_ok[i]) { w = (int)g.tex_info[i].width, h = (int)g.tex_info[i].height; return true; }
  return false;
}

int Renderer::UpdateLiveTexture(const std::string& name, int w, int h, const uint8_t* rgba) {
  int n = 0;
  for (GpuModel& g : models_)
    for (size_t i = 0; i < g.tex_info.size() && i < g.tex_ok.size(); i++) {
      const GpuTexInfo& ti = g.tex_info[i];
      if (!ti.live || ti.name != name || !g.tex_ok[i] || (int)ti.width != w || (int)ti.height != h || ti.format != assets::TexFormat::kR8G8B8A8) continue;
      if (UpdateTexture(g, i, w, h, rgba)) n++;
    }
  return n;
}

void Renderer::Clear() {
  BeforeModelsDrop();
  models_gen_++;
  models_.clear();
  ent_aim_on_ = false;  // Session 98 (stage fix): no entrance, no aim
  sprites_.clear(), DropSpriteTextures();  // Session 98 (cap / particles, review): a new scene has no entrance sprites
  AfterModelsDrop();  // (release the GPU's references to the old buffers / views; the scene pass rebinds all)
  memset(bmin_, 0, sizeof bmin_); memset(bmax_, 0, sizeof bmax_);
}

void Renderer::Bounds(float mn[3], float mx[3]) const {
  memcpy(mn, bmin_, sizeof bmin_); memcpy(mx, bmax_, sizeof bmax_);
}

bool Renderer::Shot(const Camera& cam, int w, int h, const std::string& path, std::string& err) {
  std::vector<uint8_t> px;
  if (!ShotPixels(cam, w, h, px, err)) return false;
  return WritePng(path, px.data(), w, h, err);
}

PickHit Renderer::PickAt(const Camera& cam, int w, int h, int x, int y, std::string& err) {
  if (models_.empty()) { err = "nothing is loaded"; return PickHit(); }
  if (w < 16 || h < 16 || w > 8192 || h > 8192) { err = "size out of range (16..8192)"; return PickHit(); }
  if (x < 0 || y < 0 || x >= w || y >= h) { err = "pixel outside the picture (" + std::to_string(w) + " x " + std::to_string(h) + ")"; return PickHit(); }
  return PickRead(cam, w, h, x, y, err);
}

// ---- frame constants -------------------------------------------------------------------------------------------------

// Session 98 (gen check 3): the game's crowd card LOD rule, ported from the guest code (listings out\s98\gen_check3\notes.md):
//  - sub_825132A8 (from the crowd manager sub_8250BAF0) recomputes two distances with sub_8250DE88 into 0x83762F94 (near N) and
//    0x83762F98 (step S): mode 0 (sub_8250CFA8 = 0; byte 20 of sub_827D7578() clear; sub_8250D160 false) takes N0 = N1 = 4200,
//    S0 = 1400, S1 = 6700 (table 0x8201BED0..), t = clamp((camera height - 400) / (5000 - 400)) [camera slot +148, file -Y up],
//    N = lerp(N0, N1, t), S = lerp(S0, S1, t); then u = clamp((FOV degrees [slot +128] - 10) / (30 - 10)), N *= lerp(1, a, u),
//    S *= lerp(1, 0.32, u), a = 0.9, or 0.45 when [0x836F394C] != 0.
//  - sub_825133F0 -> sub_8250E110 per stands member: clip w of (member + 120 up) through the camera matrix; w < N group 0, < N + S
//    group 1, else 2; w >= N + 3 S sets flag 0x40000000 and the card loop sub_82512F70 draws that member as a T_Audience2D card.
//  - Check against probe runs 6816 / 6819 (16 entrance frames, arena 5 and 1): the bodies / cards split at the predicted N + 3 S with
//    a = 0.45 in 15 of 16 frames (3234 predicted vs 3221 / 3248 measured, 3792 vs 3791 / 3794, 5259 vs 5258 / 5283 ...): the
//    entrance cameras run with [0x836F394C] set [C by the fit; the writer (camera code, sub_823FCFD8 0x823FD244) not traced].
static float CrowdCardDistance(float cam_height, float fovy_deg, bool entrance) {
  auto clamp01 = [](float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); };
  const float t = clamp01((cam_height - 400.f) / 4600.f), u = clamp01((fovy_deg - 10.f) / 20.f);
  const float a = entrance ? 0.45f : 0.9f;
  const float n = 4200.f * (1.f + (a - 1.f) * u);
  const float s = (1400.f + (6700.f - 1400.f) * t) * (1.f + (0.32f - 1.f) * u);
  return n + 3.f * s;
}

FrameCB Renderer::MakeFrameCB(const Camera& cam, int w, int h) const {
  float ext[3] = {bmax_[0] - bmin_[0], bmax_[1] - bmin_[1], bmax_[2] - bmin_[2]};
  float diag = std::sqrt(ext[0] * ext[0] + ext[1] * ext[1] + ext[2] * ext[2]);
  FrameCB fcb;
  fcb.viewProj = ViewProj(cam, (float)w / (float)std::max(h, 1), std::max(diag, 100.0f));
  const V3 eye = Eye(cam);
  fcb.cam[0] = eye.x; fcb.cam[1] = eye.y; fcb.cam[2] = eye.z; fcb.cam[3] = 1;
  V3 l0 = Norm(FromUpFrame(0.8f, 0.4f, 0.5f)), l1 = Norm(FromUpFrame(0.3f, -0.6f, -0.4f));
  fcb.l0[0] = l0.x; fcb.l0[1] = l0.y; fcb.l0[2] = l0.z; fcb.l0[3] = 0.65f;
  fcb.l1[0] = l1.x; fcb.l1[1] = l1.y; fcb.l1[2] = l1.z; fcb.l1[3] = 0.30f;
  fcb.amb[0] = 0.40f; fcb.amb[1] = fcb.amb[2] = fcb.amb[3] = 0;
  if (crowd_cards_ > 0) {  // Session 98 (gen check 3): the card distance (clip w) and the card gain c184.z
    const float* m = fcb.viewProj.m;
    const float ys = std::sqrt(m[1] * m[1] + m[5] * m[5] + m[9] * m[9]);  // 1 / tan(fovy / 2) (row-vector form: column 1)
    const float fovy = ys > 1e-6f ? 2.f * std::atan(1.f / ys) * 57.2957795f : 30.f;
    const bool ent = crowd_cards_ == 2 || (crowd_cards_ == 1 && entrance_);
    fcb.amb[2] = CrowdCardDistance(eye.y, fovy, ent);
    fcb.amb[3] = 2.125f;
  }
  fcb.outc[0] = 0.f; fcb.outc[1] = fcb.outc[2] = fcb.outc[3] = 0.f;  // (Session 98: the gamma curve is the post chain's last pass)
  // Session 98 (skin fix): y = the entrance variants of the wrestler shaders (four light lobes instead of the reflection cube),
  // z = 1 the old viewer wrestler recipe (A/B), w = the rim light map's value (unknown: 0 by default)
  fcb.outc[1] = entrance_ ? 1.f : 0.f; fcb.outc[2] = skin_game_ ? 0.f : 1.f; fcb.outc[3] = rim_map_;
  // Session 98: the depth haze is the combine's job now (it is the game's: one tint by depth over the whole picture)
  fcb.look[0] = look_lit_ ? 1.f : 0.f; fcb.look[1] = exposure_; fcb.look[2] = (float)debug_; fcb.look[3] = 0.f;
  // stage lightmap: g_f4LightMapParam's default (1/3500, 1/3500, 1, 1) [look spec section 2]; x2 because the map is stored at half range
  fcb.lm[0] = fcb.lm[1] = 1.f / 3500.f; fcb.lm[2] = 1.f; fcb.lm[3] = 2.f;
  return fcb;
}

bool Renderer::MakeDrawCB(const GpuModel& gm, const GpuMesh& me, bool blended, bool live_avail, DrawCB& dcb, DrawTex& dt) const {
  if (me.lid_layer && !lid_layer_on_) return false;  // Session 98 (replay): the eyelid layer the game does not draw (see the upload)
  const GpuMaterial& mat = gm.materials[me.material];
  // Session 98 (live screen): in the match look the stage's big screen shows last frame's scene colour (black on the first frame)
  dt = DrawTex();
  dt.diffuse = mat.diffuse;
  if (me.screen && live_screen_ && !entrance_ && live_avail) dt.diffuse_live = true;
  const bool has_dif = dt.diffuse_live || dt.diffuse >= 0;
  dcb = DrawCB{};
  dcb.offset[0] = gm.offset[0]; dcb.offset[1] = gm.offset[1]; dcb.offset[2] = gm.offset[2];
  dcb.offset[3] = me.skinned && gm.has_bones ? 1.f : 0.f;
  memcpy(dcb.dif, mat.diffuse_color, sizeof dcb.dif);
  // Session 98 (generalisation 2): in the entrance the game draws yBG_* surfaces that own a light map with the stage page instead
  // (switch `ent_own_lm`; see renderer.h GpuMaterial::own_lm_bg and out\s98\gen_check2\notes.md for the evidence and its limits)
  const bool own_to_page = entrance_ && fx_.ent_own_lm && mat.own_lm_bg && !me.movie && gm.stage_lm >= 0;
  dcb.flags[0] = has_dif ? 1.f : 0.f; dcb.flags[1] = mat.lightmap >= 0 && !own_to_page ? 1.f : 0.f;
  dcb.flags[2] = mat.alpha_test ? 1.f : 0.f; dcb.flags[3] = blended ? 1.f : 0.f;
  const bool stage = (own_to_page || mat.stage_lm || (entrance_ && fx_.ent_lm && mat.ent_lm && (fx_.ent_lm_bg || !mat.ent_lm_bg) && !me.movie)) && gm.stage_lm >= 0;  // (flash fix: ent_lm)
  dcb.flags2[0] = stage ? 1.f : 0.f; dcb.flags2[1] = mat.wrestler ? 1.f : 0.f;
  dcb.flags2[2] = (float)mat.spec_mask; dcb.flags2[3] = mat.crowd ? 2.f : (mat.hair ? 1.f : 0.f);
  memcpy(dcb.mat_amb, mat.amb, sizeof dcb.mat_amb); memcpy(dcb.mat_dif, mat.diffuse_color, sizeof dcb.mat_dif);
  memcpy(dcb.spec, mat.spec_col, 3 * sizeof(float)); dcb.spec[3] = mat.spec_lev;
  dcb.misc[0] = mat.spec_pow; dcb.misc[1] = gm.lm_xy[0]; dcb.misc[2] = gm.lm_xy[1]; dcb.misc[3] = gm.lm_gain;
  memcpy(dcb.limit, mat.limit, sizeof dcb.limit);
  dcb.xform[0] = gm.rot_sin; dcb.xform[1] = gm.rot_cos; dcb.xform[2] = cur_plane_y_; dcb.xform[3] = mat.ring_mat ? 1.f : 0.f;  // Session 98 (scenes): the model's turn; z = plane-shadow height; w = the ring mat (ShadowTerm)
  const assets::LightSet* lsp = light_override_ && !mat.wrestler ? &override_set_ : &gm.lights[(size_t)me.light];
  if (!set_edits_.empty() && !(light_override_ && !mat.wrestler) && (size_t)me.light + 1 < gm.lights.size()) {  // Session 98: lighting editor (not the default rig)
    const int eid = lsp->edit_id == -2 ? lsp->id : lsp->edit_id;  // (a fallback copy has -1: not edited)
    const auto ed = eid >= 0 ? set_edits_.find(eid) : set_edits_.end();
    if (ed != set_edits_.end()) lsp = &ed->second;
  }
  const assets::LightSet& ls = *lsp;
  for (int k = 0; k < 4; k++) {
    for (int c = 0; c < 3; c++) { dcb.ldir[k][c] = ls.dir[k][c]; dcb.lcol[k][c] = ls.col[k][c]; }
    dcb.ldir[k][3] = 0; dcb.lcol[k][3] = 1;
  }
  for (int c = 0; c < 3; c++) { dcb.sky[c] = ls.sky[c]; dcb.ground[c] = ls.ground[c]; }
  dcb.sky[3] = dcb.ground[3] = 1;
  for (int k = 0; k < 4; k++) dcb.lcol[k][3] = ls.wt[k];  // Session 98 (skin fix): w = the entrance lobe weight (c0..c3.w)
  {  // Session 98 (replay S5): the ambient cube's colours = the previous frame's (LightSet::pcol, when the entrance rig set them)
    float sc[3] = {0, 0, 0}, sp[3] = {0, 0, 0};
    for (int k = 0; k < 4; k++)
      for (int c = 0; c < 3; c++) {
        dcb.plcol[k][c] = ls.has_pcol ? ls.pcol[k][c] : ls.col[k][c];
        sc[c] += ls.col[k][c], sp[c] += dcb.plcol[k][c];
      }
    for (int c = 0; c < 3; c++) dcb.plscale[c] = sp[c] > 1e-6f ? sc[c] / sp[c] : 1.f;
    dcb.plscale[3] = 1.f;
    // Session 98 (match oracle): plCol[k].w = light k's cube falloff (the yPreLight pass's c16..c19.x; asset_model.h LightSet::fall)
    for (int k = 0; k < 4; k++) dcb.plcol[k][3] = cube_fall_ ? ls.fall[k] : 0.f;
  }
  for (int c = 0; c < 3; c++) { dcb.hl_dir[c] = ls.hl_dir[c]; dcb.hl_col[c] = ls.hl_col[c]; }
  dcb.hl_dir[3] = 0; dcb.hl_col[3] = 1;
  memcpy(dcb.sss1, mat.sss_col, sizeof dcb.sss1); memcpy(dcb.sss2, mat.sss_col2, sizeof dcb.sss2); memcpy(dcb.refc, mat.ref_col, sizeof dcb.refc);
  dcb.wspec[0] = mat.spec_lev; dcb.wspec[1] = mat.spec_pow; dcb.wspec[2] = mat.spec_lv_base; dcb.wspec[3] = mat.spec_pw_base;
  dcb.wmisc[0] = mat.fairy; dcb.wmisc[1] = mat.fairy_edge; dcb.wmisc[2] = mat.sss ? 1.f : 0.f;
  dcb.wmisc[3] = mat.wrestler && mat.hair && mat.normal >= 0 && skin_game_ ? 1.f : 0.f;  // Session 98 (spot / beard pass): alpha = normal map w
  // Session 98 (wrestler fix 3): the game's wrestler family maths (renderer.h GpuMaterial::wclass; the HLSL's wnrm / wdet). The colour
  // LOD bias -1 = c201.w of every skin / head / hair draw in run 6816; the detail tiling 50 = c206.x of the gear draws there (no material
  // parameter carries it). Off with the old recipe ({"cmd":"look","skin":"viewer"}) so that A/B stays as it was.
  if (mat.wrestler && skin_game_) {
    dcb.wnrm[0] = (float)mat.wclass;
    dcb.wnrm[1] = (mat.wclass == 1 || mat.wclass == 3) ? -1.f : 0.f;
    dcb.wnrm[2] = (mat.normal >= 0 && (mat.wclass == 1 || mat.wclass == 2)) ? 1.f : 0.f;
    dcb.wnrm[3] = mat.wclass == 1 ? 1.f : 0.f;
    const bool det = mat.wclass == 2 && mat.detail >= 0 && mat.detail_mask >= 0 && dcb.wnrm[2] > 0.5f;
    dcb.wdet[0] = 50.f; dcb.wdet[1] = mat.noise_dist[0]; dcb.wdet[2] = mat.noise_dist[1]; dcb.wdet[3] = det ? 1.f : 0.f;
    for (int c = 0; c < 3; c++) { dcb.wdet_a[c] = mat.noise_near[c]; dcb.wdet_b[c] = mat.noise_far[c]; }
  }
  if (!replay_.empty()) {  // Session 98 (replay S3): the game's constants for this mesh ({"cmd":"replay"}; tools\assets\replay\replay.py)
    const auto it = replay_.find({(int)(&gm - models_.data()), (int)(&me - gm.meshes.data())});
    if (it != replay_.end())
      for (const auto& sv : it->second) std::memcpy(reinterpret_cast<float*>(&dcb) + 4 * sv.first, sv.second.data(), 4 * sizeof(float));
  }
  dt.lightmap = mat.lightmap;
  dt.stage = stage ? gm.stage_lm : -1;
  dt.normal = mat.normal;
  dt.detail = mat.detail;
  dt.detail_mask = mat.detail_mask;
  return true;
}

void Renderer::BuildSceneList(const Camera& cam, std::vector<DrawItem>& out) const {
  out.clear();
  const V3 eye = Eye(cam);
  struct BlendItem { const GpuModel* model; const GpuMesh* mesh; float dist2; };
  std::vector<BlendItem> blend;
  for (auto& gm : models_)
    for (auto& me : gm.meshes) {
      const GpuMaterial& mt = gm.materials[me.material];
      if (mt.alpha_blend) {
        // (Session 98: the centre turned with the model, like the vertex stage does)
        const float cx = me.center[0] * gm.rot_cos + me.center[2] * gm.rot_sin, cz = -me.center[0] * gm.rot_sin + me.center[2] * gm.rot_cos;
        float dx = cx + gm.offset[0] - eye.x, dy = me.center[1] + gm.offset[1] - eye.y, dz = cz + gm.offset[2] - eye.z;
        blend.push_back({&gm, &me, dx * dx + dy * dy + dz * dz});
      } else if (mt.blend_inline) {  // (the stage movie surfaces: blended in this order, depth write on)
        out.push_back({&gm, &me, true, SceneRaster(mt), 1, true});
      } else {
        out.push_back({&gm, &me, false, SceneRaster(mt), 0, true});
      }
    }
  std::sort(blend.begin(), blend.end(), [](const BlendItem& a, const BlendItem& b) { return a.dist2 > b.dist2; });
  for (auto& b : blend) {
    const GpuMaterial& bm = b.model->materials[b.mesh->material];
    const uint8_t bl = bm.additive ? 2 : 1;
    if (bm.wrestler && bm.hair && skin_game_) {
      // Session 98 (spot / beard pass): the game draws each wrestler trans / hair mesh TWICE, blended, depth write as the mesh has it,
      // first with front-face CW + cull back, then CCW + cull back: one winding, then the other [C: capture rp_ringin (run 6818) events
      // 11731 / 11737, 11744 / 11750, 11757 / 11763: the same mesh, front_ccw false then true; the movie draws' front_ccw false + cull
      // back = the files' front faces = raster 1]. One draw with no culling put the cards in index order instead.
      out.push_back({b.model, b.mesh, true, 1, bl, false});
      out.push_back({b.model, b.mesh, true, 2, bl, false});
      continue;
    }
    out.push_back({b.model, b.mesh, true, SceneRaster(bm), bl, false});
  }
}

// The shadow fit (the D3D11 ShadowPasses' first half): the wrestlers' boxes -> the light matrices, the rim map's depth range.
void Renderer::PrepareShadows(const Camera& cam, int w, int h, std::vector<float>& foot_y) {
  std::vector<ShadowBox> boxes;
  foot_y.assign(models_.size(), 0.f);  // per model: its standing height (the plane pass projects onto it)
  for (size_t mi = 0; mi < models_.size(); mi++) {
    const GpuModel& g = models_[mi];
    if (!g.has_wrestler) continue;
    // the bind-pose box, turned with the model (the vertex stage's turn: x' = x cos + z sin, z' = -x sin + z cos), in world space
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (int k = 0; k < 8; k++) {
      const float px = k & 1 ? g.bmax[0] : g.bmin[0], py = k & 2 ? g.bmax[1] : g.bmin[1], pz = k & 4 ? g.bmax[2] : g.bmin[2];
      const float w3[3] = {px * g.rot_cos + pz * g.rot_sin + g.offset[0], py + g.offset[1], -px * g.rot_sin + pz * g.rot_cos + g.offset[2]};
      for (int i = 0; i < 3; i++) { mn[i] = std::min(mn[i], w3[i]); mx[i] = std::max(mx[i], w3[i]); }
    }
    ShadowBox b;
    if (g.feet_set) {  // the posed feet (the viewer's SetModelFeet): the true foot point; the box also covers a pose's reach around it
      b.foot[0] = g.feet_c[0] * g.rot_cos + g.feet_c[1] * g.rot_sin + g.offset[0];
      b.foot[2] = -g.feet_c[0] * g.rot_sin + g.feet_c[1] * g.rot_cos + g.offset[2];
      b.foot[1] = g.offset[1] + g.feet_y;
      const float reach = std::max(0.6f * (mx[1] - mn[1]), 80.f);
      mn[0] = std::min(mn[0], b.foot[0] - reach); mx[0] = std::max(mx[0], b.foot[0] + reach);
      mn[2] = std::min(mn[2], b.foot[2] - reach); mx[2] = std::max(mx[2], b.foot[2] + reach);
      mn[1] = std::min(mn[1], b.foot[1]);
    } else {
      b.foot[0] = (mn[0] + mx[0]) * 0.5f; b.foot[1] = mn[1]; b.foot[2] = (mn[2] + mx[2]) * 0.5f;
    }
    foot_y[mi] = b.foot[1];
    for (int i = 0; i < 3; i++) { b.mn[i] = mn[i] - 10.f; b.mx[i] = mx[i] + 10.f; }  // (the pad is for the light fit only; foot[] is the true height)
    boxes.push_back(b);
  }
  const bool on = fx_.shadows;
  const FrameCB fcb = MakeFrameCB(cam, w, h);
  const float fwd[3] = {fcb.viewProj.m[3], fcb.viewProj.m[7], fcb.viewProj.m[11]};  // clip w's gradient = the camera's forward axis
  shadow_set_.Prepare(boxes, on && fx_.shadow_props, on && fx_.shadow_wrestler && !boxes.empty(), on && fx_.shadow_plane && !boxes.empty(),
                      on && fx_.shadow_blobs && !boxes.empty(), fwd);
  // Session 98 (wrestler fix 2): the rim light map's depth range = the wrestlers' view-depth span (the game's c162.xy: 308 / 440 for
  // Orton at 8341, a tight fit around the casters) from the main camera's clip w over the fit boxes' corners.
  float nw = 1e30f, fw = -1e30f;
  const float* m = fcb.viewProj.m;
  for (auto& b : boxes)
    for (int k = 0; k < 8; k++) {
      const float px = k & 1 ? b.mx[0] : b.mn[0], py = k & 2 ? b.mx[1] : b.mn[1], pz = k & 4 ? b.mx[2] : b.mn[2];
      const float cw = px * m[3] + py * m[7] + pz * m[11] + m[15];
      nw = std::min(nw, cw); fw = std::max(fw, cw);
    }
  shadow_set_.SetRim(fx_.rim_light && look_lit_ && skin_game_ && !boxes.empty(), std::max(nw, 1.f), fw, w, h);
  wr_far_w_ = boxes.empty() ? 0.f : fw;  // Session 98 (wrestler fix 3): the entrance far-DoF start (MakePostParams -> PostParams::ent_far_w)
}

PostParams Renderer::MakePostParams(const FrameCB& fcb) const {
  PostParams pp;
  pp.fxaa = fx_.fxaa; pp.dof = fx_.dof; pp.dof_near = fx_.dof_near; pp.haze = haze_ && look_lit_; pp.gamma = gamma_ && look_lit_;
  pp.entrance = entrance_; pp.bloom = fx_.bloom ? bloom_ : 0.f; pp.glare = fx_.glare ? glare_ : 0.f; pp.bloom_threshold = bloom_threshold_;
  // Session 98 (wrestler fix 3): the game moves its entrance far-DoF start with each shot and keeps it past the entrant: run 6816, 11
  // probe frames, the start (9.92 / (1 - c21.y)) lies 39-565 units beyond Orton's farthest view depth (median 93; e.g. 10741: Orton
  // 751-868, start 974), so he is never blurred. The viewer's fixed start (668) blurred him in the ring shots (10741: up to ~30 %).
  // The start = the farthest corner of the wrestlers' fit boxes (PrepareShadows; the boxes reach past the body, ~the same margin).
  pp.ent_far_w = entrance_ && wr_far_w_ > 50.f ? wr_far_w_ : 0.f;
  // Session 98 (stage fix): the game's rule (probe runs 6816 + 6818, 17 entrance frames, out\s98\stage_fix\notes.md): the far-DoF start
  // = the view depth of the entrance camera track's aim point + ~155 (143-172 in 15 of them; the two exceptions are cue-20 shots whose
  // aim left the entrant), full blur 1350 view units later (exact in every entrance frame, Bryan's run 4522 too). When an entrance
  // camera track is playing, its aim point gives the start whatever the viewer's camera is; else the wrestlers' farthest depth.
  if (entrance_ && ent_aim_on_) {
    const float* m = fcb.viewProj.m;
    const float aw = ent_aim_[0] * m[3] + ent_aim_[1] * m[7] + ent_aim_[2] * m[11] + m[15];
    if (aw > 20.f) pp.ent_far_w = aw + 155.f;
  }
  for (int k = 0; k < 4; ++k) pp.film[k] = film_[k];
  if (debug_ != 0 || !look_lit_) {  // debug views and the plain look show the raw scene pass (no post chain effects)
    pp.fxaa = pp.dof = pp.haze = pp.gamma = false; pp.entrance = false;
  }
  ProjQ(fcb, pp.q, pp.nq);
  return pp;
}

// clip.z = q * clip.w - nq for a perspective matrix whatever the view rotation: column 2 = q * column 3.
// q from the largest of m3 / m7 / m11 (the view's forward vector: any one of them can be ~0), nq = q * m15 - m14
void Renderer::ProjQ(const FrameCB& fcb, float& q, float& nq) {
  const float* m = fcb.viewProj.m;
  int k = 11;
  if (std::fabs(m[3]) > std::fabs(m[k])) k = 3;
  if (std::fabs(m[7]) > std::fabs(m[k])) k = 7;
  q = std::fabs(m[k]) > 1e-9f ? m[k - 1] / m[k] : 1.f;
  nq = q * m[15] - m[14];
}

// Session 99 (particles 4): the sprite pass's constant buffer (both backends). The game's smoke pixel shader (RenderDoc
// kane_stage_frame5451: PS constants c0 = (0, 0, 1.00003, -10.0003) = its depth -> view distance, c1.x = 30) multiplies the alpha
// by min(|scene depth - particle depth|, 30) / 30 in view units; the viewer's view w comes from the depth buffer with q / nq.
void Renderer::SpriteConsts(const FrameCB& fcb, float out[20]) const {
  const float* m = fcb.viewProj.m;
  for (int k = 0; k < 4; ++k) out[4 * k + 0] = m[k], out[4 * k + 1] = m[4 + k], out[4 * k + 2] = m[8 + k], out[4 * k + 3] = m[12 + k];
  ProjQ(fcb, out[16], out[17]);
  // (Session 99 scene objects) soft particles off = a fade too steep to see (1e6 per view unit): the shader's discard stays the depth test
  out[18] = soft_range_ > 0.f ? 1.f / soft_range_ : 1e6f, out[19] = 0.f;
}

// Session 98 (cap / particles): the sprites as triangles (world position, uv, colour), back to front from the camera, in runs by texture.
void Renderer::BuildSprites(const FrameCB& fcb, size_t tex_count, std::vector<SpriteVertex>& vb, std::vector<SpriteRun>& runs) const {
  vb.clear(), runs.clear();
  vb.reserve(sprites_.size() * 6);
  // back to front from the camera (alpha blending)
  std::vector<size_t> order(sprites_.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  auto dist = [&](const Sprite& s) { float d = 0; for (int k = 0; k < 3; ++k) d += (s.c[k] - fcb.cam[k]) * (s.c[k] - fcb.cam[k]); return d; };
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return dist(sprites_[a]) > dist(sprites_[b]); });
  // the camera's right / up in viewer space: columns 0 / 1 of the view-projection (row vectors, a symmetric projection)
  const float* vm = fcb.viewProj.m;
  float cr[3] = {vm[0], vm[4], vm[8]}, cu[3] = {vm[1], vm[5], vm[9]};
  for (float* a : {cr, cu}) { const float l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); if (l > 1e-9f) for (int k = 0; k < 3; ++k) a[k] /= l; }
  for (size_t i : order) {
    Sprite s = sprites_[i];
    if (s.tex < 0 || size_t(s.tex) >= tex_count) continue;
    if (s.billboard) {
      const float x0 = s.ax[0], x1 = s.ax[1], y0 = s.ay[0], y1 = s.ay[1];
      for (int k = 0; k < 3; ++k) s.ax[k] = x0 * cr[k] + x1 * cu[k], s.ay[k] = y0 * cr[k] + y1 * cu[k];
    }
    SpriteVertex q[4];
    const float cx[4] = {-1, 1, 1, -1}, cy[4] = {1, 1, -1, -1}, u[4] = {0, 1, 1, 0}, v[4] = {0, 0, 1, 1};
    for (int k = 0; k < 4; ++k) {
      for (int j = 0; j < 3; ++j) q[k].p[j] = s.c[j] + cx[k] * s.ax[j] + cy[k] * s.ay[j];
      q[k].uv[0] = u[k], q[k].uv[1] = v[k];
      for (int j = 0; j < 4; ++j) q[k].c[j] = s.col[j];
    }
    if (runs.empty() || runs.back().tex != s.tex) runs.push_back({s.tex, vb.size(), 0});
    for (int k : {0, 1, 2, 0, 2, 3}) vb.push_back(q[k]);
    runs.back().count += 6;
  }
}

// Session 98 (flash fix): the crowd camera flashes of entrance frame flash_frame_ (crowd_flash.h): one FlashCB per flash and flash mesh.
// The game's yFlashDefault draws (vs_flash.vert / default_textured.frag): position = centre + scale x (the sprite in a camera-facing
// frame), colour = texture x vertex colour x the table colour x (1 + |facing.x|), alpha = 0.5 x texture x vertex alpha, blend SrcAlpha
// + One; the whole flash is hidden when the scene depth at its centre is nearer than the flash (the game's vertex-texture test).
void Renderer::BuildFlashes(const FrameCB& fcb, std::vector<FlashDraw>& out) {
  out.clear();
  const std::vector<assets::crowd_flash::Flash> fl = assets::crowd_flash::At(flash_frame_);
  flash_drawn_ = 0, flash_count_ = (int)fl.size();
  if (fl.empty()) return;
  const float* m = fcb.viewProj.m;
  FlashCB c = {};
  for (int k = 0; k < 4; ++k) c.col[k][0] = m[k], c.col[k][1] = m[4 + k], c.col[k][2] = m[8 + k], c.col[k][3] = m[12 + k];
  for (const GpuModel& g : models_) {
    for (const GpuMesh& me : g.flash_meshes) {
      for (const auto& f : fl) {
        // the camera-facing frame: z = from the eye to the flash, x = horizontal, y = up (the game's c76..c78 columns: x horizontal,
        // y ~ up, z ~ the view direction)
        float p[3] = {f.pos[0] + g.offset[0], f.pos[1] + g.offset[1], f.pos[2] + g.offset[2]};
        if (flash_test_ & 2) {  // (debugging: every flash 1500 units in front of the camera, spread sideways)
          const float fw[3] = {m[3], m[7], m[11]};
          const float fl2 = std::sqrt(fw[0] * fw[0] + fw[1] * fw[1] + fw[2] * fw[2]);
          const float k = float(&f - fl.data()) - 0.5f * float(fl.size());
          for (int i = 0; i < 3; ++i) p[i] = fcb.cam[i] + 1500.f * fw[i] / fl2;
          p[0] += 150.f * k * fw[2] / fl2, p[2] -= 150.f * k * fw[0] / fl2;
        }
        float z[3] = {p[0] - fcb.cam[0], p[1] - fcb.cam[1], p[2] - fcb.cam[2]};
        const float zl = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
        if (zl < 1.f) continue;
        for (float& v : z) v /= zl;
        float x[3] = {z[2], 0.f, -z[0]};  // up x z
        const float xl = std::sqrt(x[0] * x[0] + x[2] * x[2]);
        if (xl < 1e-4f) continue;
        x[0] /= xl, x[2] /= xl;
        const float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};  // z x x
        for (int k = 0; k < 3; ++k) c.ax[k] = x[k], c.ay[k] = y[k], c.az[k] = z[k], c.centre[k] = p[k];
        c.centre[3] = f.scale;
        const float lit = 1.f + std::fabs(z[0]);  // the two +-x flash lights carrying the colour, plus the colour as ambient
        for (int k = 0; k < 3; ++k) c.colour[k] = f.rgb[k] * lit;
        c.colour[3] = f.alpha;
        c.ax[3] = flash_test_ & 1 ? 1.f : 0.f;  // (no centre depth test)
        ++flash_drawn_;
        out.push_back({&g, &me, c});
      }
    }
  }
}

std::string Renderer::FlashInfo() const {
  size_t meshes = 0;
  bool tex = true;
  for (const GpuModel& g : models_)
    for (const GpuMesh& me : g.flash_meshes) ++meshes, tex = tex && g.materials[me.material].diffuse >= 0;
  return "{\"ok\":true,\"on\":" + std::string(flash_on_ ? "true" : "false") + ",\"frame\":" + std::to_string(flash_frame_) +
         ",\"meshes\":" + std::to_string(meshes) + ",\"textured\":" + (tex ? "true" : "false") + ",\"flashes\":" + std::to_string(flash_count_) +
         ",\"drawn\":" + std::to_string(flash_drawn_) + ",\"test\":" + std::to_string(flash_test_) + "}";
}

// ---- pick / highlight (Session 98 step 16) ---------------------------------------------------------------------------

bool Renderer::HighlightWanted(bool& sel, bool& hov) const {
  auto ok = [&](const PickHit& p) { return p.valid() && p.model < (int)models_.size() && p.mesh < (int)models_[(size_t)p.model].meshes.size(); };
  sel = ok(hl_sel_), hov = ok(hl_hover_) && !(hl_hover_ == hl_sel_);
  return sel || hov;
}

CompCB Renderer::MakeCompCB(int w, int h, bool sel, bool hov) const {
  CompCB cc = {};
  cc.texel[0] = 1.f / (float)w; cc.texel[1] = 1.f / (float)h;
  const float k = std::max(1.f, (float)h / 720.f);
  cc.sel[0] = 214.f / 255.f; cc.sel[1] = 22.f / 255.f; cc.sel[2] = 30.f / 255.f; cc.sel[3] = sel ? 1.f : 0.f;  // the launcher's red
  cc.hov[0] = 1.0f; cc.hov[1] = 0.42f; cc.hov[2] = 0.42f; cc.hov[3] = hov ? 1.f : 0.f;
  cc.misc[0] = 11.f * k; cc.misc[1] = 1.2f * k;
  return cc;
}

PickCB Renderer::MakePickCB(size_t mi, size_t k, bool with_id) const {
  PickCB pc = {};
  if (with_id) pc.ids[0] = ((uint32_t)mi << 20 | (uint32_t)k) + 1;
  pc.mode[0] = models_[mi].materials[(size_t)models_[mi].meshes[k].material].alpha_blend ? 1.f : 0.f;
  return pc;
}

PickHit Renderer::DecodeId(uint32_t a, uint32_t b) const {
  PickHit h;
  if (a == 0 || b == 0) return h;
  const uint32_t id = a - 1;
  h.model = (int)(id >> 20); h.mesh = (int)(id & 0xFFFFF); h.instance = (int)(b - 1);
  if (h.model >= (int)models_.size() || h.mesh >= (int)models_[(size_t)h.model].meshes.size()) return PickHit();
  return h;
}

// ---- switches ----------------------------------------------------------------------------------------------------------

bool Renderer::SetEffect(const std::string& name, bool on) {
  if (name == "shadows") fx_.shadows = on;
  else if (name == "shadow_props") fx_.shadow_props = on;
  else if (name == "shadow_wrestler") fx_.shadow_wrestler = on;
  else if (name == "shadow_plane") fx_.shadow_plane = on;
  else if (name == "shadow_blobs") fx_.shadow_blobs = on;
  else if (name == "dof") fx_.dof = on;
  else if (name == "dof_near") fx_.dof_near = on;
  else if (name == "fxaa") fx_.fxaa = on;
  else if (name == "bloom") fx_.bloom = on;
  else if (name == "glare") fx_.glare = on;
  else if (name == "live_screen") live_screen_ = on;
  else if (name == "lid_layer") lid_layer_on_ = on;  // Session 98 (replay): the eyelid layer piece (GpuMesh::lid_layer), off by default
  else if (name == "rim_light") fx_.rim_light = on;
  else if (name == "flashes") fx_.flashes = on;  // Session 98 (flash fix)
  else if (name == "ent_lm") fx_.ent_lm = on;
  else if (name == "ent_lm_bg") fx_.ent_lm_bg = on;  // (A/B: the yBG_* families of the entrance LM rule)
  else if (name == "ent_own_lm") fx_.ent_own_lm = on;  // Session 98 (generalisation 2): own-light-map yBG_* surfaces read the page
  else return false;
  return true;
}

void Renderer::SetEntrance(bool on, float bloom, float glare) {
  entrance_ = on; bloom_ = bloom; glare_ = glare;
}

// Session 98 (frame-replay S0, read-only): {"cmd":"bank_dump","path":P} -> every mesh's draw constants exactly as the scene pass would
// upload them for `cam` (the DrawCB fields by their viewer names, vectors of 4) plus the frame constants, the model placement and the bone
// palette, as JSON. Nothing is drawn and no shading state changes. tools\assets\bank_diff.py maps the fields into the game's register
// numbers (tools\assets\replay\regmap.json) and diffs them against a draw probe.
std::string Renderer::BankDump(const Camera& cam, int w, int h) {
  struct BankRec { const GpuModel* gm; const GpuMesh* me; bool blended; DrawCB dcb; };
  std::vector<BankRec> recs;
  const bool live = LiveScreenReady();
  for (auto& gm : models_)
    for (auto& me : gm.meshes) {
      const bool blended = gm.materials[me.material].alpha_blend;
      BankRec r{&gm, &me, blended, {}};
      DrawTex dt;
      if (MakeDrawCB(gm, me, blended, live, r.dcb, dt)) recs.push_back(r);
    }
  const FrameCB fcb = MakeFrameCB(cam, w, h);
  auto num = [](float v) { char b[40]; snprintf(b, sizeof b, "%.9g", std::isfinite(v) ? v : 0.f); return std::string(b); };
  auto vec = [&](const float* f, int n) { std::string s = "["; for (int i = 0; i < n; i++) s += (i ? "," : "") + num(f[i]); return s + "]"; };
  auto esc = [](const std::string& in) { std::string o; for (char c : in) { if (c == '"' || c == '\\') o += '\\'; if ((unsigned char)c >= 32) o += c; } return o; };
  std::string s = "{\"ok\":true,\"w\":" + std::to_string(w) + ",\"h\":" + std::to_string(h) + ",\"entrance\":" + (entrance_ ? "true" : "false") +
                  ",\"skin_game\":" + (skin_game_ ? "true" : "false") + ",\"frame\":{\"viewProj\":" + vec(fcb.viewProj.m, 16) + ",\"cam\":" + vec(fcb.cam, 4) +
                  ",\"look\":" + vec(fcb.look, 4) + ",\"outc\":" + vec(fcb.outc, 4) + ",\"lm\":" + vec(fcb.lm, 4) + "},\"post\":{\"film\":" + vec(film_, 4) + ",\"bloom\":" + num(bloom_) + ",\"glare\":" + num(glare_) + ",\"threshold\":" + num(bloom_threshold_) + ",\"exposure\":" + num(exposure_) + "},\"models\":[";
  for (size_t mi = 0; mi < models_.size(); mi++) {
    const GpuModel& g = models_[mi];
    s += std::string(mi ? "," : "") + "{\"label\":\"" + esc(g.label) + "\",\"offset\":" + vec(g.offset, 3) + ",\"rot_sin\":" + num(g.rot_sin) +
         ",\"rot_cos\":" + num(g.rot_cos) + ",\"has_wrestler\":" + (g.has_wrestler ? "true" : "false") + ",\"bones\":[";
    for (size_t b = 0; b < g.skin_cpu.size(); b++) s += std::string(b ? "," : "") + vec(g.skin_cpu[b].data(), 16);
    s += "]}";
  }
  s += "],\"draws\":[";
  size_t k = 0;
  for (const BankRec& r : recs) {
    const GpuModel& g = *r.gm;
    const GpuMesh& m = *r.me;
    const GpuMaterial& mt = g.materials[m.material];
    size_t mi = (size_t)(&g - models_.data()), me_i = (size_t)(&m - g.meshes.data());
    const PartMeta* pm = me_i < g.parts.size() ? &g.parts[me_i] : nullptr;
    const DrawCB& d = r.dcb;
    s += std::string(k++ ? "," : "") + "{\"model\":" + std::to_string(mi) + ",\"mesh\":" + std::to_string(me_i) + ",\"verts\":" + std::to_string(m.vertex_count) +
         ",\"index_count\":" + std::to_string(m.index_count) + ",\"skinned\":" + (m.skinned ? "true" : "false") + ",\"blended\":" + (r.blended ? "true" : "false") +
         ",\"instances\":" + std::to_string(m.placement >= 0 && (size_t)m.placement < g.placements.size() ? g.placements[m.placement].count : 1) + ",\"light\":" + std::to_string(m.light);
    if (pm) s += ",\"kind\":" + std::to_string(pm->kind) + ",\"pac\":\"" + esc(pm->pac) + "\",\"key\":" + std::to_string(pm->key) + ",\"part\":\"" + esc(pm->part) +
                 "\",\"node\":\"" + esc(pm->node) + "\",\"palette\":\"" + esc(pm->palette) + "\",\"shader\":\"" + esc(pm->shader) + "\",\"diffuse\":\"" + esc(pm->diffuse_name) + "\"";
    s += std::string(",\"mat\":{\"wrestler\":") + (mt.wrestler ? "true" : "false") + ",\"hair\":" + (mt.hair ? "true" : "false") + ",\"crowd\":" + (mt.crowd ? "true" : "false") +
         ",\"wclass\":" + std::to_string(mt.wclass) + ",\"sss\":" + (mt.sss ? "true" : "false") + ",\"spec_mask\":" + std::to_string(mt.spec_mask) +
         ",\"has_diffuse\":" + (mt.diffuse >= 0 ? "true" : "false") + ",\"has_normal\":" + (mt.normal >= 0 ? "true" : "false") + ",\"has_lightmap\":" + (mt.lightmap >= 0 ? "true" : "false") + "}";
    s += ",\"cb\":{";
#define BANKF(f, n) s += "\"" #f "\":" + vec(d.f, n) + ","
    BANKF(offset, 4); BANKF(dif, 4); BANKF(flags, 4); BANKF(flags2, 4); BANKF(mat_amb, 4); BANKF(mat_dif, 4); BANKF(spec, 4); BANKF(misc, 4);
    s += "\"ldir\":[" + vec(d.ldir[0], 4) + "," + vec(d.ldir[1], 4) + "," + vec(d.ldir[2], 4) + "," + vec(d.ldir[3], 4) + "],";
    s += "\"lcol\":[" + vec(d.lcol[0], 4) + "," + vec(d.lcol[1], 4) + "," + vec(d.lcol[2], 4) + "," + vec(d.lcol[3], 4) + "],";
    BANKF(sky, 4); BANKF(ground, 4); BANKF(limit, 4); BANKF(xform, 4); BANKF(hl_dir, 4); BANKF(hl_col, 4); BANKF(sss1, 4); BANKF(sss2, 4);
    BANKF(refc, 4); BANKF(wspec, 4); BANKF(wmisc, 4); BANKF(wnrm, 4); BANKF(wdet, 4); BANKF(wdet_a, 4);
    s += "\"wdet_b\":" + vec(d.wdet_b, 4) + ",";
    s += "\"plcol\":[" + vec(d.plcol[0], 4) + "," + vec(d.plcol[1], 4) + "," + vec(d.plcol[2], 4) + "," + vec(d.plcol[3], 4) + "],";
    s += "\"plscale\":" + vec(d.plscale, 4);
#undef BANKF
    s += "}}";
  }
  s += "],\"cb_floats\":" + std::to_string(sizeof(DrawCB) / sizeof(float)) + "}";
  return s;
}

// ---- JSON of a part (Session 98 step 16; moved from pick.cpp) ---------------------------------------------------------------

std::string PartInfoJson(const PartInfo& p) {
  auto q = [](const std::string& s) { return "\"" + jl::Esc(s) + "\""; };
  auto f = [](double v) {
    char b[48];
    snprintf(b, sizeof b, "%.2f", v);
    return std::string(b);
  };
  std::string s = "{\"ok\":true,\"valid\":" + std::string(p.valid ? "true" : "false");
  if (!p.valid) return s + "}";
  s += ",\"model\":" + std::to_string(p.hit.model) + ",\"mesh\":" + std::to_string(p.hit.mesh) + ",\"instance\":" + std::to_string(p.hit.instance);
  s += ",\"friendly\":" + q(p.friendly) + ",\"literal\":" + q(p.literal) + ",\"category\":" + q(p.category) + ",\"kind\":" + q(p.kind);
  s += ",\"source\":" + q(p.source) + ",\"mesh_note\":" + q(p.mesh_note) + ",\"shader\":" + q(p.shader);
  s += ",\"node\":" + q(p.node) + ",\"palette\":" + q(p.palette) + ",\"model_label\":" + q(p.model_label);
  s += ",\"light\":" + q(p.light) + ",\"motion\":" + q(p.motion) + ",\"placement\":" + q(p.placement);
  s += ",\"instances\":" + std::to_string(p.instances) + ",\"pos\":[" + f(p.pos[0]) + "," + f(p.pos[1]) + "," + f(p.pos[2]) + "],\"turn_deg\":" + f(p.turn_deg);
  s += ",\"textures\":[";
  for (size_t i = 0; i < p.textures.size(); i++) {
    const PartTexRef& t = p.textures[i];
    s += (i ? "," : "") + std::string("{\"role\":") + q(t.role) + ",\"name\":" + q(t.name) + ",\"slot\":" + std::to_string(t.slot) + ",\"w\":" + std::to_string(t.w) + ",\"h\":" + std::to_string(t.h) + "}";
  }
  s += "],\"movies\":[";
  for (size_t i = 0; i < p.movies.size(); i++) s += (i ? "," : "") + q(p.movies[i]);
  return s + "]}";
}

}  // namespace view
