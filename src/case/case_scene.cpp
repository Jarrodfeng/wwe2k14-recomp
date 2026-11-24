// The case ring's scene sink (Session 101, lane 26). See case_scene.h.
#include "case_scene.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../renderer_scene/record_io.h"
#include "../renderer_scene/record_all.h"   // S106 G4-2
#include "case_hook.h"

namespace rdr_case {
namespace {

std::atomic<uint64_t> g_held{0};

class CaseSceneSink : public rdr_scene::live::Sink {
 public:
  void OnRecord(const rdr_scene::live::FrameInfo& f, const rdr_scene::SceneTables& t) override {
    OnRecordShared(f, std::make_shared<const rdr_scene::SceneTables>(t));
  }
  void OnRecordShared(const rdr_scene::live::FrameInfo& f, std::shared_ptr<const rdr_scene::SceneTables> t) override {
    if (!case_hook::Enabled()) return;
    const rdr_scene::SceneTables& r = *t;
    // an estimate of the record's size (the cap only): the tables' element bytes
    const uint64_t estimate = sizeof(rdr_scene::SceneHeader) + (r.strings.size() > r.SeedPrefixBytes() ? r.strings.size() - r.SeedPrefixBytes() : r.strings.size()) +   /* S102 T11b F2: the file is written without the pool prefix */ r.assets.size() * sizeof(rdr_scene::AssetRef) +
                              r.objects.size() * sizeof(rdr_scene::Object) + r.bones.size() * sizeof(rdr_scene::Mat4) +
                              r.params.size() * sizeof(rdr_scene::ParamValue) + r.lights.size() * sizeof(rdr_scene::Light) +
                              r.sprites.size() * sizeof(rdr_scene::Sprite) + r.texts.size() * sizeof(rdr_scene::Text) +
                              r.movies.size() * sizeof(rdr_scene::Movie) + r.targets.size() * sizeof(rdr_scene::Target) +
                              // lane 226 N1: the live tables (about 80 KB a frame with caster D3 on: 13 plane calls x 96 matrices alone are 80 KB)
                              r.engine_objects.size() * 4 + r.light_states.size() * sizeof(rdr_scene::LightState) + r.light_refs.size() * sizeof(rdr_scene::LightRef) +
                              r.circle_shadows.size() * sizeof(rdr_scene::CircleShadow) + r.circle_mesh.size() * sizeof(rdr_scene::CircleMesh) + r.plane_frames.size() * sizeof(rdr_scene::PlaneFrame) +
                              r.plane_draws.size() * sizeof(rdr_scene::PlaneDraw) + r.plane_eyes.size() * sizeof(rdr_scene::PlaneEyes) + r.part_clocks.size() * sizeof(rdr_scene::PartClock) +  /* S103 E5 */ r.props_light.size() * sizeof(rdr_scene::PropsLight) + r.bone_tags.size() * sizeof(rdr_scene::BoneTag) +  /* S103 W2B */ r.change_colours.size() * sizeof(rdr_scene::ChangeColourRow) +  /* S108 THR2 */  /* S103 KS1 */ r.arena_worlds.size() * sizeof(rdr_scene::ArenaWorld) + r.ring_rows.size() * sizeof(rdr_scene::RingRows) +
                              r.ring_nodes.size() * sizeof(rdr_scene::RingNodeRows) + r.weapon_calls.size() * sizeof(rdr_scene::weapons::WeaponCall) + r.weapon_nodes.size() * sizeof(rdr_scene::RingNodeRows) +  /* S103 PW-2 */ r.caster_calls.size() * sizeof(rdr_scene::CasterCall) + r.caster_nodes.size() * 4 +
                              r.caster_mats.size() * sizeof(rdr_scene::Mat4) + r.caster_states.size() * sizeof(rdr_scene::LightState) + r.caster_extras.size() * sizeof(rdr_scene::CasterExtra) + r.sphere_occ.size() * sizeof(rdr_scene::SphereOccRow) +
                              // S102 T19 F3: the crowd run markers (tools\lint\t19_lint.py: this names every SceneTables vector)
                              r.crowd_runs.size() * sizeof(rdr_scene::CrowdRun) + r.crowd_members.size() * sizeof(rdr_scene::CrowdMember) +
                              r.model_lights.size() * sizeof(rdr_scene::ModelLightRow) + r.stage_fx.size() * sizeof(rdr_scene::StageFxRow) + r.stage_look.size() * sizeof(rdr_scene::StageLookRow) +  // S103 C, S103 L
                              r.crowd_props.size() * sizeof(rdr_scene::CrowdPropRow) + r.crowd_singles.size() * sizeof(rdr_scene::CrowdSingleRow) + r.crowd_pictures.size() * sizeof(rdr_scene::CrowdPictureRow) +  // S103 D4, S103 D6
                              // S103: lane S owned_passes, lane F particles (t19_lint.py names every SceneTables vector and the particle tables)
                              r.owned_passes.size() * sizeof(rdr_scene::OwnedStamp) + r.particles.Bytes() + r.quads2d.size() * sizeof(rdr_scene::Quad2D) /* S103 I1 */ + r.weapon_slots.size() * sizeof(rdr_scene::weapons::WeaponSlot) /* S103 PW-1 */ +
                              // S103 S: lanes A / A2's tables (t19_lint estimate)
                              r.post_params.size() * sizeof(rdr_scene::PostParamsRow) + r.model_runtime.size() * sizeof(rdr_scene::ModelRuntimeRow) +
                              r.input_stamps.size() * sizeof(rdr_scene::InputStampRow) + r.param_overrides.size() * sizeof(rdr_scene::ParamOverrideRow) +
                              r.light_objects.size() * sizeof(rdr_scene::LightObjectRow) +
                              r.liquid_maps.size() * sizeof(rdr_scene::LiquidMapRow) + r.liquid_items.size() * sizeof(rdr_scene::LiquidItemRow) + r.liquid_pictures.size() * sizeof(rdr_scene::LiquidPictureRow) +   // S103 SWF
                              r.body_scales.size() * sizeof(rdr_scene::BodyScaleRow) + r.liquid_images.size() * sizeof(rdr_scene::LiquidImageRow) + r.liquid_image_bytes.size();   // S108 SHOT (68-70)
    // S102 T19 F1: the file is the plain list (version 1, what every reader of a case folder reads): a marker record is expanded here, on the ring's thread, never on the game's
    case_hook::OnScene(f.guest_frame, estimate, [t] {
      if (t->crowd_runs.empty()) return rdr_scene::SerializeRecord(*t);
      const rdr_scene::SceneTables plain = rdr_scene::ExpandCrowdRuns(*t);
      return rdr_scene::SerializeRecord(plain);
    });
    ++g_held;
  }
  void Census(std::string& line) override { line += "case ring scene records " + std::to_string(case_hook::Stats().scene_records); }
};

CaseSceneSink& TheSink() {
  static CaseSceneSink* s = new CaseSceneSink;  // leaked: a late EndFrame never meets a destroyed sink
  return *s;
}

}  // namespace

void AttachSceneSink() { rdr_scene::live::AddSink(&TheSink()); }
void DetachSceneSink() { rdr_scene::live::RemoveSink(&TheSink()); }
uint64_t SceneRecordsHeld() { return g_held.load(); }

}  // namespace rdr_case
