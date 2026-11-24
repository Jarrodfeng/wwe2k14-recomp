// S103 A8: the wrestlers' light object at the latch, offline (0 game runs, 0 Surface runs). Docs: docs\renderer\INPUTS_A8.md.
//   a8_proof DIR... [--plant NAME] [--verbose]    replay over scene-record directories (consecutive frames), the SOURCES table, then the REPLAY of the built module (renderer_scene\wrestler_light_latch.h)
//   a8_proof --unit [--plant NAME]                the module's synthetic checks
// SOURCES (informational): the walked wrestler LightState of each ref against every source the latch could read, at the same frame (the previous record's latch: E(N-1)), this one and the one before:
//   E = the wrestler's EMBEDDED object (char + 3936; block char + 3984; a kLoNodeObject row), B = the BLEND object (mgr + 376), S = set 10's object. sub_8259F630 (0x8259F654-0x8259F680) fills E from B (mgr + 380 == 1) or S.
//   "changed" = the model's E differs from its E one latch earlier (a static light proves nothing about a stale read: A6 review 1).
// REPLAY (the verdict): per record j, the module's Store is fed each record's latch rows (LatchFromRecord); Apply(record j, synthesize=false) is the mirror census; Apply on a copy of record j with the actors' walked
//   refs removed (synthesize=true) writes states that must equal the removed walked ones. Plants: stale, early, wrong_slot, always_equal, no_changed_gate, no_walk_guard (each must change the verdict).
// Exit 0 = pass.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "../../src/renderer_scene/light_install.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_inputs.h"
#include "../../src/renderer_scene/rows_lights.h"
#include "../../src/renderer_scene/wrestler_light_latch.h"
using namespace rdr_scene;
namespace fs = std::filesystem;

static bool Eq(const uint32_t* a, const uint32_t* b) { return std::memcmp(a, b, 4 * kLsLightFloats) == 0; }
static int g_bad = 0;
#define CHECK(name, cond) do { if (!(cond)) { std::printf("FAIL %s\n", name); ++g_bad; } } while (0)

static wlight::Faults FaultsOf(const std::string& plant) {
  wlight::Faults f;
  if (plant == "stale") f.stale = true;
  if (plant == "early") f.early = true;
  if (plant == "wrong_slot") f.wrong_slot = true;
  if (plant == "always_equal") f.always_equal = true;
  if (plant == "no_changed_gate") f.no_changed_gate = true;
  if (plant == "no_walk_guard") f.no_walk_guard = true;
  if (plant == "ignore_ours") f.ignore_ours = true;
  return f;
}

