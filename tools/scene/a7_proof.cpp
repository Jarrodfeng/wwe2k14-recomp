// S103 A7: the fixed-set-id proof (S103_FABLE_REVIEW.md 1.5), offline, 0 game runs. For every walk-recorded LightRef of a record (a ring, wrestler or learned-class model) the
// light state R must equal, byte for byte, the block the FIXED-ID rule picks: id (the constant of the class's init code) -> position in the manager's id list (sub_827F6A40)
// -> sub_826203E8's override rule + sub_827F6828's remap (rdr_scene::ResolveInstallRow) -> the object's block row. Usage:
//   a7_proof DIR_OR_FILE... [--plant NAME] [--verbose]
// Plants (each must change the verdict): ring30for31, remap_ignored, override_ignored. Exit 0 = every non-modified class equal on every ref.
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

struct Tally {
  uint64_t equal = 0, differ = 0, nohole = 0, equal_prev = 0, differ_both = 0;   // equal_prev: differs from this latch's block but equals the PREVIOUS record's block (the walk copied the object before the update task wrote it again)   // nohole: the rule picks no block (absent id, unreadable row)
  int first_word = -1;
  std::set<int> diff_words;
  std::string first_why;
};

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
  InstallFaults ft;
  if (plant == "remap_ignored") ft.ignore_remap = true;
  if (plant == "override_ignored") ft.ignore_override = true;
  std::map<std::string, Tally> tally;
  uint64_t arena100 = 0, frames = 0, bad_frames = 0, with_remap = 0, with_override = 0, with_pool = 0;
  std::set<std::string> idlists;
  SceneTables prev;
  bool have_prev = false;
  for (const std::string& path : files) {
    SceneTables t;
    std::string err;
    if (!ReadRecord(path, t, err)) { ++bad_frames; continue; }
    const LightObjectRow* m = LightManagerRow(t.light_objects);
    if (!m) { ++bad_frames; continue; }
    ++frames;
    if (m->remap != 0 && m->remap != -1) ++with_remap;
    if (m->aux1) ++with_pool;
    if (m->words[kLmOverrideMode0] != 0xFFFFFFFFu || m->words[kLmOverrideMode1] != 0xFFFFFFFFu) ++with_override;
    {
      std::string s;
      for (uint32_t i = 0; i < m->aux0 && i < kLightMaxSets; ++i) s += std::to_string(m->words[kLmSetInfoBase + i] & 0xFFFF) + ",";
      idlists.insert(s);
    }
    std::map<uint32_t, uint8_t> ringcls;
    for (const RingRows& r : t.ring_rows) ringcls[r.object] = r.klass;
    std::set<uint32_t> wrestler;
    for (size_t i = 0; i < t.objects.size() && i < t.engine_objects.size(); ++i) {
      if (!t.engine_objects[i]) continue;
      std::string_view id = t.Str(t.objects[i].id);
      if (id.rfind("skel:", 0) == 0 || id.rfind("actor:", 0) == 0) wrestler.insert(t.engine_objects[i]);
    }
    // arena: model -> slot (kFxSlotWorld rows: slot + the slot's model object), slot -> the slot text's l(N) (Object::light_set of "arena:slot:K"; none = the default set 100)
    std::map<uint32_t, uint32_t> arena_set;   // model -> expected set id
    {
      std::map<uint32_t, uint32_t> slot_set;
      for (const Object& o : t.objects) {
        std::string_view id = t.Str(o.id);
        if (id.rfind("arena:slot:", 0) != 0) continue;
        size_t e = id.find(':', 11);
        uint32_t k = uint32_t(std::atoi(std::string(id.substr(11, e == std::string_view::npos ? std::string_view::npos : e - 11)).c_str()));
        if (!slot_set.count(k)) slot_set[k] = o.light_set == 0xFFFFFFFFu ? 100u : o.light_set;
      }
      for (const StageFxRow& f : t.stage_fx)
        if (f.kind == kFxSlotWorld && f.pad && slot_set.count(f.slot)) arena_set[f.pad] = slot_set[f.slot];
    }
    for (const LightRef& r : t.light_refs) {
      if (r.state >= t.light_states.size()) continue;
      FixedClass fc = FixedClass::kUnknown;
      uint32_t arena_id = 0;
      if (arena_set.count(r.object)) { fc = FixedClass::kArena; arena_id = arena_set[r.object]; if (arena_id == 100) ++arena100; }
      auto rc = ringcls.find(r.object);
      if (rc != ringcls.end()) fc = FixedClassOfRing(rc->second);
      else if (wrestler.count(r.object)) fc = FixedClass::kWrestler;
      if (fc == FixedClass::kUnknown) continue;   // arena / crowd: see the tool's second mode (learned class), not tested here
      FixedPick pick = fc == FixedClass::kArena ? FixedPick{arena_id, 1} : FixedIdFor(fc, plant == "ring30for31");
      Tally& ty = tally[FixedClassName(fc)];
      const LightObjectRow* row = nullptr;
      const char* why = "";
      if (!ResolveFixedId(t.light_objects, pick, row, why, &ft)) {
        ++ty.nohole;
        if (ty.first_why.empty()) ty.first_why = why;
        continue;
      }
      const uint32_t* st = reinterpret_cast<const uint32_t*>(t.light_states[r.state].light);
      if (fc == FixedClass::kWrestler) {   // sub_8259F630 (0x8259F654-0x8259F680): the embedded object = a copy of the BLEND object (mgr+376) when mgr+380 == 1, else of set [char+3920]: test the blend as well
        const uint32_t bref = m->words[kLmRefBase + kLmRefBlend];
        Tally& by = tally["wrestler vs blend 376"];
        if (bref == kLightNoRow || bref >= t.light_objects.size()) ++by.nohole;
        else {
          const uint32_t* bw = t.light_objects[bref].words;
          bool eq = std::memcmp(st, bw, 4 * kLsLightFloats) == 0, eqp = false;
          if (!eq && have_prev && prev.header.guest_frame + 1 == t.header.guest_frame) {
            const LightObjectRow* pm = LightManagerRow(prev.light_objects);
            const uint32_t pr = pm ? pm->words[kLmRefBase + kLmRefBlend] : kLightNoRow;
            eqp = pr != kLightNoRow && pr < prev.light_objects.size() && std::memcmp(st, prev.light_objects[pr].words, 4 * kLsLightFloats) == 0;
          }
          if (eq) ++by.equal; else if (eqp) ++by.equal_prev; else {
            ++by.differ;
            const bool set10 = std::memcmp(st, row->words, 4 * kLsLightFloats) == 0;
            Tally& nb = tally["wrestler: neither blend nor set 10"];
            if (set10) ++tally["wrestler: differs from blend, equals set 10"].equal; else { ++nb.differ; if (verbose && nb.differ < 6) std::printf("  neither %s obj %08X frame %llu\n", path.c_str(), r.object, (unsigned long long)t.header.guest_frame); }
          }
        }
      }
      int fw = -1;
      for (uint32_t w = 0; w < kLsLightFloats; ++w) if (st[w] != row->words[w]) { if (fw < 0) fw = int(w); ty.diff_words.insert(int(w)); }
      bool lag1 = false;
      if (fw >= 0 && have_prev && prev.header.guest_frame + 1 == t.header.guest_frame) {
        const LightObjectRow* prow = nullptr;
        const char* pwhy = "";
        if (ResolveFixedId(prev.light_objects, pick, prow, pwhy, &ft)) lag1 = std::memcmp(st, prow->words, 4 * kLsLightFloats) == 0;
      }
      if (fw < 0) ++ty.equal;
      else if (lag1) ++ty.equal_prev;
      else { ++ty.differ, ++ty.differ_both; if (ty.first_word < 0) ty.first_word = fw; if (verbose && ty.differ < 4) std::printf("  differ %s %s obj %08X word %d state %08X block %08X\n", path.c_str(), FixedClassName(fc), r.object, fw, st[fw], row->words[fw]); }
    }
    prev = t;
    have_prev = true;
  }
  std::printf("frames %llu (unreadable %llu), manager remap!=0 in %llu, override word set in %llu, pool>0 in %llu, distinct id lists %zu\n", (unsigned long long)frames, (unsigned long long)bad_frames, (unsigned long long)with_remap, (unsigned long long)with_override, (unsigned long long)with_pool, idlists.size());
  std::printf("arena refs whose slot set is 100 (the loader default OR an explicit l(100): the record cannot tell): %llu\n", (unsigned long long)arena100);
  uint64_t bad = 0;
  for (auto& kv : tally) {
    const Tally& y = kv.second;
    std::printf("%-22s equal %llu equal_to_previous_latch %llu differ %llu no_block %llu", kv.first.c_str(), (unsigned long long)y.equal, (unsigned long long)y.equal_prev, (unsigned long long)y.differ, (unsigned long long)y.nohole);
    if (y.differ) { std::printf("  first differing word %d, %zu distinct words:", y.first_word, y.diff_words.size()); int n = 0; for (int w : y.diff_words) { if (n++ >= 12) break; std::printf(" %d", w); } }
    if (!y.first_why.empty()) std::printf("  (%s)", y.first_why.c_str());
    std::printf("\n");
    if (std::string(kv.first).find("wrestler") == std::string::npos && (y.differ || y.nohole)) bad += y.differ + y.nohole;   // the wrestler rows are informational (INPUTS_A7.md: the embedded copy is not an id's object)
  }
  std::printf("%s\n", bad ? "FAIL: a fixed-id class differs from its walked state" : "PASS: every non-modified class equals its walked state");
  return bad ? 1 : 0;
}
