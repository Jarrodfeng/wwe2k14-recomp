// S101 lane 123 (SH2): fview --shadows-check OUT.json --capture DIR [--frames N] [--check-every K] [--shadow-map-size S] [--shadows-ours-fault NAME] [--shadow-keying rim_as_shadow]
// Two sessions on the same capture play it frame by frame in lockstep: A = the stream (the switch off), B = our shadow passes (g_shadows_ours on, maps S px). After each
// compared frame the readers' images (the destinations of the copies out of the shadow targets) and the presented picture are read back from both and compared:
//   factor 1  : texel by texel (display RGBA8; 16-bit images are scaled by their peak, the same transform in both arms)
//   factor f  : the finer image box-filtered down to the coarser one's size, plus the footprint box (where the map holds anything) against the stream's
// The shipped binder code runs in both arms; only the switch differs. The json holds numbers, never pixels. The python driver (tools\shadow\shadows_ours_check.py) judges.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "app.h"

namespace fview {
namespace {

struct Acc {
  uint64_t frames = 0, shape_fail = 0;
  double max_diff = 0, max_mean = 0, sum_mean = 0, max_bbox_err = 0;
  uint32_t a_w = 0, a_h = 0, b_w = 0, b_h = 0;
  int chain = 0;  // shadows_ours::Chain of the image (1 map, 2 plane)
};

// Box-filters `src` (w x h RGBA8) down to tw x th when w, h are whole multiples, else nearest.
std::vector<uint8_t> Reduce(const std::vector<uint8_t>& src, uint32_t w, uint32_t h, uint32_t tw, uint32_t th) {
  std::vector<uint8_t> out(size_t(tw) * th * 4);
  const uint32_t bx = w / tw, by = h / th;
  for (uint32_t y = 0; y < th; ++y)
    for (uint32_t x = 0; x < tw; ++x)
      for (int c = 0; c < 4; ++c) {
        uint32_t sum = 0;
        for (uint32_t j = 0; j < by; ++j)
          for (uint32_t i = 0; i < bx; ++i) sum += src[(size_t(y * by + j) * w + (x * bx + i)) * 4 + c];
        out[(size_t(y) * tw + x) * 4 + c] = uint8_t((sum + bx * by / 2) / (bx * by));
      }
  return out;
}

// Footprint: the box of texels that differ from the image's own corner texel (the clear colour) by more than 8, in 0..1 of the image (empty = {1,1,0,0}).
struct Box { double x0 = 1, y0 = 1, x1 = 0, y1 = 0; };
Box Footprint(const std::vector<uint8_t>& img, uint32_t w, uint32_t h) {
  Box b;
  const uint8_t* c0 = &img[0];
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      const uint8_t* p = &img[(size_t(y) * w + x) * 4];
      if (std::abs(int(p[0]) - c0[0]) > 8 || std::abs(int(p[1]) - c0[1]) > 8 || std::abs(int(p[2]) - c0[2]) > 8 || std::abs(int(p[3]) - c0[3]) > 8) {
        b.x0 = std::min(b.x0, double(x) / w), b.y0 = std::min(b.y0, double(y) / h);
        b.x1 = std::max(b.x1, double(x + 1) / w), b.y1 = std::max(b.y1, double(y + 1) / h);
      }
    }
  return b;
}

void Compare(Acc& acc, const std::vector<uint8_t>& a, uint32_t aw, uint32_t ah, const std::vector<uint8_t>& b, uint32_t bw, uint32_t bh, int32_t rx, int32_t ry, uint32_t rw, uint32_t rh, bool footprint) {
  ++acc.frames;
  acc.a_w = aw, acc.a_h = ah, acc.b_w = bw, acc.b_h = bh;
  // Whole multiples both ways, the same ratio on both axes (a 16:9-squeezed map fails here: the plant scaled_map).
  const bool finer = bw >= aw && bh >= ah;
  const uint32_t fx = finer ? bw / std::max(1u, aw) : aw / std::max(1u, bw), fy = finer ? bh / std::max(1u, ah) : ah / std::max(1u, bh);
  const bool whole = finer ? (bw % aw == 0 && bh % ah == 0) : (aw % bw == 0 && ah % bh == 0);
  if (!whole || fx != fy) {
    ++acc.shape_fail;
    return;
  }
  // Bring both to the coarser size.
  const uint32_t cw = finer ? aw : bw, ch = finer ? ah : bh;
  const std::vector<uint8_t> ra = finer ? a : Reduce(a, aw, ah, cw, ch);
  const std::vector<uint8_t> rb = finer ? Reduce(b, bw, bh, cw, ch) : b;
  // The rectangle is in the stream's pixels; at the coarser size it is scaled by cw / aw.
  const double s = double(cw) / double(aw);
  const uint32_t x0 = uint32_t(std::max(0.0, std::floor(rx * s))), y0 = uint32_t(std::max(0.0, std::floor(ry * s)));
  const uint32_t x1 = std::min(cw, uint32_t(std::ceil((rx + double(rw)) * s))), y1 = std::min(ch, uint32_t(std::ceil((ry + double(rh)) * s)));
  uint64_t n = 0;
  double sum = 0, mx = 0;
  for (uint32_t y = y0; y < y1; ++y)
    for (uint32_t x = x0; x < x1; ++x)
      for (int c = 0; c < 4; ++c) {
        const double d = std::abs(int(ra[(size_t(y) * cw + x) * 4 + c]) - int(rb[(size_t(y) * cw + x) * 4 + c]));
        sum += d, mx = std::max(mx, d), ++n;
      }
  const double mean = n ? sum / double(n) : 0;
  acc.max_diff = std::max(acc.max_diff, mx), acc.max_mean = std::max(acc.max_mean, mean), acc.sum_mean += mean;
  if (footprint) {
    const Box fa = Footprint(a, aw, ah), fb = Footprint(b, bw, bh);
    const double err = std::max({std::abs(fa.x0 - fb.x0), std::abs(fa.y0 - fb.y0), std::abs(fa.x1 - fb.x1), std::abs(fa.y1 - fb.y1)});
    acc.max_bbox_err = std::max(acc.max_bbox_err, err);
  }
}

std::string Json(const std::map<uint32_t, Acc>& images, const Acc& picture, uint64_t compared, uint64_t frames, const std::string& census, uint64_t validation, uint64_t classified_frames, uint32_t casters_max,
                 uint64_t no_dest) {
  std::ostringstream o;
  o << "{\n \"frames\": " << frames << ", \"compared\": " << compared << ", \"classified_frames\": " << classified_frames << ", \"casters_max\": " << casters_max << ", \"validation_errors\": " << validation
    << ", \"no_dest\": " << no_dest << ",\n \"census\": \"" << census << "\",\n \"images\": {";
  bool first = true;
  const auto one = [&](const Acc& a) {
    char t[512];
    std::snprintf(t, sizeof t,
                  "{\"frames\": %llu, \"shape_fail\": %llu, \"max_diff\": %.3f, \"max_mean\": %.5f, \"avg_mean\": %.5f, \"max_bbox_err\": %.5f, \"chain\": %d, \"a\": \"%ux%u\", \"b\": \"%ux%u\"}",
                  static_cast<unsigned long long>(a.frames), static_cast<unsigned long long>(a.shape_fail), a.max_diff, a.max_mean, a.frames ? a.sum_mean / double(a.frames) : 0.0, a.max_bbox_err, a.chain, a.a_w,
                  a.a_h, a.b_w, a.b_h);
    return std::string(t);
  };
  for (const auto& item : images) {
    o << (first ? "\n  \"" : ",\n  \"") << item.first << "\": " << one(item.second);
    first = false;
  }
  o << "\n },\n \"picture\": " << one(picture) << "\n}\n";
  return o.str();
}

}  // namespace

