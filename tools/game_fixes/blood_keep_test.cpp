// BLD7 (S108): the unit test of src/blood_keep_core.h (the Regrow plan) against a fake game. Exit 0 = every check passed.
// The planted twins are made by tools/game_fixes/blood_keep_selftest.py (it edits a COPY of the header and requires this test to fail on it).
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "blood_keep_core.h"

using namespace wwe2k14::blood_keep;

static int g_fail = 0, g_pass = 0;
static void Check(bool ok, const char* name, const std::string& detail = "") {
  std::printf("%s  %-46s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
  (ok ? g_pass : g_fail)++;
}

struct Call {
  Fn f;
  uint64_t r3, r4, r5;
};

// A fake guest: bytes in a map; the game functions only record their call.
struct Fake {
  std::map<uint32_t, uint8_t> mem;
  std::map<uint32_t, uint32_t> words;
  std::vector<Call> calls;
  std::vector<std::string> log;
  uint32_t found_cbdc_comp = 0, found_cclm_comp = 0;  // what the lookup returns for a tag
  // the map's list counts as the game keeps them (literal offsets +88 live / +100 waiting, sub_82726DF0 0x82726E60 / 0x82726EC0): the reset leaves
  // reset_live / reset_wait (the items of the other sub-pool); each set turns on one item, live while fewer than 16 (0x82726E64), else waiting
  uint32_t map = 0, reset_live = 0, reset_wait = 0;
  bool Ok(uint32_t p) const { return p >= 0x10000 && p < 0xC0000000u; }
  uint8_t U8(uint32_t a) { return mem.count(a) ? mem[a] : 0; }
  uint32_t U32(uint32_t a) { return words.count(a) ? words[a] : 0; }
  uint32_t Call(Fn f, uint64_t r3, uint64_t r4, uint64_t r5) {
    calls.push_back({f, r3, r4, r5});
    if (f == Fn::FindComponent) return r4 == kCbdcTag ? found_cbdc_comp : r4 == kCclmTag ? found_cclm_comp : 0;
    if (f == Fn::ResetBlood && map) words[map + 88] = reset_live, words[map + 100] = reset_wait;
    if ((f == Fn::SetFace || f == Fn::SetPart) && map) ++words[words[map + 88] < 16 ? map + 88 : map + 100];
    return 0;
  }
  void Log(const std::string& s) { log.push_back(s); }
};

constexpr uint32_t kCh = 0x40001000, kCbdc = 0x40002000, kCclm = 0x40003000;

static Fake World(int face, int p3, int p4, bool gate = true) {
  Fake w;
  // literal addresses from the listings (not the header's constants), so a wrong constant is caught
  w.mem[0x8376AAF4u + 15229u] = gate ? 1 : 0;  // 0x825A1F44
  w.mem[kCh + 1392] = uint8_t(face);           // 0x825A0FB8
  w.mem[kCh + 1396] = uint8_t(p3);             // 0x825A0FA4
  w.mem[kCh + 1397] = uint8_t(p4);             // 0x825A0F90
  w.words[kCbdc + kCbdcOwnerOff] = kCh;
  w.words[kCbdc + kCbdcMapOff] = kCclm;
  w.map = kCclm;
  w.words[kCclm + 88] = 9;  // before the reset: blood items still counted (a log of these would be wrong)
  w.words[kCclm + 100] = 3;
  w.reset_live = 4, w.reset_wait = 0;  // after it: 4 other items (the burst's frozen sweat) stay live
  return w;
}

static std::string Seq(const Fake& w) {
  std::string s;
  for (const Call& c : w.calls) {
    if (c.f == Fn::ResetBlood) s += "R" + std::to_string(c.r4) + " ";
    else if (c.f == Fn::SetFace) s += "F" + std::to_string(c.r4) + " ";
    else if (c.f == Fn::SetPart) s += "P" + std::to_string(c.r5) + ":" + std::to_string(c.r4) + " ";
    else s += "? ";
  }
  return s;
}

int main() {
  {
    Fake w = World(3, 0, 0);
    const bool ran = Regrow(w, "t", kCbdc);
    Check(ran && Seq(w) == "R0 F1 F2 F3 ", "levels 3/0/0: reset, then face 1 2 3", Seq(w));
    Check(w.calls.size() == 4 && w.calls[0].r3 == kCclm && w.calls[1].r3 == kCbdc, "reset gets the map, the sets get the cbdc");
    Check(w.log.size() == 1 && w.log[0] == "[wwe2k14][blood_keep] t char=40001000 levels=3/0/0 gate=1 live=4->7 waiting=0->0", "one log line: the counts after the reset and after the sets",
          w.log.empty() ? "" : w.log[0]);
  }
  {
    Fake w = World(3, 0, 0);
    w.reset_live = 16, w.reset_wait = 2;  // the frozen-sweat case (plan section 4 [U]): the live list is already full
    Regrow(w, "t", kCbdc);
    Check(w.log.size() == 1 && w.log[0] == "[wwe2k14][blood_keep] t char=40001000 levels=3/0/0 gate=1 live=16->16 waiting=2->5", "a full live list shows as live unchanged, every set waiting",
          w.log.empty() ? "" : w.log[0]);
  }
  {
    Fake w = World(3, 1, 2);
    Regrow(w, "t", kCbdc);
    Check(Seq(w) == "R0 F1 F2 F3 P1:1 P0:1 P0:2 ", "levels 3/1/2: face, part 3 (r5 1), part 4 (r5 0)", Seq(w));
  }
  {
    Fake w = World(0, 0, 2);
    Regrow(w, "t", kCbdc);
    Check(Seq(w) == "R0 P0:1 P0:2 ", "face 0, part 4 only", Seq(w));
  }
  {
    Fake w = World(0, 0, 0);
    Check(!Regrow(w, "t", kCbdc) && w.calls.empty(), "levels 0/0/0: no call");
    Check(w.log.size() == 1 && w.log[0] == "[wwe2k14][blood_keep] t char=40001000 levels=0/0/0 gate=1 skipped: no stored level", "levels 0/0/0: one skip line (the hook was reached)",
          w.log.empty() ? "" : w.log[0]);
  }
  {
    Fake w = World(3, 3, 3, false);
    Check(!Regrow(w, "t", kCbdc) && w.calls.empty(), "gate G+15229 = 0: no call even with levels", Seq(w));
    Check(w.log.size() == 1 && w.log[0] == "[wwe2k14][blood_keep] t char=40001000 levels=3/3/3 gate=0 skipped: blood off", "gate 0: one skip line", w.log.empty() ? "" : w.log[0]);
  }
  {
    Fake w = World(3, 0, 0);
    w.words[kCbdc + kCbdcMapOff] = 0;
    Check(!Regrow(w, "t", kCbdc) && w.calls.empty(), "map (cclm) 0: no call");
    Check(w.log.size() == 1 && w.log[0] == "[wwe2k14][blood_keep] t char=40001000 levels=3/0/0 gate=1 skipped: no map", "map 0: one skip line", w.log.empty() ? "" : w.log[0]);
    Fake v = World(3, 0, 0);
    Check(!Regrow(v, "t", 0) && v.calls.empty(), "cbdc 0: no call");
    Fake u = World(3, 0, 0);
    u.words[kCbdc + kCbdcOwnerOff] = 0;
    Check(!Regrow(u, "t", kCbdc) && u.calls.empty(), "owner char 0: no call");
  }
  {
    Fake w = World(5, 9, 255);
    Regrow(w, "t", kCbdc);
    Check(Seq(w) == "R0 F1 F2 F3 P1:1 P1:2 P1:3 P0:1 P0:2 P0:3 ", "a byte above 3 is capped at 3", Seq(w));
  }
  {
    Fake w = World(3, 1, 1);
    Regrow(w, "t", kCbdc);
    int resets = 0;
    for (const Call& c : w.calls) resets += (c.f == Fn::ResetBlood);
    const Call& last = w.calls.back();
    Check(resets == 1 && Seq(w).substr(0, 3) == "R0 " && (last.f == Fn::SetFace || last.f == Fn::SetPart), "one reset, first; nothing after the sets", Seq(w));
  }
  {
    Fake w;
    w.found_cbdc_comp = 0x40005000;
    w.words[0x40005000 + kCompDataOff] = kCbdc;
    Check(FindCbdc(w, 0x40004000) == kCbdc && w.calls.size() == 1 && w.calls[0].r3 == 0x40004000 && w.calls[0].r4 == kCbdcTag, "FindCbdc: list, tag 'CBDC', comp +24");
    Check(FindCbdc(w, 0) == 0, "FindCbdc of no list is 0");
  }
  {
    Fake w;
    w.found_cclm_comp = 0x40005000;
    w.words[0x40005000 + kCompDataOff] = kCclm;
    w.words[kCclm + kMapRegionsOff] = 0;
    Check(!RestoreRebuilt(w, 0x40004000), "restore: region entries 0 -> the original did not rebuild");
    w.words[kCclm + kMapRegionsOff] = 7;
    Check(RestoreRebuilt(w, 0x40004000), "restore: region entries set -> it rebuilt");
    Fake v;
    Check(!RestoreRebuilt(v, 0x40004000), "restore: no CCLM -> no rebuild");
  }
  std::printf("SUMMARY %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
