// The Arena Viewer's "rdr" backend (Session 101, lane 6): view::Renderer implemented on OUR renderer (src\renderer) through the scene
// front-end (src\renderer_scene\). The viewer's models are handed to the front-end whole (OnModelAdded) and drawn with the game's
// hand-written effects, materials bound by name. Version 1: hidden shots of rigid models (arenas). Windowed output, picking, timing and
// the post chain are not implemented here: they return a clear error. See docs\renderer\SCENE_FRONTEND.md.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../../renderer/problem.h"
#include "../../renderer/renderer.h"
#include "../../renderer_scene/front_end.h"
#include "../../renderer_scene/shader_reload.h"  // S101 lane24
#include "renderer.h"

namespace view {
namespace {

bool EnvOn(const char* name) {  // S101 lane 15: a planted-fault switch is on only when set to a non-zero number
  const char* v = std::getenv(name);
  return v && std::atoi(v) != 0;
}

// S101 lane 34: the game's display-gamma curve (BT.709 encode of the sRGB decode: viewer_hlsl.h Bt709Encode / SrgbDecode, run 6814). The vk and d3d11
// backends apply it as the post chain's last pass (renderer.cpp: pp.gamma = gamma_ && look_lit_); the rdr backend wrote the scene pass's colour as it came,
// so the same scene read 1.4-1.8x brighter (lane 33, LIGHT_MATCH.md: Bryan entrance game 44, vk 54, rdr 79). Applied here on the read-back picture until
// the rdr post pass (src\renderer_scene\post_pass.*) owns it as its last stage.
float DisplayCurve(float v) {
  const float l = v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
  return l < 0.018f ? 4.5f * l : 1.099f * std::pow(l, 0.45f) - 0.099f;
}
// The inverse (renderer_vk.cpp Scene(): the clear colour is chosen so that it comes out as asked after the curve).
float DisplayCurveInverse(float v) {
  const float l = v < 0.081f ? v / 4.5f : std::pow((v + 0.099f) / 1.099f, 1.f / 0.45f);
  return l <= 0.0031308f ? 12.92f * l : 1.055f * std::pow(l, 1.f / 2.4f) - 0.055f;
}

// S101 lane 13: the light set a wrestler's draws use (lighting edits and the entrance rig included), rebuilt from the viewer's own draw constants.
assets::LightSet LightSetFromDraw(const DrawCB& d) {
  assets::LightSet ls;
  for (int k = 0; k < 4; ++k)
    for (int c = 0; c < 3; ++c) {
      ls.dir[k][c] = d.ldir[k][c], ls.col[k][c] = d.lcol[k][c], ls.pcol[k][c] = d.plcol[k][c];
    }
  for (int k = 0; k < 4; ++k) ls.wt[k] = d.lcol[k][3], ls.fall[k] = d.plcol[k][3];
  for (int c = 0; c < 3; ++c) ls.sky[c] = d.sky[c], ls.ground[c] = d.ground[c], ls.hl_dir[c] = d.hl_dir[c], ls.hl_col[c] = d.hl_col[c];
  ls.has_pcol = true;
  if (EnvOn("WWE_RDR_FLIP_LIGHT_DIR")) {  // S101 lane 15: the entrance selftest's planted fault (the S98 "entrant too dark again" class: the shared light-direction code)
    for (int k = 0; k < 4; ++k)
      for (int c = 0; c < 3; ++c) ls.dir[k][c] = -ls.dir[k][c];
    for (int c = 0; c < 3; ++c) ls.hl_dir[c] = -ls.hl_dir[c];
  }
  return ls;
}

struct RdrModel : GpuModelData {
  rdr_scene::FrontEnd* fe = nullptr;
  int id = -1;
  ~RdrModel() override {
    if (fe && id >= 0) fe->ReleaseModel(id);
  }
};

class RendererRdr final : public Renderer {
 public:
  ~RendererRdr() override {
    models_.clear();  // releases the front-end's models while it is alive
    rdr_scene::ShaderReload::Shutdown();  // S101 lane24: the watcher thread stops before anything it reads is torn down
    if (fe_) std::fprintf(stderr, "[rdr] validation errors: %llu\n", static_cast<unsigned long long>(fe_->validation_errors()));
  }
  const char* BackendName() const override { return "rdr"; }

