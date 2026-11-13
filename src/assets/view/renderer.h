// The Arena Viewer's renderer interface (Session 98 phone port, stage A). `Renderer` holds the scene (the uploaded models as
// backend-neutral data + an opaque per-backend handle), every look switch, and the shared frame logic (frame / draw constants,
// the scene's draw list, the shadow fit, the post plan, sprites, flashes, the highlight); a backend (renderer_d3d11.cpp,
// renderer_vk.cpp) owns the GPU objects and records the passes. Make one with MakeRenderer.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../asset_model.h"
#include "camera.h"
#include "pick.h"
#include "render_shared.h"
#include "shot.h"

namespace view {

struct GpuMesh {
  uint32_t index_count = 0, vertex_count = 0;
  int material = -1;          // index into GpuModel::materials
  float center[3] = {0, 0, 0};  // mesh centre in model space (for blend sorting)
  int placement = -1;           // index into GpuModel::placements; -1 = one identity instance
  int light = 0;                // index into GpuModel::lights (the last entry = the default rig)
  bool skinned = false;         // follows the model's bone palette
  bool caster = false;          // Session 98: a ring piece (slots 951-999): casts the props shadow map
  bool screen = false;          // Session 98: the stage's big screen (slot 800): shows the previous frame's scene colour in the match look
  bool lid_layer = false;       // Session 98 (replay): a wrestler's eyelid layer piece (renderer.cpp upload; not drawn unless lid_layer_on_)
  bool movie = false;           // Session 98 (flash fix): a stage movie surface (slots 800-809)
  int buf = -1;                 // the backend's vertex / index buffer pair of this mesh (GpuModelData)
};

struct GpuPlacement {
  uint32_t count = 0;  // instances (the backend keeps one row-vector 4x4 per instance, by the placement's index)
};

struct GpuMaterial {
  // texture slots in GpuModel's textures (only uploads that worked; -1 = none)
  int diffuse = -1, lightmap = -1;
  int normal = -1;    // Session 97: the normal / effect map (its w = specular mask on skin and gear)
  float diffuse_color[4] = {1, 1, 1, 1};
  float amb[4] = {1, 1, 1, 1}, spec_col[4] = {1, 1, 1, 1}, limit[4] = {10, 10, 10, 1};
  float spec_lev = 0, spec_pow = 300;
  float spec_lv_base = 0, spec_pw_base = 1, ref_col[4] = {0, 0, 0, 1}, fairy = 0, fairy_edge = 1;  // Session 98 (skin fix): asset_model.h Material
  float sss_col[4] = {0, 0, 0, 1}, sss_col2[4] = {0, 0, 0, 1};
  bool sss = false;                   // the yCh_skin family's glow (c155 / c158)
  int spec_mask = 0;                  // 0 none, 1 mask in the normal map's w, 2 constant (wrestler recipe)
  // Session 98 (wrestler fix 3): the game's wrestler shader family the material maps to: 0 none / arena, 1 skin family (yCh_skin*: the
  // vertex tangent frame, colour LOD bias c201.w, the vertex colour's lobe gain), 2 gear family (yCh_gear / yCh_cloth / yCh_Reflect: the
  // horizontal tangent frame, no bias), 3 hair / translucent (colour LOD bias only).
  int wclass = 0;
  int detail = -1;       // g_mNoiseTilePrm (`noise2`): the gear's tiled detail
  int detail_mask = -1;  // g_mMaskPrm (`Mask`): its z = where the detail applies
  float noise_near[3] = {0, 0, 0}, noise_far[3] = {0, 0, 0}, noise_dist[2] = {250, 1000};
  bool wrestler = false, hair = false, stage_lm = false, crowd = false;
  bool alpha_blend = false, alpha_test = false, additive = false;
  bool cull_back = false;  // Session 98 (stage fix): asset_model.h Material::cull_back
  bool blend_inline = false;  // Session 98 (spot / beard pass): asset_model.h Material::blend_inline
  // Session 98 (flash fix): the game's entrance draws this material with its lightmap ("LM") shader variant, i.e. it samples the live
  // stage light map page (the entrance light pools) [C: probe run 6816, the same draws in entrance frame 12241 vs match frame 12841:
  // yProp_Default -> yProp_DefaultLM (the ring skirts, the announce desk), yProp_PSM -> yProp_PSMLM (the canvas), yBG_Default ->
  // yBG_DefaultLM, yBG_UVScroll -> yBG_UVScrollLM; yProp_BumpMap stays]. Set for those shaders when the material has no own lightmap.
  bool ent_lm = false, ent_lm_bg = false;  // (ent_lm_bg: a yBG_* family)
  bool ring_mat = false;  // Session 98 (shadow / match light): yProp_PSM, the ring mat (receives only the plane shadow)
  bool own_lm_bg = false;  // Session 98 (generalisation 2): a yBG_* arena surface with its OWN light map (entrance switch `ent_own_lm`)
};

// Session 98 (Arena Viewer UI): what the UI shows about an uploaded texture (parallel to the model's textures).
struct GpuTexInfo {
  std::string name;
  uint32_t width = 0, height = 0, mips = 1;
  assets::TexFormat format = assets::TexFormat::kUnknown;
  bool live = false;  // Session 98 (tron video): UpdateLiveTexture may rewrite it
};

// A backend's GPU objects of one model (buffers, textures, bone palette). Opaque outside the backend.
struct GpuModelData {
  virtual ~GpuModelData() = default;
};

struct GpuModel {
  std::string label;                 // Session 98: the model's label ("wrestler 107 attire 0", "arena 5")
  std::vector<GpuTexInfo> tex_info;  // Session 98: one per texture of the model
  std::vector<uint8_t> tex_ok;       // parallel to tex_info: 1 = uploaded (0 = the upload failed)
  float offset[3] = {0, 0, 0};
  float rot_sin = 0.f, rot_cos = 1.f;  // Session 98: the model's turn about Y (SetTransform)
  float lmin[3] = {0, 0, 0}, lmax[3] = {0, 0, 0};  // model-space bounds
  std::vector<GpuMesh> meshes;
  std::vector<GpuMaterial> materials;
  std::vector<GpuPlacement> placements;
  std::vector<assets::LightSet> lights;        // Session 97: the model's light sets + the default rig (last)
  int stage_lm = -1;                           // the stage-wide lightmap (a texture slot; -1 none)
  bool has_bones = false;                      // a skinning palette is uploaded (else identity)
  size_t bone_count = 0;
  float lm_xy[2] = {1.f / 3500.f, 1.f / 3500.f}, lm_gain = 2.5f;  // g_f4LightMapParam x, y / z
  size_t model_bones = 0;  // the model's bone count (a palette is padded to it)
  std::vector<assets::Matrix> skin_cpu;  // Session 98 (replay S0): the palette as last set (read-only copy for bank_dump)
  bool has_wrestler = false;  // Session 98: any yCh_* mesh; bmin / bmax = the bounds of those meshes (model space, no offset)
  float bmin[3] = {0, 0, 0}, bmax[3] = {0, 0, 0};
  bool feet_set = false;       // Session 98: the posed feet (SetModelFeet): centre x, z and the lowest foot height, model space
  float feet_c[2] = {0, 0}, feet_y = 0;
  // Session 98 (step 16, click to inspect): where each mesh came from (parallel to `meshes`) and the placed copies' positions.
  std::vector<PartMeta> parts;
  std::vector<PlacementMeta> pmeta;
  std::vector<GpuMesh> flash_meshes;  // Session 98 (flash fix): the crowd camera-flash sprite (arena entry 31000), drawn by the flash pass only
  std::unique_ptr<GpuModelData> gpu;  // the backend's objects
};

// Where a windowed renderer draws. Neither set = hidden (no window / swap chain; shots only).
struct Surface {
  void* hwnd = nullptr;  // Win32 window (D3D11)
  // Vulkan: the instance extensions the window needs and a function that makes its VkSurfaceKHR (instance -> surface handle).
  std::vector<std::string> vk_instance_exts;
  std::function<bool(void* vk_instance, uint64_t* vk_surface)> vk_create_surface;
  bool Windowed() const { return hwnd || vk_create_surface; }
};

class Renderer {
 public:
  virtual ~Renderer() = default;
  virtual const char* BackendName() const = 0;  // "d3d11" / "vk"
  // Without a window (Surface{}): no swap chain (hidden mode; shots only).
  virtual bool Init(const Surface& s, int w, int h, std::string& err) = 0;
  virtual void Resize(int w, int h) = 0;
  // Upload and keep a model at `offset`. Warnings (texture upload problems) are appended to `warnings`.
  bool AddModel(const assets::Model& m, const float offset[3], std::vector<std::string>& warnings, std::string& err);
  void Clear();
  // Session 98 (scenes): place model `model` at `pos` turned by `turn` radians about Y (x' = x cos + z sin, z' = -x sin + z cos); remove it.
  void SetTransform(size_t model, const float pos[3], float turn);
  void RemoveModel(size_t model);
  // Session 97: replace model `model`'s skinning palette (one matrix per bone; empty = identity / the stored pose).
  bool SetSkin(size_t model, const std::vector<assets::Matrix>& skin, std::string& err);
  // Draw into the window's back buffer and present.
  virtual void DrawFrame(const Camera& cam) = 0;
  // Render offscreen at w x h and write a PNG.
  bool Shot(const Camera& cam, int w, int h, const std::string& path, std::string& err);
  // Session 98 (offline render): the same frame as B8G8R8A8 pixels (w*h*4, alpha 255), no file.
  virtual bool ShotPixels(const Camera& cam, int w, int h, std::vector<uint8_t>& px, std::string& err) = 0;

