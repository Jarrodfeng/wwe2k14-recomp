// stage_levels_unit: the offline proof of S111 lane STAGELEVELS (docs\research\s111\STAGELEVELS.md): the texture set and level counts ours gives a bgEtc stage structure
// (src\assets\stage_levels.h: key 0 at the holder's count, key 10 at the whole chain, no other key) through the real TexSet (src\assets\dds.h) and the upload rule
// (src\renderer_scene\game_levels.h Relevel). 0 game runs; every fixture is synthetic bytes of our own (nothing from the game).
//   stage_levels_unit [--plant file_chain|flag_pick|no_growth|reads_11_19|stage_loop|cube_at_list]   a plant must make a check FAIL (exit 1)
//     file_chain   the old load: entries 0 and 10..19 added with TexSet::AddDir (no count: the file's own chain is uploaded)
//     flag_pick    the [H+180] test read the other way round (3 when the flag is clear, the whole chain when it is set)
//     no_growth    the whole chain taken as "no count" (the file's chain kept, so a 1-level file is not grown)
//     reads_11_19  entries 11..19 added as well (the game's builder reads keys 0 and 10 only)
//     stage_loop   LoadStageEntry's body read with its own directory loop (tex.AddDir) in place of stage_levels.h AddStructDirs
//     cube_at_list key 10 built at key 0's count (the game sets the count word to 0 before the cube list, sub_82577300 +0x218)
// Check source.* reads LoadStageEntry's and LoadGmEntry's bodies in src\assets\asset_load.h (they need the player's bgEtc.pac and a JBOY model, so the unit cannot call them
// whole): each must add the stage set through AddStructDirs.
// Build: pwsh -NoProfile -File tools\scene\build_stage_levels_unit.ps1
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

#include "../../src/assets/stage_levels.h"
#include "../../src/renderer_scene/game_levels.h"

using namespace assets;
namespace sl = assets::stage_levels;

static int g_fail = 0;
static std::string g_plant;
static void Check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) ++g_fail;
}
static void PutLe32(Bytes& b, size_t o, uint32_t v) { std::memcpy(b.data() + o, &v, 4); }
static std::string Num(uint32_t v) { return std::to_string(v); }

