// The frame's stage hooks and see-through order, attached the SAME way by the live drawer and the offline rig (Session 103, lane R2; FRAME_CONTRACT.md 5 and 8).
// BuildFrame makes the objects; this attaches what the front-end's generic material path cannot draw and the order it draws the see-through list in:
//   - SeeOrder::Game (the game's see-through order, see_through_order.h);
//   - lane E's LiveMapsHook (Stage::Maps) with its MapsInput built from THIS record each frame, SceneFrame::maps = &hook.maps();
//   - lane F's LiveParticles (Stage::AfterBlended) with the record's particle tables (SceneFrame::particles). It skips a viewer-space frame today (live_particles.cpp):
//     attached and counting until lane F's fix lands, then it draws with no change here.
// The maps' casters need the CPU model of each wrestler: the live ModelCache keeps one for pac\ch\ models (rf::ModelCache::KeepWrestlerCpu, turned on here), and
// the hook's ModelProvider reads it by front-end model id. The owner (the drawer / the rig) makes ONE FrameHooks beside its FrontEnd and ModelCache and calls
// AttachFrameHooks after every drawing BuildFrame (never after a kBuildWarm one: no objects, nothing to draw). Render thread only. Header + frame_hooks.cpp.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>   // S108 PRT

#include <functional>   // S103 L9
#include <vector>
#include "front_end.h"
#include "rows_quad2d.h"   // S106 G4-2
#include "live_maps.h"
#include "live_particles.h"
#include "record_frame.h"
#include "see_through_order.h"

namespace rdr_scene {

struct FrameHooksOptions {
  bool maps = true;
  bool particles = true;
  bool particles_skip_unnamed = true;   // CAWH: a batch whose texture the namer could not name draws NOTHING (counted particles_unnamed_skipped); the game always has its texture, a white quad is never right. The offline rig sets false (its fixture flare is white by design).
  SeeOrder order = SeeOrder::Game;
  MapsFitOptions maps_fit;     // S103 E3: force the fitted shadow camera (offline only; the live drawer leaves it default)
  MapsFaults maps_faults;      // S103 E3: the maps' planted faults (rig only)
  std::function<void(MapsInput&)> maps_input_patch;   // S103 L9: the offline rig tells the plane inputs the record does not carry (a fixture, never a live path); applied after BuildMapsInput
  bool maps_input_repeatable = false;   // S103 E3: the offline rig only (FrontEnd::RenderToPixels draws a seed frame first: two Records per attach); the live drawer leaves it false
};

// S103 SCS: the game's in-scene 2D rows (Quad2D::pass == kQpSceneColour: the screen flashes, T_Rect site 0x82BC3EE8 drawn by sub_827C8130, and the lens flares, T_Sprite site 0x82BC5688 drawn by
// sub_827D5E98; docs\research\s103\RFF.md part B) drawn in OUR scene colour at Stage::AfterBlended, after the maps and BEFORE the particles (the game's category 36 runs before the particle node's 47).
// The hook only filters the record's rows and hands them to the owner's draw function (the host's q2d::Append lambda: no link dependency here); no SetTargets, the scene pass is open.
// The function must draw at `ctx.width x ctx.height` (the scene target) with the scene's depth attached (quads2d_draw.h Context::scene_depth).
// S111 ZFINAL2 (docs\research\s111\ZFINAL2.md): the hook takes EVERY scene-colour row, whatever its site. A row is scene colour when the game recorded it inside its scene pass (the op 73 .. op 74 bracket: the
// target op 73 bound, or walk_cat.h SceneByGamePass); the old two-site list (the flash and the lens only, any third site dropped) is gone: a glyph, a vertex-colour rect or a movie row the game draws there is drawn here.

class LiveScene2D final : public StageHook {
 public:
  // (builder, stage ctx, the rows in seq order, count, the record's guest frame, phase)
  // S105 A4B: the hook draws twice a frame when the record has pre-3D rows. phase kPhaseOnly = the frame's only call (the late rows, no early ones); kPhaseEarly = the early rows at BeforeOpaque (a late call may
  // follow); kPhaseLate = the late rows at AfterBlended after an early call of the same record (the owner's per-frame report goes on, it does not restart).
  enum : uint8_t { kPhaseOnly = 0, kPhaseEarly = 1, kPhaseLate = 2 };
  using DrawFn = std::function<void(rdr::FrameBuilder&, const StageCtx&, const Quad2D*, size_t, uint64_t, uint8_t)>;
  explicit LiveScene2D(DrawFn draw) : draw_(std::move(draw)) {}
  const char* Name() const override { return "scene2d"; }
  // S105 A4B: a scene-colour row the tap marked kQhPre3DCat (recorded at a frame-walk category below the first wrestler category: CAF's black quad, category 3) is EARLY (pure: the unit calls it).
  static bool IsEarly(const Quad2D& q) { return (q.have & kQhPre3DCat) != 0; }
  void SetRows(const SceneTables& rec) {
    rows_.clear();
    early_.clear();
    guest_frame_ = uint64_t(rec.header.guest_frame);
    if (rec.quad2d_holes == kQuad2DNotTaken) return;
    for (const Quad2D& q : rec.quads2d)
      if (q.pass == kQpSceneColour) (IsEarly(q) ? early_ : rows_).push_back(q);   // both in record order; S111 ZFINAL2: every scene-colour row, whatever its site (the game's scene pass is the op 73 .. op 74 bracket: walk_cat.h SceneByGamePass)
  }
  size_t rows() const { return rows_.size() + early_.size(); }
  size_t early_rows() const { return early_.size(); }
  void Record(Stage stage, const StageCtx& ctx, rdr::FrameBuilder& builder) override {
    if (!draw_) return;
    // S105 A4B: the game draws the early rows before its 3D (CAF's black quad), so they go right after our colour + depth clear; the late rows (flashes, lens flares: category 36) after the 3D, as before.
    if (stage == Stage::BeforeOpaque && !early_.empty()) draw_(builder, ctx, early_.data(), early_.size(), guest_frame_, kPhaseEarly);
    else if (stage == Stage::AfterBlended && !rows_.empty()) draw_(builder, ctx, rows_.data(), rows_.size(), guest_frame_, early_.empty() ? kPhaseOnly : kPhaseLate);
  }

