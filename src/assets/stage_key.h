// Session 97 (stage motion key): the arena pac's entry 30000 (animated light sets) and 30001 (the stage slots' motions
// m(101..119) of entry 50001), both "YMKs" key sets, read from the player's own game files at run time (CLAUDE.md rules 1
// and 2: nothing from the game is kept or committed). Port of tools\assets\stagekey.py; format and evidence (which numbers
// reproduce which live values, what is unconfirmed): out\runtime\s97\stage_key.md.
//   Entry 30000 starts "YMKs"+12 bytes, entry 30001 has no such head. Index: LE u32 N at +0x110 (30000) / +0x100 (30001),
//   N 16-byte records {4 bytes, u8 track, u8 0, LE u16 act, LE u32 offset, LE u32 size (2-frame steps)}, data at
//   index + 16 N + 4 + offset. An entry = tokens (< 96 skip steps; 96 = chunk with LE u16 length; else chunk of token-96).
//   Chunk = [optional prefix 0x80|n + n bytes] count (u8: the key lasts (count+1) steps), 3 LE float32 position, then
//     30000: 3 LE int16 angles (65536 = 360 deg), 3 LE int16 scales (colour = s / 4095)
//     30001: 3 LE float32 Euler angles (radians), float32 1.0
//   Light set `act`: track 0 = sky colour, 1 = ground colour, 10..14 = lights 0..4 (direction to the light = Rz Ry Rx (-1,0,0),
//   colour = scale / 4095). Slot motion `act`: world = Rz Ry Rx * p + key position / 100.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include "asset_model.h"
#include "pac.h"

