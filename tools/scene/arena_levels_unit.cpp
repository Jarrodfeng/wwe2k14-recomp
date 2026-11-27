// arena_levels_unit: the offline proof of S111 lane ARENALEVELS (docs\research\s111\ARENALEVELS.md): the level count ours gives an arena's stage textures
// (src\assets\arena_levels.h: the 50010 table, the record byte 24 name rules, 401 / 402 at 3) through the real TexSet (src\assets\dds.h) and the upload rule
// (src\renderer_scene\game_levels.h Relevel). 0 game runs; every fixture is synthetic bytes of our own (nothing from the game).
//   arena_levels_unit [--plant file_chain|no_table|no_byte24]   a plant must make a check FAIL (exit 1)
//     file_chain  the old load: every stage directory added with TexSet::AddDir (no count: the file's own chain is uploaded)
//     no_table    the arena's 50010 table not read (every own-400 name at 3)
//     no_byte24   the arena record not read (byte 24 = 0: the whole-chain and design names keep the table's count / 3)
// Build: pwsh -NoProfile -File tools\scene\build_arena_levels_unit.ps1
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "../../src/assets/arena_levels.h"
#include "../../src/renderer_scene/game_levels.h"

using namespace assets;
namespace al = assets::arena_levels;

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
struct Tex {
  const char* name;
  uint32_t w, mips;
};
// A texture directory (dds.h TexSet::AddDir's layout): count, 12 header bytes, count x {name[16], "dds", size, offset}, then the DDS blobs.
static Bytes TexDir(const std::vector<Tex>& tex) {
  Bytes d(0x10 + 32 * tex.size(), 0);
  PutLe32(d, 0, uint32_t(tex.size()));
  for (size_t i = 0; i < tex.size(); ++i) {
    const Bytes blob = Dds(tex[i].w, tex[i].mips);
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
// A record leaf of misc01_Start p0/2's shape: 12-byte head (count at +4), 108-byte rows (byte 4 = the id, byte 24 = the flag).
static Bytes Leaf(const std::vector<std::pair<int, int>>& rec) {
  Bytes b(12 + 108 * rec.size(), 0);
  PutLe32(b, 4, uint32_t(rec.size()));
  for (size_t i = 0; i < rec.size(); ++i) b[12 + 108 * i + 4] = uint8_t(rec[i].first), b[12 + 108 * i + 24] = uint8_t(rec[i].second);
  return b;
}

// A 50010 text in the files' layout (tabs, CR LF, a byte order mark, a bias block after the rows): our own names.
static const char kXml[] =
    "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\t<MipmapData NumOfData=\"6\">\t<MipmapDataAttribute>\t<TextureName>wall_a</TextureName>\r\n"
    "\t<MipmapLevel>2</MipmapLevel>\r\n</MipmapDataAttribute>\r\n\t<MipmapDataAttribute>\t<TextureName>floor_b</TextureName>\r\n\t<MipmapLevel>0</MipmapLevel>\r\n"
    "</MipmapDataAttribute>\r\n<MipmapDataAttribute><TextureName>wall_a</TextureName><MipmapLevel>1</MipmapLevel></MipmapDataAttribute>"
    "<MipmapDataAttribute><TextureName>ji_top</TextureName><MipmapLevel>2</MipmapLevel></MipmapDataAttribute>"
    "<MipmapDataAttribute><TextureName>sign_c</TextureName><MipmapLevel>1</MipmapLevel></MipmapDataAttribute></MipmapData>\r\n"
    "<MipmapBiasData NumOfData=\"1\"><MipmapBiasDataAttribute><ShapeName>s0</ShapeName><TextureName>bias_only</TextureName><MipmapBias>-50</MipmapBias>"
    "</MipmapBiasDataAttribute></MipmapBiasData>";
static Bytes XmlBytes() { return Bytes(kXml, kXml + std::strlen(kXml)); }

// The stage directories of `f` into `tex` (and the created arena's part sets), as LoadArena adds them; the plants are the wrong rules a check must catch.
static void Build(const Fixture& f, const Bytes& leaf, int arena, bool created, TexSet& tex, TexSet* part) {
  if (g_plant == "file_chain") {
    for (const pac::Ent& e : f.h.ents)
      if (e.id >= 400 && e.id <= 404) tex.AddDir(pac::Decode(f.buf.data() + e.off, e.size));
    return;
  }
  pac::Pach h = f.h;
  if (g_plant == "no_table") h.ents.erase(std::remove_if(h.ents.begin(), h.ents.end(), [](const pac::Ent& e) { return e.id == al::kMipTableEntry; }), h.ents.end());
  const Bytes none;
  const al::Rule r = al::MakeRule(g_plant == "no_byte24" ? none : leaf, h, f.buf, arena, created);
  al::AddStageDirs(tex, part, h, f.buf, created, r);
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

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string(argv[i]) == "--plant") g_plant = argv[i + 1];
  const uint32_t kWhole = level_tag::kWholeChainLevels;

  {  // the table reader
    const Bytes x = XmlBytes();
    const std::vector<al::Row> rows = al::ParseMipmapData(x.data(), x.size());
    Check(rows.size() == 6, "parse.rows: NumOfData rows (" + Num(uint32_t(rows.size())) + ")");
    Check(rows.size() == 6 && rows[0].name == "wall_a" && rows[0].level == 2 && rows[1].name == "floor_b" && rows[1].level == 0 && rows[2].name == "wall_a" &&
              rows[2].level == 1 && rows[3].name == "ji_top" && rows[4].name == "sign_c" && rows[4].level == 1 && rows[5].name.empty(),
          "parse.order: one row per attribute, in order; the unfilled row is empty");
    bool bias = false;
    for (const al::Row& w : rows) bias = bias || w.name == "bias_only";
    Check(!bias, "parse.bias: the MipmapBiasData block makes no row");
  }
  {  // the record byte
    const Bytes leaf = Leaf({{4, 1}, {64, 0}, {200, 1}});
    Check(al::RecordByte(leaf, 4, al::kRecordByte) == 1, "record.4: byte 24 set");
    Check(al::RecordByte(leaf, 64, al::kRecordByte) == 0, "record.64: byte 24 clear");
    Check(al::RecordByte(leaf, 230, al::kRecordByte) == 1, "record.230: a created arena reads row 200");
    Check(al::RecordByte(leaf, 99, al::kRecordByte) == 0, "record.99: no row reads 0");
    Check(al::RecordByte(Bytes(8, 0), 4, al::kRecordByte) == -1, "record.short: not the table");
  }
  {  // the per-name rule itself: byte-exact names; a 16-character record name runs on into its tag
    al::Rule r;
    const Bytes x = XmlBytes();
    r.rows = al::ParseMipmapData(x.data(), x.size());
    Check(r.Of("wall_a") == 2 && r.Of("Wall_A") == 3, "rule.case: the table's names match byte for byte");
    std::vector<std::string> seen;
    TexSet ts;
    ts.AddDirNamed(TexDir({{"abcdefghijklmnop", 8, 1}, {"short", 8, 1}}), [&seen](const std::string& raw) {
      seen.push_back(raw);
      return 3u;
    });
    Check(seen.size() == 2 && seen[0] == "abcdefghijklmnopdds" && seen[1] == "short", "rule.raw: the record's bytes up to the NUL");
  }

  const Bytes leaf = Leaf({{4, 1}, {64, 0}, {200, 1}});
  const Fixture bg = MakePach({{400, TexDir({{"wall_a", 64, 7}, {"floor_b", 64, 1}, {"other", 64, 7}, {"ji_top", 64, 7}, {"ar_dm", 64, 7}, {"design", 64, 7}})},
                               {401, TexDir({{"cube_x", 32, 6}})},
                               {402, TexDir({{"extra", 16, 5}, {"wall_a", 8, 4}})},
                               {al::kMipTableEntry, XmlBytes()}});
  {  // a shipped arena whose record sets byte 24 (arena 4)
    TexSet tex;
    Build(bg, leaf, 4, false, tex, nullptr);
    Check(CountOf(tex, "wall_a") == 2 && Uploaded(tex, "wall_a") == 2, "bg.wall_a: the table's first row: 2 levels (count " + Num(CountOf(tex, "wall_a")) + ")");
    Check(CountOf(tex, "floor_b") == kWhole && Uploaded(tex, "floor_b") == 7,
          "bg.floor_b: table 0 = the whole chain: a 1-level 64 x 64 file grows to 7 (" + Num(Uploaded(tex, "floor_b")) + ")");
    Check(CountOf(tex, "other") == 3 && Uploaded(tex, "other") == 3, "bg.other: not in the table: 3 (" + Num(Uploaded(tex, "other")) + ")");
    Check(CountOf(tex, "ji_top") == kWhole && Uploaded(tex, "ji_top") == 7, "bg.ji_top: byte 24 makes it the whole chain over the table's 2 (count " + Num(CountOf(tex, "ji_top")) + ")");
    Check(CountOf(tex, "ar_dm") == kWhole, "bg.ar_dm: byte 24: the whole chain");
    Check(CountOf(tex, "design") == 3, "bg.design: the design names are a created arena's only");
    Check(CountOf(tex, "cube_x") == 3, "bg.401: the list's 3");
    Check(CountOf(tex, "extra") == 3 && Uploaded(tex, "extra") == 3, "bg.402: the list's 3");
  }
  {  // a shipped arena whose record clears byte 24 (arena 64)
    TexSet tex;
    Build(bg, leaf, 64, false, tex, nullptr);
    Check(CountOf(tex, "ji_top") == 2, "bg64.ji_top: byte 24 clear: the table's 2 (" + Num(CountOf(tex, "ji_top")) + ")");
    Check(CountOf(tex, "ar_dm") == 3, "bg64.ar_dm: byte 24 clear: 3");
  }
  const Fixture car = MakePach({{400, TexDir({{"design", 64, 7}})},
                                {403, TexDir({{"design_02", 32, 6}, {"ar_fence", 32, 6}})},
                                {401, TexDir({{"c", 16, 5}})},
                                {404, TexDir({{"ar_dm", 64, 7}, {"wall_a", 64, 7}})},
                                {402, TexDir({{"x", 16, 5}})},
                                {al::kMipTableEntry, XmlBytes()}});
  {  // a created arena (230: row 200, byte 24 set)
    TexSet tex, part[3];
    Build(car, leaf, 230, true, tex, part);
    Check(CountOf(tex, "design") == 1 && Uploaded(tex, "design") == 1, "car.design: 1 level (" + Num(Uploaded(tex, "design")) + ")");
    Check(CountOf(tex, "design_02") == 1, "car.design_02 (CAEN 400): 1");
    Check(CountOf(tex, "ar_fence") == kWhole, "car.ar_fence: the whole chain");
    Check(CountOf(tex, "ar_dm") == kWhole, "car.ar_dm (CARG 400): the whole chain");
    Check(CountOf(tex, "wall_a") == 2, "car.wall_a: the table's 2");
    Check(CountOf(tex, "c") == 3 && CountOf(tex, "x") == 3, "car.401 / 402: 3");
    Check(CountOf(part[0], "design") == 1 && CountOf(part[1], "design_02") == 1 && CountOf(part[1], "c") == 3 && CountOf(part[2], "ar_dm") == kWhole &&
              CountOf(part[2], "x") == 3,
          "car.parts: each part's set holds its own directories with the same counts");
  }
  {  // a created arena without its record
    TexSet tex, part[3];
    Build(car, Leaf({{4, 1}}), 230, true, tex, part);
    Check(CountOf(tex, "design") == 3, "car_nob24.design: no row 200: 3");
  }
  std::printf("%s: %d failed\n", g_plant.empty() ? "arena_levels_unit" : ("arena_levels_unit --plant " + g_plant).c_str(), g_fail);
  return g_fail ? 1 : 0;
}
