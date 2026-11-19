// S103 lane PW-1 (offline, 0 game runs): the weapon manager's slot reader (src\scene_live\weapon_mgr_reader.h), the contributor (weapon_contrib.h) and section 59 of the record
// (record_io.cpp), against FORGED guest memory. The expected rows are written with LITERAL offsets (388 / 580 / 848 / 1048 / 16), not the reader's constants, so a wrong constant
// in the reader cannot agree with itself. Planted twins: one reader fault each (a wrong offset shifted by 4, a secondary stride of 4, a cap not applied, a wrong vtable constant); each must FAIL the same check.
// Contract v2 (S103, the PW-1/2 review): 2 secondaries a slot (k < 2: each 16-byte block is {data0, data1, obj0, obj1}, so k = 2 would read the NEXT slot's data word); a count above the cap is REFUSED (no rows), a
// manager pointer below 0x10000 is no manager; the contributor latches with LatchMark.
//   build + run: py -I tools\weapons\weapon_slots_check.py --root .
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "../../src/renderer_scene/live_scene.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/scene_live/weapon_contrib.h"

using namespace rdr_scene;
using rdr_scene::weapons::WeaponSlot;
namespace wl = rdr_scene::live::weapons;

static int g_fail = 0, g_pass = 0;
static void Check(bool ok, const std::string& what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
  (ok ? g_pass : g_fail)++;
}

struct Fake {   // zero everywhere inside [base, base + size) (an allocated manager), unreadable elsewhere unless put
  std::map<uint32_t, uint32_t> w;
  uint32_t base = 0, size = 0;
  void put(uint32_t a, uint32_t v) { w[a] = v; }
  bool read32(uint32_t a, uint32_t& v) const {
    auto it = w.find(a);
    if (it != w.end()) return v = it->second, true;
    if (a >= base && a + 4 <= base + size) return v = 0, true;
    return false;
  }
};
static Fake* g_mem = nullptr;
static bool ReadFn(uint32_t a, uint32_t& v) { return g_mem->read32(a, v); }

constexpr uint32_t kMgr = 0x40000000, kVt = 0x8206AF90, kGlobal = 0x837635B4;
constexpr uint32_t M0 = 0x50000000, M1 = 0x50000400, M2 = 0x50000800, M3 = 0x50000C00;

static Fake Forge() {   // 5 slots: 0 chair id 4 (+ secondaries k0 / k1), 1 ladder id 6 with a WRONG vtable, 2 empty (-1), 3 steps id 32 (+ secondary k0), 4 id 7 with no model
  Fake f;
  f.base = kMgr, f.size = 0x1000;
  f.put(kGlobal, kMgr);
  f.put(kMgr + 580, 5);
  const uint32_t ids[5] = {4, 6, 0xFFFFFFFFu, 32, 7}, mains[5] = {M0, M1, M2, M3, 0};
  for (uint32_t i = 0; i < 5; ++i) f.put(kMgr + 388 + 4 * i, ids[i]), f.put(kMgr + 848 + 4 * i, mains[i]);
  for (uint32_t m : {M0, M2, M3}) f.put(m, kVt);
  f.put(M1, 0x82000000);   // a wrong vtable
  f.put(kMgr + 1048 + 16 * 0 + 4 * 0, 0x60000000), f.put(kMgr + 1048 + 16 * 0 + 4 * 1, 0x60000020);
  f.put(kMgr + 1048 + 16 * 3 + 4 * 0, 0x60000300);
  f.put(kMgr + 1040 + 16 * 1 + 4 * 0, 0x61000000);   // slot 1's DATA word = where "slot 0, k = 2" would read: a reader with k < 4 returns it as a secondary
  f.put(kMgr + 1040 + 16 * 4 + 4 * 1, 0x61000010);   // slot 4's data1 = "slot 3, k = 3"
  return f;
}
static std::vector<WeaponSlot> Expected() {
  std::vector<WeaponSlot> e(3);
  e[0].id = 4, e[0].slot = 0, e[0].main = M0, e[0].secondary[0] = 0x60000000, e[0].secondary[1] = 0x60000020, e[0].flags = 1;
  e[1].id = 6, e[1].slot = 1, e[1].main = M1, e[1].flags = 0;   // wrong vtable: a row, bit 0 clear
  e[2].id = 32, e[2].slot = 3, e[2].main = M3, e[2].secondary[0] = 0x60000300, e[2].flags = 1;
  return e;
}
static bool Same(const std::vector<WeaponSlot>& a, const std::vector<WeaponSlot>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].id != b[i].id || a[i].slot != b[i].slot || a[i].main != b[i].main || a[i].flags != b[i].flags) return false;
    for (int k = 0; k < 2; ++k)
      if (a[i].secondary[k] != b[i].secondary[k] || a[i].reserved[k] != b[i].reserved[k]) return false;
  }
  return true;
}
static std::vector<WeaponSlot> Read(Fake& f, const wl::Layout& L, wl::Why* why = nullptr, wl::Stats* st = nullptr) {
  std::vector<WeaponSlot> out;
  const wl::Why w = wl::ReadSlots(f, out, st, L);
  if (why) *why = w;
  return out;
}
static Fake ForgeBig() {   // count 60 (past the cap 48): main[48] is the word at +1040, so an unapplied cap reads a 49th row
  Fake f;
  f.base = kMgr, f.size = 0x1000;
  f.put(kGlobal, kMgr);
  f.put(kMgr + 580, 60);
  for (uint32_t i = 0; i < 48; ++i) f.put(kMgr + 388 + 4 * i, i), f.put(kMgr + 848 + 4 * i, M0 + 0x100 * i), f.put(M0 + 0x100 * i, kVt);
  f.put(kMgr + 1040, 0x50007000);
  for (uint32_t i = 49; i < 60; ++i) f.put(kMgr + 848 + 4 * i, 0x50007000 + 16 * i);
  return f;
}

