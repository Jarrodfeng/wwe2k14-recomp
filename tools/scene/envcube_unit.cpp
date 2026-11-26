// S111 ENVCUBE unit: the soft environment light cube at pixel sampler 10 (g_mEnvLPrm, name-table site sub_82BAAEE8 0x82BACFC8), through the real RoleOfSlot /
// muscle_map::Role chain and env_cube::CubeAt (src\renderer_scene\env_cube.h, called by FrontEnd::BindMaterial) over the real registry and define tables.
// 0 game runs (docs\research\s111\ENVCUBE.md).
//   out\tools\envcube_unit.exe                   selftest: slot 10's role on every chain, every yCh fragment that samples 10 defines FX_ENV_LIGHT, the image
//                                                slot 10 takes, the planted twin (the old chain: the black cube) must bind the wrong image
//   out\tools\envcube_unit.exe real --game DIR   read-only, the player's files: ch100 attire 0 through the production loader (LoadWrestler -> LoadJboy):
//                                                its yCh_Reflect eye parts carry chCommon's CUBEMAP_eyes as a 6-face DXT1 128x128 8-mip cube that uploads as one
//   out\tools\envcube_unit.exe plant             S111 GUESTCUBE twin (guestmodel::g_draw_plant 25: a created superstar's slot 10 left black, the old chain): must FAIL (exit 1)
//   out\tools\envcube_unit.exe plant26           S111 EYECUBE twin (g_draw_plant 26: the cube picked by effect though the record names it, the old rule): must FAIL (exit 1)
//   S111 EYECUBE: the selftest also checks the name read (guest_model_mat.h over forged memory), the writer (sections 72 / 73), the record round trip, the > 64 section limit and the pick by NAME
//   S111 GUESTCUBE: the selftest also builds created-superstar parts through the real guestmodel::BuildModel (guest rows, a published cube pair); `real` also reads chCommon's
//   two cubes through guest_env_cube_load.h and builds a guest yCh_Reflect part with them (slot 10 = chCommon's own CUBEMAP_eyes)
// Build + run: pwsh -NoProfile -File tools\scene\build_envcube_unit.ps1
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../src/assets/asset_load.h"
#include "../../src/renderer/shaders/spv/fx_registry.h"
#include "../../src/renderer_scene/fx_tables.h"
#include "../../src/renderer_scene/muscle_map.h"
#include "../../src/renderer_scene/env_cube.h"
#include "../../src/renderer_scene/upload_check.h"
#include "../../src/renderer_scene/guest_model_draw.h"      // S111 GUESTCUBE: a created superstar's part through the real BuildModel
#include "../../src/renderer_scene/guest_env_cube_load.h"   // S111 GUESTCUBE: chCommon's CUBEMAP_eyes / CUBEMAP, the shared cubes
#include "../../src/scene_live/guest_model_emit.h"          // S111 EYECUBE: the real row writer (sections 72 / 73)

using namespace rdr_scene;

namespace {

int g_bad = 0;
void Check(bool ok, const char* name, const std::string& note = "") {
  std::printf("%s  %-48s %s\n", ok ? "PASS" : "FAIL", name, note.c_str());
  g_bad += ok ? 0 : 1;
}

struct Info { uint32_t faces = 1; };   // FrontEnd::ModelData::ImgInfo's `faces`
constexpr int kNone = 0, kBlackCube = 9;

// BindMaterial's EnvCube case (front_end.cpp): the material's six-face image, else the black cube.
int Slot10(CharSlot role, int env, const std::vector<int>& images, const std::vector<Info>& info) {
  if (role != CharSlot::EnvCube) return -1;
  const int c = env_cube::CubeAt(images, info, env, kNone);
  return c != kNone ? c : kBlackCube;
}
// The twin: the chain before the lane (RoleOfSlot(10) = FlatBlackCube -> black_cube_, whatever the material names).
int OldSlot10() { return kBlackCube; }

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower((unsigned char)c));
  return s;
}

