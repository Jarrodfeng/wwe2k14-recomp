// S103 CPF unit: assets::AlphaCutsOut (src\assets\jboy.h) on synthetic BC3 / BC2 textures. 0 game runs, no game files.
//   a gloss mask (alpha 0..40, 18.8 % exactly 0: the shape of Cena's 13904Cap) -> not a cut-out
//   a real cut-out (half the texels 0, half 255) -> a cut-out
//   the P1 exemption (plain_default) keeps the old answer for a low-alpha texture (a shadow card)
// Exit 0 when all hold, 1 otherwise.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstdio>

#include "../../src/assets/jboy.h"

using namespace assets;

namespace {
int g_fail = 0;
void Check(const char* n, bool ok) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", n);
  if (!ok) ++g_fail;
}

// A BC3 texture of 8x8 (4 blocks): `fn(block)` gives {a0, a1, index-pattern byte} for the alpha block; colour bytes zero.
Texture MakeBC3(uint8_t a0, uint8_t a1, uint8_t ix_byte, int blocks_solid = 0) {
  Texture t;
  t.format = TexFormat::kBC3;
  t.width = t.height = 8;
  t.data.assign(4 * 16, 0);
  for (int b = 0; b < 4; ++b) {
    uint8_t* q = &t.data[size_t(b) * 16];
    const bool solid = b < blocks_solid;
    q[0] = solid ? 255 : a0, q[1] = solid ? 255 : a1;
    for (int i = 0; i < 6; ++i) q[2 + i] = solid ? 0 : ix_byte;
  }
  return t;
}
}  // namespace

int main() {
  // mask: a0 = 40, a1 = 0 (a0 > a1: 8-value ramp), indices 0 / 1 only (0b001001... bit patterns: all index 1 = a1 = 0 in the low half): use 0x00 (index 0 = a0 = 40) and 0x49 (indices 1 = a1 = 0).
  const Texture mask40 = MakeBC3(40, 0, 0x49);  // 3-bit indices 001 001 001 ... -> every texel = a1 = 0 or a0 mix
  Texture mask_mixed = MakeBC3(40, 0, 0x49);
  for (int b = 0; b < 2; ++b)
    for (int i = 0; i < 6; ++i) mask_mixed.data[size_t(b) * 16 + 2 + size_t(i)] = 0x00;  // two blocks: every texel a0 = 40
  Check("mask alpha 0..40 is not a cut-out", !AlphaCutsOut(mask_mixed) && !AlphaCutsOut(mask40));
  // real cut-out: two blocks all 255 (solid), two blocks all 0 (a0 = a1 = 0)
  const Texture cut = MakeBC3(0, 0, 0x00, 2);
  Check("half 0 / half 255 is a cut-out", AlphaCutsOut(cut));
  // a low-alpha texture, P1 domain: the old answer (a cut-out when >= 8 % are <= 32)
  Check("keep_old_rule (yBG_ arena, P1 domain) keeps the old answer for a max-40 texture", AlphaCutsOut(mask_mixed, true) == true);
  // a max-223 texture is a mask; a texture with 50 % of texels at 224 and 50 % zeros is a cut-out
  Texture m127 = MakeBC3(223, 0, 0x49);
  for (int b = 0; b < 2; ++b) for (int i = 0; i < 6; ++i) m127.data[size_t(b) * 16 + 2 + size_t(i)] = 0x00;
  Check("max alpha 223 (no solid texel) is a mask", !AlphaCutsOut(m127));
  Texture m128 = MakeBC3(224, 0, 0x49);
  for (int b = 0; b < 2; ++b) for (int i = 0; i < 6; ++i) m128.data[size_t(b) * 16 + 2 + size_t(i)] = 0x00;
  Check("alpha 224 (solid) with zeros is a cut-out", AlphaCutsOut(m128));
  // BC2: explicit 4-bit alpha: all texels 0x2 (= 34) -> mask; all 0xF / 0x0 halves -> cut-out
  Texture bc2;
  bc2.format = TexFormat::kBC2;
  bc2.width = bc2.height = 8;
  bc2.data.assign(4 * 16, 0);
  for (int b = 0; b < 4; ++b)
    for (int i = 0; i < 8; ++i) bc2.data[size_t(b) * 16 + size_t(i)] = b < 2 ? 0x22 : 0x00;
  Check("BC2 alpha 0..34 is not a cut-out", !AlphaCutsOut(bc2));
  for (int b = 0; b < 2; ++b)
    for (int i = 0; i < 8; ++i) bc2.data[size_t(b) * 16 + size_t(i)] = 0xFF;
  Check("BC2 alpha 0 / 255 is a cut-out", AlphaCutsOut(bc2));
  Material mt;
  Check("CPFKeepOld: yBG_UVScroll keeps the old rule", CPFKeepOld("ybg_uvscroll", mt));
  Check("CPFKeepOld: plain yProp_Default keeps it", CPFKeepOld("yprop_default", mt));
  Check("CPFKeepOld: yProp_BumpMap (the cap) does not", !CPFKeepOld("yprop_bumpmap", mt));
  mt.specular = 3;
  Check("CPFKeepOld: yProp_Default with a spec map does not", !CPFKeepOld("yprop_default", mt));
  std::printf("RESULT %s\n", g_fail ? "FAIL" : "PASS");
  return g_fail ? 1 : 0;
}
