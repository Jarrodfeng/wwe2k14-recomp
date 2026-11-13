// rsc_dump: prints a scene record's tables (Session 101, lane 14). Usage: rsc_dump FILE.rsc [--rewrite OUT.rsc]
// Exit 0 when the file reads, 2 when it fails (the reason on stderr). No game or renderer code: record_io only.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "../../src/renderer_scene/record_io.h"

using namespace rdr_scene;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: rsc_dump FILE.rsc\n");
    return 1;
  }
  SceneTables t;
  std::string err;
  // S101 lane 26: FILE may be a capture folder (or its scene folder) with --frame N: scene/frame_NNNNNN.rsc
  std::string path = argv[1];
  {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
      long frame = -1;
      for (int i = 2; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--frame") == 0) frame = std::atol(argv[i + 1]);
      char name[32];
      std::snprintf(name, sizeof(name), "frame_%06ld.rsc", frame);
      std::filesystem::path p = std::filesystem::path(path) / "scene" / name;
      if (!std::filesystem::exists(p, ec)) p = std::filesystem::path(path) / name;
      if (frame < 0 || !std::filesystem::exists(p, ec)) {
        std::fprintf(stderr, "rsc_dump: FAIL %s: no scene record for --frame %ld\n", argv[1], frame);
        return 2;
      }
      path = p.string();
    }
  }
  // --rewrite OUT.rsc writes the tables back out (the round trip: write -> read -> write must give the same bytes)
  const char* rewrite = nullptr;  // S101 lane 26 review: --rewrite OUT anywhere after FILE (also after FOLDER --frame N)
  for (int i = 2; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], "--rewrite") == 0) rewrite = argv[i + 1];
  if (!ReadRecord(path, t, err)) {
    std::fprintf(stderr, "rsc_dump: FAIL %s\n", err.c_str());
    return 2;
  }
  const SceneHeader& h = t.header;
  std::printf("record %s\n", path.c_str());
  std::printf("  version %u  source %u  sections %u  guest_frame %llu  time_s %.6f  address \"%s\"  flags 0x%X\n", h.version, h.source, h.section_count,
              (unsigned long long)h.guest_frame, h.time_s, std::string(t.Str(h.address)).c_str(), h.flags);
  std::printf("  strings %zu bytes, assets %zu, objects %zu, bones %zu, params %zu, lights %zu, sprites %zu, texts %zu, movies %zu, targets %zu\n",
              t.strings.size(), t.assets.size(), t.objects.size(), t.bones.size(), t.params.size(), t.lights.size(), t.sprites.size(), t.texts.size(),
              t.movies.size(), t.targets.size());
  if (t.has_camera) {
    const Camera& c = t.camera;
    std::printf("  camera: eye (%.2f %.2f %.2f) fov %.2f near %.2f far %.2f cut %u\n", c.eye[0], c.eye[1], c.eye[2], c.fov_y_deg, c.near_z, c.far_z, c.cut);
    std::printf("    view:");
    for (int i = 0; i < 16; ++i) std::printf(" %.4g", c.view.m[i]);
    std::printf("\n    proj:");
    for (int i = 0; i < 16; ++i) std::printf(" %.4g", c.proj.m[i]);
    std::printf("\n");
  } else {
    std::printf("  camera: none\n");
  }
  for (size_t i = 0; i < t.assets.size(); ++i) {
    const AssetRef& a = t.assets[i];
    std::printf("  asset %zu: kind %u pac \"%s\" entry \"%s\" variant %u\n", i, a.kind, std::string(t.Str(a.pac)).c_str(), std::string(t.Str(a.entry)).c_str(), a.variant);
  }
  for (size_t i = 0; i < t.objects.size(); ++i) {
    const Object& o = t.objects[i];
    std::printf("  object %zu: id \"%s\" asset %u class %u flags 0x%X pos (%.2f %.2f %.2f) bones %u+%u params %u+%u\n", i, std::string(t.Str(o.id)).c_str(),
                o.asset, unsigned(o.klass), o.flags, o.world.m[12], o.world.m[13], o.world.m[14], o.bones_first, o.bones_count, o.params_first, o.params_count);
  }
  for (size_t i = 0; i < t.params.size(); ++i)
    std::printf("  param %zu: \"%s\" = (%g %g %g %g)\n", i, std::string(t.Str(t.params[i].name)).c_str(), t.params[i].value[0], t.params[i].value[1],
                t.params[i].value[2], t.params[i].value[3]);
  if (rewrite && !WriteRecord(rewrite, t, err)) {
    std::fprintf(stderr, "rsc_dump: FAIL rewrite: %s\n", err.c_str());
    return 3;
  }
  return 0;
}
