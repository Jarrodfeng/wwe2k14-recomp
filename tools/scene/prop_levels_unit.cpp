// prop_levels_unit: the offline proof of S111 lane PROPLEVELS (docs\research\s111\PROPLEVELS.md): the level count ours gives an entrance prop's and an m2 weapon / arena
// prop's textures (src\assets\prop_levels.h: the PACH's entry-100 table over entry 10, entry 20 at 3) through tex_dir_order.h Add (the event prop's reader), the real TexSet
// (src\assets\dds.h) and the upload rule (src\renderer_scene\game_levels.h Relevel). 0 game runs; every fixture is synthetic bytes of our own (nothing from the game).
//   prop_levels_unit [--plant file_chain|no_table|table_on_20|m2_loop|cube_refused]   a plant must make a check FAIL (exit 1)
//     file_chain   the old load: entries 10 / 20 added with TexSet::AddDir (no count: the file's own chain is uploaded)
//     no_table     the PACH's entry 100 not read (every entry-10 name at 3)
//     table_on_20  the table applied to entry 20 too (the game appends entry 20 with no per-name step)
//     m2_loop      LoadM2Weapon's body read with its own directory loop (tex.AddDir) in place of prop_levels.h AddWeaponDirs
//     cube_refused S111 CUBELEVELS: the old Relevel, which kept a cube at its file's chain (the game builds entry 20's cubes at 3, every face)
// PROPLEVELS review: the weapon checks run prop_levels.h AddWeaponDirs, the walk LoadM2Weapon calls; check m2.source reads LoadM2Weapon's body in src\assets\asset_load.h
// (LoadM2Weapon needs the player's m2.pac and a JBOY model, so the unit cannot call it whole): it must add its directories through AddWeaponDirs and nothing else.
// Build: pwsh -NoProfile -File tools\scene\build_prop_levels_unit.ps1
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "../../src/assets/tex_dir_order.h"
#include "../../src/renderer_scene/game_levels.h"
#include "../../src/renderer_scene/upload_check.h"   // S111 CUBELEVELS: MakeTexture's plan for the relevelled cube

using namespace assets;
namespace pl = assets::prop_levels;

static int g_fail = 0;
static std::string g_plant;
static void Check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) ++g_fail;
}
static void PutLe32(Bytes& b, size_t o, uint32_t v) { std::memcpy(b.data() + o, &v, 4); }