  bool Init(const Surface& s, int w, int h, std::string& err) override {
    if (s.Windowed()) {
      err = "rdr backend: windowed output is not implemented yet (run with --hidden; shots and scripts work)";
      return false;
    }
    rdr::RendererOptions o;
    o.validation = std::getenv("WWE_RDR_VALIDATION") ? std::atoi(std::getenv("WWE_RDR_VALIDATION")) : 0;
    r_ = rdr::Renderer::Create(o, err);
    if (!r_) { err = "rdr backend: " + err; return false; }
    rdr_scene::FrontEndOptions fo;
    if (const char* t = std::getenv("WWE_RDR_NO_TEXTURES")) fo.textures = std::atoi(t) == 0;  // the selftest's planted fault
    if (const char* t = std::getenv("WWE_RDR_NO_PALETTE")) no_palette_ = std::atoi(t) != 0;    // S101 lane 13: the wrestler selftest's planted fault (bind pose)
    rdr_scene::ShaderReload::ConfigureFromEnvironment();  // S101 lane24: --shader_dir DIR (viewer_main sets WWE_SHADER_DIR); null in every normal run
    fe_ = std::make_unique<rdr_scene::FrontEnd>(*r_, fo);
    adapter_ = r_->device_name();
    w_ = w, h_ = h;
    return true;
  }
  void Resize(int w, int h) override { w_ = w, h_ = h; }
  void DrawFrame(const Camera&) override { std::fprintf(stderr, "[rdr] DrawFrame: windowed output is not implemented\n"); }
  bool GrabBackBuffer(std::vector<uint8_t>&, int&, int&, std::string& err) override { err = "rdr: no back buffer (hidden only)"; return false; }
  std::string Timing(const Camera&, int, int, int, std::string& err, bool) override { err = "rdr: timing is not implemented"; return ""; }
  bool DeviceLost() const override { return false; }
  uint64_t UiTexture(size_t, size_t) override { return 0; }
  int AddSpriteTexture(const assets::Texture&) override { return -1; }

