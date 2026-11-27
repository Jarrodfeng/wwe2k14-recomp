// game_levels_unit: the offline proof of S111 lane MIPLEVELS (docs\research\s111\MIPLEVELS.md): the levels ours uploads for a file texture that carries the game's
// level count (src\renderer_scene\game_levels.h Relevel). 0 game runs, no SDK, no game files: synthetic textures made with the header's own encoder.
//   game_levels_unit [--plant file_chain]
// `--plant file_chain` = Relevel skipped (the old upload: the file's chain as it is): the count checks must fail.
// Checks (exit 1 when any failed; the last line `N checks, N failed`):
//   count.*    GameLevels: 5 of a 2048 x 1024 = 5; 0 = the whole chain (12); 5 of an 8 x 8 = its chain (4)
//   cut.*      a whole-chain BC1 32 x 32 (6 levels) at 5: 5 levels, the first 5 levels' bytes unchanged
//   cut.blood_* a whole-chain DXT3 64 x 64 blood splat (7 levels) at 3 (the stage's blood list): 3 levels, their bytes unchanged
//   grow.bc3   a 4-level BC3 64 x 32 at 5: 5 levels, levels 0..3 unchanged, level 4 = BC3's own encoding of the box of level 3 (colour AND alpha), byte for byte
//   grow.4444  a 1-level A4R4G4B4 16 x 16 at 5: 5 levels, level 1 exact (each 2 x 2 holds one value)
//   grow.bc1a  a 1-level BC1 8 x 8 whose left half is cut out (alpha 0), at 3: level 1 keeps the cut (alpha 0 left, 255 right)
//   same.*     no count, a cube without six whole chains (S111 CUBELEVELS: a whole cube is relevelled, prop_levels_unit event.cube_*), and a file already at the count: Relevel answers false
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/renderer_scene/game_levels.h"

namespace gl = rdr_scene::game_levels;
using assets::TexFormat;

static int g_checks = 0, g_failed = 0;
static bool g_plant = false;

static void Check(const char* name, bool ok, const std::string& detail = std::string()) {
  ++g_checks;
  if (!ok) ++g_failed;
  std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : ": ", detail.c_str());
}

// What the upload would see: Relevel's answer, or the file as it is under the plant.
static assets::Texture Uploaded(const assets::Texture& t) {
  assets::Texture out;
  if (!g_plant && gl::Relevel(t, out)) return out;
  return t;
}

static size_t ChainBytes(TexFormat f, uint32_t w, uint32_t h, uint32_t levels) {
  size_t n = 0;
  for (uint32_t i = 0; i < levels; ++i) n += gl::LevelBytes(f, std::max(1u, w >> i), std::max(1u, h >> i));
  return n;
}

