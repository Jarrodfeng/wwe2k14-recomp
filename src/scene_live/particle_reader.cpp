// Route 2's source reader: see particle_reader.h and docs\formats\particle_items.md.
#include "particle_reader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace rdr_scene::live::particles {
namespace {

float F(uint32_t w) {
  float f;
  std::memcpy(&f, &w, 4);
  return f;
}
bool Sane(float v) { return std::isfinite(v) && std::fabs(v) <= 1e6f; }

// The contract: a 4-byte read whose range wraps mod 2^32 is refused here, whatever the GuestMem does.
bool Rd(const GuestMem& m, uint32_t a, uint32_t& o) { return a <= 0xFFFFFFFCu && m.Read32(a, o); }

// One byte (big-endian word, byte (a & 3) from the top).
bool RdByte(const GuestMem& m, uint32_t a, uint32_t& o) {
  uint32_t w = 0;
  if (!Rd(m, a & ~3u, w)) return false;
  o = (w >> (8 * (3 - (a & 3u)))) & 0xFFu;
  return true;
}

enum class Resolve { Yes, No, Unreadable };

// sub_82756710 / sub_82756780 [C listings]: id = [handle]; mgr = [global]; entries {id, ptr} 8 bytes from [mgr+8] to [mgr+16]; idx = id & 0xFFFF must be below
// (end - begin) >> 3 (arithmetic shift, then an unsigned compare); the entry's id must equal the handle's; the result is the entry's pointer (0 = not resolved).
Resolve ResolveHandle(const GuestMem& m, uint32_t handle, uint32_t global) {
  uint32_t id = 0, mgr = 0, b = 0, e = 0;
  if (!Rd(m, handle, id) || !Rd(m, global, mgr) || !mgr || !Rd(m, mgr + 8, b) || !Rd(m, mgr + 16, e)) return Resolve::Unreadable;
  const uint32_t count = uint32_t(int32_t(e - b) >> 3);
  const uint32_t idx = id & 0xFFFFu;
  if (idx >= count) return Resolve::No;
  uint32_t stored = 0, ptr = 0;
  if (!Rd(m, b + idx * 8, stored) || !Rd(m, b + idx * 8 + 4, ptr)) return Resolve::Unreadable;
  return stored == id && ptr ? Resolve::Yes : Resolve::No;
}

}  // namespace

CameraBasis BasisFromView(const float v[16]) {
  CameraBasis b;
  for (int r = 0; r < 3; ++r) b.right[r] = v[r * 4 + 0], b.up[r] = v[r * 4 + 1], b.fwd[r] = v[r * 4 + 2];
  return b;
}

PivotShift PivotOfCode(uint32_t code) {
  // [C: 0x82A0CB44-0x82A0CBC4] f0 = 1.0, f13 = 0.0, f12 = -1.0; the shifts (f11 = x, f13 = y) per code, then corners (shift -+ 1) * 10.
  static const PivotShift grid[9] = {{1, -1}, {0, -1}, {-1, -1}, {1, 0}, {0, 0}, {-1, 0}, {1, 1}, {0, 1}, {-1, 1}};
  return code <= 8 ? grid[code] : PivotShift{0, 0};
}

