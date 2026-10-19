// wwe2k14 - CineCam keyframed shots (Session 35, SESSION35_PLAN.md): the engine. Pure C++ (no game memory, no SDK),
// so tools\cinecam\keys_test.cpp tests it offline; src\cinecam.h plays shots through it at the renderer hand-off.
//
// A shot is a length plus five independent tracks of keys (time in seconds, values, a curve, an ease):
//   move   x y z (world, Y down, as the free camera's pos), or with a move target (orbit): distance (cm, level),
//          azimuth (degrees: the camera's heading when it looks at the target, so azimuth = the yaw it would have),
//          height (cm above the target, + = up)
//   aim    yaw pitch (degrees, as the free camera), or with an aim target (look at): framing x y = where the target
//          sits on screen in half-widths / half-heights from the centre (+x right, +y up; 0.333 = on a third line)
//   roll   degrees (Dutch angle)
//   lens   vertical FOV in degrees (the game's view block +128 is vertical: PLAYBOOK Session 18, run 1395)
//   focus  distance (cm; with a focus target: an offset from the target's distance) and aperture (a scale of the
//          depth of field's in-focus distances, 1 = CineCam's defaults, as photo mode's g_aperture)
// A track without keys is left to the free camera (fly by hand while only the lens or the focus is animated).
//
// Interpolation:
//   - scalars (aim, roll, lens, focus, orbit values): a monotone cubic through the keys in time (Fritsch-Butland
//     tangents: no overshoot past a key's value, no bounce in a zoom or a focus pull), in a space chosen per value:
//     the lens in log(tan(fov / 2)) (an even-looking zoom), the focus distance in dioptres (1 / d, like a focus ring),
//     the aperture in log; angles are not wrapped (a 270-degree orbit is 0 -> 270; capturing from the camera picks
//     the turn nearest the neighbouring key);
//   - the move path: a centripetal Catmull-Rom spline through the positions (no loops or overshoot on uneven key
//     spacing), timed separately: the distance travelled along the path is a monotone cubic through the keys'
//     cumulative path lengths (each key is reached at its time), or, with constant speed, an even speed with cosine
//     ramps at the start and the end (the keys in between are passed when the path gets there, as After Effects'
//     roving keys);
//   - ease (0..1) per key: how much the motion slows into and out of that key (1 = from / to a stop, 0 = at full speed
//     through it); -1 = auto (1 at the first and last key, 0 between). The curve of a key shapes the segment leaving
//     it: smooth, linear (straight, even), hold (stays until the next key, then cuts).
// Every evaluation is a pure function of the shot, the time and the target positions, so a re-shoot from a
// recording gives the same camera every pass.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wwe2k14::cinecam::keys {

enum Track : int { kMove = 0, kAim, kRoll, kLens, kFocus, kTrackCount };
inline constexpr const char* kTrackNames[kTrackCount] = {"move", "aim", "roll", "lens", "focus"};
inline constexpr int kTrackDims[kTrackCount] = {3, 2, 1, 1, 2};

enum class Curve : int { kSmooth = 0, kLinear = 1, kHold = 2 };
inline constexpr const char* kCurveNames[3] = {"smooth", "linear", "hold"};

struct Key {
  double t = 0;
  Curve curve = Curve::kSmooth;
  float ease = -1;  // -1 = auto
  float v[3] = {};
};

enum class TargetKind : int { kNone = 0, kSlot = 1, kPin = 2 };
struct Target {
  TargetKind kind = TargetKind::kNone;
  int slot = 0;        // character slot 0..5 (the table 0x836F38D0)
  float pin[3] = {};   // world position (Y down)
  bool on() const { return kind != TargetKind::kNone; }
};

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

struct Shot {
  double length = 8.0;
  bool constant_speed = false;
  float follow = 0.25f;  // target smoothing time constant (s); applied by the caller (src\cinecam.h)
  Target aim, move, focus;
  std::vector<Key> keys[kTrackCount];
  // Path cache (move track in world space), rebuilt when `rev` changes.
  unsigned rev = 1;
  mutable unsigned cache_rev = 0;
  mutable std::vector<double> cum_at_key;             // path length at each key
  mutable std::vector<std::vector<double>> seg_table;  // per segment: path length at kSamples+1 even parameter steps
  void Touch() { ++rev; }
};

