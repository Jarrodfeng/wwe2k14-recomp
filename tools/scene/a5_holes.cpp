// S103 A5: names the light-table models of a scene record (record_io only; no game): the model map entries (model, light object, index, mode) joined to the record's objects by engine object.
// Usage: a5_holes FILE.rsc. Prints the install groups (index/mode/object -> models and the record object ids those models carry) and the objects with an engine object that have NO map entry.
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_inputs.h"
using namespace rdr_scene;
int main(int argc, char** argv) {
  if (argc < 2) return 1;
  SceneTables t;
  std::string err;
  if (!ReadRecord(argv[1], t, err)) { std::fprintf(stderr, "FAIL %s\n", err.c_str()); return 2; }
  std::map<uint32_t, ModelMapEntry> map;
  for (const LightObjectRow& r : t.light_objects)
    if (r.kind == kLoModelMap)
      for (uint32_t k = 0; k < r.aux0; ++k) { ModelMapEntry e = GetModelMapEntry(r.words, k); map[e.model] = e; }
  {   // the arena slots the record EMITS (visible base rows "arena:slot:K"; twins ":r" / ":y" are the same slot) vs the invisible ones
    std::set<std::string> vis, hid;
    for (const Object& o : t.objects) {
      std::string id(t.Str(o.id));
      if (id.rfind("arena:slot:", 0) != 0) continue;
      id = id.substr(11, id.find(':', 11) == std::string::npos ? std::string::npos : id.find(':', 11) - 11);
      ((o.flags & kObjVisible) ? vis : hid).insert(id);
    }
    std::printf("arena slots emitted visible %zu, listed invisible %zu\n", vis.size(), hid.size());
  }
  std::map<uint32_t, std::string> name;
  for (size_t i = 0; i < t.objects.size() && i < t.engine_objects.size(); ++i)
    if (t.engine_objects[i]) name[t.engine_objects[i]] += std::string(t.Str(t.objects[i].id)) + " ";
  std::printf("map entries %zu, objects %zu, engine_objects %zu, named engine objects %zu\n", map.size(), t.objects.size(), t.engine_objects.size(), name.size());
  std::map<std::string, std::vector<uint32_t>> groups;
  for (auto& kv : map) {
    char b[96];
    std::snprintf(b, sizeof b, "index %08X mode %u object %08X", kv.second.index, kv.second.mode, kv.second.object);
    groups[b].push_back(kv.first);
  }
  for (auto& g : groups) {
    std::printf("%s: %zu models\n", g.first.c_str(), g.second.size());
    for (size_t i = 0; i < g.second.size() && i < 6; ++i) std::printf("    %08X %s\n", g.second[i], name.count(g.second[i]) ? name[g.second[i]].c_str() : "(no record object)");
  }
  size_t nohole = 0;
  for (auto& kv : name) if (!map.count(kv.first)) { ++nohole; std::printf("NO ENTRY %08X %s\n", kv.first, kv.second.c_str()); }
  std::printf("engine objects without an entry: %zu\n", nohole);
  return 0;
}
