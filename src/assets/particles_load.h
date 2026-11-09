// Session 98 (cap / particles): reading the entrance particle effects (format: particles.h). Needs the game files; included by the
// viewer's game seam only (dds.h / pac.h stay out of viewer_main).
#pragma once

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "dds.h"
#include "pac.h"
#include "particles.h"

namespace assets {
namespace particles {

// The sprite texture `file` ("Smoke0002.tga": the directory entry "Smoke0002", case-insensitive) of effect `fx` (its own directory or
// its alias's), decoded. False when absent or unreadable.
inline bool SpriteTexture(const Bank& bank, const Effect& fx, const std::string& file, Texture& t) {
  auto it = bank.tex_dirs.find(fx.tex_from);
  if (it == bank.tex_dirs.end()) return false;
  std::string want = Lower(file);
  if (const size_t dot = want.rfind('.'); dot != std::string::npos) want.resize(dot);
  const Bytes& b = it->second;
  if (b.size() < 0x30) return false;
  const uint32_t count = Le32(b, 0);
  for (uint32_t i = 0; i < count && 0x10 + size_t(i + 1) * 32 <= b.size(); ++i) {
    const size_t r = 0x10 + size_t(i) * 32;
    const std::string nm = Lower(std::string(reinterpret_cast<const char*>(b.data() + r), strnlen(reinterpret_cast<const char*>(b.data() + r), 16)));
    if (nm != want) continue;
    const uint32_t size = Le32(b, r + 20), off = Le32(b, r + 24);
    if (off > b.size() || size > b.size() - off) return false;
    std::string why;
    if (!ParseDds(b.data() + off, size, t, why)) return false;
    t.name = nm;
    return true;
  }
  return false;
}

inline std::vector<Emitter> ParsePrtc(const Bytes& b) {
  std::vector<Emitter> out;
  if (b.size() < 16 || std::memcmp(b.data(), "PRTC", 4) != 0) return out;
  for (size_t o = 0; o + 60 <= b.size(); ++o) {
    if (std::memcmp(b.data() + o, "rootchk", 8) == 0 && o + 32 <= b.size()) {
      Emitter e;
      e.name.assign(reinterpret_cast<const char*>(b.data() + o + 16), strnlen(reinterpret_cast<const char*>(b.data() + o + 16), 16));
      // Session 99 (particles 2): the header the game reads into the emitter at +192 (sub_82A10760): +8 mode, +32 u16 max particles
      if (o + 36 <= b.size()) {
        e.mode = int(Le32(b, o + 8));
        e.max_parts = int(b[o + 32] | b[o + 33] << 8);
        if (e.mode < 0 || e.mode > 2) e.mode = 0;
        if (e.max_parts <= 0 || e.max_parts > 1024) e.max_parts = 40;
      }
      out.push_back(std::move(e));
      o += 7;
    } else if (std::memcmp(b.data() + o, "cmd_chk", 8) == 0 && !out.empty()) {
      Cmd c;
      c.id = int(Le32(b, o + 8)), c.a = Le32(b, o + 12), c.b = Le32(b, o + 16);
      if (c.id == 22) c.text.assign(reinterpret_cast<const char*>(b.data() + o + 24), strnlen(reinterpret_cast<const char*>(b.data() + o + 24), 32));
      else for (int k = 0; k < 8; ++k) std::memcpy(&c.v[k], b.data() + o + 24 + 4 * k, 4);
      out.back().cmds.push_back(std::move(c));
      o += 7;
    }
  }
  return out;
}

// The PACH base of record `want` (e.g. "0107") in section `section` of an EPAC directory (12-byte records from +0x80C: char[4] name,
// LE32 start in 2 KB sectors from +0x4000; a non-digit name opens a section). 0 = none.
inline size_t EpacRecord(const Bytes& d, const std::string& section, const char* want) {
  if (d.size() < 0x4000 || std::memcmp(d.data(), "EPAC", 4) != 0) return 0;
  std::string sec(reinterpret_cast<const char*>(d.data() + 0x800), 4);
  for (size_t o = 0x80C; o + 12 <= 0x4000; o += 12) {
    const uint8_t* r = d.data() + o;
    const uint32_t st = Le32(d, o + 4);
    const bool digits = std::isdigit(r[0]) && std::isdigit(r[1]) && std::isdigit(r[2]) && std::isdigit(r[3]);
    if (!digits) {
      if (r[0] == 0 && st == 0) break;
      sec.assign(reinterpret_cast<const char*>(r), 4);
      continue;
    }
    if (sec == section && std::memcmp(r, want, 4) == 0) return 0x4000 + size_t(st) * 0x800;
  }
  return 0;
}

// (review) The opened pacs, kept by the caller (the viewer's seam) beside its files: reopened when the game folder changes, retried
// after a failure.
struct PacCache {
  std::filesystem::path root;
  pac::Pac p;
  bool ok = false;
};
struct Pacs {
  PacCache prtcls, gm;
};
template <class Files>
inline bool OpenCached(Files& files, const char* rel, PacCache& c, std::string& err) {
  if (c.ok && c.root == files.root()) return true;
  c = PacCache();
  std::string e;
  if (!pac::Open(files, rel, c.p, e)) return err = e.empty() ? std::string(rel) + " not readable" : e, false;
  c.root = files.root(), c.ok = true;
  return true;
}

// Session 99 (particles 2): the bank's scene objects that play particle effects (particles.h Placement; pac\gm.pac section GMEV).
template <class Files>
inline std::vector<Placement> LoadPlacements(Files& files, int bank, PacCache& cache) {
  std::vector<Placement> out;
  std::string e;
  if (!OpenCached(files, "pac\\gm.pac", cache, e)) return out;
  const pac::Pac& p = cache.p;
  char want[8];
  std::snprintf(want, sizeof want, "%04d", bank);
  const size_t at = EpacRecord(p.bytes, "GMEV", want);
  const pac::Pach* h = nullptr;
  for (const pac::Pach& ph : p.pachs) if (at && ph.base == at) h = &ph;
  if (!h) return out;
  const Bytes rows = pac::Get(p.bytes, *h, 0), vals = pac::Get(p.bytes, *h, 2);
  Bytes nb;
  pac::Pach np;
  if (!pac::GetNested(p.bytes, *h, 3, nb, np)) return out;
  const Bytes list = pac::Get(nb, np, 10000);
  auto f32 = [](const Bytes& b, size_t o) {
    float v = 0;
    if (o + 4 <= b.size()) std::memcpy(&v, b.data() + o, 4);
    return std::isfinite(v) ? v : 0.f;
  };
  // entry 0: u16 id, u16 type, u16 count, u16 offset (floats into entry 2)
  struct Row { int type = 0; std::vector<float> v; };
  std::map<int, Row> by_id;
  for (size_t o = 0; o + 8 <= rows.size() && by_id.size() < 4096; o += 8) {
    const int id = rows[o] | rows[o + 1] << 8, type = rows[o + 2] | rows[o + 3] << 8, n = rows[o + 4] | rows[o + 5] << 8,
              off = rows[o + 6] | rows[o + 7] << 8;
    Row r;
    r.type = type;
    for (int k = 0; k < n && k < 64; ++k) r.v.push_back(f32(vals, 4 * size_t(off + k)));
    by_id[id] = std::move(r);
  }
  if (list.size() < 16) return out;
  const uint32_t n = std::min<uint32_t>(Le32(list, 0), 1024);
  for (uint32_t i = 0; i < n && 16 + 32 * size_t(i + 1) <= list.size(); ++i) {
    const size_t o = 16 + 32 * size_t(i);
    const int id = int(Le32(list, o + 12) & 0x1FFFF);
    auto it = by_id.find(id);
    if (it == by_id.end()) continue;
    const Row& r = it->second;
    // the particle object types of the factory (sub_827D1408: 1, 2 always; 8 / 9 / 10 by ARENA tests (Session 99): 8 unless the arena
    // is 20, 76, 77, 79 or a Create-an-Arena slot 200..249 (viewer_main UpdateParticles); 9 / 10 by byte 32 of the arena's record, a
    // run-time table not read - both drawn [U])
    if (!(r.type == 1 || r.type == 2 || r.type == 8 || r.type == 9 || r.type == 10) || r.v.size() < 6) continue;
    Placement pl;
    auto ci = [](float v, int lo, int hi) { return std::isfinite(v) ? int(std::min(float(hi), std::max(float(lo), v))) : lo; };  // (review)
    pl.effect = ci(r.v[1], -1, 1 << 16), pl.type = r.type;
    for (int k = 0; k < 3; ++k) {
      pl.pos[k] = std::max(-1e5f, std::min(1e5f, f32(list, o + 4 * size_t(k)) * 10.f));
      pl.rot[k] = std::max(-10.f, std::min(10.f, f32(list, o + 16 + 4 * size_t(k))));
    }
    pl.start_sub = ci(r.v[2], -1, 1000), pl.start_f = ci(r.v[3], 0, 100000), pl.end_sub = ci(r.v[4], -1, 10000),
    pl.end_f = ci(r.v[5], 0, 100000);
    pl.floor_rel = r.v.size() > 9 && r.v[9] == 1.f;
    out.push_back(pl);
  }
  return out;
}

// Session 99 (scene objects): the bank's placed GMEV objects of types 6 (screen flash) and 11 (soft-particle switch) (particles.h
// SceneLights). Empty when the bank has no GMEV record or none of these.
template <class Files>
inline SceneLights LoadSceneLights(Files& files, int bank, PacCache& cache) {
  SceneLights out;
  std::string e;
  if (!OpenCached(files, "pac\\gm.pac", cache, e)) return out;
  const pac::Pac& p = cache.p;
  char want[8];
  std::snprintf(want, sizeof want, "%04d", bank);
  const size_t at = EpacRecord(p.bytes, "GMEV", want);
  const pac::Pach* h = nullptr;
  for (const pac::Pach& ph : p.pachs) if (at && ph.base == at) h = &ph;
  if (!h) return out;
  const Bytes rows = pac::Get(p.bytes, *h, 0), vals = pac::Get(p.bytes, *h, 2);
  Bytes nb;
  pac::Pach np;
  if (!pac::GetNested(p.bytes, *h, 3, nb, np)) return out;
  const Bytes list = pac::Get(nb, np, 10000);
  auto f32 = [&](size_t i) {  // float i of entry 2 (0 past its end)
    float v = 0;
    if (4 * i + 4 <= vals.size()) std::memcpy(&v, vals.data() + 4 * i, 4);
    return std::isfinite(v) ? v : 0.f;
  };
  auto ci = [](float v, int lo, int hi) { return int(std::min(float(hi), std::max(float(lo), v))); };
  struct Row { int type = 0, n = 0, off = 0; };
  std::map<int, Row> by_id;
  for (size_t o = 0; o + 8 <= rows.size() && by_id.size() < 4096; o += 8)
    by_id[rows[o] | rows[o + 1] << 8] = Row{rows[o + 2] | rows[o + 3] << 8, rows[o + 4] | rows[o + 5] << 8, rows[o + 6] | rows[o + 7] << 8};
  if (list.size() < 16) return out;
  const uint32_t n = std::min<uint32_t>(Le32(list, 0), 1024);
  for (uint32_t i = 0; i < n && 16 + 32 * size_t(i + 1) <= list.size(); ++i) {
    const int id = int(Le32(list, 16 + 32 * size_t(i) + 12) & 0x1FFFF);
    auto it = by_id.find(id);
    if (it == by_id.end()) continue;
    const Row& r = it->second;
    auto v = [&](int k) { return f32(size_t(r.off) + size_t(k)); };  // (the game reads past a short row into the next floats)
    if (r.type == 6 && r.n >= 10) {
      ScreenFlash s;
      s.id = id, s.cue = ci(v(2), -1, 1000), s.start_f = ci(v(3), 0, 100000), s.end_f = ci(v(5), 0, 100000);
      for (int k = 0; k < 3; ++k) s.rgb[k] = std::max(0.f, std::min(255.f, v(6 + k))) / 255.f;
      s.strength = std::max(0.f, std::min(255.f, v(9)));
      s.mode = r.n > 10 ? ci(v(10), -1, 100) : 0;
      out.flashes.push_back(s);
    } else if (r.type == 11 && r.n >= 5) {
      SoftSwitch s;
      s.id = id, s.on_cue = ci(v(1), -1, 1000), s.on_f = ci(v(2), 0, 100000), s.off_cue = ci(v(3), -1, 1000), s.off_f = ci(v(4), 0, 100000);
      s.range = std::max(0.f, std::min(10000.f, v(5)));
      out.soft.push_back(s);
    }
  }
  return out;
}

// The PCE6 effects of entrance bank `bank` (all their emitters; textures kept as directories). ok = false with err when the bank has none.
template <class Files>
inline Bank LoadBank(Files& files, int bank, std::string& err, Pacs& cache) {
  Bank out;
  out.bank = bank;
  if (!OpenCached(files, "pac\\prtcls.pac", cache.prtcls, err)) return out;
  const pac::Pac& p = cache.prtcls.p;
  const Bytes& d = p.bytes;
  if (d.size() < 0x4000 || std::memcmp(d.data(), "EPAC", 4) != 0) return err = "prtcls.pac has no directory", out;
  char want[8];
  std::snprintf(want, sizeof want, "%04d", bank);
  std::string sec(reinterpret_cast<const char*>(d.data() + 0x800), 4);
  size_t at = 0;
  for (size_t o = 0x80C; o + 12 <= 0x4000 && !at; o += 12) {
    const uint8_t* r = d.data() + o;
    const uint32_t st = Le32(d, o + 4);
    const bool digits = std::isdigit(r[0]) && std::isdigit(r[1]) && std::isdigit(r[2]) && std::isdigit(r[3]);
    if (!digits) {
      if (r[0] == 0 && st == 0) break;
      sec.assign(reinterpret_cast<const char*>(r), 4);
      continue;
    }
    if (sec == "PCE6" && std::memcmp(r, want, 4) == 0) at = 0x4000 + size_t(st) * 0x800;
  }
  const pac::Pach* h = nullptr;
  for (const pac::Pach& ph : p.pachs) if (ph.base == at) h = &ph;
  if (!at || !h) return err = "no PCE6 particle effects for bank " + std::to_string(bank), out;
  std::map<int, int> alias;
  for (const pac::Ent& en : h->ents) {
    if (en.id == 16000) {
      const Bytes t = pac::Decode(p.bytes.data() + en.off, en.size);
      if (t.size() < 2) continue;
      const size_t n = size_t(t[0] | t[1] << 8);
      for (size_t k = 0; k < n && 2 + 36 * k + 12 <= t.size(); ++k) {
        float a = 0, b = 0;
        std::memcpy(&a, t.data() + 2 + 36 * k + 4, 4), std::memcpy(&b, t.data() + 2 + 36 * k + 8, 4);
        if (std::isfinite(a) && std::isfinite(b) && std::fabs(a) <= 1e6f && std::fabs(b) <= 1e6f) alias[int(a)] = int(b);  // (review)
      }
      continue;
    }
    Bytes nb;
    pac::Pach np;
    if (!pac::GetNested(p.bytes, *h, en.id, nb, np)) continue;
    Effect fx;
    fx.id = int(en.id);
    fx.emitters = ParsePrtc(pac::Get(nb, np, 0));
    Bytes tex = pac::Get(nb, np, 1);
    if (!tex.empty()) out.tex_dirs[fx.id] = std::move(tex);
    if (!fx.emitters.empty()) out.effects[fx.id] = std::move(fx);
  }
  for (auto& [id, fx] : out.effects) fx.tex_from = out.tex_dirs.count(id) ? id : (alias.count(id) ? alias[id] : -1);
  out.ok = !out.effects.empty();
  for (const auto& [fid, fx] : out.effects)
    for (size_t ei = 0; ei < fx.emitters.size(); ++ei)
      if (const Cmd* c22 = fx.emitters[ei].Find(22)) {
        Texture t;
        if (SpriteTexture(out, fx, c22->text, t)) out.sprites[{fid, ei}] = std::move(t);
      }
  out.alias = alias;
  if (out.ok) out.placed = LoadPlacements(files, bank, cache.gm);
  if (!out.ok) err = "bank " + std::to_string(bank) + ": no readable effect";
  return out;
}


}  // namespace particles
}  // namespace assets
