// Session 98 (plan step 12): keyframed camera paths for the Arena Viewer. Portable (std only, plus json_lite.h): no Win32, no D3D.
// A path = keys of the free pose (pos, yaw, pitch, roll, fov) at times in seconds. Between two keys:
//   position   centripetal Catmull-Rom (Barry-Goldman form), end keys mirrored so the first / last segment have a tangent
//   angles     yaw / pitch / roll / fov are unwrapped along the key list (shortest way round between neighbours), then cubic Hermite with
//              Catmull-Rom tangents (so yaw 350 -> 10 turns 20 degrees, not 340)
//   timing     per-key ease: ease_out of the first key and ease_in of the second key bend the segment's time curve (0 = the spline's own
//              speed, 1 = stops at that key); the curve u' is a Hermite with end slopes (1 - ease), always monotonic
// Camera only; the scene clock is the viewer's (linking: viewer_main.cpp).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "json_lite.h"

namespace view {

struct CamPose {
  float pos[3] = {0, 0, 0};
  float yaw = 0, pitch = 0, roll = 0, fov = 40;  // degrees (camera.h's free pose)
};

struct CamKey {
  double t = 0;  // seconds
  CamPose p;
  float ease_in = 0, ease_out = 0;  // 0..1
};

class CamPath {
 public:
  std::vector<CamKey> keys;  // sorted by t
  bool loop = false;         // playback wraps from the end to the start (the spline itself is open)

  double Start() const { return keys.empty() ? 0.0 : keys.front().t; }
  double End() const { return keys.empty() ? 0.0 : keys.back().t; }
  double Duration() const { return End() - Start(); }

  // Insert a key (a key within 1e-4 s of an existing one replaces it). Returns its index.
  int Add(const CamKey& k) {
    for (size_t i = 0; i < keys.size(); i++)
      if (std::fabs(keys[i].t - k.t) < 1e-4) { keys[i] = k; keys[i].t = k.t; return (int)i; }
    size_t at = 0;
    while (at < keys.size() && keys[at].t < k.t) at++;
    keys.insert(keys.begin() + (std::ptrdiff_t)at, k);
    return (int)at;
  }
  bool Remove(int i) {
    if (i < 0 || i >= (int)keys.size()) return false;
    keys.erase(keys.begin() + i);
    return true;
  }
  // Change key i (every field of `k` replaces it); the list is re-sorted. Returns the new index, -1 for a bad index, -2 when the new
  // time collides with another key.
  int Set(int i, const CamKey& k) {
    if (i < 0 || i >= (int)keys.size()) return -1;
    for (size_t j = 0; j < keys.size(); j++)
      if ((int)j != i && std::fabs(keys[j].t - k.t) < 1e-4) return -2;
    keys.erase(keys.begin() + i);
    return Add(k);
  }

  // Wrap a playback time into [Start, End] when looping; clamp otherwise.
  double Wrap(double t) const {
    if (keys.size() < 2) return Start();
    const double s = Start(), d = Duration();
    if (loop && d > 1e-9) { t = std::fmod(t - s, d); if (t < 0) t += d; return s + t; }
    return std::min(std::max(t, s), End());
  }

