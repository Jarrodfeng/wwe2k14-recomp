// Session 97 (crowd motion): the crowd's poses from the player's own pac\m2.pac, read at run time (CLAUDE.md rules 1 and 2:
// nothing from the game is kept or committed). A port of tools\assets\m2mot.py, which reproduces a live-dumped pose to
// 1e-4 on 16 of 17 nodes; the format and its evidence: out\runtime\s97\m2_motion.md.
//   m2.pac = EPAC, at 0x4000 `YMKs`; LE u32 count N at 0x4110; N 16-byte records from 0x4120
//   {4 bytes, u8, u8 person type, LE u16 act, LE u32 offset, LE u32 size}; key data at 0x4110 + 16 N + 4 + offset.
//   An entry = tokens: < 96 skip that many 2-frame steps; 96 = chunk with a LE u16 length; 97..255 = chunk of token - 96
//   bytes. A chunk = one key: [optional prefix 0x80|n + n bytes] 0x7D count, 3 LE float32 root position, 21 slot items
//   (LE int16 angles, 65536 = 360 degrees; bytes by slot type 2 / 3 / 6 / 7 / 23).
// Approximate / unconfirmed (m2_motion.md): r_te differs by 0.018 from the game's own matrix, loops (we hold the last key),
// the real time per frame, other acts' slot tables (only the 110-byte crowd keys are decoded).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "../launcher/disc_image.h"
#include "../xctd_file.h"
#include "asset_model.h"
#include "pac.h"

namespace assets {
namespace crowdmot {

constexpr int kSlots = 21;
constexpr int kNodes = 17;
// slot type (from the live slot table) and the node each slot drives (-1 = a dummy slot: ignored)
constexpr int kSlotType[kSlots] = {7, 7, 7, 6, 23, 6, 7, 3, 6, 6, 7, 3, 6, 7, 2, 23, 2, 7, 2, 23, 2};
constexpr int kSlotNode[kSlots] = {0, 5, 6, 15, 16, 11, 12, 13, 14, 7, 8, 9, 10, 1, 2, -1, -1, 3, 4, -1, -1};
inline const char* const kNodeNames[kNodes] = {"root", "l_momo", "l_sune", "r_momo", "r_sune", "koshi", "mune", "r_sakotsu", "r_ninoude",
                                               "r_kote", "r_te", "l_sakotsu", "l_ninoude", "l_kote", "l_te", "kubi", "atama"};

struct Key {
  int frame = 0;  // game frames (2 per step)
  int count = 0;  // the key lasts 2 (count + 1) frames
  float pos[3] = {0, 0, 0};
  int16_t ang[kSlots][3] = {};  // x y z per slot (axes a slot type does not carry are 0)
};
using Keys = std::vector<Key>;

struct Entry {
  uint32_t act = 0, type = 0, off = 0, size = 0;
};

class M2 {
 public:
  // Loads the pac once; false (and err) when it is missing or not a YMKs set.
  bool Load(launcher::GameFiles& files, std::string& err) {
    if (tried_) return ok_ ? true : (err = err_, false);
    try {
      err_ = Open(files);
    } catch (const std::exception& ex) {  // (review: keep the reason; still tried once only)
      err_ = std::string("m2.pac: ") + ex.what();
    }
    tried_ = true;
    ok_ = err_.empty();
    if (!ok_) err = err_;
    return ok_;
  }
  // Keys of (act, person type); empty when there is no such entry or it is not a crowd-table motion.
  const Keys& KeysFor(int act, int type) {
    static const Keys kNone;
    if (!ok_) return kNone;
    const uint64_t id = uint64_t(uint32_t(act)) << 8 | uint32_t(type & 0xFF);
    auto it = cache_.find(id);
    if (it != cache_.end()) return it->second;
    Keys ks;
    for (const Entry& e : ents_)
      if (int(e.act) == act && int(e.type) == type) {
        ks = Decode(e);
        break;
      }
    return cache_.emplace(id, std::move(ks)).first->second;
  }

