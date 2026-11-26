// entry: RawHookEntry   (the Threads editor's read at the entrance prop's draw ask) and propcolour::OnRecolourBlock (the body of the raw hook on sub_833A1758)
// S108 lane THB: offline proof of the Superstar Threads entrance gear colours (docs\research\s108\THB_threads_gear.md). 0 game runs; the guest bytes are fixtures made here from the listings' layout (nothing from the game).
//   thb_unit.exe [--game DIR] [--plant NAME]
//   --game DIR : the player's game files (the folder that holds pac\evt\evtobj.pac, pac\edit\edit.pac, pac\_startup.pac). Runs the REAL step: every SPMT id's mask loads (Mask1, BGRA4 512 x 512) and EVERY
//                material of its prop (every entry) has a bindable cc_mask row in the EDIT/STS table through the real ChooseEffect. A prop with none = exit 1 with the ids (the note's fallback condition: stop, do not
//                build the host compose). Without --game the real step is reported SKIPPED (exit 0 on the synthetic stages only).
//   --plant NAME : a planted twin of the old / wrong rule; the unit exits 0 only when its checks SEE the plant (some check FAILS). NAME = ignore_state | any_callback | no_checks | make_no_drop | free_no_drop | no_carry |
//                zero_constants | no_hook | no_sts (needs --game)
// Production order driven: the game's draw of an event prop model (RawHookEntry: the editor read at the draw ask, both builds) / the recolour block (OnRecolourBlock = the hook body) -> the contributor's Emit
// (weapon_rows_contrib.h: the section-67 row of kCcProp) -> SerializeRecord / ParseRecord -> CarryChangeColour (the frame) -> the draw's entry (EntryOf) -> FeedPropColour -> Resolve ("ChangeColor" / "MatDifCol"),
// and the real ChooseEffect for the row (technique -> the STS table's cc_mask variant, slot 12 = the mask).
// Build: pwsh -NoProfile -File tools\scene\build_thb_unit.ps1
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "../../src/renderer_scene/front_end.cpp"   // the unit under test: ChooseEffect, Resolve, FeedPropColour (renderer symbols it never calls stay unresolved at link)
#include "../../src/assets/asset_load.h"
#include "../../src/assets/change_mask.h"
#include "../../src/renderer_scene/change_colour_draw.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/scene_live/prop_colour.h"
#include "../../src/scene_live/weapon_rows_contrib.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace rdr_scene;
namespace pc = rdr_scene::live::propcolour;
namespace wr = rdr_scene::live::weaponrows;
namespace W = rdr_scene::weapons;