  // Session 97: the look. lit = the scene-shader lighting of look.md; false = the old plain look. exposure scales the output.
  void SetLook(bool lit, float exposure) { look_lit_ = lit; exposure_ = exposure; }
  bool LookLit() const { return look_lit_; }
  void SetDebug(int d) { debug_ = d; }
  // Session 98 (skin fix): the wrestler shading: true = the game's rule (probe run 6816; out\s98\skin_fix\_model.py), false = the old viewer recipe (A/B);
  // rim_map = the value the game reads from its rim light map (a runtime pass the viewer lacks; 0 = the edge highlight off).
  void SetSkinRule(bool game, float rim_map) { skin_game_ = game; rim_map_ = rim_map; }
  // Session 98 (gen check 3): the game's crowd card LOD (CrowdCardDistance). 0 off (every stands member a body), 1 auto
  // (the entrance factor while an entrance plays, else the match factor), 2 entrance factor, 3 match factor.
  void SetCrowdCards(int mode) { crowd_cards_ = mode; }
  int CrowdCards() const { return crowd_cards_; }
  bool SkinGame() const { return skin_game_; }
  // Session 98 (match oracle): the ambient cube's per-light falloff from the light set (LightSet::fall); false = 0 for every light (A/B).
  void SetCubeFall(bool on) { cube_fall_ = on; }
  float RimMap() const { return rim_map_; }
  // Session 97: the match combine's depth haze on arena surfaces (look_live.md section 2); on by default.
  void SetHaze(bool on) { haze_ = on; }
  void SetGamma(bool on) { gamma_ = on; }  // Session 97: the game's display-gamma curve as the last step
  // Session 97: A/B tool: use one light set (numbers given by the caller) for every non-wrestler mesh instead of the file's sets.
  void SetLightOverride(bool on, const assets::LightSet& s) { light_override_ = on; override_set_ = s; }
  bool LightOverride() const { return light_override_; }
  // Session 98 (lighting editor): a light set by id replaces every mesh's set of that id (not the default rig); null clears one, no-arg clears all.
  void SetSetEdit(int id, const assets::LightSet* s) { if (s) set_edits_[id] = *s; else set_edits_.erase(id); }
  void ClearSetEdits() { set_edits_.clear(); }
  float Exposure() const { return exposure_; }
  // Session 98 (Superstar Importer): the picture's background colour where nothing is drawn (display values 0-1, after the display
  // curve). The importer's portrait renders the figure over black and over white and takes the alpha from the difference.
  void SetClearColour(float r, float g, float b) { clear_rgb_[0] = r, clear_rgb_[1] = g, clear_rgb_[2] = b; }
  void ResetClearColour() { SetClearColour(0.12f, 0.14f, 0.18f); }