int RunShadowsCheck(const Options& o, const std::filesystem::path& root) {
  (void)root;
  rdr::RendererOptions ro;
  ro.validation = o.validation < 0 ? 0 : o.validation;
  ro.gpu_index = o.gpu;
  ro.render_scale = o.scale;
  Session a, b;
  std::string error;
  if (!a.Open(o.capture, ro, error)) return std::fprintf(stderr, "[fview] open A: %s\n", error.c_str()), 2;
  if (!b.Open(o.capture, ro, error)) return std::fprintf(stderr, "[fview] open B: %s\n", error.c_str()), 2;
  const uint32_t count = std::min<uint32_t>(a.frame_count(), o.frames > 0 ? uint32_t(o.frames) : a.frame_count());
  const uint32_t size = o.shadow_map_size;
  std::map<uint32_t, Acc> images;
  Acc picture;
  uint64_t compared = 0, classified = 0, no_dest = 0;
  uint32_t casters_max = 0;
  std::vector<uint8_t> ia, ib, pa, pb;
  for (uint32_t f = 0; f < count; ++f) {
    rdr_adapter::g_shadows_ours = false;
    if (!a.StepFrame(f, error)) return std::fprintf(stderr, "[fview] A frame %u: %s\n", f, error.c_str()), 2;
    rdr_adapter::g_shadows_ours = true, rdr_adapter::g_shadow_map_size = size;
    if (!b.StepFrame(f, error)) return std::fprintf(stderr, "[fview] B frame %u: %s\n", f, error.c_str()), 2;
    rdr_adapter::g_shadows_ours = false;
    const rdr_adapter::shadows_ours::Plan& plan = b.ShadowsPlan();
    if (!plan.chain.empty()) ++classified;
    casters_max = std::max(casters_max, plan.casters);
    if (f < 2 || f % o.check_every) continue;  // frames 0-1: the stream's partial copies leave texels no frame has written yet
    ++compared;
    // The readers' images: the destinations of the frame's copies whose source is one of our shadow targets; the stream's rectangle is where the stream wrote.
    const rdr_adapter::LoadedFrame& fr = b.frame();
    std::set<uint32_t> done;
    for (const rdr_adapter::Op& op : fr.ops) {
      if (op.kind != rdr_adapter::OpKind::Copy) continue;
      const rdr::CopyOp& c = fr.copies[op.index];
      if (plan.Of(c.source.image) == rdr_adapter::shadows_ours::kNone) continue;
      uint32_t aw = 0, ah = 0, bw = 0, bh = 0;
      const uint32_t dest = c.dest.image;
      const bool new_image = done.insert(dest).second;
      if (new_image) {
        if (!a.ReadSurface(dest, ia, aw, ah) || !b.ReadSurface(dest, ib, bw, bh)) {
          ++no_dest;
          done.erase(dest);
          continue;
        }
      } else {
        continue;  // (every copy into this image is compared below through the union of its rectangles on the first visit)
      }
      // Union of the rectangles of every copy into this destination in the frame.
      for (const rdr_adapter::Op& op2 : fr.ops) {
        if (op2.kind != rdr_adapter::OpKind::Copy) continue;
        const rdr::CopyOp& c2 = fr.copies[op2.index];
        if (c2.dest.image != dest || plan.Of(c2.source.image) == rdr_adapter::shadows_ours::kNone) continue;
        const bool fp = (aw != bw) || (ah != bh);
        images[dest].chain = int(plan.Of(dest));
        Compare(images[dest], ia, aw, ah, ib, bw, bh, c2.dest_rect.x, c2.dest_rect.y, c2.dest_rect.width, c2.dest_rect.height, fp);
      }
    }
    for (const uint32_t id : plan.reduce_dests) {  // A6: the coverage reductions keep the stream's size; compared whole
      uint32_t aw = 0, ah = 0, bw = 0, bh = 0;
      if (!a.ReadSurface(id, ia, aw, ah) || !b.ReadSurface(id, ib, bw, bh)) continue;
      Compare(images[id], ia, aw, ah, ib, bw, bh, 0, 0, aw, ah, false);
    }
    uint32_t aw = 0, ah = 0, bw = 0, bh = 0;
    if (a.ReadPresented(pa, aw, ah) && b.ReadPresented(pb, bw, bh)) Compare(picture, pa, aw, ah, pb, bw, bh, 0, 0, aw, ah, false);
  }
  const std::string census = b.ShadowsReport();
  std::printf("%s\n", census.c_str());
  const uint64_t errors = a.validation_errors() + b.validation_errors();
  std::printf("[fview] shadows-check: %u frames played, %llu compared, %llu with shadow images, validation errors %llu\n", count, static_cast<unsigned long long>(compared),
              static_cast<unsigned long long>(classified), static_cast<unsigned long long>(errors));
  const std::string json = Json(images, picture, compared, count, census, errors, classified, casters_max, no_dest);
  std::filesystem::create_directories(std::filesystem::path(o.shadows_check).parent_path());
  std::ofstream(o.shadows_check, std::ios::binary) << json;
  return errors ? 4 : 0;
}

}  // namespace fview
