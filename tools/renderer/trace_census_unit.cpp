// S103 lane T: unit test of src\scene_live\trace_census_core.h (GPU free, no game). Exit 0 = pass. `trace_census_unit N` runs planted fault N and must exit 1:
//   1 the first call is not logged        2 a new key is not reported       3 distinct keys collapse into one (key ignores the second value)
//   4 the 17th distinct key vanishes without the overflow count       5 the 600th call is not logged
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "../../src/scene_live/trace_census_core.h"

static int g_fail = 0;
static void need(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

int main(int argc, char** argv) {
  const int plant = argc > 1 ? std::atoi(argv[1]) : 0;
  using namespace trace_census;
  {
    Table t;
    Result r = Note(t, 0x82C2FB14, 0, 0);
    need(r.log == (plant != 1) && r.new_key == (plant != 2), "first call: logged, a new key");
    r = Note(t, 0x82C2FB14, 0, 0);
    need(!r.log && !r.new_key && r.slot == 0, "second call, same key: silent, slot 0");
    r = Note(t, 0x82C2FB14, 5, 7);
    r = Note(t, 0x82C2FB14, 5, plant == 3 ? 7 : 8);
    need(t.used.load() == 3, "distinct (lr, a, b) are distinct keys: three keys so far");
    std::string s = Format(Site::PlaneBind, t, r, 3700, 1);
    need(s.find("sub_827F9D88 call 4 NEW KEY (frame 3700, scene mode 1)") == 0, "Format: name, call number, NEW KEY, frame, mode");
    need(s.find("0x82C2FB14 0x5 0x8 x1;") != std::string::npos, "Format: the key and its count");
  }
  {
    Table t;
    bool logged600 = false;
    for (uint64_t i = 1; i <= kEvery; ++i) {
      const Result r = Note(t, 0x1, 0, 0);
      if (i == kEvery) logged600 = r.log;
      if (i > 1 && i < kEvery && r.log) need(false, "no line between the first and the 600th call for one key");
    }
    need(logged600 == (plant != 5), "the 600th call is logged");
    need(t.n[0].load() == kEvery, "the key counted every call");
  }
  {
    Table t;
    for (uint32_t k = 0; k < 20; ++k) Note(t, 0x100 + k, 0, 0);
    need(t.used.load() == kKeys, "16 keys fit");
    need(t.overflow.load() == (plant == 4 ? 0u : 4u), "4 calls overflowed and were counted");
    need(t.total.load() == 20, "total counts every call");
  }
  {
    Table t;
    std::vector<std::thread> th;
    for (int i = 0; i < 4; ++i) th.emplace_back([&] { for (int j = 0; j < 20000; ++j) Note(t, 0x10 + uint32_t(j % 5), 0, 0); });
    for (auto& x : th) x.join();
    uint64_t sum = 0;
    for (int i = 0; i < t.used.load(); ++i) sum += t.n[i].load();
    need(t.used.load() == 5 && sum == 80000 && t.total.load() == 80000, "four threads: 5 keys, no lost count");
  }
  std::printf("%s\n", g_fail ? "FAILED" : "all passed");
  return g_fail ? 1 : 0;
}
