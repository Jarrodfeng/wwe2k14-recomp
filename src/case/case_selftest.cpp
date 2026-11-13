// case_selftest.exe (Session 101, lane 5; S101 lane 11 grew the cases): feeds the case ring from an existing capture exactly as the live sink would
// (contents appended under keys, frames naming those keys, geometry dropped after its last use, images released, read-back parts flagged),
// triggers, and writes a zip. No game, no GPU. Driven by tools\case\selftest.py.
//   case_selftest --capture DIR --out DIR [--picture PNG] [--window-s N] [--frame-gap-s N] [--cap-mb N] [--passes N]
//                 [--cap-test] [--drop-geometry] [--part-every N] [--release-images] [--lose-geometry N] [--double-trigger] [--retype-image]
// --cap-test: no zip at the end; feeds `passes` passes (default 4) under a cap of 1.2x one pass and prints the peak; a zip is triggered
//             after the first pass and written while the later passes are fed (the cap must hold while a zip is being written).
// --drop-geometry: DropGeometry for a key once its last use is fed (what the sink does); implied by --cap-test.
// --part-every N: every Nth frame is fed as a DrawAndRead part (flagged). --release-images: ReleaseImage of an unused id each frame.
// --lose-geometry N: the N-th geometry content never reaches the ring (a planted fault: the zip must report it unresolved).
// --double-trigger: two triggers back to back (the same second): two distinct, valid zips.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../renderer_adapter/capture_reader.h"
#include "../renderer/log_formats.h"  // S101 lane 19 review: the renderer's log formats
#include "case_hook.h"
#include "case_where.h"  // S101 lane32

using namespace rdr_adapter;

namespace {

// S101 lane 19: what the game side would hand the zip. Default = a PC owner build with everything known; --bare = a caller that knows nothing (every
// field must then come out as a reason); --phone = an Android build (APK number, display refresh, battery, thermal).
struct Extras {
  bool full = true, phone = false, render_timing = true, drift = false, no_next_frame = false;
  rdr_case::Trigger trigger = rdr_case::Trigger::Key;
  std::string git = "selftest", sdk_pin = "pin-selftest", kind = "dev", version = "v0.0.0-selftest+dev";
} g_extras;

struct Options {
  bool drop_geometry = false, release_images = false;
  int part_every = 0;
  bool retype_image = false;
  int lose_geometry = 0;
};

struct Feeder {
  const CaptureReader& reader;
  Options opt;
  uint64_t next_key = 1;
  double t = 0;
  double gap = 1.0 / 60.0;
  uint64_t guest = 1000;
  int geometry_seen = 0;