  bool ShotPixels(const Camera& cam, int w, int h, std::vector<uint8_t>& px, std::string& err) override {
    if (!fe_) { err = "rdr: not initialised"; return false; }
    const FrameCB fcb = MakeFrameCB(cam, w, h);
    rdr_scene::SceneFrame f;
    f.width = uint32_t(w), f.height = uint32_t(h);
    // The viewer's matrices are D3D-style row vectors (clip = p * VP): the same layout as the scene record's Mat4. `view` holds the combined
    // matrix and `proj` the identity (SCENE_RECORD.md proposal: Camera gains a combined option).
    std::memset(&f.camera.view, 0, sizeof f.camera.view);  // (Mat4 has no initialiser: zero both before filling)
    std::memset(&f.camera.proj, 0, sizeof f.camera.proj);
    std::memcpy(f.camera.view.m, fcb.viewProj.m, sizeof f.camera.view.m);
    for (int i = 0; i < 4; ++i) f.camera.proj.m[i * 5] = 1.f;
    f.camera.eye[0] = fcb.cam[0], f.camera.eye[1] = fcb.cam[1], f.camera.eye[2] = fcb.cam[2];
    // S101 lane 34: the display curve, as vk does (WWE_RDR_NO_GAMMA=1 is the planted fault: the old washed-out picture)
    const bool curve = gamma_ && look_lit_ && debug_ == 0 && !EnvOn("WWE_RDR_NO_GAMMA");  // (review F: debug views show the raw scene, as vk's renderer.cpp:548)
    for (int k = 0; k < 3; ++k) f.clear[k] = curve ? DisplayCurveInverse(clear_rgb_[k]) : clear_rgb_[k];
    // S101 lane 15 (entrances): the entrance rule (LM variants on the ring / prop classes) and each model's light sets as the entrance rig rewrote them this frame
    // (GpuModel::lights, the lighting editor's edits applied as MakeDrawCB does). WWE_RDR_NO_ENT_LIGHTS=1: the sets the models were added with (the selftest's planted fault).
    f.entrance = entrance_ && fx_.ent_lm;
    f.entrance_bg = fx_.ent_lm_bg;
    const bool no_ent_lights = EnvOn("WWE_RDR_NO_ENT_LIGHTS");
    for (size_t mi = 0; mi < models_.size() && !no_ent_lights; ++mi) {
      const GpuModel& gm = models_[mi];
      const RdrModel* rm = static_cast<const RdrModel*>(gm.gpu.get());
      if (!rm || rm->id < 0 || gm.lights.empty()) continue;
      rdr_scene::ModelLights ml;
      ml.asset = uint32_t(rm->id);
      ml.sets = gm.lights;
      if (!EnvOn("WWE_RDR_NO_MESH_SETS"))  // (the planted fault: the belt / carried-prop meshes keep the file's set choice: the S98 dull-belt class)
        for (const GpuMesh& me : gm.meshes) ml.mesh_set.push_back(int16_t(me.light));  // (gm.meshes is in the front-end's source-mesh order: both skip the same meshes)
      if (!set_edits_.empty())
        for (size_t k = 0; k + 1 < ml.sets.size(); ++k) {
          const int eid = ml.sets[k].edit_id == -2 ? ml.sets[k].id : ml.sets[k].edit_id;
          const auto ed = eid >= 0 ? set_edits_.find(eid) : set_edits_.end();
          if (ed != set_edits_.end()) ml.sets[k] = ed->second;
        }
      f.model_lights.push_back(std::move(ml));
    }
    for (const GpuModel& gm : models_) {
      const RdrModel* rm = static_cast<const RdrModel*>(gm.gpu.get());
      if (!rm || rm->id < 0) continue;
      rdr_scene::Object o;
      o.asset = uint32_t(rm->id);
      o.klass = rdr_adapter::DrawClass::ModelPart;
      // The model's place: turned about Y (x' = x cos + z sin, z' = -x sin + z cos), then moved by its offset.
      float* m = o.world.m;
      std::memset(m, 0, sizeof o.world.m);
      m[0] = gm.rot_cos, m[2] = -gm.rot_sin, m[5] = 1.f, m[8] = gm.rot_sin, m[10] = gm.rot_cos, m[15] = 1.f;
      m[12] = gm.offset[0], m[13] = gm.offset[1], m[14] = gm.offset[2];
      // S101 lane 13 (wrestlers): the pose (the viewer's skin palette, one matrix per skeleton node), the light set of the wrestler's draws, and the
      // meshes the viewer does not draw (the eyelid layer) switched off through the mesh mask.
      if (gm.has_bones && !no_palette_ && !gm.skin_cpu.empty()) {
        o.flags |= rdr_scene::kObjSkinned;
        o.bones_first = uint32_t(f.bones.size()), o.bones_count = uint32_t(gm.skin_cpu.size());
        for (const assets::Matrix& mtx : gm.skin_cpu) {
          rdr_scene::Mat4 m4;
          std::memcpy(m4.m, mtx.data(), sizeof m4.m);
          f.bones.push_back(m4);
        }
      }
      for (size_t k = 0; k < gm.meshes.size() && k < 64; ++k)
        if (gm.meshes[k].lid_layer && !lid_layer_on_) (k < 32 ? o.mesh_mask_lo : o.mesh_mask_hi) &= ~(1u << (k & 31));
      if (gm.has_wrestler) {
        for (const GpuMesh& me : gm.meshes) {
          DrawCB dcb;
          DrawTex dt;
          if (me.material < 0 || !gm.materials[size_t(me.material)].wrestler || !MakeDrawCB(gm, me, false, false, dcb, dt)) continue;
          o.light_set = uint32_t(f.light_sets.size());
          f.light_sets.push_back(LightSetFromDraw(dcb));
          break;
        }
      }
      f.objects.push_back(o);
    }
    std::vector<uint8_t> rgba;
    fe_->ResetCoverage();
    // S101 lane 34: the post pass, with the viewer's look settings (the same PostParams / BuildPostPlan the vk backend runs on: depth of field, haze, and in
    // entrances bloom + glare). The display curve below stays the last step. WWE_RDR_NO_POST=1: off (the A/B).
    rdr_scene::PostParams rp;
    const bool post_wanted = look_lit_ && debug_ == 0 && !EnvOn("WWE_RDR_NO_POST") && (fx_.dof || (haze_ && look_lit_) || (entrance_ && (fx_.bloom || fx_.glare)));
    if (post_wanted) {
      const PostParams vp = MakePostParams(fcb);
      const std::vector<PostStep> plan = BuildPostPlan(vp, w, h);
      const PostCB* cb = nullptr;
      for (const PostStep& st : plan)
        if (!st.mark && st.ps == PostShader::kCombine) cb = &st.cb;
      rp = rdr_scene::DefaultPostParams(uint32_t(w), uint32_t(h), entrance_);
      rp.dof = vp.dof, rp.fog = vp.haze;
      rp.bloom = vp.entrance && vp.bloom != 0.f, rp.glare = vp.entrance && vp.glare != 0.f;
      if (rp.glare) rp.bloom = true;
      if (cb) {
        std::memcpy(rp.DOFParam, cb->c20, 16), std::memcpy(rp.DOFParam2, cb->c21, 16);
        std::memcpy(rp.RGB, cb->haze_col, 16);
        rp.Alpha[0] = cb->haze_p[0], rp.ZAdj[0] = cb->haze_p[1];
        rp.dof_param2_down[0] = cb->c21[0], rp.dof_param2_down[1] = cb->c21[1];
      }
      rp.BloomScale[0] = vp.bloom, rp.StarScale[0] = vp.glare;
      // review B: the bloom chain starts from the depth of field's blurred picture: with DoF off there is no bloom / glare on rdr (the shot must not fail)
      if (!rp.dof) rp.bloom = rp.glare = false;
      // review C: the near blur (the match's c21.x = 0.991855) is off in the viewer unless asked for (fx_.dof_near, like vk): x = 0 turns the near branch of the combine off
      if (!vp.dof_near) rp.DOFParam2[0] = 0.f, rp.dof_param2_down[0] = 0.f;
      rp.depth_from_viewer = true, rp.proj_q = vp.q, rp.proj_nq = vp.nq;
      f.post = &rp;
      std::fprintf(stderr, "[rdr][post] pass on: dof %d haze %d bloom %d glare %d near blur %d (c21.x %.6g) depth from viewer q %.6g nq %.6g\n", rp.dof, rp.fog, rp.bloom, rp.glare, vp.dof_near ? 1 : 0,
                   rp.DOFParam2[0], rp.proj_q, rp.proj_nq);
    }
    if (!fe_->RenderToPixels(f, rgba, err)) return false;
    if (curve) {  // S101 lane 34: the display-gamma curve, last (a 256-entry table: the colour target is RGBA8)
      uint8_t lut[256];
      for (int i = 0; i < 256; ++i) lut[i] = uint8_t(std::lround(std::min(std::max(DisplayCurve(float(i) / 255.f), 0.f), 1.f) * 255.f));
      for (size_t i = 0; i + 3 < rgba.size(); i += 4) rgba[i] = lut[rgba[i]], rgba[i + 1] = lut[rgba[i + 1]], rgba[i + 2] = lut[rgba[i + 2]];
    }
    px.resize(rgba.size());
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) px[i] = rgba[i + 2], px[i + 1] = rgba[i + 1], px[i + 2] = rgba[i], px[i + 3] = 255;
    std::fprintf(stderr, "[rdr] shot %dx%d: validation errors so far: %llu\n%s", w, h, static_cast<unsigned long long>(fe_->validation_errors()), fe_->CoverageText().c_str());
    return true;
  }

