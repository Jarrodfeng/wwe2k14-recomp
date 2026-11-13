// Lane 139 (SH3 slice 2): fview --plane-check OUT.json --capture DIR --plane-oracle FILE.plane [--frames N] [--check-every K] [--replace-fault plane.NAME] [--shadows-ours-fault NAME] [--shadow-map-size S]
// Two sessions on the same capture play it frame by frame in lockstep, SH2's shadow passes ON in both: A = the stream, B = the plane hook (--plane-shadow-ours semantics) fed from the oracle file: the
// capture's own plane-chain constants laid out as the register uploads the game makes, read by the SHIPPED writer's tracker (plane_oracle.h), joined by the guest-frame stamp (half (b) of the
// acceptance). After each compared frame the plane chain's images (the destinations of the copies out of the plane targets: the downscale 220, the mask 221) and the presented picture are read back
// from both and compared texel by texel; the json holds numbers only. The python driver (tools\shadow\plane_shadow_check.py) judges: clean = every image within rounding, the picture byte-equal, every
// claimed draw replaced; each planted fault must fail.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "app.h"
#include "../renderer_scene/replace_factories.h"

namespace fview {
namespace {

struct Acc {
  uint64_t frames = 0, shape_fail = 0;
  double max_diff = 0, max_mean = 0, sum_mean = 0;
  uint32_t w = 0, h = 0;
  uint64_t over1_pixels = 0, pixels = 0;   // lane 159: pixels (any channel) more than 1 level off, of all pixels compared
  double ref_max = 0, ref_sum = 0;         // the stream's own image: its largest byte and its mean over all frames (how much signal the comparison has)
  int chain = 0;   // shadows_ours::Chain of the image (1 map, 2 plane)
};

void Compare(Acc& acc, const std::vector<uint8_t>& a, uint32_t aw, uint32_t ah, const std::vector<uint8_t>& b, uint32_t bw, uint32_t bh) {
  ++acc.frames;
  acc.w = aw, acc.h = ah;
  if (aw != bw || ah != bh || a.size() != b.size()) {
    ++acc.shape_fail;
    return;
  }
  double sum = 0, mx = 0, rsum = 0;
  const size_t bpp = (aw && ah) ? std::max<size_t>(1, a.size() / (size_t(aw) * ah)) : 1;
  for (size_t i = 0; i < a.size(); i += bpp) {
    int pixel_worst = 0;
    for (size_t c = 0; c < bpp && i + c < a.size(); ++c) {
      const int d = std::abs(int(a[i + c]) - int(b[i + c]));
      sum += d, mx = std::max(mx, double(d)), rsum += a[i + c], pixel_worst = std::max(pixel_worst, d);
      acc.ref_max = std::max(acc.ref_max, double(a[i + c]));
    }
    ++acc.pixels;
    if (pixel_worst > 1) ++acc.over1_pixels;
  }
  acc.ref_sum += a.empty() ? 0 : rsum / double(a.size());
  const double mean = a.empty() ? 0 : sum / double(a.size());
  acc.max_diff = std::max(acc.max_diff, mx), acc.max_mean = std::max(acc.max_mean, mean), acc.sum_mean += mean;
}

std::string One(const Acc& a) {
  char t[600];
  std::snprintf(t, sizeof t, "{\"frames\": %llu, \"shape_fail\": %llu, \"max_diff\": %.3f, \"max_mean\": %.5f, \"avg_mean\": %.5f, \"over1_pixels\": %llu, \"pixels\": %llu, \"ref_max\": %.0f, \"ref_mean\": %.5f, \"chain\": %d, \"size\": \"%ux%u\"}",
                static_cast<unsigned long long>(a.frames), static_cast<unsigned long long>(a.shape_fail), a.max_diff, a.max_mean, a.frames ? a.sum_mean / double(a.frames) : 0.0,
                static_cast<unsigned long long>(a.over1_pixels), static_cast<unsigned long long>(a.pixels), a.ref_max, a.frames ? a.ref_sum / double(a.frames) : 0.0, a.chain, a.w, a.h);
  return t;
}

}  // namespace

int RunPlaneCheck(const Options& o, const std::filesystem::path& root) {
  (void)root;
  rdr::RendererOptions ro;
  ro.validation = o.validation < 0 ? 0 : o.validation;
  ro.gpu_index = o.gpu;
  ro.render_scale = o.scale;
  Session a, b;
  std::string error;
  if (!a.Open(o.capture, ro, error)) return std::fprintf(stderr, "[fview] open A: %s\n", error.c_str()), 2;
  if (!b.Open(o.capture, ro, error)) return std::fprintf(stderr, "[fview] open B: %s\n", error.c_str()), 2;
  if (o.plane_oracle.empty()) return std::fprintf(stderr, "[fview] --plane-check needs --plane-oracle FILE.plane (tools\\shadow\\plane_oracle.py dump)\n"), 2;
  if (o.shadow_casters_ours) {   // F4: the composition arm. Lane 99's hook Claims the plane site first and does not pass a draw on unless it answers NotOurs, so this hook then sees only what it passed on.
    const std::string game = !o.game.empty() ? o.game : (root / "game_data" / "extracted").string();
    rdr_scene::InstallReplaceFactories();
    // lane 255: the casters' wrestler / ring joins read the wrestlers' and the ring's hooks (registered before it, as in main.cpp); without them only the chCommon proxy joins
    if (o.wrestlers_ours && !b.EnableReplace("wrestlers", o.replace_fault, game, error)) return std::fprintf(stderr, "[fview] wrestlers: %s\n", error.c_str()), 2;
    if (o.ring_ours && !b.EnableReplace("ring", o.replace_fault, game, error)) return std::fprintf(stderr, "[fview] ring: %s\n", error.c_str()), 2;
    if (!b.EnableReplace("shadow_casters", "", game, error)) return std::fprintf(stderr, "[fview] shadow_casters: %s\n", error.c_str()), 2;
  }
  if (!b.EnableReplace("plane_shadow", o.replace_fault, "", error)) return std::fprintf(stderr, "[fview] plane_shadow: %s\n", error.c_str()), 2;   // (after the casters: registration order is call order)
  if (!b.SetPlaneOracle(o.plane_oracle, error)) return std::fprintf(stderr, "[fview] plane oracle: %s\n", error.c_str()), 2;
  const uint32_t count = std::min<uint32_t>(a.frame_count(), o.frames > 0 ? uint32_t(o.frames) : a.frame_count());
  const rdr_adapter::shadows_ours::Fault sh2_fault = rdr_adapter::shadows_ours::g_fault;   // SH2's planted fault (--shadows-ours-fault) belongs to arm B only: the re-plants copy_swap, scaled_map
  std::map<uint32_t, Acc> images;
  Acc picture;
  uint64_t compared = 0, no_dest = 0, differing_frames = 0;
  std::vector<uint8_t> ia, ib, pa, pb;
  for (uint32_t f = 0; f < count; ++f) {
    rdr_adapter::g_shadows_ours = true, rdr_adapter::g_shadow_map_size = o.shadow_map_size;
    rdr_adapter::shadows_ours::g_fault = rdr_adapter::shadows_ours::Fault::None;
    if (!a.StepFrame(f, error)) return std::fprintf(stderr, "[fview] A frame %u: %s\n", f, error.c_str()), 2;
    rdr_adapter::shadows_ours::g_fault = sh2_fault;
    if (!b.StepFrame(f, error)) return std::fprintf(stderr, "[fview] B frame %u: %s\n", f, error.c_str()), 2;
    if (f < 2 || f % o.check_every) continue;   // frames 0-1: the stream's partial copies leave texels no frame has written yet
    ++compared;
    const rdr_adapter::shadows_ours::Plan& plan = b.ShadowsPlan();
    const rdr_adapter::LoadedFrame& fr = b.frame();
    std::set<uint32_t> done;
    for (const rdr_adapter::Op& op : fr.ops) {
      if (op.kind != rdr_adapter::OpKind::Copy) continue;
      const rdr::CopyOp& c = fr.copies[op.index];
      if (plan.Of(c.source.image) == rdr_adapter::shadows_ours::kNone) continue;   // the readers' images of both chains (the plane chain is this lane's; the maps are SH2's, which must stay equal)
      const uint32_t dest = c.dest.image;
      if (!done.insert(dest).second) continue;
      uint32_t aw = 0, ah = 0, bw = 0, bh = 0;
      if (!a.ReadSurface(dest, ia, aw, ah) || !b.ReadSurface(dest, ib, bw, bh)) {
        ++no_dest;
        continue;
      }
      images[dest].chain = int(plan.Of(dest));
      Compare(images[dest], ia, aw, ah, ib, bw, bh);
    }
    uint32_t aw = 0, ah = 0, bw = 0, bh = 0;
    if (a.ReadPresented(pa, aw, ah) && b.ReadPresented(pb, bw, bh)) {
      const uint64_t before = picture.frames;
      const double prev = picture.max_diff;
      Compare(picture, pa, aw, ah, pb, bw, bh);
      if (picture.frames > before && picture.max_diff > prev) ++differing_frames;
    }
  }
  rdr_adapter::g_shadows_ours = false;
  std::string report = b.ReplaceReport("plane_shadow");
  std::printf("%s\n", report.c_str());
  if (o.shadow_casters_ours) {   // where the mask draws went: the casters' census beside this hook's
    const std::string casters = b.ReplaceReport("shadow_casters");
    std::printf("%s\n", casters.c_str());
    report += " || SHADOW CASTERS (lane 99, registered first): " + casters;
  }
  const uint64_t errors = a.validation_errors() + b.validation_errors();
  std::printf("[fview] plane-check: %u frames played, %llu compared, %zu plane images, picture max diff %.3f, validation errors %llu\n", count, static_cast<unsigned long long>(compared), images.size(),
              picture.max_diff, static_cast<unsigned long long>(errors));
  std::string escaped;
  for (const char c : report) escaped += c == '"' ? std::string("'") : c == '\n' ? std::string("\\n") : c == '\\' ? std::string("/") : std::string(1, c);
  std::ostringstream js;
  js << "{\n \"frames\": " << count << ", \"compared\": " << compared << ", \"no_dest\": " << no_dest << ", \"differing_frames\": " << differing_frames << ", \"validation_errors\": " << errors << ",\n \"images\": {";
  bool first = true;
  for (const auto& item : images) js << (first ? "\n  \"" : ",\n  \"") << item.first << "\": " << One(item.second), first = false;
  js << "\n },\n \"picture\": " << One(picture) << ",\n \"report\": \"" << escaped << "\"\n}\n";
  std::filesystem::create_directories(std::filesystem::path(o.plane_check).parent_path());
  std::ofstream(o.plane_check, std::ios::binary) << js.str();
  return errors ? 4 : 0;
}

}  // namespace fview