  // The pose at time t (clamped to the first / last key). False for an empty path.
  bool Eval(double t, CamPose& out) const {
    const size_t n = keys.size();
    if (n == 0) return false;
    if (n == 1 || t <= keys.front().t) { out = keys.front().p; return true; }
    if (t >= keys.back().t) { out = keys.back().p; return true; }
    size_t i = 0;
    while (i + 2 < n && t >= keys[i + 1].t) i++;  // keys[i].t <= t < keys[i+1].t
    const double dt = keys[i + 1].t - keys[i].t;
    double u = (t - keys[i].t) / dt;
    // time curve
    {
      const double m0 = 1.0 - keys[i].ease_out, m1 = 1.0 - keys[i + 1].ease_in;
      const double u2 = u * u, u3 = u2 * u;
      u = (u3 - 2 * u2 + u) * m0 + (-2 * u3 + 3 * u2) + (u3 - u2) * m1;
    }
    // position: centripetal Catmull-Rom through p0 p1 p2 p3
    double P[4][3];
    auto Pt = [&](long j, double* o) {
      long a = std::min(std::max(j, 0L), (long)n - 1);
      for (int c = 0; c < 3; c++) o[c] = keys[(size_t)a].p.pos[c];
    };
    Pt((long)i, P[1]);
    Pt((long)i + 1, P[2]);
    if (i == 0) for (int c = 0; c < 3; c++) P[0][c] = 2 * P[1][c] - P[2][c];
    else Pt((long)i - 1, P[0]);
    if (i + 2 >= n) for (int c = 0; c < 3; c++) P[3][c] = 2 * P[2][c] - P[1][c];
    else Pt((long)i + 2, P[3]);
    double kn[4] = {0, 0, 0, 0};
    for (int j = 1; j < 4; j++) {
      double d2 = 0;
      for (int c = 0; c < 3; c++) d2 += (P[j][c] - P[j - 1][c]) * (P[j][c] - P[j - 1][c]);
      kn[j] = kn[j - 1] + std::max(std::pow(d2, 0.25), 1e-4);  // |d|^0.5 (centripetal), floored for repeated points
    }
    const double s = kn[1] + u * (kn[2] - kn[1]);
    double r[3];
    for (int c = 0; c < 3; c++) {
      const double a1 = (kn[1] - s) / (kn[1] - kn[0]) * P[0][c] + (s - kn[0]) / (kn[1] - kn[0]) * P[1][c];
      const double a2 = (kn[2] - s) / (kn[2] - kn[1]) * P[1][c] + (s - kn[1]) / (kn[2] - kn[1]) * P[2][c];
      const double a3 = (kn[3] - s) / (kn[3] - kn[2]) * P[2][c] + (s - kn[2]) / (kn[3] - kn[2]) * P[3][c];
      const double b1 = (kn[2] - s) / (kn[2] - kn[0]) * a1 + (s - kn[0]) / (kn[2] - kn[0]) * a2;
      const double b2 = (kn[3] - s) / (kn[3] - kn[1]) * a2 + (s - kn[1]) / (kn[3] - kn[1]) * a3;
      r[c] = (kn[2] - s) / (kn[2] - kn[1]) * b1 + (s - kn[1]) / (kn[2] - kn[1]) * b2;
      out.pos[c] = (float)r[c];
    }
    // angles + fov: unwrapped scalar tracks, Hermite with Catmull-Rom tangents (per unit u)
    double* none = nullptr;
    (void)none;
    for (int ch = 0; ch < 4; ch++) {
      auto val = [&](long j) { return Unwrapped(ch, (size_t)std::min(std::max(j, 0L), (long)n - 1)); };
      const double p1 = val((long)i), p2 = val((long)i + 1);
      const double pm = i == 0 ? p1 : val((long)i - 1), pn = i + 2 >= n ? p2 : val((long)i + 2);
      const double tm = i == 0 ? keys[i].t : keys[i - 1].t, tn = i + 2 >= n ? keys[i + 1].t : keys[i + 2].t;
      // tangent at key i / i+1 as value per second, scaled by dt for the u parameter
      const double m1 = (i == 0) ? (p2 - p1) / dt : (p2 - pm) / (keys[i + 1].t - tm);
      const double m2 = (i + 2 >= n) ? (p2 - p1) / dt : (pn - p1) / (tn - keys[i].t);
      const double u2 = u * u, u3 = u2 * u;
      double v = (2 * u3 - 3 * u2 + 1) * p1 + (u3 - 2 * u2 + u) * m1 * dt + (-2 * u3 + 3 * u2) * p2 + (u3 - u2) * m2 * dt;
      if (ch == 0) out.yaw = (float)WrapDeg(v);
      else if (ch == 1) out.pitch = (float)std::min(89.5, std::max(-89.5, v));
      else if (ch == 2) out.roll = (float)WrapDeg(v);
      else out.fov = (float)std::min(120.0, std::max(2.0, v));
    }
    return true;
  }

