// host_rig: the headless GPU rig of the scene host (S103 landing 2, piece P7 H1; docs\renderer\P7_DESIGN.md section 5 H1 (b)). 0 game runs.
//   host_rig --mode host --scene DIR --game DIR --out DIR --frames 60,120 [--size WxH] [--no-curve] [--plant NAME]
//   host_rig --mode ref  --scene DIR --game DIR --out DIR --frames 60,120 [--size WxH] [--no-curve]
//   host_rig --mode sizes --capture DIR            (every present of the capture: the size read from fetch words equals its image record)
//
// host: the SAME SceneHost the game runs (src\renderer_host\scene_host.cpp), fed one capture scene record per tick through its mailbox (Post), on a headless renderer whose present draws into an
// offscreen image of the frame's size (present_offscreen_w/h). Per frame the host's presented image is read back (ReadPresent) and written as <out>/host_<frame>.png (+ .rgba).
// ref: LiveSceneDraw::DrawToPng of the same record at the same size with the same display curve -> <out>/ref_<frame>.png. The comparison is tools\scene\host_rig.py (host and ref run in
// separate processes: the lane-C arena state is global).
// Rig-only fixtures, TOLD in the report: the capture's records carry no camera, so a look-at camera is synthesised (host_rig_camera.h, the frame_rig.cpp SynthCamera) the same way for both modes;
// the body scales a live run measures are set to 1.0 (frame_rig --body-scale-one); holes and missing post rows do not block (DevForce), exactly as the dev integration switch does.
// S103 P7 H2 (the 2D rows on F): `--flat` hands the records to the host as the game would (no forced OURS: a menu / title / load frame is a flat frame, black + its rows); `--capture DIR` supplies the
// sprites' pictures and the movies' planes from the capture the records came from (host_rig_2d.h; a stand-in for the live resolver); the per-frame line adds the rows drawn / dropped by reason.
// More planted faults: --plant rows_first (the rows are drawn under the copy of the 3D picture), --plant rows_never (no 2D step), --plant stale_planes (a movie row shows the previous frame's planes).
// Planted faults (host mode, each must make host_rig.py FAIL): --plant stale_record (the record of the previous frame is drawn), --plant skip_copy (F is presented without the copy of `out`),
// --plant wrong_size (the tick's size is one pixel narrower).
// S103 P7 tween (the decoupled 120 in the host): `--tween` turns HostOptions::tween_wanted on and runs the frame list TWICE (pass 1 loads the assets and warms the host; pass 2 is measured: every tick's previous
// tick is the one before it in the list, A,B,A,B ...). Per measured tick the rig prints the halfway frame's constants against its OWN expectation (every keyed draw found in both ticks: each blend range =
// (previous + this) / 2 to 1e-6, every byte outside the mixed ranges = this tick's) and three image distances (previous picture, halfway picture, this tick's final picture). `--cut N` marks record N's camera as a
// cut (Camera::cut): nothing may be blended into it. `--tween-plant alpha0|no_forget|swap` = the three planted faults (SceneHost TestSeam): weight 0 (the halfway is the previous tick), the previous tick survives a
// cut, the tick's own picture presented before the halfway one. host_rig.py judges the printed numbers; the final picture is also compared with a run without `--tween`.
#define WWE2K14_FRAME_TICK_CORE_ONLY 1   // frame_tick.h without the guest-memory part (SizeOfFetch only)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "png_out.h"
#include "host_rig_camera.h"
#include "host_rig_2d.h"
#include "host_rig_real.h"   // S103 PN4: the REAL picture resolver + movie planes over forged guest memory
#include "resolver_unit.h"   // S103 PN4F: the resolver's lifecycle twins (--mode resolver)
#include "../../src/renderer/overlay.h"   // S103 GT: rdr::g_gpu_stamps / g_gpu_pass_stamps
#include "../../src/renderer/group_stamps.h"   // S103 GSTP: rdr::g_gpu_group_stamps / g_gpu_group_plant
#include "../../src/renderer/renderer.h"
#include "../../src/renderer_game/scene_frame.h"
#include "../../src/renderer_host/scene_host.h"
#include "../../src/renderer_host/overlay_mailbox.h"   // S103 AIM (--mode overlay)
#include "../../src/renderer_scene/body_scale.h"
#include "../../src/renderer/depth_nostencil.h"   // S103 CHS C5: the stencil liveness switch, its plant and census
#include "../../src/renderer_scene/live_scene_draw.h"
#include "../../src/renderer_scene/own_blend.h"   // S103 P7 tween review: the plants, the census, the F4 check
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_adapter/capture_reader.h"
#include "../../src/renderer_game/frame_tick.h"

namespace sh = wwe2k14::scene_host;

static std::vector<uint64_t> ParseList(const std::string& s) {
  std::vector<uint64_t> v;
  for (size_t at = 0; at < s.size();) {
    const size_t c = s.find(',', at);
    const std::string item = s.substr(at, c == std::string::npos ? std::string::npos : c - at);
    if (!item.empty()) v.push_back(std::strtoull(item.c_str(), nullptr, 10));
    if (c == std::string::npos) break;
    at = c + 1;
  }
  return v;
}

// The S100 five-point display curve (rig_parity.py S100_XS / S100_YS), 256 x RGBA8, alpha 255: a curve that is NOT the identity, so a present that skips it is caught.
static std::vector<uint8_t> Curve() {
  const int xs[] = {0, 16, 32, 64, 128, 192, 255}, ys[] = {0, 6, 16, 48, 115, 185, 255};
  std::vector<uint8_t> c(1024);
  for (int i = 0; i < 256; ++i) {
    int k = 0;
    while (k < 5 && i > xs[k + 1]) ++k;
    const double t = double(i - xs[k]) / double(xs[k + 1] - xs[k]);
    const int v = int(std::lround(ys[k] + t * (ys[k + 1] - ys[k])));
    c[size_t(4 * i)] = c[size_t(4 * i + 1)] = c[size_t(4 * i + 2)] = uint8_t(std::clamp(v, 0, 255));
    c[size_t(4 * i + 3)] = 255;
  }
  return c;
}

static bool LoadRecord(const std::string& scene, uint64_t n, rdr_scene::SceneTables& t, std::string& err) {
  char name[48];
  std::snprintf(name, sizeof name, "frame_%06llu.rsc", (unsigned long long)n);
  if (!rdr_scene::ReadRecord((std::filesystem::path(scene) / name).string(), t, err)) return false;
  for (uint32_t addr : t.engine_objects) rdr_scene::BodyScaleTable::Get().Set(addr, 1.0f);   // fixture: scale 1 (frame_rig --body-scale-one)
  if (!t.has_camera) host_rig_cam::SynthCamera(t);                                        // fixture: a look-at camera (the capture's records carry none)
  return true;
}

// ---- --mode sizes -------------------------------------------------------------------------------------------------------------------------------------
// Every frame of the capture that presents: the present's image record (width x height) must equal the size SizeOfFetch reads from fetch words that describe it (encoded the way reader.h ShapeOf
// decodes them). Two different present sizes in one capture are listed (the host remakes F on a size change).
// ---- --tween: what the rig keeps of the previous tick, and the per-tick facts the host's callback fills --------------------------------------------------------------------------------------
struct KeptTick {
  bool valid = false;
  std::vector<rdr::DrawOp> draws;
  std::vector<rdr::BlendRange> ranges;
  std::vector<uint8_t> constants;
};
struct TickInfo {
  bool seen = false, half = false;
  uint32_t matched = 0;
  uint64_t keyed = 0, pairs = 0, moving = 0, floats = 0, bad_floats = 0, bad_outside = 0;
  double max_err = 0;
  std::vector<uint8_t> half_rgba;
  uint32_t hw = 0, hh = 0;
};
static double MeanAbs(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, uint64_t* moving_px = nullptr) {   // RGB(A) pictures of one size: mean absolute difference over RGB (0-255)
  if (a.size() != b.size() || a.empty()) return -1;
  double sum = 0;
  uint64_t mv = 0;
  for (size_t i = 0; i + 3 < a.size(); i += 4) {
    int m = 0;
    for (int c = 0; c < 3; ++c) { const int d = std::abs(int(a[i + size_t(c)]) - int(b[i + size_t(c)])); sum += d; m = std::max(m, d); }
    mv += m > 8;
  }
  if (moving_px) *moving_px = mv;
  return sum / (double(a.size() / 4) * 3.0);
}

static int SizesMode(const std::string& capture) {
  rdr_adapter::CaptureReader reader;
  if (!reader.Open(capture)) { std::fprintf(stderr, "host_rig: cannot open capture %s\n", capture.c_str()); return 2; }
  std::vector<std::pair<std::pair<uint32_t, uint32_t>, uint32_t>> sizes;
  uint32_t presents = 0, bad = 0, not_plain = 0;
  for (uint32_t i = 0; i < reader.frame_count(); ++i) {
    rdr_adapter::LoadedFrame fr;
    if (!reader.LoadFrame(i, fr)) continue;
    for (uint32_t p = 0; p < fr.presents.count; ++p) {
      const rdr_adapter::ImageRecord* r = reader.FindImage(uint32_t(fr.presents[p].source.image));
      if (!r) continue;
      // The "nothing known" rule (review H1): the present size is read from fetch words only for a plain 2D picture; a present of anything else (an array, a cube, a volume) must read as unknown by the
      // game's rule, so it is not part of this equality (and is counted, so a capture whose presents are not all plain 2D pictures says so).
      if (r->kind != uint8_t(rdr::ImageKind::Image2D) || r->layers != 1) { ++not_plain; continue; }
      ++presents;
      uint32_t words[6] = {0, 0, ((uint32_t(r->height) - 1u) << 13) | (uint32_t(r->width) - 1u), 0, 0, 1u << 9};   // a 2D fetch constant of that size
      if (r->flags & rdr_adapter::kImageBorder) words[3] |= 1u << 31, words[2] = ((uint32_t(r->height) - 3u) << 13) | (uint32_t(r->width) - 3u);
      uint32_t w = 0, h = 0;
      if (!wwe2k14::frame_tick::SizeOfFetch(words, w, h) || w != r->width || h != r->height) {
        ++bad;
        std::printf("MISMATCH frame %u: record %ux%u, fetch words read %ux%u\n", i, r->width, r->height, w, h);
      }
      auto key = std::make_pair(uint32_t(r->width), uint32_t(r->height));
      auto it = std::find_if(sizes.begin(), sizes.end(), [&](const auto& s) { return s.first == key; });
      if (it == sizes.end()) sizes.push_back({key, 1}); else ++it->second;
    }
  }
  std::printf("sizes: %u presents over %u frames; distinct sizes:", presents, reader.frame_count());
  for (const auto& s : sizes) std::printf(" %ux%u (x%u)", s.first.first, s.first.second, s.second);
  std::printf("; mismatches %u; presents that are not a plain 2D picture (unknown by the rule) %u\n", bad, not_plain);
  return bad || !presents ? 1 : 0;
}

