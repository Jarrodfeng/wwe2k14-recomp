// Lane 176 (S101): the unit test of src/crowd_wide_core.h on a MODELLED stands crowd. No game, no SDK, no capture: the game's two screen tests
// (sub_8250E110 per member, sub_8250DC10 per section) written out from their listings (docs/renderer/CROWD_WIDE.md section 2), run on a fake
// guest stack frame (big-endian floats at the offsets the hooks edit) with the SHIPPED edits (ApplyMember / ApplySection) in between.
//   clang++ -std=c++20 tools/crowd/crowd_wide_test.cpp -o out/tools/crowd_wide_test.exe     (tools/crowd/build_crowd_wide_test.ps1)
//   crowd_wide_test.exe [--plant NAME]      exit 0 = all checks pass, 1 = a check failed (a plant must exit 1)
// What it proves:
//   1. 16:9 (scale 1, 1): the frame and both radius registers are byte-equal after the hooks' edit (they return before any write).
//   2. 21:9, 20:9 (the OPPO), 32:9 and a taller screen: the kept set of the game's own test with the edit EQUALS the kept set of the same
//      unmodified test on a camera whose projection is k times wider (the oracle), member for member; it contains the 16:9 set; and members
//      exist in the side strips of the DISPLAYED picture (|x| > 1/k of the half width... i.e. game NDC |x| in (1, k]) that the 16:9 test kills.
//   3. Costs: kept members (= crowd draws, before the density / card split) per aspect against 16:9.
// The numbers are a model (a ring of stands around a ring, 20 x 768 members, one camera): they show the cut and its size, not a game census.
// Planted faults (each must fail): old_16_9, no_x, no_radius, wrong_axis, row_not_column, section_no_radius, section_wrong_col.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/crowd_wide_core.h"
#include "../../src/replay_carried.h"

using namespace wwe2k14::crowd_wide;

static std::string g_plant;
static int g_fail = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}

struct Mat {
  float m[4][4] = {};
};

// Row-vector convention (v * M), like the game's: prod = view * projection.
static Mat Mul(const Mat& a, const Mat& b) {
  Mat r;
  for (int i = 0; i < 4; ++i)
    for (int c = 0; c < 4; ++c) {
      float t = 0;
      for (int j = 0; j < 4; ++j) t += a.m[i][j] * b.m[j][c];
      r.m[i][c] = t;
    }
  return r;
}
static Mat Projection(float fov_y_deg, float aspect, float zn, float zf) {
  Mat p;
  const float f = 1.0f / std::tan(fov_y_deg * 3.14159265f / 360.0f);
  p.m[0][0] = f / aspect;
  p.m[1][1] = f;
  p.m[2][2] = zf / (zf - zn);
  p.m[2][3] = 1.0f;
  p.m[3][2] = -zn * zf / (zf - zn);
  return p;
}
static Mat LookAt(const float c[3], const float t[3]) {
  float f[3] = {t[0] - c[0], t[1] - c[1], t[2] - c[2]};
  const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
  for (float& v : f) v /= fl;
  const float up[3] = {0, 1, 0};
  float r[3] = {up[1] * f[2] - up[2] * f[1], up[2] * f[0] - up[0] * f[2], up[0] * f[1] - up[1] * f[0]};
  const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
  for (float& v : r) v /= rl;
  const float u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
  Mat v;
  for (int i = 0; i < 3; ++i) {
    v.m[i][0] = r[i];
    v.m[i][1] = u[i];
    v.m[i][2] = f[i];
  }
  v.m[3][0] = -(r[0] * c[0] + r[1] * c[1] + r[2] * c[2]);
  v.m[3][1] = -(u[0] * c[0] + u[1] * c[1] + u[2] * c[2]);
  v.m[3][2] = -(f[0] * c[0] + f[1] * c[1] + f[2] * c[2]);
  v.m[3][3] = 1;
  return v;
}

