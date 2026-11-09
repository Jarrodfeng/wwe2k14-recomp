// Session 98 (scenes): named wrestler spots of an arena, derived from the player's own game files at run time (CLAUDE.md
// rules 1 and 2: nothing is kept or shipped). A C++ port of tools\assets\spots.py (evidence: out\runtime\s98\spots.md).
//   stage / ramp  = the rectangles of pac entry 50003 of bgNN.pac (Shift-JIS text "start x, start z, end x, end z, light set":
//                   set 200 = the entrance stage, 201 = the ramp / hanamichi; an arena may list several rects)
//   floor heights = the median height of the flat meshes' vertices of the loaded arena inside a rectangle
//   ring          = canvas top (Y up +111) and the canvas corners (+-318.8), the same in every arena
//   ringside      = midway between the apron (-321) and the announce desk edge (-670): (0, floor, -496) [bg05 geometry; the
//                   desk may sit elsewhere in other arenas: unconfirmed]
// Space: the viewer's (X right, +Z toward the entrance stage, Y UP). Yaw (degrees) = atan2(dx, dz) of the facing vector:
// 0 faces +Z (the stage), 180 faces -Z (the ring from the stage / the announce desk), 90 faces +X.
// Needs stb_image.h (declarations) and pac.h before use, like the other loaders.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "asset_model.h"
#include "pac.h"
#include "stage_key.h"