  // Session 98 (Arena Viewer UI): read-only access for the UI, and a callback drawn into the back buffer after the scene and
  // before Present (windowed only; DrawFrame binds the back buffer and the full viewport first).
  void SetOverlay(std::function<void()> f) { overlay_ = std::move(f); }
  const std::vector<GpuModel>& Models() const { return models_; }
  // Session 98 (entrance): the light sets of model `model` (the entrance rig rewrites colours / directions every frame); null if none.
  std::vector<assets::LightSet>* MutableLights(size_t model) { return model < models_.size() ? &models_[model].lights : nullptr; }
  // Session 98 (sweep look): point a model's meshes at light set `with_normal` when their material has a normal map, else `other`
  // (the entrant's belt: its bump-mapped pieces take the entrant's lights, the rest keep theirs).
  void SetMeshLightsByNormal(size_t model, int with_normal, int other) {
    if (model >= models_.size()) return;
    GpuModel& gm = models_[model];
    for (GpuMesh& me : gm.meshes) {
      const bool nm = me.material >= 0 && size_t(me.material) < gm.materials.size() && gm.materials[size_t(me.material)].normal >= 0;
      me.light = nm ? with_normal : other;
    }
  }
  int Width() const { return w_; }
  int Height() const { return h_; }
  // Copy the current back buffer to CPU memory (B8G8R8A8, alpha forced to 255). Call from the overlay, after the UI was drawn.
  virtual bool GrabBackBuffer(std::vector<uint8_t>& bgra, int& w, int& h, std::string& err) = 0;
  // Session 98 (plan step 10): the game's frame recipe. Switches: shadows (+ shadow_props / shadow_wrestler / shadow_plane /
  // shadow_blobs), dof, fxaa, bloom, glare (bloom + glare only run with the entrance look); haze / gamma keep their setters above.
  bool SetEffect(const std::string& name, bool on);
  // The entrance look (the `stage` command's wrestler): the entrance combine's DoF constants + bloom / glare strengths.
  void SetEntrance(bool on, float bloom, float glare);
  float film_[4] = {0, 0, 0, 0};
  void SetBloomThreshold(float t) { bloom_threshold_ = t; }
  // Session 98 (stage fix): {"cmd":"look","cull":0|1|2} two-sided / the game's rule / the other winding (A/B)
  void SetCullMode(int m) { cull_mode_ = m; }
  // Session 98 (stage fix): the entrance camera track's aim point (viewer space) for the far-DoF focus; on = false: none (free roam)
  void SetEntranceAim(bool on, const float p[3]) { ent_aim_on_ = on; for (int k = 0; k < 3; ++k) ent_aim_[k] = on ? p[k] : 0.f; }
  // Session 98 (flash fix): the crowd camera flashes (crowd_flash.h) of entrance guest frame `frame`; on = false: none (no entrance).
  void SetFlashFrame(bool on, int64_t frame) { flash_on_ = on; flash_frame_ = frame; }
  void SetFlashTest(int t) { flash_test_ = t; }  // 1 = no centre depth test (debugging / A/B)
  std::string FlashInfo() const;                 // {"ok":true,"on":..,"frame":..,"meshes":..,"drawn":..} (the last flash pass)
  void SetFilm(const float rgba[4]) { for (int k = 0; k < 4; ++k) film_[k] = rgba ? rgba[k] : 0.f; }  // Session 98 (arena fix 2): the entrance film filter
  // Session 98 (cap / particles): entrance particle sprites (assets\particles.h), drawn after the scene and before the post chain (the
  // game's order at 6824 f27170): alpha-blended, depth-tested against the scene, no depth write. Textures are uploaded once per entrance.
  struct Sprite { float c[3], ax[3], ay[3], col[4]; int tex = 0; bool billboard = false; };  // billboard: ax / ay in the camera's right / up basis
  virtual int AddSpriteTexture(const assets::Texture& t) = 0;  // slot, -1 on failure
  void ClearSprites() { sprites_.clear(); DropSpriteTextures(); }
  void SetSprites(std::vector<Sprite> s) { sprites_ = std::move(s); }
  // Session 99 (scene objects): the soft-particle switch of the entrance's GMEV type-11 object (particles.h SoftSwitch): range > 0 =
  // the depth fade over that many view units (Kane 107: 30, the T_SoftParticle technique); <= 0 = off (T_Particle: the plain depth test).
  void SetSoftParticles(float range) { soft_range_ = range; }
  float SoftParticles() const { return soft_range_; }
  size_t SpriteCount() const { return sprites_.size(); }
  // The viewer tells where an actor's posed feet are (model space: centre x, z, lowest foot y): the shadows stand on them.
  void SetModelFeet(size_t model, float cx, float cz, float y);
  // Render `frames` offscreen frames at w x h and return the mean GPU ms per pass as JSON ("" + err on failure).
  virtual std::string Timing(const Camera& cam, int w, int h, int frames, std::string& err, bool with_pick = false) = 0;

