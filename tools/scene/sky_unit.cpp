// S103 lane SKY offline proof (docs\research\s103\TMR.md sections 3-4): the slot model key from the game's character descriptor. 0 game runs, no SDK.
//   sky_unit [--plant row_rule]
// Runs the SHIPPED pure rule (src\scene_live\slot_key.h) and the shipped contributor (WrestlerContributor::SubmitSlotKey) against a fake guest (address -> word) laid out
// the way the game builds it (sub_82314128: row record +16 = key, descriptor D+0 = key, char+1220 = i; sub_826D3E80: Rumble spawn into a free seat; sub_8251F290: slot swap).
// A plant swaps the rule under test for the old row rule; it must make NAMED checks fail (exit 1):
//   row_rule   the hook names bodies from the participant row   -> "swap.desc_follows_body", "rumble.rows_full_named", "rumble.seat_reoccupied" (it passes the real remap case)
//   desc_1220  the first SKY rule: the descriptor indexed by +1220 even with the remap active -> "remap.follows_1224"
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "../../src/scene_live/slot_key.h"
#include "../../src/scene_live/wrestler_contrib.h"

using namespace wwe2k14::slotkey;
using rdr_scene::live::WrestlerContributor;

namespace rdr_scene::live {   // stand-ins for the writer's registry (as wrestler_emit.cpp)
void Register(Contributor*) {}
bool Enabled() { return true; }
void EndFrame(uint64_t) {}
void AddSink(Sink*) {}
void RemoveSink(Sink*) {}
}  // namespace rdr_scene::live

static int g_fail = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

struct Guest {
  std::map<uint32_t, uint32_t> m;
  uint32_t rows = 0x40000000, ctx = 0x41000000;
  static uint32_t Char(int i) { return 0x42000000u + 0x1000u * uint32_t(i); }
  void W(uint32_t a, uint32_t v) { m[a] = v; }
  bool Read(uint32_t a, uint32_t& v) const { auto it = m.find(a); if (it == m.end()) return false; v = it->second; return true; }
  uint32_t Desc(uint32_t s) const { return ctx + 864 + 80 * s; }
  void Init() { W(kRowsRoot, rows); W(kMatchCtx, ctx); for (uint32_t r = 0; r < 43; ++r) W(rows + 148 + 56 * r + 16, 0); }
  void Row(uint32_t r, uint32_t key) { W(rows + 148 + 56 * r + 16, key); }
  // sub_82314128 for manager index i: descriptor key, +1220, slot table entry (no remap)
  void Seat(uint32_t i, uint32_t key, uint32_t table_index) { W(Desc(i), key); W(Char(int(i)) + 1220, i); W(kSlotTable + 4 * table_index, Char(int(i))); }
};