namespace assets {
namespace spots {

struct Rect {
  float x0 = 0, z0 = 0, x1 = 0, z1 = 0;
  int light = 0;
  float xmin() const { return std::min(x0, x1); }
  float xmax() const { return std::max(x0, x1); }
  float zmin() const { return std::min(z0, z1); }
  float zmax() const { return std::max(z0, z1); }
};

// The rectangles from the (decoded) text of entry 50003. Lines whose first five tokens are not numbers are skipped.
inline std::vector<Rect> ParseRects(const std::string& txt) {
  std::vector<Rect> out;
  size_t pos = 0;
  while (pos <= txt.size()) {
    size_t eol = txt.find('\n', pos);
    if (eol == std::string::npos) eol = txt.size();
    std::string ln = txt.substr(pos, eol - pos);
    pos = eol + 1;
    const size_t semi = ln.find(';');
    if (semi != std::string::npos) ln.resize(semi);
    double v[5];
    int n = 0;
    const char* p = ln.c_str();
    while (n < 5) {
      while (*p == ' ' || *p == '\t' || *p == '\r') ++p;
      if (!*p) break;
      char* end = nullptr;
      const double d = std::strtod(p, &end);
      if (end == p) break;
      v[n++] = d;
      p = end;
    }
    if (n < 5) continue;
    Rect r;
    r.x0 = (float)v[0], r.z0 = (float)v[1], r.x1 = (float)v[2], r.z1 = (float)v[3], r.light = (int)v[4];
    out.push_back(r);
    if (out.size() > 64) break;
  }
  return out;
}

// Entry 50003 of a top-level PACH of the raw bgNN.pac file (as read from the game folder), and the light sets 200 / 201 of the
// same PACH (nested 30002; with the stage key on, the values the key gives at stage_key::Cfg().time, like the arena loader).
inline bool ReadZones(const std::vector<uint8_t>& raw, std::vector<Rect>& out, std::map<int, LightSet>& lights, std::string& err) {
  out.clear();
  lights.clear();
  std::vector<uint8_t> b;
  std::string e2;
  if (!wwe2k14::xctd::Unpack(raw, b, e2)) return err = e2, false;
  if (b.size() < 16) return err = "pac too small", false;
  size_t pos = std::memcmp(b.data(), "EPAC", 4) == 0 ? 0x4000 : 0;
  while (pos + 8 <= b.size()) {
    pac::Pach p;
    if (std::memcmp(b.data() + pos, "PACH", 4) != 0 || !pac::ParsePach(b, pos, b.size(), p)) {
      pos += 0x800;
      continue;
    }
    pos = (p.end + 0x7FF) & ~size_t(0x7FF);
    if (const pac::Ent* e = pac::Find(p, 50003)) {
      const std::vector<uint8_t> d = pac::Decode(b.data() + e->off, e->size);
      out = ParseRects(std::string(d.begin(), d.end()));
      Bytes nb;
      pac::Pach np;
      if (pac::GetNested(b, p, 30002, nb, np))
        for (const pac::Ent& se : np.ents) {
          if (se.id != 200 && se.id != 201) continue;
          const Bytes sd = pac::Decode(nb.data() + se.off, se.size);
          if (sd.size() != 784 && sd.size() != 896) continue;
          const bool big = sd.size() == 896;
          LightSet ls;
          ls.id = int(se.id);
          for (int i = 0; i < 4; ++i)
            for (int k = 0; k < 3; ++k) {
              ls.dir[i][k] = BeF(sd.data() + 0x10 + 16 * i + 4 * k);
              ls.col[i][k] = BeF(sd.data() + (big ? 0x60 : 0x50) + 16 * i + 4 * k);
            }
          for (int i = 0; i < 4; ++i) {  // Session 98 (skin fix): the colour's w = the light's entrance specular-lobe weight
            const float w = BeF(sd.data() + (big ? 0x60 : 0x50) + 16 * i + 12);
            ls.wt[i] = std::isfinite(w) ? w : 0.f;
          }
          if (!big)
            for (int k = 0; k < 3; ++k) ls.sky[k] = BeF(sd.data() + 0x100 + 4 * k), ls.ground[k] = BeF(sd.data() + 0x110 + 4 * k);
          if (big)  // Session 98 (match oracle): the cube falloff per light (asset_model.h LightSet::fall)
            for (int i = 0; i < 4; ++i) {
              const float f = BeF(sd.data() + 0x140 + 4 * i);
              ls.fall[i] = std::isfinite(f) ? std::clamp(f, 0.f, 1.f) : 0.f;
            }
          lights[ls.id] = ls;
        }
      if (stage_key::Cfg().enabled) {
        const Bytes kb = pac::Get(b, p, 30000);
        stage_key::Index ki;
        if (!kb.empty() && ki.Parse(kb))
          for (auto& kv : lights) {
            LightSet k;
            if (!stage_key::EvalLightSet(ki, kv.first, stage_key::Cfg().time, k)) continue;
            std::memcpy(kv.second.dir, k.dir, sizeof k.dir);
            std::memcpy(kv.second.col, k.col, sizeof k.col);
            std::memcpy(kv.second.sky, k.sky, sizeof k.sky);
            std::memcpy(kv.second.ground, k.ground, sizeof k.ground);
          }
      }
      for (auto it = lights.begin(); it != lights.end();) {  // a set with any non-finite number would poison the shader: drop it (-> the default rig)
        bool bad = false;
        for (int i = 0; i < 4; ++i)
          for (int k = 0; k < 3; ++k)
            if (!std::isfinite(it->second.dir[i][k]) || !std::isfinite(it->second.col[i][k])) bad = true;
        for (int k = 0; k < 3; ++k)
          if (!std::isfinite(it->second.sky[k]) || !std::isfinite(it->second.ground[k])) bad = true;
        it = bad ? lights.erase(it) : std::next(it);
      }
      return true;
    }
  }
  return err = "no entry 50003 in this arena pac", false;
}

// The flat floor pieces of an arena (vertices of meshes that are drawn once as stored, not skinned, with a height extent <= 6),
// kept after the arena's textures are gone. Viewer space (Y up).
struct Flat {
  std::vector<std::array<float, 3>> v;
  void Collect(const Model& m, const float at[3] = nullptr) {
    v.clear();
    for (const Mesh& me : m.meshes) {
      if (me.skinned || me.placement >= 0 || me.vertices.empty()) continue;
      float lo = 1e30f, hi = -1e30f;
      for (const Vertex& x : me.vertices) {
        if (!std::isfinite(x.pos[1])) continue;
        lo = std::min(lo, x.pos[1]), hi = std::max(hi, x.pos[1]);
      }
      if (!(hi - lo <= 6.f)) continue;
      for (const Vertex& x : me.vertices)
        if (std::isfinite(x.pos[0]) && std::isfinite(x.pos[1]) && std::isfinite(x.pos[2]))
          v.push_back({x.pos[0] + (at ? at[0] : 0.f), x.pos[1] + (at ? at[1] : 0.f), x.pos[2] + (at ? at[2] : 0.f)});
    }
  }
  // Median height of the vertices inside the rectangle; false when there are none.
  bool FloorIn(float xa, float xb, float za, float zb, float& y) const {
    std::vector<float> ys;
    for (const auto& p : v)
      if (p[0] >= std::min(xa, xb) && p[0] <= std::max(xa, xb) && p[2] >= std::min(za, zb) && p[2] <= std::max(za, zb)) ys.push_back(p[1]);
    if (ys.empty()) return false;
    std::nth_element(ys.begin(), ys.begin() + (std::ptrdiff_t)ys.size() / 2, ys.end());
    y = ys[ys.size() / 2];
    return true;
  }
};

// Session 98 (generalisation, run 6819): the walkable surfaces of an arena's stage slots (entries 0-399, drawn as stored), as
// up-facing triangles in viewer space (Y up), bucketed on a 64-unit XZ grid. Top(x, z, ceiling) = the highest surface under (x, z)
// at or below `ceiling`. The entrance grounding (viewer_main.cpp ApplyEntrance) reads it: RAW's raised stage (deck 180, the ramp
// sloping to the floor) is where the game lifts the entrant by exactly this height [C: probe run 6819, see the call site].
struct Ground {
  struct Tri { float a[3], b[3], c[3]; };
  std::vector<Tri> tris;
  std::map<std::pair<int, int>, std::vector<uint32_t>> grid;
  static constexpr float kCell = 64.f;
  void Collect(const Model& m, const float at[3] = nullptr) {
    tris.clear(), grid.clear();
    for (const Mesh& me : m.meshes) {
      if (me.skinned || me.placement >= 0 || me.src_kind != 1 || me.src_key < 0 || me.src_key >= 400) continue;
      for (size_t i = 0; i + 2 < me.indices.size(); i += 3) {
        Tri t;
        float* d[3] = {t.a, t.b, t.c};
        bool ok = true;
        for (int k = 0; k < 3; ++k) {
          const uint32_t ix = me.indices[i + (size_t)k];
          if (ix >= me.vertices.size()) { ok = false; break; }
          for (int q = 0; q < 3; ++q) d[k][q] = me.vertices[ix].pos[q] + (at ? at[q] : 0.f), ok = ok && std::isfinite(d[k][q]);
        }
        if (!ok) continue;
        // up-facing only (|n.y| / |n| > 0.5, either winding): floors, decks and ramps, not walls
        const float ux = t.b[0] - t.a[0], uy = t.b[1] - t.a[1], uz = t.b[2] - t.a[2], vx = t.c[0] - t.a[0], vy = t.c[1] - t.a[1], vz = t.c[2] - t.a[2];
        const float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (!(nl > 1e-6f) || std::fabs(ny) < 0.5f * nl) continue;
        const uint32_t id = (uint32_t)tris.size();
        tris.push_back(t);
        const float x0 = std::min({t.a[0], t.b[0], t.c[0]}), x1 = std::max({t.a[0], t.b[0], t.c[0]});
        const float z0 = std::min({t.a[2], t.b[2], t.c[2]}), z1 = std::max({t.a[2], t.b[2], t.c[2]});
        if ((x1 - x0) / kCell * (z1 - z0) / kCell > 4096.f) continue;  // (a huge sheet: skipped, the floor under it is 0 anyway)
        for (int gx = (int)std::floor(x0 / kCell); gx <= (int)std::floor(x1 / kCell); ++gx)
          for (int gz = (int)std::floor(z0 / kCell); gz <= (int)std::floor(z1 / kCell); ++gz) grid[{gx, gz}].push_back(id);
      }
    }
  }
  bool Top(float x, float z, float ceiling, float& y) const {
    auto it = grid.find({(int)std::floor(x / kCell), (int)std::floor(z / kCell)});
    if (it == grid.end()) return false;
    bool got = false;
    for (uint32_t id : it->second) {
      const Tri& t = tris[id];
      const float d0 = (t.b[0] - t.a[0]) * (t.c[2] - t.a[2]) - (t.c[0] - t.a[0]) * (t.b[2] - t.a[2]);
      if (std::fabs(d0) < 1e-6f) continue;
      const float u = ((x - t.a[0]) * (t.c[2] - t.a[2]) - (t.c[0] - t.a[0]) * (z - t.a[2])) / d0;
      const float v = ((t.b[0] - t.a[0]) * (z - t.a[2]) - (x - t.a[0]) * (t.b[2] - t.a[2])) / d0;
      if (u < 0 || v < 0 || u + v > 1) continue;
      const float h = t.a[1] + u * (t.b[1] - t.a[1]) + v * (t.c[1] - t.a[1]);
      if (h <= ceiling && (!got || h > y)) y = h, got = true;
    }
    return got;
  }
};

struct Spot {
  std::string name;
  float pos[3] = {0, 0, 0};  // the feet
  float yaw = 0;             // degrees (default facing)
  int light = -1;            // the arena light set used there (200 stage, 201 ramp), -1 = the default match rig
  std::string note;          // where the numbers came from
};

constexpr float kCanvasTop = 111.f, kCanvasHalf = 318.8f, kCornerInset = 240.f, kRingsideZ = -496.f;
constexpr float kPi = 3.14159265358979f;

inline float YawToward(float dx, float dz) {
  float y = std::atan2(dx, dz) * 180.f / kPi;
  return y <= -179.999f ? y + 360.f : y;  // (180, not -180, for "toward -Z")
}

struct ArenaSpots {
  std::vector<Rect> rects;
  std::map<int, LightSet> lights;  // the zone light sets 200 / 201 (when the pac has them)
  bool have_rects = false;
  bool zones_tried = false;  // the pac was read once (ok or not); zones_err = why not
  std::string zones_err;
  Flat flat;