 private:
  std::string Open(launcher::GameFiles& files) {
    const std::string rel = "pac\\m2.pac";
    if (!files.Exists(rel)) return "missing " + rel;
    Bytes raw;
    if (!files.Stream(rel, [&](const uint8_t* p, size_t n) { raw.insert(raw.end(), p, p + n); })) return "read failed " + rel;
    std::string e2;
    if (!wwe2k14::xctd::Unpack(raw, d_, e2)) return rel + ": " + e2;
    raw = Bytes();
    if (d_.size() < 0x4120 || std::memcmp(d_.data() + 0x4000, "YMKs", 4) != 0) return rel + ": no YMKs set";
    const uint32_t n = Le32(d_, 0x4110);
    if (n == 0 || n > 100000 || 0x4120 + 16ull * n > d_.size()) return rel + ": bad index count";
    base_ = 0x4110 + 16ull * n + 4;
    std::vector<uint32_t> offs;
    for (uint32_t i = 0; i < n; ++i) {
      const size_t p = 0x4120 + 16ull * i;
      Entry e;
      e.type = d_[p + 5];
      e.act = uint32_t(d_[p + 6]) | uint32_t(d_[p + 7]) << 8;
      e.off = Le32(d_, p + 8);
      e.size = Le32(d_, p + 12);
      ents_.push_back(e);
      offs.push_back(e.off);
    }
    std::sort(offs.begin(), offs.end());
    offs.erase(std::unique(offs.begin(), offs.end()), offs.end());
    next_.clear();
    for (size_t k = 0; k < offs.size(); ++k) next_[offs[k]] = k + 1 < offs.size() ? offs[k + 1] : 0xFFFFFFFFu;
    return "";
  }

  Keys Decode(const Entry& e) const {
    Keys out;
    const size_t start = base_ + e.off;
    if (start >= d_.size()) return out;
    const uint32_t nx = next_.count(e.off) ? next_.at(e.off) : 0xFFFFFFFFu;
    size_t end = nx == 0xFFFFFFFFu ? d_.size() : std::min(d_.size(), base_ + size_t(nx));
    end = std::min(end, start + (size_t(1) << 20));  // (S97 review: the last entry would run to the end of the file)
    size_t p = start;
    int64_t step = 0;  // (review: no int overflow on a long run of skip bytes)
    while (p < end && p + 3 <= d_.size()) {
      const uint8_t t = d_[p];
      if (t < 96) {
        step += t;
        ++p;
        continue;
      }
      size_t len, q;
      if (t == 96) {
        len = size_t(d_[p + 1]) | size_t(d_[p + 2]) << 8;
        q = p + 3;
      } else {
        len = size_t(t) - 96;
        q = p + 1;
      }
      if (q + len > d_.size()) break;
      Key k;
      if (len && DecodeChunk(d_.data() + q, len, k)) {
        k.frame = int(std::min<int64_t>(2 * step, 1 << 30));
        out.push_back(k);
      }
      ++step;
      p = q + len;
    }
    return out;
  }

  static bool DecodeChunk(const uint8_t* b, size_t n, Key& k) {
    if (b[0] & 0x80) {
      const size_t skip = size_t(b[0] & 0x7F) + 1;
      if (skip >= n) return false;
      b += skip, n -= skip;
    }
    if (n != 110 || b[0] != 0x7D) return false;  // the crowd slot table; other skeletons are not decoded
    k.count = b[1];
    std::memcpy(k.pos, b + 2, 12);
    for (float v : k.pos)
      if (!std::isfinite(v)) return false;  // (review: a NaN root would NaN a whole body)
    size_t p = 14;
    for (int s = 0; s < kSlots; ++s) {
      const int t = kSlotType[s];
      const int nb = t == 2 ? 1 : (t == 3 || t == 6) ? 2 : 3;
      int16_t raw[3] = {0, 0, 0};
      for (int j = 0; j < nb; ++j, p += 2) raw[j] = int16_t(uint16_t(b[p]) | uint16_t(b[p + 1]) << 8);
      int16_t* a = k.ang[s];
      a[0] = a[1] = a[2] = 0;
      if (t == 2) a[1] = raw[0];
      else if (t == 3) a[0] = raw[0], a[1] = raw[1];
      else if (t == 6) a[1] = raw[0], a[2] = raw[1];
      else a[0] = raw[0], a[1] = raw[1], a[2] = raw[2];
    }
    return true;
  }