  // Session 98 (replay S0, read-only): every mesh's draw constants as the scene pass would upload them for `cam`, as JSON (no shading change).
  std::string BankDump(const Camera& cam, int w, int h);
  // Session 98 (replay S3): per-mesh DrawCB overrides (the game's constants for that draw, float4 slots by CbSlot); empty = none.
  static int CbSlot(const std::string& field, int idx);
  void SetReplaySlot(int model, int mesh, int slot, const float v[4]) {
    replay_[{model, mesh}].push_back({slot, {v[0], v[1], v[2], v[3]}});
  }
  void ClearReplay() { replay_.clear(); }
  size_t ReplayCount() const { return replay_.size(); }

  // Session 98 (step 16, click to inspect). PickAt renders the ID pass once at w x h (works hidden) and returns the mesh / instance at
  // pixel (x, y) (invalid = background). RequestHover asks the next windowed frames for the id under the cursor (a 1x1 readback, a frame
  // or two late; throttle the calls); HoverHit is the newest answer. SetHighlight sets the hovered / selected instance whose outline + glow
  // are composited after the post chain (invalid = none). Nothing here costs anything until one of them is used.
  PickHit PickAt(const Camera& cam, int w, int h, int x, int y, std::string& err);
  void RequestHover(int x, int y) { hover_req_ = true, hover_x_ = x, hover_y_ = y; }
  void ClearHover() { hover_req_ = false, hover_hit_ = PickHit(); DropPendingReadbacks(); }
  PickHit HoverHit() const { return hover_hit_; }
  void SetHighlight(const PickHit& hover, const PickHit& sel) { hl_hover_ = hover, hl_sel_ = sel; }

