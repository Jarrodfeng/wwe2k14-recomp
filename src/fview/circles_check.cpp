// Lane 136 (SH3 slice 1): fview --circles-check OUT.json --capture DIR --circles-oracle FILE.circ [--frames N] [--check-every K] [--replace-fault circles.NAME] [--shadows-ours]
// Two sessions on the same capture play it frame by frame in lockstep: A = the stream (nothing on), B = the circle hook (--circles-ours semantics) fed from the oracle file: the
// capture's own circle constants laid out as guest memory, read by the SHIPPED writer's tracker, joined by the guest-frame stamp (half (b) of the acceptance). After each compared frame
// the presented pictures are read back from both and compared; with --shadows-ours both arms run SH2's passes, so B's circle draws land in our shadow maps. The json holds numbers only.
// The python driver (tools\shadow\circles_ours_check.py) judges: clean = byte-equal pictures and every circle draw replaced; each planted fault must fail.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "app.h"

namespace fview {

int RunCirclesCheck(const Options& o, const std::filesystem::path& root) {
  (void)root;
  rdr::RendererOptions ro;
  ro.validation = o.validation < 0 ? 0 : o.validation;
  ro.gpu_index = o.gpu;
  ro.render_scale = o.scale;
  Session a, b;
  std::string error;
  if (!a.Open(o.capture, ro, error)) return std::fprintf(stderr, "[fview] open A: %s\n", error.c_str()), 2;
  if (!b.Open(o.capture, ro, error)) return std::fprintf(stderr, "[fview] open B: %s\n", error.c_str()), 2;
  if (o.circles_oracle.empty()) return std::fprintf(stderr, "[fview] --circles-check needs --circles-oracle FILE.circ (tools\\shadow\\circles_oracle.py dump)\n"), 2;
  if (!b.EnableReplace("circles", o.replace_fault, "", error)) return std::fprintf(stderr, "[fview] circles: %s\n", error.c_str()), 2;
  if (!b.SetCircleOracle(o.circles_oracle, error)) return std::fprintf(stderr, "[fview] circles oracle: %s\n", error.c_str()), 2;
  const uint32_t count = std::min<uint32_t>(a.frame_count(), o.frames > 0 ? uint32_t(o.frames) : a.frame_count());
  const rdr_adapter::shadows_ours::Fault sh2_fault = rdr_adapter::shadows_ours::g_fault;
  uint64_t compared = 0;
  double max_diff = 0, max_mean = 0, sum_mean = 0;
  uint64_t differing_frames = 0;
  std::vector<uint8_t> pa, pb;
  for (uint32_t f = 0; f < count; ++f) {
    rdr_adapter::g_shadows_ours = o.shadows_ours, rdr_adapter::g_shadow_map_size = o.shadow_map_size;
    rdr_adapter::shadows_ours::g_fault = rdr_adapter::shadows_ours::Fault::None;   // SH2's planted fault (--shadows-ours-fault) belongs to arm B only: the re-plant R floor_reads_wrestler_map
    if (!a.StepFrame(f, error)) return std::fprintf(stderr, "[fview] A frame %u: %s\n", f, error.c_str()), 2;
    rdr_adapter::shadows_ours::g_fault = sh2_fault;
    if (!b.StepFrame(f, error)) return std::fprintf(stderr, "[fview] B frame %u: %s\n", f, error.c_str()), 2;
    if (f % o.check_every) continue;
    uint32_t aw = 0, ah = 0, bw = 0, bh = 0;
    if (!a.ReadPresented(pa, aw, ah) || !b.ReadPresented(pb, bw, bh) || aw != bw || ah != bh) continue;
    ++compared;
    double sum = 0, mx = 0;
    for (size_t i = 0; i < pa.size() && i < pb.size(); ++i) {
      const double d = std::abs(int(pa[i]) - int(pb[i]));
      sum += d, mx = std::max(mx, d);
    }
    const double mean = pa.empty() ? 0 : sum / double(pa.size());
    max_diff = std::max(max_diff, mx), max_mean = std::max(max_mean, mean), sum_mean += mean;
    if (mx > 0) ++differing_frames;
  }
  const std::string report = b.ReplaceReport("circles");
  std::printf("%s\n", report.c_str());
  const uint64_t errors = a.validation_errors() + b.validation_errors();
  std::printf("[fview] circles-check: %u frames played, %llu compared, %llu differ, max diff %.3f, validation errors %llu\n", count, static_cast<unsigned long long>(compared),
              static_cast<unsigned long long>(differing_frames), max_diff, static_cast<unsigned long long>(errors));
  std::string escaped;
  for (const char c : report) escaped += c == '"' ? std::string("'") : c == '\n' ? std::string("\\n") : c == '\\' ? std::string("/") : std::string(1, c);
  std::ostringstream js;
  js << "{\n \"frames\": " << count << ", \"compared\": " << compared << ", \"differing_frames\": " << differing_frames << ", \"validation_errors\": " << errors << ",\n \"picture\": {\"max_diff\": " << max_diff
     << ", \"max_mean\": " << max_mean << ", \"avg_mean\": " << (compared ? sum_mean / double(compared) : 0.0) << "},\n \"report\": \"" << escaped << "\"\n}\n";
  std::filesystem::create_directories(std::filesystem::path(o.circles_check).parent_path());
  std::ofstream(o.circles_check, std::ios::binary) << js.str();
  return errors ? 4 : 0;
}

}  // namespace fview