  Bytes d_;
  size_t base_ = 0;
  std::vector<Entry> ents_;
  std::map<uint32_t, uint32_t> next_;
  std::map<uint64_t, Keys> cache_;
  bool tried_ = false, ok_ = false;
  std::string err_;
};

// One M2 per viewer run: arena loads after the first skip the unpack.
inline M2& TheM2() {
  static M2 m;
  return m;
}

// ---- pose synthesis (row vectors, as m2mot.py) --------------------------------------------------------------------------
namespace detail {
inline Matrix RotX(float a) {
  const float c = std::cos(a), s = std::sin(a);
  return {1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, 0, 0, 1};
}
inline Matrix RotY(float a) {
  const float c = std::cos(a), s = std::sin(a);
  return {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1};
}
inline Matrix RotZ(float a) {
  const float c = std::cos(a), s = std::sin(a);
  return {c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}
inline Matrix Euler(float ax, float ay, float az) { return Mul(Mul(RotX(ax), RotY(ay)), RotZ(az)); }

// quaternion (x y z w) of a row-vector rotation (its transpose is the column form)
inline void ToQuat(const Matrix& M, float q[4]) {
  auto m = [&](int i, int j) { return M[j * 4 + i]; };
  const float tr = m(0, 0) + m(1, 1) + m(2, 2);
  if (tr > 0) {
    const float s = std::sqrt(tr + 1) * 2;
    q[0] = (m(2, 1) - m(1, 2)) / s, q[1] = (m(0, 2) - m(2, 0)) / s, q[2] = (m(1, 0) - m(0, 1)) / s, q[3] = s / 4;
  } else if (m(0, 0) > m(1, 1) && m(0, 0) > m(2, 2)) {
    const float s = std::sqrt(1 + m(0, 0) - m(1, 1) - m(2, 2)) * 2;
    q[0] = s / 4, q[1] = (m(0, 1) + m(1, 0)) / s, q[2] = (m(0, 2) + m(2, 0)) / s, q[3] = (m(2, 1) - m(1, 2)) / s;
  } else if (m(1, 1) > m(2, 2)) {
    const float s = std::sqrt(1 + m(1, 1) - m(0, 0) - m(2, 2)) * 2;
    q[0] = (m(0, 1) + m(1, 0)) / s, q[1] = s / 4, q[2] = (m(1, 2) + m(2, 1)) / s, q[3] = (m(0, 2) - m(2, 0)) / s;
  } else {
    const float s = std::sqrt(1 + m(2, 2) - m(0, 0) - m(1, 1)) * 2;
    q[0] = (m(0, 2) + m(2, 0)) / s, q[1] = (m(1, 2) + m(2, 1)) / s, q[2] = s / 4, q[3] = (m(1, 0) - m(0, 1)) / s;
  }
}
inline Matrix FromQuat(const float in[4]) {
  const float n = std::sqrt(in[0] * in[0] + in[1] * in[1] + in[2] * in[2] + in[3] * in[3]);
  const float t[3] = {0, 0, 0};
  const float q[4] = {in[0] / n, in[1] / n, in[2] / n, in[3] / n};
  return FromQuatT(q, t);
}
inline void Slerp(const float a[4], const float b0[4], float t, float out[4]) {
  float b[4] = {b0[0], b0[1], b0[2], b0[3]};
  float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  if (d < 0) {
    for (float& v : b) v = -v;
    d = -d;
  }
  if (d > 0.9995f) {
    for (int i = 0; i < 4; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
    return;
  }
  const float th = std::acos(d), s = std::sin(th);
  const float wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
  for (int i = 0; i < 4; ++i) out[i] = wa * a[i] + wb * b[i];
}
}  // namespace detail

// Frames of a motion: the last key's frame plus its span.
inline int Length(const Keys& ks) { return ks.empty() ? 0 : ks.back().frame + 2 * (ks.back().count + 1); }

// Local matrices (row vectors, BODY units: the body's own node-table translations; the root takes the key position / 100)
// for the 17 nodes of a crowd body at game frame `frame`. `nodes` = the body's own node records (file space). False when
// the skeleton is not the 17-node crowd rig or there are no keys. Past the last key the last key is held.
// (Session 97, loop reading: the game does NOT wrap inside this layer. The MoHo update sub_823ABD30 advances each slot by
// elapsed += dt * [0x8392DC38 + 60] and only moves to the next key when one exists ([slot+128] != 0, 0x823ABECC-0x823ABED8): the
// last key is held. The seek sub_823D0A48 returns -1 when frame / 2 >= the entry's size (0x823D0A50-0x823D0A5C), sub_823AC458
// passes the -1 up (0x823AC8D4-0x823AC8EC) and its wrapper sub_823ACA40 just stops (0x823ACAB0). Whatever restarts a crowd
// motion is the caller's behaviour switch (not found), so no frame modulo is applied here. The per-update step is dt * the
// factor at 0x8392DC38 + 60 (a runtime value, not in the image): 1 or 2 game frames is NOT settled.)
inline bool Pose(const Keys& ks, double frame, const std::vector<Bone>& nodes, std::vector<Matrix>& local) {
  using namespace detail;
  if (ks.empty() || nodes.size() != size_t(kNodes)) return false;
  for (int i = 0; i < kNodes; ++i)
    if (nodes[size_t(i)].name != kNodeNames[i]) return false;
  size_t i = 0;
  while (i + 1 < ks.size() && ks[i + 1].frame <= frame) ++i;
  const Key& a = ks[i];
  const Key& b = i + 1 < ks.size() ? ks[i + 1] : a;
  const double dur = 2.0 * (a.count + 1);
  const float t = &b == &a ? 0.f : float(std::min(1.0, std::max(0.0, (frame - a.frame) / dur)));
  const float kAng = 2.f * 3.14159265358979f / 65536.f;
  local.assign(kNodes, Identity());
  for (int s = 0; s < kSlots; ++s) {
    const int node = kSlotNode[s];
    if (node < 0) continue;
    const Bone& bn = nodes[size_t(node)];
    Matrix E;
    if (kSlotType[s] == 7) {
      float qa[4], qb[4], qs[4];
      ToQuat(Euler(a.ang[s][0] * kAng, a.ang[s][1] * kAng, a.ang[s][2] * kAng), qa);
      ToQuat(Euler(b.ang[s][0] * kAng, b.ang[s][1] * kAng, b.ang[s][2] * kAng), qb);
      Slerp(qa, qb, t, qs);
      E = FromQuat(qs);
    } else {
      float ang[3];
      for (int j = 0; j < 3; ++j) {
        const int d = (int(b.ang[s][j]) - int(a.ang[s][j]) + 32768) % 65536;  // shortest way round
        const int dd = (d < 0 ? d + 65536 : d) - 32768;
        ang[j] = (float(a.ang[s][j]) + float(dd) * t) * kAng;
      }
      E = Euler(ang[0], ang[1], ang[2]);
    }
    const float zero[3] = {0, 0, 0};
    const Matrix rest = FromQuatT(bn.local_rotation, zero);
    Matrix R = Mul(E, rest);
    if (node == 0) {
      for (int j = 0; j < 3; ++j) R[12 + j] = (a.pos[j] + (b.pos[j] - a.pos[j]) * t) * 0.01f;
    } else {
      for (int j = 0; j < 3; ++j) R[12 + j] = bn.local_translation[j];
    }
    R[3] = R[7] = R[11] = 0, R[15] = 1;
    local[size_t(node)] = R;
  }
  return true;
}

}  // namespace crowdmot
}  // namespace assets