// ---- S111 GUESTCUBE: a created superstar's part (guest rows: no jboy record, so no texture NAME) through the real guestmodel::BuildModel (src\renderer_scene\guest_model_draw.h)
// A cube shaped as the loader parses chCommon's (BC1 128x128, 8 mips, 6 faces); `fill` marks its texels so the two cubes differ.
assets::Texture FakeCube(const char* name, uint8_t fill) {
  assets::Texture t;
  t.name = name, t.format = assets::TexFormat::kBC1, t.width = 128, t.height = 128, t.mips = 8, t.faces = 6;
  size_t chain = 0;
  for (uint32_t k = 0; k < 8; ++k) chain += upload::UploadMipBytes(assets::TexFormat::kBC1, std::max(1u, 128u >> k), std::max(1u, 128u >> k));
  t.data.assign(6 * chain, fill);
  return t;
}
// One guest part of the game's effect name `effect` whose one sampler row is g_mEnvLPrm in state kGtEngineDefault: what the reader writes for a sampler whose texture object is a cube
// (guest_model_tex.h kWhyEngineCube; run wwe2k14_007 line 2780: a 128x128 DXT1 six-face object). Built by the production BuildModel; an empty model when it refuses.
assets::Model GuestPart(const char* effect, const char* tex_name = nullptr) {
  rdr_scene::SceneTables t;
  rdr_scene::GuestModelPart r;
  const uint32_t F = 0x0C | 0x10 | 0x40 | 0x100;   // the flag word guest_model_unit's forged parts use
  const uint32_t stride = rdr_scene::live::guest_model::LayoutOfFlags(F).Stride();
  r.model = 0x40001000u, r.part = 0, r.parts = 1, r.model_hash = 0xC0BEu;
  r.vtx_hash = 0xC0BE01u, r.idx_hash = 0xC0BE02u;
  r.vertex_count = 3, r.stride = stride, r.index_count = 3, r.n0 = 3, r.n1 = 0, r.vflags = F;
  r.node_count = 1, r.nodes[0] = 0;
  std::strncpy(r.effect, effect, rdr_scene::kGmNameBytes);
  r.tex_count = 1;
  std::strncpy(r.tex[0].name, "g_mEnvLPrm", rdr_scene::kGmNameBytes);
  r.tex[0].key = 0x08430074u, r.tex[0].state = rdr_scene::kGtEngineDefault, r.tex[0].hash = 0;
  const size_t vb = size_t(3) * stride;
  r.vtx_off = 0, r.idx_off = uint32_t((vb + 7) & ~size_t(7));
  t.guest_models.blobs.assign(size_t(r.idx_off) + 8, 0);
  const uint16_t idx[3] = {0, 1, 2};
  std::memcpy(t.guest_models.blobs.data() + r.idx_off, idx, sizeof idx);
  t.guest_models.parts.push_back(r);
  if (tex_name) {   // S111 EYECUBE: the part's 72 rows (record words, each sampler's parameter name and t) and the model's 73 names of those t, as guestmats::AppendRows writes them. The review's twin
                    // of the join: a sampler the part row does NOT carry (a 7th, past tex[]'s kGmMaxTex) is the FIRST 72 row, its t named "sweat_map": a join by place would hand tex[0] that name
    rdr_scene::GuestModelMat extra;
    extra.model = r.model, extra.part = 0, extra.sampler = 0, extra.t = 9;
    std::strncpy(extra.param, "g_mSweatPrm", rdr_scene::kGmNameBytes);
    t.guest_models.mats.push_back(extra);
    rdr_scene::GuestModelMat mr;
    mr.model = r.model, mr.part = 0, mr.sampler = 1, mr.t = 5;
    std::memcpy(mr.param, r.tex[0].name, rdr_scene::kGmNameBytes);
    t.guest_models.mats.push_back(mr);
    rdr_scene::GuestModelTexName sweat;
    sweat.model = r.model, sweat.t = 9;
    std::strncpy(sweat.name, "sweat_map", rdr_scene::kGmNameBytes);
    t.guest_models.tex_names.push_back(sweat);
    rdr_scene::GuestModelTexName nr;
    nr.model = r.model, nr.t = 5;
    std::strncpy(nr.name, tex_name, rdr_scene::kGmNameBytes);
    t.guest_models.tex_names.push_back(nr);
  }
  const std::vector<uint8_t> bytes = rdr_scene::SerializeRecord(t);   // through the record file, as a recorded run plays
  rdr_scene::SceneTables back;
  std::string err;
  if (!rdr_scene::ParseRecord(bytes.data(), bytes.size(), back, err)) return std::printf("      record refused: %s\n", err.c_str()), assets::Model{};
  if (back.guest_models.mats.size() != t.guest_models.mats.size() || back.guest_models.tex_names.size() != t.guest_models.tex_names.size()) return std::printf("      sections 72 / 73 lost in the round trip\n"), assets::Model{};
  guestmodel::Store store;
  store.Absorb(back);
  assets::Model m;
  guestmodel::BuildStats bs;
  if (!guestmodel::BuildModel(back, store, r.model, m, bs)) std::printf("      guest build refused: %s\n", bs.why.c_str()), m = assets::Model{};
  return m;
}
int EnvOf(const assets::Model& m) { return m.materials.empty() ? -2 : m.materials[0].env_cube; }
std::string CubeName(const assets::Model& m) {
  const int e = EnvOf(m);
  return e >= 0 && size_t(e) < m.textures.size() ? m.textures[size_t(e)].name : std::string("(none)");
}
// The image slot 10 of the guest part's draw takes (BindMaterial's EnvCube case over the model's own textures, in AddModel's order: image 1000 + i).
int GuestSlot10(const assets::Model& m) {
  if (m.materials.empty()) return -2;
  std::vector<int> images;
  std::vector<Info> info;
  for (size_t i = 0; i < m.textures.size(); ++i) images.push_back(1000 + int(i)), info.push_back({m.textures[i].faces});
  return Slot10(RoleOfSlot(10), m.materials[0].env_cube, images, info);
}
void GuestChecks() {
  auto cubes = std::make_shared<guest_env_cube::Cubes>();
  cubes->eyes = FakeCube(guest_env_cube::kEyesName, 0x11), cubes->plain = FakeCube(guest_env_cube::kPlainName, 0x22);
  guest_env_cube::Publish(cubes);
  const assets::Model eye = GuestPart("ych_reflect"), trans = GuestPart("ych_trans"), tiling = GuestPart("ych_transtiling");
  const int ee = EnvOf(eye);
  Check(ee >= 0 && eye.textures[size_t(ee)].name == "CUBEMAP_eyes" && eye.textures[size_t(ee)].faces == 6 && eye.textures[size_t(ee)].data == cubes->eyes.data && GuestSlot10(eye) == 1000 + ee,
        "guest.reflect_binds_cubemap_eyes", "a created superstar's yCh_Reflect part: slot 10 = the 6-face CUBEMAP_eyes cube (got " + CubeName(eye) + ")");
  Check(CubeName(trans) == "CUBEMAP" && CubeName(tiling) == "CUBEMAP" && GuestSlot10(trans) == 1000 + EnvOf(trans) && GuestSlot10(tiling) == 1000 + EnvOf(tiling), "guest.trans_binds_cubemap",
        "yCh_trans / yCh_TransTiling parts: CUBEMAP (got " + CubeName(trans) + " / " + CubeName(tiling) + ")");
  Check(eye.textures.size() == 1 && eye.materials.size() == 1, "guest.one_cube_per_build", std::to_string(eye.textures.size()) + " texture(s) in the built model");
  guest_env_cube::Publish(nullptr);
  const assets::Model none = GuestPart("ych_reflect");
  Check(!none.materials.empty() && EnvOf(none) == -1 && GuestSlot10(none) == kBlackCube, "guest.no_cubes_black", "cubes not loaded (no game files): the black cube, as before");
}

