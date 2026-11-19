// S103 IXF: the memo shortcut across a capture Stop / Start (geo_store_reset.h). Compile only, no game, no GPU:
//   pwsh -NoProfile -File tools\scene\build_ixf_unit.ps1  ->  out\tools\geo_store_reset_test.exe [--plant]
// --plant: the fix removed (StartCapture neither advances the record frame nor clears the memo): the check must FAIL (exit 1).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "geo_store_reset.h"

namespace {
struct Entry { uint64_t used_frame = 0; uint64_t offset = 0; };
int fails = 0;
void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++fails;
}
}  // namespace

int main(int argc, char** argv) {
  const bool plant = argc > 1 && std::strcmp(argv[1], "--plant") == 0;
  using namespace rdr_adapter::geostore;
  uint64_t record_frame = 0;
  std::unordered_map<uint64_t, Entry> memo;
  // capture A records 3 frames (g_record_frame advances at each present while recording; the Stop frame does not advance it)
  for (int f = 0; f < 3; ++f) {
    for (uint64_t key = 1; key <= 4; ++key) memo[key] = {record_frame, key * 1000 /* A's geometry.bin offset */};
    ++record_frame;
  }
  // the draws of the stop frame carry used_frame = record_frame (the Stop does not advance it)
  for (uint64_t key = 1; key <= 4; ++key) memo[key].used_frame = record_frame;
  // StartCapture of B in the same process
  if (!plant) BeginNewStore(record_frame, memo);
  // B's frame 0: every draw looks its key up
  int stale_hits = 0;
  for (uint64_t key = 1; key <= 4; ++key) {
    auto it = memo.find(key);
    if (it != memo.end() && UsedThisFrame(it->second.used_frame, record_frame)) ++stale_hits;  // the shortcut returns A's offset
  }
  Check(stale_hits == 0, "frame 0 of the second capture: no memo entry takes the 'used this record frame' shortcut (old store's offsets)");
  Check(plant || memo.empty(), "the memo is empty after the store starts");
  // after the fix, an entry made in the new frame 0 still takes the shortcut at its second use in the same frame
  memo[1] = {record_frame, 77};
  Check(UsedThisFrame(memo[1].used_frame, record_frame), "an entry made in the new store's frame 0 still takes the shortcut at its second use");
  std::printf("%s\n", fails ? "FAILED" : "all passed");
  return fails ? 1 : 0;
}