// The fake guest stack frame: 512 bytes; the member test's product at +112, the section test's raw matrix at +96 and product at +160.
struct Frame {
  uint8_t b[512] = {};
  void PutMat(uint32_t off, const Mat& m) {
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c) StoreBeF(b + off + 16 * r + 4 * c, m.m[r][c]);
  }
  Mat GetMat(uint32_t off) const {
    Mat m;
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c) m.m[r][c] = LoadBeF(b + off + 16 * r + 4 * c);
    return m;
  }
};

// sub_8250E110, one member (listing 0x8250E2A0-0x8250E34C; nf/frustum_cull.h Run): the point (x, y - K, z, 1) through prod; w < kW culls; the
// radius rad / w * (m00 K, m11 K) is tested against the clip range [-1, 1].
static constexpr float K = 120.0f, kW = 1.0f;
static bool MemberVisible(const Mat& prod, float sx, float sy, const float p[3], float rad, float* ndc_x = nullptr, float bound = 1.0f, float wfloor = kW) {
  const float x = p[0], y = p[1] - K, z = p[2];
  const float o0 = x * prod.m[0][0] + y * prod.m[1][0] + z * prod.m[2][0] + prod.m[3][0];
  const float o1 = x * prod.m[0][1] + y * prod.m[1][1] + z * prod.m[2][1] + prod.m[3][1];
  const float cw = x * prod.m[0][3] + y * prod.m[1][3] + z * prod.m[2][3] + prod.m[3][3];
  if (cw < wfloor) return false;
  const float inv = 1.0f / cw, rs = rad * inv;
  const float px = o0 * inv, py = o1 * inv, ex = rs * sx, ey = rs * sy;
  if (ndc_x) *ndc_x = px;
  if (px - ex > bound) return false;
  if (px + ex < -bound) return false;
  if (py - ey < -bound) return false;
  if (ey + py > bound) return false;
  return true;
}

// sub_8250DC10, one section side (listing 0x8250DCE4-0x8250DD8C): x only. The point is the section centre, mirrored in x for side 1; the radius is
// radius_field * c * rawM00.
static bool SectionVisible(const Mat& prod, float raw_m00, const float c[3], float radius_field, int side) {
  const float x = side ? -c[0] : c[0], y = c[1], z = c[2];
  const float o0 = x * prod.m[0][0] + y * prod.m[1][0] + z * prod.m[2][0] + prod.m[3][0];
  const float cw = x * prod.m[0][3] + y * prod.m[1][3] + z * prod.m[2][3] + prod.m[3][3];
  const float rt = radius_field * 0.01f;
  if (cw < 1.0f - rt) return false;
  const float inv = 1.0f / cw, px = o0 * inv, rx = raw_m00 * rt;
  if (px - rx > 1.0f) return false;
  if (px + rx < -1.0f) return false;
  return true;
}

struct Rng {
  uint32_t s = 12345;
  float F() {
    s = s * 1664525u + 1013904223u;
    return float(s >> 8) / float(1 << 24);
  }
};

struct Member {
  float p[3];
  float rad;
};

// The stands: 20 types x 768 members on a ring bowl around the ring (units are centimetres; the radius field is x K = 60 cm).
static std::vector<Member> MakeStands() {
  std::vector<Member> v;
  Rng r;
  for (int i = 0; i < 20 * 768; ++i) {
    const float a = r.F() * 6.2831853f, d = 2600.0f + r.F() * 3200.0f;  // 26..58 m from the centre
    v.push_back({{std::cos(a) * d, 250.0f + (d - 2600.0f) * 0.45f, std::sin(a) * d}, 0.5f});
  }
  return v;
}

// One camera: 40 deg vertical fov at the game's 16:9, 7 m high, 20 m behind the ring's centre looking at it.
static void Camera(float aspect, Mat* proj, Mat* view) {
  const float c[3] = {0, 700, -2000}, t[3] = {0, 150, 0};
  *proj = Projection(40.0f, aspect, 10.0f, 20000.0f);
  *view = LookAt(c, t);
}

