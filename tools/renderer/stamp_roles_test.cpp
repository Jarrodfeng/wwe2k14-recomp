// S102 G6 (review): header-only unit test of src\renderer_adapter\stamp_roles.h.
//   clang++ -std=c++20 -Wall -Wextra -Werror=switch -I src tools\renderer\stamp_roles_test.cpp -o stamp_roles_test.exe
//   stamp_roles_test [1]    exit 0 = all pass; plant 1 (the old pick: the FIRST scene_color record, any shape) must make it exit 1
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "renderer_adapter/stamp_roles.h"

using namespace rdr_adapter;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static ImageRecord Rec(uint32_t id, uint16_t w, uint16_t h, SurfaceRole role, uint8_t flags = 0) {
  ImageRecord r;
  r.id = id, r.width = w, r.height = h, r.role = uint8_t(role), r.flags = flags, r.source = kSourceSurface, r.usage = rdr::kUsageTarget | rdr::kUsageSampled;
  return r;
}

int main(int argc, char** argv) {
  const int plant = argc > 1 ? std::atoi(argv[1]) : 0;
  // every role but Unnamed / Other has a class of its own
  for (uint8_t role = 0; role < uint8_t(SurfaceRole::Count); ++role) {
    const uint8_t cls = stamp_roles::GpuClassOfRole(role);
    const bool open = SurfaceRole(role) == SurfaceRole::Unnamed || SurfaceRole(role) == SurfaceRole::Other;
    if (!open && cls == rdr::kGpuOther) std::printf("role %s has no class\n", SurfaceRoleName(SurfaceRole(role)));
    CHECK(open ? cls == rdr::kGpuOther : cls != rdr::kGpuOther);
  }
  CHECK(stamp_roles::GpuClassOfRole(uint8_t(SurfaceRole::FrameBuffer)) == rdr::kGpuFrame);
  CHECK(stamp_roles::GpuClassOfRole(uint8_t(SurfaceRole::CopyDest)) == rdr::kGpuCopies);
  // the scene-size pick: the 736x1280 roster picture is a SceneColor record too (creator 0x82C22AF8); the frame buffer is 1280x720
  std::vector<ImageRecord> recs = {Rec(5, 736, 1280, SurfaceRole::SceneColor), Rec(7, 1280, 720, SurfaceRole::SceneColor), Rec(8, 1280, 720, SurfaceRole::Unnamed)};
  uint16_t w = 0, h = 0;
  bool have = stamp_roles::PickSceneSize(recs, w, h);
  if (plant == 1) {  // the old pick
    have = true, w = recs[0].width, h = recs[0].height;
  }
  CHECK(have && w == 1280 && h == 720);
  CHECK(stamp_roles::RoleForStamps(recs[2], w, h) == uint8_t(SurfaceRole::FrameBuffer));
  // not a frame buffer: a copy destination, a texture, the wrong size, a declared role, no scene yet
  CHECK(stamp_roles::RoleForStamps(Rec(9, 1280, 720, SurfaceRole::Unnamed, kImageCopyDest), 1280, 720) == uint8_t(SurfaceRole::CopyDest));
  ImageRecord tex = Rec(10, 1280, 720, SurfaceRole::Unnamed);
  tex.source = kSourceTexture;
  CHECK(stamp_roles::RoleForStamps(tex, 1280, 720) == uint8_t(SurfaceRole::Unnamed));
  CHECK(stamp_roles::RoleForStamps(Rec(11, 512, 512, SurfaceRole::Unnamed), 1280, 720) == uint8_t(SurfaceRole::Unnamed));
  CHECK(stamp_roles::RoleForStamps(Rec(12, 1280, 720, SurfaceRole::ShadowMap), 1280, 720) == uint8_t(SurfaceRole::ShadowMap));
  CHECK(stamp_roles::RoleForStamps(Rec(13, 1280, 720, SurfaceRole::Unnamed), 0, 0) == uint8_t(SurfaceRole::Unnamed));
  // the largest screen-shaped scene target wins; none screen-shaped = none
  std::vector<ImageRecord> two = {Rec(3, 640, 360, SurfaceRole::SceneColor), Rec(9, 1280, 720, SurfaceRole::SceneColor)};
  CHECK(stamp_roles::PickSceneSize(two, w, h) && w == 1280);
  std::vector<ImageRecord> roster = {Rec(1, 736, 1280, SurfaceRole::SceneColor)};
  CHECK(!stamp_roles::PickSceneSize(roster, w, h));
  std::printf(g_fail ? "stamp_roles_test: %d FAILED\n" : "stamp_roles_test: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