// ---- S111 EYECUBE: the cube by the NAME the part's record carries (sub_82BD9A60 / sub_82BA9B60), docs\research\s111\EYECUBE.md
struct FakeMem {   // forged guest memory: words (host values) and byte runs by start address
  std::unordered_map<uint32_t, uint32_t> w;
  std::unordered_map<uint32_t, std::vector<uint8_t>> b;
  bool u32(uint32_t a, uint32_t& v) const {
    const auto it = w.find(a);
    if (it == w.end()) return false;
    v = it->second;
    return true;
  }
  bool bytes(uint32_t a, uint32_t n, uint8_t* d) const {
    for (const auto& run : b)
      if (a >= run.first && uint64_t(a) + n <= uint64_t(run.first) + run.second.size()) {
        std::memcpy(d, run.second.data() + (a - run.first), n);
        return true;
      }
    return false;
  }
};
std::string Name16(const char* n) { return std::string(n, strnlen(n, 16)); }
void EyeCubeChecks() {
  namespace gm = rdr_scene::live::guest_model;
  namespace sk = rdr_scene::live::skin;
  // the name read: [m+268]+16t, else the data block's +36 table through the +60 getter
  const uint32_t model = 0x40002000u, names = 0x40003000u, D = 0x40004000u, VT = 0x40005000u, H = 0x40006000u, names2 = 0x40007000u;
  FakeMem a;
  a.w[model + gm::kOffTexNames] = names;
  std::vector<uint8_t> tab(16 * 4, 0);
  std::memcpy(tab.data() + 16 * 3, "cubemap_eyes", 12);
  a.b[names] = tab;
  char got[16];
  const uint32_t base = gm::TexNameTable(a, model);
  gm::ReadTexName(a, base, 3, got);
  Check(base == names && Name16(got) == "cubemap_eyes", "eyecube.reader_model_table", "[m+268]+16t, t 3: '" + Name16(got) + "'");
  FakeMem d;
  d.w[model + gm::kOffTexNames] = 0, d.w[model + sk::kSkinForwardOff] = D, d.w[D] = VT, d.w[VT + sk::kSkinDataBlockGetterOff] = sk::kSkinDataBlockGetterFn;
  d.w[D + sk::kSkinDataBlockOff] = H, d.w[H + gm::kDataTexNames] = names2;
  std::vector<uint8_t> tab2(16, 0);
  std::memcpy(tab2.data(), "CUBEMAP", 7);
  d.b[names2] = tab2;
  const uint32_t base2 = gm::TexNameTable(d, model);
  gm::ReadTexName(d, base2, 0, got);
  Check(base2 == names2 && Name16(got) == "CUBEMAP", "eyecube.reader_data_block_table", "[m+268] == 0: [[D+12]+36]+16t through the +60 getter");
  d.w[VT + sk::kSkinDataBlockGetterOff] = 0x82000000u;
  Check(gm::TexNameTable(d, model) == 0, "eyecube.reader_unknown_class_none", "a +60 that is not sub_827C17B8: no names (the legacy pick)");
  // the writer: one 72 row per sampler (every one: no cap), one words-only row for a part with none, one 73 row per (model, t)
  gm::ModelRead mr;
  mr.model = model, mr.parts = 3, mr.parts_ok = 3;
  mr.p.resize(3);
  for (uint32_t k = 0; k < 3; ++k) {
    gm::PartRead& q = mr.p[k];
    q.part = k, q.vertex_count = 3, q.stride = 4, q.n0 = 3, q.technique = 2, q.cull = 2;
    q.vtx = std::make_shared<const std::vector<uint8_t>>(12, uint8_t(0));
    q.idx = {0, 1, 2};
  }
  gm::SamplerRead s;
  std::strncpy(s.name, "g_mEnvLPrm", 16);   // (the file's spelling: the game holds parameter names as stored; S111 UNIFY2 review)
  s.t = 3;
  std::strncpy(s.tex_name, "cubemap_eyes", 16);
  mr.p[0].samplers.push_back(s);
  mr.p[0].samplers.push_back(s);   // a second sampler naming the same t: one 73 row
  // part 1: seven samplers the material table takes (S111 UNIFY2: the part row's tex[] carries only those, up to 6): section 72 carries all seven, each with its own t
  static const char* const kSeven[7] = {"g_mDiffusePrm", "g_mNormalFxPrm", "g_mMaskPrm", "g_mNoiseTilePrm", "g_mSweatPrm", "g_mMusclePrm", "g_mHairNoisePrm"};
  for (int k = 0; k < 7; ++k) {
    gm::SamplerRead sk;
    std::strncpy(sk.name, kSeven[k], sizeof sk.name);
    sk.t = 10 + k;
    std::snprintf(sk.tex_name, sizeof sk.tex_name, "n%d", k);
    mr.p[1].samplers.push_back(sk);
  }
  SceneTables wt;
  gm::BlobSender blobs;
  gm::AppendModelRows(mr, wt.guest_models, blobs, 1);
  const auto& gmats = wt.guest_models.mats;
  const auto& gnames = wt.guest_models.tex_names;
  Check(gmats.size() == 10 && gmats[0].part == 0 && gmats[0].sampler == 0 && gmats[1].sampler == 1 && gmats[0].t == 3 && gmats[1].t == 3 && gmats[0].technique == 2 && gmats[0].cull == 2 &&
            Name16(gmats[0].param) == "g_mEnvLPrm" && gnames.size() == 8 && gnames[0].t == 3 && Name16(gnames[0].name) == "cubemap_eyes",
        "eyecube.writer_sections_72_73", std::to_string(gmats.size()) + " material row(s), " + std::to_string(gnames.size()) + " name row(s)");
  const bool seven = wt.guest_models.parts.size() == 3 && wt.guest_models.parts[1].tex_count == rdr_scene::kGmMaxTex;
  char s6[16] = {};
  std::strncpy(s6, kSeven[6], sizeof s6);
  Check(seven && guestmats::NameOf(wt.guest_models, model, 1, s6).name == "n6" && guestmats::NameOf(wt.guest_models, model, 1, wt.guest_models.parts[1].tex[5].name).name == "n5",
        "eyecube.writer_all_samplers", "section 72 carries the 7th sampler the part row's tex[] (6) drops, joined by its parameter name");
  Check(gmats[9].part == 2 && gmats[9].sampler == rdr_scene::kGmMatNoSampler && gmats[9].technique == 2 && guestmats::NameOf(wt.guest_models, model, 2, s6).carried &&
            guestmats::NameOf(wt.guest_models, model, 2, s6).name.empty(),
        "eyecube.writer_words_only_row", "a part with no sampler: one row of its record words (sampler = kGmMatNoSampler), carried with no name");
  // the reader accepts more than 64 sections (the writer has 66 push sites)
  const uint32_t n = 100;
  std::vector<uint8_t> rec((sizeof(SceneHeader) + n * sizeof(SectionEntry) + 7) & ~size_t(7), 0);
  SceneHeader h;
  h.section_count = n;
  std::memcpy(rec.data(), &h, sizeof h);
  for (uint32_t i = 0; i < n; ++i) {
    SectionEntry e;
    e.id = 9000 + i, e.element_size = 1, e.count = 0, e.offset = rec.size();
    std::memcpy(rec.data() + sizeof h + size_t(i) * sizeof e, &e, sizeof e);
  }
  SceneTables back;
  std::string err;
  Check(ParseRecord(rec.data(), rec.size(), back, err), "eyecube.record_reads_over_64_sections", err.empty() ? "100 sections read" : err);
  // the pick by NAME (the cubes published as GUESTCUBE's selftest publishes them)
  auto cubes = std::make_shared<guest_env_cube::Cubes>();
  cubes->eyes = FakeCube(guest_env_cube::kEyesName, 0x11), cubes->plain = FakeCube(guest_env_cube::kPlainName, 0x22);
  guest_env_cube::Publish(cubes);
  const assets::Model trans_eyes = GuestPart("ych_trans", "cubemap_eyes");
  const int te = EnvOf(trans_eyes);
  Check(CubeName(trans_eyes) == "CUBEMAP_eyes" && te >= 0 && trans_eyes.textures[size_t(te)].data == cubes->eyes.data && GuestSlot10(trans_eyes) == 1000 + te, "eyecube.trans_naming_cubemap_eyes",
        "a yCh_trans part whose record names cubemap_eyes (stock ch101 / ch136): CUBEMAP_eyes (got " + CubeName(trans_eyes) + ")");
  const assets::Model reflect_plain = GuestPart("ych_reflect", "cubemap");
  Check(CubeName(reflect_plain) == "CUBEMAP", "eyecube.reflect_naming_cubemap", "a yCh_Reflect part whose record names cubemap (stock ch210): CUBEMAP (got " + CubeName(reflect_plain) + ")");
  const assets::Model miss = GuestPart("ych_trans", "cube_light");
  Check(!miss.materials.empty() && EnvOf(miss) == -1 && GuestSlot10(miss) == kBlackCube && miss.textures.empty(), "eyecube.miss_binds_black",
        "a name no published cube has (the game's {0, 0}): the black cube (got " + CubeName(miss) + ")");
  const assets::Model old_eye = GuestPart("ych_reflect"), old_trans = GuestPart("ych_trans");
  Check(CubeName(old_eye) == "CUBEMAP_eyes" && CubeName(old_trans) == "CUBEMAP", "eyecube.old_record_keeps_effect_pick", "no section 72 / 73: the pick by effect, as before");
  guest_env_cube::Publish(nullptr);
}