namespace {

struct ItemKeys {
  ParticleSnapshot& out;
  ReaderStats& st;
  const TextureNamer* namer;
  BatchKey base;
  uint32_t table = 0;  // slot 33's pointer (the 8-byte handle table)
  struct Cached { uint32_t idx, key; };
  std::vector<Cached> cache, hard_cache;   // hard_cache: the same texture under soft = 0 (a flat sprite is drawn unfaded, PRT fix 1)
  // The batch key for texture index `idx`: a handle {id, cached pointer}; named by the namer or left without a texture (counted once per index per item).
  uint32_t KeyFor(const GuestMem& mem, uint32_t idx, bool hard = false) {
    for (const Cached& c : hard ? hard_cache : cache)
      if (c.idx == idx) return c.key;
    BatchKey k = base;
    if (hard) k.soft = 0;
    uint32_t id = 0, ptr = 0;
    bool named = false;
    if (idx < kMaxTextureIndex && Rd(mem, table + 8 * idx, id) && Rd(mem, table + 8 * idx + 4, ptr) && namer) named = namer->Name(id, ptr, k.pac, k.entry);
    if (!named) k.pac.clear(), k.entry.clear(), ++st.tex_unresolved;
    else ++st.tex_named;
    const uint32_t ki = out.Intern(k);
    (hard ? hard_cache : cache).push_back({idx, ki});
    return ki;
  }
};

void AddCorners(Particle& p, const PivotShift& pv, float sx, float sy, float cs, float sn, uint32_t color, const float uv[4][2]) {
  static const float cn[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};  // [C] strip corners
  const float ext = g_reader_plant == 1 ? 1.f : kHalfExtentPerSize;
  for (int i = 0; i < 4; ++i) {
    const float x = (cn[i][0] + pv.x) * ext * sx, y = (cn[i][1] + pv.y) * ext * sy;  // [C: sub_82A0C758 (pivot shift -+ 1) * 10, scaled by the size]
    p.v[i].pos[0] = x * cs - y * sn, p.v[i].pos[1] = x * sn + y * cs, p.v[i].pos[2] = 0;  // [C: sub_82C32EC8: rotation by size word 2, CCW]
    p.v[i].color = color;
    p.v[i].uv[0] = uv[i][0], p.v[i].uv[1] = uv[i][1];
    p.v[i].uv2[0] = p.v[i].uv2[1] = 0.f;
  }
}

}  // namespace

