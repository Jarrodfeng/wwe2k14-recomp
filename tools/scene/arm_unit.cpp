// S103 ARM: offline proof of the attire remap (src\assets\attire_remap.h, docs\agents\s103\HNN.md) and of the hole rule for a failed composite / remapped attire load
// (src\renderer_scene\named_load_fail.h HoleOnFailure). 0 game runs; selftest uses a fixture table built here (nothing from the game).
//   arm_unit.exe selftest            the rule on a fixture table + the planted faults + the hole rule in the production order (loader failure -> Note -> contributor test)
//   arm_unit.exe real --game DIR     read-only, the player's own files: p0/600 of misc01_Start.pac, the census over every ch pac (which attire keys the game remaps, whether the base
//                                    model exists), the pick the loader makes for Heenan / Jericho / Punk / Elizabeth / Savage / Hogan and the four forced keys
// Build: pwsh -NoProfile -File tools\scene\build_arm_unit.ps1
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "../../src/assets/attire_remap.h"
#include "../../src/renderer_scene/named_load_fail.h"

namespace fs = std::filesystem;
namespace ar = assets::attire_remap;

static int g_fail = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}

// One 664-byte record: id, then per slot (flagA, flagB, kind-2 word, kind-2 attire, kind-4 word, kind-4 attire).
struct Slot { int a = 0, b = 0, w2 = 0, at2 = 0, w4 = 0, at4 = 0; };
static assets::Bytes Table(const std::map<int, std::map<int, Slot>>& recs) {
  assets::Bytes t(2 + ar::kRecord * recs.size(), 0);
  t[0] = uint8_t(recs.size() & 0xFF), t[1] = uint8_t(recs.size() >> 8);
  size_t k = 0;
  for (const auto& [cid, slots] : recs) {
    uint8_t* r = t.data() + 2 + ar::kRecord * k++;
    r[0] = uint8_t(cid & 0xFF), r[1] = uint8_t(cid >> 8);
    for (const auto& [s, v] : slots) {
      r[4 + s] = uint8_t(v.a), r[14 + s] = uint8_t(v.b), r[24 + s] = uint8_t(v.w2), r[34 + s] = uint8_t(v.at2), r[44 + s] = uint8_t(v.w4), r[54 + s] = uint8_t(v.at4);
      for (int i = 0; i < 48; ++i) r[64 + 48 * s + i] = uint8_t(s * 16 + i);   // slot-distinct colours
    }
  }
  return t;
}

