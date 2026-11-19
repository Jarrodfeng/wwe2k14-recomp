// S103 IXF: the binder's geometry cache size guard (geo_size_guard.h), synthetic (no capture, no GPU). Build: tools\scene\build_ixf_unit.ps1.
//   geo_size_guard_test.exe [--plant]   --plant: the old rule (keyed by offset only, size ignored on a hit): the "never smaller" check must FAIL.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "geo_size_guard.h"

namespace {
struct Entry { bool have = false; uint32_t size = 0; };
int fails = 0;
void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++fails;
}
bool g_plant = false;
int g_problems = 0;
// The binder's Geometry() cached branch, with the buffer reduced to its size. Returns the size of the buffer served.
uint32_t Serve(std::unordered_map<uint64_t, Entry>& cache, uint64_t offset, uint32_t want) {
  using rdr_adapter::geosize::Cache;
  Entry& e = cache[offset];
  const Cache c = g_plant ? (e.have ? Cache::kHit : Cache::kCreate) : rdr_adapter::geosize::Classify(e.have, e.size, want);
  if (c == Cache::kCreate) { e.have = true; e.size = want; return want; }
  if (c == Cache::kMismatch) { ++g_problems; return want; }  // a separate per-frame push of exactly the range
  return e.size;
}
}  // namespace

int main(int argc, char** argv) {
  g_plant = argc > 1 && std::strcmp(argv[1], "--plant") == 0;
  std::unordered_map<uint64_t, Entry> cache;
  // the a30_match case: frame 0's stale draw makes 1,270 bytes at 2,529,850; frame 1's real part wants 2,622 bytes there
  Check(Serve(cache, 2529850, 1270) == 1270, "first draw at the offset creates a buffer of its own size");
  const uint32_t served = Serve(cache, 2529850, 2622);
  Check(served >= 2622, "a later draw with a larger range is never served a smaller buffer");
  Check(g_plant || g_problems == 1, "the mismatch raised exactly one geometry_offset_size_mismatch");
  Check(Serve(cache, 2529850, 1270) == 1270, "the same size at the same offset is a plain hit");
  Check(Serve(cache, 2529850, 600) >= 600 && g_plant == (g_problems == 0), "a smaller range than the cached buffer is a hit, no problem");
  Check(Serve(cache, 99, 64) == 64 && Serve(cache, 99, 64) == 64, "an unrelated offset is unaffected");
  std::printf("%s\n", fails ? "FAILED" : "all passed");
  return fails ? 1 : 0;
}