static bool g_plant_row = false, g_plant_1220 = false;
// the rule under test: the shipped desc rule, or (planted) the old row rule
static uint32_t Name(const Guest& g, int slot) {
  uint32_t ch = 0;
  g.Read(kSlotTable + 4 * uint32_t(slot), ch);
  auto rd = [&](uint32_t a, uint32_t& v) { return g.Read(a, v); };
  if (g_plant_1220) {   // planted twin: the first SKY rule (the descriptor indexed by +1220 even when the remap is active)
    uint32_t s = 0, ctx = 0, key = 0;
    if (slot < 0 || slot > 8 || !ch || !rd(ch + 1220, s) || s > 8 || !rd(kMatchCtx, ctx) || !rd(ctx + 864 + 80 * s, key) || !ValidKey(key)) return 0;
    return key;
  }
  return SlotKeyOf(rd, slot, ch, !g_plant_row);
}

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i)
    if (!std::strcmp(argv[i], "--plant")) { g_plant_row = !std::strcmp(argv[i + 1], "row_rule"); g_plant_1220 = !std::strcmp(argv[i + 1], "desc_1220"); }

  {   // 1v1 build layout: rows 0..1 occupied, 2..5 empty
    Guest g; g.Init();
    g.Row(0, 24902); g.Row(1, 10402);
    g.Seat(0, 24902, 0); g.Seat(1, 10402, 1);
    Check(Name(g, 0) == 24902 && Name(g, 1) == 10402, "build.1v1: desc key == row key for both slots");
    Check(Name(g, 2) == 0, "build.1v1: an empty slot has no key");
  }
  {   // 6-man build layout
    Guest g; g.Init();
    const uint32_t keys[6] = {24902, 10402, 31002, 5512, 7702, 9902};
    bool ok = true;
    for (uint32_t i = 0; i < 6; ++i) { g.Row(i, keys[i]); g.Seat(i, keys[i], i); }
    for (int s = 0; s < 6; ++s) ok = ok && Name(g, s) == keys[s];
    Check(ok, "build.6man: desc key == row key for every slot 0..5");
    bool agree = true;
    auto rd = [&](uint32_t a, uint32_t& v) { return g.Read(a, v); };
    for (int s = 0; s < 6; ++s) { bool u = false; agree = agree && RowKey(rd, s, u) == keys[s] && u; }
    Check(agree, "build.6man: the census row rule is usable and agrees");
  }
  {   // slot swap (sub_8251F290 semantics): table entries, +1220 and the two 80-byte descriptors swap; the participant rows do not
    Guest g; g.Init();
    g.Row(0, 24902); g.Row(1, 10402);
    g.Seat(0, 24902, 0); g.Seat(1, 10402, 1);
    const uint32_t c0 = Guest::Char(0), c1 = Guest::Char(1);
    g.W(kSlotTable + 0, c1); g.W(kSlotTable + 4, c0);      // table swapped
    g.W(c0 + 1220, 1); g.W(c1 + 1220, 0);                  // +1220 swapped
    g.W(g.Desc(0), 10402); g.W(g.Desc(1), 24902);          // descriptors move with the character
    // slot 0 now holds char c1 (the 10402 wrestler); slot 1 holds c0 (24902)
    Check(Name(g, 0) == 10402 && Name(g, 1) == 24902, "swap.desc_follows_body: the key follows the body that is now in the slot");
    auto rd = [&](uint32_t a, uint32_t& v) { return g.Read(a, v); };
    bool u = false;
    Check(RowKey(rd, 0, u) == 24902, "swap.row_rule_wrong: the old row rule still names the OTHER wrestler (why it was replaced)");
  }
  {   // Rumble: 40 rows filled, seat 3 re-occupied by entry 9 (sub_826D3E80: D+4 = entry, D+0 = entry row key, D+5 = seat)
    Guest g; g.Init();
    for (uint32_t r = 0; r < 40; ++r) g.Row(r, (100 + r) * 100 + 2);
    for (uint32_t i = 0; i < 6; ++i) g.Seat(i, (100 + i) * 100 + 2, i);
    Check(Name(g, 3) == 10302, "rumble.rows_full_named: seat 3 holds entry 3 (the row rule has no key here: rows 6..39 are filled)");
    g.W(g.Desc(3), g.m[g.rows + 148 + 56 * 9 + 16]);   // entry 9 takes seat 3: D+0 = row 9's key, +1220 stays 3
    Check(Name(g, 3) == 10902, "rumble.seat_reoccupied: seat 3 re-occupied by entry 9 names entry 9's key");
    auto rd = [&](uint32_t a, uint32_t& v) { return g.Read(a, v); };
    bool u = true;
    RowKey(rd, 3, u);
    Check(!u, "rumble.row_rule_unusable: the census sees row_unusable");
  }
  {   // the same skeleton address reused with an old declared link: SubmitSlotKey drops it (wrestler_contrib.h:257-262)
    const uint32_t kSkel = 0x60000000;
    WrestlerContributor wt;
    wt.SubmitModelLink(kSkel, 10302, 0, 0, 0x3, {}, 109);   // the old seat-3 occupant (character 103)
    Check(wt.ModelKeyFor(kSkel).known, "reuse.link_present: the declared link is there before the new occupant");
    wt.SubmitSlotKey(kSkel, 10902);                          // the new occupant (character 109) at the same address
    Check(!wt.ModelKeyFor(kSkel).known, "reuse.link_dropped: the old character's link is dropped by the new occupant's key");
    wt.SubmitModelLink(kSkel, 10902, 0, 0, 0x3, {}, 109);
    wt.SubmitSlotKey(kSkel, 10912);                          // same character, other attire: the link stays
    Check(wt.ModelKeyFor(kSkel).known, "reuse.same_character_keeps_link");
  }
  {   // the game's real remap (story / Showcase): [[0x8392475C]+0] != 0, word table at +4. sub_827EFCD0 moves the descriptor of ORIGINAL index m into D_j where table[j] == m (D_j+5 = j);
      // sub_8251DEA0 / sub_82520C58 write char_j+1220 = table[j]; sub_8251E928 writes char_j+1224 = j and registers char_j at slot table position table[j]. Rows stay original.
    Guest g; g.Init();
    const uint32_t ptr = 0x43000000;
    g.W(kRemapTable, ptr); g.W(ptr, 1);
    const uint32_t orig[3] = {24902, 10402, 31002};   // original descriptors O[m] (= the participant rows)
    const uint32_t table[3] = {1, 2, 0};              // j -> m (a 3-cycle: not an involution, so +1220 as an index is wrong)
    for (uint32_t m = 0; m < 3; ++m) g.Row(m, orig[m]);
    for (uint32_t j = 0; j < 3; ++j) {
      g.W(ptr + 4 + 4 * j, table[j]);
      g.W(g.Desc(j), orig[table[j]]);                  // D_j = O[table[j]]
      g.W(Guest::Char(int(j)) + 1220, table[j]);
      g.W(Guest::Char(int(j)) + 1224, j);
      g.W(kSlotTable + 4 * table[j], Guest::Char(int(j)));   // slot = table[j]
    }
    bool ok = true;
    for (uint32_t j = 0; j < 3; ++j) ok = ok && Name(g, int(table[j])) == orig[table[j]];
    Check(ok, "remap.follows_1224: with the real remap, the key is the body's (D indexed by char+1224)");
    auto rd = [&](uint32_t a, uint32_t& v) { return g.Read(a, v); };
    bool rowok = true;
    for (int s = 0; s < 3; ++s) { bool u = false; rowok = rowok && RowKey(rd, s, u) == orig[s]; }
    Check(rowok, "remap.row_rule_ok: the old row rule happens to pass this case (rows = original indices)");
  }
  {   // remap table present but its active word is 0: not active, +1220 is the index
    Guest g; g.Init();
    const uint32_t ptr = 0x43000000;
    g.W(kRemapTable, ptr); g.W(ptr, 0);
    g.Seat(0, 24902, 0); g.W(Guest::Char(0) + 1224, 7);
    Check(Name(g, 0) == 24902, "remap.inactive_uses_1220");
  }
  {   // bad values
    Guest g; g.Init();
    g.Seat(0, 24902, 0);
    g.W(Guest::Char(0) + 1220, 9);
    Check(Name(g, 0) == 0 || g_plant_row, "bad.s_out_of_range: +1220 > 8 gives no key");
    g.W(Guest::Char(0) + 1220, 0);
    g.W(g.Desc(0), 51200);
    Check(Name(g, 0) == 0 || g_plant_row, "bad.invalid_key: 51200 gives no key");
    Check(Name(g, 9) == 0, "bad.slot_9: not a slot");
  }
  std::printf("%s\n", g_fail ? "SKY UNIT FAILED" : "SKY UNIT PASSED");
  return g_fail ? 1 : 0;
}