constexpr int kSamples = 256;  // per segment (64 left a 2.5% speed wobble on a 10 m segment: keys_test check 2)

// The camera state an evaluation starts from and returns. `has[track]` = the shot set that track.
struct Pose {
  float pos[3] = {};
  float yaw = 0, pitch = 0, roll = 0;
  float fov = 30;
  float focus_cm = 1000, aperture = 1;
  bool has[kTrackCount] = {};
};

inline float EaseOf(const std::vector<Key>& k, size_t i) {
  if (k[i].ease >= 0) return std::clamp(k[i].ease, 0.0f, 1.0f);
  return (i == 0 || i + 1 == k.size()) ? 1.0f : 0.0f;
}

// ---- value spaces ----
inline double ToLin(int track, int dim, float v, bool targeted) {
  if (track == kLens) return std::log(std::tan(std::clamp(double(v), 0.5, 170.0) * kDeg * 0.5));
  if (track == kFocus && dim == 0 && !targeted) return 1.0 / std::max(double(v), 1.0);
  if (track == kFocus && dim == 1) return std::log(std::max(double(v), 0.01));
  return v;
}
inline float FromLin(int track, int dim, double x, bool targeted) {
  if (track == kLens) return float(2.0 * std::atan(std::exp(x)) / kDeg);
  if (track == kFocus && dim == 0 && !targeted) return float(1.0 / std::max(x, 1e-6));
  if (track == kFocus && dim == 1) return float(std::exp(x));
  return float(x);
}

// Monotone cubic (Fritsch-Butland) tangents for points (t_i, y_i), scaled by (1 - ease_i).
inline void Tangents(const std::vector<double>& t, const std::vector<double>& y, const std::vector<float>& ease,
                     std::vector<double>& m) {
  const size_t n = t.size();
  m.assign(n, 0.0);
  if (n < 2) return;
  std::vector<double> h(n - 1), d(n - 1);
  for (size_t k = 0; k + 1 < n; ++k) {
    h[k] = std::max(t[k + 1] - t[k], 1e-9);
    d[k] = (y[k + 1] - y[k]) / h[k];
  }
  m[0] = d[0];
  m[n - 1] = d[n - 2];
  for (size_t i = 1; i + 1 < n; ++i) {
    if (d[i - 1] * d[i] <= 0) {
      m[i] = 0;
    } else {
      const double w1 = 2 * h[i] + h[i - 1], w2 = h[i] + 2 * h[i - 1];
      m[i] = (w1 + w2) / (w1 / d[i - 1] + w2 / d[i]);
    }
  }
  for (size_t i = 0; i < n; ++i) m[i] *= 1.0 - ease[i];
}

inline double Hermite(double y0, double y1, double m0, double m1, double h, double s) {
  const double s2 = s * s, s3 = s2 * s;
  return (2 * s3 - 3 * s2 + 1) * y0 + (s3 - 2 * s2 + s) * h * m0 + (-2 * s3 + 3 * s2) * y1 + (s3 - s2) * h * m1;
}

// Index of the segment [k, k+1] holding t (t clamped to the keys' range by the caller).
inline size_t SegmentAt(const std::vector<Key>& k, double t) {
  size_t i = 0;
  while (i + 2 < k.size() && k[i + 1].t <= t) ++i;
  return i;
}

// One value of a scalar track at time t (keys sorted, at least one).
inline float EvalScalar(const std::vector<Key>& k, int track, int dim, double t, bool targeted) {
  if (k.size() == 1 || t <= k.front().t) return k.front().v[dim];
  if (t >= k.back().t) return k.back().v[dim];
  std::vector<double> ts(k.size()), ys(k.size()), m;
  std::vector<float> e(k.size());
  for (size_t i = 0; i < k.size(); ++i) {
    ts[i] = k[i].t;
    ys[i] = ToLin(track, dim, k[i].v[dim], targeted);
    e[i] = EaseOf(k, i);
  }
  Tangents(ts, ys, e, m);
  const size_t i = SegmentAt(k, t);
  const double h = std::max(ts[i + 1] - ts[i], 1e-9), s = (t - ts[i]) / h;
  double x;
  switch (k[i].curve) {
    case Curve::kHold: x = ys[i]; break;
    case Curve::kLinear: x = ys[i] + (ys[i + 1] - ys[i]) * s; break;
    default: x = Hermite(ys[i], ys[i + 1], m[i], m[i + 1], h, s); break;
  }
  return FromLin(track, dim, x, targeted);
}