static int g_fail = 0, g_checks = 0;
static std::string g_plant;
static void Check(bool ok, const char* name, const std::string& why = "") {
  ++g_checks;
  std::printf("%s  %-34s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}

// ---- the REAL step ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
namespace {

bool BuildTables(launcher::GameFiles& files) {   // = live_assets.cpp read_rule::BuildEffectTable: the startup table, then edit.pac p9/0 (/EDIT/STS)
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

// every id of the evtobj.pac directory's SPMT section (the walk of PropDirStartIn, collecting)
std::vector<uint32_t> SpmtIds(launcher::GameFiles& files) {
  std::vector<uint32_t> ids;
  std::string err;
  const assets::Bytes* hdr = nullptr;
  if (auto* r = assets::load_detail::OpenEvtRanged(files, err)) hdr = &r->header;
  else if (const assets::pac::Pac* p = assets::load_detail::OpenDlcProps(files, "pac\\evt\\evtobj.pac", err)) hdr = &p->bytes;
  if (!hdr || hdr->size() < 0x4000) return ids;
  std::string cur(reinterpret_cast<const char*>(hdr->data() + 0x800), 4);
  for (size_t o = 0x80C; o + 12 <= 0x4000; o += 12) {
    const uint8_t* r = hdr->data() + o;
    const uint32_t st = assets::Le32(*hdr, o + 4);
    const bool is_id = std::isdigit(r[0]) && std::isdigit(r[1]) && std::isdigit(r[2]) && std::isdigit(r[3]);
    if (!is_id) {
      if (r[0] == 0 && st == 0) break;
      if (std::memcmp(r, "LIST", 4) != 0) cur.assign(reinterpret_cast<const char*>(r), 4);
      continue;
    }
    if (cur == "SPMT") ids.push_back(uint32_t(std::atoi(std::string(reinterpret_cast<const char*>(r), 4).c_str())));
  }
  return ids;
}

MaterialWants WantsOf(const assets::Material& mat, bool skinned, bool cc) {   // = BindMaterial's wants for a prop material (front_end.cpp)
  MaterialWants w;
  w.diffuse = mat.diffuse >= 0;
  w.skinned = skinned;
  w.normal = mat.normal >= 0 || mat.shader.find("BumpMap") != std::string::npos;   // (the neutral normal stands in for a BumpMap material with none)
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

int RealStep(const std::string& game) {
  launcher::GameFiles files;
  files.SetRoot(game);
  if (!BuildTables(files)) return Check(false, "real.tables", "the startup table or EDIT/STS (edit.pac p9/0) did not build"), 1;
  if (g_plant == "no_sts") read_rule::PublishStsEffectTable(nullptr);   // the twin: no EDIT/STS table (the draw would find no cc_mask row)
  const auto sts = read_rule::StsEffectTableNow();
  if (!sts) return Check(false, "real.sts_table", "EDIT/STS (edit.pac p9/0) is not published: no prop has a cc_mask row"), 1;
  std::vector<uint32_t> ids = SpmtIds(files);
  Check(ids.size() == 26, "real.spmt_census", std::to_string(ids.size()) + " SPMT ids in the directory (the note: 26)");
  size_t masks_ok = 0, entries = 0, mats = 0, cc_ok = 0, plain_ok_cc_bad = 0, sts_row_missing = 0, not_cc_variant = 0;
  std::vector<std::string> bad;
  std::set<std::string> seen;
  std::string idlist;
  for (uint32_t id : ids) {
    idlist += (idlist.empty() ? "" : " ") + std::to_string(id);
    const int ord = assets::PropMaskPachEvt(files, id);
    assets::Texture mask;
    std::string err;
    if (ord < 0 || !assets::LoadPropMask(files, ord, mask, err)) { bad.push_back(std::to_string(id) + ": mask not loaded (" + (ord < 0 ? std::string("no SPMT PACH") : err) + ")"); continue; }
    if (mask.format != assets::TexFormat::kB4G4R4A4 || mask.width != 512 || mask.height != 512 || mask.mips != 1 || mask.data.size() != 512u * 512u * 2u) {
      bad.push_back(std::to_string(id) + ": Mask1 is not BGRA4 512 x 512 x 1 level (" + std::to_string(mask.width) + "x" + std::to_string(mask.height) + " fmt " + std::to_string(int(mask.format)) + ")");
      continue;
    }
    ++masks_ok;
    const int pach = assets::PropPachEvt(files, id);
    if (pach < 0) { bad.push_back(std::to_string(id) + ": the prop has no SOBJ / BOBJ PACH"); continue; }
    for (uint32_t k = 0; k <= 9; ++k) {
      assets::Model m;
      std::string lerr;
      if (!assets::LoadPropEntry(files, pach, k, m, lerr)) continue;   // (an entry the PACH lacks)
      ++entries;
      assets::Texture copy = mask;
      change_mask::KeepPropMask(m, std::move(copy));
      for (size_t mi = 0; mi < m.materials.size(); ++mi) {
        const assets::Material& mat = m.materials[mi];
        ++mats;
        const bool masked = mat.change_mask[0] >= 0 && size_t(mat.change_mask[0]) < m.textures.size() && m.textures[size_t(mat.change_mask[0])].name == "Mask1";
        const std::string tag = std::to_string(id) + " entry " + std::to_string(k) + " material " + std::to_string(mi) + " " + mat.shader + " T" + std::to_string(mat.technique);
        if (!masked) { bad.push_back(tag + ": change_mask[0] does not name the mask"); continue; }
        const read_rule::PassShaders* row = technique_pick::RowOf(*sts, mat.shader, mat.technique, 0);
        int variant = -1;
        if (!row || !row->ps_ok) { ++sts_row_missing; bad.push_back(tag + ": no EDIT/STS row"); continue; }
        variant = FragmentOfHash(row->ps, mat.shader);
        if (variant < 0 || !std::strstr(kVariants[size_t(variant)].name, "cc_mask")) { ++not_cc_variant; bad.push_back(tag + ": the STS row's pixel variant is not a cc_mask one"); continue; }
        bool all = true;
        for (const bool skinned : {false, true}) {
          const FxChoice plain = ChooseEffect(mat.shader, WantsOf(mat, skinned, false));
          const FxChoice cc = ChooseEffect(mat.shader, WantsOf(mat, skinned, true));
          const bool ok = cc.Ok() && cc.cc_row && cc.tech_picked && !cc.tech_fallback;
          if (plain.Ok() && !ok) { all = false, ++plain_ok_cc_bad, bad.push_back(tag + (skinned ? " skinned" : " rigid") + ": plain binds, the cc_mask row does not (" + (cc.Ok() ? std::string("fallback pick") : cc.why_not) + ")"); }
          if (seen.insert(mat.shader + " T" + std::to_string(mat.technique) + (skinned ? " skinned" : " rigid")).second && ok)
            std::printf("      %-14s T%-2u %-6s -> %s + %s\n", mat.shader.c_str(), unsigned(mat.technique), skinned ? "skin" : "rigid", kVariants[size_t(cc.vs)].name, kVariants[size_t(cc.ps)].name);
        }
        cc_ok += all;
      }
    }
  }
  Check(masks_ok == ids.size() && !ids.empty(), "real.masks_load", std::to_string(masks_ok) + " of " + std::to_string(ids.size()) + " ids: Mask1 BGRA4 512 x 512, 1 level (the 26 of the note: " + idlist + ")");
  Check(bad.empty(), "real.every_prop_binds_cc_mask", std::to_string(entries) + " prop entries, " + std::to_string(mats) + " materials: " + std::to_string(cc_ok) + " bind the STS cc_mask row; STS row missing " + std::to_string(sts_row_missing) + ", not a cc variant " + std::to_string(not_cc_variant) + ", plain binds but cc does not " + std::to_string(plain_ok_cc_bad));
  for (const std::string& b : bad) std::printf("      NOT BINDABLE: %s\n", b.c_str());
  if (!bad.empty()) std::printf("HAND BACK: the plan's fallback condition (THB note section 5): an SPMT id's yProp technique has no bindable cc_mask row. Do not build the host compose.\n");
  return 0;
}

}  // namespace

// ---- the synthetic stages ------------------------------------------------------------------------------------------------------------------------------------------------------------------------
namespace {

struct Guest {
  std::map<uint32_t, uint32_t> words;
  bool Rw(uint32_t a, uint32_t& v) const {
    const auto it = words.find(a);
    if (it == words.end()) return false;
    return v = it->second, true;
  }
  void PutFloat(uint32_t a, float f) { std::memcpy(&words[a], &f, 4); }
  bool ReadMats(uint32_t a, uint32_t n, std::vector<Mat4>& out) const {
    out.resize(n);
    for (uint32_t i = 0; i < n * 16; ++i) {
      const auto it = words.find(a + 4 * i);
      if (it == words.end()) return false;
      std::memcpy(&reinterpret_cast<float*>(out.data())[i], &it->second, 4);
    }
    return true;
  }
  void MakeModel(uint32_t obj, uint32_t nodes) {   // a skinned model: vtable word, slot 2 = sub_82BD3AB8, the count in [this+300], alpha 1
    words[obj] = 0x8206AF90, words[0x8206AF90 + 8] = 0x82BD3AB8;
    words[obj + 296] = 1, words[obj + 300] = nodes;
    PutFloat(obj + 252, 1.0f);
  }
  void Nodes(uint32_t buf, uint32_t n) {
    for (uint32_t i = 0; i < n * 16; ++i) PutFloat(buf + 4 * i, float(i));
  }
  void PutBytes(uint32_t a, const uint8_t* b, int n) {   // big-endian words, memory order
    for (int i = 0; i < n; i += 4) words[a + uint32_t(i)] = (uint32_t(b[i]) << 24) | (uint32_t(b[i + 1]) << 16) | (uint32_t(b[i + 2]) << 8) | uint32_t(b[i + 3]);
  }
};

constexpr uint32_t kCap = 0xA0001000, kBuf = 0xB0001000, kGear = 0xC0002000, kOther = 0xA0003000, kBlock = 0xD0004000;
constexpr int32_t kCapId = 2718;
const uint8_t kEdit[12] = {200, 10, 20, 30, 220, 40, 50, 60, 230, 70, 80, 240};     // the editor's G+128..+139: four RGB triples
const uint8_t kSaved[12] = {255, 0, 255, 0, 255, 0, 0, 0, 255, 255, 255, 0};         // a saved set's 4 gear colours (magenta, green, blue, yellow)

// The editor, as the listings lay it out: the prop's model carries the switch, the callback and the gear object; the gear object its state, the mask-loaded flag and the colours.
void LayEditor(Guest& g, uint32_t sw, uint32_t cb, uint32_t state, uint32_t mask_loaded, const uint8_t colours[12]) {
  g.words[kCap + pc::kOffSwitch] = sw, g.words[kCap + pc::kOffCallback] = cb, g.words[kCap + pc::kOffCallbackArg] = kGear;
  g.words[kGear + pc::kOffState] = state, g.words[kGear + pc::kOffMaskLoaded] = mask_loaded;
  g.PutBytes(kGear + pc::kOffColours, colours, 12);
}
// The recolour block sub_833A1758 receives (96 bytes): +0 the entry-0 model, +4 the prop id, colour i = words +16 + 16 i / +20 / +24 (R, G, B), +28 255.
void LayBlock(Guest& g, uint32_t model, uint32_t id, const uint8_t colours[12]) {
  g.words[kBlock] = model, g.words[kBlock + 4] = id;
  for (int i = 0; i < 4; ++i) {
    g.words[kBlock + 16 + 16 * uint32_t(i)] = colours[3 * i], g.words[kBlock + 20 + 16 * uint32_t(i)] = colours[3 * i + 1], g.words[kBlock + 24 + 16 * uint32_t(i)] = colours[3 * i + 2], g.words[kBlock + 28 + 16 * uint32_t(i)] = 255;
  }
}

struct Rig {
  Guest g;
  live::weaponrows::WeaponRowsContributor C;
  Rig() {
    pc::TheStore().ResetForTest();
    g.MakeModel(kCap, 2), g.MakeModel(kOther, 2), g.Nodes(kBuf, 2);
    C.tracker.EventMake(kCap, uint32_t(kCapId), 0);   // MakeWeapon's hook: the cap's entry-0 model
  }
  bool Has(uint32_t model, uint64_t now) {
    pc::Out o;
    return pc::TheStore().Get(model, now, o);
  }
  // The game draws the prop's model: the production raw hook's pure body (OURS = the stream-free build).
  wr::CallResult Draw(uint32_t model, uint64_t frame) {
    wr::Slot13Scope own(model);
    wr::CallResult out;
    wr::RawHookEntry(C.tracker, true, /*ours*/ true, model, kBuf, frame, [&](uint32_t a, uint32_t& v) { return g.Rw(a, v); }, [&](uint32_t a, uint32_t n, std::vector<Mat4>& o) { return g.ReadMats(a, n, o); }, &out, 1.0f, 0, nullptr, nullptr, 0, nullptr);
    return out;
  }
  void Recolour(uint32_t block) {   // the hook on sub_833A1758: the block -> the checks -> the latch
    if (g_plant == "no_hook") return;   // the old code: no hook, the block is never read
    pc::OnRecolourBlock([&](uint32_t a, uint32_t& v) { return g.Rw(a, v); },
                        [&](uint32_t m, int32_t& id) { wr::EventProps::Entry e; if (!C.tracker.events.Find(m, e)) return false; return id = e.id, true; }, block);
  }
  // The record writer, the file, the draw's join: returns the frame's change-colour entry of the prop (null = none) after the whole chain.
  SceneTables t, back;
  SceneFrame frame;
  const SceneFrame::ChangeColour* Chain(uint64_t draw_frame, bool* roundtrip_ok = nullptr) {
    t = SceneTables();
    t.header.source = uint8_t(Source::Live);
    t.header.guest_frame = draw_frame + 1;
    live::Counters cn;
    live::FrameInfo fi;
    fi.guest_frame = draw_frame + 1;
    C.Emit(fi, t, cn);   // closes the draw's frame: the weapon rows + the prop rows
    std::string err;
    const std::vector<uint8_t> bytes = SerializeRecord(t);
    back = SceneTables();
    const bool ok = ParseRecord(bytes.data(), bytes.size(), back, err);
    if (roundtrip_ok) *roundtrip_ok = ok && back.change_colours.size() == t.change_colours.size();
    frame = SceneFrame();
    Object o;
    frame.objects.push_back(Object()), frame.objects.push_back(o);   // (the prop is the second object of the frame: its frame index is 1)
    if (g_plant != "no_carry") change_colour_draw::CarryChangeColour(back.change_colours, frame, 1u, kCap);   // rf_props.cpp: the prop's row -> the frame
    return change_colour_draw::EntryOf(frame.change_colours, 1u);
  }
};

// What the draw feeds the shader for that entry: the real FeedPropColour, then the real Resolve of both constants.
struct Fed { float cc[16] = {}, dif[4] = {}; bool any = false; };
Fed Feed(const SceneFrame::ChangeColour* e) {
  Fed f;
  assets::Material mat;   // the file's MatDifCol of a cap (2.5, 2.5, 3.0, 1)
  mat.diffuse_color[0] = 2.5f, mat.diffuse_color[1] = 2.5f, mat.diffuse_color[2] = 3.0f, mat.diffuse_color[3] = 1.f;
  const assets::LightSet rig = assets::DefaultRig();
  static const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  static const float zero3[3] = {0, 0, 0}, xy[2] = {0, 0};
  Ctx ctx{ident, ident, zero3, 0.0, &mat, &rig, xy, 1.f, 1.f};
  if (e && change_colour_draw::IsPropEntry(e->flags) && g_plant != "zero_constants") FeedPropColour(ctx, e->constants, e->flags);
  Resolve("ChangeColor", 4, ctx, f.cc);
  Resolve("MatDifCol", 1, ctx, f.dif);
  f.any = e != nullptr;
  return f;
}
bool Equals(const Fed& f, const uint8_t want[12]) {
  float c[16];
  pc::ToConstants(want, c);
  return std::memcmp(f.cc, c, sizeof c) == 0;
}

void Synthetic() {
  pc::PlantForTest() = g_plant == "ignore_state" ? pc::Fault::kIgnoreState : g_plant == "any_callback" ? pc::Fault::kAnyCallback : g_plant == "cab_foreign" ? pc::Fault::kCabAsForeign : pc::Fault::kNone;
  pc::BlockPlantForTest() = g_plant == "no_checks" ? pc::BlockFault::kNoChecks : pc::BlockFault::kNone;
  pc::DropPlantForTest() = g_plant == "make_no_drop" ? pc::Plant::kMakeNoDrop : g_plant == "free_no_drop" ? pc::Plant::kFreeNoDrop : pc::Plant::kNone;
  static_assert(pc::kSkinnedModelVtable == W::kSkinnedModelVtable, "the store's model vtable is the weapon rows' (0x8206AF90)");
  static_assert(sizeof(ChangeColourRow) == 64 && kCcProp == 2 && kCcEditor == 4, "section 67's row and the prop flags");

  // 1. THE EDITOR: the draw ask of the cap's model reads the 4 colours; the record carries them; the draw feeds them and MatDifCol 1.
  {
    Rig r;
    LayEditor(r.g, 1, pc::kCallbackEditorProp, pc::kStateShowing, 1, kEdit);
    const wr::CallResult c = r.Draw(kCap, 100);
    Check(c.event && c.recorded, "editor.draw_is_an_event_call", "the cap model's draw is an event prop call (id " + std::to_string(c.id) + ")");
    bool rt = false;
    const SceneFrame::ChangeColour* e = r.Chain(100, &rt);
    Check(rt, "editor.record_roundtrip", "section 67 survives the file");
    const Fed f = Feed(e);
    Check(e && (e->flags & kCcProp) && (e->flags & kCcEditor) && Equals(f, kEdit), "editor.colours_reach_the_draw", "the frame's entry carries kCcProp | kCcEditor; ChangeColor = G+128..139 / 255");
    Check(f.dif[0] == 1.f && f.dif[1] == 1.f && f.dif[2] == 1.f && f.dif[3] == 1.f, "editor.matdifcol_is_one", "MatDifCol (1, 1, 1, 1) as the editor's callback forces, over the file's (2.5, 2.5, 3.0)");
    // a colour edit moves the next frame's constants (the picker changes G+128..)
    uint8_t edit2[12];
    for (int i = 0; i < 12; ++i) edit2[i] = uint8_t(255 - kEdit[i]);
    LayEditor(r.g, 1, pc::kCallbackEditorProp, pc::kStateShowing, 1, edit2);
    r.Draw(kCap, 101);
    const Fed f2 = Feed(r.Chain(101));
    Check(Equals(f2, edit2) && !Equals(f2, kEdit), "editor.next_edit_moves_it", "a changed colour is in the very next frame's constants");
    const std::string cen = pc::TheStore().CensusLine();
    Check(cen.find("[prop_colour] latched 0, editor 2") == 0 && cen.find("rows 2") != std::string::npos && cen.find("shortfall 0") != std::string::npos, "editor.census", cen.substr(0, 150));
  }
  // 2. the editor's state is not 2 (the gear object is not showing the prop): the callback is stale: the prop draws plain, counted (twin: ignore_state)
  {
    Rig r;
    LayEditor(r.g, 1, pc::kCallbackEditorProp, 1, 1, kEdit);
    r.Draw(kCap, 100);
    const SceneFrame::ChangeColour* e = r.Chain(100);
    Check(e == nullptr && pc::TheStore().Counts().not_ready == 1, "editor.state_not_2_is_plain", "gear state 1: no row, counted not_ready");
    LayEditor(r.g, 1, pc::kCallbackEditorProp, pc::kStateShowing, 0, kEdit);   // the mask is not loaded yet ([G+100] == 0): the callback would bind none
    r.Draw(kCap, 101);
    Check(r.Chain(101) == nullptr && pc::TheStore().Counts().not_ready == 2, "editor.mask_not_loaded_is_plain", "mask flag 0: no row");
  }
  // 3. a switch on with ANOTHER callback = a counted shortfall, never recoloured as the editor's (twin: any_callback)
  {
    Rig r;
    LayEditor(r.g, 1, 0x8212345Cu, pc::kStateShowing, 1, kEdit);
    r.Draw(kCap, 100);
    std::vector<live::change_colour::Shortfall> sf;
    pc::TheStore().TakeShortfalls(sf);
    Check(r.Chain(100) == nullptr && sf.size() == 1 && std::string(sf[0].kind) == "prop_colour" && pc::TheStore().Counts().foreign == 1, "editor.foreign_callback_shortfall", sf.empty() ? "no shortfall" : sf[0].object);
    // the switch off (+604 == 0): the game draws its +384 row: no row, nothing counted as a shortfall
    LayEditor(r.g, 0, 0, 0, 0, kEdit);
    r.Draw(kCap, 101);
    sf.clear();
    pc::TheStore().TakeShortfalls(sf);
    Check(r.Chain(101) == nullptr && sf.empty(), "editor.switch_off_is_quiet", "model+604 == 0: no row, no shortfall");
  }
  // 3b. S111 CACBUILD: the Create a Championship belt callback (0x833445D0). A CREATED belt id at [C+228]: its sampler-12 mask is C's zero 1x1 texture, so the game recolours nothing: no row,
  //     counted cab_plain, NO shortfall (twin: cab_foreign, the old rule). A STOCK id there binds its /CBPM mask, which ours does not load: still the foreign shortfall.
  {
    Rig r;
    LayEditor(r.g, 1, pc::kCallbackCabBelt, 0, 0, kEdit);
    r.g.words[kGear + pc::kOffCabPropId] = 2926;
    r.Draw(kCap, 100);
    std::vector<live::change_colour::Shortfall> sf;
    pc::TheStore().TakeShortfalls(sf);
    Check(r.Chain(100) == nullptr && sf.empty() && pc::TheStore().Counts().cab_plain == 1 && pc::TheStore().Counts().foreign == 0, "editor.cab_created_belt_is_plain", sf.empty() ? "no shortfall" : sf[0].object);
    r.g.words[kGear + pc::kOffCabPropId] = 2004;
    r.Draw(kCap, 101);
    sf.clear();
    pc::TheStore().TakeShortfalls(sf);
    Check(r.Chain(101) == nullptr && sf.size() == 1 && pc::TheStore().Counts().foreign == 1, "editor.cab_stock_belt_shortfall", sf.empty() ? "no shortfall" : sf[0].object);
  }
  // 4. THE MATCH: the block of sub_833A1758 latched; the prop's draws carry the saved set's colours with the FILE's MatDifCol.
  {
    Rig r;
    LayBlock(r.g, kCap, uint32_t(kCapId), kSaved);
    r.Recolour(kBlock);
    r.Draw(kCap, 200);   // an ordinary draw of the cap model (no switch, no callback)
    bool rt = false;
    const SceneFrame::ChangeColour* e = r.Chain(200, &rt);
    const Fed f = Feed(e);
    Check(rt && e && (e->flags & kCcProp) && !(e->flags & kCcEditor) && Equals(f, kSaved), "match.colours_reach_the_draw", "a saved set's 4 colours: kCcProp, no kCcEditor, ChangeColor = the block's bytes / 255");
    Check(f.dif[0] == 2.5f && f.dif[2] == 3.0f, "match.matdifcol_is_the_files", "MatDifCol stays the file's (2.5, 2.5, 3.0): only the editor's callback forces 1");
    r.Draw(kCap, 201);
    Check(r.Chain(201) != nullptr, "match.colours_stay_for_the_model", "the next frame's draws carry them too (the latch lives with the model)");
    // the CWeapon's destructor hook (EventProps::OnFree): the model's colours go (its address may be reused)
    constexpr uint32_t kWeapon = 0xE0005000;
    r.g.words[kWeapon] = W::kEventWeaponVtable, r.g.words[kWeapon + W::kOffEventModels] = kCap;
    r.C.tracker.events.OnFree(kWeapon, [&](uint32_t a, uint32_t& v) { return r.g.Rw(a, v); });
    Check(!r.Has(kCap, 202), "match.free_drops_the_colours", "after the free hook the address has no colours (the store, not the row: the freed model is no event prop any more)");
  }
  // 5. the make hook: a model made at an address that held a latched prop starts clean (twin: make_no_drop)
  {
    Rig r;
    LayBlock(r.g, kCap, uint32_t(kCapId), kSaved);
    r.Recolour(kBlock);
    r.C.tracker.EventMake(kCap, 2719, 0);   // MakeWeapon again at the same address (another prop)
    Check(!r.Has(kCap, 300), "match.make_drops_the_old_colours", "a model re-made at the address has none of the old prop's colours");
    r.Draw(kCap, 300);
    Check(r.Chain(300) == nullptr, "match.remade_model_draws_stock", "its draw carries no row");
  }
  // 6. a block that fails a check is counted and ignored (twin: no_checks)
  {
    Rig r;
    LayBlock(r.g, kOther, uint32_t(kCapId), kSaved);   // the block names a model that is NOT an event prop (a wrestler's skeleton, a belt: any other address)
    r.Recolour(kBlock);
    r.Draw(kCap, 400);
    Check(r.Chain(400) == nullptr && !r.Has(kOther, 400) && pc::TheStore().Counts().refused[int(pc::Refusal::kNotEventProp)] == 1, "match.block_wrong_model_ignored", "a block naming a model the event list does not know: refused (not an event prop), nothing latched");
    Rig r2;
    LayBlock(r2.g, kCap, uint32_t(kCapId + 1), kSaved);   // the right model, another prop id
    r2.Recolour(kBlock);
    Check(!r2.Has(kCap, 400) && pc::TheStore().Counts().refused[int(pc::Refusal::kIdMismatch)] == 1, "match.block_wrong_id_ignored", "the block's id differs from the id MakeWeapon registered: refused");
    uint8_t bad[12];
    std::memcpy(bad, kSaved, 12);
    Rig r3;
    LayBlock(r3.g, kCap, uint32_t(kCapId), bad);
    r3.g.words[kBlock + 20] = 256;   // a colour word above 255
    r3.Recolour(kBlock);
    Check(!r3.Has(kCap, 400) && pc::TheStore().Counts().refused[int(pc::Refusal::kColourRange)] == 1, "match.block_colour_range_ignored", "a colour word of 256: refused");
    Rig r4;
    LayBlock(r4.g, kCap, uint32_t(kCapId), kSaved);
    r4.g.words[kCap] = 0x82020202;   // the model's vtable word is not a CSkinModel's
    r4.Recolour(kBlock);
    Check(!r4.Has(kCap, 400) && pc::TheStore().Counts().refused[int(pc::Refusal::kNotModel)] == 1, "match.block_not_a_model_ignored", "the model word's vtable is not 0x8206AF90: refused");
    std::vector<live::change_colour::Shortfall> sf;
    pc::TheStore().TakeShortfalls(sf);
    Check(sf.size() == 1, "match.refusal_is_said", "the refused block is queued for draw_shortfall once");
  }
  // 7. no hook, no set: the cap is stock (no row at all)
  {
    Rig r;
    r.Draw(kCap, 500);
    Check(r.Chain(500) == nullptr && !Feed(nullptr).any && Feed(nullptr).cc[0] == 0.f, "stock.no_row_no_colours", "a cap drawn with no set and no editor: no row, ChangeColor zero (the file's colours)");
  }
  // 8. a colour set on one model is not on the others
  {
    Rig r;
    LayBlock(r.g, kCap, uint32_t(kCapId), kSaved);
    r.Recolour(kBlock);
    Check(!r.Has(kOther, 600) && r.Has(kCap, 600), "match.colours_are_per_model", "the latch joins the draw by the model address only");
  }
  // 9. the record's flag rules (old readers / bad rows)
  {
    SceneTables t;
    t.header.source = uint8_t(Source::Live);
    ChangeColourRow body, prop, bad;
    body.object = 0x1000;
    prop.object = 0x2000, prop.flags = kCcProp | kCcEditor;
    bad.object = 0x3000, bad.flags = kCcEditor;   // an editor flag without the prop flag is no row of ours
    t.change_colours = {body, prop};
    std::string err;
    std::vector<uint8_t> bytes = SerializeRecord(t);
    SceneTables back;
    const bool ok = ParseRecord(bytes.data(), bytes.size(), back, err);
    Check(ok && back.change_colours.size() == 2 && back.change_colours[1].flags == (kCcProp | kCcEditor), "record.prop_row_roundtrip", "a body row and a prop row share section 67");
    t.change_colours = {body, bad};
    bytes = SerializeRecord(t);
    SceneTables back2;
    ParseRecord(bytes.data(), bytes.size(), back2, err);
    Check(back2.change_colours.empty(), "record.bad_flags_drop_the_section", "kCcEditor without kCcProp: the section drops itself (every row draws plain)");
  }
  // 10. the draw: SPMT mask at slot 12, the STS row, MatDifCol: pure rules without files
  {
    assets::Model m;
    assets::Material mat;
    mat.shader = "yProp_BumpMap";
    m.materials.push_back(mat);
    assets::Texture t;
    t.name = "Mask1", t.format = assets::TexFormat::kB4G4R4A4, t.width = t.height = 512, t.mips = 1, t.data.assign(512 * 512 * 2, 0);
    m.textures.push_back(assets::Texture());   // the file's diffuse at index 0
    change_mask::KeepPropMask(m, std::move(t));
    Check(m.materials[0].change_mask[0] == 1 && change_colour_draw::PropHasMask(m.materials[0]) && m.materials[0].change_mask[1] == -1 && !change_colour_draw::PropHasMask(assets::Material()), "draw.mask_is_the_models_texture", "KeepPropMask: texture 1 = Mask1, change_mask[0] names it, the others stay -1; a material with none has no mask");
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::string game;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) game = argv[++i];
    else if (a == "--plant" && i + 1 < argc) g_plant = argv[++i];
    else return std::fprintf(stderr, "usage: thb_unit [--game DIR] [--plant NAME]\n"), 2;
  }
  static const std::set<std::string> known = {"", "ignore_state", "any_callback", "no_checks", "make_no_drop", "free_no_drop", "no_carry", "zero_constants", "no_hook", "no_sts", "cab_foreign"};
  if (!known.count(g_plant)) return std::fprintf(stderr, "unknown plant %s\n", g_plant.c_str()), 2;
  Synthetic();
  int real_rc = 0;
  if (!game.empty()) real_rc = RealStep(game);
  else std::printf("SKIP  real step (no --game DIR): the SPMT census and the cc_mask binds are not proven by this run\n");
  std::printf("%d of %d checks failed\n", g_fail, g_checks);
  if (!g_plant.empty()) return g_fail > 0 ? 0 : 1;   // a planted twin: exit 0 only when the checks SEE it
  return (g_fail == 0 && real_rc == 0) ? 0 : 1;
}