// A DXT1 DDS of w x w texels holding `mips` levels of zero blocks (plus slack: ParseDds keeps the header's whole levels).
static Bytes Dds(uint32_t w, uint32_t mips) {
  size_t n = 0;
  for (uint32_t l = 0; l < mips; ++l) {
    const uint32_t s = std::max(1u, w >> l);
    n += size_t((s + 3) / 4) * ((s + 3) / 4) * 8;
  }
  Bytes d(128 + n + 64, 0);
  std::memcpy(d.data(), "DDS ", 4);
  PutLe32(d, 4, 124);
  PutLe32(d, 8, 0x20000u);   // DDSD_MIPMAPCOUNT
  PutLe32(d, 12, w);
  PutLe32(d, 16, w);
  PutLe32(d, 28, mips);
  PutLe32(d, 84, 0x31545844u);   // DXT1
  return d;
}
// S111 CUBELEVELS: a DXT1 cube DDS of w x w texels (caps2 0xFE00: the six faces back to back, each `mips` levels, as dds.h ParseDds reads them); face f's level l is the
// BC1 encoding of a pattern of its own, so the faces differ.
static Bytes CubeDds(uint32_t w, uint32_t mips) {
  Bytes d = Dds(w, mips);
  d.resize(128);
  PutLe32(d, 112, 0xFE00u);
  for (uint32_t f = 0; f < 6; ++f)
    for (uint32_t l = 0; l < mips; ++l) {
      const uint32_t s = std::max(1u, w >> l);
      std::vector<uint8_t> px(size_t(s) * s * 4);
      for (uint32_t y = 0; y < s; ++y)
        for (uint32_t x = 0; x < s; ++x) {
          uint8_t* p = &px[(size_t(y) * s + x) * 4];
          p[0] = uint8_t(40 * f + 3 * x), p[1] = uint8_t(255 - 30 * f - 3 * y), p[2] = uint8_t(20 * l + 4 * (x ^ y)), p[3] = 255;
        }
      const size_t at = d.size();
      d.resize(at + rdr_scene::game_levels::LevelBytes(TexFormat::kBC1, s, s));
      rdr_scene::game_levels::detail::Encode(TexFormat::kBC1, px, s, s, d.data() + at);
    }
  return d;
}
struct Tex {
  const char* name;
  uint32_t w, mips;
  bool cube = false;   // S111 CUBELEVELS: CubeDds
};
// A texture directory (dds.h TexSet::AddDir's layout): count, 12 header bytes, count x {name[16], "dds", size, offset}, then the DDS blobs.
static Bytes TexDir(const std::vector<Tex>& tex) {
  Bytes d(0x10 + 32 * tex.size(), 0);
  PutLe32(d, 0, uint32_t(tex.size()));
  for (size_t i = 0; i < tex.size(); ++i) {
    const Bytes blob = tex[i].cube ? CubeDds(tex[i].w, tex[i].mips) : Dds(tex[i].w, tex[i].mips);
    const size_t r = 0x10 + 32 * i;
    std::memcpy(d.data() + r, tex[i].name, std::min<size_t>(16, std::strlen(tex[i].name)));
    std::memcpy(d.data() + r + 16, "dds", 3);
    PutLe32(d, r + 20, uint32_t(blob.size()));
    PutLe32(d, r + 24, uint32_t(d.size()));
    d.insert(d.end(), blob.begin(), blob.end());
  }
  return d;
}
struct Fixture {
  Bytes buf;
  pac::Pach h;
};
static Fixture MakePach(const std::vector<std::pair<uint32_t, Bytes>>& ents) {
  Fixture f;
  for (const auto& [id, b] : ents) {
    pac::Ent e;
    e.id = id, e.off = f.buf.size(), e.size = b.size();
    f.buf.insert(f.buf.end(), b.begin(), b.end());
    f.h.ents.push_back(e);
  }
  f.h.end = f.buf.size();
  return f;
}

// An entry-100 text in the 50010 layout (a byte order mark, tabs, CR LF): our own names.
static const char kXml[] =
    "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\t<MipmapData NumOfData=\"4\">\t<MipmapDataAttribute><TextureName>dif_a</TextureName>"
    "<MipmapLevel>1</MipmapLevel></MipmapDataAttribute>\r\n<MipmapDataAttribute><TextureName>dif_b</TextureName><MipmapLevel>0</MipmapLevel></MipmapDataAttribute>\r\n"
    "<MipmapDataAttribute><TextureName>cube_ref</TextureName><MipmapLevel>2</MipmapLevel></MipmapDataAttribute>\r\n"
    "<MipmapDataAttribute><TextureName>w_two</TextureName><MipmapLevel>2</MipmapLevel></MipmapDataAttribute></MipmapData>";
static Bytes XmlBytes() { return Bytes(kXml, kXml + std::strlen(kXml)); }