static void ReadItem(const GuestMem& mem, uint32_t item, const CameraBasis& cam, const TextureNamer* namer, ParticleSnapshot& out, ReaderStats& st) {
  uint32_t vt = 0, count = 0, base = 0;
  if (!Rd(mem, item, vt) || !Rd(mem, item + 40, count) || !Rd(mem, item + 68, base)) return void(++st.unreadable);
  const bool quad = vt == kQuadItemVtable, third = vt == kThirdItemVtable;   // S103 PRT: the third class is read as a sprite with roll 0 [L]
  if (vt != kSpriteItemVtable && !quad && !third) return void(++st.items_other_class);
  if (third) ++st.items_third;
  if (count > kMaxKeysPerItem) count = kMaxKeysPerItem, ++st.over_cap, ++st.over_keys;
  const auto slot = [&](uint32_t k, uint32_t& p) { return Rd(mem, item + 4 * (base + k), p) && p; };
  uint32_t p15 = 0, p_pos = 0, p_prev = 0, p_col = 0, p_size = 0, p_tr = 0, p_tex = 0, p_mat = 0;
  if (!Rd(mem, item + 4 * (base + 15), p15) || !p15) return void(++st.items_skipped);  // [C: sub_82A0D0E0 0x82A0D108-D114 / sub_82A0DAC8 0x82A0DD14] slot base+15 null = nothing to draw
  // [C: 0x82A0D294-D2B8] the handle at +156 (-> +160) and the handle at +148 (-> +152): an unresolved one ends the draw. Under OURS the draw that refreshes the cache words does not run: the lookup is done here.
  {
    const Resolve r156 = ResolveHandle(mem, item + 156, kHandleTableGlobal156);
    if (r156 == Resolve::Unreadable) return void(++st.unreadable);
    if (r156 == Resolve::No) return void(++st.items_unresolved);
    const Resolve r148 = ResolveHandle(mem, item + 148, kHandleTableGlobal148);
    if (r148 == Resolve::Unreadable) return void(++st.unreadable);
    if (r148 == Resolve::No) return void(++st.items_unresolved);
  }
  const bool slots_ok = quad ? (slot(29, p_mat) && slot(23, p_prev) && slot(25, p_col) && slot(33, p_tex) && slot(35, p_tr))
                             : (slot(21, p_pos) && slot(23, p_prev) && slot(25, p_col) && slot(27, p_size) && slot(33, p_tex) && slot(35, p_tr));
  if (!slots_ok) return void(++st.unreadable);

  // [C listings] the item's descriptor chain: o48 = [item+48], o0 = [o48]. pivot code = halfword o0+226, blend code = word o0+196 (-> sub_82BB7630 for 0 / 1 / 2 only),
  // idx = [o48+152]: the sampler byte is at *[o48 + 28 idx + 112] (-> sub_82BB7B20 for 0..3), ZOffset = int at *[o48 + 28 idx + 108] * 0.001 (c200.x).
  uint32_t o48 = 0, o0 = 0, pw = 0, bcode = 0, didx = 0, bptr = 0, zptr = 0, sbyte = 0, zint = 0;
  if (!Rd(mem, item + 48, o48) || !o48 || !Rd(mem, o48, o0) || !o0 || !Rd(mem, o0 + 224, pw) || !Rd(mem, o0 + 196, bcode) || !Rd(mem, o48 + 152, didx) || didx > 0x10000 ||
      !Rd(mem, o48 + didx * 28 + 112, bptr) || !RdByte(mem, bptr, sbyte) || !Rd(mem, o48 + didx * 28 + 108, zptr) || !Rd(mem, zptr, zint))
    return void(++st.unreadable);
  const PivotShift pv = g_reader_plant == 3 ? PivotShift{0, 0} : PivotOfCode(pw & 0xFFFF);  // the halfword at +226 is the low half of the big-endian word at +224
  if (pv.x != 0.f || pv.y != 0.f) ++st.pivot_not_centred;

  ItemKeys ik{out, st, namer, BatchKey(), p_tex, {}};
  ik.base.site = quad ? kParticleQuad : kParticleSprite;
  ik.base.soft = st.soft;   // S103 PRT: the global soft switch at the node's call (BatchKey::soft had no writer)
  ik.base.blend = BlendOfCode(bcode);
  ++st.blend_read;
  if (!BlendCodeProven(bcode) && g_reader_plant != 5) ++st.blend_unproven;  // S103 F4 (plant 5: every code counts as proven)
  if (bcode > 2) ++st.blend_other;  // the game makes no call for such a code: the device keeps whatever blend it had [U]
  if (sbyte <= 3) ik.base.addr = uint8_t(sbyte + 1), ++st.sampler_read;
  ik.base.zoffset = float(int32_t(zint)) * kZOffsetPerInt, ++st.zoffset_read;

  // [C: slot 35's object, log] trail count N at +20, factor at +24, the four corner uv pairs at +32 / +48 / +64 / +80.
  uint32_t trail_n = 0, factor_w = 0;
  float uv[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
  if (Rd(mem, p_tr + 20, trail_n) && Rd(mem, p_tr + 24, factor_w)) {
    for (int c = 0; c < 4; ++c) {
      uint32_t a = 0, b = 0;
      if (Rd(mem, p_tr + 32 + 16 * uint32_t(c), a) && Rd(mem, p_tr + 36 + 16 * uint32_t(c), b) && Sane(F(a)) && Sane(F(b))) uv[c][0] = F(a), uv[c][1] = F(b);
    }
  } else {
    trail_n = 0;
  }
  if (trail_n > 64) trail_n = 64, ++st.over_cap, ++st.over_trail;
  const float factor = F(factor_w);
  ++st.items;
  if (quad) ++st.items_quad;
  for (uint32_t k = 0; k < count; ++k) {
    if (out.entries.size() >= kMaxParticlesPerFrame) { out.dropped += (count - k) * (trail_n + 1); break; }
    uint32_t pw3[3], vw[3], cw[4], sw[4] = {0, 0, 0, 0}, mw[16] = {}, tw = 0;
    bool ok = true;
    for (uint32_t i = 0; i < 3; ++i) ok = ok && Rd(mem, p_prev + 16 * k + 4 * i, vw[i]);
    for (uint32_t i = 0; i < 4; ++i) ok = ok && Rd(mem, p_col + 16 * k + 4 * i, cw[i]);
    if (quad) {
      for (uint32_t i = 0; i < 16; ++i) ok = ok && Rd(mem, p_mat + 64 * k + 4 * i, mw[i]);  // [C: 0x82A0DEB4-DECC] four 16-byte rows at slot 29 + 64 k
      ok = ok && Rd(mem, p_prev + 16 * k + 12, tw);                                            // [C: 0x82A0DEE4] the texture index is word 3 of the slot-23 record
      for (uint32_t i = 0; i < 3; ++i) pw3[i] = mw[12 + i];                                   // the position is the matrix's row 3
    } else {
      for (uint32_t i = 0; i < 3; ++i) ok = ok && Rd(mem, p_pos + 16 * k + 4 * i, pw3[i]);
      for (uint32_t i = 0; i < 4; ++i) ok = ok && Rd(mem, p_size + 16 * k + 4 * i, sw[i]);
      tw = sw[1];                                                                              // [C: 0x82A0D5F0] the texture index is size word 1
    }
    if (!ok) { ++st.unreadable; continue; }
    const float pos[3] = {F(pw3[0]), F(pw3[1]), F(pw3[2])}, prev[3] = {F(vw[0]), F(vw[1]), F(vw[2])};
    const float sx = F(sw[0]), angle = F(sw[2]), sy = F(sw[3]);
    bool sane = Sane(pos[0]) && Sane(pos[1]) && Sane(pos[2]) && Sane(prev[0]) && Sane(prev[1]) && Sane(prev[2]);
    if (quad) for (int i = 0; i < 16; ++i) sane = sane && Sane(F(mw[i]));
    else sane = sane && Sane(sx) && Sane(sy) && (third || Sane(angle));   // the third class reads no angle (size word 2 unused)
    if (!sane) { ++st.non_finite; continue; }
    // [C: 0x82A0D624-D700, log] colour bytes (w3 * 2, w0 * 2, w1 * 2, w2 * 2) & 0xFE = A R G B (an ARGB big-endian word). Ours: R in byte 0.
    const uint32_t a8 = (cw[3] * 2) & 0xFE, r8 = (cw[0] * 2) & 0xFE, g8 = (cw[1] * 2) & 0xFE, b8 = (cw[2] * 2) & 0xFE;
    if (a8 == 0) { ++st.alpha_zero; continue; }  // [C: 0x82A0D6A0 / 0x82A0DF60] the game skips a key whose alpha byte a8 = (w3 << 1) & 0xFE is 0
    const uint32_t key_index = ik.KeyFor(mem, tw);
    const float cs = (quad || third) ? 1.f : std::cos(angle), sn = (quad || third) ? 0.f : std::sin(angle);  // once per key, not per trail step
    float step[3] = {0, 0, 0};
    if (trail_n && factor != 0.f && Sane(factor))
      for (int i = 0; i < 3; ++i) step[i] = (pos[i] - prev[i]) / float(trail_n) * factor;  // [C: 0x82A0D75C-D810 / 0x82A0E064-E08C]
    for (uint32_t t = 0; t <= trail_n; ++t) {
      if (out.entries.size() >= kMaxParticlesPerFrame) { ++out.dropped; continue; }
      Particle p{};
      float c[3];
      for (int i = 0; i < 3; ++i) c[i] = pos[i] - step[i] * float(t);  // each trail step moves back along the trail
      if (quad && g_reader_plant != 4) {  // plant 4: a quad is placed like a sprite (the camera's axes instead of its own matrix)
        // [L: 0x82A0E2B0-E31C transposes the four rows into c228..c231] local_world row r = (M[0][r], M[1][r], M[2][r], M[3][r]) with M's row 3 = the stepped position.
        for (int r = 0; r < 3; ++r) {
          p.local_world[4 * r + 0] = F(mw[0 + r]), p.local_world[4 * r + 1] = F(mw[4 + r]), p.local_world[4 * r + 2] = F(mw[8 + r]), p.local_world[4 * r + 3] = c[r];
        }
      } else {
        for (int r = 0; r < 3; ++r) {
          p.local_world[4 * r + 0] = cam.right[r], p.local_world[4 * r + 1] = cam.up[r], p.local_world[4 * r + 2] = cam.fwd[r], p.local_world[4 * r + 3] = c[r];
        }
      }
      const uint32_t alpha = t < 8 ? (a8 >> t) : 0;  // [C: 0x82A0D880 / 0x82A0E17x] the alpha byte halves each trail step
      AddCorners(p, pv, quad ? 1.f : sx, quad ? 1.f : sy, cs, sn, r8 | (g8 << 8) | (b8 << 16) | (alpha << 24), uv);
      // S103 PRT fix 1: the game's soft vertex stage reports view z = 0 for a sprite lying flat (-LocalWorld[1].z > 0.999, ySoftParticle.vert), which leaves it unfaded. Our fold sends an identity LocalWorld, so the rule is
      // applied here, in game space before any mirror: such a particle is interned under a key with soft = 0 and draws hard.
      uint32_t ki = key_index;
      if (g_reader_plant != 7 && st.soft && -p.local_world[6] > 0.999f) ki = ik.KeyFor(mem, tw, true), ++st.soft_flat_hard;
      out.Add(ki, p);
      ++st.keys;
    }
  }
}

void ReadEntry(const GuestMem& mem, uint32_t entry, const CameraBasis& cam, ParticleSnapshot& out, ReaderStats& st, const TextureNamer* namer) {
  ++st.entries;
  uint32_t idx = 0, gate = 0, f132 = 0, e64 = 0, obj = 0, arr = 0;
  if (!Rd(mem, entry + 56, idx) || idx > 1) return void(++st.unreadable);  // two 176-byte sub-records fill the entry
  const uint32_t sub = entry + 176 * idx;
  if (!Rd(mem, sub + 208, gate) || !Rd(mem, sub + 80 + 132, f132) || !Rd(mem, entry + 64, e64)) return void(++st.unreadable);
  if (g_reader_plant != 2 && (gate == 0 || (f132 == 0 && e64 != 0))) return void(++st.entries_gated);  // [C: sub_82A0AED8's own two early outs]
  uint32_t p = 0, count = 0;
  if (!Rd(mem, entry + 44, arr)) return void(++st.unreadable);
  if (!arr) return void(++st.entries_no_items);  // [C: 0x82A0AEE4] a null item array is a normal no-draw
  if (!Rd(mem, entry + 40, obj) || !Rd(mem, obj, p) || !Rd(mem, p + 12, count)) return void(++st.unreadable);
  if (count > kMaxItemsPerEntry) count = kMaxItemsPerEntry, ++st.over_cap, ++st.over_items;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t item = 0;
    if (!Rd(mem, arr + 4 * i, item)) { ++st.unreadable; continue; }
    if (!item) continue;  // [C: 0x82A0AF74] a null item pointer is skipped
    ReadItem(mem, item, cam, namer, out, st);
  }
}

