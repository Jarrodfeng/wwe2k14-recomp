// S108 lane SHOT-B: the unit of tools\scene\subject_cam.h (the pure resolver of a subject shot and the pick). Synthetic data only: no game files, no GPU, ~0.1 s.
//   pwsh -NoProfile -File tools\scene\build_subject_unit.ps1   ->   out\tools\subject_unit.exe [--plant no_togame | nearest_only]
// Real run: every row PASSES, exit 0. A planted twin must FAIL its row (exit 1):
//   --plant no_togame     the viewer camera handed in unconverted (the game-space trap): the head does not land at the picture centre, the picture is the mirror image.
//   --plant nearest_only  the pick keeps only the nearest hit: a blended surface over another shows as ONE row.
// (a tool unit: the resolver is the rig's own code, no production hook enters it)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "subject_cam.h"

using namespace subj;

static int g_fail = 0;
static void Check(const char* name, bool ok) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) ++g_fail;
}

static const int W = 1280, H = 720;
static bool g_no_togame = false, g_nearest_only = false;

static bool Near(double a, double b, double e) { return std::fabs(a - b) <= e; }

// A square panel at x = x0 facing -x or +x, corners wound so the drawn front faces `face_dir` (+1: toward +x).
static void Panel(std::vector<wgs::Tri>& out, double x0, double y0, double y1, double z0, double z1, int face_dir) {
  wgs::Tri a, b;
  const V3 q0 = {x0, y0, z0}, q1 = {x0, y1, z0}, q2 = {x0, y1, z1}, q3 = {x0, y0, z1};
  // cross(b - a, c - a) for (q0, q1, q2) = (0,0,dz) ... compute and orient by test
  const V3 n = wgs::Cross(wgs::Sub(q1, q0), wgs::Sub(q2, q0));
  const bool pos = n[0] > 0;
  if ((face_dir > 0) == pos) { a.p[0] = q0, a.p[1] = q1, a.p[2] = q2; b.p[0] = q0, b.p[1] = q2, b.p[2] = q3; }
  else { a.p[0] = q0, a.p[1] = q2, a.p[2] = q1; b.p[0] = q0, b.p[1] = q3, b.p[2] = q2; }
  out.push_back(a), out.push_back(b);
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) {
      const std::string p = argv[++i];
      if (p == "no_togame") g_no_togame = true;
      else if (p == "nearest_only") g_nearest_only = true;
      else { std::fprintf(stderr, "unknown plant %s\n", p.c_str()); return 2; }
    }

  // ---- 1. the subject argument ----
  {
    Spec s;
    std::string e;
    Check("spec: face:0", ParseSpec("face:0", s, e) && s.kind == "face" && s.n == 0 && s.from.empty() && s.radius == 0);
    Check("spec: stage:114,from=back,radius=50", ParseSpec("stage:114,from=back,radius=50", s, e) && s.kind == "stage" && s.n == 114 && s.from == "back" && Near(s.radius, 50, 1e-9));
    Check("spec: prop:2004", ParseSpec("prop:2004", s, e) && s.n == 2004);
    Check("spec refused: no colon", !ParseSpec("face", s, e));
    Check("spec refused: unknown kind", !ParseSpec("belt:3", s, e));
    Check("spec refused: bad from", !ParseSpec("face:0,from=up", s, e));
    Check("spec refused: bad number", !ParseSpec("face:x", s, e));
    Check("spec refused: unknown option", !ParseSpec("face:0,zoom=2", s, e));
  }

  // ---- 2. the space: a known head point in GAME space (x, -y, z: y down) lands at the picture centre ----
  {
    // The game's head: 170 up (game y = -170), at x 120, z -300. The record is game space; BuildFrame's palette is S X S, viewer space y up. The bone's bind point is in VIEWER space
    // (assets::Bone::bind_world, MirrorY applied); the palette rows move it (here: a pure translation (120, 0, -300 + ...)).
    const double s = 0.9;
    assets::Matrix bind = assets::Identity();
    bind[12] = 0, bind[13] = 170.0 / s, bind[14] = 0;   // the model-space point: the head's own bind translation (unscaled file position), viewer space
    float pal[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 120, 0, -300, 1};   // UNSCALED palette, so the body scale s is applied to the bind point first
    const V3 head = JointWorld(bind, pal, s);
    Check("joint: bind x scale x palette = (120, 170, -300)", Near(head[0], 120, 1e-4) && Near(head[1], 170, 1e-4) && Near(head[2], -300, 1e-4));
    const V3 l = {head[0] - 3.5, head[1] + 9, head[2] + 12}, r = {head[0] + 3.5, head[1] + 9, head[2] + 12};   // eyelids: in front (+z), a little above the head joint
    Spec sp;
    const V3 game_dir = {0.6, 0.1, 0.8};
    Subject sub = FaceSubject(head, true, l, r, game_dir, sp);
    Check("face: forward from the eyelids = +z, kept level for an upright head", sub.ok && sub.source == "eyelids (level)" && Near(sub.normal[2], 1.0, 0.02) && Near(sub.normal[0], 0.0, 0.01) && Near(sub.normal[1], 0.0, 0.01));
    Check("face: the raw forward is perpendicular to the eye axis and points to the eyes", Near(sub.forward[0], 0, 1e-6) && Near(sub.forward[1], 0.6, 1e-3) && Near(sub.forward[2], 0.8, 1e-3));
    Check("face: target = the eyelid midpoint", Near(sub.target[0], 120, 1e-3) && Near(sub.target[1], 179, 1e-3) && Near(sub.target[2], -288, 1e-3));
    const Cam cam = CameraOf(sub);
    const GameCam g = g_no_togame ? NoToGame(cam) : ToGameCam(cam);
    float vp[16], eye[3];
    BuildFrameCamera(g, vp, eye);   // what BuildFrame makes of the record's camera: the picture the renderer draws
    const Px c = Project(vp, sub.target, W, H);
    Check("space: the target is at the picture centre +-5 %", c.ok && Near(c.x, W / 2.0, 0.05 * W) && Near(c.y, H / 2.0, 0.05 * H));
    const Px hp = Project(vp, head, W, H);
    Check("space: the head joint is below the eyelids in the picture (up = up, not mirrored)", c.ok && hp.ok && hp.y > c.y);
    Check("space: the eye sits on the face side (+z of the face)", eye[2] > head[2] && Near(eye[0], 120, 1.0));
    Check("space: the eyelid corners are inside the picture", [&] {
      const Px a = Project(vp, l, W, H), b = Project(vp, r, W, H);
      return a.ok && b.ok && a.x > 0 && a.x < W && b.x > 0 && b.x < W && a.y > 0 && a.y < H;
    }());
    // fallback when the model has no eyelid nodes
    const Subject fb = FaceSubject(head, false, l, r, game_dir, sp);
    Check("face: no eyelids -> the game's direction, labelled fallback", fb.ok && fb.source.rfind("fallback", 0) == 0 && Near(fb.normal[0], 0.6, 1e-9));
    // from=back / right
    Spec back;
    ParseSpec("face:0,from=back", back, *new std::string());
    const Subject sb = FaceSubject(head, true, l, r, game_dir, back);
    Check("face: from=back = the opposite side", Near(sb.normal[2], -sub.normal[2], 0.02));
    Spec right;
    ParseSpec("face:0,from=right", right, *new std::string());
    const Subject sr = FaceSubject(head, true, l, r, game_dir, right);
    Check("face: from=right = a side (perpendicular to the forward)", Near(wgs::Dot(sr.normal, sub.normal), 0.0, 0.05) && Near(sr.normal[1], 0.0, 0.05));
  }

  // ---- 3. the wrestler box ----
  {
    Spec sp;
    const Subject w = WrestlerSubject({-40, 0, -20}, {40, 190, 20}, true, {0, 0.3, 1}, {1, 0, 0}, sp);
    Check("wrestler: target = the box centre, radius = the half diagonal", w.ok && Near(w.target[1], 95, 1e-9) && Near(w.radius, std::sqrt(40 * 40 + 95 * 95 + 20 * 20), 1e-6));
    Check("wrestler: facing flattened to the horizontal", Near(w.normal[1], 0, 1e-9) && Near(w.normal[2], 1, 1e-9));
    const Cam c = CameraOf(w);
    float vp[16], eye[3];
    BuildFrameCamera(ToGameCam(c), vp, eye);
    bool inside = true;
    for (double x : {-40.0, 40.0})
      for (double y : {0.0, 190.0})
        for (double z : {-20.0, 20.0}) {
          const Px p = Project(vp, {x, y, z}, W, H);
          inside = inside && p.ok && p.x > 0 && p.x < W && p.y > 0 && p.y < H;
        }
    Check("wrestler: the whole box is inside the picture", inside);
  }

  // ---- 4. the stage: four panels of a ring (as slot 114), the facing that saw the most area ----
  {
    std::vector<wgs::Tri> tris;
    Panel(tris, 3550, 200, 300, -1268, 1268, -1);   // +x wall, front toward -x (inward)
    Panel(tris, -3550, 200, 300, -1268, 1268, +1);                              // -x wall, front toward +x
    Spec sp;
    // the record's camera stands at x -2000 looking toward +x: it sees the +x wall's inward face (front -x) and the -x wall's BACK
    const V3 eye = {-2000, 250, 0};
    const Subject st = StageSubject(tris, eye, sp);
    Check("stage: a subject with triangles", st.ok && st.piece && st.source == "the facing that saw the most area");
    Check("stage: the facing is the +x wall's inward face (-x)", Near(st.normal[0], -1.0, 1e-6));
    const Cam c = CameraOf(st);
    float vp[16], e[3];
    BuildFrameCamera(ToGameCam(c), vp, e);
    bool inside = true;
    for (size_t k = 0; k < 2; ++k)   // the framed strip = the panels of the chosen facing (the +x wall, the first two triangles)
      for (int j = 0; j < 3; ++j) {
        const Px p = Project(vp, tris[k].p[j], W, H);
        inside = inside && p.ok && p.x >= 0 && p.x <= W && p.y >= 0 && p.y <= H;
      }
    Check("stage: every corner of the framed strip is inside the picture", inside);
    Check("stage: the eye stands inside the hall, not behind the far wall", e[0] > -3550 && e[0] < 3550);
    Spec back;
    std::string er;
    ParseSpec("stage:114,from=back", back, er);
    Check("stage: from=back = the opposite facing", Near(StageSubject(tris, eye, back).normal[0], 1.0, 1e-6));
    Check("stage: an empty slot is refused with a reason", !StageSubject({}, eye, sp).ok && !StageSubject({}, eye, sp).why.empty());
  }

  // ---- 5. the pick: every hit front to back; the ray through a pixel finds the point that projects there ----
  {
    Cam c;
    c.eye = {0, 0, -100}, c.target = {0, 0, 0}, c.fov = 30, c.near_z = 1, c.far_z = 1000;
    wgs::MakeVP(c, {0, 1, 0});
    std::vector<PickMesh> meshes(2);
    // mesh 0: an opaque panel at z = 50; mesh 1: a BLENDED panel in front of it at z = 20 (the 806-over-band shape)
    for (int k = 0; k < 2; ++k) {
      const double z = k == 0 ? 50 : 20;
      meshes[size_t(k)].object = k == 0 ? "arena:slot:114" : "arena:slot:806";
      meshes[size_t(k)].part = k == 0 ? "slot 114" : "slot 806";
      meshes[size_t(k)].shader = "yBG_Default";
      meshes[size_t(k)].texture = k == 0 ? "met_st_logo_01" : "glow";
      meshes[size_t(k)].ordinal = 0;
      meshes[size_t(k)].blend = k == 1;
      meshes[size_t(k)].tris = {{-30, -30, z}, {-30, 30, z}, {30, 30, z}, {-30, -30, z}, {30, 30, z}, {30, -30, z}};
    }
    const V3 target = {10, 5, 50};
    const Px p = Project(c.vp, target, W, H);
    const Ray r = PixelRay(c.vp, W, H, p.x, p.y);
    Check("pick: the ray through the projected pixel passes the point", [&] {
      if (!r.ok) return false;
      const V3 d = wgs::Sub(target, r.origin);
      const double along = wgs::Dot(d, r.dir);
      const V3 q = {r.origin[0] + r.dir[0] * along, r.origin[1] + r.dir[1] * along, r.origin[2] + r.dir[2] * along};
      return wgs::Len(wgs::Sub(q, target)) < 0.05;
    }());
    const std::vector<Hit> hits = PickAll(r, meshes, c.eye, g_nearest_only);
    Check("pick: BOTH layers are listed (the blended one over the opaque one)", hits.size() == 2);
    Check("pick: front to back (the blended z=20 first, then z=50)", hits.size() == 2 && meshes[hits[0].mesh].blend && !meshes[hits[1].mesh].blend && hits[0].dist < hits[1].dist);
    Check("pick: the distance is from the eye", hits.size() == 2 && hits[0].dist > 119 && hits[0].dist < 125);
    Check("pick: the row names the object, mesh, material and texture", hits.size() == 2 && HitLine(hits[1], meshes[hits[1].mesh], 1).find("arena:slot:114 mesh 0") != std::string::npos &&
                                                                          HitLine(hits[1], meshes[hits[1].mesh], 1).find("met_st_logo_01") != std::string::npos);
    const Ray miss = PixelRay(c.vp, W, H, 5, 5);
    Check("pick: a pixel on nothing lists nothing", PickAll(miss, meshes, c.eye).empty());
  }

  std::printf("%s (%d failed)\n", g_fail ? "subject_unit FAIL" : "subject_unit PASS", g_fail);
  return g_fail ? 1 : 0;
}