// The PACH of `f` as the plant reads it (no_table: without entry 100).
static pac::Pach PachOf(const Fixture& f) {
  pac::Pach h = f.h;
  if (g_plant == "no_table") h.ents.erase(std::remove_if(h.ents.begin(), h.ents.end(), [](const pac::Ent& e) { return e.id == pl::kPropTableEntry; }), h.ents.end());
  return h;
}
// One directory as the loaders add it; the plants are the wrong rules a check must catch.
static void AddOne(TexSet& tex, const Fixture& f, const pac::Ent& e, const arena_levels::Rule& r) {
  Bytes d = pac::Decode(f.buf.data() + e.off, e.size);
  if (g_plant == "file_chain") {
    tex.AddDir(std::move(d));
    return;
  }
  if (g_plant == "table_on_20" && e.id == pl::kListEntry) {
    tex.AddDirNamed(std::move(d), r.Fn());
    return;
  }
  pl::AddDir(tex, std::move(d), e.id, r);
}
// An entrance prop: tex_dir_order::Add (the reader of LoadPropEntry / LoadProp through AddTexDirs); a plant walks the same 10, 20 itself.
static void BuildEvent(const Fixture& f, TexSet& tex) {
  const pac::Pach h = PachOf(f);
  if (g_plant.empty() || g_plant == "no_table") {
    tex_dir_order::Add(tex, f.buf, h, tex_dir_order::Owner::kEventProp);
    return;
  }
  const arena_levels::Rule r = pl::TableRule(f.buf, h);
  for (const uint32_t k : tex_dir_order::Keys(tex_dir_order::Owner::kEventProp))
    if (const pac::Ent* e = pac::Find(h, k)) AddOne(tex, f, *e, r);
}
// An m2 weapon / arena prop: LoadM2Weapon's walk, prop_levels.h AddWeaponDirs (the leaf's entries 10..19 and 20, in stored order); a plant walks the same entries itself.
static void BuildWeapon(const Fixture& f, TexSet& tex) {
  const pac::Pach h = PachOf(f);
  if (g_plant.empty() || g_plant == "no_table" || g_plant == "m2_loop") {
    pl::AddWeaponDirs(tex, f.buf, h);
    return;
  }
  const arena_levels::Rule r = pl::TableRule(f.buf, h);
  for (const pac::Ent& e : h.ents)
    if (((e.id >= 10 && e.id <= 19) || e.id == 20) && e.size >= 0x30) AddOne(tex, f, e, r);
}
static uint32_t CountOf(const TexSet& ts, const char* name) {
  Texture t;
  std::string why;
  return ts.Peek(name, t, why) ? t.game_levels : 9999u;
}
// The levels the upload makes of `name` (Relevel's copy, else the file's).
static uint32_t Uploaded(const TexSet& ts, const char* name) {
  Texture t, out;
  std::string why;
  if (!ts.Peek(name, t, why)) return 9999u;
  return rdr_scene::game_levels::Relevel(t, out) ? out.mips : t.mips;
}
static std::string Num(uint32_t v) { return std::to_string(v); }

// S111 CUBELEVELS: the texture the upload makes of `name` (Relevel's copy, else the file's, which lands in `file`); the plant cube_refused = the old Relevel (a cube kept its file's chain).
static Texture UploadedTex(const TexSet& ts, const char* name, Texture& file) {
  Texture out;
  std::string why;
  if (!ts.Peek(name, file, why)) return Texture{};
  if (g_plant == "cube_refused" && file.faces != 1) return file;
  return rdr_scene::game_levels::Relevel(file, out) ? out : file;
}
// Bytes of the first `levels` levels of a w x w DXT1 chain (= the offset of level `levels`).
static size_t Bc1Chain(uint32_t w, uint32_t levels) {
  size_t n = 0;
  for (uint32_t l = 0; l < levels; ++l) n += rdr_scene::game_levels::LevelBytes(TexFormat::kBC1, std::max(1u, w >> l), std::max(1u, w >> l));
  return n;
}
// Every face of `up` holds `file`'s levels of that face that it kept and, past the file's `file_levels`, the BC1 encoding of the box of its own level before.
static bool CubeFacesOk(const Texture& file, const Texture& up, uint32_t file_levels) {
  namespace gl = rdr_scene::game_levels;
  if (file.faces != 6 || up.faces != 6 || up.mips == 0 || file.width != up.width) return false;
  const uint32_t w = file.width, kept = std::min(file_levels, up.mips);
  const size_t up_face = Bc1Chain(w, up.mips), file_face = Bc1Chain(w, file_levels);
  if (up.data.size() != 6 * up_face || file.data.size() < 6 * file_face) return false;
  for (uint32_t f = 0; f < 6; ++f) {
    const uint8_t* u = up.data.data() + f * up_face;
    if (std::memcmp(u, file.data.data() + f * file_face, Bc1Chain(w, kept)) != 0) return false;
    for (uint32_t l = kept; l < up.mips; ++l) {
      const uint32_t ps = std::max(1u, w >> (l - 1)), ns = std::max(1u, w >> l);
      std::vector<uint8_t> prev, box, want(gl::LevelBytes(TexFormat::kBC1, ns, ns));
      gl::detail::Decode(TexFormat::kBC1, u + Bc1Chain(w, l - 1), ps, ps, prev);
      gl::detail::Box(prev, ps, ps, box);
      gl::detail::Encode(TexFormat::kBC1, box, ns, ns, want.data());
      if (std::memcmp(u + Bc1Chain(w, l), want.data(), want.size()) != 0) return false;
    }
  }
  return true;
}