// A texture of `levels` levels, each encoded from a smooth RGBA pattern (alpha a ramp when `alpha`).
static assets::Texture Make(TexFormat f, uint32_t w, uint32_t h, uint32_t levels, bool alpha) {
  assets::Texture t;
  t.name = "unit", t.format = f, t.width = w, t.height = h, t.mips = levels;
  for (uint32_t i = 0; i < levels; ++i) {
    const uint32_t lw = std::max(1u, w >> i), lh = std::max(1u, h >> i);
    std::vector<uint8_t> px(size_t(lw) * lh * 4);
    for (uint32_t y = 0; y < lh; ++y)
      for (uint32_t x = 0; x < lw; ++x) {
        uint8_t* p = &px[(size_t(y) * lw + x) * 4];
        p[0] = uint8_t(x * 255 / std::max(1u, lw - 1)), p[1] = uint8_t(y * 255 / std::max(1u, lh - 1)), p[2] = uint8_t((x + y) * 127 / std::max(1u, lw + lh - 2));
        p[3] = alpha ? uint8_t(255 - x * 200 / std::max(1u, lw - 1)) : 255;
      }
    const size_t at = t.data.size();
    t.data.resize(at + gl::LevelBytes(f, lw, lh));
    gl::detail::Encode(f, px, lw, lh, t.data.data() + at);
  }
  return t;
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "--plant") == 0 && i + 1 < argc && std::strcmp(argv[i + 1], "file_chain") == 0) g_plant = true, ++i;

  Check("count.stock_body", gl::GameLevels(5, 2048, 1024) == 5);
  Check("count.zero_whole_chain", gl::GameLevels(0, 2048, 1024) == 12);
  Check("count.above_chain", gl::GameLevels(5, 8, 8) == 4);

  {  // cut
    assets::Texture t = Make(TexFormat::kBC1, 32, 32, 6, false);
    t.game_levels = 5;
    const assets::Texture u = Uploaded(t);
    Check("cut.levels", u.mips == 5, "mips " + std::to_string(u.mips));
    const size_t want = ChainBytes(TexFormat::kBC1, 32, 32, 5);
    Check("cut.bytes", u.data.size() == want && std::memcmp(u.data.data(), t.data.data(), want) == 0, "bytes " + std::to_string(u.data.size()) + " want " + std::to_string(want));
  }
  {  // cut.blood: a whole-chain DXT3 64 x 64 blood splat (7 levels) at the stage list's 3: levels 64, 32, 16 kept, the 2x2 / 1x1 levels gone
    assets::Texture t = Make(TexFormat::kBC2, 64, 64, 7, true);
    t.game_levels = 3;
    const assets::Texture u = Uploaded(t);
    const size_t want = ChainBytes(TexFormat::kBC2, 64, 64, 3);
    Check("cut.blood_levels", u.mips == 3, "mips " + std::to_string(u.mips));
    Check("cut.blood_bytes", u.data.size() == want && std::memcmp(u.data.data(), t.data.data(), want) == 0, "bytes " + std::to_string(u.data.size()));
  }
  {  // grow.bc3
    assets::Texture t = Make(TexFormat::kBC3, 64, 32, 4, true);
    t.game_levels = 5;
    const assets::Texture u = Uploaded(t);
    const size_t four = ChainBytes(TexFormat::kBC3, 64, 32, 4), five = ChainBytes(TexFormat::kBC3, 64, 32, 5);
    Check("grow.bc3.levels", u.mips == 5 && u.data.size() == five, "mips " + std::to_string(u.mips) + " bytes " + std::to_string(u.data.size()));
    Check("grow.bc3.kept", u.data.size() >= four && std::memcmp(u.data.data(), t.data.data(), four) == 0);
    if (u.data.size() == five) {
      // MIPLEVELS review: level 4 is compared with the format's own encoding of the box of level 3, byte for byte. The old bar (decoded level 4 within 24 of the raw
      // box) measured the codec, not Relevel: one 4 x 4 block of this pattern spans two independent colour directions and ~200 levels, which a 4-colour BC line holds
      // only to ~70 whatever the encoder (it failed at 69 on the landed code). The box itself is checked exactly in a lossless format by grow.4444.exact.
      std::vector<uint8_t> l3, box;
      gl::detail::Decode(TexFormat::kBC3, t.data.data() + ChainBytes(TexFormat::kBC3, 64, 32, 3), 8, 4, l3);
      gl::detail::Box(l3, 8, 4, box);
      std::vector<uint8_t> want(gl::LevelBytes(TexFormat::kBC3, 4, 2));
      gl::detail::Encode(TexFormat::kBC3, box, 4, 2, want.data());
      Check("grow.bc3.box", std::memcmp(u.data.data() + four, want.data(), want.size()) == 0, "level 4 is not the BC3 encoding of level 3's box");
    } else {
      Check("grow.bc3.box", false, "no level 4");
    }
  }
  {  // grow.4444
    assets::Texture t;
    t.name = "mask", t.format = TexFormat::kB4G4R4A4, t.width = 16, t.height = 16, t.mips = 1, t.game_levels = 5;
    t.data.resize(16 * 16 * 2);
    for (uint32_t y = 0; y < 16; ++y)
      for (uint32_t x = 0; x < 16; ++x) {
        const uint16_t v = uint16_t(((x / 2) & 15) | (((y / 2) & 15) << 4) | (((x / 2 + y / 2) & 15) << 8) | (15u << 12));
        t.data[(y * 16 + x) * 2] = uint8_t(v), t.data[(y * 16 + x) * 2 + 1] = uint8_t(v >> 8);
      }
    const assets::Texture u = Uploaded(t);
    Check("grow.4444.levels", u.mips == 5 && u.data.size() == ChainBytes(TexFormat::kB4G4R4A4, 16, 16, 5), "mips " + std::to_string(u.mips));
    bool exact = u.data.size() >= 512 + 128;
    for (uint32_t y = 0; exact && y < 8; ++y)
      for (uint32_t x = 0; x < 8; ++x) {
        const uint16_t want = uint16_t((x & 15) | ((y & 15) << 4) | (((x + y) & 15) << 8) | (15u << 12));
        const uint16_t got = uint16_t(u.data[512 + (y * 8 + x) * 2] | (u.data[512 + (y * 8 + x) * 2 + 1] << 8));
        if (got != want) exact = false;
      }
    Check("grow.4444.exact", exact);
  }
  {  // grow.bc1a
    assets::Texture t;
    t.name = "cut", t.format = TexFormat::kBC1, t.width = 8, t.height = 8, t.mips = 1, t.game_levels = 3;
    std::vector<uint8_t> px(8 * 8 * 4);
    for (uint32_t y = 0; y < 8; ++y)
      for (uint32_t x = 0; x < 8; ++x) {
        uint8_t* p = &px[(y * 8 + x) * 4];
        p[0] = 200, p[1] = 100, p[2] = 50, p[3] = x < 4 ? 0 : 255;
      }
    t.data.resize(gl::LevelBytes(TexFormat::kBC1, 8, 8));
    gl::detail::Encode(TexFormat::kBC1, px, 8, 8, t.data.data());
    const assets::Texture u = Uploaded(t);
    Check("grow.bc1a.levels", u.mips == 3 && u.data.size() == ChainBytes(TexFormat::kBC1, 8, 8, 3), "mips " + std::to_string(u.mips));
    bool cut = u.data.size() >= 32 + 8;
    if (cut) {
      std::vector<uint8_t> l1;
      gl::detail::Decode(TexFormat::kBC1, u.data.data() + 32, 4, 4, l1);
      for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x)
          if (l1[(y * 4 + x) * 4 + 3] != (x < 2 ? 0 : 255)) cut = false;
    }
    Check("grow.bc1a.cut_kept", cut);
  }
  {  // same
    assets::Texture none = Make(TexFormat::kBC1, 16, 16, 5, false), out;
    Check("same.no_count", !gl::Relevel(none, out));
    assets::Texture at = none;
    at.game_levels = 5;
    Check("same.at_count", !gl::Relevel(at, out));
    assets::Texture cube = none;   // one face's chain under faces = 6: MakeTexture refuses it, Relevel declines it
    cube.game_levels = 3, cube.faces = 6;
    Check("same.cube_short", !gl::Relevel(cube, out));
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