static void ApplyMemberPlant(uint8_t* sp, const Scale& s, float& f28, float& f29) {
  if (g_plant == "old_16_9") return;
  if (g_plant == "no_x") {
    Scale t = s;
    t.x = 1.0f;
    ApplyMember(sp, t, f28, f29);
    return;
  }
  if (g_plant == "no_radius") {
    float a = f28, b = f29;
    ApplyMember(sp, s, a, b);
    return;
  }
  if (g_plant == "wrong_axis") {
    if (s.x != 1.0f) ScaleColumn(sp + kMemberProduct, 1, s.x);
    if (s.x != 1.0f) f29 = ScaleRadius(f29, s.x);
    return;
  }
  if (g_plant == "row_not_column") {
    for (int c = 0; c < 4; ++c) {
      uint8_t* p = sp + kMemberProduct + 4 * c;
      StoreBeF(p, LoadBeF(p) * s.x);
    }
    if (s.x != 1.0f) f29 = ScaleRadius(f29, s.x);
    return;
  }
  ApplyMember(sp, s, f28, f29);
}
static void ApplySectionPlant(uint8_t* sp, const Scale& s) {
  if (g_plant == "old_16_9") return;
  if (g_plant == "section_no_radius") {
    ScaleColumn(sp + kSectionProduct, 0, s.x);
    return;
  }
  if (g_plant == "section_wrong_col") {
    ScaleColumn(sp + kSectionProduct, 1, s.x);
    StoreBeF(sp + kSectionRaw, ScaleRadius(LoadBeF(sp + kSectionRaw), s.x));
    return;
  }
  ApplySection(sp, s);
}

static void ApplyFloorPlant(uint8_t* sp, const Scale& s) {
  if (g_plant == "old_16_9") return;
  if (g_plant == "floor_wrong_col") {
    ScaleColumn(sp + kFloorProjection, 1, s.x);
    return;
  }
  ApplyFloor(sp, s);
}

struct Census {
  size_t kept = 0, side = 0;  // side: kept with game NDC |x| in (1, 1/scale.x]  (the displayed picture's outer strips)
  std::vector<uint8_t> flag;
};

