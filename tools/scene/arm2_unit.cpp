// entry: Latch::OnBuildStep   (the function the hook wwe2k14_attire_build_step calls; the unit then drives the record side and the loader half)
// S103 ARM2: offline proof of the composite attire's colours read at the game's BUILD STEP (src\assets\attire_build.h, docs\agents\s103\HNN.md, run wwe2k14_046). 0 game runs; every byte here is a
// fixture made in this file (nothing from the game). Heenan-shaped: chosen key 24912 -> loaded 24902, mode 1, P+44 = 0 (colour set 0), the colours of "slot 1" copied into set 0 of the block.
//   arm2_unit.exe selftest
// Production order driven (as the game runs it): the model link read (ModelLinkRead, early, before the game's build: mode 0 / no colours) -> the build-step hook body (Latch::OnBuildStep, steps 0, 1, 2 with
// the guest words the game's sub_83418580 reads) -> the contributor's record (SubmitSlotKey / SubmitModelLink + VerifyBodyLink + Emit) -> the loader's half (the entry's key parse, the remap rule, the tag parse,
// Compose of base 24902 + masks with the tag's colours). Planted twins: the link-time read (no colours), the wrong slot (the key's tens digit / desc+48), mode 2 folded into a build, the switch off.
// Build: pwsh -NoProfile -File tools\scene\build_arm2_unit.ps1
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/assets/attire_build.h"
#include "../../src/assets/attire_remap.h"
#include "../../src/scene_live/model_hooks.h"
#include "../../src/scene_live/wrestler_contrib.h"
#include "../../src/scene_live/piece_link.h"   // S103 HPF: the old DescriptorOfBody rule as a planted twin
#include "../../src/scene_live/slot_key.h"
#include "../../src/renderer_scene/named_load_fail.h"

using namespace rdr_scene;
using namespace rdr_scene::live;
namespace ar = assets::attire_remap;
namespace ab = attire_build;

static int g_fail = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}

// Stand-ins for the writer's registry, which the contributor does not need (as tools\scene\wrestler_emit.cpp).
namespace rdr_scene::live {
void Register(Contributor*) {}
bool Enabled() { return true; }
void EndFrame(uint64_t) {}
void AddSink(Sink*) {}
void RemoveSink(Sink*) {}
}  // namespace rdr_scene::live

static SceneTables EmitAt(WrestlerContributor& w, uint64_t frame) {
  SceneTables t;
  Counters c;
  FrameInfo f;
  f.guest_frame = frame + WrestlerContributor::kPaletteLead;
  w.Emit(f, t, c);
  return t;
}

// ---- the guest, as a word map ----
struct Mem {
  std::map<uint32_t, uint32_t> w;
  bool operator()(uint32_t a, uint32_t& v) const {
    auto it = w.find(a);
    if (it == w.end()) return false;
    v = it->second;
    return true;
  }
};

constexpr uint32_t kComposite = 0x5000, kP = 0x30000, kBlock = 0x40000, kDesc = 0x50000, kReader = 0x1000, kBody = 0x60000000;
constexpr uint32_t kChosen = 24912, kLoaded = 24902;
// S103 HPF: the slot chain the hook walks (slot_key.h SlotDescriptor): character kChar (+1220 = manager index 1), the match context kCtx (descriptor D_i = kCtx + 864 + 80 i, byte D+7 = the manager's descriptor index),
// the manager kMgr ([kMgr + 8] = kArr, descriptor j = kArr + 192 j). kDesc is element 2 of the array: Heenan-shaped = manager index 1, D+7 = 2 (so the two rules differ and D+7 matters).
constexpr uint32_t kChar = 0x70000, kCtx = 0x80000, kMgr = 0x90000, kArr = kDesc - 192 * 2;
static void PlantSlot(Mem& m, uint32_t s, uint32_t b7) {
  m.w[0x83924734u] = kCtx, m.w[0x837635A4u] = kMgr, m.w[kMgr + 8] = kArr;
  m.w[kChar + 1220] = s;   // the remap table word 0x8392475C is unmapped here: RemapActive false, [ch + 1220] is the index
  m.w[kChar + 3760] = b7 != 255 ? b7 : s;   // S110 DESCKEY: the hook's slot chain now reads the character's own descriptor index (the one it plants as D+7 / the manager index), CharModelDesc
  m.w[kDesc + 52] = 1;                      // desc+52 = loaded (CharModelDesc refuses a freed descriptor)
  for (uint32_t i = 0; i < 9; ++i) m.w[kCtx + 864 + 80 * i + 4] = 255;
  m.w[kCtx + 864 + 80 * s + 4] = b7;   // the word's low byte is D+7 (big-endian)
  for (uint32_t j = 0; j < 12; ++j) {
    if (kArr + 192 * j == kDesc) continue;
    m.w[kArr + 192 * j + 44] = 1, m.w[kArr + 192 * j + 92] = 51200 + j, m.w[kArr + 192 * j + 4] = 0, m.w[kArr + 192 * j + 52] = 1;
  }
}