  static const char* Names() { return "stage|ramp|ring|ring_ne|ring_nw|ring_se|ring_sw|ringside"; }
  static const std::vector<std::string>& Cycle() {
    static const std::vector<std::string> c = {"stage", "ramp", "ring", "ring_ne", "ring_nw", "ring_se", "ring_sw", "ringside"};
    return c;
  }

  const Rect* StageRect() const {
    const Rect* best = nullptr;
    for (const Rect& r : rects)
      if (r.light == 200 && (!best || r.zmax() > best->zmax())) best = &r;
    if (!best)
      for (const Rect& r : rects)
        if (!best || r.zmax() > best->zmax()) best = &r;
    return best;
  }
  // The ramp line: through the ramp rects (light 201, else every other narrow rect), each along its long side, ordered from
  // the stage end. t = 0 stage end, 1 ring end. Unconfirmed for the arenas with several rects (L-shaped ramps).
  std::vector<std::array<float, 2>> RampLine() const {
    const Rect* st = StageRect();
    std::vector<const Rect*> rs;
    for (const Rect& r : rects)
      if (r.light == 201) rs.push_back(&r);
    if (rs.empty())
      for (const Rect& r : rects)
        if (&r != st && r.xmax() - r.xmin() <= 700.f) rs.push_back(&r);
    std::vector<std::array<float, 2>> line;
    if (rs.empty()) return line;
    const float sx = st ? (st->xmin() + st->xmax()) * 0.5f : 0.f, sz = st ? (st->zmin() + st->zmax()) * 0.5f : 3000.f;
    auto d2 = [&](float x, float z) { return (x - sx) * (x - sx) + (z - sz) * (z - sz); };
    std::sort(rs.begin(), rs.end(), [&](const Rect* a, const Rect* b) {
      return d2((a->xmin() + a->xmax()) * 0.5f, (a->zmin() + a->zmax()) * 0.5f) < d2((b->xmin() + b->xmax()) * 0.5f, (b->zmin() + b->zmax()) * 0.5f);
    });
    float px = sx, pz = sz;
    for (const Rect* r : rs) {
      const float cx = (r->xmin() + r->xmax()) * 0.5f, cz = (r->zmin() + r->zmax()) * 0.5f;
      std::array<float, 2> a, b;
      if (r->zmax() - r->zmin() >= r->xmax() - r->xmin()) a = {cx, r->zmin()}, b = {cx, r->zmax()};  // runs along Z
      else a = {r->xmin(), cz}, b = {r->xmax(), cz};                                                  // runs along X
      const float da = (a[0] - px) * (a[0] - px) + (a[1] - pz) * (a[1] - pz), db = (b[0] - px) * (b[0] - px) + (b[1] - pz) * (b[1] - pz);
      if (db < da) std::swap(a, b);  // start at the end nearer the previous point
      line.push_back(a);
      line.push_back(b);
      px = b[0], pz = b[1];
    }
    // the stage end first: if the line's first point is nearer the ring than its last, reverse it
    if (line.size() >= 2 && line.front()[0] * line.front()[0] + line.front()[1] * line.front()[1] <
                                line.back()[0] * line.back()[0] + line.back()[1] * line.back()[1])
      std::reverse(line.begin(), line.end());
    return line;
  }