static int SelfTest() {
  // the shape CMP2 read from the file: Heenan 249 s1 -> a0; Jericho 104 s1 s2 -> a0; Punk 131 s1; Elizabeth 253 s1-3; Savage 197 s1 s2 (kind 4 word 1 -> a0 kind 4); Hogan 109 identity
  std::map<int, std::map<int, Slot>> recs;
  recs[249][1] = {1, 1, 0, 0, 0, 0};
  recs[104][1] = {1, 1, 0, 0, 0, 0}, recs[104][2] = {1, 1, 0, 0, 0, 0};
  recs[131][1] = {1, 1, 0, 0, 0, 0};
  for (int s = 1; s <= 3; ++s) recs[253][s] = {1, 1, 0, 0, 0, 0};
  recs[197][1] = {1, 1, 0, 0, 1, 0}, recs[197][2] = {1, 1, 0, 0, 1, 0};
  for (int s = 1; s <= 5; ++s) recs[109][s] = {1, 1, 0, s, 0, s};   // identity: attire = s
  recs[363][0] = {1, 0, 0, 0, 0, 0};                                  // flag A only
  recs[150][1] = {0, 0, 0, 0, 0, 0};                                  // listed but flag A clear: nothing remaps
  recs[160][1] = {1, 1, 2, 0, 2, 0};                                  // word 2 = no remap
  ar::Table t;
  Check(ar::Parse(Table(recs), t) && t.count == 9, "the fixture table parses (9 records)");
  Check(!ar::Parse(assets::Bytes{0x05, 0x00, 0, 0}, t), "a table shorter than its record count is refused");
  Check(ar::Parse(Table(recs), t), "fixture re-parsed");
  {  // the game's scan continues past an id match whose flag A != 1 (sub_822D1598), and the id is a LE u32
    std::map<int, std::map<int, Slot>> two;
    two[300][1] = {0, 0, 0, 0, 0, 0};
    ar::Table t2;
    assets::Bytes b = Table(two);
    assets::Bytes second = Table({{300, {{1, {1, 1, 0, 0, 0, 0}}}}});
    b[0] = 2;
    b.insert(b.end(), second.begin() + 2, second.end());
    Check(ar::Parse(b, t2) && ar::Resolve(t2, 30012).to == 30002 && ar::Resolve(t2, 30012).remapped(), "a first record of the id with flag A clear is skipped: the second record remaps");
  }

  auto to = [&](int key, ar::Fault f = ar::Fault::kNone) { return ar::Resolve(t, key, f).to; };
  Check(to(24912) == 24902 && ar::Resolve(t, 24912).slot == 1 && ar::Resolve(t, 24912).remapped(), "Heenan 24912 -> 24902, colour slot 1");
  Check(to(10412) == 10402 && to(10422) == 10402, "Jericho 10412 / 10422 -> 10402");
  Check(to(13112) == 13102, "CM Punk 13112 -> 13102");
  Check(to(25312) == 25302 && to(25322) == 25302 && to(25332) == 25302, "Elizabeth 25312 / 25322 / 25332 -> 25302");
  Check(to(19712) == 19702 && to(19722) == 19702, "Savage 19712 / 19722 (kind 2) -> 19702");
  Check(to(19714) == 19704 && to(19724) == 19704, "Savage kind 4 (19714 / 19724) with kind-4 word 1 -> 19704 (kind 4 of attire 0)");
  Check(to(10912) == 10912 && to(10932) == 10932 && !ar::Resolve(t, 10912).remapped() && ar::Resolve(t, 10912).flag_a, "Hogan 10912 / 10932 unchanged (identity attire = s), still listed");
  Check(to(24902) == 24902 && to(24922) == 24922, "an unlisted slot / attire (Heenan a0, a2) is unchanged");
  Check(to(36302) == 36302 && !ar::Resolve(t, 36302).flag_b && ar::Resolve(t, 36302).flag_a, "363 s0: flag A only, kind 2 word 0 attire 0 -> itself");
  Check(to(15012) == 15012 && !ar::Resolve(t, 15012).flag_a, "flag A clear: no remap");
  Check(to(16012) == 16012 && to(16014) == 16014, "word 2: no remap");
  Check(to(99912) == 99912 && ar::Resolve(t, 99912).slot == 1 && !ar::Resolve(t, 99912).flag_a, "an unknown character: unchanged");
  Check(to(24911) == 24911 && to(24913) == 24913, "kinds other than 2 / 4 are never remapped");
  // PLANTED FAULTS: each must break at least one of the lines above
  Check(to(24912, ar::Fault::kNoRemap) == 24912, "PLANT (no remap): 24912 stays 24912 -> the pac has no such PACH, the load fails (the garbage coat of run 042)");
  Check(to(15012, ar::Fault::kIgnoreFlagA) == 15002, "PLANT (flag A ignored): an unlisted attire WOULD remap (caught by the flag-A line)");
  Check(to(16012, ar::Fault::kWordIgnored) != 16012, "PLANT (word ignored): word 2 would remap (caught by the word-2 line)");
  Check(ar::Resolve(t, 24912, ar::Fault::kSlotZero).slot != 1, "PLANT (slot 0): the colour slot is lost (caught by the slot-1 line)");

  // the hole rule in the production order: the loader fails a composite -> HoleOnFailure; ModelCache notes it -> Note; the contributor asks FailedToGuest
  using namespace rdr_scene;
  namedfail::Clear();
  const std::string pac = "pac\\ch\\ch249.pac", entry = "model_key:24912";
  namedfail::HoleOnFailure(true, false, pac, entry, 1);        // the loader: composite attire, the load failed
  Check(namedfail::Note(pac, entry, 1), "ModelCache notes the failed load (the census line)");
  Check(namedfail::Failed(pac, entry, 1) && !namedfail::FailedToGuest(pac, entry, 1) && namedfail::IsHole(pac, entry, 1), "a failed composite attire is a HOLE: the guest rows are not asked (FailedToGuest false)");
  Check(namedfail::Holes() == 1, "counted: attire_load_hole 1");
  const std::string dlc = "model_key:38510";
  namedfail::HoleOnFailure(false, false, "pac\\ch\\ch385.pac", dlc, 1);
  namedfail::Note("pac\\ch\\ch385.pac", dlc, 1);
  Check(namedfail::FailedToGuest("pac\\ch\\ch385.pac", dlc, 1) && namedfail::Holes() == 1, "a plain failed load (a DLC wrestler) still falls back to the guest rows (DLW), not counted a hole");
  const std::string tagged = "model_key:24902/c:00";
  Check(namedfail::HoleOnFailure(false, true, pac, tagged, 0), "a tagged entry (/c: or /r) failing is a hole whatever the table says");
  namedfail::Note(pac, tagged, 0);
  Check(!namedfail::FailedToGuest(pac, tagged, 0) && namedfail::Holes() == 2, "tagged: not guest rows; counted 2");
  Check(!namedfail::HoleOnFailure(true, false, pac, entry, 1) && namedfail::Holes() == 2, "the same ref twice is counted once");
  // PLANT: the pre-ARM rule (the contributor asks Failed, not FailedToGuest) would draw the composite from the guest rows
  Check(namedfail::Failed(pac, entry, 1), "PLANT (pre-ARM: Failed): the composite attire WOULD go to the guest rows");
  namedfail::Clear();
  Check(!namedfail::IsHole(pac, entry, 1), "Clear (a new game folder) forgets the holes");

  if (g_fail) std::printf("FAIL  %d check(s) failed\n", g_fail);
  else std::printf("PASS  arm_unit selftest\n");
  return g_fail ? 1 : 0;
}