// Heenan-shaped colours of "slot 1" (the S100 capture's step constants, attire_build.md): step 0 R grey / G / B / A, step 1 two reds + grey + A, step 2 grey / dark / white / white, step 3 untouched 255s.
static void Slot1(uint8_t* c) {
  std::memset(c, 255, ab::kStepBytes * 4);
  const uint8_t v[3][12] = {{35, 35, 35, 160, 160, 160, 215, 215, 215, 160, 160, 160},
                            {175, 20, 20, 175, 20, 20, 35, 35, 35, 160, 160, 160},
                            {35, 35, 35, 15, 15, 15, 255, 255, 255, 255, 255, 255}};
  for (int k = 0; k < 3; ++k) std::memcpy(c + 12 * k, v[k], 12);
}

static void PutBytes(Mem& m, uint32_t at, const uint8_t* b, int n) {
  for (int i = 0; i < n / 4; ++i) m.w[at + 4 * uint32_t(i)] = uint32_t(b[4 * i]) << 24 | uint32_t(b[4 * i + 1]) << 16 | uint32_t(b[4 * i + 2]) << 8 | b[4 * i + 3];
}

// The state at the game's first build step. `p44` = the in-memory colour set byte (0: the "Default SS Change Color Model" rule copied slot 1's colours into set 0); set 1 holds DECOY colours (what a wrong
// slot reads). desc+48 = 1 (the link's copy when the link ran after the rule... a wrong source: the tens digit of the key).
static Mem BuildScene(uint32_t mode = 1, uint32_t p44 = 0, uint32_t desc_body = kBody) {   // desc_body: desc+4 (kBody = the linked-body route; 0 = what a character slot load leaves, HPR.md)
  Mem m;
  m.w[kComposite + 100] = kP, m.w[kComposite + 140] = kDesc;
  m.w[kP + 40] = mode, m.w[kP + 44] = p44 << 24, m.w[kP + 16] = kBlock, m.w[kP + 0] = kChosen, m.w[kP + 60] = 0xC800, m.w[kP + 64] = kLoaded;
  m.w[kDesc + 28] = kLoaded, m.w[kDesc + 92] = kChosen, m.w[kDesc + 44] = mode, m.w[kDesc + 48] = 1, m.w[kDesc + 4] = desc_body, m.w[kDesc + 12] = 0, m.w[kDesc + 16] = 0;
  m.w[kReader + 96] = kDesc, m.w[kReader + 100] = kP, m.w[kReader + 68] = 0x7000, m.w[kReader + 72] = 16;
  uint8_t s1[48], decoy[48];
  Slot1(s1);
  for (int i = 0; i < 48; ++i) decoy[i] = uint8_t(100 + i);
  PutBytes(m, kBlock + 132 * 0 + 1320, s1, 48);
  PutBytes(m, kBlock + 132 * 1 + 1320, decoy, 48);
  PlantSlot(m, 1, 2);
  return m;
}

// ---- the loader's half: a fixture colour table, flat DDS inputs, the production parse of the record's entry ----
static assets::Bytes TableWithHeenan(const uint8_t* slot1) {
  assets::Bytes t(2 + ar::kRecord, 0);
  t[0] = 1, t[1] = 0;
  uint8_t* r = t.data() + 2;
  r[0] = uint8_t(249 & 0xFF), r[1] = uint8_t(249 >> 8);
  r[4 + 1] = 1, r[14 + 1] = 1, r[24 + 1] = 0, r[34 + 1] = 0;   // slot 1: flag A + B, kind-2 word 0 -> attire 0
  std::memcpy(r + 64 + 48 * 1, slot1, 48);
  return t;
}

static void Put32(std::vector<uint8_t>& d, size_t o, uint32_t v) { std::memcpy(d.data() + o, &v, 4); }
static std::vector<uint8_t> DdsHeader(uint32_t w, uint32_t h, uint32_t fourcc, uint32_t bits, const uint32_t cm[4]) {
  std::vector<uint8_t> d(128, 0);
  std::memcpy(d.data(), "DDS ", 4);
  Put32(d, 12, h), Put32(d, 16, w), Put32(d, 84, fourcc), Put32(d, 88, bits);
  if (cm) for (int i = 0; i < 4; ++i) Put32(d, 92 + 4 * size_t(i), cm[i]);
  return d;
}
static std::vector<uint8_t> BaseDds() {   // 16x16 DXT1, flat: c0 = 0xC618 > c1, indices 0
  std::vector<uint8_t> d = DdsHeader(16, 16, 0x31545844u, 0, nullptr);
  for (int b = 0; b < 16; ++b) {
    const uint8_t blk[8] = {0x18, 0xC6, 0x17, 0xC6, 0, 0, 0, 0};
    d.insert(d.end(), blk, blk + 8);
  }
  return d;
}
static std::vector<uint8_t> MaskDds(int x0, int x1, int y0, int y1, uint16_t texel) {   // 16x16 A4R4G4B4 (R 0x0F00, G 0x00F0, B 0x000F, A 0xF000), `texel` inside the rectangle
  static const uint32_t cm[4] = {0x0F00, 0x00F0, 0x000F, 0xF000};
  std::vector<uint8_t> d = DdsHeader(16, 16, 0, 16, cm);
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 16; ++x) {
      const uint16_t v = (x >= x0 && x < x1 && y >= y0 && y < y1) ? texel : 0;
      d.push_back(uint8_t(v & 0xFF)), d.push_back(uint8_t(v >> 8));
    }
  return d;
}