  // Resolve a spot. `t` (0..1) walks the ramp. False with err for an unknown name or a missing rectangle.
  bool Resolve(const std::string& name, double t, Spot& s, std::string& err) const {
    s = Spot();
    s.name = name;
    float fy = 0;
    if (name == "stage") {
      const Rect* r = StageRect();
      if (!r) return err = "arena has no entrance stage rectangle (entry 50003" + (zones_err.empty() ? std::string() : ": " + zones_err) + ")", false;
      const float x = (r->xmin() + r->xmax()) * 0.5f;
      const float z = r->zmin() + std::min(150.f, (r->zmax() - r->zmin()) * 0.5f);  // ring-side centre region (bg05: z 3050)
      if (!flat.FloorIn(r->xmin(), r->xmax(), r->zmin(), r->zmax(), fy)) fy = 0;
      s.pos[0] = x, s.pos[1] = fy, s.pos[2] = z;
      s.yaw = YawToward(-x, -z);
      s.light = 200;
      s.note = "entrance rect (set 200): ring-side centre, floor = median flat slab in the rect";
      return true;
    }
    if (name == "ramp") {
      const auto line = RampLine();
      if (line.size() < 2) return err = "arena has no ramp rectangle (entry 50003" + (zones_err.empty() ? std::string() : ": " + zones_err) + ")", false;
      t = std::max(0.0, std::min(1.0, t));
      float total = 0;
      for (size_t i = 0; i + 1 < line.size(); ++i) total += std::hypot(line[i + 1][0] - line[i][0], line[i + 1][1] - line[i][1]);
      float want = (float)t * total, x = line.back()[0], z = line.back()[1], dx = 0, dz = -1;
      for (size_t i = 0; i + 1 < line.size(); ++i) {
        const float L = std::hypot(line[i + 1][0] - line[i][0], line[i + 1][1] - line[i][1]);
        if (L < 1e-3f) continue;
        dx = (line[i + 1][0] - line[i][0]) / L, dz = (line[i + 1][1] - line[i][1]) / L;
        if (want <= L || i + 2 == line.size()) {
          const float k = std::min(want, L);
          x = line[i][0] + dx * k, z = line[i][1] + dz * k;
          break;
        }
        want -= L;
      }
      // floor: the median over the ramp rects (light 201, else the narrow ones) around the point
      bool got = flat.FloorIn(x - 60.f, x + 60.f, z - 60.f, z + 60.f, fy);
      if (!got) {
        for (const Rect& r : rects)
          if (r.light == 201 && flat.FloorIn(r.xmin(), r.xmax(), r.zmin(), r.zmax(), fy)) { got = true; break; }
      }
      if (!got) fy = 0;
      s.pos[0] = x, s.pos[1] = fy, s.pos[2] = z;
      s.yaw = YawToward(dx, dz);
      s.light = 201;
      s.note = "ramp rect line (set 201), t 0 = stage end, 1 = ring end; floor = median flat slab near the point";
      return true;
    }
    if (name == "ring") {
      s.pos[1] = kCanvasTop, s.yaw = 0;  // faces the stage (+Z)
      s.note = "ring centre on the canvas top (Y up +111), faces the stage";
      return true;
    }
    if (name == "ring_ne" || name == "ring_nw" || name == "ring_se" || name == "ring_sw") {
      const float zz = name[5] == 'n' ? kCornerInset : -kCornerInset, xx = name[6] == 'e' ? kCornerInset : -kCornerInset;
      s.pos[0] = xx, s.pos[1] = kCanvasTop, s.pos[2] = zz;
      s.yaw = YawToward(-xx, -zz);
      s.note = "ring corner (inset 240 of the +-318.8 canvas), N = +Z (stage side), E = +X, faces the ring centre";
      return true;
    }
    if (name == "ringside") {
      if (!flat.FloorIn(-150.f, 150.f, -600.f, -400.f, fy)) fy = 0;
      s.pos[0] = 0, s.pos[1] = fy, s.pos[2] = kRingsideZ;
      s.yaw = 0;  // faces the ring (+Z)
      s.note = "between the apron (-321) and the announce desk edge (-670); floor = median flat slab; faces the ring";
      return true;
    }
    return err = "unknown spot '" + name + "' (" + Names() + ")", false;
  }
};

}  // namespace spots
}  // namespace assets
