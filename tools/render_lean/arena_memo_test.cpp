// S102 RT1 (Q7): the arena of a scene record found once per record (render_lean bit 2) against the shipped rule, over many records; `plant2` = the memo is a one-slot memo of the LAST record's answer
// (a key that misses a changed record) and must be caught.
//   built and run by tools\render_lean\lean_unit.py (links ring_ours_core.cpp, arena_ours_core.cpp, record_io.cpp)
#include <cstdio>
#include <string>
#include <vector>

#include "../../src/renderer/render_lean.h"
#include "../../src/renderer_scene/arena_ours_core.h"
#include "../../src/renderer_scene/ring_ours_core.h"

using namespace rdr_scene;

static std::shared_ptr<SceneTables> Make(const std::vector<std::string>& pacs) {
  auto t = std::make_shared<SceneTables>();
  for (const std::string& p : pacs) {
    AssetRef a;
    a.pac = t->AddString(p);
    t->assets.push_back(a);
  }
  return t;
}

int main(int argc, char** argv) {
  const bool plant6 = argc > 1 && std::string(argv[1]) == "plant6";
  const bool plant = argc > 1 && (std::string(argv[1]) == "plant2" || plant6);
  std::vector<std::shared_ptr<SceneTables>> records;
  records.push_back(Make({"pac\\ch\\CH001.PAC", "pac\\bg05.pac"}));
  records.push_back(Make({"pac\\bg09.pac", "pac\\bg05.pac"}));
  records.push_back(Make({"pac\\ch\\CH002.PAC"}));
  records.push_back(Make({}));
  records.push_back(Make({"pac\\bg12.pac"}));
  int bad = 0;
  std::vector<int> want;
  for (auto& r : records) want.push_back(RingCore::ArenaOfRecord(*r));   // lean off: the shipped rule (also fills nothing)
  rdr::g_lean_hooks.store(rdr::kLeanFacts);
  if (plant) rdr::g_lean_plant.store(plant6 ? 6 : 2);
  for (int pass = 0; pass < 3; ++pass) {
    for (size_t i = 0; i < records.size(); ++i) {
      const int got_r = RingCore::ArenaOfRecord(*records[i]), got_a = ArenaCore::ArenaOfRecord(*records[i]);
      if (got_r != want[i] || got_a != want[i]) {
        ++bad;
        std::printf("DIFF pass %d record %zu: ring %d arena %d, shipped rule %d\n", pass, i, got_r, got_a, want[i]);
      }
    }
  }
  // pooled tables (SW1 TablesPool): one table refilled in place across records of different arenas must follow the shipped rule each time
  {
    SceneTables pooled;
    const std::vector<std::vector<std::string>> seq = {{"pac\\bg05.pac"}, {"pac\\ch\\CH001.PAC"}, {"pac\\bg09.pac"}, {}, {"pac\\bg12.pac"}};
    const int expect[] = {5, -1, 9, -1, 12};
    for (size_t k = 0; k < seq.size(); ++k) {
      pooled.Recycle();
      for (const std::string& p : seq[k]) { AssetRef a; a.pac = pooled.AddString(p); pooled.assets.push_back(a); }
      const int got = RingCore::ArenaOfRecord(pooled);
      if (got != expect[k]) { ++bad; std::printf("DIFF pooled table, record %zu: got %d, the shipped rule says %d\n", k, got, expect[k]); }
    }
  }
  const uint64_t skipped = rdr::LeanGet(rdr::kLcShadowSkip), ran = rdr::LeanGet(rdr::kLcShadowRun);
  std::printf("%s: %zu records x 3 passes x 2 rules, %d differing, memo skips %llu, computed %llu\n", plant ? "plant2" : "real", records.size(), bad, static_cast<unsigned long long>(skipped), static_cast<unsigned long long>(ran));
  if (!plant && skipped == 0) { std::printf("FAIL the memo never took\n"); return 1; }
  return bad ? 1 : 0;
}
