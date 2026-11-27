// flash_camera_unit: the offline proof of the S111 FLASHCAM review's keep census (src\scene_live\flash_keep.h: the game's own keep rule of a crowd flash, sub_823B2470, CENSUS ONLY). 0 game runs, no SDK,
// no game memory: a FAKE GUEST built from the listing's offsets (the engine word 0x83929A68, its handed block +256 with view +240 / projection +176, the screen words +300 / +304, the phase word 0x836F3940,
// the margins word 0x836F3A94, the 9 character words at 0x836F38D0 with +1208 / +224 / +232 / +812 -> +4 -> +48..56).
//   flash_camera_unit [--plant screen_1280|no_char_box]
// Built and run by tools\scene\flash_camera_check.py: each plant (the landed FLASHCAM rule: a 1280 x 720 screen; no character box) must fail its named check. Exit 0 = pass.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#include "../../src/scene_live/flash_keep.h"

using namespace rdr_scene;
namespace fk = rdr_scene::live::flashkeep;
namespace ffx = rdr_scene::live::flashfx;

namespace {
int g_bad = 0;
void Check(bool ok, const char* name, const std::string& detail = std::string()) {
  std::printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", name, detail.empty() ? "" : ": ", detail.c_str());
  if (!ok) ++g_bad;
}
std::unordered_map<uint32_t, uint32_t> g_words;   // host-order words; the reader hands them big-endian, as the guest holds them
void Put(uint32_t a, uint32_t v) { g_words[a] = v; }
void PutF(uint32_t a, float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  g_words[a] = b;
}
bool Read(uint32_t a, void* dst, size_t n) {
  auto* p = static_cast<uint8_t*>(dst);
  for (size_t i = 0; i < n; i += 4) {
    auto it = g_words.find(a + uint32_t(i));
    if (it == g_words.end()) return false;
    const uint32_t be = ffx::Swap32(it->second);
    std::memcpy(p + i, &be, 4);
  }
  return true;
}
constexpr uint32_t kEng = 0x40000000, kBlk = 0x40001000, kCh = 0x40002000, kComp = 0x40003000, kNode = 0x40004000;
constexpr float kXs = 1.35f, kYs = 2.4f;
// The game's camera at the origin looking along +z (y down): view = identity; projection diag(xs, ys) with w = z. A 1920 x 1080 screen.
void Build(bool character, uint32_t phase = 0, bool margins_off = false) {
  g_words.clear();
  Put(fk::kEngine, kEng);
  Put(kEng + fk::kEngineBlock, kBlk);
  Put(kEng + fk::kEngineScreenW, 1920), Put(kEng + fk::kEngineScreenH, 1080);
  const float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  const float proj[16] = {kXs, 0, 0, 0, 0, kYs, 0, 0, 0, 0, 1.0001f, 1, 0, 0, -10.f, 0};
  for (uint32_t i = 0; i < 16; ++i) PutF(kBlk + fk::kCamView + 4 * i, view[i]), PutF(kBlk + fk::kCamProj + 4 * i, proj[i]);
  Put(fk::kPhaseWord, phase);
  Put(fk::kMarginsOff, margins_off ? 1u : 0u);
  for (uint32_t i = 0; i < fk::kCharSlotCount; ++i) Put(fk::kCharSlots + 4 * i, 0);
  if (character) {   // one character straight ahead, nearer than the flash: its point lands on the screen centre
    Put(fk::kCharSlots + 8, kCh);
    Put(kCh + 1208, 1), Put(kCh + 224, 0x1234), Put(kCh + 232, 1), Put(kCh + 812, kComp), Put(kComp + 4, kNode);
    PutF(kNode + 48, 0.f), PutF(kNode + 52, 0.f), PutF(kNode + 56, 2000.f);
  }
}
}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--plant" && i + 1 < argc) {
      const std::string p = argv[++i];
      if (p == "screen_1280") fk::g_plant_screen_1280 = true;
      else if (p == "no_char_box") fk::g_plant_no_char_box = true;
      else return std::fprintf(stderr, "unknown plant %s\n", p.c_str()), 2;
    } else {
      return std::fprintf(stderr, "usage: flash_camera_unit [--plant screen_1280|no_char_box]\n"), 2;
    }
  }
  const float centre[3] = {0.f, 0.f, 3000.f};
  // a point at ndc y 0.85: 64 px inside a 1080-line screen (py 999 <= 1016), outside a 720-line one (py 666 > 656). The projection's row 1 is negated by the rule: ndc y = -ys y / z.
  const float low[3] = {0.f, -0.85f * 3000.f / kYs, 3000.f};
  fk::KeepCam kc;
  Build(false);
  const bool read = fk::ReadKeepCam(&Read, kc);
  Check(read && kc.w == (fk::g_plant_screen_1280 ? 1280 : 1920) && std::fabs(kc.proj_xs - kXs) < 1e-6f && std::fabs(kc.proj_ys - kYs) < 1e-6f, "keep.read",
        "w " + std::to_string(kc.w) + " h " + std::to_string(kc.h));
  Check(fk::GameKeeps(kc, centre) == 1, "keep.centre");
  Check(fk::GameKeeps(kc, low) == 1, "keep.screen_words", "a point 64+ px inside the game's own 1080-line screen");
  const float behind[3] = {0.f, 0.f, 40.f};
  Check(fk::GameKeeps(kc, behind) == 0, "keep.depth", "view z 40 (the game keeps only z > 50)");
  Build(true);
  fk::ReadKeepCam(&Read, kc);
  Check(kc.chars == (fk::g_plant_no_char_box ? 0u : 1u) && fk::GameKeeps(kc, centre) == 0, "keep.char_box", "a character's point on the flash's screen spot refuses it (chars " + std::to_string(kc.chars) + ")");
  Build(true, 0, true);
  fk::ReadKeepCam(&Read, kc);
  Check(fk::GameKeeps(kc, centre) == 1, "keep.margins_off", "[0x836F3A94] != 0: the box margins are 0");
  Build(false, 4);
  fk::ReadKeepCam(&Read, kc);
  const float aside[3] = {5000.f, 0.f, 3000.f};
  Check(fk::GameKeeps(kc, aside) == 1, "keep.phase4", "phase word 4: only the depth is tested");
  Build(false);
  fk::ReadKeepCam(&Read, kc);
  StageFxRow r;
  r.kind = kFxFlash, r.flags = kFxfGameScale;
  r.pos[0] = centre[0], r.pos[1] = centre[1] - ffx::kYDrop, r.pos[2] = centre[2];   // the writer's row: y - 40
  fk::Stamp(kc, true, r);
  int32_t keep = 7;
  float xs = 0.f, ys = 0.f;
  std::memcpy(&keep, &r.pad, 4), std::memcpy(&xs, &r.w, 4), std::memcpy(&ys, &r.h, 4);
  StageFxRow u = r;
  fk::Stamp(kc, false, u);
  int32_t ukeep = 7;
  std::memcpy(&ukeep, &u.pad, 4);
  g_words.erase(fk::kEngine);
  const bool none = !fk::ReadKeepCam(&Read, kc);
  Check(keep == 1 && std::fabs(xs - kXs) < 1e-6f && std::fabs(ys - kYs) < 1e-6f && (r.flags & kFxfKeepCensus) && (r.flags & kFxfGameScale) && ukeep == -1 && none, "keep.stamp");
  std::printf(g_bad ? "FAIL: flash_camera_unit, %d check(s)\n" : "PASS: flash_camera_unit\n", g_bad);
  return g_bad ? 1 : 0;
}