void ReadNode(const GuestMem& mem, uint32_t node, const CameraBasis& cam, ParticleSnapshot& out, ReaderStats& st, const TextureNamer* namer) {
  uint32_t base = 0, count = 0, flag = 0;
  if (!Rd(mem, node + 3084, flag) || !Rd(mem, node + 16, base) || !Rd(mem, node + 3088, count)) return void(++st.unreadable);
  if (flag == 0 || count == 0 || !base) return void(++st.node_idle);  // [C: 0x82254E3C-E6C] the node's own early outs ([node+3084] / [node+3088] zero)
  if (count > (g_reader_plant == 6 ? 64u : kMaxEntries)) count = (g_reader_plant == 6 ? 64u : kMaxEntries), ++st.over_cap, ++st.over_entries;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t sw = 0;
    if (!Rd(mem, (node + 20 + i) & ~3u, sw)) { ++st.unreadable; continue; }
    const uint32_t state = (sw >> (8 * (3 - ((node + 20 + i) & 3u)))) & 0xFF;  // big-endian byte of the word
    if (state != 2) continue;
    if (g_reader_plant != 8 && st.list_n && !std::binary_search(st.list, st.list + st.list_n, base + 432 * i)) { ++st.entry_not_listed; continue; }   // S103 PRT fix 5 [C: 0x822550C8-0x822550E4]
    ReadEntry(mem, base + 432 * i, cam, out, st, namer);
  }
}

}  // namespace rdr_scene::live::particles