// The loader's work on the record's entry (GameFilesSource::Load, the production order): sscanf the key, the remap rule, the tag parse, Compose of the model it remaps to. Returns the level-0 RGBA, "" on failure.
static bool LoaderCompose(const std::string& entry, const ar::Table& table, std::vector<uint8_t>& rgba, int* loaded_out = nullptr, int* slot_out = nullptr) {
  int key = 0;
  if (std::sscanf(entry.c_str(), "model_key:%d", &key) != 1) return false;
  const ar::Remap r = ar::Resolve(table, key);
  if (loaded_out) *loaded_out = r.to;
  if (slot_out) *slot_out = r.slot;
  attire_compose::Input in;
  if (!attire_compose::ParseEntryTag(entry, in.colours)) return false;
  static std::vector<uint8_t> base = BaseDds(), m1 = MaskDds(0, 8, 0, 16, 0x0F00), m2 = MaskDds(8, 16, 0, 8, 0x00F0), m3 = MaskDds(8, 16, 8, 12, 0x000F);
  in.base = base.data(), in.base_n = base.size();
  in.mask[0] = m1.data(), in.mask_n[0] = m1.size(), in.mask[1] = m2.data(), in.mask_n[1] = m2.size(), in.mask[2] = m3.data(), in.mask_n[2] = m3.size();
  attire_compose::Image img;
  std::string why;
  attire_compose::Options opt;
  if (!attire_compose::Compose(in, opt, img, why)) return false;
  return attire_compose::DecodeBc1Level(img.data.data(), img.data.size(), img.w, img.h, rgba);
}
static bool Near3(const std::vector<uint8_t>& px, int x, int y, int r, int g, int b, int tol = 10) {
  const uint8_t* p = &px[(size_t(y) * 16 + x) * 4];
  return std::abs(int(p[0]) - r) <= tol && std::abs(int(p[1]) - g) <= tol && std::abs(int(p[2]) - b) <= tol;
}
// Heenan's expected picture from slot 1's colours, written by hand: mask 1's R (left half) = step 0 R, mask 2's G (top right) = step 1 G, mask 3's B (rows 8..11 right) = step 2 B (white), the rest the base (~198).
static bool HeenanPicture(const std::vector<uint8_t>& px) {
  return Near3(px, 2, 5, 35, 35, 35) && Near3(px, 12, 2, 175, 20, 20) && Near3(px, 12, 9, 255, 255, 255) && Near3(px, 12, 14, 197, 195, 197);
}

static scene_inputs::ReadFn GuestReader(const Mem* m) {
  static const Mem* g = nullptr;
  g = m;
  return [](uint32_t a, void* dst, size_t n) -> bool {
    for (size_t k = 0; k < n; k += 4) {
      uint32_t v = 0;
      if (!(*g)(a + uint32_t(k), v)) return false;
      const uint32_t be = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
      std::memcpy(static_cast<uint8_t*>(dst) + k, &be, 4);
    }
    return true;
  };
}

// S103 HPF: the slot's descriptor through the hook's production step, with the deferred desc+4 = 0, and its twins.
static void HpfTest() {
  auto& L = ab::Latch::Instance();
  L.ResetForTest();
  std::string line;
  const Mem m = BuildScene(1, 0, 0);   // desc+4 = 0: a character slot load never writes it
  L.OnBuildStep(m, kComposite, 0, line);
  ab::SlotWhy why = ab::SlotWhy::kNoDesc;
  wwe2k14::slotkey::SlotDescInfo si;
  Check(ab::SlotDescForChar(m, kChar, kChosen, L, why, &si) == kDesc && why == ab::SlotWhy::kTag && si.s == 1 && si.b7 == 2 && si.idx == 2, "HPF: Heenan-shaped (index 1, D+7 = 2, desc+4 = 0): the descriptor is array element 2, verdict tag");
  uint32_t od = 0, ok = 0;
  Check(!scene_inputs::DescriptorOfBody(GuestReader(&m), kBody, od, ok) && L.TagForSlot(kChosen, 0).empty(), "PLANT (the old rule, DescriptorOfBody for a deferred body, desc+4 = 0): finds nothing, slot_desc 0, no tag: the 047 failure re-found");
  uint8_t s1[48];
  Slot1(s1);
  Check(L.TagForSlot(kChosen, kDesc) == attire_compose::EntryTag(s1), "HPF: the loader tag for that descriptor is Heenan's build-step colours");
  Mem f = m;   // D+7 == 255: the manager index is the descriptor index
  PlantSlot(f, 2, 255);
  Check(ab::SlotDescForChar(f, kChar, kChosen, L, why, &si) == kDesc && why == ab::SlotWhy::kTag && si.idx == 2, "twin: D+7 = 255 falls back to the slot index (2)");
  Mem o = m;   // D+7 points at another descriptor whose +92 != key
  PlantSlot(o, 1, 3);
  Check(ab::SlotDescForChar(o, kChar, kChosen, L, why) == 0 && why == ab::SlotWhy::kKeyMismatch, "twin: D+7 = 3, a descriptor of another key: no tag");
  Mem hi = m;
  PlantSlot(hi, 1, 12);
  Check(ab::SlotDescForChar(hi, kChar, kChosen, L, why) == 0 && why == ab::SlotWhy::kNoDesc, "twin: D+7 = 12 (>= 12 descriptors): refused");
  Mem ro = m;
  ro.w.erase(0x837635A4u);
  Check(ab::SlotDescForChar(ro, kChar, kChosen, L, why) == 0 && why == ab::SlotWhy::kNoDesc, "twin: the manager pointer unreadable: refused");
  Mem m0 = m;
  m0.w[kDesc + 44] = 0;
  Check(ab::SlotDescForChar(m0, kChar, kChosen, L, why) == 0 && why == ab::SlotWhy::kModeBase && L.Size() == 0, "mode 0 on the slot's descriptor: no tag, the capture is forgotten (SlotOwner unchanged)");
  L.ResetForTest();
}

