// event_props_test: the offline proof of lane BLF (Session 103; docs\research\s103\BELT.md "Adversary"). 0 game runs, no SDK, no game memory.
//   event_props_test [--plant event_no_free]
// It runs the SHIPPED code: the tracker's event prop table (src\scene_live\weapon_rows_core.h EventProps: record on make, drop on free) against a fake guest, the plan (src\renderer_scene\weapon_plan.h: the
// event family is planned, never skipped as ring steps / m2 belts) and the PACH mapping (src\assets\asset_load.h PropDirIn on a hand-built EPAC directory with BOBJ ids 2004 / 3004).
// Checks (each planted twin must fail its named check; exit code 1 when any check failed):
//   event.recorded          a model made by the hook (id 2004, k 0 / 4) becomes a WeaponCall row stamped event / id / k with the node rows of ITS call
//   event.one_row_a_frame   a second pass of the same model in a frame (slot 19 / 32 / a shadow pass) is not a second belt
//   event.reused_address    after the CWeapon's free hook, the freed model's address (reused by a ring piece) is NOT recorded      <- the planted twin: event_no_free
//   event.bad_vtable_free   a free of something that is not a MakeWeapon CWeapon (vtable word != 0x82029628) drops nothing
//   event.null_model        a failed allocation (r3 = 0) is never stored
//   plan.event_items        the plan keeps event calls (ids that look like ring steps / the m2 belt id 14 included), still skips a weapon-manager id 14 and 29
//   pach.map                the directory maps BOBJ 2004 / 3004 to their PACHs; an unknown id to -1
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>   // (asset_load.h's pac / stage_media code needs it; the build passes -I<sdk stb>)

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/assets/asset_load.h"
#include "../../src/renderer_scene/rows_ring.h"
#include "../../src/renderer_scene/weapon_draw_core.h"   // EvtEntryOf (+ weapon_plan.h)
#include "../../src/scene_live/weapon_rows_core.h"
#include "../../src/scene_live/prop_read_note.h"   // S111 FOLLOWUPS3

using namespace rdr_scene;
using namespace rdr_scene::live::weaponrows;
namespace W = rdr_scene::weapons;