static int Real(const char* game_dir) {
  launcher::GameFiles files;
  files.SetRoot(fs::path(game_dir));
  ar::Table t;
  std::string err;
  Check(ar::ReadTable(files, t, err), "pac\\misc01_Start.pac p0/600 reads");
  if (!t.ok) { std::printf("      %s\n", err.c_str()); return 1; }
  std::printf("      %u records\n", t.count);
  int remapped = 0, composite = 0, base_missing = 0, own_conflict = 0, ident = 0;
  std::map<int, int> per_char;
  std::printf("      census (key -> loaded, colour slot; own = the pac has a PACH named by the key; base = the PACH the game loads exists):\n");
  for (uint32_t k = 0; k < t.count; ++k) {
    const int cid = t.data[2 + ar::kRecord * k] | (t.data[2 + ar::kRecord * k + 1] << 8);
    for (int s = 0; s < ar::kSlots; ++s)
      for (int kind : {2, 4}) {
        const int key = cid * 100 + s * 10 + kind;
        const ar::Remap r = ar::Resolve(t, key);
        if (!r.flag_a) continue;
        const int own = assets::OrdinalOfModelKey(files, cid, key), base = assets::OrdinalOfModelKey(files, cid, r.to);
        ++composite;
        if (!r.remapped()) { ++ident; continue; }
        ++remapped, ++per_char[cid];
        if (base < 0) ++base_missing;
        if (own >= 0) ++own_conflict;
        std::printf("      %d -> %d slot %d flagB %d own %s base %s\n", key, r.to, r.slot, r.flag_b ? 1 : 0, own >= 0 ? "YES" : "no", base >= 0 ? "yes" : "MISSING");
      }
  }
  std::printf("      %d flag-A keys (kind 2 / 4), %d remapped, %d identity; wrestlers with a remap:", composite, remapped, ident);
  for (const auto& [c, n] : per_char) std::printf(" %d(x%d)", c, n);
  std::printf("\n");
  Check(remapped > 0, "the game remaps at least one attire in the player's table");
  Check(base_missing == 0, "every remapped key's base model exists in its character pac");
  Check(own_conflict == 0, "no remapped key also has a PACH of its own (so 'only when the pac lacks the key' loses nothing)");
  // the loader's pick (production function), in the production order: the pac's own key first, else the game's remap
  auto pick = [&](int key) { return ar::PickModel(files, t, key / 100, key); };
  const ar::Pick h = pick(24912), h0 = pick(24902);
  Check(h.ord >= 0 && h.loaded == 24902 && h.remapped && h.slot == 1 && h.ord == h0.ord && h.hole_class, "Heenan 24912: loads PACH of 24902, colour slot 1, a composite (hole class)");
  Check(pick(10412).loaded == 10402 && pick(10422).loaded == 10402, "Jericho 10412 / 10422 load 10402");
  Check(pick(13112).loaded == 13102, "CM Punk 13112 loads 13102");
  Check(pick(25312).loaded == 25302, "Elizabeth 25312 loads 25302");
  Check(pick(19712).loaded == 19702 || pick(19712).ord >= 0, "Savage 19712 resolves");
  Check(pick(10902).ord >= 0 && !pick(10902).remapped, "Hogan 10902 unchanged");
  for (int fk : {10912, 10932, 19714, 19724}) {
    char what[80];
    const ar::Pick p = pick(fk);
    std::snprintf(what, sizeof what, "forced key %d still resolves (loaded %d, ord %d%s)", fk, p.loaded, p.ord, p.remapped ? ", remapped" : "");
    Check(p.ord >= 0, what);
  }
  // review F3: an identity flag-A key keeps the old fallback, so its model must exist in its pac (listed when not)
  int idmiss = 0, nomodel = 0;
  for (uint32_t k = 0; k < t.count; ++k) {
    const int cid = int(ar::Table::Id(t.data.data() + 2 + ar::kRecord * k));
    for (int s = 0; s < ar::kSlots; ++s)
      for (int kind : {2, 4}) {
        const int key = cid * 100 + s * 10 + kind;
        const ar::Remap r = ar::Resolve(t, key);
        if (!r.flag_a || r.remapped()) continue;
        const ar::Pick p = pick(key);
        if (p.ord < 0) { ++nomodel; std::printf("      identity flag-A key %d has NO model in ch%d.pac (an entrance key: the old fallback, the guest rows, as before ARM)\n", key, cid); }
        if (p.hole_class) { ++idmiss; std::printf("      identity key %d is hole class\n", key); }
      }
  }
  std::printf("      %d identity flag-A keys have no model of their own (listed above)\n", nomodel);
  Check(idmiss == 0, "no identity flag-A key is a hole class (they keep the old fallback)");
  // PLANT: no remap table -> 24912 has no model: the pre-ARM failure
  const ar::Table none;
  Check(ar::PickModel(files, none, 249, 24912).ord < 0, "PLANT (no remap): 24912 resolves to nothing (the failed load of run 042)");
  if (g_fail) std::printf("FAIL  %d check(s) failed\n", g_fail);
  else std::printf("PASS  arm_unit real\n");
  return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc >= 2 && !std::strcmp(argv[1], "selftest")) return SelfTest();
  if (argc >= 4 && !std::strcmp(argv[1], "real") && !std::strcmp(argv[2], "--game")) return Real(argv[3]);
  std::printf("usage: arm_unit selftest | real --game DIR\n");
  return 2;
}