static int SelfTest() {
  HpfTest();
  uint8_t slot1[48];
  Slot1(slot1);
  ar::Table table;
  Check(ar::Parse(TableWithHeenan(slot1), table), "the fixture colour table parses");
  const std::string want_tag = attire_compose::EntryTag(slot1);

  // ---- the read, step by step ----
  {
    Mem m = BuildScene();
    ab::Capture c;
    Check(ab::ReadStep(m, kComposite, 0, c) == ab::Step::kCaptured && c.chosen == kChosen && c.loaded == kLoaded && c.mode == 1 && c.set == 0 && c.desc == kDesc && std::memcmp(c.colours, slot1, 48) == 0,
          "step 0 reads P+44 = 0 -> set 0: Heenan's 48 colour bytes at block + 1320, the chosen / loaded keys and the owner desc");
    ab::Capture twin;
    Check(ab::ReadStep(m, kComposite, 0, twin, ab::Fault::kSetFromKeySlot) == ab::Step::kCaptured && std::memcmp(twin.colours, slot1, 48) != 0, "PLANT (wrong slot: the key's tens digit = set 1): reads the decoy block, not Heenan's colours");
    ab::Capture twin2;
    Check(ab::ReadStep(m, kComposite, 0, twin2, ab::Fault::kLinkTimeSet) == ab::Step::kCaptured && std::memcmp(twin2.colours, slot1, 48) != 0, "PLANT (the link's set desc+48 = 1): the decoy block again");
    ab::Capture s2;
    Check(ab::ReadStep(m, kComposite, 2, s2) == ab::Step::kRefreshed && std::memcmp(s2.colours + 24, slot1 + 24, 12) == 0, "step 2 re-reads its own 12 bytes");
    Check(ab::ReadStep(m, kComposite, 4, s2) == ab::Step::kNotAStep, "r4 >= 4: the game's colour loop does not run (nothing read)");
    Check(ab::ReadStep(BuildScene(2), kComposite, 0, s2) == ab::Step::kModeBase && ab::ReadStep(BuildScene(0), kComposite, 0, s2) == ab::Step::kModeBase, "mode 2 and mode 0 never compose (Create's one exit, sub_83419478 @83419808): refused, not folded");
    Check(ab::ReadStep(BuildScene(2), kComposite, 0, s2, ab::Fault::kComposeMode2) == ab::Step::kCaptured, "PLANT (mode 2 folded into a build): composes");
    Check(ab::ReadStep(BuildScene(3), kComposite, 0, s2) == ab::Step::kCaptured, "mode 3 builds (any mode but 0 / 2, as Create)");
    Mem bad = BuildScene();
    bad.w[kP + 44] = 16u << 24;
    Check(ab::ReadStep(bad, kComposite, 0, s2) == ab::Step::kBadSet, "set 16: refused");
    bad = BuildScene(), bad.w[kP + 44] = 0xFFu << 24;
    Check(ab::ReadStep(bad, kComposite, 0, s2) == ab::Step::kBadSet, "set -1 (the signed byte 0xFF): refused");
    bad = BuildScene(), bad.w.erase(kBlock + 1320 + 20);
    Check(ab::ReadStep(bad, kComposite, 0, s2) == ab::Step::kUnreadable, "an unreadable colour word: refused");
  }

  // ---- the production order for the participant-slot route (Heenan in 042 / 046: no link hook line) ----
  auto& L = ab::Latch::Instance();
  {
    L.ResetForTest();
    rdr_scene::namedfail::HoleRule().store(true);
    const Mem m = BuildScene(1, 0, 0);   // desc+4 = 0: a character slot load never writes it
    std::vector<Mat4> pose(4, IdentityMat4());
    auto record = [&](bool latch_on, const Mem* scene = nullptr, bool pass_desc = true, uint32_t key = kChosen) {
      const uint32_t kChosen = key;   // S105 A5d: the slot key under test (shadows the Heenan constant; the hook-step lines below stay as they were)   // the record frame for a body named by its slot key (the hook: find the slot's descriptor, SlotOwner, SubmitSlotKey)
      rdr_scene::namedfail::HoleRule().store(latch_on);
      WrestlerContributor w;
      w.SubmitSkin(kBody, 0, 4, pose.data(), 3);
      ab::SlotWhy why;   // S103 HPF: the hook's production step (index rule -> SlotOwner), desc+4 = 0 as the game leaves it
      const uint32_t sd = pass_desc && L.HasChosen(kChosen) ? ab::SlotDescForChar(scene ? *scene : m, kChar, kChosen, L, why) : 0;
      w.SubmitSlotKey(kBody, kChosen, sd);
      SceneTables t = EmitAt(w, 3);
      return t.objects.size() == 1 ? std::string(t.Str(t.assets[t.objects[0].asset].entry)) : std::string("<no object>");
    };
    Check(record(true) == "model_key:24912", "before the game's first build step the record names the plain key (no capture yet)");
    std::string line;
    Check(L.OnBuildStep(m, kComposite, 0, line) == ab::Step::kCaptured && !line.empty(), "build step 0 captured (hook body)");
    Check(L.OnBuildStep(m, kComposite, 1, line) == ab::Step::kRefreshed && L.OnBuildStep(m, kComposite, 2, line) == ab::Step::kRefreshed, "steps 1 and 2 refresh the same capture");
    Check(ab::Count().steps0_mode1 == 1 && ab::Count().steps0_mode2 == 0 && ab::Count().refused == 0 && ab::Count().refreshed == 2 && ab::Count().late_change == 0, "census: composite at build step 1 (mode 1 1 / mode 2 0), no late change");
    const std::string entry = record(true);
    Check(entry == "model_key:24912" + want_tag, "the record's entry carries the BUILD-STEP colours (model_key:24912/c:<slot 1's colours>)");
    Check(ab::Count().applied == 1, "census: colours applied 1");
    Check(record(false) == "model_key:24912", "PLANT / switch off (wwe2k14_rdr_attire_remap false): the old plain entry");
    Check(record(true, nullptr, false) == "model_key:24912", "F2: a slot body whose descriptor the hook could not find (slot_desc 0) is not tagged");
    {  // S105 A5d (the game's value, sub_822C0998 key <= 9999; the F3 bar moved with it): a loaded == chosen capture of a key >= 10000 (a Threads-edited stock attire, Hogan's default-built identity keys) DOES tag a slot-key body
      Mem same = m;
      same.w[kDesc + 28] = kChosen;
      L.ResetForTest();
      L.OnBuildStep(same, kComposite, 0, line);
      Check(record(true, &same) == "model_key:24912" + want_tag && ab::Count().identity_tagged == 1 && ab::Count().caw_refused == 0, "A5d: chosen == loaded (key >= 10000): the slot-key entry carries the colours, identity tagged 1");
      Check(record(true, &same) == "model_key:24912" + want_tag && ab::Count().identity_tagged == 1, "A5d: the identity count is once per capture");
      L.SetPlantForTest(ab::Plant::kOldIdentityRule);
      Check(record(true, &same) != "model_key:24912" + want_tag, "PLANT (the pre-A5d rule, loaded == chosen never tags): the entry is plain, i.e. the identity check above would FAIL under it");
      L.SetPlantForTest(ab::Plant::kNone);
      // the CAW twin: id 12 attire 1 kind 2 = key 1212, no pac: stays the plain key (guest-row fallback), refused once, on both routes
      constexpr uint32_t kCaw = 1212;
      Mem caw = m;
      caw.w[kDesc + 28] = caw.w[kDesc + 92] = kCaw, caw.w[kP + 0] = kCaw, caw.w[kP + 64] = kCaw;
      L.ResetForTest();
      L.OnBuildStep(caw, kComposite, 0, line);
      Check(record(true, &caw, true, kCaw) == "model_key:1212" && record(true, &caw, true, kCaw) == "model_key:1212" && ab::Count().caw_refused == 1 && ab::Count().identity_tagged == 0, "A5d twin: a created superstar (key 1212 < 10000) stays the plain key, caw refused 1 (once per capture)");
      Check(L.TagForOwner(kDesc, kCaw, kCaw).empty() && ab::Count().caw_refused == 1, "A5d twin: the link route refuses the CAW too (no hole for a body with no pac)");
      L.SetPlantForTest(ab::Plant::kNoCreatedGuard);
      Check(record(true, &caw, true, kCaw) != "model_key:1212", "PLANT (IsCreatedKey always false): the CAW entry is tagged, i.e. the CAW check above would FAIL under it");
      Check(!L.TagForOwner(kDesc, kCaw, kCaw).empty(), "PLANT (no created guard) on the link route: the CAW is tagged, i.e. the link CAW check above would FAIL under it");
      L.SetPlantForTest(ab::Plant::kNone);
      L.ResetForTest();
      L.OnBuildStep(m, kComposite, 0, line);
      L.OnBuildStep(m, kComposite, 1, line);
      L.OnBuildStep(m, kComposite, 2, line);
    }
    {  // F2: the capture is of an earlier build of K; K is loaded again in mode 0 (the descriptor says so): plain, and the capture is gone
      Mem now0 = m;
      now0.w[kDesc + 44] = 0;
      Check(record(true, &now0) == "model_key:24912" && L.Size() == 0, "F2: K loaded again in mode 0 draws plain and forgets the stale capture");
      L.OnBuildStep(m, kComposite, 0, line);
      Mem other = m;
      other.w[kDesc + 92] = 24922;   // the descriptor now holds another attire
      Check(record(true, &other) == "model_key:24912", "F2: a descriptor whose chosen key is no longer the slot key lends nothing");
      L.OnBuildStep(m, kComposite, 1, line);
      L.OnBuildStep(m, kComposite, 2, line);
    }
    // the loader's half on that entry (GameFilesSource::Load's order)
    std::vector<uint8_t> px;
    int loaded = 0, slot = 0;
    Check(LoaderCompose(entry, table, px, &loaded, &slot) && loaded == kLoaded && slot == 1, "loader: the entry parses as key 24912, the game's remap gives 24902 (colour slot 1)");
    Check(HeenanPicture(px), "loader: base 24902 + masks + the build step's colours = Heenan's picture (grey / red / white areas, base elsewhere)");
    std::vector<uint8_t> plain;
    {  // S105 A5d: a tagged non-remapped (identity) key composes: the loader finds the named PACH (24902 is not in the table as a source), not remapped, and the build step's colours paint it
      std::vector<uint8_t> idpx;
      int idl = 0;
      Check(LoaderCompose("model_key:24902" + want_tag, table, idpx, &idl) && idl == 24902 && HeenanPicture(idpx), "A5d loader: a tagged identity key (24902) composes the colours, not remapped");
    }
    Check(!LoaderCompose("model_key:24912", table, plain), "twin: the plain entry (no colours) composes nothing: the plain base, the bug the log showed (no colour tag in the record)");

    // PLANT: the wrong slot through the whole chain: the hook reads the decoy block, the record carries it, the picture is wrong
    L.ResetForTest();
    std::string l2;
    L.OnBuildStep(m, kComposite, 0, l2, ab::Fault::kSetFromKeySlot);
    std::vector<uint8_t> px_bad;
    Check(LoaderCompose(record(true), table, px_bad) && !HeenanPicture(px_bad), "PLANT (colour slot = the key's tens digit): the picture is NOT Heenan's (the check around it fails)");

    // an orphan step (no step 0 seen: the hook was attached late) keeps nothing
    L.ResetForTest();
    Check(L.OnBuildStep(m, kComposite, 1, l2) == ab::Step::kRefreshed && ab::Count().orphan == 1 && L.Size() == 0 && record(true) == "model_key:24912", "an orphan step 1 is counted, nothing latched, the record stays plain");
    // a colour that changes between steps (the game reads it at each step): the capture follows, counted
    L.OnBuildStep(m, kComposite, 0, l2);
    Mem m2 = m;
    m2.w[kBlock + 1320 + 12] = 0x0A0B0C0Du;   // step 1's first word
    L.OnBuildStep(m2, kComposite, 1, l2);
    uint8_t cols[48];
    Check(ab::Count().late_change == 1 && attire_compose::ParseEntryTag(record(true), cols) && cols[12] == 0x0A && cols[15] == 0x0D && std::memcmp(cols, slot1, 12) == 0, "a step's colours that changed after step 0 are followed (counted late change), earlier steps untouched");
  }

  // ---- S105 A5d review F1: a LINKED created superstar (desc+4 != 0, key 1212 < 10000) gets no tag from the link read either (through ModelLinkRead + SubmitSkin + VerifyBodyLink + Emit) ----
  {
    L.ResetForTest();
    rdr_scene::namedfail::HoleRule().store(true);
    std::vector<Mat4> pose(4, IdentityMat4());
    auto linked = [&](uint32_t key, std::string* link_tag) {
      Mem lk = BuildScene(1, 0);   // a mode 1 link read: the link-time tag is the decoy colours (desc+48 = 1), non-empty for any key
      lk.w[kDesc + 28] = lk.w[kDesc + 92] = key, lk.w[kP + 0] = key, lk.w[kP + 64] = key;
      WrestlerContributor w;
      wwe2k14::wrestlers::ModelLinkRead(w, kReader, lk);
      if (link_tag) *link_tag = w.ModelKeyFor(kBody).tag;
      w.SubmitSkin(kBody, 0, 4, pose.data(), 3);
      w.VerifyBodyLink(kBody, true);
      SceneTables t = EmitAt(w, 3);
      return t.objects.size() == 1 ? std::string(t.Str(t.assets[t.objects[0].asset].entry)) : std::string("<none>");
    };
    std::string tag_id, tag_caw;
    const std::string id_entry = linked(24902, &tag_id);
    Check(!tag_id.empty() && id_entry.find("/c:") != std::string::npos && ab::Count().caw_refused == 0, "A5d F1 precondition: a linked stock key (24902, mode 1) carries the link-time tag (non-empty): the route does tag");
    const std::string caw_entry = linked(1212, &tag_caw);
    Check(tag_caw.empty() && caw_entry == "model_key:1212" && ab::Count().caw_refused == 1, "A5d F1: a linked CAW (key 1212) has NO tag from the link read, its record entry is the plain key, caw refused 1 (dropped at the source)");
  }

  // ---- the production order for the model-link route: link (early, no colours) -> build steps -> record ----
  {
    L.ResetForTest();
    rdr_scene::namedfail::HoleRule().store(true);
    Mem early = BuildScene(0);   // the link read of a load whose mode is not yet a build: base, no tag (the old path's whole answer for Heenan)
    WrestlerContributor w;
    wwe2k14::wrestlers::ModelLinkRead(w, kReader, early);
    Check(w.ModelKeyFor(kBody).known && w.ModelKeyFor(kBody).tag.empty() && w.ModelKeyFor(kBody).desc == kDesc && w.ModelKeyFor(kBody).key == kLoaded, "link-time read (old path): a base, NO colours in the link (twin of the bug)");
    const Mem m = BuildScene();
    std::string line;
    for (uint32_t s = 0; s < 3; ++s) L.OnBuildStep(m, kComposite, s, line);
    std::vector<Mat4> pose(4, IdentityMat4());
    w.SubmitSkin(kBody, 0, 4, pose.data(), 3);
    w.VerifyBodyLink(kBody, true);
    SceneTables t = EmitAt(w, 3);
    const std::string entry = t.objects.size() == 1 ? std::string(t.Str(t.assets[t.objects[0].asset].entry)) : std::string("<none>");
    Check(entry == "model_key:24902" + want_tag, "the body's link names key 24902 and the capture matched by OWNER (desc + loaded key) supplies the colours");
    std::vector<uint8_t> px;
    Check(LoaderCompose(entry, table, px) && HeenanPicture(px), "loader: the linked body composes Heenan's picture too");
    // owner pairing: another body / descriptor with the same loaded key gets nothing
    WrestlerContributor wo;
    wo.SubmitModelLink(kBody + 0x1000, kLoaded, 0, 0, 0x9000, {}, 0, std::string(), kChosen);
    wo.SubmitSkin(kBody + 0x1000, 0, 4, pose.data(), 3);
    wo.VerifyBodyLink(kBody + 0x1000, true);
    SceneTables to = EmitAt(wo, 3);
    Check(to.objects.size() == 1 && std::string(to.Str(to.assets[to.objects[0].asset].entry)) == "model_key:24902", "owner pairing: a body of another descriptor with the same loaded key (Heenan a0) stays the plain stock attire");
    // a mode 1 link read after the capture: the build-step colours win over the link-time ones
    Mem lk = BuildScene(1, 0);
    lk.w[kDesc + 48] = 1;   // the link's copy of the set: the decoy block
    WrestlerContributor wl;
    wwe2k14::wrestlers::ModelLinkRead(wl, kReader, lk);
    uint8_t link_cols[48];
    Check(attire_compose::ParseEntryTag(wl.ModelKeyFor(kBody).tag, link_cols) && link_cols[0] == 100, "(precondition) a mode 1 link read composes from desc+48 = 1: the decoy colours");
    wl.SubmitSkin(kBody, 0, 4, pose.data(), 3);
    wl.VerifyBodyLink(kBody, true);
    SceneTables tl = EmitAt(wl, 3);
    Check(tl.objects.size() == 1 && std::string(tl.Str(tl.assets[tl.objects[0].asset].entry)) == "model_key:24902" + want_tag, "a link-time composite tag (decoy set) is overridden by the build step's colours (the game's own)");
    // switch off: the link's own tag only
    rdr_scene::namedfail::HoleRule().store(false);
    SceneTables tf = EmitAt(wl, 3);
    Check(tf.objects.size() == 1 && std::string(tf.Str(tf.assets[tf.objects[0].asset].entry)) != "model_key:24902" + want_tag, "switch off: the build-step colours are not used (the old link tag)");
    rdr_scene::namedfail::HoleRule().store(true);
  }

  // ---- F1: a reused descriptor never lends one attire's colours to another; F5 / Clear ----
  {
    L.ResetForTest();
    rdr_scene::namedfail::HoleRule().store(true);
    std::string line;
    const Mem a = BuildScene();
    Mem b = BuildScene();
    b.w[kDesc + 92] = 24922;   // the same descriptor and loaded key now build another chosen key (Heenan a2)
    b.w[kBlock + 1320] = 0x01020304u;
    L.OnBuildStep(a, kComposite, 0, line);
    Check(!L.TagForOwner(kDesc, kLoaded, kChosen).empty() && L.TagForOwner(kDesc, kLoaded, 24922).empty() && L.TagForOwner(kDesc, kLoaded, 0).empty(), "owner match needs the chosen key too (24922 and unknown chosen get nothing)");
    L.OnBuildStep(b, kComposite, 0, line);
    Check(L.Size() == 1 && L.TagForOwner(kDesc, kLoaded, kChosen).empty() && !L.TagForOwner(kDesc, kLoaded, 24922).empty(), "F1: the step-0 capture of the same descriptor erases the other chosen key's (twin: a1's black never paints a2)");
    WrestlerContributor wl;
    wl.SubmitModelLink(kBody, kLoaded, 0, 0, kDesc, {}, 0, std::string(), kChosen);
    std::vector<Mat4> pose(4, IdentityMat4());
    wl.SubmitSkin(kBody, 0, 4, pose.data(), 3);
    wl.VerifyBodyLink(kBody, true);
    SceneTables t = EmitAt(wl, 3);
    Check(t.objects.size() == 1 && std::string(t.Str(t.assets[t.objects[0].asset].entry)) == "model_key:24902", "a body linked as chosen 24912 on that descriptor stays plain (Heenan a0 / a1 never get a2's colours)");
    // F5: a capture in use is never the eviction victim
    L.ResetForTest();
    for (uint32_t i = 0; i < ab::kMaxEntries; ++i) {
      Mem m = BuildScene();
      m.w[kDesc + 92] = 20000 + 10 * i + 2, m.w[kDesc + 28] = 20000 + 10 * i;
      m.w[kComposite + 140] = 0x50000 + 0x100 * (i + 1);   // another descriptor each (so F1 does not erase)
      m.w[0x50000 + 0x100 * (i + 1) + 92] = 20000 + 10 * i + 2, m.w[0x50000 + 0x100 * (i + 1) + 28] = 20000 + 10 * i;
      L.OnBuildStep(m, kComposite, 0, line);
    }
    const uint32_t first_desc = 0x50000 + 0x100, first_chosen = 20002, first_loaded = 20000;
    Check(!L.TagForOwner(first_desc, first_loaded, first_chosen).empty(), "the oldest capture is used");
    Mem extra = BuildScene();
    extra.w[kDesc + 92] = 29992, extra.w[kDesc + 28] = 29990;
    L.OnBuildStep(extra, kComposite, 0, line);
    Check(L.Size() == ab::kMaxEntries && !L.TagForOwner(first_desc, first_loaded, first_chosen).empty(), "F5: the capture just used survives the eviction of the 65th");
    L.Clear();
    Check(L.Size() == 0, "Clear (ModelCache::ClearFailed, a new game folder) empties the latch");
  }

  // ---- mode 0 / mode 2 at a later link forget the capture; mode 2 is counted apart ----
  {
    L.ResetForTest();
    std::string line;
    L.OnBuildStep(BuildScene(), kComposite, 0, line);
    Check(L.Size() == 1, "a capture is held");
    WrestlerContributor w;
    wwe2k14::wrestlers::ModelLinkRead(w, kReader, BuildScene(1, 0));   // a mode 1 link: the capture stays
    Check(L.Size() == 1, "a mode 1 link read keeps the capture");
    wwe2k14::wrestlers::ModelLinkRead(w, kReader, BuildScene(2, 0));   // the same chosen key loaded without a build
    Check(L.Size() == 0 && ab::Count().forgotten == 1 && attire_compose::Count().base_mode2.load() >= 1, "a mode 2 link read of the chosen key forgets it, counted apart from mode 0 (never folded)");
    L.OnBuildStep(BuildScene(), kComposite, 0, line);
    wwe2k14::wrestlers::ModelLinkRead(w, kReader, BuildScene(0, 0));
    Check(L.Size() == 0 && ab::Count().forgotten == 2, "a mode 0 link read forgets it too");
    // mode 2 at a build step: counted, never composed
    L.OnBuildStep(BuildScene(2), kComposite, 0, line);
    Check(L.Size() == 0 && ab::Count().steps0_mode2 == 1, "census: a build step in mode 2 is counted (mode 2 1) and composes nothing");
  }
  // ---- the latch is bounded ----
  {
    L.ResetForTest();
    std::string line;
    for (uint32_t i = 0; i < ab::kMaxEntries + 5; ++i) {
      Mem m = BuildScene();
      m.w[kComposite + 140] = 0x50000 + 0x100 * (i + 1);   // another descriptor each (one descriptor holds one capture, F1)
      m.w[0x50000 + 0x100 * (i + 1) + 92] = 20000 + 10 * i + 2, m.w[0x50000 + 0x100 * (i + 1) + 28] = kLoaded;
      L.OnBuildStep(m, kComposite, 0, line);
    }
    Check(L.Size() == ab::kMaxEntries && ab::Count().evicted == 5, "the latch keeps at most 64 captures (oldest out)");
  }
  L.ResetForTest();
  rdr_scene::namedfail::HoleRule().store(true);
  std::printf("%s\n", ab::CensusLine().c_str());
  std::printf("arm2_unit: %s\n", g_fail ? "FAIL" : "ok");
  return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc >= 2 && std::strcmp(argv[1], "selftest") == 0) return SelfTest();
  if (argc >= 2 && std::strcmp(argv[1], "hpf") == 0) { HpfTest(); std::printf("hpf: %s\n", g_fail ? "FAIL" : "ok"); return g_fail ? 1 : 0; }
  std::printf("usage: arm2_unit selftest | hpf\n");
  return 2;
}
