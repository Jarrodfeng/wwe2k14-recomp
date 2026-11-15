// S102 lane U4: unit test of the benchmark button's judging, CSV reader, results writer and scene list (src\launcher\bench_button.h). No ImGui, no game.
//   bench_button_test SCENES_DIR OUT_DIR ARENAS_JSON [REAL_FRAMES_CSV REAL_LOG]
// SCENES_DIR = tools\input_scripts\scenes (the routes the list must match); OUT_DIR gets results.txt / results.json (the python driver parses them); ARENAS_JSON =
// db\ids\arenas.json (the arena id is read from it); REAL_* = a real game's frames_NNN.csv and its log: MeasureRun runs on them and the numbers go to real.json for
// the driver's independent recomputation. Exit 0 = every check passed. Planted twins are marked PLANT: each must be refused.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "bench_button.h"

using namespace launcher::bench;
namespace fs = std::filesystem;

static int fails = 0, checks = 0;
#define CHECK(c, ...)                         \
  do {                                        \
    ++checks;                                 \
    if (!(c)) {                               \
      ++fails;                                \
      std::printf("FAIL  " __VA_ARGS__);      \
      std::printf("\n");                      \
    }                                         \
  } while (0)

static std::string Slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
static bool Near(double a, double b, double e = 0.01) { return std::fabs(a - b) <= e; }

static const char* kHead = "frame,t_seconds,frame_ms,game_cpu_ms,gpu_thread_cpu_ms,gpu_ms,draw_rows,gpu_mhz,textures,texture_mb,warm_mb,gpu_wait_ms,gpu_wait_main,scene,presented,present_gap_ms\n";

// N frames at 20 ms (50 fps); every 100th frame (f % 100 == 99) is a 50 ms hitch. game 4 ms, render 6 ms, gpu 12 ms. A scene-transition text on frame 100 (the old window rule's
// anchor; the new rule ignores it).
static std::string Synthetic(int n, bool presented_col = true) {
  std::string s = presented_col ? kHead : "frame,frame_ms,game_cpu_ms,gpu_thread_cpu_ms,gpu_ms,scene\n";
  char b[256];
  for (int f = 0; f < n; ++f) {
    const double ms = (f % 100 == 99) ? 50.0 : 20.0;
    const char* scene = f == 100 ? "scene word 1 -> 4 (match load / entrances)" : "";
    if (presented_col)
      std::snprintf(b, sizeof b, "%d,%.3f,%.3f,4.000,6.000,12.000,0,,0,0.0,0,0.00,,%s,1,%.3f\n", f, f * 0.02, ms, scene, ms);
    else
      std::snprintf(b, sizeof b, "%d,%.3f,4.000,6.000,12.000,%s\n", f, ms, scene);
    s += b;
  }
  return s;
}
static std::string GoodLog(long mark1, long end) {
  return "[wwe2k14][input] mark 1 tron_343 (frame " + std::to_string(mark1) + ")\n[wwe2k14][input] mark 2 step15 (frame " + std::to_string((mark1 + end) / 2) +
         ")\n[wwe2k14][input] mark 3 end (frame " + std::to_string(end) + ")\n[wwe2k14][input] log: bench: done\n";
}