 protected:
  std::unique_ptr<GpuModelData> NewModelData() override {
    auto d = std::make_unique<RdrModel>();
    d->fe = fe_.get();
    return d;
  }
  // S101 lane 6: the whole model, once (assets by name, our loaders' host formats).
  void OnModelAdded(GpuModelData& d, const assets::Model& m) override {
    auto& rm = static_cast<RdrModel&>(d);
    if (!fe_) return;
    std::vector<std::string> warnings;
    rm.id = fe_->AddModel(m, warnings);
    for (const std::string& w : warnings) std::fprintf(stderr, "[rdr] %s\n", w.c_str());
  }
  bool UploadTexture(GpuModelData&, const assets::Texture&, std::vector<std::string>&) override { return true; }  // (done in OnModelAdded)
  bool UploadPlacement(GpuModelData&, const std::vector<assets::Matrix>&, std::string&) override { return true; }
  bool UploadMesh(GpuModelData& d, const assets::Mesh&, int& buf, std::string&) override {
    buf = nbuf_++;
    (void)d;
    return true;
  }
  // S101 lane 13: the palette itself is read from GpuModel::skin_cpu at every shot (it is the viewer's posed skin); nothing to upload here.
  bool UploadBones(GpuModel& g, const std::vector<assets::Matrix>& full, std::string&) override { g.has_bones = true, g.bone_count = full.size(); return true; }
  void DropBones(GpuModel&) override {}
  // S101 lane 15 (entrances): the tron video frames and the stage light-map page (the viewer's live textures) go up as new pictures.
  bool UpdateTexture(GpuModel& g, size_t slot, int w, int h, const uint8_t* rgba) override {
    const RdrModel* rm = static_cast<const RdrModel*>(g.gpu.get());
    if (!fe_ || !rm || rm->id < 0 || w <= 0 || h <= 0) return false;
    if (EnvOn("WWE_RDR_NO_LIVE")) return true;  // the entrance selftest's planted fault: a video surface that is never uploaded (a stale / black tron)
    return fe_->UpdateTexture(rm->id, slot, uint32_t(w), uint32_t(h), rgba);
  }
  void DropSpriteTextures() override {}
  PickHit PickRead(const Camera&, int, int, int, int, std::string& err) override { err = "rdr: picking is not implemented"; return PickHit(); }
  void BeforeModelsDrop() override { if (r_) r_->WaitIdle(); }

 private:
  std::unique_ptr<rdr::Renderer> r_;
  std::unique_ptr<rdr_scene::FrontEnd> fe_;
  int nbuf_ = 0;
  bool no_palette_ = false;
};

}  // namespace

std::unique_ptr<Renderer> MakeRendererRdr() { return std::make_unique<RendererRdr>(); }

}  // namespace view
