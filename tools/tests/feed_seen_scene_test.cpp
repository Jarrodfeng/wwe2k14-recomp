// S110 FEEDREC review: the pure scene readers in the COVERAGE shape (feed_seen.h's ReadFnT struct and the location macros
// live). 0 game runs. Built and run by tools\tests\feed_seen_test.ps1.
// 1. The headers that use ReadFn compile with the converting struct (none of them may need the plain pointer).
// 2. A shadowing `ReadWord` lambda passes its caller's line down: scene_inputs::ReadLightObjects reads the manager words
//    at two source lines (mgr+124 on one, mgr+144 on the next); both rows naming one line = the lambda's own line.
#define WWE2K14_FEEDSEEN_TEST 1
#include "feed_seen.h"
#include "scene_live/inputs_core.h"
#include "scene_live/circle_points.h"
#include "scene_live/piece_link.h"
#include "scene_live/flash_fx_core.h"
#include "scene_live/stage_fx_core.h"
#include "scene_live/part_clock_read.h"
#include "scene_live/screen_planes_core.h"
#include "scene_live/movie_planes.h"
#include "scene_live/light_latch.h"
#include "scene_live/plane_eye_read.h"
#include "scene_live/props_light_read.h"

extern "C" uint32_t wwe2k14_feedrec_on = 0;
namespace wwe2k14::storewatch {
void Add(uint32_t, uint32_t) {}
}  // namespace wwe2k14::storewatch

constexpr uint32_t kMgr = 0x82100000u;
static bool Fake(uint32_t guest, void* dst, size_t n) {   // zeros, and the light manager pointer
  std::memset(dst, 0, n);
  if (guest == scene_inputs::kLightMgrPtr && n == 4) {
    const uint32_t be = __builtin_bswap32(kMgr);
    std::memcpy(dst, &be, 4);
  }
  return true;
}

// The line of the feed_reads.tsv row of address `addr` in inputs_core.h; 0 when there is none.
static uint32_t LineOf(const std::filesystem::path& f, uint32_t addr) {
  FILE* h = std::fopen(f.string().c_str(), "rb");
  if (!h) return 0;
  char file[512];
  unsigned line = 0, a = 0, w = 0;
  unsigned long long d = 0;
  uint32_t found = 0;
  char hdr[256];
  if (std::fgets(hdr, sizeof hdr, h))
    while (std::fscanf(h, "%511s %u 0x%X %u %llu", file, &line, &a, &w, &d) == 5)
      if (a == addr && std::strstr(file, "inputs_core.h")) found = line;
  std::fclose(h);
  return found;
}

int main() {
  const std::filesystem::path dir = std::filesystem::current_path() / "feed_scene_out";
  std::filesystem::create_directories(dir);
  __atomic_store_n(&wwe2k14_feedrec_on, 1u, __ATOMIC_RELAXED);
  std::vector<rdr_scene::LightObjectRow> out;
  scene_inputs::LightReadInfo info;
  scene_inputs::ReadLightObjects(&Fake, 1, out, info);
  int bad = 0;
  if (wwe2k14::feedseen::Write(dir) <= 0) std::printf("FAIL nothing written\n"), ++bad;
  const uint32_t l124 = LineOf(dir / "feed_reads.tsv", kMgr + 124), l144 = LineOf(dir / "feed_reads.tsv", kMgr + 144);
  if (!l124 || !l144) std::printf("FAIL the manager words were not recorded (mgr+124 line %u, mgr+144 line %u)\n", l124, l144), ++bad;
  else if (l124 == l144) std::printf("FAIL the manager words read at two lines are recorded at one (%u): the lambda's line\n", l124), ++bad;
  std::printf(bad ? "feed_seen_scene_test: %d problem(s)\n" : "feed_seen_scene_test: 0 problems\n", bad);
  return bad;
}
