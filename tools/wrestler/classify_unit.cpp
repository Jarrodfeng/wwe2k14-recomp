// classify_unit: lane 134 follow-up (Session 101). Synthetic element lists against wrestler_geom::Classify's short maps (the game's 11-dword yCh_trans vertex). Our code; no game data.
//   classify_unit [--plant]     exit 0 = every check holds; --plant runs case A with the short maps off and must therefore FAIL (the twin).
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../../src/assets/wrestler_geom.h"

using namespace assets;
using namespace assets::wrestler_geom;

static int g_fail = 0;
static void Check(bool ok, const char* what) { std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what); g_fail += !ok; }
static void PutBeF(uint8_t* d, float f) { uint32_t u; std::memcpy(&u, &f, 4); d[0] = uint8_t(u >> 24), d[1] = uint8_t(u >> 16), d[2] = uint8_t(u >> 8), d[3] = uint8_t(u); }

// A 2-vertex part: S0 28 B (pos, normal, colour), S1 4 pairs (32 B), S2 uv (8 B), S3 tangent (12 B) or none.
struct Synth {
  std::vector<uint8_t> s0, s1, s2, s3;
  jboy_parts::Part p;
  explicit Synth(bool with_s3) {
    s0.assign(56, 0), s1.assign(64, 0), s2.assign(16, 0), s3.assign(with_s3 ? 24 : 0, 0);
    const float pos[2][3] = {{1, 2, 3}, {4, 5, 6}}, nrm[2][3] = {{0, 1, 0}, {1, 0, 0}}, tan[2][3] = {{1, 0, 0}, {0, 0, 1}};   // normal perpendicular to tangent
    for (int v = 0; v < 2; ++v) {
      for (int c = 0; c < 3; ++c) { PutBeF(&s0[28 * v + 4 * c], pos[v][c]); PutBeF(&s0[28 * v + 12 + 4 * c], nrm[v][c]); if (with_s3) PutBeF(&s3[12 * v + 4 * c], tan[v][c]); }
      s0[28 * v + 27] = 0x7F;   // colour word
      for (int k = 0; k < 4; ++k) { s1[32 * v + 8 * k] = uint8_t(k); PutBeF(&s1[32 * v + 8 * k + 4], 0.25f); }
      PutBeF(&s2[8 * v], 0.5f), PutBeF(&s2[8 * v + 4], 0.25f);
    }
    p.vertices = 2, p.ok = true, p.tiles = true;
    p.stride[0] = 28, p.stride[1] = 32, p.stride[2] = 8, p.stride[3] = with_s3 ? 12 : 0;
    p.s[0] = s0.data(), p.s[1] = s1.data(), p.s[2] = s2.data(), p.s[3] = with_s3 ? s3.data() : nullptr;
  }
};
static LayoutElement El(uint8_t fmt, int dword, uint16_t vk, uint16_t host, uint8_t flags = 0) { LayoutElement e; e.source_format = fmt, e.guest_offset = dword, e.vk_format = vk, e.host_offset = host, e.flags = flags; return e; }
static bool HasRole(const Roles& r, Role x, uint16_t off) { for (auto& e : r.r) if (e.role == x && e.offset == off) return true; return false; }