  // One pass over the capture: frames as the live sink gets them (offsets are keys). `after_frame(i)` runs after each frame is fed.
  template <class F>
  bool Pass(F&& after_frame) {
    std::vector<ImageRecord> records = reader.images();
    if (opt.retype_image) {
      // S101 lane 11 (case 7041): an image whose record changed after its content was read (the reader recreated it with another storage format).
      // The table the ring keeps is the newest one, so the content frame 0 names no longer fits it; the zip must leave that content out.
      LoadedFrame first;
      if (reader.LoadFrame(0, first) && first.contents.count) {
        const uint32_t id = first.contents.data[0].image;
        for (ImageRecord& r : records) {
          if (r.id != id) continue;
          const uint8_t was = r.format;
          r.format = uint8_t(was == uint8_t(rdr::Format::RGBA16F) ? rdr::Format::RGBA8 : rdr::Format::RGBA16F);
          std::printf("RETYPED image %u format %u -> %u\n", id, unsigned(was), unsigned(r.format));
        }
      }
    }
    case_hook::SetImages(records.data(), records.size());
    // Last frame each geometry offset is drawn in (the sink drops its key then).
    std::unordered_map<uint64_t, uint32_t> last_use;
    if (opt.drop_geometry) {
      for (uint32_t i = 0; i < reader.frame_count(); ++i) {
        LoadedFrame lf;
        if (!reader.LoadFrame(i, lf)) return false;
        for (uint32_t d = 0; d < lf.draws.count; ++d) {
          const Draw& draw = lf.draws.data[d];
          if (draw.flags & kDrawNoGeometry) continue;
          if (draw.vertex_bytes) last_use[draw.vertex_offset] = i;
          if (draw.index_bytes) last_use[draw.index_offset] = i;
        }
      }
    }
    std::unordered_map<uint64_t, uint64_t> geometry_keys;  // capture offset -> key (the reader keeps a key while a draw uses it)
    for (uint32_t i = 0; i < reader.frame_count(); ++i) {
      LoadedFrame lf;
      if (!reader.LoadFrame(i, lf)) {
        std::fprintf(stderr, "cannot load frame %u\n", i);
        return false;
      }
      std::vector<uint64_t> frame_offsets;
      auto* draws = const_cast<Draw*>(lf.draws.data);
      for (uint32_t d = 0; d < lf.draws.count; ++d) {
        Draw& draw = draws[d];
        if (draw.flags & kDrawNoGeometry) continue;
        auto key_for = [&](uint64_t offset, uint32_t bytes) -> uint64_t {
          if (!bytes) return offset;
          auto it = geometry_keys.find(offset);
          if (it != geometry_keys.end()) return it->second;
          std::vector<uint8_t> data;
          if (!reader.ReadGeometry(offset, bytes, data)) return offset;
          const uint64_t key = next_key++;
          ++geometry_seen;
          if (opt.lose_geometry && geometry_seen == opt.lose_geometry) {
            geometry_keys[offset] = key;  // the key exists, its bytes never reach the ring
            return key;
          }
          case_hook::OnContent(true, key, data.data(), data.size());
          geometry_keys[offset] = key;
          return key;
        };
        if (draw.vertex_bytes) frame_offsets.push_back(draw.vertex_offset);
        if (draw.index_bytes) frame_offsets.push_back(draw.index_offset);
        draw.vertex_offset = key_for(draw.vertex_offset, draw.vertex_bytes);
        draw.index_offset = key_for(draw.index_offset, draw.index_bytes);
      }
      auto* contents = const_cast<ContentRef*>(lf.contents.data);
      for (uint32_t c = 0; c < lf.contents.count; ++c) {
        LoadedTexture tex;
        if (!reader.ReadTexture(contents[c], tex)) continue;
        const uint64_t key = next_key++;
        case_hook::OnContent(false, key, tex.bytes.data(), tex.bytes.size());
        contents[c].offset = key;
      }
      if (opt.release_images) case_hook::ReleaseImage(0x00FFFFF0u + (i & 7));  // ids no capture uses: the call path only
      const bool part = opt.part_every > 0 && int(i % opt.part_every) == opt.part_every - 1;
      // S101 lane 19 review: the render thread may report a frame before its step row exists (every 3rd frame here): the ring keeps it pending
      const auto render_first = [&] { if (g_extras.render_timing) case_hook::OnRenderTiming(guest, 4.25, i % 5 == 4 ? -1.0 : 16.7); };
      if (i % 3 == 0) render_first();
      case_hook::OnTiming(guest, 16.6, std::nan(""), std::nan(""), std::nan(""));
      if (i % 3 != 0) render_first();
      case_hook::OnFrameAt(lf.bytes.data(), lf.bytes.size(), guest, t, part);
      if (opt.drop_geometry) {
        std::sort(frame_offsets.begin(), frame_offsets.end());
        frame_offsets.erase(std::unique(frame_offsets.begin(), frame_offsets.end()), frame_offsets.end());
        for (uint64_t offset : frame_offsets) {
          auto lu = last_use.find(offset);
          auto ki = geometry_keys.find(offset);
          if (lu != last_use.end() && lu->second == i && ki != geometry_keys.end()) {
            case_hook::DropGeometry(ki->second);
            geometry_keys.erase(ki);
          }
        }
      }
      ++guest;
      t += gap;
      after_frame(i);
    }
    return true;
  }
};

std::mutex g_results_mu;
std::vector<rdr_case::CaseResult> g_results;

case_hook::Context MakeContext(const std::filesystem::path& picture) {
  case_hook::Context ctx;
  ctx.manifest.device.platform = g_extras.phone ? "android" : "windows";
  if (g_extras.full) {
    // S101 lane 19: what bookmark.h's FillCaseManifest and the live sink's log tee would hand over
    ctx.manifest.build.git = g_extras.git;
    ctx.manifest.build.sdk_pin = g_extras.sdk_pin;
    ctx.manifest.build.kind = g_extras.kind;
    ctx.manifest.build.version = g_extras.version;
    ctx.manifest.device.resolution_scale = 2;
    ctx.manifest.where.route = "selftest_route.txt";
    {  // S101 lane32: what bookmark.h's FillCaseWhere would hand over for a 1v1 in the ring (the shipped case_where.h turns it into the where block)
      rdr_case::where::Inputs in;
      in.seq = 9, in.scene_word = 4, in.menu = -1, in.match_clock = 1200;
      in.entrance_steps = {5, -1, -1, -1, -1, -1};
      in.match_spec = "type=0;p=343:0,324:-1;arena=4;pace=1";
      in.match_go = 1;
      in.live.have = true, in.live.type = 0, in.live.arena = 4, in.live.pace = 1, in.live.ids = {343, 324};
      in.ring_bank = 324, in.ring_type = 3;
      in.guest_frame = 3000;
      rdr_case::where::Fill(ctx.manifest, in);
    }
    if (g_extras.phone) ctx.manifest.build.apk = 30101;
    ctx.manifest.settings.effective = {{"wwe2k14_case", "true"}, {"vsync", "false"}, {"wwe2k14_launch_arena", "3"}};
    ctx.manifest.settings.non_default = {"wwe2k14_case", "vsync", "wwe2k14_launch_arena"};
    ctx.manifest.settings.launcher_file = "settings_selftest.json";
    ctx.manifest.way_back.builder = "wwe2k14_launch=@quick; wwe2k14_launch_arena=3";
    ctx.manifest.way_back.seed = 12345;
    ctx.manifest.way_back.replay_mode = true;
    if (g_extras.phone) ctx.manifest.device.battery_pct = 81.5, ctx.manifest.device.thermal = "nominal";
    ctx.inputs_for_window = [](uint64_t first, uint64_t last) {
      // a synthetic recorder file in src\replay.h's format: header, a pad state before the window, rows inside, one after
      std::string t = "# wwe2k14 replay v2 (selftest)\nname selftest\nseed 12345\ndlc none\nflag wwe2k14_launch @quick\n";
      t += "S " + std::to_string(first - 5) + " 0 0 0 0 00000000\n";
      for (uint64_t f = first; f <= last; f += 9) t += "S " + std::to_string(f) + " 0 0 0 0 0100" + "\n";
      t += "K " + std::to_string(first + 3) + " 0 0 0 0 0 00\nT " + std::to_string(first + 9) + " 77\n";
      t += "S " + std::to_string(last + 50) + " 0 0 0 0 FFFF\n";
      t += "S " + std::to_string(last) + " 0 0 0 0 PARTIALROW";  // the recorder mid-row at the flush: no newline, must be dropped
      return rdr_case::info::InputsWindow(t, first, last);
    };
  }
  if (ctx.manifest.where.words.empty()) ctx.manifest.where.words = "selftest: frames fed from a capture, not a game";
  ctx.log_text = "selftest log line\n";
  ctx.states_text = "selftest states\n";
  ctx.picture_path = picture;
  return ctx;
}

void PrintResult(const rdr_case::CaseResult& r, double wall_ms) {
  std::printf("RESULT zip=%s zip_bytes=%llu frames=%u capture_bytes=%llu unresolved=%u trigger_to_zip_ms=%.1f wall_ms=%.1f picture=%d stale=%u\n",
              r.path.string().c_str(), (unsigned long long)r.zip_bytes, r.frames, (unsigned long long)r.capture_bytes, r.unresolved, r.ms_from_trigger,
              wall_ms, r.has_picture ? 1 : 0, r.stale_contents);
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path capture, out, picture;
  double window_s = 5.0, gap = 1.0 / 60.0;
  uint64_t cap_mb = 512;
  int passes = 1;
  bool cap_test = false, double_trigger = false, dir_test = false;
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--capture") capture = next();
    else if (a == "--out") out = next();
    else if (a == "--picture") picture = next();
    else if (a == "--window-s") window_s = std::atof(next());
    else if (a == "--frame-gap-s") gap = std::atof(next());
    else if (a == "--cap-mb") cap_mb = std::strtoull(next(), nullptr, 10);
    else if (a == "--passes") passes = std::atoi(next());
    else if (a == "--cap-test") cap_test = true;
    else if (a == "--dir-test") dir_test = true;
    else if (a == "--drop-geometry") opt.drop_geometry = true;
    else if (a == "--release-images") opt.release_images = true;
    else if (a == "--part-every") opt.part_every = std::atoi(next());
    else if (a == "--lose-geometry") opt.lose_geometry = std::atoi(next());
    else if (a == "--double-trigger") double_trigger = true;
    else if (a == "--retype-image") opt.retype_image = true;
    else if (a == "--bare") g_extras.full = false, g_extras.render_timing = false;
    else if (a == "--phone") g_extras.phone = true;
    else if (a == "--drift") g_extras.drift = true;
    else if (a == "--no-next-frame") g_extras.no_next_frame = true;
    else if (a == "--trigger") g_extras.trigger = std::string(next()) == "run_exit" ? rdr_case::Trigger::RunExit : rdr_case::Trigger::Key;
    else if (a == "--no-render-timing") g_extras.render_timing = false;
    else if (a == "--build-git") g_extras.git = next();
    else if (a == "--sdk-pin") g_extras.sdk_pin = next();
    else if (a == "--kind") g_extras.kind = next();
    else if (a == "--version") g_extras.version = next();
  }
  if (dir_test) {  // S101 lane 59: Configure warns on a relative case_dir (the run log names where the zips go) and is silent on an absolute one
    int bad = 0;
    for (const char* d : {"rel_cases", R"(C:\abs_cases)", "/abs_cases"}) {
      std::vector<std::string> lines;
      case_hook::Config c;
      c.enabled = true;
      c.dir = d;
      c.log = [&](const std::string& l) { lines.push_back(l); };
      case_hook::Configure(c);
      const bool want = !c.dir.is_absolute();
      bool got = false;
      for (const auto& l : lines) got = got || (l.find("relative") != std::string::npos && l.find("case_dir") != std::string::npos);
      std::printf("DIR %s is_absolute=%d warned=%d\n", d, c.dir.is_absolute() ? 1 : 0, got ? 1 : 0);
      if (want != got) ++bad;
    }
    std::printf(bad ? "FAIL case_dir warning\n" : "OK case_dir warning\n");
    return bad ? 1 : 0;
  }
  CaptureReader reader;
  if (capture.empty() || !reader.Open(capture)) {
    std::fprintf(stderr, "FAIL cannot open capture '%s'\n", capture.string().c_str());
    return 2;
  }
  uint64_t capture_bytes = 0;
  for (const auto& e : std::filesystem::recursive_directory_iterator(capture)) {
    if (e.is_regular_file()) capture_bytes += e.file_size();
  }
  if (cap_test) opt.drop_geometry = true;  // a sink drops its geometry; the cap cannot evict what the sink holds

