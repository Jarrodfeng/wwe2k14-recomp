// S103 LH: the planned light hold's cause and its removal, offline, 0 game runs. A walked record's kLoModelMap is the walk's learned install table at that frame; the next record's map is "what the game's walk wrote on the
// next frame". For every ARENA slot model of record j (the class bit 4 asks about and the planned hold waits on):
//   * the COMPUTED entry (light_fixed_id.h ComputeByIds: the slot's set id -> position -> object) is compared with the walk's entry of record j + 1 (index, mode, source object): `next_equal` / `next_differ`;
//   * the merge decision the latch makes when the learned entry is about to expire (DefaultMergeFor for a default-100 slot, the explicit rule otherwise) is counted: an entry that stays LEARNED (kept) is one that still
//     ages and plans a refresh walk (a held frame, a repeated picture in the stream-free build) = `would_hold`. The rule under test must leave 0 of them on these captures.
// Usage: lh_proof DIR_OR_FILE... [--plant old_rule|wrong_index|keep_all_default] [--verbose]. Exit 0 = every arena model refreshed and equal to the walk's next frame, would_hold 0.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "../../src/renderer_scene/light_fixed_id.h"
#include "../../src/renderer_scene/record_io.h"
using namespace rdr_scene;
namespace fs = std::filesystem;

static std::map<uint32_t, ModelMapEntry> MapOf(const SceneTables& t) {
  std::map<uint32_t, ModelMapEntry> m;
  for (const LightObjectRow& r : t.light_objects) {
    if (r.kind != kLoModelMap) continue;
    const uint32_t n = r.aux0 < kLightModelsPerRow ? r.aux0 : kLightModelsPerRow;
    for (uint32_t k = 0; k < n; ++k) { const ModelMapEntry e = GetModelMapEntry(r.words, k); m[e.model] = e; }
  }
  return m;
}