  // Union of all models' bounds (with offsets); valid when HasModel().
  bool HasModel() const { return !models_.empty(); }
  void Bounds(float mn[3], float mx[3]) const;
  const std::string& AdapterName() const { return adapter_; }
  size_t ModelCount() const { return models_.size(); }
  // Session 98 (tron video): live textures (assets::Texture::live) are rewritten in place. LiveTextureSize: the size of the
  // first live texture called `name` (false = none); UpdateLiveTexture: RGBA8 of exactly that size into every model's texture of that name
  // (returns how many). ModelsGen() changes whenever models are added / removed, so a caller knows to re-send its picture.
  bool LiveTextureSize(const std::string& name, int& w, int& h) const;
  int UpdateLiveTexture(const std::string& name, int w, int h, const uint8_t* rgba);
  uint32_t ModelsGen() const { return models_gen_; }
  // True once the GPU device was removed / reset (driver crash, TDR): nothing works after that.
  virtual bool DeviceLost() const = 0;
  // The UI's handle (ImTextureID) of texture `tex` of model `model`; 0 = none / failed upload.
  virtual uint64_t UiTexture(size_t model, size_t tex) = 0;

 protected:
  // ---- backend hooks of the shared model code ----
  virtual std::unique_ptr<GpuModelData> NewModelData() = 0;
  // S101 lane 6: the whole model as the loader made it, right after NewModelData (a backend that draws from the model itself, "rdr", takes it here).
  virtual void OnModelAdded(GpuModelData& d, const assets::Model& m) { (void)d, (void)m; }
  // Upload texture `t` as the next texture slot of `d` (a failed upload still takes its slot). True = uploaded.
  virtual bool UploadTexture(GpuModelData& d, const assets::Texture& t, std::vector<std::string>& warnings) = 0;
  virtual bool UploadPlacement(GpuModelData& d, const std::vector<assets::Matrix>& transforms, std::string& err) = 0;  // the next placement slot
  virtual bool UploadMesh(GpuModelData& d, const assets::Mesh& m, int& buf, std::string& err) = 0;
  virtual bool FinishModel(GpuModelData& d, std::string& err) { (void)d, (void)err; return true; }  // after the last upload of a model
  // The palette `full` (padded to the model's bone count): make or rewrite the model's bone buffer. False = the model has none now.
  virtual bool UploadBones(GpuModel& g, const std::vector<assets::Matrix>& full, std::string& err) = 0;
  virtual void DropBones(GpuModel& g) = 0;
  virtual bool UpdateTexture(GpuModel& g, size_t slot, int w, int h, const uint8_t* rgba) = 0;  // an R8G8B8A8 live texture
  virtual void DropSpriteTextures() = 0;
  virtual void DropPendingReadbacks() {}
  // PickAt after its checks: the ID pass at w x h and the id at (x, y).
  virtual PickHit PickRead(const Camera& cam, int w, int h, int x, int y, std::string& err) = 0;
  virtual bool LiveScreenReady() const { return false; }  // the big screen's copy of last frame's scene colour exists
  virtual void BeforeModelsDrop() {}  // before models are destroyed (Clear / RemoveModel): the GPU must be done with them
  virtual void AfterModelsDrop() {}   // after: release any GPU references the backend still holds

