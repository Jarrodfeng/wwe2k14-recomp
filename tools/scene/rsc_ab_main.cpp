// S106 G4-1A: the driver of tools\scene\rsc_ab.py (the record round-trip A/B). The SAME source is compiled twice, once against the base tree's
// src\renderer_scene (record_io.cpp + headers) and once against this tree's; the two exes must print the same line for every stored record.
//   rsc_ab_main LISTFILE      one record path per line (UTF-8); prints one line per record:
//                             <index> <ok | err:TEXT> errs=<every *_errors counter>,holes counts=<every table's row count> fnv=<FNV-64 of
//                             SerializeRecord(parsed)> same=<1 when the re-serialized bytes equal the file's bytes>
//   rsc_ab_main --make OUT    writes a small synthetic record (a few rows of several families) to OUT (the selftest's input; no game data)
// entry: rdr_scene::ParseRecord / rdr_scene::SerializeRecord (the codec's public pair)
// Includes record_all.h when the tree has it (the split headers), else record_io.h (the base tree).
#if defined(__has_include)
#if __has_include("record_all.h")
#include "record_all.h"
#else
#include "record_io.h"
#endif
#else
#include "record_io.h"
#endif

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace rdr_scene;

static uint64_t Fnv64(const std::vector<uint8_t>& b) {
  uint64_t h = 1469598103934665603ull;
  for (uint8_t c : b) h = (h ^ c) * 1099511628211ull;
  return h;
}

static std::string Describe(const SceneTables& t) {
  char buf[4096];
  int n = 0;
  n += snprintf(buf + n, sizeof buf - n, "errs=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,holes=%u",
                t.plane_errors, t.circle_errors, t.clock_errors, t.liquid_errors, t.arena_world_errors, t.ring_errors, t.weapon_call_errors,
                t.caster_errors, t.sphere_errors, t.param_errors, t.light_errors, t.inputs_errors, t.c_errors, t.quad2d_errors, t.quad2d_holes);
  n += snprintf(buf + n, sizeof buf - n, " counts=cam%d,str%zu", int(t.has_camera), t.strings.size());
#define C(f) n += snprintf(buf + n, sizeof buf - n, "," #f "%zu", size_t(t.f.size()));
  C(assets) C(objects) C(bones) C(params) C(lights) C(sprites) C(texts) C(movies) C(targets) C(engine_objects) C(light_states) C(light_refs)
  C(circle_shadows) C(circle_mesh) C(plane_frames) C(plane_draws) C(plane_eyes) C(bone_tags) C(props_light) C(part_clocks) C(liquid_maps)
  C(liquid_items) C(liquid_pictures) C(arena_worlds) C(ring_rows) C(ring_nodes) C(weapon_calls) C(weapon_nodes) C(caster_calls) C(caster_nodes)
  C(caster_mats) C(caster_states) C(caster_extras) C(sphere_occ) C(post_params) C(model_runtime) C(input_stamps) C(param_overrides)
  C(light_objects) C(crowd_runs) C(crowd_members) C(model_lights) C(stage_fx) C(stage_look) C(crowd_props) C(crowd_pictures) C(crowd_singles)
  C(owned_passes) C(quads2d) C(weapon_slots) C(particles.batches) C(particles.particles) C(particles.status) C(particles.screen)
  C(guest_models.parts) C(guest_models.blobs) C(guest_models.textures)
#undef C
  if (n < 0) n = 0;   // S106 G4-1AF: snprintf returns the length it WOULD have written: clamp to the buffer
  if (n > int(sizeof buf) - 1) n = int(sizeof buf) - 1;
  return std::string(buf, size_t(n));
}

static int Make(const char* out) {
  SceneTables t;
  t.has_camera = true;
  t.assets.resize(1);
  t.objects.resize(2);
  t.objects[0].asset = 0, t.objects[0].id = 0, t.objects[1].asset = 0, t.objects[1].flags = kObjVisible | kObjSkinned;
  t.lights.resize(1);
  t.lights[0].set = 3, t.lights[0].dir_or_pos[1] = 1.f;
  t.part_clocks.resize(1);
  t.part_clocks[0].object = 1, t.part_clocks[0].time = 0.5f, t.part_clocks[0].period = 2.f;
  t.stage_look.resize(1);
  t.stage_look[0].kind = kSlEntrance, t.stage_look[0].flags = kSlfEntrance;
  t.crowd_props.resize(1);
  t.crowd_props[0].kind = kCpSign, t.crowd_props[0].model_key = 40000;
  std::vector<uint8_t> b = SerializeRecord(t);
  std::ofstream f(out, std::ios::binary);
  f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
  return f ? 0 : 2;
}

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--make") return Make(argv[2]);
  if (argc != 2) {
    fprintf(stderr, "usage: rsc_ab_main LISTFILE | --make OUT\n");
    return 2;
  }
  std::ifstream list(argv[1]);
  std::string path;
  size_t idx = 0;
  while (std::getline(list, path)) {
    while (!path.empty() && (path.back() == '\r' || path.back() == '\n')) path.pop_back();
    if (path.empty()) continue;
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> in((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    SceneTables t;
    std::string err;
    if (!f || !ParseRecord(in.data(), in.size(), t, err)) {
      printf("%zu err:%s\n", idx++, f ? err.c_str() : "unreadable");
      continue;
    }
    std::vector<uint8_t> out = SerializeRecord(t);
    printf("%zu ok %s fnv=%016llx same=%d\n", idx++, Describe(t).c_str(), static_cast<unsigned long long>(Fnv64(out)), int(out == in));
  }
  return 0;
}
