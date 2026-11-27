// chcommon_levels_unit: the offline proof of S111 lane CHCOMMONCUBE (docs\research\s111\CHCOMMONCUBE.md): the level count ours gives chCommon.pac's texture directories
// (src\assets\chcommon_levels.h AddDir: entries 20 and 21 at the common load's 4, any other entry the file's chain) through the real loader over a synthetic game folder
// (src\assets\asset_load.h load_detail::LoadCommonTexDirs / AddCommonTexDirs, src\renderer_scene\guest_env_cube_load.h LoadCubes) and the upload rule
// (src\renderer_scene\game_levels.h Relevel, a cube face by face since S111 CUBELEVELS). 0 game runs; every fixture is synthetic bytes of our own (nothing from the game).
//   chcommon_levels_unit [--plant file_chain|no_cube]   a plant must make a check FAIL (exit 1)
//     file_chain   the old load: every chCommon directory added with TexSet::AddDir (no count: the files' whole chains are uploaded)
//     no_cube      entry 21 (the cube list) added without its count, entry 20 at 4
// Check source.wrestler reads LoadWrestlerPac's body in src\assets\asset_load.h (it needs a whole character pac, so the unit cannot call it): it must add chCommon's
// directories through AddCommonTexDirs and read CommonTexDirs nowhere else.
// Build: pwsh -NoProfile -File tools\scene\build_chcommon_levels_unit.ps1
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

#include "../../src/renderer_scene/guest_env_cube_load.h"
#include "../../src/renderer_scene/game_levels.h"
#include "../../src/renderer_scene/upload_check.h"

using namespace assets;
namespace fs = std::filesystem;
namespace gl = rdr_scene::game_levels;
namespace gec = rdr_scene::guest_env_cube;

static int g_fail = 0;
static std::string g_plant;
static void Check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) ++g_fail;
}
static void PutLe32(Bytes& b, size_t o, uint32_t v) { std::memcpy(b.data() + o, &v, 4); }
static std::string Num(uint32_t v) { return std::to_string(v); }