  // ---- shared frame logic ----
  FrameCB MakeFrameCB(const Camera& cam, int w, int h) const;
  // The textures one scene draw reads (texture slots of the model; -1 none). diffuse_live: the big screen shows the live copy instead.
  struct DrawTex { int diffuse = -1, lightmap = -1, stage = -1, normal = -1, detail = -1, detail_mask = -1; bool diffuse_live = false; };
  // The draw constants of one mesh, as the scene pass uploads them. False = the mesh is not drawn (the eyelid layer).
  // live_avail: a live screen copy (last frame's scene colour) exists.
  bool MakeDrawCB(const GpuModel& gm, const GpuMesh& me, bool blended, bool live_avail, DrawCB& dcb, DrawTex& dt) const;
  // The scene pass in order: opaque (and inline-blended) meshes in model order, then the blended ones far to near.
  // raster: 0 two-sided, 1 cull back (the game's rule), 2 cull front; blend: 0 off, 1 alpha, 2 additive.
  struct DrawItem { const GpuModel* gm; const GpuMesh* me; bool blended; uint8_t raster, blend; bool depth_write; };
  void BuildSceneList(const Camera& cam, std::vector<DrawItem>& out) const;
  uint8_t SceneRaster(const GpuMaterial& m) const { return m.cull_back && cull_mode_ == 1 ? 1 : m.cull_back && cull_mode_ == 2 ? 2 : 0; }
  // The shadow casters: 0 props (ring pieces), 1 wrestlers (opaque + cut-outs).
  static bool IsCaster(const GpuMaterial& mt, const GpuMesh& me, int kind) {
    if (mt.alpha_blend || mt.crowd) return false;
    return kind == 0 ? (me.caster && !mt.wrestler) : mt.wrestler;
  }
  // Fits the shadow set for this frame (shadow_set_, wr_far_w_); foot_y = each model's standing height (the plane pass).
  void PrepareShadows(const Camera& cam, int w, int h, std::vector<float>& foot_y);
  PostParams MakePostParams(const FrameCB& fcb) const;
  struct SpriteVertex { float p[3], uv[2], c[4]; };
  struct SpriteRun { int tex; size_t first, count; };
  void BuildSprites(const FrameCB& fcb, size_t tex_count, std::vector<SpriteVertex>& vb, std::vector<SpriteRun>& runs) const;
  // Session 99 (particles 4): the sprite pass's constants (viewer_hlsl.h kSprite SpriteCB): vpc[4] = the view-projection columns,
  // soft = (q, nq, 1 / the soft-particle range 30, 0) for the scene depth -> view w and the game's depth fade.
  static constexpr float kSoftRange = 30.f;
  static void ProjQ(const FrameCB& fcb, float& q, float& nq);
  void SpriteConsts(const FrameCB& fcb, float out[20]) const;  // (Session 99 scene objects: reads soft_range_)
  struct FlashDraw { const GpuModel* gm; const GpuMesh* me; FlashCB cb; };
  bool FlashesWanted() const { return flash_on_ && fx_.flashes && entrance_ && look_lit_ && debug_ == 0; }
  void BuildFlashes(const FrameCB& fcb, std::vector<FlashDraw>& out);  // (updates FlashInfo's counts)
  // The highlight: which of the selected / hovered instances to draw, and the composite's constants.
  bool HighlightWanted(bool& sel, bool& hov) const;
  CompCB MakeCompCB(int w, int h, bool sel, bool hov) const;
  PickCB MakePickCB(size_t mi, size_t k, bool with_id) const;
  bool PickSkip(const GpuModel& gm, size_t k) const { return k < gm.parts.size() && gm.parts[k].additive; }  // (glows, beams and fog are not parts you click)
  PickHit DecodeId(uint32_t a, uint32_t b) const;
  bool HasScreen() const { for (auto& g : models_) for (auto& me : g.meshes) if (me.screen) return true; return false; }
  void RecomputeBounds();

