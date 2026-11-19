// The texture registry walk, ONE place (Session 103, lane F4; shared with lane D6's guest-texture helper: do not fork it). Pure over a GuestMem.
// Handle id -> registry entry -> texture WRAPPER -> library header -> the six fetch dwords. [C: sub_82294F20 listing (global 0x8392DD9C, +8 / +16 array, index = id & 0xFFFF, stored id must
// equal the handle), E_inline_callbacks.md section 3 / H_textures.md 1.1-1.2 (wrapper vtable 0x8206ADB8 or its second copy 0x82210B40, +52 kind, +80 / +84 / +88 header, header +28..+48).]
// sub_82294F20 itself writes the handle's cache word (handle+4), so the cache has a known writer; resolving fresh from the id is still right (the cache may be stale or never written under OURS).
#pragma once
#include <cstdint>

#include "../guest_view.h"   // ToPhysical: the 0xE0000000 view's +4 KB (S103 TX)
#include "particle_reader.h"   // GuestMem

namespace rdr_scene::live::guest_tex {
// Test only (guest_texture.h documents plants 1-6): 7 = the walk leaves words 1 and 5 as stored (the CPU view's +4 KB never added: the S103 SM fault, mip 0 read one page early in an E-view texture).
inline int g_tex_plant = 0;
}  // namespace rdr_scene::live::guest_tex

namespace rdr_scene::live::particles {

inline constexpr uint32_t kTextureHandleGlobal = 0x8392DD9Cu;
inline constexpr uint32_t kWrapperVtableA = 0x8206ADB8u, kWrapperVtableB = 0x82210B40u;
inline constexpr uint32_t kWrapperKind = 52, kWrapperTexture = 80, kHeaderFetch = 28;

enum class TexWalk { Ok, NoHandle, NotWrapper2D, NoHeader, Unreadable };

// S103 TX: the header's words 1 (base) and 5 (mip chain base) hold CPU view addresses; the game's SetTexture turns them into GPU (physical) form, +4 KB for the 0xE0000000 view
// [C: listing 0x82CA8B6C-0x82CA8BEC; src\guest_view.h ToPhysical, static_assert 0xFC545000 -> 0x1C546000]. A no-op on a physical or 0xA / 0xC view address (ViewOffset 0). Low bits (format, endian, dimension, packed) are kept.
// EVERY read of a texture header word's base address goes through this (tools\scene\texreg_lint.py).
inline void FetchToGpuForm(uint32_t f[6]) {
  if (guest_tex::g_tex_plant == 7) return;
  f[1] = wwe2k14::guest_view::ToPhysical(f[1]);
  f[5] = wwe2k14::guest_view::ToPhysical(f[5]);
}

struct TextureObject {
  uint32_t wrapper = 0, header = 0, fetch[6] = {};
};

inline TexWalk WalkTextureHandle(const GuestMem& m, uint32_t id, TextureObject& out) {
  const auto rd = [&](uint32_t a, uint32_t& o) { return a <= 0xFFFFFFFCu && m.Read32(a, o); };
  uint32_t mgr = 0, b = 0, e = 0, stored = 0, wrapper = 0;
  if (!rd(kTextureHandleGlobal, mgr) || !mgr || !rd(mgr + 8, b) || !rd(mgr + 16, e)) return TexWalk::NoHandle;
  const uint32_t count = uint32_t(int32_t(e - b) >> 3), idx = id & 0xFFFFu;
  if (idx >= count || !rd(b + idx * 8, stored) || !rd(b + idx * 8 + 4, wrapper) || stored != id || !wrapper) return TexWalk::NoHandle;
  uint32_t vt = 0, kind = 0, header = 0;
  if (!rd(wrapper, vt) || !rd(wrapper + kWrapperKind, kind)) return TexWalk::Unreadable;
  if ((vt != kWrapperVtableA && vt != kWrapperVtableB) || kind != 0) return TexWalk::NotWrapper2D;   // not the wrapper class, or a cube / volume
  if (!rd(wrapper + kWrapperTexture, header) || !header) return TexWalk::NoHeader;
  out.wrapper = wrapper, out.header = header;
  for (uint32_t i = 0; i < 6; ++i)
    if (!rd(header + kHeaderFetch + 4 * i, out.fetch[i])) return TexWalk::NoHeader;
  FetchToGpuForm(out.fetch);
  return TexWalk::Ok;
}

}  // namespace rdr_scene::live::particles