// ---- the move path (world space) ----
inline double Dist3(const float a[3], const float b[3]) {
  const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
  return std::sqrt(x * x + y * y + z * z);
}

// Point at parameter u (0..1) of segment i of the spline through the move keys.
inline void PathPoint(const std::vector<Key>& k, size_t i, double u, float out[3]) {
  const float* p1 = k[i].v;
  const float* p2 = k[i + 1].v;
  if (k[i].curve == Curve::kLinear) {
    for (int a = 0; a < 3; ++a) out[a] = float(p1[a] + (p2[a] - p1[a]) * u);
    return;
  }
  if (k[i].curve == Curve::kHold) {
    std::memcpy(out, p1, sizeof(float) * 3);
    return;
  }
  float p0[3], p3[3];
  for (int a = 0; a < 3; ++a) {
    p0[a] = i > 0 ? k[i - 1].v[a] : 2 * p1[a] - p2[a];
    p3[a] = i + 2 < k.size() ? k[i + 2].v[a] : 2 * p2[a] - p1[a];
  }
  // Centripetal Catmull-Rom (alpha 0.5, Barry-Goldman).
  const double t0 = 0, t1 = t0 + std::max(std::sqrt(Dist3(p0, p1)), 1e-3), t2 = t1 + std::max(std::sqrt(Dist3(p1, p2)), 1e-3),
               t3 = t2 + std::max(std::sqrt(Dist3(p2, p3)), 1e-3);
  const double t = t1 + (t2 - t1) * u;
  for (int a = 0; a < 3; ++a) {
    const double A1 = ((t1 - t) * p0[a] + (t - t0) * p1[a]) / (t1 - t0);
    const double A2 = ((t2 - t) * p1[a] + (t - t1) * p2[a]) / (t2 - t1);
    const double A3 = ((t3 - t) * p2[a] + (t - t2) * p3[a]) / (t3 - t2);
    const double B1 = ((t2 - t) * A1 + (t - t0) * A2) / (t2 - t0);
    const double B2 = ((t3 - t) * A2 + (t - t1) * A3) / (t3 - t1);
    out[a] = float(((t2 - t) * B1 + (t - t1) * B2) / (t2 - t1));
  }
}

inline void BuildPathCache(const Shot& s) {
  if (s.cache_rev == s.rev) return;
  s.cache_rev = s.rev;
  const auto& k = s.keys[kMove];
  s.cum_at_key.assign(k.size(), 0.0);
  s.seg_table.assign(k.size() > 0 ? k.size() - 1 : 0, {});
  for (size_t i = 0; i + 1 < k.size(); ++i) {
    auto& tab = s.seg_table[i];
    tab.resize(kSamples + 1);
    tab[0] = 0;
    float prev[3];
    PathPoint(k, i, 0.0, prev);
    for (int j = 1; j <= kSamples; ++j) {
      float p[3];
      PathPoint(k, i, double(j) / kSamples, p);
      tab[j] = tab[j - 1] + Dist3(prev, p);
      std::memcpy(prev, p, sizeof(prev));
    }
    if (k[i].curve == Curve::kHold) tab.assign(kSamples + 1, 0.0);
    s.cum_at_key[i + 1] = s.cum_at_key[i] + tab[kSamples];
  }
}

// Position at path length `len` (0 .. total).
inline void PointAtLength(const Shot& s, double len, float out[3]) {
  const auto& k = s.keys[kMove];
  size_t i = 0;
  while (i + 2 < k.size() && s.cum_at_key[i + 1] <= len) ++i;
  const auto& tab = s.seg_table[i];
  const double local = std::clamp(len - s.cum_at_key[i], 0.0, tab[kSamples]);
  if (tab[kSamples] <= 1e-9) {
    PathPoint(k, i, 0.0, out);
    return;
  }
  const int j = int(std::upper_bound(tab.begin(), tab.end(), local) - tab.begin()) - 1;
  const int jj = std::clamp(j, 0, kSamples - 1);
  const double span = tab[jj + 1] - tab[jj];
  const double u = (jj + (span > 1e-12 ? (local - tab[jj]) / span : 0.0)) / kSamples;
  PathPoint(k, i, std::clamp(u, 0.0, 1.0), out);
}

