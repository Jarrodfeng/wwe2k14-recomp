// Session 97 (motions): the motion pacs (pac\mot00..08.pac) -> per-track keys -> a pose for the viewer's wrestlers.
// Format: out\runtime\s97\motion.md (our reader tools\assets\yanm.py is the reference this is ported from). Read from the
// player's own game files at run time; nothing from the game is kept or shipped (CLAUDE.md rules 1 and 2).
//
// Covered: the DIRECT form (the ZLIB half of every act: tracks listed in the YANM itself). Not covered: the INDIRECT form
// (needs the layout tables of the executable: not copied, such motions are reported unsupported), the bit-0 tracks (the
// calves: solved by the game with an IK routine, sub_823DA1F0; here the bone keeps its bind rotation), spline
// interpolation (linear / slerp between keys), the YAT2 event table.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "asset_model.h"
#include "pac.h"

namespace assets {
namespace motion {

using Quat = std::array<double, 4>;  // x y z w

inline Quat QMul(const Quat& a, const Quat& b) {  // Hamilton a*b
  return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
          a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}
inline Quat QNorm(Quat q) {
  const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (n < 1e-12) return {0, 0, 0, 1};
  for (double& c : q) c /= n;
  return q;
}
inline Quat QSlerp(const Quat& a, Quat b, double t) {
  double d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  if (d < 0) {
    for (double& c : b) c = -c;
    d = -d;
  }
  if (d > 0.9995) {
    Quat r;
    for (int i = 0; i < 4; ++i) r[i] = a[i] + (b[i] - a[i]) * t;
    return QNorm(r);
  }
  const double th = std::acos(std::min(1.0, d)), s = std::sin(th), wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
  Quat r;
  for (int i = 0; i < 4; ++i) r[i] = wa * a[i] + wb * b[i];
  return r;
}
inline Quat EulerQuat(double ax, double ay, double az) {  // q = qz * qy * qx
  const Quat qx{std::sin(ax / 2), 0, 0, std::cos(ax / 2)}, qy{0, std::sin(ay / 2), 0, std::cos(ay / 2)},
      qz{0, 0, std::sin(az / 2), std::cos(az / 2)};
  return QMul(QMul(qz, qy), qx);
}
inline Quat AxisQuat(int axis, double ang) {
  Quat q{0, 0, 0, std::cos(ang / 2)};
  q[size_t(axis)] = std::sin(ang / 2);
  return q;
}

// The game's node-name hash (sub_823D2050 loop, reproduced in motion.md): h = ((h << 7) | c) ^ (h >> 25).
inline uint32_t NodeHash(const std::string& name) {
  uint32_t h = 0;
  for (unsigned char c : name) h = (((h << 7) | c) ^ (h >> 25));
  return h;
}

struct RotKey {
  int t;
  Quat q;
};
struct PosKey {
  int t;
  double p[3];
};
struct Track {
  uint32_t hash = 0;
  uint16_t flags = 0;
  std::vector<std::vector<RotKey>> rot;  // components multiplied in order (coarse * fine)
  bool has_pos = false, pos_const = false;
  double pos_c[3] = {0, 0, 0};
  std::vector<PosKey> pos;
  bool bit0 = false;         // a calf-style curve: a foot target for the leg IK, not a rotation
  std::vector<std::array<double, 5>> ik;  // bit 0 keys: {time, x, y, z, w} (x y z = the ankle target, cm, w = a knee value)
  std::string unsupported;   // non-empty: something in this track was not decoded
};
struct Motion {
  std::string name;
  std::vector<Track> tracks;
  int length = 0;  // last key time (frames of 1/60 s)
  std::string unsupported;  // non-empty: the whole motion could not be read (indirect form, ...)
  // Session 98 (stance): this motion's root rule, overriding PoseOpts::root_delta: 1 = the `Bip` sample is a delta on the default pose,
  // 0 = the sample as is, -1 = use the option. The match-start stances (mot00 CA01 top 30, acts 29000-29022: Bip y 0-5, quiet) only
  // stand with the delta (as is, the pelvis would sit 2-5 units above the floor); the old idle (CA01 top 40 act 23101, Bip y 76.6) as is.
  int root_delta = -1;
  const Track* Find(uint32_t hash) const {
    for (const Track& t : tracks)
      if (t.hash == hash) return &t;
    return nullptr;
  }
};

namespace detail {
constexpr double kPi = 3.14159265358979323846;
constexpr double kCoarse = kPi / 128, kFine = kPi / 4608, kFinest = kPi / 11520, kI16 = kPi / 32768;

inline int8_t S8(const uint8_t* p) { return int8_t(*p); }
inline int16_t S16(const uint8_t* p) { return int16_t(Be16(p)); }

inline int CompSize(int bit, uint16_t flags) {
  const int b5 = (flags >> 5) & 1;
  if (bit == 15) return 12 * (b5 + 2);
  if (bit == 8) return 8 * b5 + 24;
  if (bit == 14 || bit == 13 || bit == 12 || bit == 11 || bit == 10 || bit == 9 || bit == 4 || bit == 3 || bit == 2) return 8 * (b5 + 1);
  return 8;
}
constexpr int kOrder[13] = {15, 14, 13, 12, 11, 10, 9, 8, 4, 3, 2, 1, 0};

// bounds-checked view of the act block
struct Blk {
  const uint8_t* b;
  size_t n;
  bool ok(size_t o, size_t len) const { return o <= n && len <= n - o; }
};

inline std::vector<RotKey> Euler4(const Blk& k, size_t base, uint32_t ptr, uint32_t cnt, double scale, bool& bad) {
  std::vector<RotKey> out;
  if (!k.ok(base + ptr, size_t(cnt) * 4) || cnt > 100000) return bad = true, out;
  int64_t t = 0;  // (review: no int overflow over 100000 keys)
  for (uint32_t i = 0; i < cnt; ++i) {
    const uint8_t* o = k.b + base + ptr + 4 * i;
    t += o[3];
    out.push_back({int(std::min<int64_t>(t, 1 << 30)), EulerQuat(S8(o) * scale, S8(o + 1) * scale, S8(o + 2) * scale)});
  }
  return out;
}
inline std::vector<RotKey> Euler16(const Blk& k, size_t base, uint32_t ptr, uint32_t cnt, bool& bad) {
  std::vector<RotKey> out;
  if (!k.ok(base + ptr, size_t(cnt) * 8) || cnt > 100000) return bad = true, out;
  int64_t t = 0;  // (review: no int overflow over 100000 keys)
  for (uint32_t i = 0; i < cnt; ++i) {
    const uint8_t* o = k.b + base + ptr + 8 * i;
    t += Be16(o + 6);
    out.push_back({int(std::min<int64_t>(t, 1 << 30)), EulerQuat(S16(o) * kI16, S16(o + 2) * kI16, S16(o + 4) * kI16)});
  }
  return out;
}
inline std::vector<RotKey> Euler20(const Blk& k, size_t base, uint32_t ptr, uint32_t cnt, bool& bad) {
  std::vector<RotKey> out;
  if (!k.ok(base + ptr, size_t(cnt) * 20) || cnt > 100000) return bad = true, out;
  int64_t t = 0;  // (review: no int overflow over 100000 keys)
  for (uint32_t i = 0; i < cnt; ++i) {
    const uint8_t* o = k.b + base + ptr + 20 * i;
    t += S16(o);
    out.push_back({int(std::min<int64_t>(t, 1 << 30)), EulerQuat(S16(o + 2) * kI16, S16(o + 4) * kI16, S16(o + 6) * kI16)});
  }
  return out;
}
inline std::vector<RotKey> Hinge(const Blk& k, size_t base, uint32_t ptr, uint32_t cnt, int axis, double scale, bool& bad) {
  std::vector<RotKey> out;
  if (!k.ok(base + ptr, size_t(cnt) * 2) || cnt > 100000) return bad = true, out;
  int64_t t = 0;  // (review: no int overflow over 100000 keys)
  for (uint32_t i = 0; i < cnt; ++i) {
    const uint8_t* o = k.b + base + ptr + 2 * i;
    t += o[1];
    out.push_back({int(std::min<int64_t>(t, 1 << 30)), AxisQuat(axis, S8(o) * scale)});
  }
  return out;
}
// b15 (4-byte keys) / b8 (8-byte keys): record {ptr, count, scale, base x, y, z}; position = base + scale * running sum.
inline void PosKeys(const Blk& k, size_t base, size_t comp, bool wide, Track& tr) {
  tr.has_pos = true;
  const uint32_t ptr = Be32(k.b + comp), cnt = Be32(k.b + comp + 4);
  const double scale = BeF(k.b + comp + 8);
  const double bx = BeF(k.b + comp + 12), by = BeF(k.b + comp + 16), bz = BeF(k.b + comp + 20);
  if (!(scale >= 1e-7) || ptr == 0 || cnt == 0) {
    tr.pos_const = true;
    tr.pos_c[0] = bx, tr.pos_c[1] = by, tr.pos_c[2] = bz;
    return;
  }
  const size_t sz = wide ? 8 : 4;
  if (cnt > 100000 || !k.ok(base + ptr, size_t(cnt) * sz)) {
    tr.unsupported = "position keys out of range";
    tr.has_pos = false;
    return;
  }
  int64_t t = 0;  // (review: no int overflow over 100000 keys)
  double sx = 0, sy = 0, sz2 = 0;
  for (uint32_t i = 0; i < cnt; ++i) {
    const uint8_t* o = k.b + base + ptr + sz * i;
    if (wide) sx += S16(o), sy += S16(o + 2), sz2 += S16(o + 4), t += Be16(o + 6);
    else sx += S8(o), sy += S8(o + 1), sz2 += S8(o + 2), t += o[3];
    tr.pos.push_back({int(std::min<int64_t>(t, 1 << 30)), {bx + sx * scale, by + sy * scale, bz + sz2 * scale}});
  }
}

inline double Half(uint16_t h) {
  const double s = (h & 0x8000) ? -1.0 : 1.0;
  const int e = (h >> 10) & 31, m = h & 1023;
  if (e == 0) return s * m * std::ldexp(1.0, -24);
  if (e == 31) return m ? 0.0 : s * 65504.0;
  return s * (1 + m / 1024.0) * std::ldexp(1.0, e - 15);
}
// bit 0, mode & 7 == 1 (single stream): a 16 byte header of eight float16 (4 bases, 4 scales) unless the key format is 3
// (floats), then keys = u8 time delta + a packed 4-vector: format (mode >> 3) & 7: 0 = 4 x u8, 1 = 4 x 12 bit, 2 = 4 x u16
// (two planes), 3 = 4 floats (motion.md section 3). Other modes are not decoded.
inline bool Packed4(const Blk& k, size_t base, uint16_t mode, uint16_t cnt, uint32_t ptr, Track& tr) {
  const int fmt = (mode >> 3) & 7;
  if ((mode & 7) != 1 || fmt > 3) return false;
  static const int kStride[4] = {4, 6, 8, 16};
  size_t o = base + ptr;
  double A[4] = {0, 0, 0, 0}, B[4] = {0, 0, 0, 0};
  if (fmt != 3) {
    if (!k.ok(o, 16)) return false;
    for (int i = 0; i < 4; ++i) A[i] = Half(Be16(k.b + o + 2 * i)), B[i] = Half(Be16(k.b + o + 8 + 2 * i));
    o += 16;
  }
  const size_t stride = size_t(kStride[fmt]) + 1;
  if (cnt > 100000 || !k.ok(o, stride * cnt)) return false;
  int64_t t = 0;  // (review: no int overflow over 100000 keys)
  for (uint16_t i = 0; i < cnt; ++i) {
    const uint8_t* q = k.b + o + stride * i;
    t += q[0];
    const uint8_t* d = q + 1;
    double c[4];
    if (fmt == 0) for (int j = 0; j < 4; ++j) c[j] = d[j];
    else if (fmt == 1) c[0] = (d[0] << 4) | (d[4] >> 4), c[1] = (d[1] << 4) | (d[4] & 15), c[2] = (d[2] << 4) | (d[5] >> 4), c[3] = (d[3] << 4) | (d[5] & 15);
    else if (fmt == 2) for (int j = 0; j < 4; ++j) c[j] = (d[j] << 8) | d[4 + j];
    std::array<double, 5> key{double(t), 0, 0, 0, 0};
    for (int j = 0; j < 4; ++j) key[size_t(1 + j)] = fmt == 3 ? double(BeF(d + 4 * j)) : A[j] + B[j] * c[j];
    tr.ik.push_back(key);
  }
  return true;
}

inline void BuildTrack(const Blk& k, size_t base, Track& tr, const std::map<int, size_t>& at) {
  const uint16_t fl = tr.flags;
  if ((fl >> 5) & 1) {
    tr.unsupported = "b5 (second key set)";
    return;
  }
  bool bad = false;
  auto kc = [&](int bit, uint32_t& ptr, uint32_t& cnt) {
    ptr = Be32(k.b + at.at(bit)), cnt = Be32(k.b + at.at(bit) + 4);
  };
  uint32_t ptr, cnt;
  if (fl & 0x0001) {
    tr.bit0 = true;  // not rotations: the game feeds them to an IK routine (sub_823DA1F0), see BuildSkin
    uint16_t mode = Be16(k.b + at.at(0)), c0 = Be16(k.b + at.at(0) + 2);
    uint32_t p0 = Be32(k.b + at.at(0) + 4);
    if (!Packed4(k, base, mode, c0, p0, tr)) tr.unsupported = "b0 mode not decoded";
  } else if (fl & 0x2000) {
    kc(13, ptr, cnt);
    tr.rot.push_back(Euler16(k, base, ptr, cnt, bad));
  } else if (fl & 0x4000) {
    kc(14, ptr, cnt);
    tr.rot.push_back(Euler4(k, base, ptr, cnt, kCoarse, bad));
    if (fl & 0x1000) {
      kc(12, ptr, cnt);
      tr.rot.push_back(Euler4(k, base, ptr, cnt, kFine, bad));
    } else if (fl & 0x0010) {
      kc(4, ptr, cnt);
      tr.rot.push_back(Euler4(k, base, ptr, cnt, kFinest, bad));
    }
  } else if (fl & 0x0800) {
    bool extra = false;
    const int bits[3] = {11, 10, 3};
    const double scs[3] = {kCoarse, kFine, kFinest};
    for (int j = 0; j < 3; ++j) {
      const int bit = bits[j];
      if (!(fl & (1 << bit)) || (bit != 11 && extra)) continue;
      const uint32_t p = Be32(k.b + at.at(bit)), c = Be16(k.b + at.at(bit) + 4), axis = Be16(k.b + at.at(bit) + 6);
      if (axis > 2) {
        tr.unsupported = "hinge axis";
        continue;
      }
      tr.rot.push_back(Hinge(k, base, p, c, int(axis), scs[j], bad));
      if (bit != 11) extra = true;
    }
  } else if (fl & 0x0004) {
    kc(2, ptr, cnt);
    tr.rot.push_back(Euler20(k, base, ptr, cnt, bad));
  }
  if (bad) tr.unsupported = "key data out of range";
  if (fl & 0x8000) PosKeys(k, base, at.at(15), false, tr);
  else if (fl & 0x0100) PosKeys(k, base, at.at(8), true, tr);
  const uint16_t known = 0x8000 | 0x4000 | 0x2000 | 0x1000 | 0x0800 | 0x0400 | 0x0100 | 0x0010 | 0x0008 | 0x0004 | 0x0001;
  if (fl & ~known & 0xFFDF) tr.unsupported = "flags";
}
}  // namespace detail

// One act (leaf of the motion pac): the name table and the YANM chunk offsets.
struct Act {
  Bytes blk;
  std::vector<std::string> motion_names;  // the 8-digit names, in YANM order
  std::vector<size_t> yanm;               // offsets of the 'YANM' tags
};
inline bool ParseAct(Bytes blk, Act& a) {
  a = Act();
  a.blk = std::move(blk);
  const Bytes& b = a.blk;
  if (b.size() < 0x28) return false;
  const uint32_t end_names = Be32(b.data()), name_off = Be32(b.data() + 8), first = Be32(b.data() + 12);
  if (end_names > b.size() || name_off > end_names) return false;
  // (Session 98: the table's last entry can be shorter than 40 bytes - Orton's act 25000 lost its fifth name to `o + 40 <= end_names`.
  // Entries before the first motion name (root, act name) are skipped; a later entry that is not a motion name keeps its place as ""
  // so the names stay parallel to the motions.)
  // (the last name can start past `end_names` - the +0 field is not the table end for every act - so the scan runs to the first motion)
  const size_t names_end = std::max<size_t>(end_names, std::min<size_t>(first, b.size()));
  for (size_t o = name_off; o + 12 <= names_end; o += 40) {
    std::string nm;
    for (size_t i = 0; i < 32 && o + 4 + i < b.size() && b[o + 4 + i]; ++i) nm += char(b[o + 4 + i]);
    if (nm.size() == 8 && std::all_of(nm.begin(), nm.end(), [](char c) { return c >= '0' && c <= '9'; })) a.motion_names.push_back(nm);
    else if (!a.motion_names.empty()) a.motion_names.push_back("");
  }
  while (!a.motion_names.empty() && a.motion_names.back().empty()) a.motion_names.pop_back();
  size_t p = first;
  while (p + 8 <= b.size()) {
    if (!std::memcmp(b.data() + p, "YANM", 4) || !std::memcmp(b.data() + p, "POF0", 4) || !std::memcmp(b.data() + p, "YAT2", 4)) {
      if (!std::memcmp(b.data() + p, "YANM", 4)) a.yanm.push_back(p);
      p += 8 + Be32(b.data() + p + 4);
    } else {
      p += 4;
    }
  }
  return !a.yanm.empty();
}

// The tracks of the i-th motion of the act. `unsupported` is set when the motion is not the direct form.
inline Motion ParseMotion(const Act& a, size_t i) {
  Motion mo;
  mo.name = i < a.motion_names.size() ? a.motion_names[i] : "?";
  if (i >= a.yanm.size()) return mo.unsupported = "no such motion", mo;
  const detail::Blk k{a.blk.data(), a.blk.size()};
  const size_t base = a.yanm[i] + 8;
  if (!k.ok(base, 8)) return mo.unsupported = "truncated", mo;
  const uint8_t b1 = a.blk[base + 1];
  const uint32_t cnt = Be16(a.blk.data() + base + 2), ptr = Be32(a.blk.data() + base + 4);
  if (b1 & 2) return mo.unsupported = "indirect form (needs the executable's layout tables)", mo;
  // Session 98 (DLC): 2048, was 200 - prop rigs carry one track per node (Fandango's stage set 2591: 659-node model, its leaf 12
  // motions were all refused; 412's 2762 / 413's 2414 have 432 / 730). Every track stays bounds-checked and under the key budget.
  if (cnt > 2048) return mo.unsupported = "track count", mo;
  size_t q = base + ptr;
  size_t keys_total = 0;
  for (uint32_t t = 0; t < cnt; ++t) {
    if (!k.ok(q, 8)) return mo.unsupported = "track directory out of range", mo;
    Track tr;
    tr.flags = Be16(a.blk.data() + q);
    const uint32_t sz = Be16(a.blk.data() + q + 2);
    tr.hash = Be32(a.blk.data() + q + 4);
    std::map<int, size_t> at;
    size_t o = q + 8;
    for (int bit : detail::kOrder)
      if (tr.flags & (1 << bit)) {
        at[bit] = o;
        o += size_t(detail::CompSize(bit, tr.flags));
      }
    if (o != q + sz || !k.ok(q, sz)) tr.unsupported = "size mismatch";
    else detail::BuildTrack(k, base, tr, at);
    // (S97 review: a key budget per motion - tracks pointing at the same bytes must not add up to gigabytes)
    for (const auto& c : tr.rot) keys_total += c.size();
    keys_total += tr.pos.size() + tr.ik.size();
    if (keys_total > 1000000) return mo.tracks.clear(), mo.unsupported = "too many keys", mo;
    mo.tracks.push_back(std::move(tr));
    q += sz;
  }
  for (const Track& tr : mo.tracks) {
    for (const auto& c : tr.rot)
      if (!c.empty()) mo.length = std::max(mo.length, c.back().t);
    if (!tr.pos.empty()) mo.length = std::max(mo.length, tr.pos.back().t);
    if (!tr.ik.empty()) mo.length = std::max(mo.length, int(tr.ik.back()[0]));  // (review: calf curves count too)
  }
  return mo;
}

namespace detail {
template <class K>
inline void Hold(const std::vector<K>& keys, double f, size_t& i, double& u) {
  i = 0, u = 0;
  if (keys.size() < 2 || !(f == f) || f <= keys.front().t) return;  // (review: one key / NaN -> key 0)
  if (f >= keys.back().t) return i = keys.size() - 1, void();
  size_t lo = 0, hi = keys.size() - 1;
  while (hi - lo > 1) {
    const size_t mid = (lo + hi) / 2;
    (keys[mid].t <= f ? lo : hi) = mid;
  }
  const int span = keys[lo + 1].t - keys[lo].t;
  i = lo, u = span > 0 ? (f - keys[lo].t) / span : 0.0;
}
}  // namespace detail

// Local rotation at frame f (linear / slerp between keys; components multiply in order like the game). False: no rotation.
inline bool SampleRot(const Track& tr, double f, Quat& out) {
  bool first = true;
  Quat q{0, 0, 0, 1};
  for (const auto& keys : tr.rot) {
    if (keys.empty()) continue;
    size_t i;
    double u;
    detail::Hold(keys, f, i, u);
    const Quat v = u == 0.0 ? keys[i].q : QSlerp(keys[i].q, keys[i + 1].q, u);
    q = first ? v : QMul(q, v);
    first = false;
  }
  out = q;
  return !first;
}
inline bool SamplePos(const Track& tr, double f, double out[3]) {
  if (!tr.has_pos) return false;
  if (tr.pos_const) {
    for (int k = 0; k < 3; ++k) out[k] = tr.pos_c[k];
    return true;
  }
  if (tr.pos.empty()) return false;
  size_t i;
  double u;
  detail::Hold(tr.pos, f, i, u);
  for (int k = 0; k < 3; ++k) out[k] = u == 0.0 ? tr.pos[i].p[k] : tr.pos[i].p[k] + (tr.pos[i + 1].p[k] - tr.pos[i].p[k]) * u;
  return true;
}

// ---- the library: the mot pacs -----------------------------------------------------------------------------------------

struct ActRef {
  uint32_t top = 0, act = 0;
  bool zlib = false;
  size_t off = 0, size = 0;  // the leaf in the pac buffer
};

class Pack {
 public:
  Bytes d;  // the whole pac (plain EPK8, not XCTD)
  struct Bank {
    std::string name;
    size_t off = 0, size = 0;
  };
  std::vector<Bank> banks;