// S103 AIM (--mode overlay [--plant overlay_never] [--size WxH] [--out DIR]): the stream-free build's ImGui overlay path, with no capture and no game files. The REAL SceneHost presents black
// ticks (a record with no picture); the test posts an OverlayList the way overlay_off.h's Drawer does (the static mailbox: one red quad over the top-left quarter, one quad textured with a 2x2 green
// image over the bottom-right quarter), and the presented picture (ReadPresent) must show both; then the list is taken off (SetList(nullptr)) and the picture must be black again.
// Planted fault: --plant overlay_never (the mailbox is never emptied onto the renderer) -> the quads never show -> FAIL.
static int OverlayMode(const std::string& plant, uint32_t w, uint32_t h) {
  namespace mb = wwe2k14::scene_host::overlay_mailbox;
  sh::HostOptions ho;
  ho.renderer.present_offscreen_w = w, ho.renderer.present_offscreen_h = h;
  ho.renderer.vsync = false;
  sh::SceneHost host;
  std::string err;
  if (!host.Start(ho, err)) { std::fprintf(stderr, "host_rig: FAIL host: %s\n", err.c_str()); return 3; }
  sh::TestSeam seam;
  seam.overlay_never = plant == "overlay_never";
  host.SetTestSeam(seam);
  uint64_t posted = 0;
  const auto tick = [&]() -> bool {
    sh::Msg m;
    m.guest_frame = ++posted;
    m.width = w, m.height = h;
    m.no_picture = true;
    m.record = std::make_shared<rdr_scene::SceneTables>();
    host.Post(std::move(m));
    if (!host.WaitHandled(posted, std::chrono::milliseconds(60000))) { std::fprintf(stderr, "host_rig: FAIL the host did not handle tick %llu\n", (unsigned long long)posted); return false; }
    return true;
  };
  struct Px { int r, g, b; };
  const auto at = [&](const std::vector<uint8_t>& rgba, uint32_t pw, uint32_t ph, uint32_t x, uint32_t y) { (void)ph; const uint8_t* p = &rgba[(size_t(y) * pw + x) * 4]; return Px{p[0], p[1], p[2]}; };
  const auto red = [](Px p) { return p.r > 200 && p.g < 60 && p.b < 60; };
  const auto green = [](Px p) { return p.g > 200 && p.r < 60 && p.b < 60; };
  int bad = 0;
  const auto read = [&](const char* what, std::vector<uint8_t>& rgba, uint32_t& pw, uint32_t& ph) -> bool {
    if (!tick()) return false;
    if (!host.ReadPresent(rgba, pw, ph) || rgba.size() < size_t(pw) * ph * 4 || pw < 8 || ph < 8) { std::fprintf(stderr, "host_rig: FAIL no presented picture (%s)\n", what); ++bad; return false; }
    return true;
  };
  std::vector<uint8_t> rgba;
  uint32_t pw = 0, ph = 0;
  if (!read("before the overlay", rgba, pw, ph)) return 3;
  Px a = at(rgba, pw, ph, pw / 4, ph / 4), b = at(rgba, pw, ph, 3 * pw / 4, 3 * ph / 4);
  std::printf("overlay before: top-left (%d,%d,%d) bottom-right (%d,%d,%d)\n", a.r, a.g, a.b, b.r, b.g, b.b);
  if (red(a) || green(b)) { std::fprintf(stderr, "host_rig: FAIL the overlay colours are on the picture before any overlay was posted\n"); ++bad; }
  // the textures and the list, as the Drawer hands them (CreateTexture before the first Draw, End after the last batch)
  mb::SetTexture(1, 2, 2, std::vector<uint8_t>{0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255});
  auto list = std::make_shared<rdr::OverlayList>();
  list->width = float(w), list->height = float(h);
  const auto quad = [&](float x0, float y0, float x1, float y1, uint32_t colour, uint32_t texture) {
    const uint32_t v = uint32_t(list->vertices.size()), i = uint32_t(list->indices.size());
    list->vertices.push_back({x0, y0, 0, 0, colour}), list->vertices.push_back({x1, y0, 1, 0, colour}), list->vertices.push_back({x1, y1, 1, 1, colour}), list->vertices.push_back({x0, y1, 0, 1, colour});
    for (uint16_t k : {0, 1, 2, 0, 2, 3}) list->indices.push_back(uint16_t(v + k));
    rdr::OverlayDraw d;
    d.texture = texture, d.first_index = i, d.count = 6;
    list->draws.push_back(d);
  };
  quad(0, 0, float(w) / 2, float(h) / 2, 0xFF0000FFu, 0);                       // RGBA8, R in the low byte: opaque red, the plain white texture
  quad(float(w) / 2, float(h) / 2, float(w), float(h), 0xFFFFFFFFu, 1);         // opaque white x the green image
  mb::SetList(list);
  if (!read("with the overlay", rgba, pw, ph)) return 3;
  a = at(rgba, pw, ph, pw / 4, ph / 4), b = at(rgba, pw, ph, 3 * pw / 4, 3 * ph / 4);
  const Px c = at(rgba, pw, ph, 3 * pw / 4, ph / 4);
  std::printf("overlay shown: top-left (%d,%d,%d) bottom-right (%d,%d,%d) top-right (%d,%d,%d)\n", a.r, a.g, a.b, b.r, b.g, b.b, c.r, c.g, c.b);
  if (!red(a)) { std::fprintf(stderr, "host_rig: FAIL the red overlay quad is not on the presented picture\n"); ++bad; }
  if (!green(b)) { std::fprintf(stderr, "host_rig: FAIL the textured (green) overlay quad is not on the presented picture\n"); ++bad; }
  if (red(c) || green(c)) { std::fprintf(stderr, "host_rig: FAIL the overlay spilled outside its quads\n"); ++bad; }
  mb::SetList(nullptr);
  if (!read("overlay taken off", rgba, pw, ph)) return 3;
  a = at(rgba, pw, ph, pw / 4, ph / 4), b = at(rgba, pw, ph, 3 * pw / 4, 3 * ph / 4);
  std::printf("overlay off: top-left (%d,%d,%d) bottom-right (%d,%d,%d)\n", a.r, a.g, a.b, b.r, b.g, b.b);
  if (red(a) || green(b)) { std::fprintf(stderr, "host_rig: FAIL the overlay stayed on the picture after SetList(nullptr)\n"); ++bad; }
  // review fix 2: the ImGui texture object is destroyed -> the mailbox frees the renderer's image of id 1 at the next drain (the id leaves the map; a later list naming it falls back to plain white)
  mb::ReleaseTexture(1);
  if (!tick()) return 3;
  // review fix 1: the paint request with no dialog open must be SKIPPED (the census's `overlay requests: skipped`), and a pushed toast must be a request (posted): the same WantPaint / toast clock the app's
  // RequestPaint calls. The planted fault (--plant overlay_never also plants this) treats the toast as never idle (the SDK's always-registered toast made HasDialogs() always true).
  {
    auto& cnt = rdr_scene::swap_once::g_count.overlay_skipped;
    const int64_t t0 = 1'000'000'000'000;
    const bool never_idle = plant == "overlay_never";
    const uint64_t s0 = cnt.load();
    uint32_t wanted = 0;
    for (int i = 0; i < 50; ++i) wanted += mb::WantPaint(true, false, never_idle ? false : mb::ToastIdle(t0), false) ? 1 : 0;
    const uint64_t skipped = cnt.load() - s0;
    std::printf("overlay requests, no dialog open: skipped %llu of 50, wanted %u\n", (unsigned long long)skipped, wanted);
    if (skipped != 50 || wanted != 0) { std::fprintf(stderr, "host_rig: FAIL a paint request was made with no dialog open (skipped %llu of 50)\n", (unsigned long long)skipped); ++bad; }
    mb::ToastPushed(t0);   // a guest thread pushes an achievement
    const bool busy = !mb::ToastIdle(t0 + 1'000'000'000), still = !mb::ToastIdle(t0 + mb::kToastEachNs - 1), over = mb::ToastIdle(t0 + mb::kToastEachNs + 1);
    const bool want = mb::WantPaint(true, false, mb::ToastIdle(t0 + 1'000'000'000), false);
    mb::ToastPushed(t0 + 1'000'000'000);   // a second one queues behind the first
    const bool two = !mb::ToastIdle(t0 + mb::kToastEachNs + 1) && mb::ToastIdle(t0 + 1'000'000'000 + 2 * mb::kToastEachNs + 1);
    const bool shown_asks = mb::WantPaint(true, false, true, true);   // a list still on the screen: one more request (it takes the list off)
    std::printf("overlay toast: busy %d, still busy near its end %d, idle after %d, request while busy %d, two queue %d, request for a shown list %d\n", busy, still, over, want, two, shown_asks);
    if (!busy || !still || !over || !want || !two || !shown_asks) { std::fprintf(stderr, "host_rig: FAIL the toast clock / paint request\n"); ++bad; }
  }
  host.Stop();
  std::printf("overlay result: %s\n", bad ? "FAIL" : "PASS");
  return bad ? 3 : 0;
}

int main(int argc, char** argv) {
  std::string mode, scene, game, out, size_arg, capture, plant, rows_dir;
  std::vector<uint64_t> frames;
  bool missing_input = false;   // S103 ONF: the record has a required input missing (bit 10, the crowd snapshot): the OFF host must still present the 3D frame and count the bit
  uint64_t census_every = 0;   // --mode census --every N
  long long walk_until = -1;    // S103 WB (--walk-until N): records < N carry a Model walk bit (an OURS Scene3D stamp), records >= N carry no Scene3D stamp at all (the game asks no hook in the post-match menu); the rig then checks the verdicts
  bool walk_all = false;        // twin: every record carries the walk bit (the sticky arena counted as walked): the check must FAIL at the menu records
  int group_stamps = 0;      // S103 GSTP (--gpu-group-stamps N): the GPU group stamps inside the colour / frame passes (N = the least draws a segment holds); the renderer's [rdr][gpu][groups] table is logged after the frames (host_rig.py reads it)
  bool gpu_stamps = false;   // S103 GT (--gpu-stamps): the per-pass GPU stamps on, and the bracket / role check after the frames
  bool real = false, no_curve = false, flat = false, once = false, game_pass = false, tween = false, reintern = false, f4_check = false;
  std::string tween_plant;
  bool present_direct = true;   // RIG-DEFAULT: the game's default (wwe2k14_rdr_present_direct true), so every row / shot / GT / WB lane tests direct -> row -> direct; --present-direct 0 is the old copy arm (S103 CHS review fix 2)
  long long rows_only = -1, hold_after = -1, fault_after = -1;   // S103 CHS: --rows-only N (the rows-dir rows go on frame N only), --hold-after N (a no-record tick after frame N), --fault-after N (a planted draw: fault after frame N)
  int fault_kind = 1;            // --fault-kind 1 (the failed draw recorded nothing into `out`) / 2 (it spoiled `out`)
  int direct_plant = 0;          // S103 CHS C6 (--direct-plant N)
  bool stage_stamp = false;      // S103 SVO (--stage-stamp): every posted record carries kSceneStagePlaying (what live_scene.cpp stamps on the guest thread while a stage push plays); --plant stage_ignore = the host ignores the stamp (the twin)
  long long cut_frame = -1;
  static bool no_scissor = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--mode") mode = next();
    else if (a == "--scene") scene = next();
    else if (a == "--game") game = next();
    else if (a == "--out") out = next();
    else if (a == "--frames") frames = ParseList(next());
    else if (a == "--size") size_arg = next();
    else if (a == "--capture") capture = next();
    else if (a == "--plant") plant = next();
    else if (a == "--no-curve") no_curve = true;
    else if (a == "--missing-input") missing_input = true;
    else if (a == "--flat") flat = true;
    else if (a == "--every") census_every = std::strtoull(next().c_str(), nullptr, 10);
    else if (a == "--walk-until") walk_until = std::atoll(next().c_str()), flat = true;
    else if (a == "--walk-all") walk_all = true;
    else if (a == "--once") once = true;
    else if (a == "--game-pass") game_pass = true;   // the GAME's rule for a row whose pass is unknown (pixel-placed rows only; the rest dropped `pass_unknown`) instead of the rig's draw-all
    else if (a == "--no-scissor") no_scissor = true;   // the rows drawn unclipped (what a record with no scissor field gives today)
    else if (a == "--rows-dir") rows_dir = next();
    else if (a == "--stencil-dead") rdr::g_stencil_dead.store(std::atoi(next().c_str()) != 0);   // RIG-ONLY: S103 CHS C5, the rig starts the host without ApplyStartSettings
    else if (a == "--stencil-dead-plant") rdr::g_stencil_dead_plant.store(std::atoi(next().c_str()));   // S103 CHS C5 planted faults (depth_nostencil.h)
    else if (a == "--stencil-dead-poison") rdr::g_stencil_dead_poison.store(true);   // S103 CHS C5: the removed stencil work leaves garbage (depth_nostencil.h)
    else if (a == "--rows-only") rows_only = std::atoll(next().c_str());
    else if (a == "--hold-after") hold_after = std::atoll(next().c_str());
    else if (a == "--fault-after") fault_after = std::atoll(next().c_str());
    else if (a == "--fault-kind") fault_kind = std::atoi(next().c_str());
    else if (a == "--present-direct") present_direct = std::atoi(next().c_str()) != 0;   // S103 CHS C6: the host's present_direct option (a frame with no 2D row presents `out` itself)
    else if (a == "--direct-plant") direct_plant = std::atoi(next().c_str());              // S103 CHS C6 planted faults (scene_host.h TestSeam::direct_plant)
    else if (a == "--stage-stamp") stage_stamp = true;   // S103 SVO
    else if (a == "--real") real = true;   // S103 PN4: the host's pictures from the REAL PictureResolver (forged guest memory, the player's file index) and its movie planes from the plane store (forged display sets), not the capture stand-ins
    else if (a == "--tween") tween = true;
    else if (a == "--gpu-stamps") gpu_stamps = true;
    else if (a == "--gpu-group-stamps") group_stamps = std::max(0, std::atoi(next().c_str()));
    else if (a == "--tween-plant") tween_plant = next();
    else if (a == "--cut") cut_frame = std::atoll(next().c_str());
    else if (a == "--reintern") reintern = true;   // review F1: every object id of the measured pass is re-pointed at a NEW copy of its string, appended in reverse order (other blob offsets, same text)
    else if (a == "--f4") f4_check = true;         // review F4: no map-lookup range is in the own blend set of any vertex variant (no host, no GPU)
  }
  if (tween_plant == "rawid") rdr_scene::g_own_blend_plant = 1;        // planted: the own key takes the raw Object::id (a blob offset)
  else if (tween_plant == "bakepair") rdr_scene::g_own_blend_plant = 2;  // planted: the card bake is tagged like a member
  else if (tween_plant == "lookup") rdr_scene::g_own_blend_plant = 3;    // planted: the shadow / plane lookup matrices stay in the blend set
  if (f4_check) {   // review F4: for every vertex variant, the own blend ranges (block of 8192 bytes) must not cover a map-lookup field of the variant's table
    using namespace rdr_scene;
    uint32_t variants = 0, lookup_fields = 0, lookup_blendable = 0, lookup_in_set = 0, ranges_total = 0;
    for (size_t vi = 0; vi < sizeof(rdr::fx::kVariants) / sizeof(rdr::fx::kVariants[0]); ++vi) {
      const rdr::fx::Variant& v = rdr::fx::kVariants[vi];
      if (!v.name) continue;
      const fxt::VariantTable* table = nullptr;
      for (const fxt::VariantTable& t : fxt::kTables) if (std::strcmp(t.variant, v.name) == 0) { table = &t; break; }
      if (!table) continue;
      ++variants;
      rdr::BlendRange ranges[16];
      const uint32_t count = OwnBlendRanges(v, int(vi), 8192, ranges);
      ranges_total += count;
      for (uint32_t k = 0; k < table->field_count; ++k) {
        if (!OwnBlendIsMapLookup(table->fields[k].name)) continue;
        ++lookup_fields;
        const uint32_t off = table->fields[k].offset;
        for (uint32_t r = 0; r < v.constant_count; ++r)
          if (v.constants[r].blend && v.constants[r].offset <= off && off < v.constants[r].offset + uint32_t(v.constants[r].count) * 16u) { ++lookup_blendable; break; }
        for (uint32_t r = 0; r < count; ++r) if (ranges[r].offset <= off && off < ranges[r].offset + ranges[r].bytes) { ++lookup_in_set; break; }
      }
    }
    std::printf("own_blend F4: %u vertex variants, %u own ranges, %u map-lookup fields of which %u are marked blendable by the shader build, %u of those are in the own blend set\n", variants, ranges_total, lookup_fields, lookup_blendable, lookup_in_set);
    return lookup_in_set ? 1 : 0;
  }
  if (mode == "sizes") return SizesMode(capture);

  if (mode == "overlay") {   // S103 AIM: the ImGui overlay path of the stream-free host (no capture, no game files)
    uint32_t ow = 640, oh = 360;
    if (!size_arg.empty()) std::sscanf(size_arg.c_str(), "%ux%u", &ow, &oh);
    return OverlayMode(plant, ow, oh);
  }
  if (mode == "census") {   // CVO: per record facts (no GPU): objects by id prefix, asset pacs, stamps, 2D rows, particles
    for (uint64_t n = 0; n < (frames.empty() ? 1000000ull : 1000000ull); ++n) {
      char name[48];
      std::snprintf(name, sizeof name, "frame_%06llu.rsc", (unsigned long long)n);
      bool want = frames.empty() ? (census_every ? n % census_every == 0 : true) : std::find(frames.begin(), frames.end(), n) != frames.end();
      if (!want) { if (frames.empty() && !std::filesystem::exists(std::filesystem::path(scene) / name)) break; if (!frames.empty() && n > *std::max_element(frames.begin(), frames.end())) break; continue; }
      if (!std::filesystem::exists(std::filesystem::path(scene) / name)) { std::printf("rec %llu MISSING\n", (unsigned long long)n); continue; }
      rdr_scene::SceneTables t;
      std::string e;
      if (!rdr_scene::ReadRecord((std::filesystem::path(scene) / name).string(), t, e)) { std::printf("rec %llu ERR %s\n", (unsigned long long)n, e.c_str()); continue; }
      std::map<std::string, int> pre, pacs;
      for (const auto& o : t.objects) {
        std::string id(t.Str(o.id));
        size_t c = id.find(':');
        if (c != std::string::npos) { size_t c2 = id.find(':', c + 1); if (id.compare(0, c, "arena") == 0 || id.compare(0, c, "actor") == 0) id = id.substr(0, c2 == std::string::npos ? id.size() : c2); else id = id.substr(0, c); }
        if (id.size() > 24) id = id.substr(0, 24);
        ++pre[id];
        if (o.asset < t.assets.size()) { std::string p(t.Str(t.assets[o.asset].pac)); ++pacs[p]; }
      }
      std::printf("rec %llu gf %llu obj %zu cam %d lights %zu sprites %zu texts %zu movies %zu targets %zu particles %zu batches %zu quads %zu holes %d worlds %zu stagefx %zu stamps %zu inputs %zu engobj %zu | ids:", (unsigned long long)n, (unsigned long long)t.header.guest_frame, t.objects.size(), t.has_camera ? 1 : 0, t.lights.size(), t.sprites.size(), t.texts.size(), t.movies.size(), t.targets.size(), t.particles.particles.size(), t.particles.batches.size(), t.quads2d.size(), t.quad2d_holes == rdr_scene::kQuad2DNotTaken ? -1 : int(t.quad2d_holes), t.arena_worlds.size(), t.stage_fx.size(), t.owned_passes.size(), t.input_stamps.size(), t.engine_objects.size());
      for (auto& kv : pre) std::printf(" %s=%d", kv.first.c_str(), kv.second);
      std::printf(" | pacs:");
      int k = 0;
      for (auto& kv : pacs) { if (k++ >= 12) break; std::printf(" %s=%d", kv.first.c_str(), kv.second); }
      std::printf(" | stamps:");
      for (const auto& s : t.owned_passes) std::printf(" p%u/d%u/w%u/k%u", s.pass, s.decision, s.why_not, s.pad);
      for (const auto& m : t.movies) std::printf(" movie=%s", std::string(t.Str(t.assets[m.asset].pac)).c_str());
      std::printf("\n");
    }
    return 0;
  }
  if (mode == "samplers") {   // the sampler state the STREAM bound for every 2D row-site draw of capture frames --frames (the rows carry none: REMNANT_MAP 10.3), counted by state
    rdr_adapter::CaptureReader reader;
    if (!reader.Open(capture)) return 2;
    for (uint64_t n : frames) {
      rdr_adapter::LoadedFrame f;
      if (!reader.LoadFrame(uint32_t(n), f)) continue;
      std::map<std::string, std::pair<uint32_t, std::string>> by;
      for (uint32_t d = 0; d < f.draws.count; ++d) {
        const rdr_adapter::DrawOrigin* o = f.OriginOf(d);
        if (!o || o->op != 19) continue;
        const rdr_adapter::Draw& dr = f.draws[d];
        for (uint32_t t = 0; t < dr.texture_count && dr.texture_first + t < f.texture_slots.count; ++t) {
          const rdr_adapter::TextureSlot& ts = f.texture_slots[dr.texture_first + t];
          if (ts.slot != 0 || ts.sampler >= f.samplers.count) continue;
          const rdr::SamplerDesc& s = f.samplers[ts.sampler];
          const rdr_adapter::ImageRecord* im = reader.FindImage(ts.image);
          const rdr_adapter::DrawDynamic& dd = f.dynamics[dr.dynamic];
          char b[360];
          std::snprintf(b, sizeof b, "scissor %d,%d %ux%u ndc scale %.4f %.4f %.4f offset %.5f %.5f %.5f | ", int(dd.host.scissor.x), int(dd.host.scissor.y), unsigned(dd.host.scissor.width), unsigned(dd.host.scissor.height), dd.ndc_scale[0], dd.ndc_scale[1], dd.ndc_scale[2], dd.ndc_offset[0], dd.ndc_offset[1], dd.ndc_offset[2]);
          std::snprintf(b + std::strlen(b), sizeof b - std::strlen(b), "site %08X mag %u min %u mip %u addr %u,%u aniso %u lod_bias %.2f lod [%.1f %.1f] image %ux%u mips %u fmt %u", o->call_site, s.mag_filter, s.min_filter, s.mip_mode, s.address_u, s.address_v, s.max_anisotropy, s.lod_bias, s.min_lod, s.max_lod,
                        im ? im->width : 0, im ? im->height : 0, im ? im->mips : 0, im ? im->format : 0);
          auto& e = by[b];
          ++e.first;
        }
      }
      for (const auto& kv : by) std::printf("frame %llu: %u x %s\n", (unsigned long long)n, kv.second.first, kv.first.c_str());
    }
    return 0;
  }
  if (mode == "rows") {   // every record of a scene folder: its 2D rows by kind / pass, the tap's hole count (NotTaken = the record was written without the 2D tap)
    uint64_t recs = 0, with_rows = 0, not_taken = 0, total = 0;
    for (uint64_t n = 0;; ++n) {
      rdr_scene::SceneTables t;
      std::string e;
      char name[48];
      std::snprintf(name, sizeof name, "frame_%06llu.rsc", (unsigned long long)n);
      if (!std::filesystem::exists(std::filesystem::path(scene) / name)) break;
      if (!rdr_scene::ReadRecord((std::filesystem::path(scene) / name).string(), t, e)) { std::printf("record %llu: %s\n", (unsigned long long)n, e.c_str()); continue; }
      ++recs;
      if (t.quad2d_holes == rdr_scene::kQuad2DNotTaken) ++not_taken;
      if (!t.quads2d.empty()) {
        ++with_rows, total += t.quads2d.size();
        unsigned kinds[8] = {}, passes[8] = {};
        for (const auto& r : t.quads2d) ++kinds[r.kind & 7], ++passes[r.pass & 7];
        std::printf("record %llu: %zu rows (rect %u sprite %u glyph %u grad %u yuv %u yuva %u), pass unknown %u final %u other %u, holes %u\n", (unsigned long long)n, t.quads2d.size(), kinds[1], kinds[2], kinds[3], kinds[4], kinds[5], kinds[6], passes[0], passes[1],
                    passes[2] + passes[3] + passes[4], t.quad2d_holes == rdr_scene::kQuad2DNotTaken ? 0u : t.quad2d_holes);
      }
    }
    std::printf("rows: %llu records, %llu with rows (%llu rows), %llu NotTaken\n", (unsigned long long)recs, (unsigned long long)with_rows, (unsigned long long)total, (unsigned long long)not_taken);
    return 0;
  }
  if (mode == "resolver") {   // S103 PN4F: the picture resolver's lifecycle twins (resolver_unit.h) over one capture frame's pictures; --scene --capture --rows-dir --frames N --game [--plant NAME]
    if (scene.empty() || capture.empty() || rows_dir.empty() || game.empty() || frames.empty()) { std::fprintf(stderr, "host_rig --mode resolver needs --scene --capture --rows-dir --frames --game\n"); return 1; }
    const uint64_t n = frames.front();
    std::string e;
    auto rec = std::make_shared<rdr_scene::SceneTables>();
    if (!LoadRecord(scene, n, *rec, e)) { std::fprintf(stderr, "host_rig: FAIL record %llu: %s\n", (unsigned long long)n, e.c_str()); return 2; }
    rdr_scene::SceneTables rt;
    const std::string rp = (std::filesystem::path(rows_dir) / ("r" + std::to_string((unsigned long long)n)) / "rows.rsc").string();
    if (!rdr_scene::ReadRecord(rp, rt, e)) { std::fprintf(stderr, "host_rig: FAIL rows %s: %s\n", rp.c_str(), e.c_str()); return 2; }
    rec->quads2d = rt.quads2d, rec->quad2d_holes = 0;
    rdr::RendererOptions ro;
    ro.present_offscreen_w = 1280, ro.present_offscreen_h = 720, ro.vsync = false;
    auto renderer = rdr::Renderer::Create(ro, e);
    if (!renderer) { std::fprintf(stderr, "host_rig: FAIL renderer: %s\n", e.c_str()); return 3; }
    host_rig_2d::CapSource cap;
    if (!cap.Open(capture)) { std::fprintf(stderr, "host_rig: cannot open capture %s\n", capture.c_str()); return 2; }
    cap.renderer = renderer.get();
    if (!cap.Prepare(uint32_t(n), *rec)) { std::fprintf(stderr, "host_rig: FAIL capture frame %llu\n", (unsigned long long)n); return 2; }
    resolver_unit::Env env;
    env.renderer = renderer.get(), env.cap = &cap, env.rec = rec.get(), env.game = game;
    const char* cache_env = std::getenv("WWE2K14_UI_INDEX_CACHE");
    env.cache = cache_env ? cache_env : "";
    const int rc = resolver_unit::Run(env, plant);
    renderer->WaitIdle();
    return rc;
  }
  if ((mode != "host" && mode != "ref") || scene.empty() || game.empty() || out.empty() || frames.empty()) {
    std::fprintf(stderr, "usage: host_rig --mode host|ref|sizes|rows --scene DIR --game DIR --out DIR --frames A,B [--size WxH] [--no-curve] [--plant NAME] | --mode sizes --capture DIR\n");
    return 1;
  }
  uint32_t w = 1280, h = 720;
  if (!size_arg.empty()) std::sscanf(size_arg.c_str(), "%ux%u", &w, &h);
  std::filesystem::create_directories(out);
  // RIG-ONLY: the fixture needs the dev integration switch (the game sets it only in a +dev build from --wwe2k14_rdr_scene_force_inputs, in ApplyStartSettings)
  scene_frame::g_force_inputs.store(true, std::memory_order_relaxed);   // fixture: holes / missing post rows do not block READY (the dev integration switch); the freshness check is skipped by the host's seam
  const std::vector<uint8_t> curve = no_curve ? std::vector<uint8_t>() : Curve();
  int bad = 0;

  if (mode == "ref") {
    rdr::RendererOptions ro;
    std::string err;
    auto renderer = rdr::Renderer::Create(ro, err);
    if (!renderer) { std::fprintf(stderr, "host_rig: FAIL renderer: %s\n", err.c_str()); return 3; }
    rdr_scene::LiveSceneDraw draw(*renderer);
    draw.SetGameRoot(game);
    for (uint64_t n : frames) {
      rdr_scene::SceneTables rec;
      if (!LoadRecord(scene, n, rec, err)) { std::fprintf(stderr, "host_rig: FAIL record %llu: %s\n", (unsigned long long)n, err.c_str()); ++bad; continue; }
      std::string report;
      char name[64];
      std::snprintf(name, sizeof name, "ref_%llu.png", (unsigned long long)n);
      const bool ok = draw.DrawToPng(rec, w, h, curve, std::filesystem::path(out) / name, report);
      std::printf("ref frame %llu: %s %s\n", (unsigned long long)n, ok ? "ok" : "FAILED", report.c_str());
      if (!ok) ++bad;
    }
    renderer->WaitIdle();
    return bad ? 3 : 0;
  }

  // ---- host mode ----------------------------------------------------------------------------------------------------------------------------------------
  sh::HostOptions ho;
  ho.renderer.present_offscreen_w = w, ho.renderer.present_offscreen_h = h;
  ho.renderer.vsync = false;
  ho.game_root = game;
  ho.tween_wanted = tween;
  host_rig_real::Real real_src;   // S103 PN4 (--real)
  real_src.plant = plant;
  if (real) {
    const char* cache_env = std::getenv("WWE2K14_UI_INDEX_CACHE");
    if (!real_src.BuildIndex(game, cache_env ? cache_env : "")) { std::fprintf(stderr, "host_rig: FAIL the file index\n"); return 2; }
    static uint32_t last_key = 0;
    ho.picture = [&real_src](const rdr_scene::Quad2D& row) {   // HostOptions::picture = resolver->Function(), with the two planted faults (the resolver does not exist until the host's renderer does: the pointer is read per call)
      if (!real_src.resolver || real_src.plant == "resolver_null") return rdr::TextureSlot{};   // twin: no resolver answers: every textured row is dropped `picture`
      if (real_src.plant == "wrong_entry" && last_key && last_key != row.tex_key[0] && (row.tex_key[0] & 1)) {   // twin: an odd key gets the previous key's picture (a wrong entry)
        rdr_scene::Quad2D other = row;
        other.tex_key[0] = last_key;
        return real_src.resolver->Resolve(other);
      }
      last_key = row.tex_key[0];
      return real_src.resolver->Resolve(row);
    };
    ho.frame_done = [&real_src](uint64_t f) { if (real_src.resolver) real_src.resolver->EndFrame(f); };
    // (the movie twins no_flush / swap_chroma are in FeedMovies)
  }
  if (plant == "gpu_unbracketed") gpu_stamps = true, ho.renderer.gpu_stamp_plant = 8;
  else if (plant == "gpu_overflow") gpu_stamps = true, ho.renderer.gpu_stamp_plant = 9;   // S103 GT twin: a stamp pool too small for the frame
  else if (plant == "gpu_ident") gpu_stamps = true, ho.renderer.gpu_stamp_plant = 10;      // twin: a stamp written outside the counted sites
  else if (plant == "gpu_unnamed") gpu_stamps = true, ho.renderer.gpu_stamp_plant = 11;    // twin: no image is given a role
  else if (plant == "gpu_work_csv") gpu_stamps = true, ho.renderer.gpu_stamp_plant = 12;   // twin: every other frame is left out of the role sums
  else if (plant == "gpu_off") gpu_stamps = true;                                          // twin: the stamps are not switched on   // S103 GT planted twin: one step of every frame has no stamp of its own (the check must FAIL)
  if (plant == "group_chain") group_stamps = std::max(group_stamps, 1), rdr::g_gpu_group_plant.store(1);   // S103 GSTP twin: a group stamp is written but its segment is not recorded (a boundary dropped from the chain): "chain broken" > 0
  else if (plant == "group_cover") group_stamps = std::max(group_stamps, 1), rdr::g_gpu_group_plant.store(2);   // twin: a draw is left out of the segment counts: "coverage short" > 0
  else if (plant == "group_nolabel") group_stamps = std::max(group_stamps, 1), rdr::g_gpu_group_plant.store(3);   // twin: the scene front-end does not label its model draws (front_end.cpp emit): too few named groups
  if (group_stamps) {   // S103 GSTP: what ApplyStartSettings does in the game (the timestamp pool, the per-step stamps the group stamps chain between, the group min run)
    ho.renderer.gpu_timestamps = true;
    rdr::g_gpu_stamps.store(true), rdr::g_gpu_pass_stamps.store(true), rdr::g_gpu_group_stamps.store(std::min(group_stamps, 255));   // RIG-ONLY: the rig starts the host without ApplyStartSettings
  }
  if (gpu_stamps) {   // S103 GT: what ApplyStartSettings does in the game (gpu_timestamps + the per-step stamps)
    ho.renderer.gpu_timestamps = true;
    if (plant != "gpu_off") rdr::g_gpu_stamps.store(true), rdr::g_gpu_pass_stamps.store(true);   // RIG-ONLY: the rig starts the host without ApplyStartSettings
  }
  if (plant == "skip_copy") present_direct = false;   // the skip_copy twin plants the COPY (the direct arm makes none): it must run the copy arm (S103 CHS: P7H1 caught nothing once the rig defaulted to direct)
  ho.present_direct = present_direct;   // S103 CHS C6 (RIG-ONLY: the rig starts the host without ApplyStartSettings; default off = the copy as before)
  ho.draw_unknown_pass = !game_pass;   // rig only (unless --game-pass): the recorded rows carry no pass (every row reads Unknown) and the stream's own 2D arm draws them all; the game draws an Unknown row only when it is pixel-placed (P7 review (c))
  sh::SceneHost host;
  std::string err;
  if (!host.Start(ho, err)) { std::fprintf(stderr, "host_rig: FAIL host: %s\n", err.c_str()); return 3; }
  if (real) real_src.resolver = std::make_unique<rdr_scene::PictureResolver>(*host.renderer(), real_src.source, real_src.index, game);
  sh::TestSeam seam;
  seam.force_ours = !flat;
  seam.skip_copy = plant == "skip_copy";
  seam.direct_plant = direct_plant;
  if (missing_input || plant == "stale_walks") seam.inject_stale = 1u << 10;   // S103 ONF: bit 10 (crowd_snapshot) reads stale on every record
  seam.stale_walks = plant == "stale_walks";   // planted twin: the old rule (a stale input stops the frame): no 3D is ever presented
  seam.shot_no_curve = plant == "shot_no_curve";   // S103 lane SHOT twins: the capture without the display curve / one answer late (the capture check must fail)
  seam.shot_late = plant == "shot_late";
  seam.rows_first = plant == "rows_first";
  seam.rows_never = plant == "rows_never";
  seam.stage_plant = plant == "stage_ignore";   // S103 SVO twin: a stamped record still draws its rows
  host_rig_2d::CapSource cap2d;
  const bool have_cap = !capture.empty();
  if (have_cap) {
    if (!cap2d.Open(capture)) { std::fprintf(stderr, "host_rig: cannot open capture %s\n", capture.c_str()); return 2; }
    cap2d.renderer = host.renderer();
    cap2d.stale_planes = plant == "stale_planes";
    if (!real) seam.picture = [&cap2d](const rdr_scene::Quad2D& row) { return cap2d.Picture(row); };
    seam.scissor = [&cap2d](const rdr_scene::Quad2D& row, rdr::Rect& out) { return !no_scissor && cap2d.Scissor(row, out); };
    if (!real) seam.plane = [&cap2d](const rdr_scene::Quad2D& row, int slot) { return cap2d.Plane(row, slot); };   // (--real: the host's own PlaneOf over the plane store)
  }
  if (plant == "no_picture") {   // R8 proof: no resolver answers (a key nothing resolves, a movie with no planes): those rows are dropped one by one, counted by reason, the rest is drawn
    seam.picture = [](const rdr_scene::Quad2D&) { return rdr::TextureSlot{}; };
    seam.plane = [](const rdr_scene::Quad2D&, int) { return rdr::TextureSlot{}; };
  }
  KeptTick kept;
  TickInfo info;
  if (tween) {
    seam.tween_alpha = tween_plant == "alpha0" ? 0.0f : 0.5f;
    seam.tween_no_forget = tween_plant == "no_forget";
    seam.tween_swap = tween_plant == "swap";
    seam.on_tick = [&](const rdr::Frame& built, const rdr::Frame* half, uint32_t matched) {
      info = TickInfo{};
      info.seen = true;
      if (half) {
        info.half = true, info.matched = matched;
        std::unordered_map<uint64_t, uint32_t> by_key;
        for (uint32_t i = 0; i < uint32_t(kept.draws.size()); ++i) {
          const uint64_t key = uint64_t(kept.draws[i].blend_key_high) << 32 | kept.draws[i].blend_key_low;
          if (key && kept.draws[i].blend_count) by_key.emplace(key, i);
        }
        std::vector<uint8_t> covered(built.constants.count, 0);
        for (uint32_t i = 0; i < built.draws.count; ++i) {
          const rdr::DrawOp& d = built.draws[i];
          const uint64_t key = uint64_t(d.blend_key_high) << 32 | d.blend_key_low;
          if (!key || !d.blend_count) continue;
          ++info.keyed;
          const auto f = by_key.find(key);
          if (!kept.valid || f == by_key.end()) continue;
          const rdr::DrawOp& before = kept.draws[f->second];
          if (before.blend_count != d.blend_count) continue;
          ++info.pairs;
          for (uint32_t r = 0; r < d.blend_count; ++r) {
            const rdr::BlendRange& rg = built.blend_ranges[d.blend_first + r];
            const rdr::BlendRange& rb = kept.ranges[before.blend_first + r];
            if (rg.bytes != rb.bytes || rg.offset != rb.offset) continue;
            if (size_t(before.vs_constants) + rb.offset + rb.bytes > kept.constants.size() || size_t(d.vs_constants) + rg.offset + rg.bytes > built.constants.count || built.constants.count != half->constants.count) continue;
            for (uint32_t c = 0; c < rg.bytes / 4; ++c) {
              float from, to, got;
              std::memcpy(&from, kept.constants.data() + before.vs_constants + rb.offset + 4 * c, 4);
              std::memcpy(&to, built.constants.data + d.vs_constants + rg.offset + 4 * c, 4);
              std::memcpy(&got, half->constants.data + d.vs_constants + rg.offset + 4 * c, 4);
              const float want = from + (to - from) * 0.5f;
              const double err = std::fabs(double(got) - double(want));
              info.max_err = std::max(info.max_err, err);
              ++info.floats;
              if (from != to) ++info.moving;
              if (!(err <= 1e-6 * (1.0 + std::fabs(double(want))))) ++info.bad_floats;
            }
            for (uint32_t b = 0; b < rg.bytes; ++b) covered[d.vs_constants + rg.offset + b] = 1;
          }
        }
        if (half->constants.count == built.constants.count)
          for (uint32_t b = 0; b < built.constants.count; ++b)
            if (!covered[b] && half->constants.data[b] != built.constants.data[b]) ++info.bad_outside;
        host.ReadPresent(info.half_rgba, info.hw, info.hh);   // the halfway picture, still the one on the present (the tick's own redraw follows)
      }
      kept.draws.assign(built.draws.data, built.draws.data + built.draws.count);
      kept.ranges.assign(built.blend_ranges.data, built.blend_ranges.data + built.blend_ranges.count);
      kept.constants.assign(built.constants.data, built.constants.data + built.constants.count);
      kept.valid = true;
    };
  }
  host.SetTestSeam(seam);
  uint64_t tot_seen = 0, tot_drawn = 0, tot_dropped[sh::kDrop2DCount] = {};
  uint64_t posted = 0, real_frames = 0, real_pend_frames = 0, real_pend_posts = 0, real_worst_posts = 0;
  // S103 GSTP (review fix 4): every WaitHandled return is CHECKED (lint tools\scene\gstp_lint.py): a host that does not answer fails loudly, naming the tick and the place; the post loop has a deadline and a heartbeat.
  const auto rig_start = std::chrono::steady_clock::now();
  const long long deadline_s = std::getenv("WWE_RIG_DEADLINE_S") ? std::atoll(std::getenv("WWE_RIG_DEADLINE_S")) : 3000;   // gstp_check sets its own
  uint64_t heartbeat_posts = 0;
  const auto wait_tick = [&](const char* where) -> bool {   // false = the host did not handle the tick: said once, the caller returns 3
    if (host.WaitHandled(posted, std::chrono::milliseconds(60000))) return true;
    std::fprintf(stderr, "host_rig: FAIL the host did not handle tick %llu (%s) within 60 s\n", (unsigned long long)posted, where);
    return false;
  };
  const auto past_deadline = [&](const char* where, uint64_t frame) -> bool {
    const long long elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - rig_start).count();
    if (elapsed <= deadline_s) return false;
    std::fprintf(stderr, "host_rig: FAIL deadline %lld s exceeded after %lld s at tick %llu, record %llu (%s)\n", deadline_s, elapsed, (unsigned long long)posted, (unsigned long long)frame, where);
    return true;
  };
  bool curve_sent = false;
  std::shared_ptr<const rdr_scene::SceneTables> prev;
  std::vector<uint8_t> prev_final;   // --tween: the picture the previous tick ended on
  for (int pass = 0; pass < (tween ? 2 : 1); ++pass)
  for (uint64_t n : frames) {
    auto rec = std::make_shared<rdr_scene::SceneTables>();
    if (!LoadRecord(scene, n, *rec, err)) { std::fprintf(stderr, "host_rig: FAIL record %llu: %s\n", (unsigned long long)n, err.c_str()); ++bad; continue; }
    if (!rows_dir.empty() && (rows_only < 0 || uint64_t(rows_only) == n)) {   // the 2D rows of this frame, written by `fview --quads-2d-rows --quads-2d-dump <dir>\r<n>` (the captured records carry none: they were written before the 2D tap): the record's rows become these
      rdr_scene::SceneTables rt;
      const std::string rp = (std::filesystem::path(rows_dir) / ("r" + std::to_string((unsigned long long)n)) / "rows.rsc").string();
      if (!rdr_scene::ReadRecord(rp, rt, err)) { std::fprintf(stderr, "host_rig: FAIL rows %s: %s\n", rp.c_str(), err.c_str()); ++bad; continue; }
      rec->quads2d = rt.quads2d, rec->quad2d_holes = 0;
    }
    if (walk_until >= 0) {   // S103 WB: inject the stamps the stream-era capture lacks (the game's own: a Model / Crowd / Ring ask makes an OURS stamp with the walk bit; no ask = no stamp)
      rec->owned_passes.erase(std::remove_if(rec->owned_passes.begin(), rec->owned_passes.end(), [](const rdr_scene::OwnedStamp& s) { return s.pass == uint8_t(rdr_own::Pass::Scene3D); }), rec->owned_passes.end());
      if (walk_all || n < uint64_t(walk_until)) {
        rdr_scene::OwnedStamp st;
        st.pass = uint8_t(rdr_own::Pass::Scene3D), st.decision = uint8_t(rdr_own::Decision::Ours), st.why_not = 0, st.pad = scene_kind::kWalkBit, st.inputs_mask = 0;
        st.guest_frame = rec->header.guest_frame >= 1 ? rec->header.guest_frame - 1 : 0;   // the hook frame of this record (scene_frame.h HookFrameOf)
        rec->owned_passes.push_back(st);
      }
    }
    if (cut_frame >= 0 && uint64_t(cut_frame) == n && rec->has_camera) rec->camera.cut = 1;   // --cut: a camera cut on this record
    if (stage_stamp) rec->header.flags |= rdr_scene::kSceneStagePlaying;   // S103 SVO: the guest thread's stamp (live_scene.cpp), set here on the record the host is posted
    if (reintern && pass == 1) {   // review F1: same object names, other string-blob offsets (the pooled live record's offsets move between ticks): each id gets its own fresh copy of its text, appended in reverse order
      rec->strings.append(size_t(n * 7919u % 251u) + 1u, 'x'), rec->strings.push_back('\0');   // a decoy of a tick-dependent length first: two ticks never land on the same offsets by chance
      for (size_t oi = rec->objects.size(); oi-- > 0;) {
        const std::string text(rec->Str(rec->objects[oi].id));
        if (text.empty()) continue;
        rec->objects[oi].id = uint32_t(rec->strings.size() + 1);
        rec->strings.append(text);
        rec->strings.push_back('\0');
      }
    }
    std::shared_ptr<const rdr_scene::SceneTables> shown = rec;
    if (plant == "stale_record" && prev) shown = prev;   // planted: the previous frame's record
    // The assets load on the loader thread: the same tick is posted again until the host drew the frame (a real run misses and repeats the same way); a count of the misses is reported.
    bool drawn = false;
    int tries = 0;
    if (have_cap && !cap2d.Prepare(uint32_t(n), *rec)) { std::fprintf(stderr, "host_rig: FAIL capture frame %llu\n", (unsigned long long)n); ++bad; continue; }
    if (real && have_cap && pass == 0) real_src.FeedPictures(cap2d, *rec, n), real_src.FeedMovies(cap2d, *rec, n);   // the game's side: the rows' first sight (births, Seen) and the movie present's plane flush, before the host draws the record
    uint64_t seen0 = 0, drawn0 = 0, dropped0[sh::kDrop2DCount] = {};
    uint64_t pending0 = real && real_src.resolver ? real_src.resolver->stats().pending : 0;
    for (; tries < 1500 && !drawn; ++tries) {
      seen0 = host.counters().rows_seen, drawn0 = host.counters().rows_drawn;
      for (unsigned k = 0; k < sh::kDrop2DCount; ++k) dropped0[k] = host.counters().rows_dropped[k];
      sh::Msg m;
      m.guest_frame = rec->header.guest_frame;
      info = TickInfo{};
      m.width = plant == "wrong_size" ? w - 1 : w, m.height = h;
      m.record = shown;
      if (!curve_sent && !curve.empty()) { m.curve_changed = true, std::memcpy(m.curve.data(), curve.data(), 1024); curve_sent = true; }
      host.Post(std::move(m));
      ++posted;
      if (!wait_tick("the post loop")) return 3;
      if (++heartbeat_posts % 25 == 0) std::fprintf(stderr, "host_rig: heartbeat: %llu posts, record %llu, try %d, %lld s\n", (unsigned long long)posted, (unsigned long long)n, tries, (long long)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - rig_start).count());
      if (past_deadline("the post loop", n)) return 3;
      if (walk_until >= 0) drawn = n >= uint64_t(walk_until) ? tries >= 1 : host.LastSource() == sh::Source::Draw3D;   // S103 WB: a walked record waits for its 3D (the assets load on the loader thread); a menu record has no 3D to wait for: two posts (the first builds, the second decides on the built record)
      else drawn = host.LastSource() == sh::Source::Draw3D || (flat && host.LastSource() == sh::Source::Black) || once;   // --once: one post per frame, whatever the host showed (the rows count of a 3D frame)
      if (real && real_src.resolver && real_src.resolver->stats().pending > pending0) drawn = false;   // --real: a row dropped `pending` (the resolver's per-frame load budget) is retried on the next frame, as in a run: the same tick is posted again
      if (real && real_src.resolver) pending0 = real_src.resolver->stats().pending;
      if (!drawn) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (drawn && host.LastSource() == sh::Source::Draw3D) {   // S103 SCX (SCG.md): the stage screens' grab is the PREVIOUS draw's scene (the game's own timing, a gap keeps it): draw this record once more, same guest frame, so the grab is this record's own scene = the mirror's seed rule (DrawToPng)
      seen0 = host.counters().rows_seen, drawn0 = host.counters().rows_drawn;
      for (unsigned k = 0; k < sh::kDrop2DCount; ++k) dropped0[k] = host.counters().rows_dropped[k];
      sh::Msg m;
      m.guest_frame = rec->header.guest_frame;
      info = TickInfo{};
      m.width = plant == "wrong_size" ? w - 1 : w, m.height = h;
      m.record = shown;
      host.Post(std::move(m));
      ++posted;
      if (!wait_tick("the grab re-post")) return 3;
    }
    prev = rec;
    if (real && pass == 0) { ++real_frames; if (tries > 1) ++real_pend_frames, real_pend_posts += uint64_t(tries - 1); real_worst_posts = std::max<uint64_t>(real_worst_posts, uint64_t(tries)); }   // PN4F: posts beyond the first = the host drew a frame with pictures still arriving (rows `pending`)
    // the rows of THIS frame: the last post's own counts (an earlier post that missed its assets drew rows too, over a held picture; those are not the frame's)
    const sh::Counters& cn = host.counters();
    std::string by;
    for (unsigned k = 1; k < sh::kDrop2DCount; ++k)
      if (k != sh::kDropHole && cn.rows_dropped[k] > dropped0[k]) by += std::string(by.empty() ? "" : " ") + sh::Drop2DName(k) + " " + std::to_string((unsigned long long)(cn.rows_dropped[k] - dropped0[k])), tot_dropped[k] += cn.rows_dropped[k] - dropped0[k];
    tot_seen += cn.rows_seen - seen0, tot_drawn += cn.rows_drawn - drawn0;
    std::printf("rows frame %llu: %llu rows (%zu in the record, holes %u), drawn %llu, not drawn %s\n", (unsigned long long)n, (unsigned long long)(cn.rows_seen - seen0), rec->quads2d.size(), rec->quad2d_holes == rdr_scene::kQuad2DNotTaken ? 0u : rec->quad2d_holes,
                (unsigned long long)(cn.rows_drawn - drawn0), by.empty() ? "-" : by.c_str());
    std::vector<uint8_t> rgba;
    uint32_t pw = 0, ph = 0;
    if (!host.ReadPresent(rgba, pw, ph)) { std::fprintf(stderr, "host_rig: FAIL no presented picture for frame %llu\n", (unsigned long long)n); ++bad; continue; }
    {   // S103 lane SHOT (src\shown_frame.h): the on-request readback of what the host presented (F through the display table) must equal the presented picture (RGB; the curve here is not the identity)
      std::vector<uint8_t> shot;
      uint32_t sw = 0, sh_ = 0;
      const bool got = host.CaptureShown(shot, sw, sh_, 5000);
      int maxd = -1;
      if (got && sw == pw && sh_ == ph && shot.size() == rgba.size() && rgba.size() >= size_t(pw) * ph * 4) {
        maxd = 0;
        for (size_t i = 0; i + 2 < rgba.size(); i += 4)
          for (int c3 = 0; c3 < 3; ++c3) maxd = std::max(maxd, std::abs(int(rgba[i + c3]) - int(shot[i + c3])));
      }
      std::printf("shot frame %llu: captured %s %ux%u (present %ux%u), max RGB diff %d\n", (unsigned long long)n, got ? "yes" : "NO", sw, sh_, pw, ph, maxd);
      if (maxd < 0 || maxd > 1) { std::fprintf(stderr, "host_rig: FAIL CaptureShown differs from the presented picture (frame %llu)\n", (unsigned long long)n); ++bad; }
    }
    if (!tween && !real && plant.empty()) {   // S103 lane SHOT (review F1 / F4): three callers at once while a frame is about to render: the queued (no-drain) readback answers them all with that frame's picture
      struct Got { bool ok = false; std::vector<uint8_t> px; uint32_t w = 0, h = 0; uint64_t frame = 0; } got[3];
      std::vector<std::thread> asks;
      for (int k = 0; k < 3; ++k) asks.emplace_back([&host, &got, k] { got[k].ok = host.CaptureShown(got[k].px, got[k].w, got[k].h, 8000, &got[k].frame); });
      std::this_thread::sleep_for(std::chrono::milliseconds(30));   // (shorter than the host's 100 ms idle poll: the drain does not serve them)
      sh::Msg again;
      again.guest_frame = rec->header.guest_frame;
      again.width = w, again.height = h;
      again.record = shown;
      host.Post(std::move(again));
      ++posted;
      const bool waited = wait_tick("the three-caller readback");
      for (std::thread& t : asks) t.join();
      if (!waited) return 3;
      std::vector<uint8_t> after;
      uint32_t aw = 0, ah = 0;
      host.ReadPresent(after, aw, ah);
      int worst = 0, served_by_frame = 0;
      for (int k = 0; k < 3; ++k) {
        int d = -1;
        if (got[k].ok && got[k].w == aw && got[k].h == ah && got[k].px.size() == after.size()) {
          d = 0;
          for (size_t i = 0; i + 2 < after.size(); i += 4)
            for (int c3 = 0; c3 < 3; ++c3) d = std::max(d, std::abs(int(after[i + c3]) - int(got[k].px[i + c3])));
        }
        if (got[k].frame == rec->header.guest_frame) ++served_by_frame;
        worst = d < 0 ? 999 : std::max(worst, d);
      }
      std::printf("shot frame %llu: 3 concurrent callers, worst RGB diff %d, %d served by the queued readback\n", (unsigned long long)n, worst, served_by_frame);
      if (worst > 1) { std::fprintf(stderr, "host_rig: FAIL concurrent CaptureShown (frame %llu)\n", (unsigned long long)n); ++bad; }
    }
    char name[64];
    std::snprintf(name, sizeof name, "host_%llu.png", (unsigned long long)n);
    scene_png::WritePngRgb((std::filesystem::path(out) / name).string(), rgba, pw, ph);
    if ((hold_after >= 0 && uint64_t(hold_after) == n) || (fault_after >= 0 && uint64_t(fault_after) == n)) {   // S103 CHS (review fixes 3c / 3d): the tick AFTER a drawn frame that has no record (a held frame), or whose draw faults: the picture must stay the one shown
      const bool fault = fault_after >= 0 && uint64_t(fault_after) == n;
      sh::Msg extra;
      extra.guest_frame = rec->header.guest_frame;
      extra.width = w, extra.height = h;
      if (fault) extra.record = shown, host.PlantFaultNext(fault_kind);
      host.Post(std::move(extra));
      ++posted;
      if (!host.WaitHandled(posted, std::chrono::milliseconds(60000))) { std::fprintf(stderr, "host_rig: FAIL the host did not handle the %s tick\n", fault ? "fault" : "hold"); return 3; }
      std::vector<uint8_t> held;
      uint32_t hw2 = 0, hh2 = 0;
      if (!host.ReadPresent(held, hw2, hh2)) { std::fprintf(stderr, "host_rig: FAIL no presented picture for the %s tick\n", fault ? "fault" : "hold"); ++bad; }
      else {
        char hn2[64];
        std::snprintf(hn2, sizeof hn2, "%s_%llu.png", fault ? "fault" : "hold", (unsigned long long)n);
        scene_png::WritePngRgb((std::filesystem::path(out) / hn2).string(), held, hw2, hh2);
        std::printf("%s tick after frame %llu: source %d, presented %ux%u\n", fault ? "fault" : "hold", (unsigned long long)n, int(host.LastSource()), hw2, hh2);
      }
    }
    if (tween) {
      if (pass == 1) {   // the measured pass: what the halfway frame was made of and where its picture lies
        uint64_t mv = 0;
        const double d_pc = prev_final.empty() ? -1 : MeanAbs(prev_final, rgba, &mv);
        const double d_ph = info.half ? MeanAbs(prev_final, info.half_rgba) : -1;
        const double d_hc = info.half ? MeanAbs(info.half_rgba, rgba) : -1;
        std::printf("tween tick %llu: half %d matched %u keyed %llu pairs %llu floats %llu moving_floats %llu bad_floats %llu bad_outside %llu max_err %.3g | d_prev_cur %.4f d_prev_half %.4f d_half_cur %.4f moving_px %llu\n", (unsigned long long)n, info.half ? 1 : 0,
                    info.matched, (unsigned long long)info.keyed, (unsigned long long)info.pairs, (unsigned long long)info.floats, (unsigned long long)info.moving, (unsigned long long)info.bad_floats, (unsigned long long)info.bad_outside, info.max_err, d_pc, d_ph, d_hc, (unsigned long long)mv);
        if (info.half && !info.half_rgba.empty()) {
          char hn[64];
          std::snprintf(hn, sizeof hn, "half_%llu.png", (unsigned long long)n);
          scene_png::WritePngRgb((std::filesystem::path(out) / hn).string(), info.half_rgba, info.hw, info.hh);
        }
      }
      prev_final = rgba;
    }
    std::printf("host frame %llu: %s after %d post(s) (source %s, why \"%s\"), presented %ux%u\n", (unsigned long long)n, drawn ? "DRAWN" : "NOT DRAWN", tries, sh::SourceName(host.LastSource()), host.LastWhy().c_str(), pw, ph);
    if (!drawn) ++bad;
    if (walk_until >= 0) {   // S103 WB: the verdicts of the walk bit (PM_post_match_menu.md 5 item 2): records before the walk ends are DRAWN in 3D, the menu records are flat (Black under their rows), never a drawn dark arena and never Hold3D
      const bool before = n < uint64_t(walk_until);
      const sh::Source want = before ? sh::Source::Draw3D : sh::Source::Black;
      const bool ok = host.LastSource() == want;
      std::printf("walk check frame %llu (%s): %s (source %s, wanted %s)\n", (unsigned long long)n, before ? "walked" : "no walk", ok ? "PASS" : "FAIL", sh::SourceName(host.LastSource()), sh::SourceName(want));
      if (!ok) ++bad;
    }
  }
  {
    std::string by;
    for (unsigned k = 1; k < sh::kDrop2DCount; ++k)
      if (k != sh::kDropHole && tot_dropped[k]) by += std::string(by.empty() ? "" : " ") + sh::Drop2DName(k) + " " + std::to_string((unsigned long long)tot_dropped[k]);
    std::printf("rows total: %llu seen, %llu drawn, not drawn by reason: %s", (unsigned long long)tot_seen, (unsigned long long)tot_drawn, by.empty() ? "-" : by.c_str());
    if (have_cap) std::printf("; rig resolver: pictures %llu / %llu missed, planes %llu / %llu missed, %llu uploads", (unsigned long long)cap2d.pic_hit, (unsigned long long)cap2d.pic_miss, (unsigned long long)cap2d.plane_hit, (unsigned long long)cap2d.plane_miss, (unsigned long long)cap2d.uploads);
    std::printf("\n");
  }
  if (tween) std::printf("own_blend census: instanced_tagged %llu bake_seen %llu bake_tagged %llu\n", (unsigned long long)rdr_scene::g_own_blend_census.instanced_tagged.load(), (unsigned long long)rdr_scene::g_own_blend_census.bake_seen.load(), (unsigned long long)rdr_scene::g_own_blend_census.bake_tagged.load());
  std::printf("%s\n", sh::CensusLine(host.counters()).c_str());
  std::printf("%s\n", scene_frame::OursMissingLine().c_str());   // S103 ONF
  if (real && real_src.resolver) {
    const auto c = real_src.source.counters();
    const auto pc = rdr_scene::live::movieplanes::Store().census();
    uint64_t refused_total = 0;
    for (uint64_t v : c.refused) refused_total += v;
    for (int w8 = 0; w8 < 500 && !real_src.resolver->Idle(); ++w8) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::printf("real resolver: %s\n", real_src.resolver->Report().c_str());
    std::printf("real pending: %llu of %llu frames drew with pictures still arriving (%llu extra posts, the worst frame took %llu posts); resolver idle at the end: %s\n", (unsigned long long)real_pend_frames, (unsigned long long)real_frames, (unsigned long long)real_pend_posts, (unsigned long long)real_worst_posts, real_src.resolver->Idle() ? "yes" : "NO");
    std::printf("real source: born %llu died %llu seen %llu hits %llu joined %llu guest_copied %llu reads %llu hashes %llu trimmed %llu refill_unannounced %llu refused_total %llu; fed %llu of %llu rows (no texture %llu, format %llu), births %llu refills %llu\n",
                (unsigned long long)c.born, (unsigned long long)c.died, (unsigned long long)c.seen, (unsigned long long)c.hits, (unsigned long long)c.joined, (unsigned long long)c.guest_copied, (unsigned long long)c.reads,
                (unsigned long long)c.hashes, (unsigned long long)c.trimmed, (unsigned long long)c.refill_unannounced, (unsigned long long)refused_total,
                (unsigned long long)real_src.fed, (unsigned long long)real_src.rows, (unsigned long long)real_src.skipped_no_texture, (unsigned long long)real_src.skipped_format, (unsigned long long)real_src.births, (unsigned long long)real_src.refills);
    std::printf("real planes: movie rows %llu, flushes %llu, skipped %llu; store: copied %llu asked %llu answered %llu ambiguous %llu no_flush %llu too_old %llu\n", (unsigned long long)real_src.movie_rows, (unsigned long long)real_src.movie_flushes,
                (unsigned long long)real_src.movie_skipped, (unsigned long long)pc.copied, (unsigned long long)pc.asked, (unsigned long long)pc.answered, (unsigned long long)pc.ambiguous, (unsigned long long)pc.no_flush, (unsigned long long)pc.too_old);
  }
  if (gpu_stamps || group_stamps) {   // S103 GT: every pass bracketed, and the passes add up to the frame's GPU work (PC numbers, the rig's headless 3D draw of the last record); S103 GSTP: the group table
    for (int k = 0; k < 8 && prev; ++k) {   // the stamps of a frame are read a few frames late: more ticks of the last record flush them
      sh::Msg m;
      m.guest_frame = prev->header.guest_frame;
      m.width = w, m.height = h;
      m.record = prev;
      host.Post(std::move(m));
      ++posted;
      if (!wait_tick("the gpu stamps flush ticks")) return 3;
    }
    // S103 GSTP (review fix 4): NO LogGpuSummary here (the host's render thread may still be inside Render: group_table / group_pass are not locked). The renderer logs its [rdr][gpu][groups] table itself when the host stops and the renderer closes ("last window"), after the render thread is gone.
    if (gpu_stamps) {
    const rdr::Renderer::GpuRoleTotals t = host.renderer()->GpuRoles();
    const rdr::Renderer::UploadTotals up = host.renderer()->UploadRun();
    std::printf("gpu stamps: PC numbers, %llu GPU frames (%llu stamp-per-step), ms a frame by pass role:", (unsigned long long)t.frames, (unsigned long long)t.census_frames);
    for (int c = 0; c < rdr::kGpuClassCount; ++c) if (t.cls_ms[c] > 0 || t.cls_passes_a_frame[c] > 0) std::printf(" %s %.4f (%.1f passes)", rdr::GpuClassName(uint32_t(c)), t.cls_ms[c], t.cls_passes_a_frame[c]);
    std::printf("; work %.4f, start gap %.4f, total %.4f; stamped frames %llu, stamp identity broken %llu, overflows %llu; steps recorded %llu closed %llu; uploads steady %llu frames %.3f MB/frame, loading %llu frames %.3f MB/frame, upload GPU span %.4f ms over %llu frames\n",
                t.work_ms, t.gap_ms, t.total_ms, (unsigned long long)t.ident_frames, (unsigned long long)t.ident_bad, (unsigned long long)t.overflow, (unsigned long long)t.steps_recorded, (unsigned long long)t.steps_closed,
                (unsigned long long)up.steady_frames, up.steady_frames ? double(up.steady_bytes) / 1048576.0 / double(up.steady_frames) : 0.0, (unsigned long long)up.loading_frames,
                up.loading_frames ? double(up.loading_bytes) / 1048576.0 / double(up.loading_frames) : 0.0, up.frames_with_span ? up.gpu_ms_sum / double(up.frames_with_span) : 0.0, (unsigned long long)up.frames_with_span);
    std::vector<std::string> why;
    {   // S103 GT round 3: the roles' bracket sum must equal the GPU time the csv reports (rdr::g_gpu_ns_total: the same frames' [0]..tail bracket); the gap is its own named term, never silent
      const double csv_ms = double(rdr::g_gpu_ns_total.load()) / 1e6, roles_ms = t.total_ms * double(t.frames);
      std::printf("gpu stamps: roles bracket sum %.4f ms over %llu frames vs csv-basis GPU time %.4f ms over %llu frames (start gap %.4f ms a frame is inside both)\n", roles_ms, (unsigned long long)t.frames, csv_ms, (unsigned long long)rdr::g_gpu_frames.load(), t.gap_ms);
      if (csv_ms > 0 && std::fabs(roles_ms - csv_ms) > 0.02 * csv_ms + 0.01) why.push_back("UNSTAMPED: the roles' bracket sum " + std::to_string(roles_ms) + " ms differs from the csv GPU time " + std::to_string(csv_ms) + " ms by more than 2 %");
      std::vector<std::string> bare;
      const uint32_t nbare = host.renderer()->UnnamedColourTargets(&bare);
      if (nbare) { std::string l; for (const std::string& b : bare) l += (l.empty() ? "" : "; ") + b; why.push_back("colour target images have no role (their passes fall into 'other'): " + l); }
      std::printf("gpu stamps: roles absent in the stream-free build by design: ui (the 2D rows draw into F = frame), feedback (the sweat / damage maps are drawn by the scene draw into the colour pass, not a separate image); named when the record draws them: rim, plane, prelight, cards, post, copies, shadow\n");
    }
    if (t.census_frames < 3) why.push_back("fewer than 3 stamp-per-step frames were stamped (the stamps are off)");
    if (t.steps_recorded == 0 || t.steps_closed != t.steps_recorded) why.push_back("UNBRACKETED: steps recorded " + std::to_string((unsigned long long)t.steps_recorded) + " != steps closed by a written stamp " + std::to_string((unsigned long long)t.steps_closed));
    if (t.ident_bad) why.push_back("UNBRACKETED: the stamp count identity (2 + uploads pair + closes + tail) broke in " + std::to_string((unsigned long long)t.ident_bad) + " frames");
    if (t.overflow) why.push_back("UNBRACKETED: the stamp pool overflowed " + std::to_string((unsigned long long)t.overflow) + " times (stamps dropped)");
    const double up_ms = up.frames_with_span && t.frames ? up.gpu_ms_sum / double(t.frames) : 0.0;   // the uploads' own stamp pair is class other: take it out
    if (t.work_ms > 0 && t.cls_ms[rdr::kGpuOther] - up_ms > 0.10 * t.work_ms) why.push_back("role 'other' holds " + std::to_string(100.0 * (t.cls_ms[rdr::kGpuOther] - up_ms) / t.work_ms) + " % of the work beside the uploads (a pass is not named)");
    if (t.cls_passes_a_frame[rdr::kGpuColor] < 0.05) why.push_back("no colour pass counted (the scene colour image is not named)");
    const bool no_frame_pass_ok = host.DirectCopies() == 0;   // S103 CHS (review fix 2): with present_direct a run of frames with no 2D row never copies into F and never touches it: the frame role is legally 0 then
    if (t.cls_ms[rdr::kGpuColor] <= 0 || t.cls_ms[rdr::kGpuOutput] <= 0 || (t.cls_ms[rdr::kGpuFrame] <= 0 && !no_frame_pass_ok)) why.push_back("a role every stream-free frame has (colour, output, frame) holds 0 ms");
    if (t.cls_passes_a_frame[rdr::kGpuFrame] < 0.05 && !no_frame_pass_ok) why.push_back("no frame pass counted (F is not named)");
    if (why.empty()) std::printf("gpu stamps check: PASS (every recorded step closed by a written stamp, the stamp count identity holds, no overflow, the scene passes are named)\n");
    else { for (const std::string& s : why) std::printf("gpu stamps check: FAIL %s\n", s.c_str()); ++bad; }
    }   // S103 GSTP: (gpu_stamps)
  }
  if (!wait_tick("the final drain")) return 3;
  if (rdr::g_stencil_dead.load()) for (const std::string& l : rdr::DepthCensusLines()) std::printf("%s\n", l.c_str());   // S103 CHS C5: the lever's census
  real_src.resolver.reset();   // PN4: its images go while the renderer still exists (the game's resolver is never destroyed: the app leaves with a hard exit)
  host.Stop();
  return bad ? 3 : 0;
}