int main() {
  Fake f = Forge();
  g_mem = &f;

  // 1. the rows
  wl::Stats st;
  wl::Why why = wl::Why::Unreadable;
  const auto rows = Read(f, wl::kLayout, &why, &st);
  Check(why == wl::Why::None && Same(rows, Expected()), "forged manager: 5 slots -> 3 rows (chair id 4 + secondaries k0/k1, ladder id 6 wrong vtable flags 0, steps id 32 slot 3 + secondary k0); empty id -1 and a model-less slot give no row");
  Check(st.mgr == kMgr && st.count == 5 && st.scanned == 5 && st.empty == 2 && st.bad_vtable == 1 && st.refused == 0, "stats: count 5, scanned 5, empty 2, bad_vtable 1, refused 0");
  Check(rdr_scene::weapons::kMaxSecondary == 2 && sizeof(WeaponSlot) == 32 && offsetof(WeaponSlot, reserved) == 20, "contract v2: kMaxSecondary 2 (literal), WeaponSlot 32 bytes with reserved[2] at +20");
  Check(rows.size() == 3 && rows[0].flags == 1 && rows[1].flags == 0 && rows[2].flags == 1, "flags bit 0 = main's vtable == 0x8206AF90, per row");
  Check(rdr_scene::weapons::kMgrGlobal == kGlobal && rdr_scene::weapons::kOffSlotCount == 580 && rdr_scene::weapons::kOffSlotId == 388 && rdr_scene::weapons::kOffSlotMain == 848 &&
            rdr_scene::weapons::kOffSlotSecondary == 1048 && rdr_scene::weapons::kMaxSlots == 48 && rdr_scene::weapons::kSkinnedModelVtable == kVt,
        "weapon_rows.h constants equal the adversary's literals (PW_adversary.md section 2)");

  // 2. the cap, no manager, an unreadable word, no stale cache
  Fake big = ForgeBig();
  g_mem = &big;
  wl::Stats sb;
  wl::Why wb = wl::Why::None;
  const auto brows = Read(big, wl::kLayout, &wb, &sb);
  Check(brows.empty() && wb == wl::Why::Refused && sb.refused == 1 && sb.count == 60 && sb.scanned == 0, "a count of 60 (past the game's cap 48) is REFUSED: no rows, Refused, refused 1 (not clamped: the one rule of both readers)");
  Fake lowmgr;   // a readable block at 0x8000 holding one valid slot: only the floor keeps it from being a manager
  lowmgr.base = 0x8000, lowmgr.size = 0x1000;
  lowmgr.put(kGlobal, 0x8000), lowmgr.put(0x8000 + 580, 1), lowmgr.put(0x8000 + 388, 4), lowmgr.put(0x8000 + 848, M0), lowmgr.put(M0, kVt);
  g_mem = &lowmgr;
  wl::Why wl0 = wl::Why::None;
  Check(Read(lowmgr, wl::kLayout, &wl0).empty() && wl0 == wl::Why::NoManager, "a manager pointer below 0x10000 -> NoManager (the rule PW-2's own reader had)");
  g_mem = &big;
  Fake none = Forge();
  none.put(kGlobal, 0);
  g_mem = &none;
  wl::Why w2 = wl::Why::None;
  Check(Read(none, wl::kLayout, &w2).empty() && w2 == wl::Why::NoManager, "manager global 0 -> no rows, NoManager");
  Fake hole;   // the global itself is not readable
  g_mem = &hole;
  wl::Why w3 = wl::Why::None;
  Check(Read(hole, wl::kLayout, &w3).empty() && w3 == wl::Why::Unreadable, "unreadable global -> no rows, Unreadable");
  Fake half = Forge();
  half.size = 0x400;   // the manager block ends at +1024: the main words are readable, the secondaries (+1048..) are not
  g_mem = &half;
  wl::Why w4 = wl::Why::None;
  Check(Read(half, wl::kLayout, &w4).empty() && w4 == wl::Why::Unreadable, "an unreadable secondary word -> no rows at all (never a half-read table), Unreadable");
  {   // a reload: the same addresses come back for another id; a second read sees the new table, not the old one
    Fake r = Forge();
    g_mem = &r;
    auto a = Read(r, wl::kLayout);
    r.put(kMgr + 388 + 4 * 0, 99), r.put(kMgr + 848 + 4 * 3, 0);   // slot 0 is id 99 now, slot 3 lost its model
    auto b = Read(r, wl::kLayout);
    Check(a.size() == 3 && b.size() == 2 && b[0].id == 99 && b[1].slot == 1, "a reload between two frames: the second read shows the new table (nothing cached across frames)");
  }

  // 3. planted twins: each reader fault must change the answer (so the check above FAILS for it)
  g_mem = &f;
  struct Twin {
    const char* name;
    wl::Layout L;
  };
  std::vector<Twin> twins;
  {
    wl::Layout L;
    L.off_id += 4;
    twins.push_back({"ids at +392 (388 shifted by 4)", L});
  }
  {
    wl::Layout L;
    L.off_count += 4;
    twins.push_back({"count at +584 (580 shifted by 4)", L});
  }
  {
    wl::Layout L;
    L.off_main += 4;
    twins.push_back({"main models at +852 (848 shifted by 4)", L});
  }
  {
    wl::Layout L;
    L.off_secondary += 4;
    twins.push_back({"secondaries at +1052 (1048 shifted by 4)", L});
  }
  {
    wl::Layout L;
    L.stride_secondary = 4;
    twins.push_back({"secondary stride 4 (16 changed to 4)", L});
  }
  {
    wl::Layout L;
    L.sec_count = 4;   // the first contract's "k < 4": reads the next slot's data words as models
    twins.push_back({"k < 4 (4 secondaries a slot: the next slot's data words are read as models)", L});
  }
  {
    wl::Layout L;
    L.vtable += 4;
    twins.push_back({"vtable constant wrong (flags never set)", L});
  }
  int caught = 0;
  for (const Twin& t : twins) {
    const bool differs = !Same(Read(f, t.L), Expected());
    Check(differs, std::string("planted twin: ") + t.name + " is caught");
    caught += differs;
  }
  {
    g_mem = &big;
    wl::Layout L;
    L.cap = 1000;   // the cap is not applied
    const bool differs = !Read(big, L).empty();   // the shipped reader returns no rows for a count of 60
    Check(differs, "planted twin: the cap is not applied (a count of 60 reads past slot 47) is caught");
    caught += differs;
    g_mem = &f;
  }
  {
    g_mem = &lowmgr;
    wl::Layout L;
    L.min_mgr = 0;
    const bool differs = Read(lowmgr, L).size() == 1;   // with the floor removed the tiny pointer is read as a manager (one row); the shipped reader gives none
    Check(differs, "planted twin: the manager pointer floor removed is caught");
    caught += differs;
    g_mem = &f;
  }
  Check(caught == 9, "9 planted twins (7 layout twins, the cap, the manager floor), each caught: " + std::to_string(caught));

  // 4. the contributor: rows into the table, the census line, section 59 through the writer
  g_mem = &f;
  wl::WeaponContributor c;
  c.SetReader(&ReadFn);
  live::FrameInfo fi;
  Check(c.Latch(fi), "Latch takes the table (true)");
  SceneTables t;
  live::Counters cn;
  c.Emit(fi, t, cn);
  Check(Same(t.weapon_slots, Expected()) && cn.unknown == 0, "Emit appends the three rows to SceneTables::weapon_slots");
  std::string census;
  c.Census(census);
  Check(census.find("[scene][weapons] slots 3 (ids 4 6 32)") != std::string::npos && census.find("refused 0") != std::string::npos, "census: " + census);
  SceneTables t2;
  wl::WeaponContributor c2;   // no Latch call: Emit reads itself
  c2.SetReader(&ReadFn);
  c2.Emit(fi, t2, cn);
  Check(Same(t2.weapon_slots, Expected()), "Emit without a Latch reads the table itself");
  wl::WeaponContributor c3;   // no reader: nothing, no error
  SceneTables t3;
  c3.Latch(fi), c3.Emit(fi, t3, cn);
  Check(t3.weapon_slots.empty(), "no reader -> no rows");
  {   // the LatchMark rule (tail_latch.h): a latch is this frame's; a reload after it does not change what Emit writes for that frame; the next frame reads the new table
    Fake r = Forge();
    g_mem = &r;
    wl::WeaponContributor lc;
    lc.SetReader(&ReadFn);
    live::FrameInfo f5, f6;
    f5.guest_frame = 5, f6.guest_frame = 6;
    lc.Latch(f5);
    r.put(kMgr + 388 + 4 * 0, 99);   // a reload after the latch
    SceneTables a, b;
    lc.Emit(f5, a, cn);
    lc.Emit(f6, b, cn);   // no latch for frame 6 (and not in a deferred emit): it takes now
    Check(a.weapon_slots.size() == 3 && a.weapon_slots[0].id == 4 && b.weapon_slots.size() == 3 && b.weapon_slots[0].id == 99, "LatchMark: Emit of the latched frame writes the latched table; the next frame (no latch) re-reads the new one");
    g_mem = &f;
  }

  const std::vector<uint8_t> bytes = SerializeRecord(t);
  SceneTables back;
  std::string err;
  Check(ParseRecord(bytes.data(), bytes.size(), back, err) && Same(back.weapon_slots, Expected()), "section 59 survives SerializeRecord -> ParseRecord (rows equal)");
  bool has59 = false;
  {
    const auto* h = reinterpret_cast<const SceneHeader*>(bytes.data());
    const auto* dir = reinterpret_cast<const SectionEntry*>(bytes.data() + sizeof(SceneHeader));
    for (uint32_t i = 0; i < h->section_count; ++i) has59 = has59 || (dir[i].id == 59 && dir[i].element_size == 32 && dir[i].count == 3);
  }
  Check(has59, "the file has section id 59, element size 32, 3 rows");
  SceneTables none_t;
  const std::vector<uint8_t> old = SerializeRecord(none_t);   // an old record: no section 59
  SceneTables oldback;
  oldback.weapon_slots.resize(2);
  Check(ParseRecord(old.data(), old.size(), oldback, err) && oldback.weapon_slots.empty(), "a record with no section 59 parses with no weapons recorded");
  {   // a section 59 with a foreign element size is skipped (the record is still read)
    std::vector<uint8_t> bad = bytes;
    auto* dir = reinterpret_cast<SectionEntry*>(bad.data() + sizeof(SceneHeader));
    const auto* h = reinterpret_cast<const SceneHeader*>(bad.data());
    for (uint32_t i = 0; i < h->section_count; ++i)
      if (dir[i].id == 59) dir[i].element_size = 36;
    SceneTables bb;
    Check(ParseRecord(bad.data(), bad.size(), bb, err) && bb.weapon_slots.empty(), "section 59 with a wrong element size drops that section only");
  }

  std::printf("%d passed, %d failed\n", g_pass, g_fail);
  if (g_fail) return 1;
  std::printf("all checks passed\n");
  return 0;
}