// The hooked member test: the product and radii as the game builds them, then the edit, then the test.
static Census RunMembers(const std::vector<Member>& ms, float aspect_game, const Scale& s, double side_limit) {
  Mat proj, view;
  Camera(aspect_game, &proj, &view);
  Frame fr;
  fr.PutMat(kMemberProduct, Mul(view, proj));
  float f29 = proj.m[0][0] * K, f28 = proj.m[1][1] * K;
  ApplyMemberPlant(fr.b, s, f28, f29);
  const Mat prod = fr.GetMat(kMemberProduct);
  Census c;
  for (const Member& m : ms) {
    float nx = 0;
    const bool vis = MemberVisible(prod, f29, f28, m.p, m.rad, &nx);
    c.flag.push_back(vis);
    if (vis) {
      ++c.kept;
      if (std::fabs(nx) > 1.0 && std::fabs(nx) <= side_limit) ++c.side;
    }
  }
  return c;
}
// The oracle: the game's UNMODIFIED test on a camera whose projection is k / t times wider / taller.
static Census RunOracle(const std::vector<Member>& ms, float aspect_game, double k, double t) {
  Mat proj, view;
  Camera(aspect_game, &proj, &view);
  proj.m[0][0] = float(proj.m[0][0] / k);
  proj.m[1][1] = float(proj.m[1][1] / t);
  const Mat prod = Mul(view, proj);
  Census c;
  for (const Member& m : ms) {
    const bool vis = MemberVisible(prod, proj.m[0][0] * K, proj.m[1][1] * K, m.p, m.rad);
    c.flag.push_back(vis);
    c.kept += vis;
  }
  return c;
}

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string(argv[i]) == "--plant") g_plant = argv[i + 1];
  const float game_aspect = 16.0f / 9.0f;
  const std::vector<Member> stands = MakeStands();

  // 1. 16:9: nothing is written.
  {
    const Scale s = FromFactors(1.0, 1.0);
    Check(!s.On(), "16:9 factors (1, 1) -> the edit is off");
    Mat proj, view;
    Camera(game_aspect, &proj, &view);
    Frame a, b;
    a.PutMat(kMemberProduct, Mul(view, proj));
    a.PutMat(kSectionRaw, proj);
    a.PutMat(kSectionProduct, Mul(view, proj));
    std::memcpy(b.b, a.b, sizeof(a.b));
    float f28 = proj.m[1][1] * K, f29 = proj.m[0][0] * K;
    const float e28 = f28, e29 = f29;
    ApplyMemberPlant(a.b, s, f28, f29);
    ApplySectionPlant(a.b, s);
    Check(std::memcmp(a.b, b.b, sizeof(a.b)) == 0 && f28 == e28 && f29 == e29, "16:9: the stack frame and both radius registers are byte-equal after the edit");
    const Census base = RunMembers(stands, game_aspect, s, 1e9), orc = RunOracle(stands, game_aspect, 1.0, 1.0);
    Check(base.flag == orc.flag, "16:9: the hooked test keeps exactly the game's own members");
  }

  // 2. wider / taller screens.
  const Census base = RunMembers(stands, game_aspect, Scale{}, 1e9);
  std::printf("16:9 baseline: %zu of %zu members kept\n", base.kept, stands.size());
  struct Case {
    const char* name;
    double wide, tall;
  };
  const Case cases[] = {{"20:9 (the OPPO, k 1.250)", 1.25, 1.0}, {"21:9 (k 1.3125)", 21.0 / 9.0 / (16.0 / 9.0), 1.0},
                        {"32:9 (k 2.000)", 2.0, 1.0}, {"taller 4:3 (t 1.333)", 1.0, 4.0 / 3.0}};
  for (const Case& cs : cases) {
    const Scale s = FromFactors(cs.wide, cs.tall);
    const Census w = RunMembers(stands, game_aspect, s, cs.wide), orc = RunOracle(stands, game_aspect, cs.wide, cs.tall);
    size_t diff = 0, lost = 0;
    for (size_t i = 0; i < w.flag.size(); ++i) {
      diff += w.flag[i] != orc.flag[i];
      lost += base.flag[i] && !w.flag[i];
    }
    std::printf("%-26s kept %6zu (x%.3f of 16:9)  in the side strips %5zu (16:9: 0)  differs from the oracle %zu  lost vs 16:9 %zu\n", cs.name, w.kept,
                double(w.kept) / double(base.kept), w.side, diff, lost);
    Check(diff == 0, (std::string(cs.name) + ": the hooked test keeps exactly the members the unmodified test keeps on a k-times wider camera").c_str());
    Check(lost == 0, (std::string(cs.name) + ": every member the 16:9 test kept is still kept").c_str());
    if (cs.wide > 1.0) {
      Check(w.side > 0, (std::string(cs.name) + ": members exist in the side strips (the 16:9 test leaves them empty)").c_str());
      Check(w.kept > base.kept, (std::string(cs.name) + ": more members are kept than at 16:9").c_str());
    }
  }

  // 3. the section test (sub_8250DC10): 128 sections x 2 sides on the same ring; the oracle is the same test on a wider camera.
  {
    Rng r;
    struct Sec {
      float c[3], radius_field;
    };
    std::vector<Sec> secs;
    for (int i = 0; i < 128; ++i) {
      const float a = r.F() * 3.14159265f, d = 2600.0f + r.F() * 3200.0f;
      secs.push_back({{std::cos(a) * d, 250.0f + (d - 2600.0f) * 0.45f, std::sin(a) * d}, 150.0f + r.F() * 250.0f});
    }
    for (const double k : {1.25, 21.0 / 9.0 / (16.0 / 9.0), 2.0}) {
      const Scale s = FromFactors(k, 1.0);
      Mat proj, view;
      Camera(game_aspect, &proj, &view);
      Frame fr;
      fr.PutMat(kSectionRaw, proj);
      fr.PutMat(kSectionProduct, Mul(view, proj));
      ApplySectionPlant(fr.b, s);
      const Mat prod = fr.GetMat(kSectionProduct);
      const float raw = LoadBeF(fr.b + kSectionRaw);
      Mat po = proj;
      po.m[0][0] = float(po.m[0][0] / k);
      const Mat prodo = Mul(view, po);
      size_t diff = 0, kept = 0, kept16 = 0;
      for (const Sec& sc : secs)
        for (int side = 0; side < 2; ++side) {
          const bool a = SectionVisible(prod, raw, sc.c, sc.radius_field, side);
          const bool o = SectionVisible(prodo, po.m[0][0], sc.c, sc.radius_field, side);
          const bool b = SectionVisible(Mul(view, proj), proj.m[0][0], sc.c, sc.radius_field, side);
          diff += a != o;
          kept += a;
          kept16 += b;
        }
      std::printf("section test k %.4f: sections kept %zu (16:9: %zu), differs from the oracle %zu\n", k, kept, kept16, diff);
      Check(diff == 0, "section test: the hooked test equals the unmodified test on a k-times wider camera");
    }
  }

  // 4. the seated floor people (sub_8250E7E0): the projection copy at sp + 96 is edited BEFORE the in-place multiply and BEFORE the radius scales are loaded;
  // a box of +-0.9 and a w floor of 80; the oracle is the same test on a k-times wider camera.
  {
    const float bound = 0.9f, wfloor = 80.0f;
    size_t base_kept = 0;
    {
      Mat proj, view;
      Camera(game_aspect, &proj, &view);
      const Mat prod = Mul(view, proj);
      for (const Member& m : stands) base_kept += MemberVisible(prod, proj.m[0][0] * K, proj.m[1][1] * K, m.p, 0.5f, nullptr, bound, wfloor);
    }
    {
      Mat proj, view;
      Camera(game_aspect, &proj, &view);
      Frame a, b;
      a.PutMat(kFloorProjection, proj);
      std::memcpy(b.b, a.b, sizeof(a.b));
      ApplyFloorPlant(a.b, Scale{});
      Check(std::memcmp(a.b, b.b, sizeof(a.b)) == 0, "floor: 16:9 leaves the stack frame byte-equal");
    }
    for (const Case& cs : cases) {
      const Scale s = FromFactors(cs.wide, cs.tall);
      Mat proj, view;
      Camera(game_aspect, &proj, &view);
      Frame fr;
      fr.PutMat(kFloorProjection, proj);
      ApplyFloorPlant(fr.b, s);
      const Mat pp = fr.GetMat(kFloorProjection);  // what the game then reads for f29 / f28 and multiplies by the view
      const Mat prod = Mul(view, pp);
      Mat po = proj;
      po.m[0][0] = float(po.m[0][0] / cs.wide);
      po.m[1][1] = float(po.m[1][1] / cs.tall);
      const Mat prodo = Mul(view, po);
      size_t diff = 0, kept = 0, side = 0;
      for (const Member& m : stands) {
        float nx = 0;
        const bool a = MemberVisible(prod, pp.m[0][0] * K, pp.m[1][1] * K, m.p, 0.5f, &nx, bound, wfloor);
        const bool o = MemberVisible(prodo, po.m[0][0] * K, po.m[1][1] * K, m.p, 0.5f, nullptr, bound, wfloor);
        diff += a != o;
        kept += a;
        side += a && std::fabs(nx) > bound;
      }
      std::printf("floor test %-24s kept %5zu (16:9: %zu) beyond the 16:9 box %zu, differs from the oracle %zu\n", cs.name, kept, base_kept, side, diff);
      Check(diff == 0, (std::string("floor test ") + cs.name + ": equals the unmodified test on a k-times wider camera").c_str());
      if (cs.wide > 1.0) Check(side > 0 && kept > base_kept, (std::string("floor test ") + cs.name + ": seats beyond the 16:9 box are kept").c_str());
    }
  }

  // 5. the replay header (src/replay_carried.h + crowd_wide_core.h RecordValue, what src/replay.h writes and reads): the RESOLVED switch is carried, an old recording plays as off.
  {
    using namespace wwe2k14::replay;
    auto lineFor = [&](const std::string& mode, double w, double t) {
      std::string value = RecordValue(mode, w, t);
      if (g_plant == "record_raw") value = mode;  // the unresolved cvar text is written
      return HeaderLine(kCrowdFillWide, value, false);
    };
    auto parse = [&](const std::string& header) {
      std::vector<std::pair<std::string, std::string>> flags;
      size_t at = 0;
      while (at < header.size()) {
        const size_t e = header.find('\n', at);
        std::pair<std::string, std::string> kv;
        if (ParseFlagLine(header.substr(at, e - at), kv)) flags.push_back(kv);
        at = e == std::string::npos ? header.size() : e + 1;
      }
      return flags;
    };
    auto play = [&](std::vector<std::pair<std::string, std::string>> flags) {  // what Replay::Apply does with the file's flags
      if (g_plant != "old_not_off") AddOldRecordingDefaults(flags);
      std::string v = "auto";  // the player's cvar at its default
      for (auto& kv : flags)
        if (kv.first == kCrowdFillWide) v = kv.second;
      return v;
    };
    // a recording made at auto on 21:9 holds the factor
    const std::string h219 = "# wwe2k14 replay v2\n" + lineFor("auto", 21.0 / 9.0 / (16.0 / 9.0), 1.0);
    const auto f219 = parse(h219);
    Check(f219.size() == 1 && f219[0].first == kCrowdFillWide && f219[0].second.rfind("k=1.3125", 0) == 0, ("21:9 auto: the header line holds the factor (" + h219.substr(h219.find("flag")) + ")").c_str());
    // played back on a 16:9 display (gpu factors 1) it widens the same
    const Scale played = FromSwitch(play(f219), 1.0, 1.0);
    Check(std::fabs(double(played.x) - 1.0 / (21.0 / 9.0 / (16.0 / 9.0))) < 1e-6 && played.y == 1.0f, "21:9 recording on a 16:9 display plays back widened (the same Scale)");
    // 16:9 auto: off is written
    const auto f169 = parse("# wwe2k14 replay v2\n" + lineFor("auto", 1.0, 1.0));
    Check(f169.size() == 1 && f169[0].second == "off", "16:9 auto: the header line says off");
    // 16:9 recording played on a 21:9 display: off wins over the display
    const Scale on219 = FromSwitch(play(f169), 21.0 / 9.0 / (16.0 / 9.0), 1.0);
    Check(!on219.On(), "a 16:9 recording on a 21:9 display plays back with the 16:9 test");
    // an old recording (no line) plays as off whatever the display
    const auto old = parse("# wwe2k14 replay v2\nname t\nseed 5\nflag det_frames true\n");
    Check(play(old) == "off" && !FromSwitch(play(old), 21.0 / 9.0 / (16.0 / 9.0), 1.0).On(), "an old recording (no crowd line) plays as off on a 21:9 display");
    // taller screens, and an explicit k=,t= text round-trips through RecordValue
    Check(RecordValue(RecordValue("auto", 1.0, 4.0 / 3.0), 1.0, 1.0) == RecordValue("auto", 1.0, 4.0 / 3.0), "a recorded value re-resolves to itself (taller screen)");
    Check(RecordValue("off", 2.0, 1.0) == "off" && !FromSwitch("k=garbage", 2.0, 1.0).On(), "off stays off; an unreadable value is the game's own test");
  }

  // 6. (lane 203) the native copy of sub_8250E110 (nf/frustum_cull.h, verify's independent arm) against the hooked recompiled path, same inputs: the product
  // matrix (all 16 floats) and both radius scales must be bit-equal, at 16:9 and the wide cases. Plant native_no_scale: the native side drops its scale.
  {
    struct Aspect {
      const char* name;
      double wide, tall;
    };
    const Aspect aspects[] = {{"16:9", 1.0, 1.0}, {"21:9", 21.0 / 9.0 / (16.0 / 9.0), 1.0}, {"32:9", 2.0, 1.0}, {"taller 4:3", 1.0, 4.0 / 3.0}};
    Mat proj, view;
    Camera(game_aspect, &proj, &view);
    const Mat prod0 = Mul(view, proj);
    for (const Aspect& as : aspects) {
      const Scale s = FromFactors(as.wide, as.tall);
      // recompiled path: the stack product and the f registers as the game built them, then the hook's edit
      Frame fr;
      fr.PutMat(kMemberProduct, prod0);
      float f29 = proj.m[0][0] * K, f28 = proj.m[1][1] * K;
      ApplyMember(fr.b, s, f28, f29);
      // native path: host arrays and doubles as nf/frustum_cull.h builds them, then the shared helpers
      float np[4][4];
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) np[r][c] = prod0.m[r][c];
      double sx = double(float(double(proj.m[0][0]) * double(K))), sy = double(float(double(proj.m[1][1]) * double(K)));
      const Scale ns = (g_plant == "native_no_scale") ? Scale{} : s;
      NativeRadius(sx, sy, ns);
      NativeProduct(np, ns);
      bool eq = true;
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
          const float h = LoadBeF(fr.b + kMemberProduct + 16 * r + 4 * c);
          eq = eq && std::memcmp(&h, &np[r][c], 4) == 0;
        }
      const float nsx = float(sx), nsy = float(sy);
      eq = eq && std::memcmp(&nsx, &f29, 4) == 0 && std::memcmp(&nsy, &f28, 4) == 0;
      Check(eq, (std::string(as.name) + ": the native copy's product and radius scales equal the hooked recompiled path bit for bit").c_str());
    }
  }

  // 7. (lane 234) the STAGE visibility frustum (sub_82769B98, hook 0x82769D10): 8 corners of 16 bytes in the camera's own space (x = +-hx d, y = +-hy d, z = d at the near and the far
  // plane); the shipped edit (ApplyStageFrustum) widens x by k and y by t. Oracle: the same frustum BUILT with hx k and hy t. The test the game runs per stage node is a six-plane
  // reject; here the four side planes from the corners and a sphere test, over a grid of stage nodes (barricade corners and the like, centimetres, in front of the camera).
  {
    struct Aspect {
      const char* name;
      double wide, tall;
    };
    const Aspect aspects[] = {{"16:9", 1.0, 1.0}, {"20:9 (k 1.25)", 1.25, 1.0}, {"21:9", 21.0 / 9.0 / (16.0 / 9.0), 1.0}, {"32:9", 2.0, 1.0}, {"taller 4:3", 1.0, 4.0 / 3.0}};
    const float hx = 0.82f, hy = 0.46f, zn = 10.0f, zf = 6000.0f;  // the game's 16:9 half extents per unit depth (a model: the point is the ratio)
    auto build = [&](uint8_t* fr, float kx, float ky) {
      int j = 0;
      for (float d : {zn, zf})
        for (int sy = -1; sy <= 1; sy += 2)
          for (int sx = -1; sx <= 1; sx += 2) {
            StoreBeF(fr + 16 * j + 0, sx * hx * kx * d);
            StoreBeF(fr + 16 * j + 4, sy * hy * ky * d);
            StoreBeF(fr + 16 * j + 8, d);
            StoreBeF(fr + 16 * j + 12, 1.0f);
            ++j;
          }
    };
    struct Planes {
      float n[4][3];  // left, right, bottom, top (inward)
    };
    auto planes = [&](const uint8_t* fr) {  // from the far corners: the side planes of a symmetric frustum through the origin
      auto C = [&](int j, int c) { return LoadBeF(fr + 16 * j + 4 * c); };
      const float xf = C(5, 0), yf = C(6, 1), zfar = C(5, 2);  // far corners are j = 4..7 : (-x,-y) (+x,-y) (-x,+y) (+x,+y)
      Planes p;
      const float L = std::sqrt(xf * xf + zfar * zfar), T = std::sqrt(yf * yf + zfar * zfar);
      p.n[0][0] = zfar / L, p.n[0][1] = 0, p.n[0][2] = xf / L;
      p.n[1][0] = -zfar / L, p.n[1][1] = 0, p.n[1][2] = xf / L;
      p.n[2][0] = 0, p.n[2][1] = zfar / T, p.n[2][2] = yf / T;
      p.n[3][0] = 0, p.n[3][1] = -zfar / T, p.n[3][2] = yf / T;
      return p;
    };
    auto visible = [&](const Planes& p, const float c[3], float r) {
      for (int i = 0; i < 4; ++i)
        if (p.n[i][0] * c[0] + p.n[i][1] * c[1] + p.n[i][2] * c[2] < -r) return false;
      return true;
    };
    for (const Aspect& as : aspects) {
      const Scale s = FromFactors(as.wide, as.tall);
      Frame fr, orc, base;
      build(fr.b, 1.0f, 1.0f);
      build(orc.b, float(as.wide > 1.001 ? as.wide : 1.0), float(as.tall > 1.001 ? as.tall : 1.0));
      std::memcpy(base.b, fr.b, sizeof(fr.b));
      Scale use = s;
      if (g_plant == "stage_no_x") use.x = 1.0f;
      if (g_plant == "stage_old") use = Scale{};
      ApplyStageFrustum(fr.b, use);
      if (g_plant == "stage_wrong_axis" && s.On()) {  // the factor on the wrong component (z)
        std::memcpy(fr.b, base.b, sizeof(base.b));
        for (int j = 0; j < 8; ++j) StoreBeF(fr.b + 16 * j + 8, LoadBeF(fr.b + 16 * j + 8) * float(as.wide));
      }
      const Planes pw = planes(fr.b), po = planes(orc.b), p0 = planes(base.b);
      size_t diff = 0, lost = 0, kept = 0, kept0 = 0, side = 0, n = 0;
      for (int ix = -40; ix <= 40; ++ix)
        for (int iz = 1; iz <= 30; ++iz) {
          const float z = 120.0f * iz, c[3] = {ix * 60.0f * (iz / 8.0f + 0.5f), 20.0f, z};  // nodes across the view at 30 depths, radius 90
          const bool a = visible(pw, c, 90.0f), b = visible(po, c, 90.0f), z0 = visible(p0, c, 90.0f);
          ++n;
          diff += a != b;
          lost += z0 && !a;
          kept += a;
          kept0 += z0;
          side += a && !z0;
        }
      std::printf("%-16s stage nodes kept %5zu of %zu (16:9 %5zu), side strip %4zu, differs from the oracle %zu, lost vs 16:9 %zu\n", as.name, kept, n, kept0, side, diff, lost);
      Check(diff == 0, (std::string(as.name) + ": the stage frustum with the edit culls exactly like one built k times wider").c_str());
      Check(lost == 0, (std::string(as.name) + ": no stage node the 16:9 frustum kept is lost").c_str());
      if (as.wide > 1.001) Check(side > 0, (std::string(as.name) + ": stage nodes in the side strip (corners the 16:9 frustum culls) are kept").c_str());
      else if (as.tall <= 1.001) Check(std::memcmp(fr.b, base.b, sizeof(fr.b)) == 0, "16:9: the frustum corners are byte-equal after the edit");
    }
  }

  std::printf("%s\n", g_fail ? "FAILED" : "all passed");
  return g_fail ? 1 : 0;
}