// A DXT1 DDS of w x w texels whose `mips` levels are each the BC1 encoding of a pattern of its own (`seed` makes files and cube faces differ).
static Bytes Levels(uint32_t w, uint32_t mips, uint32_t seed) {
  Bytes d;
  for (uint32_t l = 0; l < mips; ++l) {
    const uint32_t s = std::max(1u, w >> l);
    std::vector<uint8_t> px(size_t(s) * s * 4);
    for (uint32_t y = 0; y < s; ++y)
      for (uint32_t x = 0; x < s; ++x) {
        uint8_t* p = &px[(size_t(y) * s + x) * 4];
        p[0] = uint8_t(40 * seed + 3 * x), p[1] = uint8_t(255 - 30 * seed - 3 * y), p[2] = uint8_t(20 * l + 4 * (x ^ y)), p[3] = 255;
      }
    const size_t at = d.size();
    d.resize(at + rdr_scene::game_levels::LevelBytes(TexFormat::kBC1, s, s));
    rdr_scene::game_levels::detail::Encode(TexFormat::kBC1, px, s, s, d.data() + at);
  }
  return d;
}
// The DDS header (DDSD_MIPMAPCOUNT, DXT1; caps2 0xFE00 for a cube) followed by one chain, or six for a cube (the faces back to back, as dds.h ParseDds reads them).
static Bytes Dds(uint32_t w, uint32_t mips, bool cube) {
  Bytes d(128, 0);
  std::memcpy(d.data(), "DDS ", 4);
  PutLe32(d, 4, 124);
  PutLe32(d, 8, 0x20000u);
  PutLe32(d, 12, w);
  PutLe32(d, 16, w);
  PutLe32(d, 28, mips);
  PutLe32(d, 84, 0x31545844u);   // DXT1
  if (cube) PutLe32(d, 112, 0xFE00u);
  for (uint32_t f = 0; f < (cube ? 6u : 1u); ++f) {
    const Bytes c = Levels(w, mips, f + 1);
    d.insert(d.end(), c.begin(), c.end());
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
    const Bytes blob = Dds(tex[i].w, tex[i].mips, tex[i].cube);
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

// The set of bgEtc PACH `pach` as LoadStageEntry builds it; the plants are the wrong rules a check must catch.
static void Build(const Fixture& f, int pach, TexSet& tex) {
  if (g_plant.empty() || g_plant == "stage_loop") {
    sl::AddStructDirs(tex, f.buf, f.h, pach);
    return;
  }
  if (g_plant == "file_chain") {   // the old loop of LoadStageEntry
    for (const pac::Ent& t : f.h.ents)
      if ((t.id == 0 || (t.id >= 10 && t.id <= 19)) && t.size >= 0x30) tex.AddDir(pac::Decode(f.buf.data() + t.off, t.size));
    return;
  }
  const int type = sl::HolderTypeOfPach(pach);
  const bool flag = sl::LadderFlagOfPach(pach);
  for (const uint32_t key : {sl::kListEntry, sl::kCubeEntry}) {
    const pac::Ent* e = pac::Find(f.h, key);
    if (!e) continue;
    Bytes d = pac::Decode(f.buf.data() + e->off, e->size);
    const uint32_t n = (key == sl::kListEntry || g_plant == "cube_at_list") ? sl::ListLevels(type, g_plant == "flag_pick" ? !flag : flag) : level_tag::kWholeChainLevels;
    if (g_plant == "no_growth" && n == level_tag::kWholeChainLevels)
      tex.AddDir(std::move(d));
    else
      tex.AddDirLevels(std::move(d), n);
  }
  if (g_plant == "reads_11_19")
    for (const pac::Ent& t : f.h.ents)
      if (t.id >= 11 && t.id <= 19) tex.AddDir(pac::Decode(f.buf.data() + t.off, t.size));
}
static uint32_t CountOf(const TexSet& ts, const char* name) {
  Texture t;
  std::string why;
  return ts.Peek(name, t, why) ? t.game_levels : 9999u;
}
// The texture the upload makes of `name` (Relevel's copy, else the file's, which lands in `file`).
static Texture UploadedTex(const TexSet& ts, const char* name, Texture& file) {
  Texture out;
  std::string why;
  if (!ts.Peek(name, file, why)) return Texture{};
  return rdr_scene::game_levels::Relevel(file, out) ? out : file;
}
static uint32_t Uploaded(const TexSet& ts, const char* name) {
  Texture file;
  return UploadedTex(ts, name, file).mips;
}
// Bytes of the first `levels` levels of a w x w DXT1 chain (= the offset of level `levels`).
static size_t Bc1Chain(uint32_t w, uint32_t levels) {
  size_t n = 0;
  for (uint32_t l = 0; l < levels; ++l) n += rdr_scene::game_levels::LevelBytes(TexFormat::kBC1, std::max(1u, w >> l), std::max(1u, w >> l));
  return n;
}
// Every face of `up` (`faces` of them) holds `file`'s levels of that face that it kept and, past the file's `file_levels`, the BC1 encoding of the box of its own level before.
static bool FacesOk(const Texture& file, const Texture& up, uint32_t file_levels, uint32_t faces) {
  namespace gl = rdr_scene::game_levels;
  if (file.faces != faces || up.faces != faces || up.mips == 0 || file.width != up.width) return false;
  const uint32_t w = file.width, kept = std::min(file_levels, up.mips);
  const size_t up_face = Bc1Chain(w, up.mips), file_face = Bc1Chain(w, file_levels);
  if (up.data.size() != faces * up_face || file.data.size() < faces * file_face) return false;
  for (uint32_t f = 0; f < faces; ++f) {
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

// The body of `head` in src\assets\asset_load.h (from its definition to the first line that is a lone closing brace); empty when the file or the function is not found.
static std::string Body(const std::string& head) {
  const std::filesystem::path p = std::filesystem::path(__FILE__).parent_path() / ".." / ".." / "src" / "assets" / "asset_load.h";
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  std::string s = ss.str();
  s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
  const size_t b = s.find(head);
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

  {  // the key-0 count per PACH (sub_82576DD8's type and flag, sub_82577300 +0x1EC)
    bool ok = true;
    std::string bad;
    for (int p = 0; p <= 61; ++p) {
      const int type = sl::HolderTypeOfPach(p);
      const uint32_t want = (p == 8 || p == 9 || p == 10) ? 0u : p == 0 ? 4u : p == 3 ? 7u : p >= 11 ? 3u : kWhole;
      const uint32_t got = type < 0 ? 0u : sl::ListLevels(type, g_plant == "flag_pick" ? !sl::LadderFlagOfPach(p) : sl::LadderFlagOfPach(p));
      if (got != want) ok = false, bad += " " + std::to_string(p) + ":" + Num(got);
    }
    Check(ok, "type.map: CAGE 4, CELL 7, /STGL 11..61 3, CABU / CABA / LADM / LADW / CHAI / INFE the whole chain, 8..10 no request" + (ok ? std::string() : " (" + bad + " )"));
  }

  const Fixture fx = MakePach({{0, TexDir({{"body", 128, 8}, {"short", 128, 1}, {"dup", 64, 7}})},
                               {10, TexDir({{"cube_ref", 64, 1, true}, {"dup", 64, 1, true}})},
                               {11, TexDir({{"eleven", 32, 6}})},
                               {19, TexDir({{"nineteen", 32, 6}})}});
  {  // CAGE (PACH 0, type 0)
    TexSet tex;
    Build(fx, 0, tex);
    Check(CountOf(tex, "body") == 4 && Uploaded(tex, "body") == 4, "cage: key 0 at 4: an 8-level file cut to 4 (count " + Num(CountOf(tex, "body")) + ")");
    Check(CountOf(tex, "cube_ref") == kWhole && Uploaded(tex, "cube_ref") == 7,
          "cage.cube: key 10 at the whole chain whatever key 0's count (count " + Num(CountOf(tex, "cube_ref")) + ", uploaded " + Num(Uploaded(tex, "cube_ref")) + ")");
  }
  {  // CELL (PACH 3, type 3)
    TexSet tex;
    Build(fx, 3, tex);
    Check(CountOf(tex, "body") == 7 && Uploaded(tex, "body") == 7, "cell: key 0 at 7 (count " + Num(CountOf(tex, "body")) + ")");
  }
  for (const int p : {11, 61}) {  // a per-title ladder rig (/STGL, type 4 with [H+180] set)
    TexSet tex;
    Build(fx, p, tex);
    Check(CountOf(tex, "body") == 3 && Uploaded(tex, "body") == 3, "stgl." + std::to_string(p) + ": key 0 at 3 ([H+180] set) (count " + Num(CountOf(tex, "body")) + ")");
  }
  for (const int p : {1, 2, 4, 5, 7}) {  // CABU, CABA, LADM, LADW, INFE: [H+180] clear
    TexSet tex;
    Build(fx, p, tex);
    Check(CountOf(tex, "body") == kWhole && Uploaded(tex, "body") == 8, "whole." + std::to_string(p) + ": key 0 at the whole chain (count " + Num(CountOf(tex, "body")) + ")");
  }
  {  // the Elimination Chamber (PACH 6, type 5): key 0 at the whole chain, key 10's cube at the whole chain
    TexSet tex;
    Build(fx, 6, tex);
    Texture f1;
    const Texture up = UploadedTex(tex, "short", f1);
    Check(CountOf(tex, "short") == kWhole && up.mips == 8 && FacesOk(f1, up, 1, 1),
          "grow.2d: a 1-level 128 x 128 key-0 file at the whole chain: 8 levels, each the box of the one before (" + Num(up.mips) + ")");
    Texture fc;
    const Texture cube = UploadedTex(tex, "cube_ref", fc);
    Check(CountOf(tex, "cube_ref") == kWhole && cube.mips == 7 && FacesOk(fc, cube, 1, 6),
          "grow.cube: key 10's 1-level 64 x 64 cube at the whole chain: six faces of 7 levels, each its own box (" + Num(cube.mips) + ")");
    Texture d;
    std::string why;
    Check(tex.Peek("dup", d, why) && d.faces == 1, "order: a name in keys 0 and 10 resolves to key 0's texture (the first list appended)");
    Check(!tex.Has("eleven") && !tex.Has("nineteen"), "entries: keys 11..19 are not read (the builder looks up keys 0 and 10 only)");
  }
  {  // PACH 8 (/STG/SNCE): no request names it, the file's chain
    TexSet tex;
    Build(fx, 8, tex);
    Check(CountOf(tex, "body") == 0, "snce: a PACH no request names keeps the file's chain (count " + Num(CountOf(tex, "body")) + ")");
  }
  {  // LoadStageEntry and LoadGmEntry add the stage set through AddStructDirs
    std::string stage = Body("inline bool LoadStageEntry(");
    const std::string gm = Body("inline bool LoadGmEntry(");
    if (g_plant == "stage_loop") {
      const size_t at = stage.find("stage_levels::AddStructDirs(");
      if (at != std::string::npos) stage.replace(at, std::strlen("stage_levels::AddStructDirs("), "tex.AddDir(");
    }
    const size_t calls = Count(stage, "stage_levels::AddStructDirs(tex, p->bytes, h, pach)");
    const size_t others = Count(stage, "AddDir(") + Count(stage, "AddDirLevels(") + Count(stage, "AddDirNamed(") + Count(stage, "AddTexDirs(");
    Check(!stage.empty() && calls == 1 && others == 0,
          "source.stage: LoadStageEntry calls stage_levels::AddStructDirs(tex, p->bytes, h, pach) once and adds no directory itself (" + Num(uint32_t(calls)) + " calls, " +
              Num(uint32_t(others)) + " other adds)");
    Check(!gm.empty() && Count(gm, "stage_levels::AddStructDirs(tex, ep->bytes, ep->pachs[6], 6)") == 1,
          "source.gm: LoadGmEntry adds the chamber's PACH 6 set through stage_levels::AddStructDirs");
  }
  std::printf("%s: %d failed\n", g_plant.empty() ? "stage_levels_unit" : ("stage_levels_unit --plant " + g_plant).c_str(), g_fail);
  return g_fail ? 1 : 0;
}