// Fraction (0..1) of the way for constant speed: even speed with cosine ramps of r0 / r1 of the duration.
inline double ConstantSpeedFraction(double u, double r0, double r1) {
  u = std::clamp(u, 0.0, 1.0);
  const double v = 1.0 / (1.0 - 0.5 * (r0 + r1));
  auto ramp = [](double x, double r) { return r > 0 ? x / 2 - r / (2 * kPi) * std::sin(kPi * x / r) : 0.0; };
  double s;
  if (u < r0) {
    s = ramp(u, r0);
  } else if (u <= 1 - r1) {
    s = r0 / 2 + (u - r0);
  } else {
    const double w = 1 - u;  // mirror of the start ramp
    s = (1 - 0.5 * (r0 + r1)) - ramp(w, r1);
  }
  return std::clamp(s * v, 0.0, 1.0);
}

inline void EvalMoveWorld(const Shot& s, double t, float out[3]) {
  const auto& k = s.keys[kMove];
  if (k.size() == 1 || t <= k.front().t) {
    std::memcpy(out, k.front().v, sizeof(float) * 3);
    return;
  }
  if (t >= k.back().t) {
    std::memcpy(out, k.back().v, sizeof(float) * 3);
    return;
  }
  BuildPathCache(s);
  const double total = s.cum_at_key.back();
  bool hold = false;
  for (size_t i = 0; i + 1 < k.size(); ++i) hold = hold || k[i].curve == Curve::kHold;
  double len;
  if (s.constant_speed && !hold) {
    const double u = (t - k.front().t) / std::max(k.back().t - k.front().t, 1e-9);
    len = total * ConstantSpeedFraction(u, 0.25 * EaseOf(k, 0), 0.25 * EaseOf(k, k.size() - 1));
  } else {
    std::vector<double> ts(k.size()), m;
    std::vector<float> e(k.size());
    for (size_t i = 0; i < k.size(); ++i) {
      ts[i] = k[i].t;
      e[i] = EaseOf(k, i);
    }
    Tangents(ts, s.cum_at_key, e, m);
    const size_t i = SegmentAt(k, t);
    const double h = std::max(ts[i + 1] - ts[i], 1e-9), u = (t - ts[i]) / h;
    if (k[i].curve == Curve::kHold) {
      len = s.cum_at_key[i];
    } else if (k[i].curve == Curve::kLinear) {
      len = s.cum_at_key[i] + (s.cum_at_key[i + 1] - s.cum_at_key[i]) * u;
    } else {
      len = Hermite(s.cum_at_key[i], s.cum_at_key[i + 1], m[i], m[i + 1], h, u);
    }
    // The hold segment's path is a cut: stay on the key's point until the next key.
    if (k[i].curve == Curve::kHold) {
      std::memcpy(out, k[i].v, sizeof(float) * 3);
      return;
    }
  }
  PointAtLength(s, std::clamp(len, 0.0, total), out);
}

// ---- angles to / from a target ----
// Yaw / pitch of the direction from `from` to `to` (the free camera's convention: yaw 0 = +X, right = -Z; pitch > 0 =
// up = -Y).
inline void LookAngles(const float from[3], const float to[3], float* yaw, float* pitch) {
  const double dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] - from[2];
  const double n = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (n < 1e-6) return;
  *yaw = float(std::atan2(-dz, dx) / kDeg);
  *pitch = float(std::asin(std::clamp(-dy / n, -1.0, 1.0)) / kDeg);
}

// Framing (half-widths / half-heights, + = right / up) -> the angles to turn away from the target direction.
inline void FramingAngles(float fx, float fy, float fov, float aspect, float* dyaw, float* dpitch) {
  const double tv = std::tan(fov * kDeg * 0.5), th = tv * aspect;
  *dyaw = float(std::atan(fx * th) / kDeg);
  *dpitch = float(std::atan(fy * tv) / kDeg);
}

// Orbit values (distance, azimuth, height) -> world position around `target`.
inline void OrbitPosition(const float target[3], float dist, float az, float height, float out[3]) {
  const double a = az * kDeg;
  out[0] = float(target[0] - dist * std::cos(a));
  out[1] = target[1] - height;
  out[2] = float(target[2] + dist * std::sin(a));
}

