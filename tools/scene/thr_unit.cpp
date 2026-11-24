// entry: WrestlerContributor::NoteDrawAsked   (the raw hook on sub_82C04DF0 calls it at the game's draw of a slot body; the change-colour read follows the stamp)
// S108 lane THR: offline proof of the Superstar Threads editor's live colours, read where the game hands them to the GPU (src\scene_live\change_colour.h,
// docs\research\s108\THR_plan.md). 0 game runs; every byte is a fixture made here from the listings' layout (nothing from the game):
//   model+604 = the switch (sub_825A0E00), model+324 = 0x8339F0E0 (the callback, sub_8339F4C0), model+328 = E, E+20 = the editor state (10 = editing), E+92 = 16 RGB triples.
//   thr_unit.exe selftest
// Production order: the update hook makes the body known (SubmitSkin) -> the latch publishes the slot skeletons (LatchTargets) -> the game's draw asks for the body
// (NoteDrawAsked: the stamp, then the change-colour read) -> the record side reads the per-body store (Bodies().Get) -> the row (rows_change_colour.h) -> the
// constants the shader takes (ToConstants) and the mask slots (MaskIndexOfSlot).
// Planted twins (must FAIL on the rule they plant): old_zero (the pre-THR rule: no read, ChangeColor zero), no_switch (recolour without model+604),
// state_not_10 (a non-editing state read as fresh), foreign_callback (another editor's callback recoloured as Threads).
// S108 THR2: the hand-off stage (Handoff) drives the record, the carry, the loader's masks and the draw's pick / binds / constants; twin no_wiring = the draw side as it was before THR2.
// Build: pwsh -NoProfile -File tools\scene\build_thr_unit.ps1
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/scene_live/change_colour.h"
#include "../../src/scene_live/wrestler_contrib.h"
#include "../../src/renderer_scene/rows_change_colour.h"
#include "../../src/assets/change_mask.h"                 // S108 THR2: the `/m` tag, the masks as model textures
#include "../../src/renderer_scene/change_colour_draw.h"  // S108 THR2: the carry, the ask, the mask slots
#include "../../src/renderer_scene/char_effects.h"        // S108 THR2: ChooseCharEffectTable / RoleOfChangeSlot / ResolveChar
#include "../../src/renderer_scene/read_rule.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer/shaders/spv/fx_registry.h"
#include <memory>

using namespace rdr_scene;
using namespace rdr_scene::live;
namespace cc = rdr_scene::live::change_colour;

static int g_fail = 0;
static std::string g_plant;   // a planted twin: old_zero / no_switch / state_not_10 / foreign_callback (the selftest must FAIL under each)
static void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}

// Stand-ins for the writer's registry, which the contributor does not need (as arm2_unit.cpp).
namespace rdr_scene::live {
void Register(Contributor*) {}
bool Enabled() { return true; }
void EndFrame(uint64_t) {}
void AddSink(Sink*) {}
void RemoveSink(Sink*) {}
}  // namespace rdr_scene::live