int main(int argc, char** argv) {
  std::vector<std::string> files;
  std::string plant;
  bool verbose = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--plant" && i + 1 < argc) plant = argv[++i];
    else if (a == "--verbose") verbose = true;
    else if (fs::is_directory(a)) { for (auto& e : fs::directory_iterator(a)) if (e.path().extension() == ".rsc") files.push_back(e.path().string()); }
    else files.push_back(a);
  }
  std::sort(files.begin(), files.end());
  uint64_t pairs = 0, models = 0, defaults = 0, explicit_ = 0, next_equal = 0, next_differ = 0, next_missing = 0, refreshed = 0, kept = 0, computed_explicit = 0, would_hold = 0, differ_now = 0, no_entry = 0;
  SceneTables prev;
  bool have_prev = false;
  const bool refresh_on = plant != "old_rule";
  for (const std::string& path : files) {
    SceneTables t;
    std::string err;
    if (!ReadRecord(path, t, err)) continue;
    if (have_prev && prev.header.guest_frame + 1 == t.header.guest_frame && LightManagerRow(prev.light_objects)) {
      ++pairs;
      // record j = prev: slot models of its arena (a7_proof's join): model -> set id, and whether the slot text named it (Object::light_set != none) or it is the loader default 100
      std::map<uint32_t, std::pair<uint32_t, bool>> arena;   // model -> (set, is_default)
      {
        std::map<uint32_t, std::pair<uint32_t, bool>> slot_set;
        for (const Object& o : prev.objects) {
          std::string_view id = prev.Str(o.id);
          if (id.rfind("arena:slot:", 0) != 0) continue;
          size_t e = id.find(':', 11);
          uint32_t k = uint32_t(std::atoi(std::string(id.substr(11, e == std::string_view::npos ? std::string_view::npos : e - 11)).c_str()));
          if (!slot_set.count(k)) slot_set[k] = o.light_set == 0xFFFFFFFFu ? std::make_pair(100u, true) : std::make_pair(o.light_set, false);
        }
        for (const StageFxRow& f : prev.stage_fx)
          if (f.kind == kFxSlotWorld && f.pad && slot_set.count(f.slot)) arena[f.pad] = slot_set[f.slot];
      }
      std::vector<FixedModel> fm;
      for (auto& kv : arena) fm.push_back(FixedModel{kv.first, FixedClass::kArena, kv.second.first, true});   // EVERY slot is treated as a loader default (these captures name every slot's set, so none is a real default): a default model is refreshed only when its learned entry equals the computed one, so this is the strictest test of the rule
      const std::map<uint32_t, ModelMapEntry> learned_j = MapOf(prev), walk_next = MapOf(t);
      std::vector<LearnedInstall> learned;
      for (auto& kv : learned_j) learned.push_back(LearnedInstall{kv.first, kv.second.object, kv.second.index, kv.second.mode});   // sorted by model (std::map)
      std::vector<ByIdEntry> es;
      ByIdCensus c = ComputeByIds(prev.light_objects, fm, learned, es);
      (void)c;
      for (ByIdEntry e : es) {
        ++models;
        if (plant == "wrong_index") ++e.index;
        const bool is_default = e.is_default;
        (is_default ? defaults : explicit_)++;
        auto lt = std::lower_bound(learned.begin(), learned.end(), e.model, [](const LearnedInstall& x, uint32_t v) { return x.model < v; });
        const LearnedInstall* li = lt != learned.end() && lt->model == e.model ? &*lt : nullptr;
        if (!li) ++no_entry;
        // the decision the latch makes (inputs_contrib.h ApplyLightIds): who ends up computed (never ages) and who stays learned (ages: a planned walk)
        bool computed = true;
        if (KeepLearned(li, e)) computed = false;
        else if (is_default) {
          const DefaultMerge dm = plant == "keep_all_default" ? DefaultMerge::kKeep : DefaultMergeFor(li, e, refresh_on);
          if (dm == DefaultMerge::kKeep) { computed = false; if (li && (li->index != e.index || li->mode != e.mode)) ++differ_now; }
          else if (dm == DefaultMerge::kRefresh) ++refreshed;
        } else ++computed_explicit;
        if (!computed) { ++kept; if (li) ++would_hold; }   // a kept learned entry is one that ages: the planned hold waits on it
        // the walk's entry of the NEXT record: what the game's walk wrote one frame later
        auto wn = walk_next.find(e.model);
        if (wn == walk_next.end()) { ++next_missing; continue; }
        if (wn->second.index == e.index && wn->second.mode == e.mode) ++next_equal;   // (index, mode): the object is resolved from them; the record rows are deduplicated
        else {
          ++next_differ;
          if (verbose && next_differ < 6) std::printf("  next differs model %08X: computed idx %u mode %u obj %08X, walk idx %08X mode %u obj %08X\n", e.model, e.index, e.mode, e.object, wn->second.index, wn->second.mode, wn->second.object);
        }
      }
    }
    prev = std::move(t);
    have_prev = true;
  }
  std::printf("record pairs %llu, arena slot models %llu (default-100 %llu, explicit %llu, no learned entry %llu)\n", (unsigned long long)pairs, (unsigned long long)models, (unsigned long long)defaults, (unsigned long long)explicit_, (unsigned long long)no_entry);
  std::printf("refreshed_default %llu, computed_explicit %llu, kept_learned %llu (differ now %llu): would_hold %llu\n", (unsigned long long)refreshed, (unsigned long long)computed_explicit, (unsigned long long)kept, (unsigned long long)differ_now, (unsigned long long)would_hold);
  std::printf("computed entry == the walk's entry one frame later: equal %llu, differ %llu, (model not in the next map %llu)\n", (unsigned long long)next_equal, (unsigned long long)next_differ, (unsigned long long)next_missing);
  const bool ok = pairs > 0 && models > 0 && would_hold == 0 && next_differ == 0 && next_equal > 0;
  if (pairs == 0 || models == 0) std::printf("FAIL LH EMPTY: no arena models found on these records\n");
  if (would_hold) std::printf("FAIL LH HOLD: %llu arena entries stay walk-learned and would plan a refresh hold\n", (unsigned long long)would_hold);
  if (next_differ) std::printf("FAIL LH NEXT: %llu computed entries differ from the walk's entry of the next frame\n", (unsigned long long)next_differ);
  std::printf("%s\n", ok ? "PASS: every arena entry is refreshed from the record and equals the walk's next frame; 0 would-hold" : "FAIL: see above");
  return ok ? 0 : 1;
}