  // rim_light (Session 98, wrestler fix 2): the game's runtime rim light map (shadow.h SetRim); off = the constant rim_map value
  struct Effects { bool shadows = true, shadow_props = true, shadow_wrestler = true, shadow_plane = true, shadow_blobs = true, dof = true, dof_near = false, fxaa = true, bloom = true, glare = true, rim_light = true;
                   bool flashes = true, ent_lm = true, ent_lm_bg = false, ent_own_lm = true; };  // ent_lm_bg off: flash_fix ab3 (the yBG_* part darkens the stage floor 0.87 -> 0.69 at 8941 while fixing the stands 1.28 -> 1.01) [U]  // Session 98 (flash fix): the crowd flashes; the entrance's LM shader variants
  Effects fx_;
  bool entrance_ = false, live_screen_ = true, lid_layer_on_ = false;
  bool flash_on_ = false;     // Session 98 (flash fix): SetFlashFrame
  int64_t flash_frame_ = 0;
  int flash_test_ = 0, flash_drawn_ = -1, flash_count_ = 0;
  std::vector<Sprite> sprites_;  // Session 98 (cap / particles)
  float soft_range_ = kSoftRange;  // Session 99 (scene objects): SetSoftParticles
  bool ent_aim_on_ = false;                   // Session 98 (stage fix): the entrance camera track's aim point (SetEntranceAim)
  float ent_aim_[3] = {0, 0, 0};
  float wr_far_w_ = 0.f;                      // Session 98 (wrestler fix 3): the wrestlers' farthest view depth (entrance far-DoF start)
  float cur_plane_y_ = 0.f;                    // the standing height the plane-shadow pass is drawing (DrawCB xform.z)
  float bloom_ = 0.4f, glare_ = 0.f, bloom_threshold_ = 0.45f;  // (glare 1.6 is Brock's only: viewer_main EntranceStrengths)
  ShadowSetup shadow_set_;
  bool hover_req_ = false, timing_pick_ = false;
  int hover_x_ = 0, hover_y_ = 0;
  std::map<std::pair<int, int>, std::vector<std::pair<int, std::array<float, 4>>>> replay_;  // Session 98 (replay S3): (model, mesh) -> slots
  PickHit hover_hit_, hl_hover_, hl_sel_;
  int cull_mode_ = 1;  // Session 98 (stage fix): see SetCullMode
  std::vector<GpuModel> models_;
  uint32_t models_gen_ = 0;
  std::string adapter_;
  int w_ = 0, h_ = 0;
  bool look_lit_ = true;
  bool skin_game_ = true;
  bool cube_fall_ = true;
  float rim_map_ = 0.f;
  int crowd_cards_ = 1;
  int debug_ = 0;
  bool haze_ = true, light_override_ = false, gamma_ = true;
  assets::LightSet override_set_;
  std::map<int, assets::LightSet> set_edits_;  // Session 98 (lighting editor)
  float exposure_ = 1.0f;
  float clear_rgb_[3] = {0.12f, 0.14f, 0.18f};  // Session 98 (importer): SetClearColour
  float bmin_[3] = {0, 0, 0}, bmax_[3] = {0, 0, 0};
  std::function<void()> overlay_;  // Session 98
};

// "d3d11" (Windows) or "vk". Null + err when the backend is not built in.
std::unique_ptr<Renderer> MakeRenderer(const std::string& backend, std::string& err);

}  // namespace view