namespace assets {
namespace stage_key {

// What the viewer uses (set by {"cmd":"lights","time":T,"mode":"match"|"entrance"} and applied at the next arena load).
// The times are game frames (30001 / 30000 keys sit every 2). [U] which time the game is at in a given scene: the defaults
// are the keys that best reproduce the live draw probes (stage_key.md section 5).
struct Settings {
  bool enabled = true;
  double time = 132;  // match default (the celebration probe's best key)
};
inline Settings& Cfg() {
  static Settings s;
  return s;
}
constexpr double kMatchTime = 132, kEntranceTime = 140;

struct Rec {
  uint8_t track = 0;
  uint16_t act = 0;
  uint32_t off = 0, size = 0;
  uint32_t end = 0;  // next record's offset (or the data end)
};
struct Index {
  const Bytes* d = nullptr;
  size_t base = 0;
  std::vector<Rec> rec;
  bool Parse(const Bytes& b) {
    d = &b;
    rec.clear();
    if (b.size() < 0x120) return false;
    const size_t idx = std::memcmp(b.data(), "YMKs", 4) == 0 ? 0x110 : 0x100;
    const uint32_t n = Le32(b, idx);
    if (n == 0 || n > 4096 || idx + 16 * size_t(n) + 4 > b.size()) return false;
    base = idx + 16 * size_t(n) + 4;
    for (uint32_t i = 0; i < n; ++i) {
      const size_t p = idx + 16 * size_t(i);
      Rec r;
      r.track = b[p + 4];
      r.act = uint16_t(b[p + 6] | b[p + 7] << 8);
      r.off = Le32(b, p + 8);
      r.size = Le32(b, p + 12);
      rec.push_back(r);
    }
    for (size_t i = 0; i < rec.size(); ++i) rec[i].end = i + 1 < rec.size() ? rec[i + 1].off : uint32_t(b.size() - base);
    return true;
  }
  const Rec* Find(int act, int track) const {
    for (const Rec& r : rec)
      if (r.act == act && r.track == track) return &r;
    return nullptr;
  }
};

struct Chunk {
  int step = 0;
  size_t at = 0, len = 0;  // body (after the prefix): count byte first
};
inline std::vector<Chunk> Chunks(const Index& ix, const Rec& r) {
  std::vector<Chunk> out;
  const Bytes& d = *ix.d;
  size_t p = ix.base + r.off;
  const size_t e = std::min(d.size(), ix.base + size_t(r.end));
  int step = 0;
  while (p < e) {
    const uint8_t t = d[p];
    if (t < 96) {
      step += t, ++p;
      continue;
    }
    size_t len, q;
    if (t == 96) {
      if (p + 3 > e) break;
      len = d[p + 1] | d[p + 2] << 8, q = p + 3;
    } else {
      len = size_t(t) - 96, q = p + 1;
    }
    if (q + len > d.size()) break;
    size_t at = q, l = len;
    if (l && (d[at] & 0x80)) {
      const size_t skip = size_t(d[at] & 0x7F) + 1;
      if (skip > l) break;
      at += skip, l -= skip;
    }
    out.push_back({step, at, l});
    ++step;
    p = q + len;
  }
  return out;
}

inline float F32(const Bytes& b, size_t o) {
  float f;
  std::memcpy(&f, b.data() + o, 4);
  return f;
}
inline int I16(const Bytes& b, size_t o) { return int16_t(b[o] | b[o + 1] << 8); }

struct Sample {
  double pos[3] = {0, 0, 0};
  double v[3] = {0, 0, 0};   // light: angles in int16 units; motion: Euler radians
  double sc[3] = {0, 0, 0};  // light: scales (int16 units)
  bool ok = false;
};

// Value of a track at game frame `frame` (loops over the entry's length): key -> next key linearly (angles the short way
// round for the int16 form). light = true: 30000 chunk layout, false: 30001.
inline Sample SampleTrack(const Index& ix, const Rec& r, double frame, bool light) {
  Sample s;
  const std::vector<Chunk> ch = Chunks(ix, r);
  const size_t need = light ? 25 : 29;
  std::vector<Chunk> ks;
  for (const Chunk& c : ch)
    if (c.len >= need) ks.push_back(c);
  if (ks.empty() || r.size == 0) return s;
  const double t = std::fmod(frame / 2.0, double(r.size));
  size_t ci = 0;
  for (size_t i = 0; i < ks.size(); ++i)
    if (ks[i].step <= t) ci = i;
  size_t ni = ci + 1 < ks.size() ? ci + 1 : (light ? ci : 0);
  const Chunk &a = ks[ci], &b = ks[ni];
  const Bytes& d = *ix.d;
  double gap = b.step > a.step ? double(b.step - a.step) : (ni == ci ? 1.0 : std::max(1.0, double(r.size) - a.step));
  double f = ni == ci ? 0.0 : std::min(1.0, std::max(0.0, (t - a.step) / gap));
  for (int k = 0; k < 3; ++k) {
    s.pos[k] = F32(d, a.at + 1 + 4 * k) + (F32(d, b.at + 1 + 4 * k) - F32(d, a.at + 1 + 4 * k)) * f;
    if (light) {
      const int av = I16(d, a.at + 13 + 2 * k), bv = I16(d, b.at + 13 + 2 * k);
      const int dl = ((bv - av + 32768) & 0xFFFF) - 32768;
      s.v[k] = av + dl * f;
      const int sa = I16(d, a.at + 19 + 2 * k), sb = I16(d, b.at + 19 + 2 * k);
      s.sc[k] = sa + (sb - sa) * f;
    } else {
      const float ea = F32(d, a.at + 13 + 4 * k), eb = F32(d, b.at + 13 + 4 * k);
      s.v[k] = ea + (eb - ea) * f;
    }
  }
  s.ok = true;
  return s;
}

// Column-vector rotation Rz Ry Rx of Euler angles (radians) as a 3x3 [row][col].
inline void EulerZYX(double ex, double ey, double ez, double R[3][3]) {
  const double cx = std::cos(ex), sx = std::sin(ex), cy = std::cos(ey), sy = std::sin(ey), cz = std::cos(ez), sz = std::sin(ez);
  R[0][0] = cz * cy, R[0][1] = cz * sy * sx - sz * cx, R[0][2] = cz * sy * cx + sz * sx;
  R[1][0] = sz * cy, R[1][1] = sz * sy * sx + cz * cx, R[1][2] = sz * sy * cx - cz * sx;
  R[2][0] = -sy, R[2][1] = cy * sx, R[2][2] = cy * cx;
}

// The evaluated light set `id` (4 lights + sky / ground). False when the key has no such set.
inline bool EvalLightSet(const Index& ix, int id, double frame, LightSet& out) {
  if (!ix.Find(id, 0) && !ix.Find(id, 10)) return false;
  out = LightSet();
  out.id = id;
  const double k = 2.0 * 3.14159265358979 / 65536.0;
  for (int i = 0; i < 4; ++i) {
    const Rec* r = ix.Find(id, 10 + i);
    if (!r) continue;
    const Sample s = SampleTrack(ix, *r, frame, true);
    if (!s.ok) continue;
    double R[3][3];
    EulerZYX(s.v[0] * k, s.v[1] * k, s.v[2] * k, R);
    for (int c = 0; c < 3; ++c) {
      out.dir[i][c] = float(-R[c][0]);
      out.col[i][c] = float(s.sc[c] / 4095.0);
    }
  }
  const Rec* sky = ix.Find(id, 0);
  const Rec* gnd = ix.Find(id, 1);
  const Sample ss = sky ? SampleTrack(ix, *sky, frame, true) : Sample();
  const Sample sg = gnd ? SampleTrack(ix, *gnd, frame, true) : Sample();
  for (int c = 0; c < 3; ++c) {
    out.sky[c] = ss.ok ? float(ss.sc[c] / 4095.0) : 0.f;
    out.ground[c] = sg.ok ? float(sg.sc[c] / 4095.0) : 0.f;
  }
  return true;
}

// The slot motion m(act) at `frame` as a FILE-space row-vector matrix (p' = p * M, translation in 12..14; run MirrorY for
// the viewer). False when the key has no such motion.
inline bool EvalMotion(const Index& ix, int act, double frame, Matrix& m) {
  const Rec* r = ix.Find(act, 0);
  if (!r) return false;
  const Sample s = SampleTrack(ix, *r, frame, false);
  if (!s.ok) return false;
  double R[3][3];
  EulerZYX(s.v[0], s.v[1], s.v[2], R);
  m = Identity();
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) m[i * 4 + j] = float(R[j][i]);  // row vectors: the transpose of the column form
  for (int k = 0; k < 3; ++k) m[12 + k] = float(s.pos[k] * 0.01);
  return true;
}

}  // namespace stage_key
}  // namespace assets