// ---- the whole shot ----
// `base` = the free camera's state now (tracks without keys keep it). Targets: world positions already smoothed by
// the caller, or null when the shot has none or it cannot be read this frame (then the targeted part keeps the base
// pose for that frame). aspect = width / height of the picture.
inline Pose Evaluate(const Shot& s, double t, const Pose& base, const float* aim_target, const float* move_target,
                     const float* focus_target, float aspect) {
  Pose p = base;
  std::memset(p.has, 0, sizeof(p.has));
  if (!s.keys[kLens].empty()) {
    p.fov = std::clamp(EvalScalar(s.keys[kLens], kLens, 0, t, false), 1.0f, 120.0f);
    p.has[kLens] = true;
  }
  if (!s.keys[kMove].empty()) {
    if (s.move.on()) {
      if (move_target) {
        const auto& k = s.keys[kMove];
        OrbitPosition(move_target, EvalScalar(k, kMove, 0, t, true), EvalScalar(k, kMove, 1, t, true),
                      EvalScalar(k, kMove, 2, t, true), p.pos);
        p.has[kMove] = true;
      }
    } else {
      EvalMoveWorld(s, t, p.pos);
      p.has[kMove] = true;
    }
  }
  if (s.aim.on()) {
    if (aim_target) {
      float yaw = p.yaw, pitch = p.pitch;
      LookAngles(p.pos, aim_target, &yaw, &pitch);
      float fx = 0, fy = 0;
      if (!s.keys[kAim].empty()) {
        fx = EvalScalar(s.keys[kAim], kAim, 0, t, true);
        fy = EvalScalar(s.keys[kAim], kAim, 1, t, true);
      }
      float dyaw, dpitch;
      FramingAngles(fx, fy, p.fov, aspect, &dyaw, &dpitch);
      p.yaw = yaw - dyaw;  // the subject right of centre = the camera turned left of it
      p.pitch = std::clamp(pitch - dpitch, -89.0f, 89.0f);
      p.has[kAim] = true;
    }
  } else if (!s.keys[kAim].empty()) {
    p.yaw = EvalScalar(s.keys[kAim], kAim, 0, t, false);
    p.pitch = std::clamp(EvalScalar(s.keys[kAim], kAim, 1, t, false), -89.0f, 89.0f);
    p.has[kAim] = true;
  }
  if (p.yaw > 180 || p.yaw < -180) p.yaw = float(std::remainder(double(p.yaw), 360.0));
  if (!s.keys[kRoll].empty()) {
    p.roll = EvalScalar(s.keys[kRoll], kRoll, 0, t, false);
    p.has[kRoll] = true;
  }
  const bool focus_follow = s.focus.on() && focus_target;
  if (!s.keys[kFocus].empty() || focus_follow) {
    const auto& k = s.keys[kFocus];
    if (focus_follow) {
      p.focus_cm = float(Dist3(p.pos, focus_target)) + (k.empty() ? 0.0f : EvalScalar(k, kFocus, 0, t, true));
    } else {
      p.focus_cm = EvalScalar(k, kFocus, 0, t, false);
    }
    p.focus_cm = std::max(p.focus_cm, 10.0f);
    p.aperture = k.empty() ? 1.0f : std::clamp(EvalScalar(k, kFocus, 1, t, false), 0.1f, 10.0f);
    p.has[kFocus] = true;
  }
  return p;
}

// ---- editing ----
inline float Unwrap(float v, float nbr) { return nbr + float(std::remainder(double(v - nbr), 360.0)); }

