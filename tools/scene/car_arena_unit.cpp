// car_arena_unit: the offline proof of S103 lane CAR piece 1 (BCA G1): a Create-an-Arena venue (ids 200-249) from the game's own part pacs (src\assets\car_arena.h, asset_load.h LoadCustomArena / OpenCached).
//   car_arena_unit [--game DIR] [--plant NAME] [--sel CAEN CASD CARL]
// Part A (no game files): MergeParts on synthetic parts: the three slot bands, the texture directory remap, the CARL light entries, the joined slot text, duplicates, a missing part; the row round trip
// (ReadCarSel / CarToRows / CarFromRows against a fake guest) and the selection registry (the thread-local override, the key).
// Part B (--game DIR, the player's own files): the real parts CAEN n / CASD n / CARL n + CARG 0 through LoadCustomArena, then the crowd / blood readers through OpenCached; the counts are printed.
// A plant is a FAULT in the thing under test; each must make a NAMED check fail (exit 1 when any check failed):
//   missing_part    the merge is handed no CASD part, the bands check expects all three      -> "merge.bands_all_present"
//   old_bg_path     the venue is loaded the old way (the missing pac\bg\bg200.pac, no selection) -> "real.loads_created_arena"
//   key_ignored     the registry key leaves out the CARL index                                -> "sel.key_distinguishes"
//   light_casd      the CASD light entries win over CARL's                                    -> "merge.carl_lights_win"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../../src/assets/asset_load.h"
#include "../../src/renderer_scene/rows_stage_fx.h"
#include "../../src/renderer_scene/car_arena_rows.h"
#include "../../src/scene_live/arena_provider.h"
#include "../../src/scene_live/stage_fx_contrib.h"

