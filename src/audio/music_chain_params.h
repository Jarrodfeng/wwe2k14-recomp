// music_chain_params.h - the stock themes' music effects chain as numbers, read at boot from the PLAYER's own
// Wwise banks (Game_Sound_E_S.pck, AKPK, bank format v65), with built-in defaults (the same numbers) when
// parsing fails. Header-only, standard C++ only (no Windows / game includes): PC and Android share it.
//
// What it answers: docs\systems\audio.md section 7.1 (lane 284): music actor-mixer -> RoomVerb share-set, music bus,
// the "Game" bus's two Parametric EQs, master, the ducking table, the music-volume RTPC curve.
// Only parameter VALUES leave the banks; no bank bytes are kept or written. Python twin that prints the
// same numbers: tools\audio\wwise_music_chain.py.
//
// Use:
//   wwe::musicchain::Params p = wwe::musicchain::defaults();
//   std::string why;
//   if (!wwe::musicchain::load_from_pck(path, p, &why)) { /* p is the defaults; log why */ }
//   puts(wwe::musicchain::describe(p).c_str());
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace wwe::musicchain {

enum FilterType : int { kLowShelf = 4, kHighShelf = 5, kPeaking = 6 };  // Wwise filter codes seen in the banks

struct EqBand {
  int type = kPeaking;
  float gain_db = 0, freq = 1000, q = 1;
  bool on = false;
};
struct ParamEq {
  EqBand band[3];
  float out_db = 0;
};

struct RoomVerbParams {
  float decay_s = 2.3f, hf_damping = 6.45f, diffusion = 46, stereo_width = 167;
  float pre_delay_ms = 36, room_size = 100, density = 28, room_shape = 100;
  int reverb_units = 8;
  bool er_enable = false;
  bool tone_enable = true;
  // tone filters: gain, freq, Q, on (position != 0). Filter 1 low shelf, 2 (off in stock), 3 peaking notch.
  float f_gain_db[3] = {6.5f, 7.0f, -24.0f};
  float f_freq[3] = {300, 400, 7500};
  float f_q[3] = {1.0f, 1.0f, 8.5f};
  bool f_on[3] = {true, false, true};
  float dry_db = -3.3f, er_db = -2.0f, reverb_db = -17.0f;
};

struct Duck {
  uint32_t ducker = 0;  // bus id of the ducking bus
  float db = 0;
  uint32_t fade_down_ms = 0, fade_up_ms = 0;
  int curve = 0;
};

struct Params {
  RoomVerbParams rv;
  ParamEq eq[2];
  float music_bus_db = -9.0f;
  float master_db = -2.0f;
  // music-bus volume RTPC (0xEDF036D6): piecewise-linear in dB over the game parameter value
  int rtpc_n = 2;
  float rtpc_x[8] = {0, 100};
  float rtpc_db[8] = {-96.3f, 0};
  int n_ducks = 5;
  Duck ducks[8];
  bool from_banks = false;  // false = built-in defaults
};

inline Params defaults() {
  Params p;
  p.eq[0].band[0] = {kLowShelf, -4.0f, 333, 1.0f, true};
  p.eq[0].band[1] = {kLowShelf, -3.0f, 5665, 63.3f, false};
  p.eq[0].band[2] = {kHighShelf, -8.0f, 1646, 1.0f, true};
  p.eq[0].out_db = 3.0f;
  p.eq[1].band[0] = {kLowShelf, 0.0f, 821, 1.0f, true};
  p.eq[1].band[1] = {kPeaking, -2.0f, 1000, 1.0f, true};
  p.eq[1].band[2] = {kHighShelf, -1.5f, 763, 1.0f, true};
  p.eq[1].out_db = 1.0f;
  p.ducks[0] = {0x01494F41u, -3.0f, 1000, 1000, 4};
  p.ducks[1] = {0x10CB6EF5u, -2.0f, 500, 1000, 1};
  p.ducks[2] = {0x150CF384u, -2.0f, 500, 500, 4};
  p.ducks[3] = {0x213DB458u, -5.0f, 0, 1000, 4};
  p.ducks[4] = {0x3A999EE8u, -1.0f, 500, 1000, 7};
  return p;
}

// Static dry-path gain of a stock theme: bus + master + EQ outputs + RoomVerb dry (-10.3 dB for the stock numbers).
inline float net_dry_db(const Params& p) { return p.music_bus_db + p.master_db + p.eq[0].out_db + p.eq[1].out_db + p.rv.dry_db; }

namespace detail {

constexpr uint32_t kBankVersion = 65;
constexpr uint32_t kMusicMixerId = 126675442u;
enum : uint8_t { kActorMixer = 7, kBus = 8, kFxShare = 18, kFxCustom = 19 };

struct Cur {  // bounds-checked big-endian reader
  const uint8_t* d;
  size_t n, p = 0;
  bool bad = false;
  Cur(const uint8_t* data, size_t size, size_t pos = 0) : d(data), n(size), p(pos) {}
  bool need(size_t k) {
    if (p + k > n) bad = true;
    return !bad;
  }
  uint8_t u8() { return need(1) ? d[p++] : 0; }
  uint16_t u16() {
    if (!need(2)) return 0;
    uint16_t v = uint16_t(d[p] << 8 | d[p + 1]);
    p += 2;
    return v;
  }
  uint32_t u32() {
    if (!need(4)) return 0;
    uint32_t v = uint32_t(d[p]) << 24 | uint32_t(d[p + 1]) << 16 | uint32_t(d[p + 2]) << 8 | d[p + 3];
    p += 4;
    return v;
  }
  float f32() {
    uint32_t v = u32();
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  }
  void skip(size_t k) {
    if (need(k)) p += k;
  }
};

inline uint64_t key(uint8_t t, uint32_t id) { return (uint64_t(t) << 32) | id; }
using Index = std::unordered_map<uint64_t, std::vector<uint8_t>>;

// Parse one bank into idx (mixers, buses, fx only). Returns false + why for a damaged or non-v65 bank.
inline bool index_bank(const uint8_t* b, size_t n, Index& idx, std::string* why) {
  Cur c(b, n);
  if (n < 12 || std::memcmp(b, "BKHD", 4) != 0) {
    if (why) *why = "no BKHD";
    return false;
  }
  c.p = 8;
  uint32_t ver = c.u32();
  if (ver != kBankVersion) {
    if (why) *why = "bank version " + std::to_string(ver) + ", only v65 is understood";
    return false;
  }
  size_t p = 0;
  while (p + 8 <= n) {
    Cur h(b, n, p + 4);
    uint32_t sz = h.u32();
    if (p + 8 + sz > n) {
      if (why) *why = "chunk overruns bank";
      return false;
    }
    if (std::memcmp(b + p, "HIRC", 4) == 0) {
      Cur q(b, p + 8 + sz, p + 8);
      uint32_t cnt = q.u32();
      for (uint32_t i = 0; i < cnt; i++) {
        uint8_t t = q.u8();
        uint32_t ln = q.u32();
        if (q.bad || ln < 4 || q.p + ln > p + 8 + sz) {
          if (why) *why = "HIRC object overruns chunk";
          return false;
        }
        uint32_t id = Cur(b, n, q.p).u32();
        if (t == kActorMixer || t == kBus || t == kFxShare || t == kFxCustom) {
          auto k = key(t, id);
          if (!idx.count(k)) idx[k].assign(b + q.p + 4, b + q.p + ln);
        }
        q.p += ln;
      }
    }
    p += 8 + sz;
  }
  return true;
}

struct FxRef { uint8_t slot; uint32_t id; uint8_t share; };

inline bool node_fx(Cur& c, std::vector<FxRef>& out) {
  c.u8();  // override parent fx
  uint8_t n = c.u8();
  if (n) {
    c.u8();  // bypass bits
    for (int i = 0; i < n; i++) {
      FxRef r;
      r.slot = c.u8();
      r.id = c.u32();
      r.share = c.u8();
      c.u8();
      out.push_back(r);
    }
  }
  return !c.bad;
}

struct BusInfo {
  uint32_t parent = 0;
  float volume_db = 0;
  std::vector<Duck> ducks;      // ducker field holds the TARGET bus here
  std::vector<FxRef> fx;
  std::vector<std::pair<float, float>> volume_rtpc;
};

inline bool decode_bus(const std::vector<uint8_t>& body, BusInfo& o) {
  Cur c(body.data(), body.size());
  o.parent = c.u32();
  uint8_t n = c.u8();
  std::vector<uint8_t> ids(n);
  for (auto& i : ids) i = c.u8();
  for (int i = 0; i < n; i++) {
    float v = c.f32();
    if (ids[i] == 0) o.volume_db = v;
  }
  c.skip(10);
  c.u32();
  c.f32();
  uint32_t nd = c.u32();
  if (nd > 64) return false;
  for (uint32_t i = 0; i < nd; i++) {
    Duck d;
    d.ducker = c.u32();  // target
    d.db = c.f32();
    d.fade_down_ms = c.u32();
    d.fade_up_ms = c.u32();
    d.curve = c.u8();
    o.ducks.push_back(d);
  }
  uint8_t nfx = c.u8();
  if (nfx) {
    c.u8();
    for (int i = 0; i < nfx; i++) {
      FxRef r;
      r.slot = c.u8();
      r.id = c.u32();
      r.share = c.u8();
      c.u8();
      o.fx.push_back(r);
    }
  }
  uint16_t nr = c.u16();
  for (int i = 0; i < nr && !c.bad; i++) {
    c.u32();
    uint32_t param = c.u32();
    c.u32();
    c.u8();
    uint16_t sz = c.u16();
    for (int j = 0; j < sz && !c.bad; j++) {
      float x = c.f32(), y = c.f32();
      c.u32();
      if (param == 0) o.volume_rtpc.push_back({x, y});
    }
  }
  return !c.bad;
}

inline const std::vector<uint8_t>* find_fx(const Index& idx, const FxRef& r, uint32_t* plugin) {
  for (uint8_t t : {r.share ? kFxShare : kFxCustom, r.share ? kFxCustom : kFxShare}) {
    auto it = idx.find(key(t, r.id));
    if (it != idx.end() && it->second.size() >= 8) {
      Cur c(it->second.data(), it->second.size());
      *plugin = c.u32() >> 16;
      return &it->second;
    }
  }
  return nullptr;
}

inline bool decode_roomverb(const std::vector<uint8_t>& body, RoomVerbParams& r) {
  Cur c(body.data(), body.size());
  c.u32();
  uint32_t sz = c.u32();
  if (sz < 80 + 5 + 20 + 4 + 1 + 24 || 8 + sz > body.size()) return false;
  r.decay_s = c.f32();
  r.hf_damping = c.f32();
  r.diffusion = c.f32();
  r.stereo_width = c.f32();
  for (int i = 0; i < 3; i++) {
    r.f_gain_db[i] = c.f32();
    r.f_freq[i] = c.f32();
    r.f_q[i] = c.f32();
  }
  c.f32();  // front
  c.f32();  // rear
  c.f32();  // centre
  c.f32();  // lfe
  r.dry_db = c.f32();
  r.er_db = c.f32();
  r.reverb_db = c.f32();
  r.er_enable = c.u8() != 0;
  c.u32();  // ER pattern
  r.pre_delay_ms = c.f32();
  r.room_size = c.f32();
  c.f32();  // ER feedback
  r.density = c.f32();
  r.room_shape = c.f32();
  r.reverb_units = int(c.u32());
  r.tone_enable = c.u8() != 0;
  for (int i = 0; i < 3; i++) {
    r.f_on[i] = c.u32() != 0;  // position: 0 = off
    c.u32();                   // curve
  }
  return !c.bad;
}

inline bool decode_parameq(const std::vector<uint8_t>& body, ParamEq& e) {
  Cur c(body.data(), body.size());
  c.u32();
  uint32_t sz = c.u32();
  if (sz < 56 || 8 + sz > body.size()) return false;
  for (int i = 0; i < 3; i++) {
    e.band[i].type = int(c.u32());
    e.band[i].gain_db = c.f32();
    e.band[i].freq = c.f32();
    e.band[i].q = c.f32();
    e.band[i].on = c.u8() != 0;
  }
  e.out_db = c.f32();
  return !c.bad;
}

// From a populated index to Params. False + why when the chain cannot be read.
inline bool build(const Index& idx, Params& out, std::string* why) {
  auto fail = [&](const char* m) {
    if (why) *why = m;
    return false;
  };
  auto mit = idx.find(key(kActorMixer, kMusicMixerId));
  if (mit == idx.end()) return fail("music actor-mixer not found");
  Params p = defaults();
  p.from_banks = true;
  p.n_ducks = 0;
  Cur c(mit->second.data(), mit->second.size());
  std::vector<FxRef> mfx;
  if (!node_fx(c, mfx)) return fail("music actor-mixer damaged");
  uint32_t bus = c.u32();
  if (c.bad) return fail("music actor-mixer damaged");
  bool haveRv = false;
  for (auto& r : mfx) {
    uint32_t plug = 0;
    auto* b = find_fx(idx, r, &plug);
    if (b && plug == 0x76 && decode_roomverb(*b, p.rv)) haveRv = true;
  }
  if (!haveRv) return fail("no readable RoomVerb on the music actor-mixer");
  uint32_t musicBus = bus;
  int level = 0, eqn = 0;
  float volumes[8] = {};
  for (uint32_t b = bus; b && level < 8; level++) {
    auto it = idx.find(key(kBus, b));
    if (it == idx.end()) break;
    BusInfo bi;
    if (!decode_bus(it->second, bi)) return fail("bus damaged");
    volumes[level] = bi.volume_db;
    if (level == 0 && !bi.volume_rtpc.empty()) {
      p.rtpc_n = int(bi.volume_rtpc.size() < 8 ? bi.volume_rtpc.size() : 8);
      for (int i = 0; i < p.rtpc_n; i++) {
        p.rtpc_x[i] = bi.volume_rtpc[i].first;
        p.rtpc_db[i] = bi.volume_rtpc[i].second;
      }
    }
    for (auto& r : bi.fx) {
      uint32_t plug = 0;
      auto* body = find_fx(idx, r, &plug);
      if (body && plug == 0x69 && eqn < 2) {
        ParamEq e;
        if (decode_parameq(*body, e)) {
          // slot order: lower slot first (stock: slot 1 then slot 2)
          p.eq[eqn++] = e;
        }
      }
    }
    b = bi.parent;
  }
  if (eqn != 2) return fail("expected two Parametric EQs above the music bus");
  p.music_bus_db = volumes[0];
  p.master_db = level > 1 ? volumes[level - 1] : 0.0f;
  for (auto& kv : idx) {
    if ((kv.first >> 32) != kBus) continue;
    BusInfo bi;
    if (!decode_bus(kv.second, bi)) continue;
    for (auto& d : bi.ducks) {
      if (d.ducker == musicBus && p.n_ducks < 8) {
        Duck o = d;
        o.ducker = uint32_t(kv.first & 0xffffffffu);
        p.ducks[p.n_ducks++] = o;
      }
    }
  }
  // stable order by ducker id
  for (int i = 1; i < p.n_ducks; i++)
    for (int j = i; j > 0 && p.ducks[j].ducker < p.ducks[j - 1].ducker; j--) std::swap(p.ducks[j], p.ducks[j - 1]);
  out = p;
  return true;
}

}  // namespace detail

// Parse an in-memory list of banks (used by the unit test and by load_from_pck).
inline bool load_from_bank_bytes(const std::vector<std::vector<uint8_t>>& banks, Params& out, std::string* why = nullptr) {
  detail::Index idx;
  for (auto& b : banks)
    if (!detail::index_bank(b.data(), b.size(), idx, why)) return false;
  return detail::build(idx, out, why);
}

// Read the player's own Game_Sound_E_S.pck (AKPK big endian). On failure `out` is left as the defaults and
// `why` says what went wrong; the caller logs it and keeps going.
inline bool load_from_pck(const std::string& path, Params& out, std::string* why = nullptr) {
  Params fallback = defaults();
  auto fail = [&](const std::string& m) {
    out = fallback;
    if (why) *why = m;
    return false;
  };
  std::ifstream f(path, std::ios::binary);
  if (!f) return fail("cannot open " + path);
  uint8_t head[28];
  f.read(reinterpret_cast<char*>(head), 28);
  if (f.gcount() != 28 || std::memcmp(head, "AKPK", 4) != 0) return fail("not an AKPK package");
  detail::Cur h(head, 28, 4);
  uint32_t hs = h.u32();
  h.u32();
  uint32_t fl = h.u32(), bl = h.u32();
  if (hs > (64u << 20) || hs < 24) return fail("implausible AKPK header size");
  std::vector<uint8_t> hdr(hs + 8);
  f.seekg(0);
  f.read(reinterpret_cast<char*>(hdr.data()), std::streamsize(hdr.size()));
  if (size_t(f.gcount()) != hdr.size()) return fail("AKPK header truncated");
  detail::Cur c(hdr.data(), hdr.size(), 28 + size_t(fl));
  uint32_t n = c.u32();
  if (c.bad || 28 + size_t(fl) + 4 + size_t(n) * 20 > hdr.size() || bl < 4) return fail("bank LUT damaged");
  detail::Index idx;
  std::vector<uint8_t> buf;
  for (uint32_t i = 0; i < n; i++) {
    c.u32();  // bank id
    uint64_t blk = c.u32();
    uint64_t size = c.u32();
    uint64_t start = c.u32();
    c.u32();  // folder
    if (size > (64u << 20)) return fail("implausible bank size");
    buf.resize(size_t(size));
    f.clear();
    f.seekg(std::streamoff(start * blk));
    f.read(reinterpret_cast<char*>(buf.data()), std::streamsize(size));
    if (uint64_t(f.gcount()) != size) return fail("bank truncated");
    std::string w;
    if (!detail::index_bank(buf.data(), buf.size(), idx, &w)) return fail("bank " + std::to_string(i) + ": " + w);
  }
  std::string w;
  if (!detail::build(idx, out, &w)) return fail(w);
  return true;
}

// The 7.1 numbers as text (same shape as the Python tool's output).
inline std::string describe(const Params& p) {
  char b[512];
  std::string s;
  const auto& r = p.rv;
  std::snprintf(b, sizeof b, "music chain (%s): bus %.1f dB, master %.1f dB\n", p.from_banks ? "from banks" : "defaults", p.music_bus_db, p.master_db);
  s += b;
  std::snprintf(b, sizeof b,
                "RoomVerb: decay %.2f s, HF damping %.2f, diffusion %.0f, width %.0f, ER %s, pre-delay %.0f ms, room size %.0f, "
                "density %.0f, units %d\n  tone: %+.1f dB @ %.0f Hz Q %.1f (%s), %+.1f dB @ %.0f Hz Q %.1f (%s), %+.1f dB @ %.0f Hz Q %.1f (%s)\n"
                "  levels: dry %.1f dB, ER %.1f dB, reverb %.1f dB\n",
                r.decay_s, r.hf_damping, r.diffusion, r.stereo_width, r.er_enable ? "on" : "off", r.pre_delay_ms, r.room_size, r.density,
                r.reverb_units, r.f_gain_db[0], r.f_freq[0], r.f_q[0], r.f_on[0] ? "on" : "off", r.f_gain_db[1], r.f_freq[1], r.f_q[1],
                r.f_on[1] ? "on" : "off", r.f_gain_db[2], r.f_freq[2], r.f_q[2], r.f_on[2] ? "on" : "off", r.dry_db, r.er_db, r.reverb_db);
  s += b;
  static const char* names[] = {"?", "?", "?", "?", "low shelf", "high shelf", "peaking"};
  for (int e = 0; e < 2; e++) {
    std::snprintf(b, sizeof b, "Parametric EQ %d: out %+.1f dB\n", e + 1, p.eq[e].out_db);
    s += b;
    for (int k = 0; k < 3; k++) {
      const auto& x = p.eq[e].band[k];
      std::snprintf(b, sizeof b, "  band %d: %s %+.1f dB @ %.0f Hz Q %.1f %s\n", k + 1, (x.type >= 4 && x.type <= 6) ? names[x.type] : "?", x.gain_db,
                    x.freq, x.q, x.on ? "on" : "off");
      s += b;
    }
  }
  for (int i = 0; i < p.n_ducks; i++) {
    std::snprintf(b, sizeof b, "duck by 0x%08X: %+.1f dB, %u ms down / %u ms up, curve %d\n", p.ducks[i].ducker, p.ducks[i].db,
                  p.ducks[i].fade_down_ms, p.ducks[i].fade_up_ms, p.ducks[i].curve);
    s += b;
  }
  std::snprintf(b, sizeof b, "net dry gain: %.2f dB\n", net_dry_db(p));
  s += b;
  return s;
}

}  // namespace wwe::musicchain