int main(int argc, char** argv) {
  if (argc < 4) return std::printf("usage: bench_button_test SCENES_DIR OUT_DIR ARENAS_JSON [REAL_CSV REAL_LOG]\n"), 2;
  const std::string scenes_dir = argv[1], out_dir = argv[2];

  // 1. The scene list: routes exist; @quick (never a lesnar_bryan launch); the arena id equals db\ids\arenas.json's "Over the Limit".
  CHECK(Scenes().size() == 3, "three scenes, got %zu", Scenes().size());
  for (size_t i = 0; i < Scenes().size(); ++i) {
    for (size_t j = i + 1; j < Scenes().size(); ++j) CHECK(std::string(Scenes()[i].id) != Scenes()[j].id, "duplicate scene id %s", Scenes()[i].id);
    const Scene& s = Scenes()[i];
    const std::string text = Slurp(scenes_dir + "/" + s.id + ".txt");
    CHECK(!text.empty(), "route file missing or empty: %s/%s.txt", scenes_dir.c_str(), s.id);
    CHECK(std::string(s.route) == std::string("bench/") + s.id + ".txt", "route path of %s is not bench/<id>.txt", s.id);
    CHECK(text.find("log bench: done") != std::string::npos, "route %s never logs 'bench: done'", s.id);
    CHECK(text.find("mark 1 ") != std::string::npos, "route %s has no 'mark 1'", s.id);
    CHECK(SceneFlagsError(s).empty(), "%s", SceneFlagsError(s).c_str());
    CHECK(std::string(s.flags).find("--wwe2k14_launch=") == std::string::npos, "scene %s carries a --wwe2k14_launch flag", s.id);
    CHECK(std::string(s.flags).find("--vulkan_stored_pipelines_at_boot=true") != std::string::npos, "scene %s: boot pipelines are on (M6)", s.id);
  }
  CHECK(std::string(Scenes()[0].quick_ids) == "343,324" && std::string(Scenes()[1].quick_ids) == "324,343" && Scenes()[2].skip_entrances && !Scenes()[0].skip_entrances,
        "Brock first for his entrance, Bryan first for his, the match skips entrances");
  {  // PLANT: any scene flag --wwe2k14_launch= other than @quick is refused
    Scene bad = Scenes()[0];
    bad.flags = "--wwe2k14_launch=lesnar_bryan --wwe2k14_fixed_seed=1";
    CHECK(!SceneFlagsError(bad).empty(), "PLANT --wwe2k14_launch=lesnar_bryan must be refused");
    bad.flags = "--wwe2k14_fixed_seed=1 --wwe2k14_launch=foo";
    CHECK(!SceneFlagsError(bad).empty(), "PLANT launch flag after another flag must be refused");
    bad.flags = "--wwe2k14_launch=@quick --wwe2k14_fixed_seed=1";
    CHECK(SceneFlagsError(bad).empty(), "@quick itself is allowed");
  }
  {
    const std::string arenas = Slurp(argv[3]);
    const size_t at = arenas.find("\"name\": \"Over the Limit\"");
    CHECK(at != std::string::npos, "arenas.json has no Over the Limit entry");
    const size_t idp = arenas.rfind("\"id\":", at);
    CHECK(idp != std::string::npos && std::atoi(arenas.c_str() + idp + 5) == kArenaOverTheLimit, "arenas.json says Over the Limit is id %d, header says %d", idp == std::string::npos ? -1 : std::atoi(arenas.c_str() + idp + 5), kArenaOverTheLimit);
  }

  // 2. RequiredFiles / MissingFiles: all present -> none missing; PLANT bench\ without states.toml -> missing (the button must not show).
  {
    const fs::path base = fs::path(out_dir) / "exe_dir";
    std::error_code ec;
    fs::remove_all(base, ec);
    for (const std::string& f : RequiredFiles()) {
      fs::create_directories((base / f).parent_path(), ec);
      std::ofstream(base / f) << "x\n";
    }
    CHECK(MissingFiles(base).empty(), "all files present -> nothing missing");
    CHECK(RequiredFiles().size() == 5, "5 required files: 3 routes + nav/boot_to_menu.txt + states.toml");
    fs::remove(base / "bench" / "states.toml");
    const auto miss = MissingFiles(base);
    CHECK(miss.size() == 1 && miss[0] == "bench/states.toml", "PLANT no states.toml -> exactly it is missing");
    fs::create_directories(base / "bench" / "states.toml", ec);  // a folder is not a file
    CHECK(MissingFiles(base).size() == 1, "a folder named states.toml does not count");
    fs::remove_all(base / "bench" / "nav", ec);
    CHECK(MissingFiles(base).size() == 2, "no nav -> two missing");
  }

  // 3. The numbers on a synthetic run with known answers: window = mark 1 .. last mark, NOT the CSV's scene text.
  const SceneResult r = MeasureRun("match", "m", Synthetic(1000), GoodLog(160, 999), false, true);
  CHECK(r.error.empty(), "synthetic run errored: %s", r.error.c_str());
  CHECK(r.frames == 999 - 160 + 1, "window = frames 160..999 = 840, got %zu", r.frames);
  const double total = 9 * 50.0 + 831 * 20.0;  // hitches at 199,...,999
  CHECK(Near(r.swap_fps, 1000.0 * 840 / total, 0.01), "avg fps %.3f != %.3f", r.swap_fps, 1000.0 * 840 / total);
  CHECK(Near(r.swap_low1_fps, 20.0, 0.01), "1%% low %.3f != 20", r.swap_low1_fps);
  CHECK(Near(r.shown_fps, r.swap_fps, 0.01), "every frame presented: shown == swap fps");
  CHECK(Near(r.game_ms.avg, 4.0) && Near(r.render_ms.avg, 6.0) && Near(r.gpu_ms.avg, 12.0), "three sides avg 4 / 6 / 12, got %.2f / %.2f / %.2f", r.game_ms.avg, r.render_ms.avg, r.gpu_ms.avg);
  {  // M2 PLANT: an early scene marker (frame 100 in the CSV) and a later mark 1 (frame 400): the window starts at the MARK
    const SceneResult m = MeasureRun("match", "m", Synthetic(1000), GoodLog(400, 999), false, true);
    CHECK(m.error.empty() && m.frames == 600, "PLANT early scene marker + mark 1 at 400: window must be 400..999 = 600 frames, got %zu (%s)", m.frames, m.error.c_str());
  }
  {  // marks out of order in the log: the last mark by frame wins, mark 1 is the first with id 1
    const std::vector<Mark> mk = ParseMarks("[wwe2k14][input] mark 2 end (frame 900)\n[wwe2k14][input] mark 1 x (frame 100)\nnoise\n[wwe2k14][input] mark 3 (frame 500)\n");
    long a = 0, b = 0;
    std::string err;
    CHECK(mk.size() == 3 && WindowFromMarks(mk, a, b, err) && a == 100 && b == 900, "marks parsed: 100..900");
  }

  // 4. Loud failures (H2, M1, H1): each is NO RESULT with its reason; nothing becomes a number.
  {
    const SceneResult nodone = MeasureRun("m", "m", Synthetic(1000), "[wwe2k14][input] mark 1 a (frame 160)\n[wwe2k14][input] mark 3 end (frame 999)\n", false, true);
    CHECK(!nodone.error.empty() && nodone.frames == 0, "PLANT good CSV + log without 'bench: done' -> NO RESULT");
    const SceneResult quoted = MeasureRun("m", "m", Synthetic(1000), "x\n[wwe2k14][input] step 14 wait_state entrance_step_0 failed after 3600 frames (line 20)\n", false, true);
    CHECK(quoted.error.find("wait_state entrance_step_0 failed") != std::string::npos, "NO RESULT quotes the failure line, got '%s'", quoted.error.c_str());
    CHECK(MeasureRun("m", "m", Synthetic(1000), GoodLog(160, 999), true, true).error.find("time limit") != std::string::npos, "PLANT a scene that never exits: timed out -> NO RESULT at the limit");
    CHECK(OverLimit(601) && !OverLimit(599) && OverLimit(10, 5), "OverLimit");
    CHECK(MeasureRun("m", "m", Synthetic(1000), GoodLog(160, 999), false, false).error.find("no new log") != std::string::npos, "PLANT an old log -> NO RESULT");
    CHECK(LogNumber("wwe2k14_123.log") == 123 && LogNumber("wwe2k14_x.log") == -1 && LogNumber("frames_5.csv") == -1 && LogNumber("wwe2k14_.log") == -1, "LogNumber");
    CHECK(LogIsNew(122, "wwe2k14_123.log") && !LogIsNew(123, "wwe2k14_123.log") && !LogIsNew(123, "wwe2k14_100.log"), "LogIsNew is strictly newer");
    CHECK(MeasureRun("m", "m", Synthetic(1000), "[wwe2k14][input] mark 2 end (frame 999)\nbench: done\n", false, true).error.find("mark 1") != std::string::npos, "no mark 1 -> NO RESULT");
    CHECK(MeasureRun("m", "m", Synthetic(1000), "[wwe2k14][input] mark 1 x (frame 160)\nbench: done\n", false, true).error.find("no mark after") != std::string::npos, "only mark 1 -> NO RESULT");
    CHECK(!MeasureRun("m", "m", Synthetic(1000), GoodLog(5000, 6000), false, true).error.empty(), "marks beyond the CSV -> NO RESULT (fewer than 30 frames)");
  }
  // No presented column: shown is n/a; an empty side column is n/a, not zero.
  const SceneResult np = MeasureRun("m", "m", Synthetic(400, false), GoodLog(160, 399), false, true);
  CHECK(np.error.empty() && np.shown_fps < 0, "no presented column -> shown fps n/a");
  std::string empty_cols = kHead;
  for (int f = 0; f < 200; ++f) empty_cols += std::to_string(f) + ",0,16.667,,,,0,,0,0.0,0,0.00,,,1,16.667\n";
  const SceneResult ec = MeasureRun("m", "m", empty_cols, GoodLog(10, 190), false, true);
  CHECK(ec.error.empty() && ec.gpu_ms.n == 0 && ec.gpu_ms.avg < 0 && ec.game_ms.n == 0, "empty side columns are 'not measured'");
  CHECK(StatText(ec.gpu_ms) == "not measured on this run", "text for an unmeasured side");
  CHECK(!MeasureRun("m", "m", "", GoodLog(1, 2), false, true).error.empty(), "empty CSV must be an error");
  CHECK(!MeasureRun("m", "m", kHead, GoodLog(1, 2), false, true).error.empty(), "header only must be an error");
  CHECK(!MeasureRun("m", "m", "foo,bar\n1,2\n", GoodLog(1, 2), false, true).error.empty(), "wrong columns must be an error");
  {
    std::string plant = Synthetic(500);
    plant.replace(plant.find("frame_ms"), 8, "frametime");
    CHECK(!MeasureRun("m", "m", plant, GoodLog(10, 400), false, true).error.empty(), "PLANT frame_ms renamed must be refused");
  }

  // 5. The writer. Earlier scenes keep their numbers when a later one has none.
  Info info;
  info.version = "v0.0.0-test", info.made = "2026-11-14 12:00", info.gpu = "Test GPU (1 MB)", info.driver_and_api = "Test GPU, Vulkan 1.3.0, driver 1", info.cpu = "Test CPU", info.ram = "32 GB",
  info.windows = "Windows 11", info.resolution = "1280x720 (720p)", info.tier = "STANDARD";
  std::vector<SceneResult> results = {r, np, MeasureRun("bad", "broken scene", "", "", false, true)};
  const std::string text = ResultsText(info, results), json = ResultsJson(info, results);
  for (const char* must : {"Test GPU (1 MB)", "Vulkan 1.3.0", "Test CPU", "32 GB", "1280x720", "STANDARD", "shown fps:", "1% low", "game side (swap thread)", "our render thread", "GPU:", "NO RESULT: the route never logged 'bench: done'", "Heat / power: not recorded", "frames measured: 840"})
    CHECK(text.find(must) != std::string::npos, "results.txt lacks '%s'", must);
  for (const char* must : {"\"gpu\": \"Test GPU (1 MB)\"", "\"tier\": \"STANDARD\"", "\"shown_fps\"", "\"game\"", "\"render_thread\"", "\"gpu\": {", "\"error\": \"the route never logged"})
    CHECK(json.find(must) != std::string::npos, "results.json lacks %s", must);
  CHECK(ResultsText(Info{}, {}).find("Driver / API (as the renderer saw it): unknown") != std::string::npos, "unknown fields print 'unknown'");
  CHECK(DeviceFromLog("x\n[rdr] device: GPU A, Vulkan 1.3.0, driver 5, unified memory 0\ny") == "GPU A, Vulkan 1.3.0, driver 5, unified memory 0", "device line from a log");
  CHECK(DeviceFromLog("nothing").empty(), "no device line -> empty");
  CHECK(TierFromLog("a\n[gfx][applied] tier=auto_integrated render_scale=1 internal=1280x720\n") == "auto_integrated", "tier from the [gfx][applied] line");
  CHECK(TierFromLog("[gfx][applied] tier=chosen\nrender_scale=1").empty() && TierFromLog("nothing").empty(), "no or broken tier line -> empty");
  std::ofstream(out_dir + "/results.txt", std::ios::binary) << text;
  std::ofstream(out_dir + "/results.json", std::ios::binary) << json;

  // 6. A real run, when given: its numbers go to real.json for the driver's independent recomputation.
  if (argc > 5) {
    const SceneResult real = MeasureRun("real", "real", Slurp(argv[4]), Slurp(argv[5]), false, true);
    CHECK(real.error.empty(), "real run: %s", real.error.c_str());
    std::ofstream(out_dir + "/real.json", std::ios::binary) << ResultsJson(Info{}, {real});
  }
  std::printf("%s  %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
  return fails ? 1 : 0;
}