// Bytes of the first `levels` levels of a w x w DXT1 chain.
static size_t Bc1Chain(uint32_t w, uint32_t levels) {
  size_t n = 0;
  for (uint32_t l = 0; l < levels; ++l) n += gl::LevelBytes(TexFormat::kBC1, std::max(1u, w >> l), std::max(1u, w >> l));
  return n;
}
// A DXT1 DDS of w x w texels holding `mips` levels of zero blocks.
static Bytes Dds(uint32_t w, uint32_t mips) {
  Bytes d(128 + Bc1Chain(w, mips), 0);
  std::memcpy(d.data(), "DDS ", 4);
  PutLe32(d, 4, 124);
  PutLe32(d, 8, 0x20000u);   // DDSD_MIPMAPCOUNT
  PutLe32(d, 12, w);
  PutLe32(d, 16, w);
  PutLe32(d, 28, mips);
  PutLe32(d, 84, 0x31545844u);   // DXT1
  return d;
}
// A DXT1 cube DDS of w x w texels (caps2 0xFE00: the six faces back to back, each `mips` levels, as dds.h ParseDds reads them); every face and level its own pattern.
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
      d.resize(at + gl::LevelBytes(TexFormat::kBC1, s, s));
      gl::detail::Encode(TexFormat::kBC1, px, s, s, d.data() + at);
    }
  return d;
}
struct Tex {
  const char* name;
  uint32_t w, mips;
  bool cube = false;
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
// A pac file with no EPAC header: one PACH at offset 0 holding `ents` (pac.h OpenBytes reads a PACH at 0 when the file is not headed).
static Bytes PacFile(const std::vector<std::pair<uint32_t, Bytes>>& ents) {
  Bytes f(8 + 12 * ents.size(), 0);
  std::memcpy(f.data(), "PACH", 4);
  PutLe32(f, 4, uint32_t(ents.size()));
  size_t at = 0;
  for (size_t i = 0; i < ents.size(); ++i) {
    PutLe32(f, 8 + 12 * i, ents[i].first);
    PutLe32(f, 8 + 12 * i + 4, uint32_t(at));
    PutLe32(f, 8 + 12 * i + 8, uint32_t(ents[i].second.size()));
    at += ents[i].second.size();
  }
  for (const auto& e : ents) f.insert(f.end(), e.second.begin(), e.second.end());
  return f;
}

// chCommon's directories into `tex` as the loader adds them; the plants are the old rules a check must catch.
static void AddCommon(TexSet& tex) {
  if (g_plant.empty()) {
    load_detail::AddCommonTexDirs(tex);
    return;
  }
  for (const load_detail::CommonTexDir& c : load_detail::CommonTexDirs()) {
    if (g_plant == "file_chain" || (g_plant == "no_cube" && c.key == chcommon_levels::kCubeEntry)) tex.AddDir(Bytes(c.dir));
    else chcommon_levels::AddDir(tex, Bytes(c.dir), c.key);
  }
}
// The guest cubes: the production LoadCubes; a plant Peeks them from its own set (the same names, the same directories).
static bool ReadCubes(launcher::GameFiles& files, gec::Cubes& out, std::string& why) {
  if (g_plant.empty()) return gec::LoadCubes(files, out, why);
  load_detail::LoadCommonTexDirs(files);
  TexSet set;
  AddCommon(set);
  return set.Peek(gec::kEyesName, out.eyes, why) && set.Peek(gec::kPlainName, out.plain, why);
}
static uint32_t CountOf(const TexSet& ts, const char* name) {
  Texture t;
  std::string why;
  return ts.Peek(name, t, why) ? t.game_levels : 9999u;
}
// The texture the upload makes of `t` (Relevel's copy, else the file's).
static Texture Upload(const Texture& t) {
  Texture out;
  return gl::Relevel(t, out) ? out : t;
}
static uint32_t UploadedMips(const TexSet& ts, const char* name) {
  Texture t;
  std::string why;
  return ts.Peek(name, t, why) ? Upload(t).mips : 9999u;
}
// `up` is `file` (a w x w DXT1 cube of `file_levels` levels a face) cut to `want` levels: six faces, each that face's own first `want` levels.
static bool FacesCut(const Texture& file, const Texture& up, uint32_t file_levels, uint32_t want) {
  if (file.faces != 6 || up.faces != 6 || up.mips != want || file.width != up.width || want > file_levels) return false;
  const uint32_t w = file.width;
  const size_t up_face = Bc1Chain(w, want), file_face = Bc1Chain(w, file_levels);
  if (up.data.size() != 6 * up_face || file.data.size() < 6 * file_face) return false;
  for (uint32_t f = 0; f < 6; ++f)
    if (std::memcmp(up.data.data() + f * up_face, file.data.data() + f * file_face, up_face) != 0) return false;
  return true;
}

// LoadWrestlerPac's body in src\assets\asset_load.h (from its definition's second line to the first lone closing brace); empty when not found.
static std::string WrestlerPacBody() {
  const fs::path p = fs::path(__FILE__).parent_path() / ".." / ".." / "src" / "assets" / "asset_load.h";
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  std::string s = ss.str();
  s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
  const size_t b = s.find("bool prop_slots, bool character, uint32_t char_levels) {");
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
  const uint32_t kFour = chcommon_levels::kListLevels;
  Check(kFour == 4 && chcommon_levels::kDetailEntry == 20 && chcommon_levels::kCubeEntry == 21, "rule: entries 20 / 21 at 4 (sub_8271CB50 +0x48)");

  // A synthetic game folder whose pac\ch\chCommon.pac holds chCommon's entry layout with our own textures: 0 (a model slot), 10 / 12 (2D lists built at the count in
  // force), 11 (not a directory), 20 (2D at 4), 21 (the cube list at 4).
  const fs::path root = fs::temp_directory_path() / ("chcommon_levels_unit_root_" + (g_plant.empty() ? std::string("clean") : g_plant));   // one folder per run: the proof runs them side by side
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "pac" / "ch");
  {
    const Bytes pac = PacFile({{0, Bytes(64, 0)},
                               {10, TexDir({{"decal_a", 64, 1}})},
                               {11, Bytes(64, 'X')},
                               {12, TexDir({{"drop_a", 64, 7}})},
                               {20, TexDir({{"detail_a", 64, 7}, {"bump_a", 32, 4}})},
                               {21, TexDir({{gec::kPlainName, 16, 5, true}, {gec::kEyesName, 16, 5, true}})}});
    std::ofstream out(root / "pac" / "ch" / "chCommon.pac", std::ios::binary);
    out.write(reinterpret_cast<const char*>(pac.data()), std::streamsize(pac.size()));
  }
  launcher::GameFiles files;
  files.SetRoot(root);