 private:
  DrawFn draw_;
  std::vector<Quad2D> rows_;    // reused every frame: the rows drawn after the 3D
  std::vector<Quad2D> early_;   // S105 A4B: the rows drawn before the 3D (BeforeOpaque)
  uint64_t guest_frame_ = 0;
};

class FrameHooks {
 public:
  FrameHooks(rdr::Renderer& renderer, rf::ModelCache& cache, const FrameHooksOptions& options = FrameHooksOptions());
  const FrameHooksOptions& options() const { return options_; }
  LiveMapsHook* maps() { return maps_.get(); }
  LiveParticles* particles() { return particles_.get(); }
  // S111 TITLESMOKE: the particle drawer's frame stamp and texture lookup for `rec` (what AttachFrameHooks does for a 3D frame), for a FLAT frame whose particle groups the stream-free host draws
  // between its 2D rows (LiveSceneDraw::DrawFlatParticles). The lookup keeps copies of the record's names, as AttachFrameHooks's. No-op when the particles hook is off.
  void AttachParticles(const SceneTables& rec);
  // S103 SCS: turns the in-scene 2D hook on (the stream-free host only; the stream build never calls it: the stream draws these rows itself). Null = off.
  void SetScene2D(LiveScene2D::DrawFn draw) { scene2d_ = draw ? std::make_unique<LiveScene2D>(std::move(draw)) : nullptr; }
  bool Scene2DOn() const { return scene2d_ != nullptr; }
  ~FrameHooks();
  const MapsInput& input() const { return input_; }
  uint64_t frames() const { return frames_; }
  // "[scene][hooks] order game, frames N; <maps census>; <particles census>" (counters only; strings built here).
  std::string CensusLine() const;

 private:
  friend void AttachFrameHooks(SceneFrame&, const SceneTables&, FrameHooks&);
  FrameHooksOptions options_;
  MapsInput input_;
  std::unique_ptr<LiveMapsHook> maps_;
  std::unique_ptr<LiveParticles> particles_;
  std::unique_ptr<LiveScene2D> scene2d_;   // S103 SCS
  uint64_t frames_ = 0;
  // S103 F5: the record's particle Texture assets (pac\prtcls.pac, entry "pBBBB/EEE/1/Name") -> image, loaded through the cache's asset source on first sight, kept (a failure is kept too: no
  // reload every frame). Render thread only.
  rdr::Renderer& renderer_;
  rf::AssetSource& source_;
  std::unordered_map<std::string, rdr::ImageId> particle_images_;
  uint64_t particle_loaded_ = 0, particle_failed_ = 0;
  uint64_t particle_guest_loaded_ = 0, particle_guest_missing_ = 0;   // S108 PRT: images made from the game's own texture (guest:prtcls); guest names with no picture in this process (offline replay)
  std::unordered_set<std::string> guest_told_;   // S108 PRT: the guest keys already logged (one line each, at most 16)
  rdr::ImageId ParticleImage(const std::string& pac, const std::string& entry);   // render thread; never loads (see the .cpp)
};

// After rf::BuildFrame (a drawing build) of `rec` into `frame`: sets frame.see_order, pushes the hooks onto frame.stage_hooks, sets frame.maps and frame.particles.
// `rec` must outlive the Record / RenderToPixels of `frame` (the particle tables are read from it).
void AttachFrameHooks(SceneFrame& frame, const SceneTables& rec, FrameHooks& owned);

}  // namespace rdr_scene