int Selftest() {
  // ---- the role of slot 10 on every chain BindMaterial takes
  Check(RoleOfSlot(10) == CharSlot::EnvCube && RoleOfWetSlot(10) == CharSlot::EnvCube && RoleOfChangeSlot(10) == CharSlot::EnvCube, "role.slot10_env_cube",
        "dry, wet and change-colour chains");
  Check(muscle_map::Role(10, false, false, true) == CharSlot::EnvCube && muscle_map::Role(10, false, true, false) == CharSlot::EnvCube &&
            muscle_map::Role(10, true, false, false) == CharSlot::EnvCube,
        "role.through_muscle_map", "BindMaterial's switch (muscle_map::Role) hands 10 to the chain");

  // ---- every character fragment of the registry that samples slot 10
  std::unordered_map<std::string, const fxt::VariantTable*> named;
  for (const fxt::VariantTable& t : fxt::kTables) named.emplace(t.variant, &t);
  int readers = 0, no_env = 0, unnamed = 0, change = 0;
  for (const rdr::fx::Variant& v : rdr::fx::kVariants) {
    if (!v.name || std::strncmp(v.name, "yCh_", 4) != 0) continue;
    const size_t n = std::strlen(v.name);
    if (n < 9 || std::strcmp(v.name + n - 9, "_fragment") != 0) continue;
    bool r10 = false;
    for (uint32_t s = 0; s < v.sampler_count; ++s) r10 = r10 || v.samplers[s].slot == env_cube::kSlot;
    if (!r10) continue;
    ++readers;
    const auto it = named.find(v.name);
    if (it == named.end()) { ++unnamed; continue; }   // a registry twin with no define table of its own (musclemap_unit skips these too); its base variant is counted
    no_env += muscle_map::HasDefine(it->second->defines, "FX_ENV_LIGHT") ? 0 : 1;
    change += muscle_map::HasDefine(it->second->defines, "FX_CHANGE_COLOR") ? 1 : 0;
  }
  Check(readers - unnamed > 0 && no_env == 0, "registry.slot10_readers_define_env_light",
        std::to_string(readers) + " yCh fragments sample 10 (" + std::to_string(unnamed) + " twins without a define table), " + std::to_string(no_env) + " of the rest without FX_ENV_LIGHT");
  Check(change == 0, "registry.no_change_colour_reader", "slot 10 has one meaning: no change-colour fragment samples it");

  // ---- CubeAt: only a six-face image reaches the cube slot
  const std::vector<int> images = {11, 12, kNone, 14};
  const std::vector<Info> info = {{1}, {6}, {6}, {6}};
  Check(env_cube::CubeAt(images, info, 1, kNone) == 12, "cube.six_face_image", "index 1: a cube that uploaded");
  Check(env_cube::CubeAt(images, info, 0, kNone) == kNone, "cube.never_a_2d_image", "index 0: a 2D texture");
  Check(env_cube::CubeAt(images, info, 2, kNone) == kNone, "cube.upload_failed_none", "index 2: no image (reflections off / refused)");
  Check(env_cube::CubeAt(images, info, -1, kNone) == kNone && env_cube::CubeAt(images, info, 4, kNone) == kNone, "cube.out_of_range_none", "-1 and past the end");
  const std::vector<Info> short_info = {{1}, {6}};
  Check(env_cube::CubeAt(images, short_info, 3, kNone) == kNone, "cube.no_info_none", "an image with no info row");

  // ---- the image slot 10 takes
  Check(Slot10(RoleOfSlot(10), 1, images, info) == 12 && Slot10(RoleOfWetSlot(10), 1, images, info) == 12, "bind.env_draw_takes_material_cube", "dry and wet");
  Check(Slot10(RoleOfSlot(10), -1, images, info) == kBlackCube, "bind.no_env_cube_black", "a material naming none: the black cube (today's look)");
  Check(Slot10(RoleOfSlot(10), 0, images, info) == kBlackCube, "bind.2d_texture_black", "a 2D picture named there is never bound as the cube");
  assets::Material dflt;
  Check(dflt.env_cube == -1, "material.default_none", "Material::env_cube = -1 until the loader reads g_mEnvLPrm");

  // ---- the planted twin: the old chain binds the black cube at an env draw's slot 10
  const int twin = OldSlot10();
  Check(twin != 12, "twin.old_chain_caught", "the old chain gives image " + std::to_string(twin) + " (the black cube), not the material's 12");

  // ---- S111 GUESTCUBE: created superstars' parts
  GuestChecks();
  // ---- S111 EYECUBE: the cube by the record's texture name
  EyeCubeChecks();

  std::printf("%s envcube_unit selftest: slot 10 of an FX_ENV_LIGHT draw = the material's g_mEnvLPrm cube\n", g_bad ? "FAIL" : "PASS");
  return g_bad ? 1 : 0;
}

