// entry: beltcolour::OnBeltBlock   (the body of the raw hook on sub_83327660, the R5 composer's block), beltcolour::RowOf (the record writer), beltcolour::BindBeltMask (the drawer) and assets::BeltMaskCache::Get (the loader's named mask)
// S111 STOCKBELT + BELTMASKNAME: offline proof of the stock-belt recolour of a created title, all three R5 modes (docs\research\s111\STOCKBELT.md, docs\research\s111\BELTMASKNAME.md). 0 game runs; the guest bytes are
// fixtures made here from the listings' layout (nothing from the game).
//   stockbelt_unit.exe [--game DIR] [--plant NAME]
//   --game DIR : the player's game files (the folder that holds pac\cab\CAB.pac, pac\evt\evtobj.pac, pac\m2.pac, pac\bg\bgEtc.pac, pac\edit\edit.pac, pac\_startup.pac). Runs the REAL step: every CBPM / CBWM / CBLM
//                row's Mask1 loads (BGRA4, 1 level, 512 / 256 / 512 square), its belt model loads (evtobj prop entries 0..9 / m2 p0/1000/14/<sub> / bgEtc /STGL PACH 11 + row entry 1), texture key 0 is the `color` one
//                and EVERY key-0 material binds the EDIT/STS cc_mask row through the real ChooseEffect. Without --game the real step is SKIPPED.
//   --plant NAME : a planted twin of the old / wrong rule; exit 0 only when a check SEES it. NAME = no_latch (nothing latched: the tip before STOCKBELT) | entrance_only (modes 1 / 2 counted only: the STOCKBELT
//                tip) | no_owner_check (a block's model trusted without its owner's check) | no_drop (the slot free / member make / scene clear drop nothing) | kind_any (a latch gives a row to any call of its
//                model) | spmt_section (the mask looked up in the SPMT section, the Threads rule, instead of CBPM) | all_materials (the mask named on EVERY material, KeepPropMask's Threads rule, instead of the
//                key-0 materials R5 recolours) | slice_next_row (the ranged CAB.pac read decodes the directory row after the asked one) | hold_no_rows (a held structure frame drops the belt row: the ladder
//                belt drawn plain) | ask_all_modes (mode 1 asks its mask although no STS row can draw it) | no_pending_mark (a row waiting for its mask draws in the plain pool's name: never retired)
// Build: pwsh -NoProfile -File tools\scene\build_stockbelt_unit.ps1
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "../../src/renderer_scene/front_end.cpp"   // ChooseEffect for the real step (renderer symbols it never calls stay unresolved at link)
#include "../../src/assets/asset_load.h"
#include "../../src/assets/belt_mask.h"
#include "../../src/assets/change_mask.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/weapon_plan.h"
#include "../../src/scene_live/belt_plate.h"
#include "../../src/scene_live/belt_colour.h"
#include "../../src/scene_live/prop_colour.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace rdr_scene;
namespace pc = rdr_scene::live::propcolour;
namespace bc = rdr_scene::live::beltcolour;