static int g_fail = 0, g_checks = 0;
static std::string g_plant;
static bool Plant(const char* n) { return g_plant == n; }
static void Check(bool ok, const char* name, const std::string& why = "") {
  ++g_checks;
  std::printf("%s  %-34s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}

using assets::Bytes;
namespace car = assets::car;

// a fabricated part: entries id -> bytes (the PACH header is not needed: MergeParts reads Pac::bytes + Pach::ents)
struct Fake {
  assets::pac::Pac pac;
  void Add(uint32_t id, const std::string& s) {
    assets::pac::Ent e;
    e.id = id, e.off = pac.bytes.size(), e.size = s.size();
    pac.bytes.insert(pac.bytes.end(), s.begin(), s.end());
    if (pac.pachs.empty()) pac.pachs.emplace_back();
    pac.pachs[0].ents.push_back(e);
  }
  car::PartSrc Src() const { return {&pac, &pac.pachs[0]}; }
};
static std::string Text(const assets::pac::Pac& p, uint32_t id) {
  const assets::pac::Ent* e = assets::pac::Find(p.pachs[0], id);
  return e ? std::string(reinterpret_cast<const char*>(p.bytes.data() + e->off), e->size) : std::string("<none>");
}

int main(int argc, char** argv) {
  std::string game;
  uint32_t sel_caen = 0, sel_casd = 0, sel_carl = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--plant" && i + 1 < argc) g_plant = argv[++i];
    else if (a == "--game" && i + 1 < argc) game = argv[++i];
    else if (a == "--sel" && i + 3 < argc) sel_caen = uint32_t(std::atoi(argv[i + 1])), sel_casd = uint32_t(std::atoi(argv[i + 2])), sel_carl = uint32_t(std::atoi(argv[i + 3])), i += 3;
  }

  // ---------------------------------------------------------------- A1. the merge of synthetic parts
  {
    Fake casd, caen, carg, carl;
    for (uint32_t s : {0u, 5u, 100u, 199u}) casd.Add(s, "casd-slot");
    casd.Add(400, "TEX-CASD400"), casd.Add(20000, "crowd"), casd.Add(30000, "KEY-CASD"), casd.Add(30002, "SETS-CASD"), casd.Add(50001, "10 l(1)\n"), casd.Add(50020, "tree");
    for (uint32_t s : {200u, 300u, 399u}) caen.Add(s, "caen-slot");
    caen.Add(400, "TEX-CAEN400"), caen.Add(401, "TEX-CAEN401"), caen.Add(800, "screen"), caen.Add(30001, "MOTION"), caen.Add(50001, "200 l(2)"), caen.Add(100, "DUP-IN-CAEN");   // 100: a duplicate of CASD's
    for (uint32_t s : {25u, 60u, 94u}) carg.Add(s, "carg-slot");
    carg.Add(400, "TEX-CARG400"), carg.Add(402, "TEX-CARG402"), carg.Add(952, "turnbuckle"), carg.Add(32000, "blood"), carg.Add(50001, "25 l(3)\n");
    carl.Add(30000, "KEY-CARL"), carl.Add(30002, "SETS-CARL"), carl.Add(30003, "SPECIAL-CARL");
    assets::pac::Pac out;
    car::MergeInfo mi;
    std::string err;
    const car::PartSrc s_casd = Plant("missing_part") ? car::PartSrc() : casd.Src();
    const bool ok = car::MergeParts(s_casd, caen.Src(), carg.Src(), carl.Src(), out, mi, err);
    Check(ok && out.pachs.size() == 1, "merge.one_pach", err);
    Check(ok && mi.slots_casd == 4 && mi.slots_caen == 3 && mi.slots_carg == 3 && mi.slot_max_casd == 199 && mi.slot_min_caen == 200 && mi.slot_max_caen == 399 && mi.slot_min_carg == 25 && mi.slot_max_carg == 94, "merge.bands_all_present",
          "CASD 0-199 (4), CAEN 200-399 (3), CARG 25-94 (3): got " + std::to_string(mi.slots_casd) + " / " + std::to_string(mi.slots_caen) + " / " + std::to_string(mi.slots_carg));
    if (ok) {
      Check(Text(out, 400) == "TEX-CASD400" && Text(out, 403) == "TEX-CAEN400" && Text(out, 404) == "TEX-CARG400" && Text(out, 401) == "TEX-CAEN401" && Text(out, 402) == "TEX-CARG402", "merge.texture_dirs",
            "CASD 400 -> 400, CAEN 400 -> 403, CARG 400 -> 404, 401 / 402 kept");
      const bool carl_wins = Text(out, 30000) == (Plant("light_casd") ? "KEY-CASD" : "KEY-CARL") && Text(out, 30002) == "SETS-CARL" && Text(out, 30003) == "SPECIAL-CARL" && Text(out, 30001) == "MOTION";
      Check(carl_wins, "merge.carl_lights_win", "30000 / 30002 / 30003 from CARL, 30001 from CAEN");
      const std::string t = Text(out, 50001);
      Check(t.find("25 l(3)") != std::string::npos && t.find("200 l(2)") != std::string::npos && (!Plant("missing_part") ? t.find("10 l(1)") != std::string::npos : true) && t.find("25 l(3)") < t.find("200 l(2)"), "merge.text_joined",
            "CARG then CAEN then CASD, newline separated");
      Check(mi.duplicates >= 1 && Text(out, 100) == "casd-slot", "merge.duplicate_first_wins", "slot 100 in CAEN too: CASD's kept, " + std::to_string(mi.duplicates) + " duplicate(s) counted");
      Check(Text(out, 20000) == "crowd" && Text(out, 800) == "screen" && Text(out, 952) == "turnbuckle" && Text(out, 32000) == "blood" && Text(out, 50020) == "tree", "merge.unique_ids_carried", "crowd 20000, screen 800, ring 952, blood 32000, tree 50020");
    }
    // a genuinely missing part: no CAEN part: the venue still builds, the band is absent, the census says so
    assets::pac::Pac out2;
    car::MergeInfo m2;
    const bool ok2 = car::MergeParts(casd.Src(), car::PartSrc(), carg.Src(), carl.Src(), out2, m2, err);
    Check(ok2 && !m2.have_caen && m2.slots_caen == 0 && m2.slots_casd == 4 && assets::pac::Find(out2.pachs[0], 200) == nullptr, "merge.missing_part_degrades", "no CAEN: CASD + CARG merge, no slot 200-399, have_caen false");
    assets::pac::Pac out3;
    car::MergeInfo m3;
    Check(!car::MergeParts(car::PartSrc(), car::PartSrc(), car::PartSrc(), carl.Src(), out3, m3, err), "merge.no_scenery_fails", "only CARL: nothing to draw: false");
  }

  // ---------------------------------------------------------------- A2. the selection registry, the row round trip
  {
    car::Sel a;
    a.valid = true, a.arena = 200, a.caen = 3, a.casd = 2, a.carl = 1, a.bg = 7;
    car::Sel b = a;
    b.carl = 9;
    std::string ka = a.Key(), kb = b.Key();
    if (Plant("key_ignored")) ka = ka.substr(0, ka.rfind('/', ka.rfind('/') - 1)), kb = kb.substr(0, kb.rfind('/', kb.rfind('/') - 1));   // the CARL index (and the bg id) dropped
    Check(ka != kb, "sel.key_distinguishes", "two selections that differ only in the light have different keys: " + ka + " vs " + kb);
    car::Clear();
    Check(!car::Current().valid, "sel.cleared", "");
    car::Publish(a);
    {
      const car::ScopedThreadSel ts(b);
      Check(car::Current() == b, "sel.thread_override", "this thread's selection wins");
    }
    Check(car::Current() == a, "sel.global_after_scope", "");
    car::Clear();

    // a fake guest: stage at 0x40000000
    constexpr uint32_t kStageAt = 0x40000000u;
    std::map<uint32_t, uint8_t> mem;
    auto W = [&](uint32_t at, uint32_t v) {
      for (int i = 0; i < 4; ++i) mem[at + uint32_t(i)] = uint8_t(v >> (24 - 8 * i));
    };
    W(kStageAt + 21184, 5), W(kStageAt + 21188, 3), W(kStageAt + 21192, 22);
    auto rd = [&](uint32_t at, void* d, size_t n) {
      for (size_t i = 0; i < n; ++i) {
        auto it = mem.find(at + uint32_t(i));
        if (it == mem.end()) return false;
        static_cast<uint8_t*>(d)[i] = it->second;
      }
      return true;
    };
    car::Sel got;
    const bool read = rdr_scene::ReadCarSel(rd, kStageAt, 200, 2, got);
    Check(read && got.valid && got.arena == 200 && got.caen == 5 && got.casd == 3 && got.carl == 2 && got.bg == 22, "row.read_from_stage", "CAEN [+21184], CASD [+21188], bg [+21192], CARL from the hook");
    car::Sel none;
    Check(!rdr_scene::ReadCarSel(rd, kStageAt, 200, -1, none) && !none.valid, "row.no_hook_no_row", "the light hook never fired: not valid (fail closed)");
    mem.erase(kStageAt + 21188);
    Check(!rdr_scene::ReadCarSel(rd, kStageAt, 200, 2, none), "row.unread_fails_closed", "an unreadable word = not valid");
    W(kStageAt + 21188, 3);
    std::vector<rdr_scene::StageFxRow> rows;
    rdr_scene::CarToRows(got, true, kStageAt, 200, rows);
    int arena = -1;
    car::Sel back;
    Check(rdr_scene::CarFromRows(rows, arena, back) && arena == 200 && back == got, "row.roundtrip", "the row carries the selection and the arena id");
    rows.clear();
    rdr_scene::CarToRows(got, false, kStageAt, 201, rows);
    Check(rdr_scene::CarFromRows(rows, arena, back) && arena == 201 && !back.valid, "row.unread_row_not_valid", "flags bit 0 clear: the applier gets no selection");
    rdr_scene::StageFxRow odd;
    rows.assign(1, odd);
    Check(!rdr_scene::CarFromRows(rows, arena, back), "row.absent", "no kFxCar row: false (an ordinary arena's stage)");
    car::Sel hi;
    mem[kStageAt + 21184] = 0x00, mem[kStageAt + 21185] = 0x00, mem[kStageAt + 21186] = 0x10, mem[kStageAt + 21187] = 0x00;   // CAEN 4096: not a part index
    Check(!rdr_scene::ReadCarSel(rd, kStageAt, 200, 2, hi), "row.insane_index_refused", "an index past the part counts is not a row");
  }

  // ---------------------------------------------------------------- A3. S103 CAR fix round: the arena in the selection, the all-zero stage, the latch publishes, the slot-table key
  {
    constexpr uint32_t kStageAt = 0x40000000u;
    std::map<uint32_t, uint8_t> mem;
    auto W = [&](uint32_t at, uint32_t v) {
      for (int i = 0; i < 4; ++i) mem[at + uint32_t(i)] = uint8_t(v >> (24 - 8 * i));
    };
    auto rd = [&](uint32_t at, void* d, size_t n) {
      for (size_t i = 0; i < n; ++i) {
        auto it = mem.find(at + uint32_t(i));
        if (it == mem.end()) return false;
        static_cast<uint8_t*>(d)[i] = it->second;
      }
      return true;
    };
    W(kStageAt + 1216, 205), W(kStageAt + 21184, 0), W(kStageAt + 21188, 0), W(kStageAt + 21192, 0);
    car::Sel z;
    Check(!rdr_scene::ReadCarSel(rd, kStageAt, 205, 0, z), "row.all_zero_is_unread", "bg 0 / CAEN 0 / CASD 0 = the constructor's zeroes, not a venue");
    W(kStageAt + 21184, 4), W(kStageAt + 21188, 2), W(kStageAt + 21192, 0);
    Check(rdr_scene::ReadCarSel(rd, kStageAt, 205, 1, z) && z.arena == 205 && z.caen == 4 && z.bg == 0, "row.zero_bg_with_parts_ok", "bg 0 with real part indices is a venue");
    car::Sel other = z;
    other.arena = 206;
    Check(z.Key() != other.Key() && z != other, "sel.key_has_arena", "the arena id is part of the key");
    // the latch publishes (blocker a): a take of a created arena's stage fills the registry with the stage's own arena id
    car::Clear();
    static std::map<uint32_t, uint8_t>* gmem;
    gmem = &mem;
    rdr_scene::live::stagefx::StageFxContributor con;
    con.SetReader(+[](uint32_t a, void* d, size_t n) {
      for (size_t i = 0; i < n; ++i) {
        auto it = gmem->find(a + uint32_t(i));
        if (it == gmem->end()) return false;
        static_cast<uint8_t*>(d)[i] = it->second;
      }
      return true;
    });
    con.SetStage(kStageAt);
    con.NoteCarLight(3);
    rdr_scene::live::stagefx::StageFxContributor::Frame fr;
    con.Take(10, fr);
    const car::Sel cur = car::Current();
    Check(fr.car_present && fr.car_ok && cur.valid && cur.arena == 205 && cur.caen == 4 && cur.casd == 2 && cur.carl == 3, "take.publishes_selection", "Take reads [stage+1216] = 205 and publishes CAEN 4 / CASD 2 / CARL 3");
    car::Clear();
    // the slot table is keyed by the selection (blocker c): published for one selection, wanted again for another
    car::Sel s1 = cur;
    s1.arena = 205;
    car::Publish(s1);
    Check(rdr_scene::live::arena::NeedsSlots(205), "slots.needed_until_published", "no table yet");
    {
      const car::ScopedThreadSel ts(s1);
      std::vector<rdr_scene::live::arena::SlotSnap> sl(1);
      rdr_scene::live::arena::PublishSlots(205, rdr_scene::live::arena::Look::Match, std::move(sl), {});
    }
    Check(!rdr_scene::live::arena::NeedsSlots(205), "slots.published_for_selection", "the table of this selection is published");
    car::Sel s2 = s1;
    s2.caen = 9;
    car::Publish(s2);
    Check(rdr_scene::live::arena::NeedsSlots(205), "slots.new_selection_needs_table", "another selection: the table is wanted again");
    car::Clear();
    Check(rdr_scene::live::arena::NeedsSlots(205), "slots.no_selection_needs_table", "no selection published: still not ready");
    rdr_scene::live::arena::PublishAll(rdr_scene::live::arena::ArenaSnapshot());   // leave the contributor state clean
  }

  // ---------------------------------------------------------------- B. the player's own part pacs
  if (!game.empty()) {
    launcher::GameFiles files;
    files.SetRoot(game);
    car::Sel sel;
    sel.valid = true, sel.arena = 200, sel.caen = sel_caen, sel.casd = sel_casd, sel.carl = sel_carl, sel.bg = 5;
    assets::Model m;
    std::string err;
    car::Clear();
    bool ok;
    if (Plant("old_bg_path")) ok = assets::LoadArena(files, 200, m, err);   // the old way: the missing pac\bg\bg200.pac
    else ok = assets::LoadCustomArena(files, 200, sel, m, err);
    Check(ok, "real.loads_created_arena", ok ? m.label : err);
    if (ok) {
      size_t scenery = 0, ring = 0, crowd = 0;
      std::set<int> slots;
      int lo_casd = 1 << 30, hi_casd = -1, lo_caen = 1 << 30, hi_caen = -1;
      for (const assets::Mesh& me : m.meshes) {
        if (me.src_kind == 1 && me.src_key < 400) {
          ++scenery, slots.insert(me.src_key);
          if (me.src_key < 200) lo_casd = std::min(lo_casd, me.src_key), hi_casd = std::max(hi_casd, me.src_key);
          else lo_caen = std::min(lo_caen, me.src_key), hi_caen = std::max(hi_caen, me.src_key);
        } else if (me.src_kind == 1 && me.src_key >= 900 && me.src_key < 1100) {
          ++ring;
        } else if (me.src_kind == 3) {
          ++crowd;
        }
      }
      std::printf("created arena CAEN %u CASD %u CARL %u: meshes %zu (scenery %zu in %zu slots: CASD slots %d-%d, CAEN slots %d-%d; ring-set %zu; crowd %zu), light_sets %zu, slot_records %zu, textures %zu, cycle groups %zu, warnings %zu\n", sel.caen, sel.casd, sel.carl,
                  m.meshes.size(), scenery, slots.size(), lo_casd, hi_casd, lo_caen, hi_caen, ring, crowd, m.light_sets.size(), m.slot_records.size(), m.textures.size(), m.cycle_slot_groups.size(), m.warnings.size());
      {   // the merge itself, once more, for its own census (bands, entries, the indices used)
        assets::pac::Pac mp;
        car::MergeInfo mi;
        std::string e3;
        const bool mok = car::BuildFromFiles(files, sel, mp, mi, e3);
        std::printf("merged bands: CASD slots %u (%u-%u), CAEN slots %u (%u-%u), CARG slots %u (%u-%u); entries %u, duplicates %u, lights from CARL %u / CASD %u, text joined %d; parts used CAEN %u CASD %u CARL %u (fallback %d/%d/%d)\n", mi.slots_casd, mi.slot_min_casd,
                    mi.slot_max_casd, mi.slots_caen, mi.slot_min_caen, mi.slot_max_caen, mi.slots_carg, mi.slot_min_carg, mi.slot_max_carg, mi.entries, mi.duplicates, mi.lights_from_carl, mi.lights_from_casd, int(mi.text_joined), mi.caen, mi.casd, mi.carl,
                    int(mi.caen_fallback), int(mi.casd_fallback), int(mi.carl_fallback));
        Check(mok && mi.slots_casd > 0 && mi.slot_max_casd < 200 && mi.slots_caen > 0 && mi.slot_min_caen >= 200 && mi.slot_max_caen <= 399 && mi.slots_carg > 0 && mi.slot_min_carg >= 25 && mi.slot_max_carg <= 94 && mi.lights_from_carl >= 1, "real.merged_bands",
              "CASD 0-199, CAEN 200-399, CARG 25-94, the light entries from CARL");
      }
      Check(m.meshes.size() > 20 && scenery > 0, "real.meshes", std::to_string(m.meshes.size()) + " meshes, " + std::to_string(scenery) + " scenery");
      Check(!m.light_sets.empty(), "real.light_sets", std::to_string(m.light_sets.size()) + " light sets");
      Check(hi_casd >= 0 && hi_casd < 200 && lo_caen >= 200 && hi_caen <= 399, "real.slot_bands", "CASD slots in 0-199, CAEN slots in 200-399");
      Check(m.custom_key == sel.Key() && m.arena == 200, "real.model_carries_selection", m.custom_key);
      // the readers that open the arena through OpenCached (crowd, blood, ring) see the venue with the selection published
      car::Publish(sel);
      std::string e2;
      const assets::pac::Pac* p = assets::load_detail::OpenCached(files, assets::load_detail::ArenaRel(200), e2);
      Check(p && assets::pac::Find(p->pachs[0], 20000) && assets::pac::Find(p->pachs[0], 32000) && assets::pac::Find(p->pachs[0], 952), "real.open_cached_serves_readers", p ? "crowd 20000, blood 32000, turnbuckle 952 present" : e2);
      assets::Model blood;
      const bool bl = assets::LoadBloodEntry(files, 200, 0, blood, e2);
      Check(bl, "real.blood_reader", bl ? "blood model " + std::to_string(blood.meshes.size()) + " mesh(es)" : e2);
      car::Clear();
      const assets::pac::Pac* q = assets::load_detail::OpenCached(files, assets::load_detail::ArenaRel(200), e2);
      Check(q == nullptr, "real.unpublished_is_old_path", "with no selection published pac\\bg\\bg200.pac is opened as before: " + e2);
      {   // the provider (blocker b): the slot table of the venue from the merged parts, then through the contributor's keyed publish
        std::vector<rdr_scene::live::arena::SlotSnap> sl;
        std::vector<rdr_scene::live::arena::ScreenSnap> sc;
        std::string e4;
        const bool pv = rdr_scene::live::arena::SlotsFromCustom(files, sel, sl, sc, e4);
        Check(pv && sl.size() > 100, "real.provider_slot_table", pv ? std::to_string(sl.size()) + " slots, " + std::to_string(sc.size()) + " screens from the merged parts" : e4);
        car::Publish(sel);
        {
          const car::ScopedThreadSel ts(sel);
          rdr_scene::live::arena::PublishSlots(200, rdr_scene::live::arena::Look::Match, std::move(sl), std::move(sc));
        }
        Check(!rdr_scene::live::arena::NeedsSlots(200), "real.provider_published_keyed", "the created arena's table is published for its selection");
        car::Clear();
        rdr_scene::live::arena::PublishAll(rdr_scene::live::arena::ArenaSnapshot());
        // the per-part texture bands: the merge's part map (LoadArena picks a texture set by it)
        car::Publish(sel);
        std::string e5;
        const assets::pac::Pac* pp = assets::load_detail::OpenCached(files, assets::load_detail::ArenaRel(200), e5);
        Check(pp && car::PartOfKey(0) == 0 && car::PartOfKey(200) == 1 && car::PartOfKey(25) == 2 && car::PartOfKey(952) == 2, "real.part_map_bands", "CASD slot 0 -> 0, CAEN slot 200 -> 1, CARG slot 25 / ring 952 -> 2");
        car::Publish([&] { car::Sel w = sel; w.arena = 201; return w; }());
        std::string e6;
        const assets::pac::Pac* wrong = assets::load_detail::OpenCached(files, assets::load_detail::ArenaRel(200), e6);
        Check(wrong == nullptr, "real.selection_of_another_arena_refused", "a selection for arena 201 is not taken for arena 200");
        car::Clear();
      }
      // another selection, and one past the part counts (the game's fallback to part 0)
      car::Sel s2 = sel;
      s2.caen = 5, s2.casd = 3, s2.carl = 2;
      assets::Model m2;
      const bool ok2 = assets::LoadCustomArena(files, 200, s2, m2, err);
      Check(ok2 && m2.custom_key != m.custom_key && !m2.meshes.empty(), "real.other_selection", "CAEN 5 / CASD 3 / CARL 2: " + std::to_string(m2.meshes.size()) + " meshes, " + std::to_string(m2.light_sets.size()) + " light sets");
      car::Sel s3 = sel;
      s3.caen = 900, s3.casd = 900, s3.carl = 900;
      assets::Model m3;
      const uint64_t fb0 = car::Stats().fallback_index.load();
      const bool ok3 = assets::LoadCustomArena(files, 200, s3, m3, err);
      Check(ok3 && car::Stats().fallback_index.load() >= fb0 + 3 && !m3.meshes.empty(), "real.missing_index_falls_back", "indices 900 fall back to part 0 (3 fallbacks counted)");
    }
  }
  std::printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