  case_hook::Config cfg;
  cfg.enabled = true;
  cfg.window_s = window_s;
  cfg.cap_mb = cap_mb;
  cfg.dir = out;
  cfg.log = [](const std::string& line) { std::printf("LOG %s\n", line.c_str()); };
  uint64_t cap_bytes = cap_mb * 1024ull * 1024ull;
  if (cap_test) {
    cap_bytes = capture_bytes * 12 / 10;
    cfg.window_s = 1e9;  // only the byte cap trims
  }
  // Configure takes MB; the cap test needs finer: build the ring through the hook with the closest MB, then check against the real cap.
  cfg.cap_mb = cap_test ? std::max<uint64_t>(1, cap_bytes / (1024 * 1024)) : cap_mb;
  cap_bytes = cfg.cap_mb * 1024ull * 1024ull;
  case_hook::Configure(cfg, [](const rdr_case::CaseResult& r) {
    std::lock_guard lock(g_results_mu);
    g_results.push_back(r);
  });

  rdr_case::info::ResetDevice();
  if (g_extras.full) {  // S101 lane 19: the lines the live sink's log tee reads, and the names probe the app sets
    namespace info = rdr_case::info;
    // the lines are built from the renderer's own formats (src/renderer/log_formats.h): a drifted format breaks this test.
    // --drift prints a device line the parser does not know (the renderer "ran" but its line changed): the zip must say PARSE FAILED.
    char l1[256], l2[256], l3[256];
    std::snprintf(l1, sizeof(l1), g_extras.drift ? "[rdr] gpu: %s, Vulkan %u.%u.%u, driver %08X, unified memory %d, lazy memory %d, BC %d, ASTC %d" : RDR_LOG_DEVICE_FMT,
                  "Selftest GPU 9000", 1u, 3u, 280u, 0xABCDu, 0, 0, 1, 0);
    std::snprintf(l2, sizeof(l2), RDR_LOG_SWAPCHAIN_FMT, 1920u, 1080u, 3u, 0u, 2);
    std::snprintf(l3, sizeof(l3), RDR_LOG_DISPLAY_TIMING_FMT, 8.333);
    info::NoteRendererLine(l1);
    info::NoteRendererLine(l2);
    if (g_extras.phone) info::NoteRendererLine(l3);
    info::SetRender(2560, 1440, 2.0);
    info::SetNamesProbe([]() {
      static info::NamesSample c;  // cumulative, as reader.h's counters are
      c.draws += 10, c.untagged += 1, c.unclassified += 2, c.textures_unnamed += 3, c.texture_binds += 20, c.surfaces_guessed += 1;
      return c;
    });
  }
  Feeder feeder{reader, opt};
  feeder.gap = gap;
  const int total_passes = cap_test ? std::max(passes, 4) : passes;
  uint64_t peak = 0;
  bool mid_triggered = false;
  for (int p = 0; p < total_passes; ++p) {
    const bool ok = feeder.Pass([&](uint32_t frame_index) {
      // B4: a zip is being written (its snapshot holds references) while the ring keeps being fed under its cap.
      if (cap_test && p == 0 && frame_index + 1 == reader.frame_count() / 2 && !mid_triggered) {
        mid_triggered = true;
        case_hook::Trigger(rdr_case::Trigger::RunShot, "cap test: a zip written while the ring is fed", MakeContext(picture));
      }
    });
    if (!ok) return 2;
    peak = std::max(peak, case_hook::Stats().peak_bytes);
  }
  const rdr_case::RingStats st = case_hook::Stats();
  std::printf("RING cap=%llu peak=%llu settled_peak=%llu now=%llu frames=%u dropped=%llu baseline_dropped=%llu unresolved=%llu static_over_cap=%d fed_bytes=%llu\n",
              (unsigned long long)cap_bytes, (unsigned long long)st.peak_bytes, (unsigned long long)st.settled_peak_bytes, (unsigned long long)st.bytes,
              st.frames, (unsigned long long)st.frames_dropped, (unsigned long long)st.baseline_dropped, (unsigned long long)st.unresolved,
              st.static_over_cap ? 1 : 0, (unsigned long long)(capture_bytes * total_passes));
  if (cap_test) {
    rdr_case::WaitIdle();
    bool zip_ok = false;
    {
      std::lock_guard lock(g_results_mu);
      zip_ok = !g_results.empty() && g_results.front().ok;
    }
    const bool ok = st.settled_peak_bytes <= cap_bytes && st.frames_dropped > 0 && zip_ok && !st.static_over_cap;
    std::printf("%s ring cap: settled peak %llu <= cap %llu (in-flight peak, a frame being built: %llu), fed %llu (%.1fx the cap), %llu frames dropped, "
                "mid-run zip %s\n",
                ok ? "OK" : "FAIL", (unsigned long long)st.settled_peak_bytes, (unsigned long long)cap_bytes, (unsigned long long)st.peak_bytes,
                (unsigned long long)(capture_bytes * total_passes), double(capture_bytes * total_passes) / double(cap_bytes),
                (unsigned long long)st.frames_dropped, zip_ok ? "written" : "NOT WRITTEN");
    return ok ? 0 : 1;
  }

