// Offline test of src\cinecam_keys.h (Session 35 step 1). Build and run: pwsh -File tools\cinecam\build_test.ps1
// Prints one line per check and exits 1 on any failure.
#include "../../src/cinecam_keys.h"

#include <cstdio>
#include <cstdlib>

using namespace wwe2k14::cinecam::keys;

static int g_fail = 0;
static void Check(bool ok, const char* what, double a = 0, double b = 0) {
  std::printf("%s  %s", ok ? "PASS" : "FAIL", what);
  if (a != 0 || b != 0) std::printf("  (%.6g, %.6g)", a, b);
  std::printf("\n");
  if (!ok) ++g_fail;
}

static Key K(double t, float a, float b = 0, float c = 0, Curve curve = Curve::kSmooth, float ease = -1) {
  Key k;
  k.t = t;
  k.v[0] = a, k.v[1] = b, k.v[2] = c;
  k.curve = curve;
  k.ease = ease;
  return k;
}

static Pose Eval(const Shot& s, double t, const float* aim = nullptr, const float* mv = nullptr,
                 const float* fo = nullptr) {
  Pose base;
  return Evaluate(s, t, base, aim, mv, fo, 16.0f / 9.0f);
}

int main() {
  // 1. Move: keys are hit at their times; the path is continuous and starts / ends at rest.
  {
    Shot s;
    s.keys[kMove] = {K(0, 0, 0, 0), K(2, 400, -100, 0), K(5, 400, -100, 600), K(8, -300, -300, 600)};
    s.Touch();
    double worst_key = 0;
    for (const Key& k : s.keys[kMove]) {
      const Pose p = Eval(s, k.t);
      worst_key = std::max(worst_key, Dist3(p.pos, k.v));
    }
    Check(worst_key < 0.5, "move: every key reached at its time (max miss, cm)", worst_key);
    double max_step = 0, max_jerk = 0, prev_step = -1;
    Pose prev = Eval(s, 0);
    for (int f = 1; f <= 480; ++f) {
      const Pose p = Eval(s, f / 60.0);
      const double step = Dist3(p.pos, prev.pos);
      max_step = std::max(max_step, step);
      if (prev_step >= 0) max_jerk = std::max(max_jerk, std::abs(step - prev_step));
      prev_step = step;
      prev = p;
    }
    Check(max_step < 12, "move: no jumps (largest step per frame, cm)", max_step);
    Check(max_jerk < 0.6, "move: speed changes smoothly (largest change of step per frame, cm)", max_jerk);
    const double start = Dist3(Eval(s, 1 / 60.0).pos, Eval(s, 0).pos), end = Dist3(Eval(s, 8).pos, Eval(s, 8 - 1 / 60.0).pos);
    Check(start < 0.5 && end < 1.5, "move: eases from rest and to rest (first / last step, cm)", start, end);
  }
  // 2. Constant speed: even steps between the ramps.
  {
    Shot s;
    s.constant_speed = true;
    s.keys[kMove] = {K(0, 0, 0, 0), K(1, 100, 0, 0), K(6, 1000, 0, 400), K(8, 1200, -200, 400)};
    s.Touch();
    double lo = 1e9, hi = 0;
    for (int f = 150; f <= 330; ++f) {  // inside the ramps (0.25 * 8 s = 2 s at each end)
      const double step = Dist3(Eval(s, f / 60.0).pos, Eval(s, (f - 1) / 60.0).pos);
      lo = std::min(lo, step), hi = std::max(hi, step);
    }
    Check(hi / lo < 1.02, "constant speed: even steps in the middle (min / max step, cm)", lo, hi);
    Check(Dist3(Eval(s, 8).pos, s.keys[kMove].back().v) < 0.5, "constant speed: ends on the last key");
  }
  // 3. Zoom: monotone, no overshoot, keys hit, even in log(tan) space.
  {
    Shot s;
    s.keys[kLens] = {K(0, 60), K(3, 60), K(6, 15)};
    s.Touch();
    bool monotone = true, inside = true;
    float prev = 60;
    for (int f = 0; f <= 400; ++f) {
      const float fov = Eval(s, f / 60.0).fov;
      if (fov > prev + 1e-4f) monotone = false;
      if (fov > 60.0001f || fov < 14.9999f) inside = false;
      prev = fov;
    }
    Check(monotone && inside, "lens: no overshoot or bounce through a hold and a zoom");
    Check(std::abs(Eval(s, 6).fov - 15) < 1e-3 && std::abs(Eval(s, 3).fov - 60) < 1e-3, "lens: keys hit");
    // Half way in time with ease-free linear curve = half way in log(tan): the geometric mean of the tans.
    Shot l;
    l.keys[kLens] = {K(0, 60, 0, 0, Curve::kLinear), K(2, 15)};
    l.Touch();
    const double expect = 2 * std::atan(std::sqrt(std::tan(30 * kDeg) * std::tan(7.5 * kDeg))) / kDeg;
    Check(std::abs(Eval(l, 1).fov - expect) < 1e-3, "lens: linear zoom is even in log(tan(fov/2))", Eval(l, 1).fov, expect);
  }
  // 4. Focus pull in dioptres: half way (linear) = the harmonic mean.
  {
    Shot s;
    s.keys[kFocus] = {K(0, 1400, 1, 0, Curve::kLinear), K(4, 450, 2)};
    s.Touch();
    const double expect = 1.0 / ((1.0 / 1400 + 1.0 / 450) / 2);
    Check(std::abs(Eval(s, 2).focus_cm - expect) < 0.5, "focus: linear pull is even in dioptres", Eval(s, 2).focus_cm, expect);
    Check(std::abs(Eval(s, 2).aperture - std::sqrt(2.0)) < 1e-3, "focus: aperture even in log", Eval(s, 2).aperture);
  }
  // 5. Hold: stays, then cuts.
  {
    Shot s;
    s.keys[kMove] = {K(0, 0, 0, 0, Curve::kHold), K(2, 500, 0, 0)};
    s.keys[kRoll] = {K(0, 0, 0, 0, Curve::kHold), K(2, 10)};
    s.Touch();
    Check(Dist3(Eval(s, 1.99).pos, s.keys[kMove][0].v) < 1e-3 && Dist3(Eval(s, 2).pos, s.keys[kMove][1].v) < 1e-3,
          "hold: position stays, then cuts at the next key");
    Check(Eval(s, 1.99).roll == 0 && Eval(s, 2).roll == 10, "hold: roll stays, then cuts");
  }
  // 6. Orbit: constant radius around a moving target, angle 0 -> 270 the long way, look-at centres it.
  {
    Shot s;
    s.move.kind = TargetKind::kSlot;
    s.aim.kind = TargetKind::kSlot;
    s.keys[kMove] = {K(0, 500, 0, 50), K(6, 500, 270, 50)};
    s.Touch();
    double rmin = 1e9, rmax = 0, worst_aim = 0;
    for (int f = 0; f <= 360; ++f) {
      const float tgt[3] = {float(f), -200, float(-f) * 0.5f};
      const Pose p = Eval(s, f / 60.0, tgt, tgt);
      const double dx = p.pos[0] - tgt[0], dz = p.pos[2] - tgt[2];
      const double r = std::sqrt(dx * dx + dz * dz);
      rmin = std::min(rmin, r), rmax = std::max(rmax, r);
      float yaw, pitch;
      LookAngles(p.pos, tgt, &yaw, &pitch);
      worst_aim = std::max(worst_aim, std::abs(std::remainder(double(yaw - p.yaw), 360.0)) + std::abs(pitch - p.pitch));
    }
    Check(rmax - rmin < 0.01, "orbit: radius constant around a moving target", rmin, rmax);
    Check(worst_aim < 1e-3, "orbit: look-at keeps the target centred (degrees)", worst_aim);
    const float tgt[3] = {0, -200, 0};
    const Pose half = Eval(s, 3, tgt, tgt);
    float yaw, pitch;
    LookAngles(half.pos, tgt, &yaw, &pitch);
    Check(std::abs(std::remainder(double(yaw - 135), 360.0)) < 0.01, "orbit: heading = azimuth (135 at half way)", yaw);
    Check(std::abs(half.pos[1] - (-250)) < 1e-3, "orbit: height above the target", half.pos[1]);
  }
  // 7. Capture is the inverse of Evaluate (orbit, framing, focus follow).
  {
    Shot s;
    s.move.kind = s.aim.kind = s.focus.kind = TargetKind::kPin;
    s.keys[kMove] = {K(0, 700, -40, 120)};
    s.keys[kAim] = {K(0, 0.333f, -0.2f)};
    s.keys[kFocus] = {K(0, 35, 1.5f)};
    s.keys[kLens] = {K(0, 40)};
    s.Touch();
    const float tgt[3] = {120, -205, -60};
    const Pose p = Eval(s, 0, tgt, tgt, tgt);
    float v[3];
    CaptureValues(s, kMove, 0, p, tgt, tgt, tgt, 16.0f / 9.0f, v);
    Check(std::abs(v[0] - 700) < 0.01 && std::abs(v[1] + 40) < 0.01 && std::abs(v[2] - 120) < 0.01,
          "capture: orbit values round trip", v[0], v[1]);
    CaptureValues(s, kAim, 0, p, tgt, tgt, tgt, 16.0f / 9.0f, v);
    Check(std::abs(v[0] - 0.333f) < 1e-3 && std::abs(v[1] + 0.2f) < 1e-3, "capture: framing round trip", v[0], v[1]);
    CaptureValues(s, kFocus, 0, p, tgt, tgt, tgt, 16.0f / 9.0f, v);
    Check(std::abs(v[0] - 35) < 0.01 && std::abs(v[1] - 1.5f) < 1e-4, "capture: focus follow offset round trip", v[0]);
    Check(std::abs(p.focus_cm - (700 * 0 + Dist3(p.pos, tgt) + 35)) < 0.01, "focus follow: distance + offset");
  }
  // 8. Untouched tracks keep the base pose; yaw unwrap on capture.
  {
    Shot s;
    s.keys[kLens] = {K(0, 50), K(2, 20)};
    s.Touch();
    Pose base;
    base.pos[0] = 11, base.yaw = 33;
    const Pose p = Evaluate(s, 1, base, nullptr, nullptr, nullptr, 1.7f);
    Check(p.pos[0] == 11 && p.yaw == 33 && p.has[kLens] && !p.has[kMove], "only the keyed tracks change the camera");
    Shot a;
    a.keys[kAim] = {K(0, 170, 0)};
    Pose c;
    c.yaw = -175;
    float v[3];
    CaptureValues(a, kAim, 1, c, nullptr, nullptr, nullptr, 1.7f, v);
    Check(std::abs(v[0] - 185) < 1e-3, "capture: yaw unwrapped next to its neighbour (170 -> 185, not -175)", v[0]);
  }
  // 9. File round trip.
  {
    Shot s;
    s.length = 9.5;
    s.constant_speed = true;
    s.follow = 0.4f;
    s.aim.kind = TargetKind::kSlot;
    s.aim.slot = 1;
    s.focus.kind = TargetKind::kPin;
    s.focus.pin[0] = 1, s.focus.pin[1] = -2, s.focus.pin[2] = 3;
    s.keys[kMove] = {K(0, 1, 2, 3, Curve::kLinear, 0.5f), K(4, 5, 6, 7)};
    s.keys[kAim] = {K(1, 0.1f, 0.2f)};
    s.keys[kLens] = {K(2, 33, 0, 0, Curve::kHold)};
    s.keys[kFocus] = {K(3, 900, 1.25f)};
    s.Touch();
    const std::string path = "keys_test_roundtrip.shot";
    Check(SaveShot(s, path), "file: saved");
    Shot r;
    std::string err;
    const bool ok = LoadShot(path, r, err);
    Check(ok, ok ? "file: loaded" : err.c_str());
    bool same = r.length == 9.5 && r.constant_speed && std::abs(r.follow - 0.4f) < 1e-6 &&
                r.aim.kind == TargetKind::kSlot && r.aim.slot == 1 && r.focus.kind == TargetKind::kPin &&
                r.focus.pin[2] == 3 && r.move.kind == TargetKind::kNone;
    for (int tr = 0; tr < kTrackCount; ++tr) {
      same = same && r.keys[tr].size() == s.keys[tr].size();
      for (size_t i = 0; same && i < s.keys[tr].size(); ++i) {
        const Key &a = s.keys[tr][i], &b = r.keys[tr][i];
        same = a.t == b.t && a.curve == b.curve && a.ease == b.ease;
        for (int d = 0; d < kTrackDims[tr]; ++d) same = same && std::abs(a.v[d] - b.v[d]) < 1e-4f;
      }
    }
    Check(same, "file: every field round trips");
    std::remove(path.c_str());
  }
  // 10. Editing helpers.
  {
    Shot s;
    const float a[3] = {1, 0, 0}, b[3] = {2, 0, 0}, c[3] = {3, 0, 0};
    SetKey(s, kMove, 2, b);
    SetKey(s, kMove, 0, a);
    SetKey(s, kMove, 2.004, c);  // within half a frame: replaces
    Check(s.keys[kMove].size() == 2 && s.keys[kMove][0].v[0] == 1 && s.keys[kMove][1].v[0] == 3,
          "edit: keys sorted, a key within half a frame is replaced");
    const size_t j = MoveKey(s, kMove, 0, 5);
    Check(j == 1 && s.keys[kMove][0].v[0] == 3 && s.keys[kMove][1].t == 5, "edit: moving a key keeps the order");
    DeleteKey(s, kMove, 0);
    Check(s.keys[kMove].size() == 1 && LastKeyTime(s) == 5, "edit: delete");
  }
  std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
  return g_fail ? 1 : 0;
}
