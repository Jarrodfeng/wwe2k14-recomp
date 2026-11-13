// Session 101, lane 14: the viewer's `record PATH` command (see scene_record_out.h). The viewer's scene in the scene record's terms:
// assets by pac + entry (from the model's label: "arena 5", "wrestler 107 attire 0", "prop 349"), objects with world matrices,
// the skin palette of a posed model, the camera. Space: the viewer's (SCENE_RECORD.md "Space and projection"): the record keeps the
// combined view-projection in Camera::view and the identity in Camera::proj (the viewer's convention until Camera gets a v2 option).
#include "scene_record_out.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../../renderer_scene/record_io.h"
#include "json_lite.h"

namespace view {
namespace {

std::string Err(const std::string& m) { return "{\"ok\":false,\"error\":\"" + jl::Esc(m) + "\"}"; }

struct Parsed {
  rdr_scene::AssetKind kind = rdr_scene::AssetKind::Model;
  std::string pac, entry, id;  // pac path under game_data\extracted, entry in it, the stable object id
  uint32_t variant = 0;
  bool ok = false;
};

// "[actor NAME ]arena N" / "wrestler N attire M" / "prop N". Anything else is not recordable (the ring props, entrance pieces, ...).
Parsed ParseLabel(const std::string& label) {
  Parsed p;
  std::string who;
  std::string rest = label;
  if (rest.rfind("actor ", 0) == 0) {
    const size_t sp = rest.find(' ', 6);
    if (sp == std::string::npos) return p;
    who = rest.substr(6, sp - 6);
    rest = rest.substr(sp + 1);
  }
  int a = 0, b = 0;
  char buf[160];
  if (std::sscanf(rest.c_str(), "arena %d", &a) == 1) {
    std::snprintf(buf, sizeof buf, "pac\\bg\\bg%02d.pac", a);
    p.pac = buf, p.id = "arena:" + std::to_string(a), p.ok = true;
  } else if (std::sscanf(rest.c_str(), "wrestler %d attire %d", &a, &b) == 2) {
    std::snprintf(buf, sizeof buf, "pac\\ch\\ch%03d.pac", a);
    p.pac = buf, p.entry = "attire", p.variant = uint32_t(b), p.id = "actor:" + (who.empty() ? std::string("w") + std::to_string(a) : who), p.ok = true;
  } else if (std::sscanf(rest.c_str(), "prop %d", &a) == 1) {
    p.pac = "pac\\evt\\evtobj.pac", p.entry = std::to_string(a), p.id = "prop:" + std::to_string(a), p.ok = true;
  }
  return p;
}

}  // namespace

std::string RecordScene(const Renderer& r, const Camera& cam, int w, int h, const std::string& path, long long frame) {
  if (path.empty()) return Err("missing path");
  if (!r.HasModel()) return Err("nothing loaded (load an arena first)");
  using rdr_scene::SceneTables; using rdr_scene::AssetRef; using rdr_scene::Object; using rdr_scene::Source; using rdr_scene::WriteRecord; using rdr_scene::IdentityMat4;
  using rdr_scene::kObjSkinned;
  SceneTables t;
  t.header.source = uint8_t(Source::Viewer);
  t.header.guest_frame = uint64_t(std::max(0ll, frame));

  // The camera, for a w x h picture: the same matrix the viewer's shot uses (the scene's diagonal decides the clip planes).
  float mn[3] = {0, 0, 0}, mx[3] = {0, 0, 0};
  r.Bounds(mn, mx);
  const float diag = std::sqrt((mx[0] - mn[0]) * (mx[0] - mn[0]) + (mx[1] - mn[1]) * (mx[1] - mn[1]) + (mx[2] - mn[2]) * (mx[2] - mn[2]));
  const view::Mat4 vp = ViewProj(cam, float(w) / float(std::max(h, 1)), std::max(diag, 100.0f));
  const V3 eye = Eye(cam);
  t.has_camera = true;
  std::memcpy(t.camera.view.m, vp.m, sizeof vp.m);
  t.camera.proj = IdentityMat4();
  t.camera.flags = rdr_scene::kCamViewIsViewProj;  // S101 lane 1: declared, not inferred from proj == identity
  t.camera.eye[0] = eye.x, t.camera.eye[1] = eye.y, t.camera.eye[2] = eye.z;
  t.camera.fov_y_deg = cam.fov;

  int skipped = 0;
  const std::vector<GpuModel>& models = r.Models();
  for (size_t i = 0; i < models.size(); ++i) {
    const GpuModel& gm = models[i];
    const Parsed p = ParseLabel(gm.label);
    if (!p.ok) { ++skipped; continue; }
    AssetRef ar;
    ar.kind = uint8_t(p.kind);
    ar.pac = t.AddString(p.pac);
    ar.entry = t.AddString(p.entry);
    ar.variant = p.variant;
    // one AssetRef per distinct asset
    uint32_t asset = uint32_t(t.assets.size());
    for (size_t k = 0; k < t.assets.size(); ++k)
      if (t.assets[k].pac == ar.pac && t.assets[k].entry == ar.entry && t.assets[k].variant == ar.variant) { asset = uint32_t(k); break; }
    if (asset == t.assets.size()) t.assets.push_back(ar);

    Object o;
    o.asset = asset;
    o.klass = rdr_adapter::DrawClass::ModelPart;
    o.id = t.AddString(p.id);
    // The model's place: turned about Y (x' = x cos + z sin, z' = -x sin + z cos), then moved by its offset (the rdr backend's rule).
    float* m = o.world.m;
    std::memset(m, 0, sizeof o.world.m);
    m[0] = gm.rot_cos, m[2] = -gm.rot_sin, m[5] = 1.f, m[8] = gm.rot_sin, m[10] = gm.rot_cos, m[15] = 1.f;
    m[12] = gm.offset[0], m[13] = gm.offset[1], m[14] = gm.offset[2];
    if (gm.has_bones && !gm.skin_cpu.empty()) {
      o.flags |= kObjSkinned;
      o.bones_first = uint32_t(t.bones.size());
      o.bones_count = uint32_t(gm.skin_cpu.size());
      for (const assets::Matrix& b : gm.skin_cpu) {
        rdr_scene::Mat4 bm{};
        std::memcpy(bm.m, b.data(), sizeof bm.m);
        t.bones.push_back(bm);
      }
    }
    t.objects.push_back(o);
  }
  if (t.objects.empty()) return Err("no model of the scene can be recorded (labels not recognised)");

  std::string error;
  if (!WriteRecord(path, t, error)) return Err(error);
  return "{\"ok\":true,\"path\":\"" + jl::Esc(path) + "\",\"objects\":" + std::to_string(t.objects.size()) + ",\"assets\":" + std::to_string(t.assets.size()) +
         ",\"bones\":" + std::to_string(t.bones.size()) + ",\"skipped\":" + std::to_string(skipped) + "}";
}

}  // namespace view