static int g_fail = 0;
static void Check(bool ok, const char* name, const std::string& why = "") {
  std::printf("%s  %-26s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}

struct Guest {
  std::map<uint32_t, uint32_t> words;
  bool ReadWord(uint32_t a, uint32_t& v) const {
    auto it = words.find(a);
    if (it == words.end()) return false;
    v = it->second;
    return true;
  }
  void PutFloat(uint32_t a, float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    words[a] = b;
  }
  bool ReadMats(uint32_t a, uint32_t n, std::vector<Mat4>& out) const {
    out.resize(n);
    for (uint32_t i = 0; i < n * 16; ++i) {
      auto it = words.find(a + 4 * i);
      if (it == words.end()) return false;
      std::memcpy(&reinterpret_cast<float*>(out.data())[i], &it->second, 4);
    }
    return true;
  }
  void WriteNodes(uint32_t buf, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; ++i)
      for (uint32_t j = 0; j < 16; ++j) PutFloat(buf + 64 * i + 4 * j, float(seed * 1000 + i * 16 + j));
  }
  void MakeModel(uint32_t obj, uint32_t nodes) {   // a skinned model whose slot 2 is sub_82BD3AB8 and whose count is in [this+300] (the listing's addresses, spelled out)
    words[obj] = 0x8206AF90, words[0x8206AF90 + 8] = 0x82BD3AB8;
    words[obj + 296] = 1, words[obj + 300] = nodes;
    PutFloat(obj + 252, 1.0f);
  }
  void MakeWeapon(uint32_t w, std::initializer_list<uint32_t> models, uint32_t vtable = W::kEventWeaponVtable) {
    words[w] = vtable;
    uint32_t i = 0;
    for (uint32_t m : models) words[w + W::kOffEventModels + 4 * i++] = m;
  }
};

struct Rig {
  Guest g;
  Tracker t;
  CallResult Call(uint32_t model, uint32_t r5, uint64_t frame, bool own = true, uint8_t shadow = 0) {
    return t.OnModelCall(model, r5, frame, own, [&](uint32_t a, uint32_t& v) { return g.ReadWord(a, v); }, [&](uint32_t a, uint32_t n, std::vector<Mat4>& o) { return g.ReadMats(a, n, o); }, 1.0f, 0, 0, nullptr, nullptr, shadow);
  }
  uint32_t Free(uint32_t w) { return t.EventFree(w, [&](uint32_t a, uint32_t& v) { return g.ReadWord(a, v); }); }
};

static void ExpectRows(uint32_t node, uint32_t seed, float out[12]) {
  float m[16];
  for (uint32_t j = 0; j < 16; ++j) m[j] = float(seed * 1000 + node * 16 + j);
  for (int k = 0; k < 3; ++k) out[4 * k + 0] = m[k], out[4 * k + 1] = m[4 + k], out[4 * k + 2] = m[8 + k], out[4 * k + 3] = m[12 + k];
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::string plant_name;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) plant_name = argv[++i];
  if (!plant_name.empty() && plant_name != "event_no_free" && plant_name != "event_instance0" && plant_name != "shadow_as_colour" && plant_name != "asked_light_lost") {
    std::printf("unknown plant %s\n", plant_name.c_str());
    return 2;
  }
  const uint32_t kStrap = 0xA0001000, kPlate = 0xA0002000, kBuf = 0xB0001000, kBuf2 = 0xB0002000, kWeapon = 0x90000000, kOther = 0x90001000;
  Rig r;
  if (plant_name == "event_no_free") r.t.plant = Plant::EventNoFree;
  if (plant_name == "event_instance0") r.t.plant = Plant::EventInstance0;
  if (plant_name == "shadow_as_colour") r.t.plant = Plant::ShadowAsColour;
  r.g.MakeModel(kStrap, 36), r.g.MakeModel(kPlate, 1);
  const uint64_t F = 200;

  // before the hooks ran: not a weapon
  {
    r.g.WriteNodes(kBuf, 36, 5);
    const CallResult c = r.Call(kStrap, kBuf, F);
    Check(!c.weapon && !c.event, "event.unknown_not_recorded", "a model nobody named is not an event prop");
  }
  // MakeWeapon's hook: the belt (prop 2004) made two models, entry 0 (the strap, 36 nodes) and entry 4 (the plate, one node); a null allocation is ignored
  r.t.EventMake(kStrap, 2004, 0);
  r.t.EventMake(kPlate, 2004, 4);
  r.t.EventMake(0, 2004, 7);
  Check(r.t.events.Size() == 2, "event.null_model", "a failed allocation (r3 = 0) is never stored: table size " + std::to_string(r.t.events.Size()));
  r.g.WriteNodes(kBuf, 36, 11);
  r.g.WriteNodes(kBuf2, 1, 12);
  const CallResult c0 = r.Call(kStrap, kBuf, F + 1);
  const CallResult c1 = r.Call(kPlate, kBuf2, F + 1);
  const CallResult c0b = r.Call(kStrap, kBuf, F + 1, /*own_submit=*/false);   // another pass of the same model in the same frame (slot 19 / 32): not a second belt
  r.g.WriteNodes(kBuf, 36, 13);
  r.Call(kStrap, kBuf, F + 2);   // the next frame closes F + 1
  FrameBuffer fb;
  bool got = r.t.TakeNext(fb);   // the first closed frame is F + 1 (F recorded nothing but still opened a frame? take until the one with calls)
  while (got && fb.calls.empty()) got = r.t.TakeNext(fb);
  bool rec_ok = got && fb.calls.size() == 2;
  if (rec_ok) {
    const W::WeaponCall& a = fb.calls[0];
    const W::WeaponCall& b = fb.calls[1];
    float e[12];
    ExpectRows(0, 11, e);
    rec_ok = a.object == kStrap && a.id == 2004 && W::IsEventCall(a.slot_flags) && W::EventEntry(a.slot_flags) == 0 && a.node_count == 36 && std::memcmp(fb.nodes[a.node_first].rows, e, sizeof e) == 0 &&
             b.object == kPlate && b.id == 2004 && W::IsEventCall(b.slot_flags) && W::EventEntry(b.slot_flags) == 4 && b.node_count == 1;
    ExpectRows(0, 12, e);
    rec_ok = rec_ok && std::memcmp(fb.nodes[b.node_first].rows, e, sizeof e) == 0;
  }
  Check(rec_ok && c0.event && c0.id == 2004 && c1.k == 4, "event.recorded", "2 rows (strap k 0 with 36 node rows, plate k 4), id 2004, rows = the buffer at the call; calls " + std::to_string(got ? fb.calls.size() : 0));
  Check(c0b.weapon && !c0b.recorded && got && fb.calls.size() == 2, "event.one_row_a_frame", "a second pass of the strap in the same frame made no second row");

  // S103 STG (PMW.md): in a walked frame the props-map submit (flag A, own submit) comes BEFORE the colour submit. It must not become the event's first call (which would drop the real colour call as other_pass): it is a tagged caster row,
  // and the colour call keeps the event row (instance 0, the colour buffer).
  {
    const uint32_t kS = 0xA0020000, kSBuf = 0xB0020000;
    r.g.MakeModel(kS, 1), r.t.EventMake(kS, 2004, 0);
    r.g.WriteNodes(kSBuf, 1, 41);
    r.Call(kS, kSBuf, F + 5, true, 1);   // the props pass
    r.g.WriteNodes(kSBuf, 1, 42);
    r.Call(kS, kSBuf, F + 5, true, 2);   // map B
    r.g.WriteNodes(kSBuf, 1, 43);
    const CallResult col = r.Call(kS, kSBuf, F + 5, true, 0);   // the colour call
    r.Call(kS, kSBuf, F + 6, true);
    FrameBuffer fbs;
    int n_colour = 0, n_props = 0, n_b = 0;
    bool colour_rows = false;
    while (r.t.TakeNext(fbs))
      for (const W::WeaponCall& c : fbs.calls)
        if (c.frame == uint32_t(F + 5) && c.object == kS) {
          if (!W::IsShadowCall(c.slot_flags)) { ++n_colour; float e[12]; ExpectRows(0, 43, e); colour_rows = W::IsEventCall(c.slot_flags) && c.instance == 0 && std::memcmp(fbs.nodes[c.node_first].rows, e, sizeof e) == 0; }
          else if (W::IsShadowPropsCall(c.slot_flags)) ++n_props;
          else ++n_b;
        }
    r.g.MakeWeapon(0x90003000, {kS});   // its CWeapon goes away: the table is back to its two models for the checks below
    r.t.events.OnFree(0x90003000, [&](uint32_t a, uint32_t& v) { return r.g.ReadWord(a, v); }, true);
    Check(col.event && col.recorded && n_colour == 1 && colour_rows && n_props == 1 && n_b == 1, "shadow.event_colour_kept", "props-map + map B + colour calls of one event prop: the colour call keeps the event row (colour " + std::to_string(n_colour) + ", rows from the colour buffer: " + (colour_rows ? "yes" : "no") + "), props " + std::to_string(n_props) + ", map B " + std::to_string(n_b));
  }

  // review 1 / 5: two models of the SAME (id, k) (tag-team belts, two identical mics) get instance 0 and 1 (two posed slots); a non-own pass followed by the own submit in a frame leaves ONE row, the own one
  {
    const uint32_t kA = 0xA0010000, kB = 0xA0011000, kBufA = 0xB0010000, kBufB = 0xB0011000;
    r.g.MakeModel(kA, 1), r.g.MakeModel(kB, 1);
    r.t.EventMake(kA, 2004, 0), r.t.EventMake(kB, 2004, 0);
    r.g.WriteNodes(kBufA, 1, 31), r.g.WriteNodes(kBufB, 1, 32);
    r.Call(kA, kBufA, F + 20, /*own_submit=*/false);   // a shadow-style pass first ...
    r.Call(kB, kBufB, F + 20, true);
    r.g.WriteNodes(kBufA, 1, 33);
    const CallResult own = r.Call(kA, kBufA, F + 20, true);   // ... then the own submit of A: it replaces A's row
    r.Call(kA, kBufA, F + 21, true);
    FrameBuffer fb3;
    int na = 0, nb = 0;
    uint32_t ia = 99, ib = 99;
    bool a_rows_own = false;
    while (r.t.TakeNext(fb3))
      for (const W::WeaponCall& c : fb3.calls)
        if (c.frame == uint32_t(F + 20)) {
          if (c.object == kA) { ++na, ia = c.instance; float e[12]; ExpectRows(0, 33, e); a_rows_own = std::memcmp(fb3.nodes[c.node_first].rows, e, sizeof e) == 0; }
          if (c.object == kB) ++nb, ib = c.instance;
        }
    Check(na == 1 && nb == 1 && ia != ib && (ia + ib) == 1, "event.two_models_two_slots", "models of 2004/0: instances " + std::to_string(ia) + " and " + std::to_string(ib) + " (must differ)");
    Check(na == 1 && a_rows_own && own.own_submit, "event.own_submit_wins", "A has one row (" + std::to_string(na) + "), from its own-submit call: " + (a_rows_own ? "yes" : "no"));
    // the section-59 prefetch rows: an event slot row asks for (prop id, k | kWantEvent), never an m2 weapon
    W::WeaponSlot s0;
    s0.id = 2004, s0.slot = 4, s0.main = kA, s0.flags = W::kSlotFlagEvent;
    W::View v;
    v.slots = &s0, v.n_slots = 1;
    const auto want = W::Prefetch(v, W::Plants());
    Check(want.size() == 1 && want[0].first == 2004 && want[0].second == (4u | W::kWantEvent), "plan.event_prefetch", "event slot row -> want (" + std::to_string(want.empty() ? -1 : want[0].first) + ", 0x" + std::to_string(want.empty() ? 0 : want[0].second) + ")");
    const auto snap = r.t.events.Snapshot();
    Check(snap.size() >= 4 && snap.front().first < snap.back().first, "event.snapshot", "table rows for the contributor: " + std::to_string(snap.size()));
    r.g.MakeWeapon(0x90002000, {kA, kB});   // their CWeapon goes away (a real free whatever the plant: the reused-address check below owns the event_no_free twin)
    r.t.events.OnFree(0x90002000, [&](uint32_t a, uint32_t& v) { return r.g.ReadWord(a, v); }, true);
  }

  // the CWeapon is freed (the free hook): its models leave the table; a ring piece then gets the strap's old address
  r.g.MakeWeapon(kWeapon, {kStrap, kPlate});
  r.g.MakeWeapon(kOther, {kStrap}, 0x82029700);   // not a MakeWeapon CWeapon
  const uint32_t bad = r.Free(kOther);
  Check(bad == 0 && r.t.events.Size() == 2, "event.bad_vtable_free", "a free of a foreign object dropped " + std::to_string(bad));
  const uint32_t dropped = r.Free(kWeapon);
  r.g.MakeModel(kStrap, 36);   // the freed address comes back for a ring piece / CAW model
  r.g.WriteNodes(kBuf, 36, 21);
  const CallResult reuse = r.Call(kStrap, kBuf, F + 10);
  r.Call(kStrap, kBuf, F + 11);
  FrameBuffer fb2;
  size_t rows_for_strap = 0;
  while (r.t.TakeNext(fb2))
    for (const W::WeaponCall& c : fb2.calls)
      if (c.object == kStrap && c.frame == uint32_t(F + 10)) ++rows_for_strap;
  Check(dropped == 2 && !reuse.weapon && rows_for_strap == 0, "event.reused_address", "free dropped " + std::to_string(dropped) + " models; a reused address recorded " + std::to_string(rows_for_strap) + " row(s) (must be 0)");

  // the plan: event calls are items, whatever their id; a weapon-manager id 14 / 29 is still skipped
  {
    std::vector<W::WeaponCall> calls(4);
    std::vector<RingNodeRows> nodes(4);
    for (auto& n : nodes) std::memset(n.rows, 0, sizeof n.rows);
    const int32_t ids[4] = {14, 14, 29, 2004};
    const bool ev[4] = {true, false, false, true};
    for (int i = 0; i < 4; ++i) {
      calls[size_t(i)].id = ids[i], calls[size_t(i)].frame = 5, calls[size_t(i)].node_first = uint32_t(i), calls[size_t(i)].node_count = 1;
      calls[size_t(i)].slot_flags = ev[i] ? W::PackEventFlags(uint32_t(i == 3 ? 4 : 0)) : W::PackSlotFlags(0, false, true);
    }
    W::View v;
    v.calls = calls.data(), v.n_calls = calls.size(), v.nodes = reinterpret_cast<const float*>(nodes.data()), v.n_nodes = nodes.size();
    W::Plants pl;
    pl.draw_steps = false;   // S103 LDR: the retired steps skip as a labelled twin (this check is about the event rows and the belt skip bookkeeping)
    W::Census c;
    const std::vector<W::Item> items = W::Plan(v, pl, c);
    const bool ok = items.size() == 2 && items[0].event && items[0].id == 14 && items[0].entry == 0 && items[1].event && items[1].id == 2004 && items[1].entry == 4 && c.belts == 1 && c.skipped_steps == 1 && c.event_items == 2 &&
                    W::EvtEntryOf(2004, 4) == "evtprop:2004/4";
    Check(ok, "plan.event_items", "items " + std::to_string(items.size()) + ", belts skipped " + std::to_string(c.belts) + ", steps skipped " + std::to_string(c.skipped_steps) + ", entry " + W::EvtEntryOf(2004, 4));
    {   // S111 FOLLOWUPS3 (READANSWER review F2): one prop id built from the recoloured read for one participant and from the plain read for another = two models, two keys, two asks
      namespace prn = rdr_scene::live::prop_read_note;
      prn::Clear();
      std::map<uint32_t, uint32_t> g;   // a synthetic guest: the recolour task T (recolour object at T + 208), its CWeapon W = [T + 192], W's models at W + 1228 + 4k
      const uint32_t kT = 0x91000000u, kW = 0x92000000u, kM0 = 0x93000000u, kM4 = 0x93000400u, kPlainM = 0x94000000u;
      g[kT + 192] = kW, g[kW + prn::kWeaponModelsOff] = kM0, g[kW + prn::kWeaponModelsOff + 16] = kM4;
      const auto rd = [&](uint32_t a, uint32_t& v) { auto f = g.find(a); v = f == g.end() ? 0u : f->second; return true; };
      const bool noted = prn::NoteFromRecolourCall(rd, kT + 208);
      const bool marks = prn::IsRecoloured(kM0) && prn::IsRecoloured(kM4) && !prn::IsRecoloured(kPlainM) && !prn::IsRecoloured(0);
      W::WeaponSlot rows[2];
      for (int i = 0; i < 2; ++i) {
        const uint32_t m = i == 0 ? kM0 : kPlainM;
        rows[i].id = 2718, rows[i].slot = 0, rows[i].main = m, rows[i].flags = W::kSlotFlagEvent | (prn::IsRecoloured(m) ? W::kSlotFlagRecoloured : 0u);   // as WeaponRowsContributor::Emit writes them
      }
      W::View v2;
      v2.slots = rows, v2.n_slots = 2;
      std::vector<std::string> keys;
      for (const auto& pr : W::Prefetch(v2, W::Plants())) keys.push_back(W::EvtEntryOf(pr.first, pr.second & ~W::kWantEvent));
      const bool two = keys.size() == 2 && keys[0] == "evtprop:2718/0/s" && keys[1] == "evtprop:2718/0" && W::EvtEntryRecoloured(keys[0]) && !W::EvtEntryRecoloured(keys[1]);
      const bool items_key = W::EventRecoloured(v2, kM0) && !W::EventRecoloured(v2, kPlainM);
      Check(noted && marks && two && items_key, "event.recoloured_own_key", "noted " + std::to_string(noted) + ", marks " + std::to_string(marks) + ", keys " + std::to_string(keys.size()) + (keys.empty() ? "" : " first " + keys[0]) + ", item key " + std::to_string(items_key));
      // the twin: rows written without the flag (the old writer) make ONE ask for both models
      W::WeaponSlot old_rows[2] = {rows[0], rows[1]};
      old_rows[0].flags = old_rows[1].flags = W::kSlotFlagEvent;
      W::View v3;
      v3.slots = old_rows, v3.n_slots = 2;
      Check(W::Prefetch(v3, W::Plants()).size() == 1, "event.twin_one_key_per_id", "the old rows -> one ask for two models (event.recoloured_own_key tells the rules apart)");
      prn::Forget(kW);
      Check(!prn::IsRecoloured(kM0) && !prn::IsRecoloured(kM4), "event.recoloured_forget", "the CWeapon's free drops its models' note (a reused address is no recoloured model)");
      // S111 FOLLOWUPS3 review F1: the two models of one (id, k) draw from two pools (rf_props.cpp ApplyWeaponRows: pool, bound copy, pose slots). Neither stem is a prefix of the other (PoolTrack /
      // RetireOther match by prefix) and both start "weapon:ev" (the entrance end's RetireOther). The twin: the old stem ("weapon:ev<id>/<k>" whatever the read) names both models alike.
      const std::string sp = W::EvtPoolStemOf(2718, 0, false), sr = W::EvtPoolStemOf(2718, 0, true);
      const bool apart = sp != sr && sp.rfind(sr, 0) != 0 && sr.rfind(sp, 0) != 0 && sp.rfind("weapon:ev", 0) == 0 && sr.rfind("weapon:ev", 0) == 0 && sp == "weapon:ev2718/0";
      Check(apart, "event.recoloured_own_pool", "plain pool " + sp + ", recoloured pool " + sr);
      const auto old_stem = [](int32_t id, uint32_t k) { return "weapon:ev" + std::to_string(id) + "/" + std::to_string(k); };
      Check(old_stem(2718, 0) == sp && old_stem(2718, 0) != sr, "event.twin_old_pool_shared", "the old stem " + old_stem(2718, 0) + " is the plain model's and the recoloured one's alike (event.recoloured_own_pool tells the rules apart)");
      prn::Clear();
    }
  }

  // the PACH mapping: a hand-built EPAC directory (SOBJ LIST, then BOBJ 2004 -> PACH 1, 3004 -> PACH 2), the game's lookup (PropDirIn)
  {
    assets::pac::Pac p;
    p.bytes.assign(0x4000 + 4 * 0x800, 0);
    std::memcpy(p.bytes.data(), "EPAC", 4);
    auto rec = [&](size_t o, const char* name, uint32_t start) {
      std::memcpy(p.bytes.data() + o, name, 4);
      for (int i = 0; i < 4; ++i) p.bytes[o + 4 + size_t(i)] = uint8_t(start >> (8 * i));
    };
    std::memcpy(p.bytes.data() + 0x800, "SOBJ", 4);
    rec(0x80C, "LIST", 0), rec(0x818, "BOBJ", 0), rec(0x824, "2004", 1), rec(0x830, "3004", 2), rec(0x83C, "2717", 3), rec(0x848, "SOBJ", 0), rec(0x854, "2004", 3), rec(0x860, "SPMT", 0), rec(0x86C, "2718", 2);   // S108 THB: + the SPMT section (the Threads gear colours' masks): 2718 -> start 2
    for (int i = 0; i < 4; ++i) {
      assets::pac::Pach h;
      h.base = 0x4000 + size_t(i) * 0x800, h.end = h.base + 0x800;
      if (i != 3) h.ents.push_back({0, h.base + 0x100, 0x10});
      else h.ents.push_back({5, h.base + 0x100, 0x10});   // 2717-style: the model is entry 5 only
      if (i == 1) h.ents.push_back({4, h.base + 0x200, 0x10});
      p.pachs.push_back(h);
    }
    const int a = assets::PropDirIn(&p, 2004, true), b = assets::PropDirIn(&p, 3004), c = assets::PropDirIn(&p, 2009);
    const int d_default = assets::PropDirIn(&p, 2717), d_evt = assets::PropDirIn(&p, 2717, false, false);   // review 2: no entry-0 test for the posed draw
    const int s2004 = assets::PropDirIn(&p, 2004, false, false, true), b2004 = assets::PropDirIn(&p, 2004, true, false);   // review 6: in both sections, SOBJ (PACH 3) first, BOBJ (PACH 1) the other
    Check(d_default == -1 && d_evt == 3 && s2004 == 3 && b2004 == 1, "pach.no_entry0_and_both", "2717: default " + std::to_string(d_default) + ", event path " + std::to_string(d_evt) + "; 2004 SOBJ " + std::to_string(s2004) + " BOBJ " + std::to_string(b2004));
    Check(a == 1 && b == 2 && c == -1, "pach.map", "2004 -> " + std::to_string(a) + ", 3004 -> " + std::to_string(b) + ", 2009 -> " + std::to_string(c));
    {   // S108 THB: the SPMT section is its own directory section: an id is found in it only, and the SOBJ / BOBJ lookups never see it
      size_t at_spmt = 0, at_sobj = 0, at_x = 0;
      const bool in_spmt = assets::PropDirStartIn(p.bytes, 2718, "SPMT", at_spmt), in_sobj = assets::PropDirStartIn(p.bytes, 2718, "SOBJ", at_sobj), spmt_2004 = assets::PropDirStartIn(p.bytes, 2004, "SPMT", at_x);
      size_t at_any = 0;
      const bool old_walk_sees_it = assets::PropDirStart(p.bytes, 2718, false, false, at_any);   // (the SOBJ / BOBJ walk: SPMT rows are not props)
      Check(in_spmt && at_spmt == p.pachs[2].base && !in_sobj && !spmt_2004 && !old_walk_sees_it, "pach.spmt_section", "2718 is an SPMT row at PACH 2's start (" + std::string(in_spmt ? "found" : "missing") + "), in no SOBJ / BOBJ row; 2004 has no SPMT row");
    }
  }
  // the evtobj.pac slot (decision 2026-11-19): closed when the last event model retires, only for the live game (the viewer never marks it releasable), reopened by the next open (a cache miss)
  {
    const std::string key = "pac\\evt\\evtobj.pac";
    auto& st = assets::load_detail::DlcProps();
    st.cache[key] = assets::pac::Pac();
    W::g_evtobj_release = [] { bool busy = false; assets::load_detail::ReleaseEvtobj(&busy); return !busy; };
    W::ReleaseEvtobjSlot();   // the viewer case: not releasable -> stays open
    const bool kept = st.cache.count(key) == 1;
    st.releasable = true;     // the live loader marked it
    W::ReleaseEvtobjSlot();   // the last event want retired
    const bool closed = st.cache.count(key) == 0 && st.releases == 1;
    W::ReleaseEvtobjSlot();   // nothing open: no double count
    st.cache[key] = assets::pac::Pac();   // a new entrance prop's open (OpenDlcProps on a miss) puts it back; the next retire closes it again
    W::ReleaseEvtobjSlot();
    Check(kept && closed && st.releases == 2 && st.cache.count(key) == 0, "evt.slot_release", std::string("viewer keeps it: ") + (kept ? "yes" : "NO") + ", live retire closes it: " + (closed ? "yes" : "NO") + ", releases " + std::to_string(st.releases));
  }
  // S111 PROPSPEC review: the game binds its entrance specular set by MODEL ([CWeapon + 48], sub_82335070 +170..+17C). The entrance's steps object (sub_825E2DE0(-1, 32) -> sub_825E0F60 -> base ctor sub_83422628 +F8) keeps the
  // weapon manager's slot main model ([mgr + 848 + 4i], the slot whose id is 32), the very model our slot row names, and the weapon drawer draws ids 29 / 32 / 35 from those rows (weapon_plan.h Plants::draw_steps). So the set
  // rides that slot row as well as an event row, through the SHIPPED tracker. The planted twin is the lane's first landing: only the event row carried the code (the steps row 0).
  {
    namespace ps = rdr_scene::live::propspec;
    Rig q;
    const uint32_t kMgr = 0x70000000, kSteps = 0xA0300000, kStepsBuf = 0xB0300000, kEvt = 0xA0310000, kEvtBuf = 0xB0310000, kPlain = 0xA0320000, kPlainBuf = 0xB0320000;
    q.g.MakeModel(kSteps, 1), q.g.MakeModel(kEvt, 1), q.g.MakeModel(kPlain, 1);
    q.g.words[W::kMgrGlobal] = kMgr, q.g.words[kMgr + W::kOffSlotCount] = 2;
    q.g.words[kMgr + W::kOffSlotId] = 32, q.g.words[kMgr + W::kOffSlotMain] = kSteps;        // slot 0: the ring steps (id 32)
    q.g.words[kMgr + W::kOffSlotId + 4] = 7, q.g.words[kMgr + W::kOffSlotMain + 4] = kPlain;  // slot 1: a chair no set names
    for (uint32_t i = 0; i < 2; ++i)
      for (uint32_t k = 0; k < W::kMaxSecondary; ++k) q.g.words[kMgr + W::kOffSlotSecondary + 16 * i + 4 * k] = 0;
    q.t.EventMake(kEvt, 2004, 0);
    ps::Store& s = ps::TheStore();
    s.Bind(kSteps, ps::kLev, 0.3f), s.Bind(kSteps, ps::kPow, 40.f), s.Bind(kSteps, ps::kRefl, 0.3f);   // the steps set (sub_82335268 +114..+120)
    s.Bind(kEvt, ps::kLev, 0.f), s.Bind(kEvt, ps::kPow, 0.f), s.Bind(kEvt, ps::kRefl, 0.f);           // the bank-104 belt (+258..+260)
    q.g.WriteNodes(kStepsBuf, 1, 61), q.g.WriteNodes(kEvtBuf, 1, 62), q.g.WriteNodes(kPlainBuf, 1, 63);
    const uint64_t P = 400;
    q.Call(kSteps, kStepsBuf, P), q.Call(kEvt, kEvtBuf, P), q.Call(kPlain, kPlainBuf, P), q.Call(kSteps, kStepsBuf, P + 1);
    FrameBuffer fbp;
    uint32_t steps_code = 9, evt_code = 9, plain_code = 9;
    int32_t steps_id = -1;
    bool steps_slot_row = false;
    while (q.t.TakeNext(fbp))
      for (const W::WeaponCall& c : fbp.calls) {
        if (c.frame != uint32_t(P)) continue;
        if (c.object == kSteps) steps_code = ps::SpecCodeOf(c.slot_flags), steps_id = c.id, steps_slot_row = !W::IsEventCall(c.slot_flags) && W::CallSlot(c.slot_flags) == 0;
        if (c.object == kEvt) evt_code = ps::SpecCodeOf(c.slot_flags);
        if (c.object == kPlain) plain_code = ps::SpecCodeOf(c.slot_flags);
      }
    s.Clear();
    Check(steps_code == ps::kSpecSteps && steps_id == 32 && steps_slot_row && evt_code == ps::kSpecOff && plain_code == ps::kSpecNone, "propspec.rows_by_model",
          "steps slot row code " + std::to_string(steps_code) + " (id " + std::to_string(steps_id) + "), bank-104 belt event row code " + std::to_string(evt_code) + ", unbound chair code " + std::to_string(plain_code) + " (want 1 / 2 / 0)");
  }
  // S111 STAGELESS2 (src\renderer_scene\prop_light_id.h): Create a Title's belt asks the light manager for set 10 ([cw + 1468], sub_83344760 0x8334487C). The SHIPPED chain: the ctor hook hands the CWeapon to the
  // tracker, the once-a-frame read puts the id on its models, the section-59 row carries it (+ 1), the drawer's pick lights the belt by set 10 when the record holds it and keeps its older rule when not.
  // The planted twin asked_light_lost is the rule before (the row carries nothing: the belt keeps the arena props / file sets).
  {
    if (plant_name == "asked_light_lost") W::g_plant_asked_light = 1;
    Rig q;
    const uint32_t kBelt = 0xA0400000, kBeltBuf = 0xB0400000, kCw = 0x90400000;
    q.g.MakeModel(kBelt, 1);
    q.t.EventMake(kBelt, 4000, 0);
    q.t.EventWeapon(kCw);
    q.g.MakeWeapon(kCw, {kBelt});
    q.g.words[kCw + W::kOffAskedLightSet] = 10;
    q.g.WriteNodes(kBeltBuf, 1, 71);
    q.Call(kBelt, kBeltBuf, 600), q.Call(kBelt, kBeltBuf, 601);   // a model call of a new frame runs the once-a-frame read
    std::vector<W::WeaponSlot> rows;
    for (const auto& ev : q.t.events.Snapshot()) {   // the contributor's section-59 event rows (weapon_rows_contrib.h Emit)
      W::WeaponSlot s;
      s.id = ev.second.id, s.slot = ev.second.k, s.main = ev.first, s.flags = W::kSlotFlagEvent;
      s.reserved[0] = W::PackAskedLightSet(ev.second.light_set);
      rows.push_back(s);
    }
    const int32_t asked = W::AskedLightSet(rows, kBelt);
    const int lit = W::PropLightKey(asked, [](int32_t id) { return id == 10; });       // the record holds set 10 (Create a Title's key)
    const int miss = W::PropLightKey(asked, [](int32_t) { return false; });            // a record without it: the older rule
    const uint32_t freed = q.Free(kCw);
    W::g_plant_asked_light = 0;
    Check(asked == 10 && lit == 10 && miss == -1 && freed == 1, "prop.asked_light_set",
          "asked " + std::to_string(asked) + ", lit by " + std::to_string(lit) + ", without the set " + std::to_string(miss) + ", models freed " + std::to_string(freed) + " (want 10 / 10 / -1 / 1)");
  }
  std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
  return g_fail ? 1 : 0;
}