// The player's files, read-only: ch100 attire 0 through the production loader.
int Real(const char* game) {
  launcher::GameFiles files;
  files.SetRoot(std::filesystem::path(game));
  assets::Model m;
  std::string err;
  const bool ok = assets::LoadWrestler(files, 100, 0, m, err);
  Check(ok && !m.materials.empty(), "real.load_ch100", ok ? std::to_string(m.materials.size()) + " materials" : err);
  std::vector<int> images;
  std::vector<Info> info;
  for (size_t i = 0; i < m.textures.size(); ++i) images.push_back(1000 + int(i)), info.push_back({m.textures[i].faces});
  size_t chain = 0;
  for (uint32_t k = 0; k < 8; ++k) chain += upload::UploadMipBytes(assets::TexFormat::kBC1, std::max(1u, 128u >> k), std::max(1u, 128u >> k));
  int reflect = 0, with = 0, bad = 0;
  for (const assets::Material& mt : m.materials) {
    if (mt.shader != "yCh_Reflect") continue;
    ++reflect;
    if (mt.env_cube < 0 || size_t(mt.env_cube) >= m.textures.size()) continue;
    ++with;
    const assets::Texture& t = m.textures[size_t(mt.env_cube)];
    upload::TexturePlan plan;
    const char* why = upload::TextureRefusal(t, plan);
    const std::string nm = Lower(t.name);
    const bool good = (nm == "cubemap_eyes" || nm == "cubemap") && t.faces == 6 && t.width == 128 && t.height == 128 && t.mips == 8 &&
                      t.format == assets::TexFormat::kBC1 && t.data.size() == 6 * chain && !why && plan.cube && plan.fit == 8 &&
                      Slot10(RoleOfSlot(10), mt.env_cube, images, info) == 1000 + mt.env_cube;
    bad += good ? 0 : 1;
    if (with <= 4 || !good)
      std::printf("      yCh_Reflect -> env cube '%s' faces %u %ux%u mips %u fmt %d bytes %zu (%s)\n", t.name.c_str(), t.faces, t.width, t.height, t.mips, int(t.format),
                  t.data.size(), why ? why : "uploads as a cube");
  }
  Check(reflect > 0 && with == reflect && bad == 0, "real.reflect_parts_bind_cubemap_eyes",
        std::to_string(with) + " of " + std::to_string(reflect) + " yCh_Reflect materials carry g_mEnvLPrm; " + std::to_string(bad) +
            " not a 6-face BC1 128x128 8-mip cube of " + std::to_string(6 * chain) + " bytes bound at slot 10");
  // ---- S111 GUESTCUBE: the cubes a created superstar's parts get (guest_env_cube_load.h LoadCubes over the same chCommon directories), and a guest eye part built with them
  guest_env_cube::Cubes cubes;
  std::string cwhy;
  const bool got = guest_env_cube::LoadCubes(files, cubes, cwhy);
  const auto shape = [&](const assets::Texture& c, const char* want) {
    upload::TexturePlan plan;
    const char* w = upload::TextureRefusal(c, plan);
    return Lower(c.name) == Lower(want) && c.faces == 6 && c.width == 128 && c.height == 128 && c.mips == 8 && c.format == assets::TexFormat::kBC1 && c.data.size() == 6 * chain && !w && plan.cube &&
           plan.fit == 8;
  };
  Check(got && shape(cubes.eyes, guest_env_cube::kEyesName) && shape(cubes.plain, guest_env_cube::kPlainName) && cubes.eyes.data != cubes.plain.data, "real.guest_cubes_from_chcommon",
        got ? "CUBEMAP_eyes and CUBEMAP: 6-face BC1 128x128 8-mip cubes, different texels" : cwhy);
  guest_env_cube::Publish(std::make_shared<const guest_env_cube::Cubes>(cubes));
  const assets::Model geye = GuestPart("ych_reflect");
  const int ge = EnvOf(geye);
  Check(ge >= 0 && geye.textures[size_t(ge)].data == cubes.eyes.data && GuestSlot10(geye) == 1000 + ge, "real.guest_reflect_binds_cubemap_eyes",
        "a created superstar's yCh_Reflect part: slot 10 = chCommon's own CUBEMAP_eyes (got " + CubeName(geye) + ")");
  const assets::Model gtrans = GuestPart("ych_trans", "cubemap_eyes");
  const int gt = EnvOf(gtrans);
  Check(gt >= 0 && gtrans.textures[size_t(gt)].data == cubes.eyes.data, "real.guest_trans_naming_cubemap_eyes", "S111 EYECUBE: a yCh_trans part naming cubemap_eyes gets chCommon's CUBEMAP_eyes (got " + CubeName(gtrans) + ")");
  guest_env_cube::Publish(nullptr);
  std::printf("%s envcube_unit real\n", g_bad ? "FAIL" : "PASS");
  return g_bad ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "real") {
    const char* game = nullptr;
    for (int i = 2; i + 1 < argc; ++i)
      if (std::string(argv[i]) == "--game") game = argv[i + 1];
    if (!game) {
      std::printf("usage: envcube_unit real --game DIR\n");
      return 2;
    }
    return Real(game);
  }
  if (argc >= 2 && std::string(argv[1]) == "plant") guestmodel::g_draw_plant = 25;   // S111 GUESTCUBE twin: a created superstar's slot 10 left black (the old chain); the selftest must FAIL
  if (argc >= 2 && std::string(argv[1]) == "plant26") guestmodel::g_draw_plant = 26;   // S111 EYECUBE twin: the pick by effect though the record names the cube (the old rule); the selftest must FAIL
  return Selftest();
}