// ---- the module's synthetic checks -------------------------------------------------------------------------------------------------------------------------------------------------------------------
static wlight::Entry MkEntry(uint32_t slot, uint32_t seed) {
  wlight::Entry e;
  e.slot = slot, e.character = 0xA0000000u + slot * 0x10000u;
  for (uint32_t i = 0; i < kLsLightFloats; ++i) e.block[i] = seed * 1000u + i;
  return e;
}
static void MkRecord(SceneTables& t, uint32_t header, uint32_t slot, uint32_t engine) {
  t.header.guest_frame = header;
  t.strings.clear();
  Object o;
  o.id = uint32_t(t.strings.size()) + 1;   // Str(field): field - 1 is the offset
  t.strings += "actor:slot:" + std::to_string(slot);
  t.strings.push_back('\0');
  t.objects.push_back(o);
  t.engine_objects.push_back(engine);
}
static void AddWalked(SceneTables& t, uint32_t engine, uint32_t frame, const wlight::Entry& like, int perturb_word) {
  LightState s = {};
  std::memcpy(s.light, like.block, sizeof s.light);
  if (perturb_word >= 0) { uint32_t w; std::memcpy(&w, &s.light[perturb_word], 4); w ^= 1u; std::memcpy(&s.light[perturb_word], &w, 4); }
  LightRef r;
  r.object = engine, r.state = uint32_t(t.light_states.size()), r.frame = frame;
  t.light_states.push_back(s);
  t.light_refs.push_back(r);
}
static int Unit(const std::string& plant) {
  const wlight::Faults ft = FaultsOf(plant);
  const wlight::Faults* fp = plant.empty() ? nullptr : &ft;
  // record 100: ref frame 99, the same-frame latch = hook 98; the previous hook 97 (the changed baseline); hook 99 = the NEXT frame's (a wrong pick).
  wlight::Store st;
  auto put = [&](uint32_t hook, uint32_t slot, uint32_t seed) { std::vector<wlight::Entry> v; v.push_back(MkEntry(slot, seed)); st.Put(hook, std::move(v)); };
  put(97, 3, 1), put(98, 3, 2), put(99, 3, 3);
  { std::vector<wlight::Entry> v; v.push_back(MkEntry(3, 3)); v.push_back(MkEntry(4, 9)); st.Put(96, std::move(v)); }   // hook 96: another slot's block, never the answer
  {   // U1 / U2: a walked state equal to the same-frame block is equal; one word off is a DIFFER on a changed block
    SceneTables t; MkRecord(t, 100, 3, 0xB0000001u);
    AddWalked(t, 0xB0000001u, 99, MkEntry(3, 2), -1);
    wlight::Census c;
    wlight::Apply(t, st, false, c, fp);
    CHECK("A8 CENSUS equal", c.eq == 1 && c.differ == 0 && c.changed == 1 && c.eq_changed == 1);
    SceneTables t2; MkRecord(t2, 100, 3, 0xB0000001u);
    AddWalked(t2, 0xB0000001u, 99, MkEntry(3, 2), 17);
    wlight::Census c2;
    wlight::Apply(t2, st, false, c2, fp);
    CHECK("A8 CENSUS differ", c2.differ == 1 && c2.differ_changed == 1 && c2.eq == 0);
    CHECK("A8 HOOK", c.eq == 1);   // the stale / early twins read hook 97 / 99: the equal walked state then differs
  }
  {   // U3: a static block (hook 98 == hook 97's) is not a changed frame
    wlight::Store s2;
    { std::vector<wlight::Entry> v; v.push_back(MkEntry(3, 5)); s2.Put(97, std::move(v)); }
    { std::vector<wlight::Entry> v; v.push_back(MkEntry(3, 5)); s2.Put(98, std::move(v)); }
    SceneTables t; MkRecord(t, 100, 3, 0xB0000001u);
    AddWalked(t, 0xB0000001u, 99, MkEntry(3, 5), 4);   // walked differs on a static light
    wlight::Census c;
    wlight::Apply(t, s2, false, c, fp);
    CHECK("A8 CHANGED GATE", c.differ == 1 && c.changed == 0 && c.differ_changed == 0);
  }
  {   // U4 / U5: synthesis writes only where the walk has no ref; ref frame = header - 1; the state equals the block; a walked ref is never overwritten
    SceneTables t; MkRecord(t, 100, 3, 0xB0000001u);
    wlight::Census c;
    const uint32_t n = wlight::Apply(t, st, true, c, fp);
    CHECK("A8 SYNTH written", n == 1 && t.light_refs.size() == 1 && t.light_states.size() == 1 && t.light_refs[0].frame == 99 && t.light_refs[0].object == 0xB0000001u);
    CHECK("A8 SYNTH block", t.light_states.size() == 1 && std::memcmp(t.light_states[0].light, MkEntry(3, 2).block, sizeof t.light_states[0].light) == 0);
    SceneTables w; MkRecord(w, 100, 3, 0xB0000001u);
    AddWalked(w, 0xB0000001u, 99, MkEntry(3, 2), -1);
    wlight::Census cw;
    const uint32_t nw = wlight::Apply(w, st, true, cw, fp);
    CHECK("A8 WALK GUARD", nw == 0 && w.light_refs.size() == 1);
  }
  {   // U6: the join is by slot; a slot with no entry is counted, never filled
    SceneTables t; MkRecord(t, 100, 5, 0xB0000002u);
    wlight::Census c;
    const uint32_t n = wlight::Apply(t, st, true, c, fp);
    CHECK("A8 NO ENTRY", n == 0 && c.no_entry == 1 && t.light_refs.empty());
  }
  {   // U7: a walked / fallback frame (the scene decision did not say OURS) never gets a synthetic state; an OURS frame does, and the state is marked kLsSynthetic
    SceneTables t; MkRecord(t, 100, 3, 0xB0000001u);
    auto not_ours = [](uint64_t) { return false; };
    auto ours = [](uint64_t f) { return f == 99; };
    CHECK("A8 OURS GATE", !wlight::ShouldSynthesize(true, +not_ours, t, fp) && !wlight::ShouldSynthesize(true, nullptr, t, fp) && !wlight::ShouldSynthesize(false, +ours, t, fp));
    CHECK("A8 OURS GATE on", wlight::ShouldSynthesize(true, +ours, t, fp) || plant == "ignore_ours");
    wlight::Census c;
    wlight::Apply(t, st, true, c, fp);
    CHECK("A8 SYNTH flag", t.light_states.size() == 1 && (t.light_states[0].flags & kLsSynthetic));
  }
  std::printf("%s\n", g_bad ? "FAIL: a8 unit" : "PASS: a8 unit");
  return g_bad ? 1 : 0;
}