  const auto t0 = std::chrono::steady_clock::now();
  case_hook::Context first_ctx = MakeContext(picture);
  if (g_extras.no_next_frame) {
    // S101 lane 19 (Check B): the game is quitting, no further frame is presented. The "next shown picture" saver accepts the request and never writes;
    // only the last-presented saver can deliver. Which one the trigger asks is case_hook::SavePictureFor's choice (run exit / failure: the last one).
    std::error_code ec;
    std::filesystem::create_directories(out, ec);
    const std::filesystem::path path = out / ".case_picture_test.png";
    case_hook::SetPictureSaver([](const std::filesystem::path&) { return true; });
    case_hook::SetLastPictureSaver([picture](const std::filesystem::path& to) {
      std::error_code e;
      std::filesystem::copy_file(picture, to.string() + ".part", std::filesystem::copy_options::overwrite_existing, e);
      if (e) return false;
      std::filesystem::rename(to.string() + ".part", to, e);
      return !e;
    });
    first_ctx.picture_path.clear();
    if (case_hook::SavePictureFor(g_extras.trigger, path)) first_ctx.picture_path = path, first_ctx.picture_temp = true;
  }
  case_hook::Trigger(g_extras.trigger, "selftest", std::move(first_ctx));
  if (double_trigger) case_hook::Trigger(rdr_case::Trigger::Key, "selftest second", MakeContext(picture));
  rdr_case::WaitIdle();
  const double wall_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::vector<rdr_case::CaseResult> results;
  {
    std::lock_guard lock(g_results_mu);
    results = g_results;
  }
  if (results.empty()) {
    std::fprintf(stderr, "FAIL no zip result\n");
    return 1;
  }
  int rc = 0;
  for (const auto& r : results) {
    if (!r.ok) {
      std::fprintf(stderr, "FAIL zip not written: %s\n", r.error.c_str());
      rc = 1;
      continue;
    }
    PrintResult(r, wall_ms);
  }
  case_hook::Shutdown();
  return rc;
}