  gec::Cubes cubes;
  std::string why;
  const bool got = ReadCubes(files, cubes, why);   // the first read: LoadCommonTexDirs runs once a process, on the synthetic folder
  Check(got && load_detail::CommonTexError().empty() && load_detail::CommonTexDirs().size() == 5,
        "load: chCommon's entries 10..29 read with their ids (" + Num(uint32_t(load_detail::CommonTexDirs().size())) + " kept" + (why.empty() ? "" : ", " + why) + ")");

  TexSet tex;   // the wrestler fallback's set
  AddCommon(tex);
  Check(CountOf(tex, "detail_a") == kFour && UploadedMips(tex, "detail_a") == 4,
        "entry20.cut: a 7-level 64 x 64 file at the common load's 4 (count " + Num(CountOf(tex, "detail_a")) + ", uploaded " + Num(UploadedMips(tex, "detail_a")) + ")");
  Check(CountOf(tex, "bump_a") == kFour && UploadedMips(tex, "bump_a") == 4, "entry20.same: a 4-level file at 4 uploads as it is");
  Check(CountOf(tex, "decal_a") == 0 && UploadedMips(tex, "decal_a") == 1 && CountOf(tex, "drop_a") == 0 && UploadedMips(tex, "drop_a") == 7,
        "entries10_12: no count (built at the count in force, not read): the files' chains (decal " + Num(UploadedMips(tex, "decal_a")) + ", drop " +
            Num(UploadedMips(tex, "drop_a")) + ")");
  {
    Texture file;
    std::string w;
    const bool ok = tex.Peek(gec::kEyesName, file, w);
    const Texture up = Upload(file);
    Check(ok && file.game_levels == kFour && FacesCut(file, up, 5, 4),
          "entry21.fallback: the wrestler set's cube at 4, six faces of 4 levels, each its own face's (" + Num(up.mips) + " levels)");
  }
  for (const Texture* c : {&cubes.eyes, &cubes.plain}) {
    const Texture up = Upload(*c);
    Check(got && c->game_levels == kFour && FacesCut(*c, up, 5, 4),
          "guest." + c->name + ": LoadCubes' cube carries 4 and uploads six faces of 4 levels (count " + Num(c->game_levels) + ", " + Num(up.mips) + " levels)");
    rdr_scene::upload::TexturePlan plan;
    const char* refused = rdr_scene::upload::TextureRefusal(up, plan);
    Check(refused == nullptr && plan.cube && plan.fit == 4 && plan.total == Bc1Chain(16, 4),
          "guest.upload." + c->name + ": MakeTexture's plan takes the relevelled cube whole" + (refused ? std::string(": ") + refused : std::string()));
  }
  {  // LoadWrestlerPac adds chCommon's directories through AddCommonTexDirs and reads CommonTexDirs nowhere else
    const std::string body = WrestlerPacBody();
    const size_t calls = Count(body, "AddCommonTexDirs(tex);"), raw = Count(body, "CommonTexDirs()");
    Check(!body.empty() && calls == 1 && raw == 0,
          "source.wrestler: LoadWrestlerPac calls AddCommonTexDirs(tex) once and reads CommonTexDirs() nowhere (" + Num(uint32_t(calls)) + " calls, " + Num(uint32_t(raw)) +
              " raw reads" + (body.empty() ? ", body not found" : "") + ")");
  }
  fs::remove_all(root, ec);
  std::printf("%s: %d failed\n", g_plant.empty() ? "chcommon_levels_unit" : ("chcommon_levels_unit --plant " + g_plant).c_str(), g_fail);
  return g_fail ? 1 : 0;
}