namespace {

constexpr uint32_t kSkel = 0x40A00000u;   // the body model [char+48] (= the skeleton key)
constexpr uint32_t kEditor = 0x41200000u; // E = [0x839BFC84]
constexpr uint32_t kCount = 4;

struct Guest {
  std::map<uint32_t, uint32_t> words;
  bool readable = true;
  void Put(uint32_t a, uint32_t v) { words[a] = v; }
  void PutBytes(uint32_t a, const uint8_t* b, int n) {   // big-endian words, memory order
    for (int i = 0; i < n; i += 4) Put(a + uint32_t(i), (uint32_t(b[i]) << 24) | (uint32_t(b[i + 1]) << 16) | (uint32_t(b[i + 2]) << 8) | uint32_t(b[i + 3]));
  }
};
Guest g;
bool Rw(uint32_t a, uint32_t& v) {
  auto it = g.words.find(a);
  if (!g.readable || it == g.words.end()) return false;
  v = it->second;
  return true;
}

void Colours(uint8_t out[48], uint8_t seed) {
  for (int i = 0; i < 48; ++i) out[i] = uint8_t(seed + 5 * i);
}

// The game's editor, as the listings lay it out: the callback installed, the switch on, state 10, these colours.
void LayEditor(uint32_t sw, uint32_t cb, uint32_t state, const uint8_t colours[48]) {
  g.words.clear(), g.readable = true;
  g.Put(kSkel + cc::kOffSwitch, sw);
  g.Put(kSkel + cc::kOffCallback, cb);
  g.Put(kSkel + cc::kOffCallbackArg, kEditor);
  g.Put(kEditor + cc::kOffState, state);
  g.PutBytes(kEditor + cc::kOffColours, colours, 48);
}

struct Rig {
  WrestlerContributor w;
  explicit Rig() {
    w.SetNoWalk(true);
    cc::Bodies().ResetForTest();
  }
  // One frame in the production order. `old_rule` = the pre-THR planted twin: the draw ask is stamped and nothing reads the colours.
  void Frame(uint64_t hook) {
    const bool old_rule = g_plant == "old_zero";
    std::vector<Mat4> d(kCount);
    for (uint32_t i = 0; i < kCount; ++i)
      for (uint32_t j = 0; j < 16; ++j) d[i].m[j] = float(hook) + float(i) * 0.01f + float(j) * 0.0001f;
    w.SubmitSkin(kSkel, 0, kCount, d.data(), hook);
    WrestlerContributor::SlotTable s;
    s.skels.push_back({kSkel, 0, 0u, true});
    std::vector<WrestlerContributor::PaletteTarget> t;
    uint32_t missing = 0;
    w.LatchTargets(hook, &s, t, missing);
    w.NoteDrawAsked(kSkel, hook, Rw);
    if (old_rule) cc::Bodies().ResetForTest();   // the old code: the draw ask leaves no change-colour state (ChangeColor stays zero)
  }
};

// The record side as it runs: the store's body -> the row -> the 16 constants.
bool RowOf(uint64_t now, ChangeColourRow& row) {
  cc::Body b;
  if (!cc::Bodies().Get(kSkel, now, b)) return false;
  row = ChangeColourRow{};
  row.object = kSkel;
  row.flags = b.unsettled ? kCcUnsettled : 0u;
  std::memcpy(row.colours, b.colours, 48);
  return true;
}
bool ConstantsMatch(const ChangeColourRow& row, const uint8_t want[48]) {
  float k[64];
  cc::ToConstants(row.colours, k);
  for (int i = 0; i < 16; ++i)
    for (int c = 0; c < 3; ++c)
      if (std::fabs(k[4 * i + c] - float(want[3 * i + c]) / 255.f) > 1e-6f) return false;
  for (int i = 0; i < 16; ++i)
    if (k[4 * i + 3] != 1.f) return false;
  return true;
}

// ---- S108 THR2: the hand-off stage (production order, the real code at every step) ------------------------------------------------------------------------------
//   the game's draw (NoteDrawAsked: the read) -> Collect / EmitItems (section 67 + the `/m` entry) -> SerializeRecord / ParseRecord -> rf_wrestlers' carry (CarryChangeColour) ->
//   the loader's masks (change_mask::KeepMasks on a `/m` load) -> the draw's ask (WantsChangeColour) -> the STS row (ChooseCharEffectTable change_colour) -> the mask binds
//   (RoleOfChangeSlot + MaskTextureOfSlot) -> the constants (ResolveChar ChangeColor). Planted twin no_wiring = the draw side as it was (startup row, RoleOfSlot, no constants).
constexpr uint32_t kKey = 2401, kDesc = 0x50000u;

std::vector<uint8_t> Dds16(uint32_t w, uint32_t h, const uint32_t cm[4], const std::vector<uint16_t>& px) {   // a 16-bit uncompressed DDS made here (no game bytes)
  std::vector<uint8_t> d(128 + px.size() * 2, 0);
  const auto put = [&](size_t o, uint32_t v) { std::memcpy(d.data() + o, &v, 4); };
  std::memcpy(d.data(), "DDS ", 4);
  put(4, 124), put(8, 0x1007u), put(12, h), put(16, w), put(84, 0), put(88, 16);
  for (int c = 0; c < 4; ++c) put(92 + 4 * size_t(c), cm[c]);
  for (size_t i = 0; i < px.size(); ++i) d[128 + 2 * i] = uint8_t(px[i]), d[129 + 2 * i] = uint8_t(px[i] >> 8);
  return d;
}

using rdr::fx::kVariants;
constexpr int kNv = int(sizeof(rdr::fx::kVariants) / sizeof(rdr::fx::kVariants[0]));
uint64_t HashOf(int v) { for (const auto& h : rdr::fx::kHashes) if (int(h.variant) == v) return uint64_t(h.hash); return 0; }
bool Named(int v, const char* prefix, const char* suffix, const char* not_prefix = nullptr) {
  const char* n = kVariants[v].name;
  if (!n) return false;
  const size_t len = std::strlen(n), sl = std::strlen(suffix);
  return std::strncmp(n, prefix, std::strlen(prefix)) == 0 && (!not_prefix || std::strncmp(n, not_prefix, std::strlen(not_prefix)) != 0) && len > sl && !std::strcmp(n + len - sl, suffix);
}
bool SamplesAllMasks(int f) {
  int got = 0;
  for (uint32_t s = 0; s < kVariants[f].sampler_count; ++s) got |= cc::MaskIndexOfSlot(kVariants[f].samplers[s].slot) >= 0 ? 1 << cc::MaskIndexOfSlot(kVariants[f].samplers[s].slot) : 0;
  return got == 15;
}
std::shared_ptr<const read_rule::EffectTable> GearTable(uint64_t vs, uint64_t ps) {
  auto t = std::make_shared<read_rule::EffectTable>();
  read_rule::EffectTech et;
  et.name = "yCh_gear";
  et.techs.assign(1, std::vector<read_rule::PassShaders>(12, read_rule::PassShaders{vs, ps, true, true}));
  t->emplace("ych_gear", std::move(et));
  return t;
}

void Handoff() {
  const bool wired = g_plant != "no_wiring";
  uint8_t col[48];
  Colours(col, 33);
  // 0. The tables the loader publishes (fixtures from our own registry, no game file): startup = a plain yCh_gear row, STS = a change-colour yCh_gear row whose fragment samples
  //    all four mask slots. A change-colour fragment whose hash another effect's variant shares is preferred (the pick must still return the gear's OWN variant).
  int fv = -1, fcc = -1, fplain = -1, fpass = 0;
  bool skinned = false, shared = false;
  for (int want_shared = 1; want_shared >= 0 && fv < 0; --want_shared)
    for (int pass : {0, 2})
      for (int sk = 0; sk < 2 && fv < 0; ++sk)
        for (int v = 0; v < kNv && fv < 0; ++v) {
          if (!Named(v, "yCh_gear_", "_vertex", "yCh_gear_flex_") || !HashOf(v)) continue;
          int c = -1, p = -1;
          for (int f = 0; f < kNv && (c < 0 || p < 0); ++f) {
            if (!Named(f, "yCh_gear_", "_fragment", "yCh_gear_flex_") || !HashOf(f)) continue;
            const bool is_cc = std::strstr(kVariants[f].name, "change_color") != nullptr;
            if (is_cc && (c >= 0 || !SamplesAllMasks(f))) continue;
            if (is_cc && want_shared) {
              int owners = 0;
              for (const auto& h : rdr::fx::kHashes) owners += h.hash == HashOf(f) && h.variant != uint32_t(f);
              if (!owners) continue;
            }
            if (!is_cc && p >= 0) continue;
            read_rule::PublishEffectTable(GearTable(HashOf(v), HashOf(f)));
            read_rule::PublishStsEffectTable(nullptr);
            const CharChoice ch = ChooseCharEffectTable("yCh_gear", sk != 0, false, 0, pass);
            if (!ch.Ok() || ch.ps != f) continue;
            (is_cc ? c : p) = f;
          }
          if (c >= 0 && p >= 0) fv = v, fcc = c, fplain = p, fpass = pass, skinned = sk != 0, shared = want_shared != 0;
        }
  Check(fv >= 0, "handoff 0: the registry has a bindable yCh_gear vertex + a change-colour fragment sampling 7 / 4 / 14 / 2 + a plain fragment");
  if (fv < 0) return;
  std::printf("  fixture: %s + %s (STS) / %s (startup)%s\n", kVariants[fv].name, kVariants[fcc].name, kVariants[fplain].name, shared ? "; the STS fragment's hash is shared with another effect's variant" : "");
  read_rule::PublishEffectTable(GearTable(HashOf(fv), HashOf(fplain)));
  read_rule::PublishStsEffectTable(GearTable(HashOf(fv), HashOf(fcc)));

  // 1. The game's editor and a linked slot body; the game's draw asks for the body (the read).
  WrestlerContributor w;
  w.SetNoWalk(true);
  cc::Bodies().ResetForTest();
  rdr_scene::change_colour_draw::Draws().carried = 0;
  const uint64_t hook = 3;
  LayEditor(1, cc::kCallbackThreads, cc::kStateEditing, col);
  w.SubmitModelLink(kSkel, kKey, 0, 0, kDesc, {}, 0, std::string(), kKey);
  std::vector<Mat4> pose(kCount, IdentityMat4());
  w.SubmitSkin(kSkel, 0, kCount, pose.data(), hook);
  WrestlerContributor::SlotTable s;
  s.skels.push_back({kSkel, 0, 0u, true});
  std::vector<WrestlerContributor::PaletteTarget> targets;
  uint32_t missing = 0;
  w.LatchTargets(hook, &s, targets, missing);
  w.VerifyBodyLink(kSkel, true);
  const uint64_t record_frame = hook + WrestlerContributor::kPaletteLead;
  w.NoteDrawAsked(kSkel, record_frame - 1, Rw);
  if (g_plant == "old_zero") cc::Bodies().ResetForTest();   // the pre-THR read: nothing stored at the draw
  // 2. The record (Collect / EmitItems), written and read back.
  SceneTables t;
  {
    Counters c;
    FrameInfo f;
    f.guest_frame = record_frame;
    w.Emit(f, t, c);
  }
  size_t obj = t.objects.size();
  for (size_t i = 0; i < t.objects.size() && i < t.engine_objects.size(); ++i)
    if (t.engine_objects[i] == kSkel && (t.objects[i].flags & kObjVisible)) obj = i;
  Check(obj < t.objects.size(), "handoff 2: the body is a visible object of the record");
  if (obj >= t.objects.size()) return;
  const std::string entry(t.Str(t.assets[t.objects[obj].asset].entry));
  int key = 0;
  Check(change_mask::HasTag(entry) && std::sscanf(entry.c_str(), "model_key:%d", &key) == 1 && key == int(kKey), ("handoff 2: the body is named with the /m tag (" + entry + ")").c_str());
  Check(t.change_colours.size() == 1 && t.change_colours[0].object == kSkel && std::memcmp(t.change_colours[0].colours, col, 48) == 0 && t.change_colours[0].flags == 0, "handoff 2: section 67 carries the body's 48 colour bytes");
  SceneTables t2;
  {
    const std::vector<uint8_t> bytes = SerializeRecord(t);
    std::string err;
    Check(ParseRecord(bytes.data(), bytes.size(), t2, err) && t2.change_colours.size() == t.change_colours.size(), ("handoff 2: section 67 survives the file round trip " + err).c_str());
  }
  // 3. rf_wrestlers: the body's frame entry (its index in the frame's objects = 0 here), joined by the engine object.
  struct Entry { uint32_t object = 0, flags = 0; float constants[64] = {}; };
  struct Frame { std::vector<Entry> change_colours; } fr;
  const uint32_t engine = obj < t2.engine_objects.size() ? t2.engine_objects[obj] : 0u;
  Check(rdr_scene::change_colour_draw::CarryChangeColour(t2.change_colours, fr, 0u, engine), "handoff 3: the frame carries the body's colours (by engine object)");
  // 4. The loader's `/m` step: entries 13..16 -> the model's textures, change_mask on every material. Mask 1 in the common A4R4G4B4 layout (kept as stored), mask 2 in another layout (decoded).
  assets::Model mdl;
  mdl.materials.resize(2);
  mdl.materials[0].shader = "yCh_gear", mdl.materials[1].shader = "yCh_skin";
  mdl.textures.resize(3);   // the model's own pictures come first
  const uint32_t std_cm[4] = {0x0F00u, 0x00F0u, 0x000Fu, 0xF000u}, odd_cm[4] = {0xF000u, 0x000Fu, 0x00F0u, 0x0F00u};
  std::vector<std::vector<uint8_t>> blobs = {Dds16(2, 1, std_cm, {0x0F00, 0x00F0}), Dds16(2, 1, odd_cm, {0xF000, 0x0003}), Dds16(2, 1, std_cm, {0, 0x000F}), Dds16(2, 1, std_cm, {0xF000, 0})};
  std::string why;
  const int kept = change_mask::KeepMasks(mdl, [&](int k, size_t& n) -> const uint8_t* { n = blobs[size_t(k)].size(); return blobs[size_t(k)].data(); }, why);
  Check(kept == 4 && why.empty() && mdl.materials[1].change_mask[3] == 6 && mdl.materials[0].change_mask[0] == 3, "handoff 4: the four masks are model textures 3..6, named on every material");
  Check(mdl.textures[3].format == assets::TexFormat::kB4G4R4A4 && mdl.textures[4].format == assets::TexFormat::kR8G8B8A8 && mdl.textures[4].data[0] == 255 && mdl.textures[4].data[4 + 1] == 51,
        "handoff 4: a standard mask is kept as stored, another layout decodes by its header (R 15/15 -> 255, G 3/15 -> 51)");
  // 5. The draw: the ask, the row, the binds, the constants.
  const assets::Material& mat = mdl.materials[0];
  const float* k = rdr_scene::change_colour_draw::ConstantsOf(fr.change_colours, 0u);
  const bool want = rdr_scene::change_colour_draw::WantsChangeColour(k, mat);
  Check(want, "handoff 5: the body's draw asks for the change-colour row (colours this frame, masks on the material)");
  const CharChoice ch = ChooseCharEffectTable(mat.shader, skinned, false, 0, fpass, false, wired && want);
  const char* owner_ok = kVariants[ch.ps < 0 ? 0 : ch.ps].name;
  Check(ch.Ok() && ch.change_colour_table && ch.ps == fcc && !ch.wet && owner_ok && Named(ch.ps, "yCh_gear_", "_fragment", "yCh_gear_flex_"), "handoff 5: the STS row: the gear's OWN change-colour variant, slot 14 not the liquid map");
  int bound = 0;
  for (uint32_t si = 0; ch.Ok() && si < kVariants[ch.ps].sampler_count; ++si) {
    const int slot = kVariants[ch.ps].samplers[si].slot, mk = cc::MaskIndexOfSlot(slot);
    if (mk < 0) continue;
    const CharSlot role = wired ? RoleOfChangeSlot(slot) : RoleOfSlot(slot);
    bound += role == CharSlot(int(CharSlot::ChangeMask1) + mk) && rdr_scene::change_colour_draw::MaskTextureOfSlot(mat, slot) == 3 + mk;
  }
  Check(bound == 4, "handoff 5: Mask1..Mask4 bound at slots 7 / 4 / 14 / 2 (the model's textures 3..6)");
  assets::LightSet ls;
  CharCtx x;
  x.mat = &mat, x.lights = &ls;
  x.change_color = wired && ch.change_colour_table ? k : nullptr;
  float out[64] = {};
  Check(ResolveChar("ChangeColor", 16, x, out), "handoff 5: ResolveChar knows ChangeColor");
  bool same = true;
  for (int i = 0; i < 16; ++i) {
    for (int c = 0; c < 3; ++c) same = same && std::fabs(out[4 * i + c] - float(col[3 * i + c]) / 255.f) < 1e-6f;
    same = same && out[4 * i + 3] == 1.f;
  }
  Check(same, "handoff 5: the draw's ChangeColor = the editor's 48 bytes / 255, w = 1");
  // The plain body is untouched: no colours this frame = no ask, the startup row, ChangeColor zero.
  const bool plain_ask = rdr_scene::change_colour_draw::WantsChangeColour(nullptr, mat);
  const CharChoice pl = ChooseCharEffectTable(mat.shader, skinned, false, 0, fpass, false, plain_ask);
  CharCtx xp;
  xp.mat = &mat, xp.lights = &ls;
  float zero[64] = {};
  ResolveChar("ChangeColor", 16, xp, zero);
  bool all0 = true;
  for (float z : zero) all0 = all0 && z == 0.f;
  Check(!plain_ask && pl.Ok() && pl.ps == fplain && !pl.change_colour_table && all0, "handoff 5: a body with no row keeps the startup row and ChangeColor zero");
  // A body whose model has no masks (the PACH has none: the game clears [E+80]) is drawn plain and counted.
  assets::Material nomask;
  const uint64_t nm0 = rdr_scene::change_colour_draw::Draws().no_masks.load();
  Check(!rdr_scene::change_colour_draw::WantsChangeColour(k, nomask) && rdr_scene::change_colour_draw::Draws().no_masks.load() == nm0 + 1, "handoff 5: no masks = no change colour, counted");
  read_rule::PublishEffectTable(nullptr), read_rule::PublishStsEffectTable(nullptr);
}

// S110 LOGOSAMP: the logo-sampler rule (change_colour.h TakesLogoSampler) against BindMaterial's role switch (front_end.cpp:1402: change-colour -> RoleOfChangeSlot, wet ->
// RoleOfWetSlot, else RoleOfSlot): it must hold exactly where a character bind's role is Logo7 / Logo4. Returns the mismatching (character, change-colour, wet, slot) cases.
// `plant` "logo_any_bind" = the planted twin (every character bind's 7 / 4, no change-colour / wet exclusion): must be caught.
int LogoSamplerMismatches(const std::string& plant) {
  int bad = 0;
  for (int chr = 0; chr < 2; ++chr)
    for (int ccb = 0; ccb < 2; ++ccb)
      for (int wet = 0; wet < 2; ++wet)
        for (int s = 0; s < 16; ++s) {
          const CharSlot role = ccb ? RoleOfChangeSlot(s) : wet ? RoleOfWetSlot(s) : RoleOfSlot(s);
          const bool want = chr && (role == CharSlot::Logo7 || role == CharSlot::Logo4);
          const bool got = plant == "logo_any_bind" ? (chr != 0 && (s == 7 || s == 4)) : cc::TakesLogoSampler(chr != 0, ccb != 0, wet != 0, s);
          bad += got != want ? 1 : 0;
        }
  return bad;
}

// S110 LOGOSAMP (review): the prop-mask sampler rule (change_colour.h TakesPropMaskSampler) against the game's cases, written out: only the Threads editor's callback sub_8338EC00 sets stage 12
// (clamp, trilinear); a saved set's match draw has no callback (the game paints texture 0); a prop's other stages and a draw that is not a recoloured prop keep their sampler.
// `plant` "prop_any_row" = the planted twin (every recoloured prop draw's slot 12, the editor test dropped): must be caught.
int PropMaskSamplerMismatches(const std::string& plant) {
  struct Case { bool draw, editor; int slot; bool want; };
  static const Case kCases[] = {{true, true, 12, true}, {true, false, 12, false}, {false, true, 12, false}, {false, false, 12, false}, {true, true, 4, false}, {true, true, 0, false}, {true, true, 7, false}};
  int bad = 0;
  for (const Case& c : kCases) {
    const bool got = plant == "prop_any_row" ? (c.draw && c.slot == 12) : cc::TakesPropMaskSampler(c.draw, c.editor, c.slot);
    bad += got != c.want ? 1 : 0;
  }
  return bad;
}

int Selftest() {
  uint8_t a[48], b[48];
  Colours(a, 10), Colours(b, 77);

  {   // 1. The editing state: the colours the callback would send reach the row and the constants.
    Rig r;
    LayEditor(1, cc::kCallbackThreads, cc::kStateEditing, a);
    r.Frame(100);
    ChangeColourRow row;
    Check(RowOf(100, row) && std::memcmp(row.colours, a, 48) == 0 && row.flags == 0, "editing: the 48 bytes at E+92 are the body's row");
    Check(ConstantsMatch(row, a), "editing: ChangeColor[i] = (E+92+3i .. +94+3i) / 255, w = 1");
    // The player picks another colour: the next draw reads it (no rebuild, no latch).
    LayEditor(1, cc::kCallbackThreads, cc::kStateEditing, b);
    r.Frame(101);
    Check(RowOf(101, row) && std::memcmp(row.colours, b, 48) == 0, "a colour change shows at the next draw");
  }
  {   // 2. The switch off (model+604 == 0): the game draws the +384 row: no recolour, and the twin no_switch would recolour.
    Rig r;
    LayEditor(0, cc::kCallbackThreads, cc::kStateEditing, a);
    r.Frame(100);
    ChangeColourRow row;
    Check(!RowOf(100, row), "switch off: no change colour");
  }
  {   // 3. Not editing (state != 10): the callback sends nothing; we hold the last state-10 colours and say so (unsettled).
    Rig r;
    LayEditor(1, cc::kCallbackThreads, cc::kStateEditing, a);
    r.Frame(100);
    LayEditor(1, cc::kCallbackThreads, 11, b);   // E+92 changed while not editing: the game sends none of it
    r.Frame(101);
    ChangeColourRow row;
    Check(RowOf(101, row) && std::memcmp(row.colours, a, 48) == 0 && (row.flags & kCcUnsettled), "state != 10: the last editing colours, flagged unsettled");
    Check(cc::Bodies().Counts().unsettled == 1, "state != 10: counted unsettled");
    // Never edited yet: an unsettled first draw has no colours: plain.
    Rig r2;
    LayEditor(1, cc::kCallbackThreads, 3, a);
    r2.Frame(200);
    Check(!RowOf(200, row), "state != 10 before any editing read: plain row");
  }
  {   // 4. Another callback (an editor we do not know): a shortfall, never recoloured as Threads; queued once for the shared report (the render side drains it).
    Rig r;
    LayEditor(1, 0x82123450u, cc::kStateEditing, a);
    r.Frame(100);
    ChangeColourRow row;
    Check(!RowOf(100, row) && cc::Bodies().Counts().foreign == 1, "foreign callback: counted shortfall, no change colour");
    r.Frame(101);
    std::vector<cc::Shortfall> sf;
    cc::Bodies().TakeShortfalls(sf);
    Check(sf.size() == 1 && std::string(sf[0].kind) == "change_colour" && sf[0].object.find("foreign callback") != std::string::npos, "foreign callback: ONE draw_shortfall queued for two draws (once per model and reason)");
    // S108 THR2: the CAW editor's logo callback (sub_822E6CC8, A5c's draw) with the switch on: not ours to recolour, and not a shortfall (no false alarm in Create a Superstar).
    Rig r2;
    LayEditor(1, cc::kCallbackCawLogo, cc::kStateEditing, a);
    r2.Frame(100);
    sf.clear();
    cc::Bodies().TakeShortfalls(sf);
    Check(!RowOf(100, row) && cc::Bodies().Counts().caw_logo == 1 && cc::Bodies().Counts().foreign == 0 && sf.empty(), "CAW logo callback: counted caw_logo, no row, no shortfall");
  }
  {   // 5. Unreadable guest memory: a shortfall, no row.
    Rig r;
    LayEditor(1, cc::kCallbackThreads, cc::kStateEditing, a);
    g.words.erase(kEditor + cc::kOffColours + 8);
    r.Frame(100);
    ChangeColourRow row;
    Check(!RowOf(100, row) && cc::Bodies().Counts().unreadable == 1, "an unreadable colour word: counted, no row");
  }
  {   // 6. Lifetime: E is re-read at each draw (a moved editor object is followed); a destroyed model's entry goes at once; an undrawn body ages out.
    Rig r;
    LayEditor(1, cc::kCallbackThreads, cc::kStateEditing, a);
    r.Frame(100);
    const uint32_t e2 = kEditor + 0x1000;   // the game made a new editor object and re-installed the callback argument
    g.Put(kSkel + cc::kOffCallbackArg, e2), g.Put(e2 + cc::kOffState, cc::kStateEditing), g.PutBytes(e2 + cc::kOffColours, b, 48);
    r.Frame(101);
    ChangeColourRow row;
    Check(RowOf(101, row) && std::memcmp(row.colours, b, 48) == 0, "E is re-read from [model+328] at every draw");
    cc::Bodies().ForgetModel(kSkel);
    Check(!RowOf(101, row) && cc::Bodies().Counts().forgotten == 1, "ForgetModel: a destroyed model's colours go at once");
    r.Frame(102);
    Check(RowOf(102, row), "the body drawn again: a fresh read");
    Check(!RowOf(102 + cc::Store::kAgeFrames + 1, row) && cc::Bodies().Counts().aged == 1, "an undrawn body ages out");
  }
  {   // 7. The mask slots as the callback binds them (Mask1..Mask4 -> samplers 7, 4, 14, 2).
    Check(cc::MaskIndexOfSlot(7) == 0 && cc::MaskIndexOfSlot(4) == 1 && cc::MaskIndexOfSlot(14) == 2 && cc::MaskIndexOfSlot(2) == 3 && cc::MaskIndexOfSlot(0) == -1, "mask slots 7 / 4 / 14 / 2");
    // S110 ARMBAND (review): the mask sampler reaches a wrestler change-colour bind's mask stages only; an entrance prop's cc_mask bind keeps its normal map (slot 4) as it was
    Check(cc::TakesMaskSampler(true, true, 7) && cc::TakesMaskSampler(true, true, 4) && cc::TakesMaskSampler(true, true, 14) && cc::TakesMaskSampler(true, true, 2) && !cc::TakesMaskSampler(true, true, 0)
          && !cc::TakesMaskSampler(true, false, 4) && !cc::TakesMaskSampler(false, true, 4) && !cc::TakesMaskSampler(false, true, 12), "mask sampler: a wrestler change-colour bind's 7 / 4 / 14 / 2 only (not a prop's cc_mask normal map)");
  }
  {   // 8. S110 LOGOSAMP: the CAW logo sampler (sub_822E6CC8) reaches exactly the slots BindMaterial binds as Logo7 / Logo4.
    Check(cc::TakesLogoSampler(true, false, false, 7) && cc::TakesLogoSampler(true, false, false, 4) && cc::TakesLogoSampler(true, false, true, 7) && !cc::TakesLogoSampler(true, false, true, 4)
          && !cc::TakesLogoSampler(true, true, false, 7) && !cc::TakesLogoSampler(true, true, false, 4) && !cc::TakesLogoSampler(false, false, false, 7) && !cc::TakesLogoSampler(true, false, false, 0),
          "logo sampler: a character bind's 7 / 4, not a change-colour bind's masks, not a wet bind's sweat bump, not a prop");
    Check(LogoSamplerMismatches(g_plant) == 0, "logo sampler = the bind's Logo7 / Logo4 role, every (character, change-colour, wet, slot 0..15)");
  }
  {   // 9. S110 LOGOSAMP (review): the Threads editor's prop mask sampler (sub_8338EC00) reaches slot 12 of an editor prop draw only.
    Check(PropMaskSamplerMismatches(g_plant) == 0, "prop mask sampler: slot 12 of a recoloured prop drawn under the editor's callback; not a saved set's match draw, not another slot");
  }
  Handoff();
  std::printf("%s\n", cc::Bodies().CensusLine().c_str());
  std::printf("%s\n", rdr_scene::change_colour_draw::Draws().Text().c_str());
  std::printf("%s: %d failure(s)\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "selftest";
  if (mode == "selftest") {
    g_plant = argc > 2 ? argv[2] : "";
    cc::PlantForTest() = g_plant == "no_switch" ? cc::Fault::kIgnoreSwitch : g_plant == "state_not_10" ? cc::Fault::kIgnoreState : g_plant == "foreign_callback" ? cc::Fault::kAnyCallback : cc::Fault::kNone;
    if (!g_plant.empty()) std::printf("planted twin: %s (this run must FAIL)\n", g_plant.c_str());
    return Selftest();
  }
  if (mode == "plantcheck") {   // S110 LOGOSAMP: one planted twin of the logo-sampler rule, run alone; exit 0 = its check caught it (prove.py reads the exit code)
    const std::string plant = argc > 2 ? argv[2] : "";
    const int bad = plant.empty() ? 0 : plant == "prop_any_row" ? PropMaskSamplerMismatches(plant) : LogoSamplerMismatches(plant);
    std::printf("plantcheck %s: %d mismatching cases -> %s\n", plant.c_str(), bad, bad ? "caught" : "NOT CAUGHT");
    return bad ? 0 : 1;
  }
  std::fprintf(stderr, "usage: thr_unit.exe selftest [old_zero|no_switch|state_not_10|foreign_callback|no_wiring|logo_any_bind|prop_any_row] | thr_unit.exe plantcheck logo_any_bind|prop_any_row\n");
  return 2;
}
