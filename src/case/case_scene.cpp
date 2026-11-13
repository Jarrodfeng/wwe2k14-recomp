// The case ring's scene sink (Session 101, lane 26). See case_scene.h.
#include "case_scene.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../renderer_scene/record_io.h"
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
    const uint64_t estimate = sizeof(rdr_scene::SceneHeader) + r.strings.size() + r.assets.size() * sizeof(rdr_scene::AssetRef) +
                              r.objects.size() * sizeof(rdr_scene::Object) + r.bones.size() * sizeof(rdr_scene::Mat4) +
                              r.params.size() * sizeof(rdr_scene::ParamValue) + r.lights.size() * sizeof(rdr_scene::Light) +
                              r.sprites.size() * sizeof(rdr_scene::Sprite) + r.texts.size() * sizeof(rdr_scene::Text) +
                              r.movies.size() * sizeof(rdr_scene::Movie) + r.targets.size() * sizeof(rdr_scene::Target) +
                              // lane 226 N1: the live tables (about 80 KB a frame with caster D3 on: 13 plane calls x 96 matrices alone are 80 KB)
                              r.engine_objects.size() * 4 + r.light_states.size() * sizeof(rdr_scene::LightState) + r.light_refs.size() * sizeof(rdr_scene::LightRef) +
                              r.circle_shadows.size() * sizeof(rdr_scene::CircleShadow) + r.circle_mesh.size() * sizeof(rdr_scene::CircleMesh) + r.plane_frames.size() * sizeof(rdr_scene::PlaneFrame) +
                              r.plane_draws.size() * sizeof(rdr_scene::PlaneDraw) + r.arena_worlds.size() * sizeof(rdr_scene::ArenaWorld) + r.ring_rows.size() * sizeof(rdr_scene::RingRows) +
                              r.ring_nodes.size() * sizeof(rdr_scene::RingNodeRows) + r.caster_calls.size() * sizeof(rdr_scene::CasterCall) + r.caster_nodes.size() * 4 +
                              r.caster_mats.size() * sizeof(rdr_scene::Mat4) + r.caster_states.size() * sizeof(rdr_scene::LightState) + r.caster_extras.size() * sizeof(rdr_scene::CasterExtra) + r.sphere_occ.size() * sizeof(rdr_scene::SphereOccRow);
    case_hook::OnScene(f.guest_frame, estimate, [t] { return rdr_scene::SerializeRecord(*t); });
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