static int g_fail = 0, g_checks = 0;
static std::string g_plant;
static void Check(bool ok, const char* name, const std::string& why = "") {
  ++g_checks;
  std::printf("%s  %-34s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}
static const char* MaskSection() { return g_plant == "spmt_section" ? "SPMT" : "CBPM"; }
// The drawer's bind of a named mask (rf_props.cpp -> BindBeltMask); the all_materials twin = the Threads rule (every material names it).
static int KeepMask(assets::Model& m, const assets::Texture& t) {
  if (g_plant != "all_materials") return bc::BindBeltMask(m, t);
  assets::Texture copy = t;
  change_mask::KeepPropMask(m, std::move(copy));
  return int(m.materials.size());
}

namespace {

// ---- the synthetic stages ------------------------------------------------------------------------------------------------------------------------------------------------------------------------
struct Guest {
  std::map<uint32_t, uint32_t> words;
  bool Rw(uint32_t a, uint32_t& v) const {
    const auto it = words.find(a);
    if (it == words.end()) return false;
    return v = it->second, true;
  }
};

constexpr uint32_t kBelt = 0xA0001000, kWeaponBelt = 0xA0002000, kChair = 0xA0002800, kLadderBelt = 0xA0003000, kRope = 0xA0003800, kBlock = 0xD0004000;
constexpr uint32_t kProp = 0xB0000000, kMgr = 0xB0100000, kHolder = 0xB0200000;
constexpr uint32_t kBeltId = 2004, kTitleRow = 7;
const uint8_t kColours[12] = {200, 10, 20, 30, 220, 40, 50, 60, 230, 70, 80, 240};

// The block sub_83327660 receives (sub_833275E8 + sub_833275F8 x 4): +0 model, +4 id, +8 mode, colour k = words +16 + 16 k / +20 / +24 (R, G, B), +28 the fourth word, +80 a global word (sub_825F0000).
void LayBlock(Guest& g, uint32_t model, uint32_t id, uint32_t mode) {
  g.words[kBlock] = model, g.words[kBlock + 4] = id, g.words[kBlock + 8] = mode, g.words[kBlock + 80] = 0x12345678;
  for (uint32_t k = 0; k < 4; ++k) {
    g.words[kBlock + 16 + 16 * k] = kColours[3 * k], g.words[kBlock + 20 + 16 * k] = kColours[3 * k + 1], g.words[kBlock + 24 + 16 * k] = kColours[3 * k + 2], g.words[kBlock + 28 + 16 * k] = 255;
  }
}
weapons::WeaponCall Call(uint32_t object, int32_t id, uint32_t flags) {
  weapons::WeaponCall c;
  c.object = object, c.id = id, c.slot_flags = flags;
  return c;
}

void Synthetic() {
  if (g_plant == "no_latch") bc::PlantForTest() = bc::Fault::kNoLatch;
  if (g_plant == "entrance_only") bc::PlantForTest() = bc::Fault::kEntranceOnly;
  if (g_plant == "no_owner_check") bc::PlantForTest() = bc::Fault::kNoOwnerCheck;
  if (g_plant == "no_drop") bc::PlantForTest() = bc::Fault::kNoDrop;
  if (g_plant == "kind_any") bc::PlantForTest() = bc::Fault::kKindAny;
  if (g_plant == "ask_all_modes") bc::PlantForTest() = bc::Fault::kAskAllModes;
  if (g_plant == "no_pending_mark") bc::PlantForTest() = bc::Fault::kNoPendingMark;
  Check(std::strcmp(bc::kMaskPac, assets::kCabPac) == 0, "mask_pac.same_path", "belt_colour.h kMaskPac == belt_mask.h kCabPac");
  Guest g;
  for (uint32_t m : {kBelt, kWeaponBelt, kChair, kLadderBelt, kRope}) g.words[m] = pc::kSkinnedModelVtable;
  // The weapon manager (sub_8258DC70's scan): slot 0 a chair (id 3), slot 1 the belt (id 14), the other 46 slots empty (main 0). The stage holder: member 0 the rope, member 1 the belt ([H+80]).
  for (uint32_t i = 0; i < 48; ++i) g.words[kMgr + weapons::kOffSlotMain + 4 * i] = 0, g.words[kMgr + weapons::kOffSlotId + 4 * i] = 0xFFFFFFFFu;
  g.words[kMgr + weapons::kOffSlotMain] = kChair, g.words[kMgr + weapons::kOffSlotId] = 3;
  g.words[kMgr + weapons::kOffSlotMain + 4] = kWeaponBelt, g.words[kMgr + weapons::kOffSlotId + 4] = 14;
  g.words[kHolder + weapons::kHolderOffModel] = kRope, g.words[kHolder + weapons::kHolderOffModel + 4] = kLadderBelt;
  // EventProps as the 0x8272E030 hook fills it: only the entrance belt is an event prop.
  auto find = [](uint32_t model, int32_t& id) { return model == kBelt ? (id = int32_t(kBeltId), true) : false; };
  auto rw = [&g](uint32_t a, uint32_t& v) { return g.Rw(a, v); };
  pc::TheStore().ResetForTest();
  bc::ResetForTest();
  bc::Latched v;

  LayBlock(g, kBelt, kBeltId, 0);   // mode 0: the entrance belt of a created title on stock prop 2004 (CAB prop object state 1; r3 = the object + 192)
  bc::OnBeltBlock(rw, find, kProp + 192, kBlock);
  Check(bc::TheStore().Get(kBelt, v) && v.mode == 0 && v.id == kBeltId && std::memcmp(v.colours, kColours, 12) == 0, "mode0.latched", "the entrance belt's (mode, id, colours)");
  pc::Out out;
  Check(!pc::TheStore().Get(kBelt, 1, out), "mode0.not_in_thb_store", "one store for the three modes (the THB store keeps the Threads gear only)");

  LayBlock(g, kWeaponBelt, kTitleRow, 1);   // mode 1: the match weapon belt (r3 = the manager + 3168)
  bc::OnBeltBlock(rw, find, kMgr + bc::kComposerInManager, kBlock);
  Check(bc::TheStore().Get(kWeaponBelt, v) && v.mode == 1 && v.id == kTitleRow, "mode1.latched", "the first id-14 slot's main model, mask /CBWM/7");

  LayBlock(g, kLadderBelt, kTitleRow, 2);   // mode 2: the ladder rig's belt (r3 = the holder + 192)
  bc::OnBeltBlock(rw, find, kHolder + bc::kComposerInHolder, kBlock);
  Check(bc::TheStore().Get(kLadderBelt, v) && v.mode == 2 && v.id == kTitleRow, "mode2.latched", "holder member 1, mask /CBLM/7");

  // A block whose model is not its owner's belt is refused, never trusted (mode 1: the chair slot; mode 2: member 0; mode 0: an id MakeWeapon did not register).
  LayBlock(g, kChair, kTitleRow, 1);
  bc::OnBeltBlock(rw, find, kMgr + bc::kComposerInManager, kBlock);
  LayBlock(g, kRope, kTitleRow, 2);
  bc::OnBeltBlock(rw, find, kHolder + bc::kComposerInHolder, kBlock);
  Check(!bc::TheStore().Get(kChair, v) && !bc::TheStore().Get(kRope, v) && bc::TheStore().Counts().refused[int(bc::Refusal::kNotOwnersBelt)] == 2, "owner.refused", "the chair (mode 1) and the rope (mode 2)");
  LayBlock(g, kBelt, kBeltId + 1, 0);
  bc::OnBeltBlock(rw, find, kProp + 192, kBlock);
  Check(bc::TheStore().Counts().refused[int(bc::Refusal::kIdMismatch)] == 1 && bc::TheStore().Get(kBelt, v) && v.id == kBeltId, "mode0.id_mismatch_refused", "the latched entry stays the good block's");
  std::vector<live::change_colour::Shortfall> sf;
  bc::TheStore().TakeShortfalls(sf);
  Check(sf.size() == 3 && std::string(sf[0].kind) == "belt_colour", "refusal.shortfalls", std::to_string(sf.size()) + " shortfalls queued (once each)");
  Guest empty;
  bc::OnBeltBlock([&empty](uint32_t a, uint32_t& w) { return empty.Rw(a, w); }, find, 0, kBlock);
  Check(bc::TheStore().Counts().refused[int(bc::Refusal::kUnreadable)] == 1, "block.unreadable_counted");

  // The record writer: each latch gives a row to the call kind its mode draws, and to no other call of that address.
  ChangeColourRow row;
  const bool r0 = bc::RowOf(Call(kBelt, int32_t(kBeltId), weapons::kCallEventProp), row);
  Check(r0 && row.flags == (kCcProp | kCcBeltMask) && row.reserved == bc::MaskName(0, kBeltId) && std::memcmp(row.colours, kColours, 12) == 0, "row.mode0_event_call", "kCcProp | kCcBeltMask, the name /CBPM/2004");
  const bool r1 = bc::RowOf(Call(kWeaponBelt, weapons::BeltId(kTitleRow), weapons::kCallVtableOk | 1u), row);
  Check(r1 && row.reserved == bc::MaskName(1, kTitleRow) && bc::MaskEntryOf(row.reserved) == "beltmask:1/7", "row.mode1_belt_call", bc::MaskEntryOf(row.reserved));
  const bool r2 = bc::RowOf(Call(kLadderBelt, weapons::StageId(11 + int32_t(kTitleRow)), weapons::PackStageFlags(1, true, false, 0)), row);
  Check(r2 && row.reserved == bc::MaskName(2, kTitleRow), "row.mode2_member1_call");
  const bool w1 = bc::RowOf(Call(kWeaponBelt, weapons::StageId(18), weapons::PackStageFlags(1, true, false, 0)), row);   // the weapon belt's address seen as a stage call
  const bool w2 = bc::RowOf(Call(kLadderBelt, weapons::StageId(18), weapons::PackStageFlags(0, true, false, 0)), row);   // the ladder belt's address as member 0
  const bool w3 = bc::RowOf(Call(kWeaponBelt, weapons::BeltId(kTitleRow), weapons::kCallVtableOk | weapons::kCallSecondary), row);   // a secondary model
  const bool w4 = bc::RowOf(Call(kBelt, int32_t(kBeltId), 0u), row);   // the entrance belt's address as an m2 call
  Check(!w1 && !w2 && !w3 && !w4, "row.kind_only", "no row for another call kind at a latched address");
  uint32_t pm = 9, pi = 9;
  Check(bc::ParseMaskEntry("beltmask:2/45", pm, pi) && pm == 2 && pi == 45 && !bc::ParseMaskEntry("beltmask:3/1", pm, pi) && !bc::ParseMaskEntry("beltmask:1/7x", pm, pi) &&
            std::string(bc::MaskSection(0)) == "CBPM" && std::string(bc::MaskSection(1)) == "CBWM" && std::string(bc::MaskSection(2)) == "CBLM",
        "name.entry_and_sections");

  // The row through the record file (record_io.cpp ParseRecord: kCcBeltMask is a known flag; the name rides `reserved`).
  {
    SceneTables t;
    t.header.source = uint8_t(Source::Live);
    t.header.guest_frame = 2;
    ChangeColourRow a, b;
    bc::RowOf(Call(kWeaponBelt, weapons::BeltId(kTitleRow), weapons::kCallVtableOk | 1u), a);
    bc::RowOf(Call(kLadderBelt, weapons::StageId(18), weapons::PackStageFlags(1, true, false, 0)), b);
    t.change_colours = {a, b};
    const std::vector<uint8_t> bytes = SerializeRecord(t);
    SceneTables back;
    std::string err;
    const bool ok = ParseRecord(bytes.data(), bytes.size(), back, err);
    Check(ok && back.change_colours.size() == 2 && back.change_colours[0].reserved == a.reserved && back.change_colours[1].flags == b.flags, "row.record_roundtrip", ok ? std::to_string(back.change_colours.size()) + " rows back" : err);
  }

  // The drawer's join: the belt row of the object, its mask by name.
  {
    std::vector<ChangeColourRow> rows(2);
    rows[0].object = kChair, rows[0].flags = kCcProp;   // a Threads gear row: no belt mask
    bc::RowOf(Call(kLadderBelt, weapons::StageId(18), weapons::PackStageFlags(1, true, false, 0)), rows[1]);
    Check(!bc::BeltRowOf(rows, kChair) && bc::BeltRowOf(rows, kLadderBelt) == &rows[1] && bc::PoolSuffix(rows[1].reserved) == "~cb2.7", "draw.belt_row_of");
  }

  // The lifetimes: the slot free drops mode 1 only, the member make mode 2 only, the scene clear mode 0 only, an event model's make / free its own address.
  bc::DropOnSlotFree();
  const bool after_slot = !bc::TheStore().Get(kWeaponBelt, v) && bc::TheStore().Get(kBelt, v) && bc::TheStore().Get(kLadderBelt, v);
  bc::DropOnMemberMake();
  const bool after_member = !bc::TheStore().Get(kLadderBelt, v) && bc::TheStore().Get(kBelt, v);
  bc::ClearOnScene();
  const bool after_scene = !bc::TheStore().Get(kBelt, v);
  Check(after_slot && after_member && after_scene, "life.by_mode", std::string("slot free ") + (after_slot ? "ok" : "WRONG") + ", member make " + (after_member ? "ok" : "WRONG") + ", scene clear " + (after_scene ? "ok" : "WRONG"));
  LayBlock(g, kWeaponBelt, kTitleRow, 1);
  bc::OnBeltBlock(rw, find, kMgr + bc::kComposerInManager, kBlock);
  bc::ClearOnScene();   // a weapon-manager pointer change (NoteScene -> EventProps::Clear) must not take the match weapon belt (latched at the weapon data load)
  const bool survives = bc::TheStore().Get(kWeaponBelt, v);
  bc::DropOnMake(kWeaponBelt);   // an event model made at that address: the old latch is not its
  Check(survives && !bc::TheStore().Get(kWeaponBelt, v), "life.mode1_survives_scene_clear", "the scene clear keeps it; a make at the address drops it");

  // F2 (review): a held structure frame (weapon_plan.h HoldStageRows: a record with no newly closed guest frame, Emit's `if (!any) return`) keeps the ladder belt's row with its held call.
  {
    weapons::Plants pl;
    weapons::StageFamilyLatch latch;
    pl.latch = &latch;
    pl.hold_no_rows = g_plant == "hold_no_rows";
    const int32_t pach = 11 + int32_t(kTitleRow);
    latch.fam[pach] = int32_t(weapons::kStageFamilyOwn);
    weapons::WeaponCall sc = Call(kLadderBelt, weapons::StageId(pach), weapons::PackStageFlags(1, true, false, 0));
    sc.frame = 40, sc.node_first = 0, sc.node_count = 1, sc.alpha = 1.f;
    float nodes[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    weapons::View v;
    v.calls = &sc, v.n_calls = 1, v.nodes = nodes, v.n_nodes = 1;
    std::vector<ChangeColourRow> rec(1), none, out0, out1;
    rec[0].object = kLadderBelt, rec[0].flags = kCcProp | kCcBeltMask, rec[0].reserved = bc::MaskName(2, kTitleRow);
    std::vector<weapons::WeaponCall> hc, hc2;
    std::vector<float> hn, hn2;
    weapons::Census c0, c1;
    weapons::HoldStageRows(v, pl, hc, hn, c0, &rec, &out0);   // the masked frame: the snapshot
    const weapons::View held = weapons::HoldStageRows(weapons::View(), pl, hc2, hn2, c1, &none, &out1);   // the next record carries no calls and no rows
    const bool call_held = c1.stage.held == 1 && held.n_calls == 1 && held.calls[0].object == kLadderBelt;
    const ChangeColourRow* hr = bc::BeltRowOf(out1, kLadderBelt);
    Check(out0.empty() && call_held && hr && hr->reserved == rec[0].reserved, "hold.keeps_belt_row",
          std::string(call_held ? "call held" : "call NOT held") + ", " + std::to_string(out1.size()) + " belt row(s) held with it (want 1: the masked pool of the frame it holds)");
  }

  // F5 (review): a mode-1 row asks no mask (no STS row can draw it: kModeDrawable); modes 0 / 2 ask theirs.
  {
    ChangeColourRow a0, a1, a2;
    a0.flags = a1.flags = a2.flags = kCcProp | kCcBeltMask;
    a0.reserved = bc::MaskName(0, kBeltId), a1.reserved = bc::MaskName(1, kTitleRow), a2.reserved = bc::MaskName(2, kTitleRow);
    Check(bc::AsksMask(&a0) && !bc::AsksMask(&a1) && bc::AsksMask(&a2) && !bc::AsksMask(nullptr), "ask.mode1_asks_no_mask", "the drawer asks /CBPM and /CBLM, never /CBWM while mode 1 is not drawable");
  }
  // F3 (review): an entrance belt drawn N frames while its mask is pending, then resident, leaves ONE live pool (the pending one is handed to the retire).
  {
    live::beltplate::PoolTrack track;
    const uint32_t name = bc::MaskName(0, kBeltId);
    const std::string base = "weapon:ev" + std::to_string(kBeltId) + "/0";
    for (int f = 0; f < 5; ++f) track.Note(kBelt, base + bc::PoolSuffix(name, false));
    track.Note(kBelt, base + bc::PoolSuffix(name, true));
    const std::vector<std::string> gone = track.TakeRetire();
    Check(gone.size() == 1 && gone[0] == base + bc::PoolSuffix(name, false) && gone[0] != base + bc::PoolSuffix(name, true) && track.Models() == 1, "pool.pending_retired_when_masked",
          std::to_string(gone.size()) + " pool(s) retired (want 1: the pending one; the masked one stays)");
  }

  const std::string line = bc::CensusLine();
  Check(line.find("[belt_colour] blocks: entrance ") == 0 && line.find("refused 4 (unreadable 1") != std::string::npos, "census.line", line);

  // The CAB.pac directory, as the player's file lays it out: EPAC, section CBPM (12-byte rows, the id, its sector), and a decoy SPMT section holding the same id (the Threads rule's section).
  assets::pac::Pac p;
  p.bytes.assign(0x6000, 0);
  std::memcpy(p.bytes.data(), "EPAC", 4);
  auto drow = [&p](size_t o, const char* name, uint32_t sv) { std::memcpy(p.bytes.data() + o, name, 4); p.bytes[o + 4] = uint8_t(sv), p.bytes[o + 5] = uint8_t(sv >> 8); };
  drow(0x800, "SPMT", 3), drow(0x80C, "2004", 2);   // section SPMT: one row, sector 2 (a different PACH)
  drow(0x818, "CBPM", 6), drow(0x824, "2001", 0), drow(0x830, "2004", 1);   // section CBPM: 2001 -> sector 0, 2004 -> sector 1
  p.pachs.resize(3);
  p.pachs[0].base = 0x4000, p.pachs[1].base = 0x4800, p.pachs[2].base = 0x5000;
  const std::set<uint32_t> ids = assets::SectionIds(p.bytes, MaskSection());
  Check(ids == std::set<uint32_t>{2001, 2004}, "cab.cbpm_ids", std::to_string(ids.size()) + " ids in the " + MaskSection() + " section (want 2001, 2004 of CBPM)");
  Check(assets::BeltMaskPach(p, 2004) == 1 && assets::BeltMaskPach(p, 2001) == 0 && assets::BeltMaskPach(p, 2718) == -1, "cab.cbpm_ordinal", "2004 -> PACH 1 (CBPM), not PACH 2 (SPMT)");

  // R5 recolours texture key 0 only. A belt model as the files hold it: a key-0 mesh, a plate mesh on another texture, a mesh whose diffuse did not resolve.
  {
    assets::Model bm;
    bm.textures.resize(2), bm.textures[0].name = "color", bm.textures[1].name = "nameplate";
    bm.materials.resize(3), bm.materials[0].diffuse = 0, bm.materials[1].diffuse = 1, bm.materials[2].diffuse = -1;
    assets::Texture mask;
    mask.name = "Mask1";
    const int named = KeepMask(bm, mask);
    Check(named == 1 && bm.textures.size() == 3 && bm.materials[0].change_mask[0] == 2 && bm.materials[1].change_mask[0] == -1 && bm.materials[2].change_mask[0] == -1, "keep.key0_only",
          "the mask names the key-0 material only (named " + std::to_string(named) + ")");
    assets::Model plate;   // no material samples texture 0
    plate.textures.resize(2), plate.textures[0].name = "color", plate.textures[1].name = "nameplate";
    plate.materials.resize(1), plate.materials[0].diffuse = 1;
    const int named2 = KeepMask(plate, mask);
    Check(named2 == 0 && plate.textures.size() == 2 && plate.materials[0].change_mask[0] == -1, "keep.no_key0_no_texture", "a model with no key-0 material keeps no mask texture");
  }
  // A CAB.pac that cannot be opened is said ONCE, then every ask is kNone (no line per belt).
  {
    launcher::GameFiles none;
    none.SetRoot("C:\\stockbelt_unit_no_such_folder");
    assets::BeltMaskCache cache;
    assets::Texture t;
    std::string why1, why2;
    const assets::BeltMaskCache::Got a = cache.Get(none, "CBPM", 2001, t, why1), b = cache.Get(none, "CBWM", 0, t, why2);
    Check(a == assets::BeltMaskCache::Got::kError && b == assets::BeltMaskCache::Got::kNone && !why1.empty(), "cab.unreadable_said_once", why1);
  }
}

// ---- the REAL step ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool BuildTables(launcher::GameFiles& files) {   // = live_assets.cpp read_rule::BuildEffectTable: the startup table, then edit.pac p9/0 (/EDIT/STS) (as thb_unit.cpp)
  using namespace rdr_scene::read_rule;
  const auto hash = [](const uint8_t* d, size_t n) { return uint64_t(XXH3_64bits(d, n)); };
  assets::pac::Pac p;
  std::string err;
  if (!assets::pac::Open(files, "pac\\_startup.pac", p, err)) return std::fprintf(stderr, "no pac\\_startup.pac: %s\n", err.c_str()), false;
  auto table = std::make_shared<EffectTable>();
  for (const assets::pac::Pach& pach : p.pachs)
    for (const assets::pac::Ent& e : pach.ents) {
      const assets::Bytes dec = assets::pac::Decode(p.bytes.data() + e.off, e.size);
      static const uint8_t kMagic[3] = {0x10, 0x2A, 0x11};
      if (dec.size() < 16 || std::search(dec.begin(), dec.end(), kMagic, kMagic + 3) == dec.end()) continue;
      EffectTable part;
      if (!ParseEffectLeaf(dec.data(), dec.size(), hash, part)) continue;
      for (auto& kv : part) table->emplace(kv.first, std::move(kv.second));
    }
  if (table->empty()) return std::fprintf(stderr, "no effect table parsed\n"), false;
  PublishEffectTable(std::move(table));
  assets::pac::Pac ep;
  if (!assets::pac::Open(files, "pac\\edit\\edit.pac", ep, err)) return std::fprintf(stderr, "no pac\\edit\\edit.pac: %s\n", err.c_str()), false;
  if (ep.pachs.size() <= 9 || ep.pachs[9].ents.empty()) return std::fprintf(stderr, "edit.pac has no p9/0\n"), false;
  const assets::pac::Ent& le = ep.pachs[9].ents[0];
  const assets::Bytes ldec = assets::pac::Decode(ep.bytes.data() + le.off, le.size);
  auto st = std::make_shared<EffectTable>();
  if (!ParseEffectLeaf(ldec.data(), ldec.size(), hash, *st)) return std::fprintf(stderr, "edit.pac p9/0: no effect table\n"), false;
  PublishStsEffectTable(std::move(st));
  return true;
}

MaterialWants WantsOf(const assets::Material& mat, bool skinned, bool cc) {   // = BindMaterial's wants for a prop material (front_end.cpp; as thb_unit.cpp)
  MaterialWants w;
  w.diffuse = mat.diffuse >= 0;
  w.skinned = skinned;
  w.normal = mat.normal >= 0 || mat.shader.find("BumpMap") != std::string::npos;
  w.specmap = mat.specular >= 0;
  w.spec = mat.spec_lev > 0.f && !mat.no_highlight;
  w.alpha_test = mat.alpha_test;
  w.alpha_mask = mat.alpha_mask;
  w.cube = g_reflections.load() && mat.cube >= 0;
  w.refmap = g_reflections.load() && mat.refmap >= 0;
  if (technique_pick::AppliesTo(read_rule::Lower(mat.shader)) && mat.technique < 4096u) w.technique = int(mat.technique);
  w.change_colour = cc;
  return w;
}

// One family's census (the masks of one CAB section and the belt models they recolour).
struct Family {
  size_t masks_ok = 0, models = 0, mats = 0, key0 = 0, key0_named_ok = 0, other_named = 0, key0_unnamed = 0, first_not_color = 0, cc_ok = 0;
  std::vector<std::string> bad, not_color;
};
// `recoloured` = this is the model R5 recolours (mode 0: entry 0, the CWeapon's W + 1228; mode 1 / 2: the one model): only its texture 0 must be the `color` one.
void CheckModel(const read_rule::EffectTable& sts, assets::Model& m, const assets::Texture& mask, const std::string& what, Family& f, std::set<std::string>& seen, bool recoloured = true) {
  ++f.models;
  const std::string n0 = m.textures.empty() ? std::string() : read_rule::Lower(m.textures[0].name);
  if (recoloured && n0 != "color" && n0 != "k_color") ++f.first_not_color, f.not_color.push_back(what + " texture 0 '" + n0 + "'");   // the loader's texture 0 = the name table's key 0 (`color` / `k_color`): the key-0 rule rests on it
  KeepMask(m, mask);
  for (size_t mi = 0; mi < m.materials.size(); ++mi) {
    const assets::Material& mat = m.materials[mi];
    ++f.mats;
    const bool on_key0 = mat.diffuse == 0;
    if (!on_key0) { f.other_named += change_colour_draw::PropHasMask(mat); continue; }
    ++f.key0;
    f.key0_unnamed += !change_colour_draw::PropHasMask(mat);
    const std::string tag = what + " material " + std::to_string(mi) + " " + mat.shader + " T" + std::to_string(mat.technique);
    const read_rule::PassShaders* srow = technique_pick::RowOf(sts, mat.shader, mat.technique, 0);
    if (!srow || !srow->ps_ok) { f.bad.push_back(tag + ": no EDIT/STS row"); continue; }
    const int variant = FragmentOfHash(srow->ps, mat.shader);
    if (variant < 0 || !std::strstr(kVariants[size_t(variant)].name, "cc_mask")) { f.bad.push_back(tag + ": the STS row's pixel variant is not a cc_mask one"); continue; }
    bool all = true;
    for (const bool skinned : {false, true}) {
      const FxChoice plain = ChooseEffect(mat.shader, WantsOf(mat, skinned, false));
      const FxChoice cc = ChooseEffect(mat.shader, WantsOf(mat, skinned, true));
      const bool ok = cc.Ok() && cc.cc_row && cc.tech_picked && !cc.tech_fallback;
      if (plain.Ok() && !ok) all = false, f.bad.push_back(tag + (skinned ? " skinned" : " rigid") + ": plain binds, the cc_mask row does not (" + (cc.Ok() ? std::string("fallback pick") : cc.why_not) + ")");
      if (seen.insert(mat.shader + " T" + std::to_string(mat.technique) + (skinned ? " skinned" : " rigid")).second && ok)
        std::printf("      %-14s T%-2u %-6s -> %s + %s\n", mat.shader.c_str(), unsigned(mat.technique), skinned ? "skin" : "rigid", kVariants[size_t(cc.vs)].name, kVariants[size_t(cc.ps)].name);
    }
    f.cc_ok += all;
  }
}
// BELTMASKNAME review: a belt whose key-0 material the EDIT/STS table cannot bind (mode 1's m2 belts) is drawn PLAIN = the pick the same material gets with no recolour row (the startup table's
// technique pick), never a sampler-scored stand-in: front_end Record asks BindMaterial(prop_cc_ask) for every carried belt row, so the cc ask with no STS row must land on the plain pick.
// Returns the key-0 binds whose cc ask differs from the plain pick (rigid and skinned), naming the first few in `bad`.
size_t PlainPickDiffers(const assets::Model& m, const std::string& what, std::vector<std::string>& bad) {
  size_t n = 0;
  for (size_t mi = 0; mi < m.materials.size(); ++mi) {
    const assets::Material& mat = m.materials[mi];
    if (mat.diffuse != 0) continue;
    for (const bool skinned : {false, true}) {
      const FxChoice plain = ChooseEffect(mat.shader, WantsOf(mat, skinned, false));
      const FxChoice cc = ChooseEffect(mat.shader, WantsOf(mat, skinned, true));
      if (!plain.Ok()) continue;
      const bool same = cc.Ok() && cc.vs == plain.vs && cc.ps == plain.ps && cc.tech_picked == plain.tech_picked && !cc.cc_row;
      if (same) continue;
      ++n;
      if (bad.size() < 6)
        bad.push_back(what + " material " + std::to_string(mi) + " " + mat.shader + " T" + std::to_string(mat.technique) + (skinned ? " skinned" : " rigid") + ": plain " + kVariants[size_t(plain.ps)].name +
                      (plain.tech_picked ? " (table pick)" : "") + ", cc ask " + (cc.Ok() ? std::string(kVariants[size_t(cc.ps)].name) + (cc.tech_fallback ? " (stand-in)" : "") : std::string("none")));
    }
  }
  return n;
}
void Report(const char* fam, const Family& f, size_t ids) {
  const std::string pre = std::string("real.") + fam;
  Check(f.masks_ok == ids && ids > 0, (pre + ".masks_load").c_str(), std::to_string(f.masks_ok) + " of " + std::to_string(ids) + " masks: Mask1 BGRA4, its size, 1 level");
  Check(f.bad.empty() && f.models > 0 && f.key0 > 0, (pre + ".every_belt_binds_cc_mask").c_str(),
        std::to_string(f.models) + " belt models, " + std::to_string(f.mats) + " materials (" + std::to_string(f.mats - f.key0) + " not on texture key 0: the game does not recolour them), " + std::to_string(f.cc_ok) + " key-0 materials bind the STS cc_mask row");
  for (size_t i = 0; i < f.bad.size() && i < 40; ++i) std::printf("      NOT BINDABLE: %s\n", f.bad[i].c_str());
  Check(f.other_named == 0 && f.key0_unnamed == 0, (pre + ".mask_only_on_key0").c_str(), std::to_string(f.other_named) + " materials off key 0 name the mask (want 0), " + std::to_string(f.key0_unnamed) + " key-0 materials do not (want 0)");
  std::string names;
  for (size_t i = 0; i < f.not_color.size() && i < 6; ++i) names += (i ? "; " : ": ") + f.not_color[i];
  Check(f.first_not_color == 0, (pre + ".key0_is_color").c_str(), std::to_string(f.first_not_color) + " recoloured belt models whose texture 0 is not `color` / `k_color`" + names);
}

int RealStep(const std::string& game) {
  launcher::GameFiles files;
  files.SetRoot(game);
  if (!BuildTables(files)) return Check(false, "real.tables", "the startup table or EDIT/STS (edit.pac p9/0) did not build"), 1;
  const auto sts = read_rule::StsEffectTableNow();
  if (!sts) return Check(false, "real.sts_table", "EDIT/STS is not published"), 1;
  assets::pac::Pac cab;
  std::string err;
  if (!assets::pac::Open(files, assets::kCabPac, cab, err)) return Check(false, "real.cab_open", err), 1;
  std::set<std::string> seen;
  const auto sized = [](const assets::Texture& t, uint32_t side) { return t.format == assets::TexFormat::kB4G4R4A4 && t.width == side && t.height == side && t.mips == 1 && t.data.size() == size_t(side) * side * 2u; };
  // Mode 0: /CBPM/<prop id> on every entry of the stock belt prop (evtobj.pac).
  const std::set<uint32_t> pids = assets::SectionIds(cab.bytes, MaskSection());
  Check(pids.size() == 140, "real.cbpm_census", std::to_string(pids.size()) + " CBPM ids in CAB.pac's directory (the note: 140)");
  Family f0;
  std::vector<uint32_t> no_prop;
  for (uint32_t id : pids) {
    assets::Texture mask;
    std::string lerr;
    if (!assets::LoadBeltMaskFrom(cab, id, mask, lerr, MaskSection()) || !sized(mask, 512)) { f0.bad.push_back("CBPM " + std::to_string(id) + ": " + (lerr.empty() ? "not BGRA4 512 x 512 x 1" : lerr)); continue; }
    ++f0.masks_ok;
    const int pach = assets::PropPachEvt(files, id);
    if (pach < 0) { no_prop.push_back(id); continue; }   // (7004 / 7009: CBPM rows with no evtobj.pac prop: no entrance belt to recolour)
    for (uint32_t k = 0; k <= 9; ++k) {
      assets::Model m;
      std::string merr;
      if (assets::LoadPropEntry(files, pach, k, m, merr)) CheckModel(*sts, m, mask, "CBPM " + std::to_string(id) + " entry " + std::to_string(k), f0, seen, k == 0);
    }
  }
  Report("cbpm", f0, pids.size());
  Check(no_prop == std::vector<uint32_t>{7004, 7009}, "real.cbpm_props", std::to_string(no_prop.size()) + " CBPM ids with no evtobj.pac prop (the note: 7004, 7009)");
  // Mode 1: /CBWM/<sub> on the m2 belt p0/1000/14/<sub> (the loader's "belt:<sub>").
  const std::set<uint32_t> wids = assets::SectionIds(cab.bytes, "CBWM");
  Check(wids.size() == 51, "real.cbwm_census", std::to_string(wids.size()) + " CBWM ids (the m2 belts: 51)");
  Family f1;
  for (uint32_t id : wids) {
    assets::Texture mask;
    std::string lerr;
    if (!assets::LoadBeltMaskFrom(cab, id, mask, lerr, "CBWM") || !sized(mask, 256)) { f1.bad.push_back("CBWM " + std::to_string(id) + ": " + (lerr.empty() ? "not BGRA4 256 x 256 x 1" : lerr)); continue; }
    ++f1.masks_ok;
    assets::Model m;
    std::string merr;
    if (!assets::LoadM2Weapon(files, int(weapons::kWeaponIdBelt), m, merr, 10, 0u, int(id))) { f1.bad.push_back("m2 belt " + std::to_string(id) + ": " + merr); continue; }
    CheckModel(*sts, m, mask, "m2 belt " + std::to_string(id), f1, seen);
  }
  {   // BELTMASKNAME review: mode 1 is latched and rowed, so the drawer asks the cc bind; with no STS row the draw must be the plain pick (as before the row existed), not a stand-in
    size_t differs = 0, binds = 0;
    std::vector<std::string> bad;
    for (uint32_t id : wids) {
      assets::Model m;
      std::string merr;
      if (!assets::LoadM2Weapon(files, int(weapons::kWeaponIdBelt), m, merr, 10, 0u, int(id))) continue;
      for (const assets::Material& mat : m.materials) binds += mat.diffuse == 0 ? 2u : 0u;
      differs += PlainPickDiffers(m, "m2 belt " + std::to_string(id), bad);
    }
    std::string names;
    for (size_t i = 0; i < bad.size(); ++i) names += (i ? "; " : ": ") + bad[i];
    Check(differs == 0 && binds > 0, "real.cbwm.no_row_draws_plain_pick", std::to_string(differs) + " of " + std::to_string(binds) + " key-0 binds take another pick when asked recoloured" + names);
  }
  // The m2 belts' one material is yProp_Default T0, and EDIT/STS has NO row for it (the first landing's real step, 51 of 51): our draw-time row cannot bind there, so a mode-1 belt is latched and
  // rowed but drawn plain, counted `no row` by the drawer ([prop_colour] census). Asserted as that known gap: when the table ever binds one, this check fails and says so.
  {
    size_t no_sts = 0;
    for (const std::string& b : f1.bad) no_sts += b.find(": no EDIT/STS row") != std::string::npos;
    Check(f1.masks_ok == wids.size() && f1.models == wids.size() && f1.cc_ok == 0 && no_sts == f1.key0 && f1.key0 > 0 && !bc::kModeDrawable[1], "real.cbwm.known_gap_no_sts_row",
          std::to_string(f1.masks_ok) + " masks, " + std::to_string(f1.models) + " m2 belts, " + std::to_string(f1.key0) + " key-0 materials, " + std::to_string(no_sts) + " with no EDIT/STS row, " + std::to_string(f1.cc_ok) + " bind; belt_colour.h kModeDrawable[1] " + (bc::kModeDrawable[1] ? "ON (the drawer would ask a mask it cannot draw)" : "off"));
    for (size_t i = 0; i < f1.not_color.size() && i < 6; ++i) std::printf("      INFO m2 %s\n", f1.not_color[i].c_str());
  }
  // Mode 2: /CBLM/<row> on the ladder rig's member 1 of bgEtc /STGL/<row> (PACH 11 + row, StagePathPach).
  const std::set<uint32_t> lids = assets::SectionIds(cab.bytes, "CBLM");
  Check(lids.size() == 46, "real.cblm_census", std::to_string(lids.size()) + " CBLM ids (the /STGL rows 0000..0045: 46)");
  Family f2;
  for (uint32_t id : lids) {
    assets::Texture mask;
    std::string lerr;
    if (!assets::LoadBeltMaskFrom(cab, id, mask, lerr, "CBLM") || !sized(mask, 512)) { f2.bad.push_back("CBLM " + std::to_string(id) + ": " + (lerr.empty() ? "not BGRA4 512 x 512 x 1" : lerr)); continue; }
    ++f2.masks_ok;
    char path[16];
    std::snprintf(path, sizeof path, "/STGL/%04u", id);
    const int32_t pach = weapons::StagePathPach(path, uint32_t(std::strlen(path)));
    assets::Model m;
    std::string merr;
    if (pach < 0 || !assets::LoadStageEntry(files, pach, 1, m, merr)) { f2.bad.push_back(std::string(path) + " (PACH " + std::to_string(pach) + ") entry 1: " + merr); continue; }
    CheckModel(*sts, m, mask, std::string(path) + " entry 1", f2, seen);
  }
  Report("cblm", f2, lids.size());
  // The loader reads CAB.pac BY BLOCKS (BeltMaskCache): every mask from its PACH slice equals the whole unpack's, byte for byte, in all three sections.
  {
    assets::BeltMaskCache ranged;
    if (g_plant == "slice_next_row") ranged.plant_row_shift = 1;
    size_t same = 0, all = 0;
    std::string first_diff;
    for (const char* sec : {"CBPM", "CBWM", "CBLM"})
      for (uint32_t id : assets::SectionIds(cab.bytes, sec)) {
        ++all;
        assets::Texture whole, part;
        std::string e1, e2;
        const bool w_ok = assets::LoadBeltMaskFrom(cab, id, whole, e1, sec);
        const bool r_ok = ranged.Get(files, sec, id, part, e2) == assets::BeltMaskCache::Got::kMask;
        if (w_ok && r_ok && whole.data == part.data && whole.width == part.width && whole.height == part.height && whole.format == part.format) ++same;
        else if (first_diff.empty()) first_diff = std::string(sec) + "/" + std::to_string(id) + (r_ok ? ": bytes differ" : ": " + e2);
      }
    Check(same == all && all > 0 && ranged.Ranged() && ranged.Slices() == int(all), "real.ranged_equals_whole",
          std::to_string(same) + " of " + std::to_string(all) + " masks equal (" + std::to_string(ranged.Slices()) + " slices, " + (ranged.Ranged() ? "XCTD blocks" : "plain ranges") + ")" + (first_diff.empty() ? "" : "; first: " + first_diff));
  }
  // The loader's own path (live_assets.cpp "beltmask:<mode>/<id>" -> MaskSection(mode)): a named mask loads, an id with no row (a Threads cap, an SPMT id) does not.
  {
    assets::BeltMaskCache cache;
    assets::Texture t0, t1, t2, tn;
    std::string why;
    const bool a = cache.Get(files, bc::MaskSection(0), 2001, t0, why) == assets::BeltMaskCache::Got::kMask && t0.width == 512;
    const bool b = cache.Get(files, bc::MaskSection(1), 0, t1, why) == assets::BeltMaskCache::Got::kMask && t1.width == 256;
    const bool c = cache.Get(files, bc::MaskSection(2), 0, t2, why) == assets::BeltMaskCache::Got::kMask && t2.width == 512;
    const bool d = cache.Get(files, bc::MaskSection(0), 2718, tn, why) == assets::BeltMaskCache::Got::kNone;
    Check(a && b && c && d && cache.Ids("CBPM") == 140 && cache.Ids("CBWM") == 51 && cache.Ids("CBLM") == 46, "real.cache_path", why);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::string game;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) game = argv[++i];
    else if (a == "--plant" && i + 1 < argc) g_plant = argv[++i];
    else return std::fprintf(stderr, "usage: stockbelt_unit [--game DIR] [--plant NAME]\n"), 2;
  }
  static const std::set<std::string> known = {"", "no_latch", "entrance_only", "no_owner_check", "no_drop", "kind_any", "spmt_section", "all_materials", "slice_next_row", "hold_no_rows", "ask_all_modes", "no_pending_mark"};
  if (!known.count(g_plant)) return std::fprintf(stderr, "unknown plant %s\n", g_plant.c_str()), 2;
  Synthetic();
  int real_rc = 0;
  if (!game.empty()) real_rc = RealStep(game);
  else std::printf("SKIP  real step (no --game DIR): the CAB census, the belt models and the cc_mask binds are not proven by this run\n");
  std::printf("%d of %d checks failed\n", g_fail, g_checks);
  if (!g_plant.empty()) return g_fail > 0 ? 0 : 1;
  return (g_fail == 0 && real_rc == 0) ? 0 : 1;
}