int main(int argc, char** argv) {
  const bool plant = argc > 1 && std::string(argv[1]) == "--plant";
  Synth withS3(true), noS3(false);
  // stream element lists: pos@0 (host 0), weights@4 (16), slots@7 (28), normal@8 (32), [X@9 (40)], uv@10 (48 or 40)
  auto base = [&](bool with9, LayoutElement nine) {
    std::vector<LayoutElement> v = {El(38, 0, 109, 0), El(57, 4, 106, 16), El(6, 7, 41, 28, 2), El(17, 8, 92, 32, 1)};
    if (with9) v.push_back(nine);
    v.push_back(El(31, 10, 83, with9 ? 48 : 40));
    return v;
  };
  Roles r;
  {  // A: f17@9 signed -> accepted, as Tangent only
    auto v = base(true, El(17, 9, 92, 40, 1));
    const bool ok = Classify(withS3.p, v.data(), v.size(), 52, 11, r, !plant);
    Check(ok && HasRole(r, Role::Tangent, 40) && !HasRole(r, Role::Colour, 40), "A: f17@9 accepted (short maps allowed) and named Tangent, never Colour");
    Roles r2;
    Check(!Classify(withS3.p, v.data(), v.size(), 52, 11, r2), "A': the same list with the short maps off (the default: props / ring / screens) is refused");
  }
  {  // B: f6@9 unsigned normalised -> Colour only
    auto v = base(true, El(6, 9, 37, 40, 0));
    const bool ok = Classify(withS3.p, v.data(), v.size(), 52, 11, r, true);
    Check(ok && HasRole(r, Role::Colour, 40) && !HasRole(r, Role::Tangent, 40), "B: f6@9 accepted and named Colour, never Tangent");
  }
  {  // C: nothing fetched at dword 9: both maps give the same Roles
    auto v = base(false, El(0, 0, 0, 0));
    const GuestMap g = MapFor(withS3.p);
    GuestMap nc = g; nc.colour = -2, nc.uv = g.uv - 1, nc.dwords = g.dwords - 1;
    GuestMap nt = g; nt.tangent = -1, nt.colour = g.tangent, nt.uv = g.uv - 1, nt.dwords = g.dwords - 1;
    Roles a, b;
    const bool oka = ClassifyMap(withS3.p, nc, v.data(), v.size(), 48, 11, a), okb = ClassifyMap(withS3.p, nt, v.data(), v.size(), 48, 11, b);
    bool same = a.r.size() == b.r.size();
    for (size_t i = 0; same && i < a.r.size(); ++i) same = a.r[i].role == b.r[i].role && a.r[i].offset == b.r[i].offset && a.r[i].vk == b.r[i].vk;
    Check(oka && okb && same && Classify(withS3.p, v.data(), v.size(), 48, 11, r, true), "C: no element at dword 9: no_colour and no_tangent give equal Roles, Classify accepts");
  }
  {  // D: stride dwords - 2 refused
    auto v = base(false, El(0, 0, 0, 0));
    for (auto& e : v) if (e.source_format == 31) e.guest_offset = 9;
    Check(!Classify(withS3.p, v.data(), v.size(), 48, 10, r, true), "D: guest stride 10 (two dwords short) is refused");
  }
  {  // E: no S3 (g.tangent < 0): no fallback
    auto v = base(false, El(0, 0, 0, 0));
    for (auto& e : v) if (e.source_format == 31) e.guest_offset = 9;
    Check(!Classify(noS3.p, v.data(), v.size(), 48, 10, r, true), "E: a file without S3 gets no fallback (stride 10 against map 11)");
  }
  {  // F: the "no normal" shape: tangent-only stream: f17@8 is really the tangent. Classify accepts (by position), the byte compare must not
    auto v = base(false, El(0, 0, 0, 0));
    Roles acc;
    const bool ok = Classify(withS3.p, v.data(), v.size(), 48, 11, acc, true);
    Check(ok && HasRole(acc, Role::Normal, 32), "F: Classify accepts f17@8 as Normal by position (the shape it cannot tell apart)");
    Roles truth = acc;   // the stream's bytes: the element at 32 carries the TANGENT (perpendicular to the normal)
    for (auto& e : truth.r) if (e.role == Role::Normal) e.role = Role::Tangent;
    BuildOptions bo;
    std::vector<uint8_t> theirs, ours;
    BuildVertices(withS3.p, truth, bo, theirs);
    BuildVertices(withS3.p, acc, bo, ours);
    CompareResult cr;
    Compare(acc, ours, theirs.data(), 2, cr);
    Check(!cr.all_ok && cr.role[uint8_t(Role::Normal)].bad > 0, "F': Compare fails on the perpendicular normal / tangent (the byte proof is what names the roles)");
    CompareResult cr2;
    Compare(truth, theirs, theirs.data(), 2, cr2);
    Check(cr2.all_ok, "F'': the control: the true roles compare clean");
  }
  {  // G (lane 206): the LONG map. A file with no S3 drawn from a 12-dword vertex: pos0 weights4 slots7 normal8 TANGENT9 colour10 uv11 (The Rock Retro's elbow pads)
    auto lng = [&](int uv_dword) {
      return std::vector<LayoutElement>{El(38, 0, 109, 0), El(57, 4, 106, 16), El(6, 7, 41, 28, 2), El(17, 8, 92, 32, 1), El(17, 9, 92, 40, 1), El(6, 10, 37, 48, 0), El(31, uv_dword, 83, 52)};
    };
    auto v = lng(11);
    Roles rl;
    const bool ok = Classify(noS3.p, v.data(), v.size(), 56, 12, rl, true, false, true);
    Check(ok && rl.tangent_not_in_file && HasRole(rl, Role::Tangent, 40) && HasRole(rl, Role::Colour, 48) && HasRole(rl, Role::Uv, 52), "G: a file without S3 and a 12-dword stream joins by the long map (tangent 9, colour 10, uv 11), flagged tangent_not_in_file");
    Roles r0;
    Check(!Classify(noS3.p, v.data(), v.size(), 56, 12, r0, true), "G1: the long map is off by default and with only the short maps on (CAW editor, props, ring, screens)");
    auto v10 = lng(10);
    Roles r1;
    Check(!Classify(noS3.p, v10.data(), v10.size(), 56, 12, r1, true, false, true), "G2: PLANT uv shifted to dword 10 is still refused");
    Roles r2;
    Check(!Classify(noS3.p, v.data(), v.size(), 56, 12, r2, true, /*shift_fault=*/true, true), "G3: the shift plant (uv left at the file's dword) refuses the real layout");
    Roles r3;
    Check(Classify(withS3.p, v.data(), v.size(), 56, 12, r3, true, false, true) && !r3.tangent_not_in_file, "G4: a file WITH S3 takes its own map, never the long one (no flag)");
    // the bytes: zero tangent from the file's truth; the stream's leftovers must not count, unless the flag is off (the plant)
    BuildOptions bo;
    std::vector<uint8_t> ours, theirs;
    BuildVertices(noS3.p, rl, bo, ours);
    theirs = ours;
    for (int vtx = 0; vtx < 1; ++vtx) theirs[size_t(vtx) * 56 + 40] = 0x55, theirs[size_t(vtx) * 56 + 41] = 0x12;   // leftover bytes in the tangent slot
    CompareResult cr;
    Compare(rl, ours, theirs.data(), 2, cr);
    Check(cr.all_ok && cr.role[uint8_t(Role::Tangent)].bad == 0, "G5: leftover tangent bytes in the stream are not counted bad when the file has no tangent");
    Roles flagless = rl;
    flagless.tangent_not_in_file = false;
    CompareResult cr2;
    Compare(flagless, ours, theirs.data(), 2, cr2);
    Check(!cr2.all_ok && cr2.role[uint8_t(Role::Tangent)].bad == 1, "G6: PLANT the tangent compared normally refuses");
    Check(ours[40] == 0 && ours[41] == 0, "G7: we write a zero tangent (rule 6: the 360's leftovers are never copied)");
    // review S2: a stream whose tangents are NOT mostly zero is not this layout
    std::vector<uint8_t> real = ours;
    for (int vtx = 0; vtx < 2; ++vtx) real[size_t(vtx) * 56 + 40] = 0x55, real[size_t(vtx) * 56 + 41] = 0x12;
    CompareResult cr3;
    Compare(rl, ours, real.data(), 2, cr3);
    Check(!cr3.all_ok, "G8: S2 all stream tangents non-zero: the exemption does not apply, refused");
    // review S3a: no tangent element fetched: no flag
    std::vector<LayoutElement> v_nt = {El(38, 0, 109, 0), El(57, 4, 106, 16), El(6, 7, 41, 28, 2), El(17, 8, 92, 32, 1), El(6, 10, 37, 40, 0), El(31, 11, 83, 44)};
    Roles r4;
    Check(Classify(noS3.p, v_nt.data(), v_nt.size(), 48, 12, r4, true, false, true) && !r4.tangent_not_in_file, "G9: S3a no tangent fetched: joined by the long map without the flag");
  }
  std::printf("%d failure(s)\n", g_fail);
  return g_fail ? 1 : 0;
}