  // ---- JSON (path_save / path_load) ----
  std::string ToJson() const {
    std::string s = "{\"version\":1,\"loop\":" + std::string(loop ? "true" : "false") + ",\"keys\":[";
    for (size_t i = 0; i < keys.size(); i++) {
      if (i) s += ",";
      s += KeyJson(keys[i]);
    }
    return s + "]}\n";
  }
  static std::string KeyJson(const CamKey& k) {
    char b[400];
    snprintf(b, sizeof b,
             "{\"t\":%.9g,\"pos\":[%.9g,%.9g,%.9g],\"yaw\":%.9g,\"pitch\":%.9g,\"roll\":%.9g,\"fov\":%.9g,\"ease_in\":%.9g,\"ease_out\":%.9g}",
             k.t, k.p.pos[0], k.p.pos[1], k.p.pos[2], k.p.yaw, k.p.pitch, k.p.roll, k.p.fov, k.ease_in, k.ease_out);
    return b;
  }
  // Fill `k` from the numeric fields of an object (missing fields keep what `k` has). False + err for a wrong type.
  static bool KeyFrom(const jl::Value& o, CamKey& k, std::string& err) {
    if (o.t != jl::Value::Obj) return err = "a key must be an object", false;
    if (const jl::Value* pv = o.get("pos")) {
      if (pv->t != jl::Value::Arr || pv->a.size() != 3) return err = "'pos' must be [x,y,z]", false;
      for (int i = 0; i < 3; i++) {
        if (pv->a[(size_t)i].t != jl::Value::Num) return err = "'pos' must be three numbers", false;
        k.p.pos[i] = (float)pv->a[(size_t)i].n;
      }
    }
    for (const char* f : {"t", "yaw", "pitch", "roll", "fov", "ease_in", "ease_out"})
      if (const jl::Value* v = o.get(f); v && v->t != jl::Value::Num) return err = std::string("'") + f + "' must be a number", false;
    k.t = o.num("t", k.t);
    k.p.yaw = (float)o.num("yaw", k.p.yaw);
    k.p.pitch = (float)o.num("pitch", k.p.pitch);
    k.p.roll = (float)o.num("roll", k.p.roll);
    k.p.fov = (float)o.num("fov", k.p.fov);
    k.ease_in = std::min(1.f, std::max(0.f, (float)o.num("ease_in", k.ease_in)));
    k.ease_out = std::min(1.f, std::max(0.f, (float)o.num("ease_out", k.ease_out)));
    if (!(k.p.fov >= 2 && k.p.fov <= 120)) k.p.fov = std::min(120.f, std::max(2.f, k.p.fov));
    k.p.pitch = std::min(89.5f, std::max(-89.5f, k.p.pitch));
    return true;
  }
  bool FromJson(const std::string& text, std::string& err) {
    jl::Value v;
    if (!jl::Parse(text, v, err)) return false;
    if (v.t != jl::Value::Obj) return err = "path file must be a JSON object", false;
    const jl::Value* ks = v.get("keys");
    if (!ks || ks->t != jl::Value::Arr) return err = "path file has no \"keys\" array", false;
    CamPath np;
    if (const jl::Value* lv = v.get("loop")) np.loop = lv->t == jl::Value::Bool ? lv->b : lv->n != 0;
    for (const jl::Value& o : ks->a) {
      CamKey k;
      if (!KeyFrom(o, k, err)) return false;
      if (!o.has("t")) return err = "a key has no \"t\"", false;
      np.Add(k);
    }
    *this = np;
    return true;
  }

  static double WrapDeg(double a) {
    a = std::fmod(a + 180.0, 360.0);
    if (a < 0) a += 360.0;
    return a - 180.0;
  }

 private:
  // channel 0 yaw, 1 pitch, 2 roll, 3 fov of key j, with the angles unwrapped from key 0 (each step is the shortest way round)
  double Unwrapped(int ch, size_t j) const {
    auto raw = [&](size_t q) { const CamPose& p = keys[q].p; return double(ch == 0 ? p.yaw : ch == 1 ? p.pitch : ch == 2 ? p.roll : p.fov); };
    double v = raw(0);
    if (ch == 1 || ch == 3) return raw(j);  // pitch / fov never wrap
    for (size_t q = 1; q <= j; q++) v += WrapDeg(raw(q) - raw(q - 1));
    return v;
  }
};

// Panel / viewer shared state (no ImGui here: the panel reads it, the viewer fills it).
struct PathUi {
  CamPath path;
  double time = 0;           // the timeline cursor, seconds
  bool playing = false, link = true;  // link: the scene clock follows the path time (actors' motions play in sync)
  int selected = -1;
  // render job progress (the viewer copies it each frame)
  bool rendering = false;
  int frame = 0, frames = 0;
  std::string render_msg, render_out;
};

}  // namespace view