// LoadM2Weapon's body in src\assets\asset_load.h (from its definition to the first line that is a lone closing brace); empty when the file or the function is not found.
static std::string M2WeaponBody() {
  const std::filesystem::path p = std::filesystem::path(__FILE__).parent_path() / ".." / ".." / "src" / "assets" / "asset_load.h";
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  std::string s = ss.str();
  s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
  const size_t b = s.find("inline bool LoadM2Weapon(");
  if (b == std::string::npos) return {};
  const size_t e = s.find("\n}\n", b);
  return e == std::string::npos ? std::string() : s.substr(b, e - b);
}
static size_t Count(const std::string& s, const std::string& w) {
  size_t n = 0;
  for (size_t at = s.find(w); at != std::string::npos; at = s.find(w, at + w.size())) ++n;
  return n;
}

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string(argv[i]) == "--plant") g_plant = argv[i + 1];
  const uint32_t kWhole = level_tag::kWholeChainLevels;

  const Fixture ev = MakePach({{10, TexDir({{"dif_a", 64, 7}, {"dif_b", 64, 1}, {"other", 64, 7}})},
                               {20, TexDir({{"cube_ref", 16, 5}, {"extra", 16, 5}, {"cube_one", 16, 1, true}, {"cube_big", 16, 5, true}})},
                               {pl::kPropTableEntry, XmlBytes()}});
  {  // an entrance prop (MakeWeapon)
    TexSet tex;
    BuildEvent(ev, tex);
    Check(CountOf(tex, "dif_a") == 1 && Uploaded(tex, "dif_a") == 1, "event.dif_a: the entry-100 table's 1: one level (count " + Num(CountOf(tex, "dif_a")) + ")");
    Check(CountOf(tex, "dif_b") == kWhole && Uploaded(tex, "dif_b") == 7,
          "event.dif_b: table 0 = the whole chain: a 1-level 64 x 64 file grows to 7 (" + Num(Uploaded(tex, "dif_b")) + ")");
    Check(CountOf(tex, "other") == 3 && Uploaded(tex, "other") == 3, "event.other: not in the table: 3 (" + Num(Uploaded(tex, "other")) + ")");
    Check(CountOf(tex, "cube_ref") == 3 && CountOf(tex, "extra") == 3 && Uploaded(tex, "extra") == 3,
          "event.20: entry 20 at 3, the table's row for one of its names not applied (cube_ref " + Num(CountOf(tex, "cube_ref")) + ")");
    // S111 CUBELEVELS: entry 20 is a cube list (sub_8272D710 +0x4A8 kind 1) built at the 3; the cube create takes the count word as the 2D one does, every face alike
    Texture f1, f5;
    const Texture one = UploadedTex(tex, "cube_one", f1), big = UploadedTex(tex, "cube_big", f5);
    Check(f1.faces == 6 && one.mips == 3 && CubeFacesOk(f1, one, 1),
          "event.cube_grow: a 1-level 16 x 16 cube at 3: six faces of 3 levels, each face's level 0 kept, levels 1 and 2 its own box (" + Num(one.mips) + " levels)");
    Check(f5.faces == 6 && big.mips == 3 && CubeFacesOk(f5, big, 5),
          "event.cube_cut: a 5-level cube at 3: each face's first 3 levels, taken from that face (" + Num(big.mips) + " levels)");
    rdr_scene::upload::TexturePlan plan;
    const char* refused = rdr_scene::upload::TextureRefusal(one, plan);
    Check(refused == nullptr && plan.cube && plan.fit == 3 && plan.total == Bc1Chain(16, 3),
          std::string("event.cube_upload: MakeTexture's plan takes the relevelled cube whole (six faces of 3 levels)") + (refused ? std::string(": ") + refused : std::string()));
  }
  const Fixture wp = MakePach({{10, TexDir({{"w_one", 64, 7}, {"w_two", 64, 7}})},
                               {11, TexDir({{"w_eleven", 32, 6}})},
                               {20, TexDir({{"w_twenty", 32, 6}})},
                               {pl::kPropTableEntry, XmlBytes()}});
  {  // an m2 weapon / arena prop (SetWeaponData's table, the set builder)
    TexSet tex;
    BuildWeapon(wp, tex);
    Check(CountOf(tex, "w_two") == 2 && Uploaded(tex, "w_two") == 2, "weapon.w_two: the table's 2 (" + Num(Uploaded(tex, "w_two")) + ")");
    Check(CountOf(tex, "w_one") == 3 && Uploaded(tex, "w_one") == 3, "weapon.w_one: not in the table: 3");
    Check(CountOf(tex, "w_twenty") == 3, "weapon.20: entry 20 at 3");
    Check(CountOf(tex, "w_eleven") == 0 && Uploaded(tex, "w_eleven") == 6, "weapon.11: an entry the game's builder does not read keeps the file's chain (count 0)");
  }
  {  // LoadM2Weapon adds its texture directories through AddWeaponDirs (the walk the weapon checks above run) and through nothing else
    std::string body = M2WeaponBody();
    if (g_plant == "m2_loop") {   // the landed loop: `for (...) if (...) tex.AddDir(...)` in the body itself
      const size_t at = body.find("prop_levels::AddWeaponDirs(");
      if (at != std::string::npos) body.replace(at, std::strlen("prop_levels::AddWeaponDirs("), "tex.AddDir(");
    }
    const size_t walks = Count(body, "prop_levels::AddWeaponDirs(tex, nb, np, tex_first)");
    const size_t others = Count(body, "AddDir(") + Count(body, "AddDirLevels(") + Count(body, "AddDirNamed(") + Count(body, "AddTexDirs(");
    Check(!body.empty() && walks == 1 && others == 0,
          "m2.source: LoadM2Weapon calls prop_levels::AddWeaponDirs(tex, nb, np, tex_first) once and adds no directory itself (" + Num(uint32_t(walks)) + " calls, " +
              Num(uint32_t(others)) + " other adds" + (body.empty() ? ", body not found" : "") + ")");
  }
  {  // a character's directories: an entry 100 is not its table
    const Fixture ch = MakePach({{10, TexDir({{"dif_a", 64, 7}})}, {pl::kPropTableEntry, XmlBytes()}});
    TexSet tex;
    tex_dir_order::Add(tex, ch.buf, ch.h, tex_dir_order::Owner::kCharacter);
    Check(CountOf(tex, "dif_a") == level_tag::kStockTexLevels, "character: the load's 4, the entry-100 table not applied (" + Num(CountOf(tex, "dif_a")) + ")");
  }
  {  // a prop PACH without entry 100
    const Fixture nt = MakePach({{10, TexDir({{"dif_a", 64, 7}})}, {20, TexDir({{"extra", 16, 5}})}});
    TexSet tex;
    tex_dir_order::Add(tex, nt.buf, nt.h, tex_dir_order::Owner::kEventProp);
    Check(CountOf(tex, "dif_a") == 3 && CountOf(tex, "extra") == 3, "event_no100: every name at 3");
  }
  std::printf("%s: %d failed\n", g_plant.empty() ? "prop_levels_unit" : ("prop_levels_unit --plant " + g_plant).c_str(), g_fail);
  return g_fail ? 1 : 0;
}
