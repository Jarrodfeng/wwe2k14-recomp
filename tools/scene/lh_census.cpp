// S103 LH: which models of a record's kLoModelMap (the walk's learned install table) are NOT covered by the computed classes (ring / arena-slot ids, wrestler embedded block)? Those are the entries that age
// and plan holds. Census only; 0 game runs. Usage: lh_census DIR...
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "../../src/renderer_scene/light_fixed_id.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_inputs.h"
#include "../../src/renderer_scene/rows_ring.h"
using namespace rdr_scene;
namespace fs = std::filesystem;
int main(int argc, char** argv) {
  std::vector<std::string> files;
  for (int i = 1; i < argc; ++i) { std::string a = argv[i]; if (fs::is_directory(a)) { for (auto& e : fs::directory_iterator(a)) if (e.path().extension() == ".rsc") files.push_back(e.path().string()); } else files.push_back(a); }
  std::sort(files.begin(), files.end());
  std::map<std::string, uint64_t> cls;
  std::map<std::string, std::set<uint32_t>> models;
  uint64_t frames = 0, withmap = 0;
  std::map<std::string, uint64_t> pairs;
  for (const std::string& p : files) {
    SceneTables t; std::string err;
    if (!ReadRecord(p, t, err)) continue;
    ++frames;
    std::map<uint32_t, std::string> name;
    for (size_t i = 0; i < t.objects.size() && i < t.engine_objects.size(); ++i) if (t.engine_objects[i]) name[t.engine_objects[i]] = std::string(t.Str(t.objects[i].id));
    std::set<uint32_t> ring;
    for (const RingRows& r : t.ring_rows) ring.insert(r.object);
    bool any = false;
    for (const LightObjectRow& r : t.light_objects) {
      if (r.kind != kLoModelMap) continue;
      const uint32_t n = r.aux0 < kLightModelsPerRow ? r.aux0 : kLightModelsPerRow;
      for (uint32_t k = 0; k < n; ++k) {
        const ModelMapEntry e = GetModelMapEntry(r.words, k); any = true;
        std::string nm = name.count(e.model) ? name[e.model] : "?";
        std::string c = ring.count(e.model) ? "ring" : nm.rfind("skel:", 0) == 0 ? "skel" : nm.rfind("actor:", 0) == 0 ? "actor" : nm.rfind("arena:", 0) == 0 ? "arena" : nm == "?" ? "unnamed" : nm.substr(0, nm.find(':'));
        char b[160]; std::snprintf(b, sizeof b, "%s idx %08X mode %u", c.c_str(), e.index, e.mode);
        ++cls[b]; models[b].insert(e.model);
        if (e.index == 0xFFFFFFFEu) {
          bool nodeobj = false;
          for (const LightObjectRow& q : t.light_objects) if (q.kind == kLoNodeObject && q.object == e.object) nodeobj = true;
          char d[200]; std::snprintf(d, sizeof d, "%s model %08X object %08X nodeobj %d", c.c_str(), e.model, e.object, nodeobj ? 1 : 0); ++pairs[d];
        }
      }
    }
    if (any) ++withmap;
  }
  std::printf("frames %llu with map %llu\n", (unsigned long long)frames, (unsigned long long)withmap);
  for (auto& kv : cls) std::printf("%-40s entries %llu distinct models %zu\n", kv.first.c_str(), (unsigned long long)kv.second, models[kv.first].size());
  for (auto& kv : pairs) std::printf("DIRECT %s frames %llu\n", kv.first.c_str(), (unsigned long long)kv.second);
}