  bool Parse(std::string& err) {
    banks.clear();
    if (d.size() < 0x4000 || std::memcmp(d.data(), "EPK8", 4) != 0) return err = "not an EPK8 motion pac", false;
    for (size_t o = 0x80C; o + 16 <= 0x4000; o += 16) {
      std::string n;
      for (int i = 0; i < 8 && d[o + i] && d[o + i] != ' '; ++i) n += char(d[o + i]);
      if (n.empty()) break;
      Bank b;
      b.name = n, b.off = 0x4000 + size_t(Le32(d, o + 8)) * 0x800, b.size = size_t(Le32(d, o + 12)) * 256;
      if (b.off < d.size()) banks.push_back(b);
    }
    return !banks.empty() || (err = "no banks", false);
  }
  const Bank* FindBank(const std::string& n) const {
    for (const Bank& b : banks)
      if (b.name == n) return &b;
    return nullptr;
  }
  // Every act leaf of a bank (both halves).
  std::vector<ActRef> Acts(const Bank& bk) const {
    std::vector<ActRef> out;
    pac::Pach tops;
    if (!pac::ParsePach(d, bk.off, d.size(), tops)) return out;
    for (const pac::Ent& te : tops.ents) {
      pac::Pach mids;
      if (!pac::ParsePach(d, te.off, d.size(), mids)) continue;
      for (const pac::Ent& me : mids.ents) {
        pac::Pach leaves;
        if (!pac::ParsePach(d, me.off, d.size(), leaves)) continue;
        for (const pac::Ent& le : leaves.ents) {
          if (le.id >= 65000 || le.size < 16) continue;
          const bool z = !std::memcmp(d.data() + le.off, "ZLIB", 4), bpe = !std::memcmp(d.data() + le.off, "BPE ", 4);
          if (!z && !bpe) continue;
          ActRef r;
          r.top = te.id, r.act = le.id, r.zlib = z, r.off = le.off, r.size = le.size;
          out.push_back(r);
        }
      }
    }
    return out;
  }
  bool LoadAct(const ActRef& r, Act& a, std::string& err) const {
    Bytes blk = pac::Decode(d.data() + r.off, r.size);
    if (blk.empty()) return err = "leaf did not decode", false;
    if (!ParseAct(std::move(blk), a)) return err = "act block not understood", false;
    return true;
  }
};

// ---- posing ------------------------------------------------------------------------------------------------------------

using V3 = std::array<double, 3>;
using M3 = std::array<double, 9>;  // row-major rotation, column-vector use: v' = R v (file space)
inline V3 Sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline V3 Add(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline V3 Scale(const V3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
inline double Dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline V3 Cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
inline double Len(const V3& a) { return std::sqrt(Dot(a, a)); }
inline V3 Unit(const V3& a, const V3& dflt) {
  const double l = Len(a);
  return l < 1e-9 ? dflt : Scale(a, 1.0 / l);
}
inline M3 MMul(const M3& a, const M3& b) {
  M3 r{};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      for (int k = 0; k < 3; ++k) r[size_t(i * 3 + j)] += a[size_t(i * 3 + k)] * b[size_t(k * 3 + j)];
  return r;
}
inline V3 MVec(const M3& a, const V3& v) {
  return {a[0] * v[0] + a[1] * v[1] + a[2] * v[2], a[3] * v[0] + a[4] * v[1] + a[5] * v[2], a[6] * v[0] + a[7] * v[1] + a[8] * v[2]};
}
inline M3 MFromQuat(const double q[4]) {  // the same column-form rotation as asset_model.h FromQuatT
  const double x = q[0], y = q[1], z = q[2], w = q[3];
  return {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 2 * (x * y + z * w), 1 - 2 * (x * x + z * z),
          2 * (y * z - x * w),     2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)};
}
// The rotation that carries unit vector a onto unit vector b by the shortest way.
inline M3 Arc(const V3& a, const V3& b) {
  const V3 v = Cross(a, b);
  const double c = Dot(a, b);
  M3 I{1, 0, 0, 0, 1, 0, 0, 0, 1};
  if (c < -0.99999) {  // opposite: half a turn about any axis normal to a
    V3 n = Unit(Cross(a, V3{1, 0, 0}), V3{0, 1, 0});
    if (Len(Cross(a, V3{1, 0, 0})) < 1e-3) n = Unit(Cross(a, V3{0, 1, 0}), V3{0, 0, 1});
    return {2 * n[0] * n[0] - 1, 2 * n[0] * n[1], 2 * n[0] * n[2], 2 * n[1] * n[0], 2 * n[1] * n[1] - 1, 2 * n[1] * n[2],
            2 * n[2] * n[0], 2 * n[2] * n[1], 2 * n[2] * n[2] - 1};
  }
  const M3 K{0, -v[2], v[1], v[2], 0, -v[0], -v[1], v[0], 0};
  const M3 K2 = MMul(K, K);
  M3 r;
  for (size_t i = 0; i < 9; ++i) r[i] = I[i] + K[i] + K2[i] / (1.0 + c);
  return r;
}

// bit-0 curve at frame f: {x, y, z, w} (linear between keys). False when the track has none.
inline bool SampleIk(const Track& tr, double f, double out[4]) {
  if (tr.ik.empty()) return false;
  size_t lo = 0;
  while (lo + 1 < tr.ik.size() && tr.ik[lo + 1][0] <= f) ++lo;
  const auto& a = tr.ik[lo];
  const auto& b = tr.ik[std::min(lo + 1, tr.ik.size() - 1)];
  const double span = b[0] - a[0], u = span > 0 && f > a[0] ? std::min(1.0, (f - a[0]) / span) : 0.0;
  for (int k = 0; k < 4; ++k) out[k] = a[size_t(k + 1)] + (b[size_t(k + 1)] - a[size_t(k + 1)]) * u;
  return true;
}

struct PoseOpts {
  // How a motion rotation meets the bone's bind rotation (the node record's): 0 = the motion replaces it (confirmed by the
  // numbers: Pelvis, Spine, Toe frames of the idle equal the bind rotations), 1 = bind * motion, 2 = motion * bind.
  int rot_mode = 0;
  bool conj = false;         // use the inverse quaternion (unconfirmed; false is the one that matches the node records)
  int flip = 2;              // bit mask of motion translation axes to negate (bit 1 = y: the files are -Y up)
  // Translation scale: 1 = the motion's positions are in the rig's own units (the entrances always ran at 1: their feet land on the
  // stage / ramp). <= 0 = the old auto rule bind pelvis height / ref_pelvis (A/B only: {"cmd":"pose","tscale":-1}).
  // Session 98 (match look): the auto rule was the cause of the floating default idle. Its 76.6 is the pelvis of the idle itself
  // (mot00 CA01 40 23101, a knees-bent stance), not a standing height, so it scaled every in-ring motion by 96.55 / 76.6 = 1.26: the
  // pelvis rose 20 units, the ankle targets (y 12.3) went out of reach (hip-target 93.2 > thigh + calf 87), the legs came out straight
  // and the feet 10 units above the pose's own ground (the reground then hung the wrestler by his toes). At 1: hip-target 73.6, knees
  // 64 degrees, and the lowest foot vertex lands on the pose's own ground by itself (-0.9 / -0.2 / +0.1 for the Rock / Orton / Cena).
  double tscale = 1.0;
  double ref_pelvis = 76.6;  // (the auto rule's reference: the idle's own pelvis height, see above)
  bool trans = true;         // apply the Bip translation
  bool vector_track = true;  // apply the track of the root node `vector`
  bool no_tracks = false;    // debug: ignore every track (the pose must equal the bind pose: identity skin)
  bool leg_ik = true;        // the legs: a two-bone solve to the bit-0 foot target
  int leg_solver = 2;        // 2 = the game's solver sub_823DA1F0 ported (Session 97), 1 = our earlier approximation (kept to compare)
  int leg_axis = 1;          // the hinge axis of the thigh frame the solver uses: 1 = Y (stream type 1 = solver mode 0, the only type decoded), 2 = Z (mode 2)
  // Session 98 (motion rule): a track's position is a DELTA on the player's default pose (sub_823D9A40 0x823DA06C-0x823DA0A8: local
  // translation = default[node].t (+16) + sampled when [player+268] is set; [player+268] = the skeleton's node table, set by sub_823D8218
  // 0x823D8330 from the skeleton's vtable slot 4; its records have the node-record layout: +16 translation, +32 euler, which
  // sub_823E2880 0x823E2F7C reads as the default rotation of a node with no track). Here: default = the bind node record.
  // pos_rule 1 = that rule for every node; 0 = the earlier name rule (only non-`Bip ...` nodes take their track's position, as is).
  int pos_rule = 1;
  // The root-motion node (`Bip`, [skeleton+288]): true = default + sampled like every node (the game's code path, and what the entrance
  // data need: hip and ankle targets then sit a leg apart); false = the sample as is. The in-ring motions (the idle: Bip y 76.55 on a rig
  // whose bind Bip is -96.55) only stand with the sample as is (scaled, flipped), so their player's default pose cannot be the model's
  // bind table: [U] what [player+268] holds for an in-ring player (a game trace decides). Entrances set true.
  bool root_delta = false;
};

// The skinning palette (viewer space, row vectors): per bone inv_bind * MirrorY(pose world). `info` gets a short note.
//
// Legs. The thigh tracks of the motions are NOT the final thigh rotations: the game replaces thigh and calf with the result
// of a two-bone IK (sub_823DA1F0) to a target kept in the calf's bit-0 curve. The reading used here (unconfirmed, judged by the
// picture): the curve's x y z is the ankle target in the character's root space (cm, Y up; left foot x > 0), bone lengths
// from the bind pose, the knee bends towards -Z (the forward direction of the motion data, hands and head lean to -Z), and
// a target beyond reach gives a straight leg towards it. The curve's w is not used.
inline std::vector<Matrix> BuildSkin(const std::vector<Bone>& bones, const Motion& mo, double frame, const PoseOpts& opt,
                                     std::string* info = nullptr) {
  const size_t n = bones.size();
  std::vector<Matrix> skin(n);
  std::vector<M3> Wc(n), Wb(n);
  std::vector<V3> pos(n), pb(n);
  std::vector<int> par(n);  // effective parents (see "orphans" below)
  auto chain = [&](M3& W, V3& P, size_t i, const M3& R, const V3& t, const std::vector<M3>& Wv, const std::vector<V3>& Pv) {
    const int p = par[i];
    if (p >= 0 && size_t(p) < i) W = MMul(Wv[size_t(p)], R), P = Add(Pv[size_t(p)], MVec(Wv[size_t(p)], t));
    else W = R, P = t;
  };
  // bind chain in file space (for bone lengths and bind directions)
  for (size_t i = 0; i < n; ++i) par[i] = bones[i].parent;
  for (size_t i = 0; i < n; ++i) {
    const double q[4] = {bones[i].local_rotation[0], bones[i].local_rotation[1], bones[i].local_rotation[2], bones[i].local_rotation[3]};
    const V3 t{bones[i].local_translation[0], bones[i].local_translation[1], bones[i].local_translation[2]};
    chain(Wb[i], pb[i], i, MFromQuat(q), t, Wb, pb);
  }
  // Orphans: gear pieces (elbow pads, ...) bring nodes with no parent (the piece's own root, e.g. `l_elbow`). The game must
  // attach them to the skeleton by some other rule (unconfirmed: not found). Here an orphan follows the bone whose bind
  // position is nearest to it (lower index only), keeping its bind offset, so a pad follows its arm instead of floating
  // at its T-pose place. The skeleton roots (vector, null, root) stay as they are.
  std::vector<M3> adopt_R(n);
  std::vector<V3> adopt_t(n);
  int orphans = 0;
  // (Session 97 integration: a piece's own root is often literally named "root" (the Rock's elbow pads: `root` ->
  // `l_elbow`), so a name test kept the pads at their T-pose place. Every node of a parentless piece subtree (index >= 3)
  // now adopts the nearest MAIN skeleton bone by bind position; piece nodes are never candidates.)
  std::vector<char> in_piece(n, 0);
  for (size_t i = 0; i < n; ++i)
    in_piece[i] = i >= 3 && (bones[i].parent < 0 || (size_t(bones[i].parent) < i && in_piece[size_t(bones[i].parent)]));
  for (size_t i = 3; i < n; ++i) {
    if (!in_piece[i]) continue;
    int best = -1;
    double bd = 1e30;
    for (size_t j = 2; j < i; ++j) {
      if (in_piece[j] || (bones[j].parent < 0 && par[j] < 0)) continue;
      const double dd = Len(Sub(pb[i], pb[j]));
      if (dd < bd) bd = dd, best = int(j);
    }
    if (best < 0) continue;
    par[i] = best;
    // bind local: world_bind(i) = world_bind(best) * local  ->  local = Wb[best]^T * ...
    M3 Tp;
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) Tp[size_t(r * 3 + c)] = Wb[size_t(best)][size_t(c * 3 + r)];
    adopt_R[i] = MMul(Tp, Wb[i]);
    adopt_t[i] = MVec(Tp, Sub(pb[i], pb[size_t(best)]));
    ++orphans;
  }
  double scale = opt.tscale;
  if (scale <= 0) {
    scale = 1;
    for (const Bone& b : bones)
      if (b.name == "Bip") {
        const double h = std::sqrt(double(b.local_translation[0]) * b.local_translation[0] + double(b.local_translation[1]) * b.local_translation[1] +
                                   double(b.local_translation[2]) * b.local_translation[2]);
        if (h > 1.0) scale = h / opt.ref_pelvis;
        break;
      }
  }
  auto find = [&](const char* nm) {
    for (size_t i = 0; i < n; ++i)
      if (bones[i].name == nm) return int(i);
    return -1;
  };
  struct Leg { int thigh, calf, foot; };
  std::vector<Leg> legs;
  for (const char* side : {"L", "R"}) {
    const std::string s = side;
    Leg l{find(("Bip " + s + " Thigh").c_str()), find(("Bip " + s + " Calf").c_str()), find(("Bip " + s + " Foot").c_str())};
    if (l.thigh >= 0 && l.calf > l.thigh && l.foot > l.calf) legs.push_back(l);
  }
  std::vector<int> calf_target(n, -1);  // index into `cw` (a calf world rotation decided at its thigh)
  std::vector<M3> cw;
  int mapped = 0, iks = 0;
  std::string legnote;
  for (size_t i = 0; i < n; ++i) {
    const Bone& b = bones[i];
    V3 t{b.local_translation[0], b.local_translation[1], b.local_translation[2]};
    double q[4] = {b.local_rotation[0], b.local_rotation[1], b.local_rotation[2], b.local_rotation[3]};
    const Track* tr = opt.no_tracks ? nullptr : mo.Find(NodeHash(b.name));
    if (tr && !(b.name == "vector" && !opt.vector_track)) {
      Quat m;
      if (SampleRot(*tr, frame, m)) {
        ++mapped;
        if (opt.conj) m[0] = -m[0], m[1] = -m[1], m[2] = -m[2];
        const Quat bq{q[0], q[1], q[2], q[3]};
        const Quat r = opt.rot_mode == 1 ? QMul(bq, m) : opt.rot_mode == 2 ? QMul(m, bq) : m;
        for (int k = 0; k < 4; ++k) q[k] = r[size_t(k)];
      }
      double p[3];
      // Positions (PoseOpts::pos_rule). The game adds the sampled position to the default pose's translation for every node
      // (sub_823D9A40 0x823DA06C-0x823DA0A8, no flag test: bit 9 / 0x0200 is never read there). So the entrance's 30 finger tracks
      // (flags 0x2300, position ~0) leave the bind offsets as they are (S98 hands probe: taking the ~0 as the offset collapsed the
      // fingers), and the entrance `Bip` sample (y ~125 on the ramp) lands 96.55 lower, a leg's length above the ankle targets.
      if (opt.trans && SamplePos(*tr, frame, p)) {
        const bool root = b.name == "Bip";
        const bool root_d = mo.root_delta >= 0 ? mo.root_delta != 0 : opt.root_delta;  // (Session 98 stance: the motion's own rule first)
        const bool delta = opt.pos_rule == 1 ? (!root || root_d) : false;
        const bool take = opt.pos_rule == 1 || b.name.compare(0, 4, "Bip ") != 0;  // (rule 0: the earlier name rule, for the A/B)
        if (take)
          for (int k = 0; k < 3; ++k) t[size_t(k)] = (delta ? t[size_t(k)] : 0.0) + p[k] * scale * ((opt.flip >> k) & 1 ? -1.0 : 1.0);
      }
    }
    if (par[i] != bones[i].parent) chain(Wc[i], pos[i], i, adopt_R[i], adopt_t[i], Wc, pos);  // an adopted orphan keeps its bind offset
    else chain(Wc[i], pos[i], i, MFromQuat(q), t, Wc, pos);
    if (calf_target[i] >= 0) Wc[i] = cw[size_t(calf_target[i])];
    if (opt.leg_ik)
      for (const Leg& lg : legs) {
        if (int(i) != lg.thigh) continue;
        const Track* ct = opt.no_tracks ? nullptr : mo.Find(NodeHash(bones[size_t(lg.calf)].name));
        double v[4];
        if (!ct || !SampleIk(*ct, frame, v)) continue;
        const V3 H = pos[i];
        // The ankle target is used as is, in the same space as the FK world matrices ([player+264]): sub_823E2210 only mirrors x and
        // scales it about the root-motion node's world position by [player+236] (0x823E2490-0x823E25B4; [player+240] = that node's
        // world translation, sub_823E3000 0x823E35D0), then sub_823DA1F0 takes target - hip (0x823DA314). No default-pose term.
        V3 A{v[0] * scale * ((opt.flip & 1) ? -1.0 : 1.0), v[1] * scale * ((opt.flip & 2) ? -1.0 : 1.0), v[2] * scale * ((opt.flip & 4) ? -1.0 : 1.0)};
        const double l1 = Len(Sub(pb[size_t(lg.calf)], pb[i])), l2 = Len(Sub(pb[size_t(lg.foot)], pb[size_t(lg.calf)]));
        if (l1 < 1e-3 || l2 < 1e-3) continue;
        if (opt.leg_solver == 2) {
          // ---- Port of sub_823DA1F0 (called from sub_823E2210 0x823E27A8), solver mode 0 (axis Y) / 2 (axis Z). The game uses row
          // vectors and matrices whose rows are the bone axes in world; here Wc has them as columns (the same thing transposed).
          // 0x823DA268-0x823DA2F0: the hinge axis = the thigh's own local Y (Z) axis in world (the thigh's FK matrix, from its track).
          // Note l1 / l2 are the table's [20] / [24] (f30 / f31); taken as thigh / calf bind length (the table was not read).
          const int ax = opt.leg_axis;
          const V3 axn = Unit(V3{Wc[i][size_t(ax)], Wc[i][size_t(3 + ax)], Wc[i][size_t(6 + ax)]}, V3{0, 1, 0});
          // 0x823DA314-0x823DA37C: D = target - hip (the hip = the thigh's position, 0x823DA304 sub_823D8188).
          const V3 D = Sub(A, H);
          const double dist = Len(D);  // 0x823DA418 sub_82C327F0 of the target in the frame below (it lies on the frame's X axis)
          if (dist < 1e-6) continue;
          // 0x823DA398 sub_823D6A70: the frame F, rows X = Dn, Y = (Dn x axn) x Dn, Z = Dn x axn (the axis normalised, the cross
          // products normalised by sub_82324380; a zero cross product stays zero in the game, here the leg keeps its FK pose).
          const V3 Dn = Scale(D, 1.0 / dist);
          const V3 c1 = Cross(Dn, axn);
          if (Len(c1) < 1e-6) continue;
          const V3 F2 = Unit(c1, V3{0, 0, 1});
          const V3 F1 = Unit(Cross(F2, Dn), V3{0, 1, 0}), F0 = Dn;
          // 0x823DA3F0-0x823DA4B0: the knee value w (radians) widens the reach: reach_w = |(l1 + l2 cos w, l2 sin w)|
          // (0x823DA3F4 sub_82C7A548 = cos, 0x823DA408 sub_82C7A468 = sin: checked, they are sin(|x| + pi/2) and sin),
          // blend t = clamp01((w - pi/6) * -5.7296) (constants 0x8200F2FC / 0x82010034: t = 1 below 20 degrees, 0 above 30),
          // effective distance = dist + (reach_w - dist) * t, the target scaled to it along the frame's X axis
          // (0x823DA484-0x823DA49C; the target is on X, so y = z = 0 after the scaling).
          const double w = v[3];
          const double tb = std::max(0.0, std::min(1.0, (w - 0.5235987901687622) * -5.729578018188477));
          const double reach_w = std::sqrt((l1 + l2 * std::cos(w)) * (l1 + l2 * std::cos(w)) + (l2 * std::sin(w)) * (l2 * std::sin(w)));
          const double dx = dist + (reach_w - dist) * tb;
          // 0x823DA4AC-0x823DA4D4: cos of the bend by the law of cosines, clamped to [-1, 1] (0x823DA4B8 constants -1 / 1)
          const double cosv = std::max(-1.0, std::min(1.0, (dx * dx - l1 * l1 - l2 * l2) / (2.0 * l1 * l2)));
          // 0x823DA4D8 sub_823A59A8 = acos: the bend angle (0 = straight); 0x823DA4E0 sub_822B4C60 = sin of it
          const double kb = std::acos(cosv), sk = std::sin(kb);
          // 0x823DA4E4-0x823DA4FC: the thigh's swing off the target line (sub_82C7A898 = atan2) as the game writes it,
          // with the numerator l1 * sin (not l2 as a textbook solve would have; equal when the bones are equally long)
          const double f13 = cosv * l2 + l1, f0 = sk * l1;
          const double th = std::atan2(-f0 * dx, f13 * dx);
          // 0x823DA524-0x823DA5D4: the thigh's world frame = the frame F turned about its Y axis (Z axis for mode 2) by th
          // (sub_82C335A0 / sub_82C335F0 build the turn and multiply, 0x82296630); its local rotation = that times the
          // parent's inverse (0x823DA5D8-0x823DA5F0), i.e. the world frame is set directly here.
          const double ct = std::cos(th), st = std::sin(th);
          V3 X, Y, Z;
          if (ax == 1) X = Sub(Scale(F0, ct), Scale(F2, st)), Y = F1, Z = Add(Scale(F0, st), Scale(F2, ct));
          else X = Add(Scale(F0, ct), Scale(F1, st)), Y = Add(Scale(F0, -st), Scale(F1, ct)), Z = F2;
          auto cols = [](const V3& x, const V3& y, const V3& z) { return M3{x[0], y[0], z[0], x[1], y[1], z[1], x[2], y[2], z[2]}; };
          Wc[i] = cols(X, Y, Z);
          // 0x823DA52C-0x823DA55C: the calf's local rotation is a pure turn about Y (Z) by the bend angle
          // (sub_82C40D38 / sub_82C40D68: quaternion (0, sin(a/2), 0, cos(a/2))), so its world frame is the thigh's turned by kb.
          const double ck = std::cos(kb);
          V3 Xc, Yc, Zc;
          if (ax == 1) Xc = Sub(Scale(X, ck), Scale(Z, sk)), Yc = Y, Zc = Add(Scale(X, sk), Scale(Z, ck));
          else Xc = Add(Scale(X, ck), Scale(Y, sk)), Yc = Add(Scale(X, -sk), Scale(Y, ck)), Zc = Z;
          cw.push_back(cols(Xc, Yc, Zc));
          calf_target[size_t(lg.calf)] = int(cw.size()) - 1;
          ++iks;
          if (info && !opt.no_tracks) {
            // log first: the bind frame against the bone direction (the solver assumes the bone runs along the frame's X)
            const V3 bd = Unit(Sub(pb[size_t(lg.calf)], pb[i]), V3{1, 0, 0});
            char buf[200];
            std::snprintf(buf, sizeof buf, " [leg %zu: w=%.3f t=%.2f dist=%.1f l1=%.1f l2=%.1f bend=%.1fdeg th=%.1fdeg bind.dir.axes=(%.2f %.2f %.2f)]", i, w, tb, dist, l1, l2,
                          kb * 57.29578, th * 57.29578, Dot(bd, V3{Wb[i][0], Wb[i][3], Wb[i][6]}), Dot(bd, V3{Wb[i][1], Wb[i][4], Wb[i][7]}),
                          Dot(bd, V3{Wb[i][2], Wb[i][5], Wb[i][8]}));
            legnote += buf;
          }
          continue;
        }
        const V3 u0 = Sub(A, H);
        // The solver's own reach term (sub_823DA1F0 0x823DA3F0-0x823DA4B0): the foot offset in the leg's plane is
        // (l1 + l2 cos w, l2 sin w) for the knee value w (w = 0 is a straight leg), so the reach is sqrt(l1^2 + l2^2 +
        // 2 l1 l2 cos w). The game blends between this and the target distance by a factor of w (constants not read): taken
        // as 1, i.e. the reach follows w and the target gives the direction (unconfirmed).
        const double reach_w = std::sqrt(l1 * l1 + l2 * l2 + 2 * l1 * l2 * std::cos(v[3]));
        const double d = std::min(v[3] > 0.0 && v[3] < 3.1416 ? reach_w : Len(u0), l1 + l2 - 1e-3);
        const V3 u = Unit(u0, V3{0, 1, 0});
        const double a = (l1 * l1 - l2 * l2 + d * d) / (2 * std::max(d, 1e-3)), h = std::sqrt(std::max(0.0, l1 * l1 - a * a));
        // (Session 97 review: the knee bends toward the PELVIS's forward, turned with the body - the bind forward is -Z)
        V3 fwd{0, 0, -1};
        if (const int pel = find("Bip Pelvis"); pel >= 0 && size_t(pel) < i) {
          M3 WbT;
          for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) WbT[size_t(r * 3 + c)] = Wb[size_t(pel)][size_t(c * 3 + r)];
          fwd = MVec(MMul(Wc[size_t(pel)], WbT), fwd);
        }
        const V3 pole = Unit(Sub(fwd, Scale(u, Dot(fwd, u))), V3{0, 0, -1});
        const V3 K = Add(Add(H, Scale(u, a)), Scale(pole, h)), E = Add(H, Scale(u, d));
        const V3 d1 = Unit(Sub(K, H), u), d2 = Unit(Sub(E, K), u);
        const V3 b1 = Unit(Sub(pb[size_t(lg.calf)], pb[i]), V3{0, 1, 0}), b2 = Unit(Sub(pb[size_t(lg.foot)], pb[size_t(lg.calf)]), V3{0, 1, 0});
        Wc[i] = MMul(Arc(b1, d1), Wb[i]);
        cw.push_back(MMul(Arc(b2, d2), Wb[size_t(lg.calf)]));
        calf_target[size_t(lg.calf)] = int(cw.size()) - 1;
        ++iks;
      }
    // the row-vector world matrix: rotation transposed, translation in row 3
    Matrix w = Identity();
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) w[size_t(r * 4 + c)] = float(Wc[i][size_t(c * 3 + r)]);
    w[12] = float(pos[i][0]), w[13] = float(pos[i][1]), w[14] = float(pos[i][2]);
    skin[i] = Mul(b.inv_bind, MirrorY(w));
  }
  if (info)
    *info = std::to_string(mapped) + " of " + std::to_string(n) + " bones follow tracks, " + std::to_string(iks) + " legs by IK, " + std::to_string(orphans) + " orphan nodes adopted, translation scale " +
            std::to_string(scale);
  if (info) *info += legnote;
  if (info)
    for (const char* nm : {"Bip Head", "Bip L Hand", "Bip L Foot", "Bip L Calf"}) {
      const int k = find(nm);
      if (k >= 0) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "; %s=(%.1f,%.1f,%.1f)", nm, pos[size_t(k)][0], pos[size_t(k)][1], pos[size_t(k)][2]);
        *info += buf;
      }
    }
  return skin;
}

}  // namespace motion
}  // namespace assets