// Key values for `track` from the camera state `c` (the inverse of Evaluate for that track). `t` picks the
// neighbouring key that angles are unwrapped against.
inline void CaptureValues(const Shot& s, int track, double t, const Pose& c, const float* aim_target,
                          const float* move_target, const float* focus_target, float aspect, float out[3]) {
  const auto& k = s.keys[track];
  const Key* nbr = nullptr;
  for (const Key& key : k) {
    if (!nbr || std::abs(key.t - t) < std::abs(nbr->t - t)) nbr = &key;
  }
  out[0] = out[1] = out[2] = 0;
  switch (track) {
    case kMove:
      if (s.move.on() && move_target) {
        const double dx = c.pos[0] - move_target[0], dz = c.pos[2] - move_target[2];
        out[0] = float(std::sqrt(dx * dx + dz * dz));
        out[1] = float(std::atan2(dz, -dx) / kDeg);  // inverse of OrbitPosition
        out[2] = move_target[1] - c.pos[1];
        if (nbr) out[1] = Unwrap(out[1], nbr->v[1]);
      } else {
        std::memcpy(out, c.pos, sizeof(float) * 3);
      }
      break;
    case kAim:
      if (s.aim.on() && aim_target) {
        float yaw = c.yaw, pitch = c.pitch;
        LookAngles(c.pos, aim_target, &yaw, &pitch);
        const double tv = std::tan(c.fov * kDeg * 0.5), th = tv * aspect;
        out[0] = float(std::tan(std::remainder(double(yaw - c.yaw), 360.0) * kDeg) / th);
        out[1] = float(std::tan((pitch - c.pitch) * kDeg) / tv);
      } else {
        out[0] = nbr ? Unwrap(c.yaw, nbr->v[0]) : c.yaw;
        out[1] = c.pitch;
      }
      break;
    case kRoll: out[0] = c.roll; break;
    case kLens: out[0] = c.fov; break;
    case kFocus:
      out[0] = (s.focus.on() && focus_target) ? c.focus_cm - float(Dist3(c.pos, focus_target)) : c.focus_cm;
      out[1] = c.aperture;
      break;
  }
}

// Sets (or replaces, within half a frame at 60 fps) the key of `track` at time t. Returns its index.
inline size_t SetKey(Shot& s, int track, double t, const float v[3]) {
  auto& k = s.keys[track];
  for (size_t i = 0; i < k.size(); ++i) {
    if (std::abs(k[i].t - t) < 1.0 / 120) {
      std::memcpy(k[i].v, v, sizeof(float) * 3);
      s.Touch();
      return i;
    }
  }
  Key key;
  key.t = t;
  std::memcpy(key.v, v, sizeof(float) * 3);
  const auto it = std::upper_bound(k.begin(), k.end(), t, [](double x, const Key& a) { return x < a.t; });
  const size_t i = size_t(it - k.begin());
  k.insert(it, key);
  s.Touch();
  return i;
}

// Moves a key in time (keeps the order). Returns its new index.
inline size_t MoveKey(Shot& s, int track, size_t i, double t) {
  auto& k = s.keys[track];
  if (i >= k.size()) return i;
  Key key = k[i];
  k.erase(k.begin() + long(i));
  key.t = std::max(0.0, t);
  const auto it = std::upper_bound(k.begin(), k.end(), key.t, [](double x, const Key& a) { return x < a.t; });
  const size_t j = size_t(it - k.begin());
  k.insert(it, key);
  s.Touch();
  return j;
}

inline void DeleteKey(Shot& s, int track, size_t i) {
  auto& k = s.keys[track];
  if (i < k.size()) k.erase(k.begin() + long(i));
  s.Touch();
}

// The last key time over all tracks (0 with no keys).
inline double LastKeyTime(const Shot& s) {
  double t = 0;
  for (const auto& k : s.keys) {
    if (!k.empty()) t = std::max(t, k.back().t);
  }
  return t;
}

// ---- files: "# wwe2k14 cinecam shot v1", then lines (a '#' starts a comment):
//   length SECONDS | constant_speed 0|1 | follow SECONDS
//   target aim|move|focus none | slot N | pin X Y Z
//   key TRACK TIME smooth|linear|hold EASE V0 [V1 [V2]]      (EASE -1 = auto)
inline std::string TargetText(const Target& t) {
  char b[96];
  if (t.kind == TargetKind::kSlot) std::snprintf(b, sizeof(b), "slot %d", t.slot);
  else if (t.kind == TargetKind::kPin) std::snprintf(b, sizeof(b), "pin %.3f %.3f %.3f", t.pin[0], t.pin[1], t.pin[2]);
  else std::snprintf(b, sizeof(b), "none");
  return b;
}