int main(int argc, char** argv) {
  std::vector<std::string> dirs;
  std::string plant;
  bool verbose = false, unit = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--plant" && i + 1 < argc) plant = argv[++i];
    else if (a == "--verbose") verbose = true;
    else if (a == "--unit") unit = true;
    else dirs.push_back(a);
  }
  if (unit) return Unit(plant);
  const wlight::Faults ft = FaultsOf(plant);
  const wlight::Faults* fp = plant.empty() ? nullptr : &ft;
  uint64_t tot_refs = 0, tot_changed = 0, no_src = 0, none = 0, none_ch = 0;
  uint64_t cand_eq[9] = {}, cand_eq_ch[9] = {};
  static const char* kCand[9] = {"E(N)", "E(N-1) [same frame]", "E(N-2)", "B(N)", "B(N-1) [same frame]", "B(N-2)", "S10(N)", "S10(N-1) [same frame]", "S10(N-2)"};
  std::map<std::string, uint64_t> why;
  uint64_t emb_total = 0, emb_b = 0, emb_s = 0, emb_none = 0;
  wlight::Census cen, cen_synth;
  uint64_t syn_eq = 0, syn_diff = 0, syn_total = 0;
  uint64_t records = 0;
  for (const std::string& d : dirs) {
    std::vector<std::string> files;
    for (auto& e : fs::directory_iterator(d)) if (e.path().extension() == ".rsc") files.push_back(e.path().string());
    std::sort(files.begin(), files.end());
    std::vector<SceneTables> ring(3);
    std::vector<uint64_t> fr(3, 0);
    std::vector<std::map<uint32_t, std::vector<uint32_t>>> emb(3);
    wlight::Store store;
    int have = 0;
    uint64_t prev_frame = 0;
    for (const std::string& path : files) {
      SceneTables t;
      std::string err;
      if (!ReadRecord(path, t, err)) continue;
      const uint64_t frame = t.header.guest_frame;
      if (have && frame != prev_frame + 1) { have = 0; store.Reset(); }
      prev_frame = frame;
      std::rotate(ring.rbegin(), ring.rbegin() + 1, ring.rend());
      std::rotate(emb.rbegin(), emb.rbegin() + 1, emb.rend());
      std::rotate(fr.rbegin(), fr.rbegin() + 1, fr.rend());
      ring[0] = std::move(t);
      fr[0] = frame;
      emb[0].clear();
      have = std::min(have + 1, 3);
      ++records;
      SceneTables& r0 = ring[0];
      // ---- REPLAY of the module ----
      {
        wlight::Latch L;
        wlight::LatchFromRecord(r0, L);
        std::vector<wlight::Entry> ents = L.entries;
        // the record's latch is stored BEFORE its own Apply (the writer applies record j with the hook j - 2 latch, which an earlier record put there)
        SceneTables a = r0;
        wlight::Apply(a, store, false, cen, fp);
        SceneTables b = r0;
        const uint32_t rf = wlight::RefFrameOfRecord(b);
        std::set<uint32_t> actors;
        for (size_t i = 0; i < b.objects.size() && i < b.engine_objects.size(); ++i) { uint32_t s; if (b.engine_objects[i] && wlight::ActorSlot(b.Str(b.objects[i].id), s)) actors.insert(b.engine_objects[i]); }
        std::vector<LightRef> removed, kept;
        for (const LightRef& x : b.light_refs) (actors.count(x.object) && x.frame == rf ? removed : kept).push_back(x);
        b.light_refs = kept;
        const size_t before = b.light_refs.size();
        wlight::Apply(b, store, true, cen_synth, fp);
        for (size_t i = before; i < b.light_refs.size(); ++i) {
          const LightRef& sr = b.light_refs[i];
          for (const LightRef& orig : removed) {
            if (orig.object != sr.object || orig.state >= r0.light_states.size()) continue;
            ++syn_total;
            const bool same = Eq(reinterpret_cast<const uint32_t*>(b.light_states[sr.state].light), reinterpret_cast<const uint32_t*>(r0.light_states[orig.state].light));
            (same ? syn_eq : syn_diff)++;
          }
        }
        store.Put(L.hook, std::move(ents));
      }
      // ---- SOURCES ----
      const LightObjectRow* m = LightManagerRow(r0.light_objects);
      if (!m) continue;
      std::set<uint32_t> wr;
      for (size_t i = 0; i < r0.objects.size() && i < r0.engine_objects.size(); ++i) {
        if (!r0.engine_objects[i]) continue;
        std::string_view id = r0.Str(r0.objects[i].id);
        if (id.rfind("skel:", 0) == 0 || id.rfind("actor:", 0) == 0) wr.insert(r0.engine_objects[i]);
      }
      std::map<uint32_t, uint32_t> src_of;
      for (const LightObjectRow& r : r0.light_objects) {
        if (r.kind != kLoModelMap) continue;
        for (uint32_t k = 0; k < r.aux0; ++k) { ModelMapEntry e = GetModelMapEntry(r.words, k); if (wr.count(e.model)) src_of[e.model] = e.object; }
      }
      for (auto& kv : src_of)
        for (const LightObjectRow& r : r0.light_objects)
          if (r.kind == kLoNodeObject && r.object == kv.second) emb[0][kv.first] = std::vector<uint32_t>(r.words, r.words + kLsLightFloats);
      auto blend = [&](const SceneTables& s) -> const uint32_t* { const LightObjectRow* mm = LightManagerRow(s.light_objects); if (!mm) return nullptr; uint32_t b = mm->words[kLmRefBase + kLmRefBlend]; return b < s.light_objects.size() && b != kLightNoRow ? s.light_objects[b].words : nullptr; };
      auto set10 = [&](const SceneTables& s) -> const uint32_t* { const LightObjectRow* mm = LightManagerRow(s.light_objects); if (!mm) return nullptr; for (uint32_t i = 0; i < mm->aux0 && i < kLightMaxSets; ++i) if ((mm->words[kLmSetInfoBase + i] & 0xFFFF) == 10) { uint32_t b = mm->words[kLmRefBase + i]; return b < s.light_objects.size() && b != kLightNoRow ? s.light_objects[b].words : nullptr; } return nullptr; };
      const uint32_t* bN[3] = {}; const uint32_t* sN[3] = {};
      for (int k = 0; k < 3 && k < have; ++k) bN[k] = blend(ring[k]), sN[k] = set10(ring[k]);
      for (auto& kv : emb[0]) {
        ++emb_total;
        const uint32_t* e = kv.second.data();
        if (bN[0] && Eq(e, bN[0])) ++emb_b;
        else if (sN[0] && Eq(e, sN[0])) ++emb_s;
        else {
          ++emb_none;
          std::string k = "E equals neither blend(N) nor set10(N): ";
          bool f = false;
          for (int g = 1; g < 3 && g < have; ++g) {
            if (bN[g] && Eq(e, bN[g])) { k += "= blend(N-" + std::to_string(g) + ") "; f = true; }
            if (sN[g] && Eq(e, sN[g])) { k += "= set10(N-" + std::to_string(g) + ") "; f = true; }
          }
          if (!f) {
            int db = 0, ds = 0;
            for (uint32_t w = 0; w < kLsLightFloats; ++w) { if (bN[0] && e[w] != bN[0][w]) ++db; if (sN[0] && e[w] != sN[0][w]) ++ds; }
            k += "no source at N-1 / N-2 either (differs from blend(N) in " + std::string(db < 20 ? "<20" : ">=20") + " words, from set10(N) in " + std::string(ds < 20 ? "<20" : ">=20") + ")";
          }
          ++why[k];
        }
      }
      for (const LightRef& r : r0.light_refs) {
        if (!wr.count(r.object) || r.state >= r0.light_states.size()) continue;
        ++tot_refs;
        const uint32_t* st = reinterpret_cast<const uint32_t*>(r0.light_states[r.state].light);
        const uint32_t* eN[3] = {};
        for (int k = 0; k < 3 && k < have; ++k) { auto it = emb[k].find(r.object); if (it != emb[k].end()) eN[k] = it->second.data(); }
        if (!eN[1]) { ++no_src; continue; }
        const bool changed = have > 2 && eN[2] && !Eq(eN[1], eN[2]);
        if (changed) ++tot_changed;
        const uint32_t* cands[9] = {eN[0], eN[1], eN[2], bN[0], bN[1], bN[2], sN[0], sN[1], sN[2]};
        bool any = false;
        for (int c = 0; c < 9; ++c) if (cands[c] && Eq(st, cands[c])) { ++cand_eq[c]; if (changed) ++cand_eq_ch[c]; any = true; }
        if (!any) { ++none; if (changed) ++none_ch; if (verbose && none < 4) std::printf("  matches no source: %s obj %08X\n", path.c_str(), r.object); }
      }
    }
  }
  std::printf("records %llu; wrestler refs with an embedded row %llu (changed frames: %llu), refs of skel: models that install a manager object instead (the ring's, set 31: A7's class): %llu\n", (unsigned long long)records, (unsigned long long)tot_refs - no_src, (unsigned long long)tot_changed, (unsigned long long)no_src);
  for (int c = 0; c < 9; ++c) std::printf("  walk == %-24s all %llu changed %llu\n", kCand[c], (unsigned long long)cand_eq[c], (unsigned long long)cand_eq_ch[c]);
  std::printf("  walk equals NO source: %llu (changed %llu)\n", (unsigned long long)none, (unsigned long long)none_ch);
  std::printf("embedded object read at the latch, %llu model-latches: == blend(N) %llu, == set10(N) %llu, neither %llu\n", (unsigned long long)emb_total, (unsigned long long)emb_b, (unsigned long long)emb_s, (unsigned long long)emb_none);
  for (auto& kv : why) std::printf("    %-120s %llu\n", kv.first.c_str(), (unsigned long long)kv.second);
  std::printf("%s\n", wlight::CensusText(cen, false).c_str());
  std::printf("synthesis: %llu states written where the walk's were removed: equal %llu, differ %llu\n", (unsigned long long)syn_total, (unsigned long long)syn_eq, (unsigned long long)syn_diff);
  std::printf("%s\n", wlight::CensusText(cen_synth, true).c_str());
  const bool ok = cen.differ_changed == 0 && cen.eq_changed > 0 && cen.differ == 0 && syn_diff == 0 && syn_eq > 0;
  std::printf("%s\n", ok ? "PASS: wrestler_light latch==walk, differ 0 (changed frames and all), the synthesis equals the walk" : "FAIL: wrestler_light latch differs from the walk (or nothing changed / nothing synthesised)");
  return ok ? 0 : 1;
}