inline bool SaveShot(const Shot& s, const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "w");
  if (!f) return false;
  std::fprintf(f, "# wwe2k14 cinecam shot v1\n");
  std::fprintf(f, "length %.4f\nconstant_speed %d\nfollow %.3f\n", s.length, s.constant_speed ? 1 : 0, s.follow);
  std::fprintf(f, "target aim %s\ntarget move %s\ntarget focus %s\n", TargetText(s.aim).c_str(),
               TargetText(s.move).c_str(), TargetText(s.focus).c_str());
  for (int tr = 0; tr < kTrackCount; ++tr) {
    for (const Key& k : s.keys[tr]) {
      std::fprintf(f, "key %s %.4f %s %.3f", kTrackNames[tr], k.t, kCurveNames[int(k.curve)], k.ease);
      for (int d = 0; d < kTrackDims[tr]; ++d) std::fprintf(f, " %.5f", k.v[d]);
      std::fprintf(f, "\n");
    }
  }
  const bool ok = std::ferror(f) == 0;
  std::fclose(f);
  return ok;
}

inline int TrackByName(const char* n) {
  for (int i = 0; i < kTrackCount; ++i) {
    if (std::strcmp(n, kTrackNames[i]) == 0) return i;
  }
  return -1;
}

// Reads a shot file; false (and `error`) when it is not one. Unknown lines are skipped (later versions may add some).
inline bool LoadShot(const std::string& path, Shot& out, std::string& error) {
  std::FILE* f = std::fopen(path.c_str(), "r");
  if (!f) {
    error = "cannot open " + path;
    return false;
  }
  Shot s;
  char line[512];
  bool header = false;
  int n = 0;
  while (std::fgets(line, sizeof(line), f)) {
    ++n;
    if (std::strncmp(line, "# wwe2k14 cinecam shot v", 24) == 0) {
      header = true;
      continue;
    }
    if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
    char w1[32] = {}, w2[32] = {}, w3[32] = {};
    if (std::sscanf(line, "%31s", w1) != 1) continue;
    if (std::strcmp(w1, "length") == 0) {
      std::sscanf(line, "%*s %lf", &s.length);
    } else if (std::strcmp(w1, "constant_speed") == 0) {
      int v = 0;
      std::sscanf(line, "%*s %d", &v);
      s.constant_speed = v != 0;
    } else if (std::strcmp(w1, "follow") == 0) {
      std::sscanf(line, "%*s %f", &s.follow);
    } else if (std::strcmp(w1, "target") == 0) {
      Target t;
      if (std::sscanf(line, "%*s %31s %31s", w2, w3) < 2) continue;
      if (std::strcmp(w3, "slot") == 0) {
        t.kind = TargetKind::kSlot;
        std::sscanf(line, "%*s %*s %*s %d", &t.slot);
        t.slot = std::clamp(t.slot, 0, 8);
      } else if (std::strcmp(w3, "pin") == 0) {
        t.kind = TargetKind::kPin;
        std::sscanf(line, "%*s %*s %*s %f %f %f", &t.pin[0], &t.pin[1], &t.pin[2]);
      }
      if (std::strcmp(w2, "aim") == 0) s.aim = t;
      else if (std::strcmp(w2, "move") == 0) s.move = t;
      else if (std::strcmp(w2, "focus") == 0) s.focus = t;
    } else if (std::strcmp(w1, "key") == 0) {
      Key k;
      char curve[16] = {};
      const int got = std::sscanf(line, "%*s %31s %lf %15s %f %f %f %f", w2, &k.t, curve, &k.ease, &k.v[0], &k.v[1],
                                  &k.v[2]);
      const int tr = TrackByName(w2);
      if (tr < 0 || got < 4 + kTrackDims[tr]) {
        error = "line " + std::to_string(n) + ": bad key";
        std::fclose(f);
        return false;
      }
      for (int c = 0; c < 3; ++c) {
        if (std::strcmp(curve, kCurveNames[c]) == 0) k.curve = Curve(c);
      }
      s.keys[tr].push_back(k);
    }
  }
  std::fclose(f);
  if (!header) {
    error = path + " is not a CineCam shot file";
    return false;
  }
  for (auto& k : s.keys) {
    std::stable_sort(k.begin(), k.end(), [](const Key& a, const Key& b) { return a.t < b.t; });
  }
  s.length = std::max(s.length, LastKeyTime(s));
  s.Touch();
  out = std::move(s);
  return true;
}

}  // namespace wwe2k14::cinecam::keys
