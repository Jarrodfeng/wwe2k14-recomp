// S102 lane U4: the launcher's BENCHMARK button, the part that needs no window and no game: the scene list, the judging of a run, the reader of a run's frames CSV,
// the numbers (shown fps, 1 % low, the three sides' ms) and the results writer. Header-only, std only: tools\launcher\s102_tests\bench_button_test.cpp compiles and
// checks it. launcher_main.cpp holds the few marked lines that start the runs and put the file into the report zip.
//
// What a run is: the Surface bench's per-scene routes (tools\input_scripts\scenes\brock_entrance.txt, bryan_entrance.txt, match.txt, S89), but launched the way the
// PLAYER-SHIPPED performance test is: the "@quick" scenario (Over the Limit, CPU vs CPU, the two participants' ids; the match skips the entrances), never the
// lesnar_bryan scenario capture (local only, never ships). The launcher runs the scenes one after the other at the PLAYER'S OWN settings (the number is what that
// player gets), on a copy of the save and of the settings file, with fixed seed 1 so Bryan's camera variant is pinned. The routes are shipped next to the exe in bench\
// (made by tools\launcher\make_bench_dir.py, screenshot lines removed; release.ps1 -BenchButton, OFF until the button's first real run); the launcher offers the button
// only when every file of RequiredFiles() is there.
//
// The CSV (src\frame_stats.h, one row per guest frame): frame_ms, game_cpu_ms (the game's own swap thread = the game side), gpu_thread_cpu_ms (our render
// thread), gpu_ms (the GPU), presented + present_gap_ms (the frames that reached the screen). The measured window is the route's own: from `mark 1` to the LAST mark
// the route logged ("[wwe2k14][input] mark N text (frame F)"; the input script's frame counter is the guest frame the CSV counts: unconfirmed on a real run). 1 % low =
// 1000 / the 99th-percentile frame time, as tools\native\frames_fps.py reads it. A scene counts only if its log has the route's `bench: done` line.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace launcher::bench {

constexpr int kArenaOverTheLimit = 4;       // db\ids\arenas.json: "Over the Limit", BG04.PAC (the unit test reads the file and compares)
constexpr double kSceneLimitSeconds = 600;  // wall clock per scene: boot + load + 5-10 s of scene; a scene past it is ended and gets NO RESULT

struct Scene {
  const char* id;         // result key and file stem
  const char* label;      // shown to the player
  const char* route;      // under bench\ next to the exe
  const char* quick_ids;  // the @quick participants, slot 0 first (Brock 343, Bryan 324; Bryan's entrance has him first)
  bool skip_entrances;    // the match scene
  const char* flags;      // extra game flags (never --wwe2k14_launch=)
};

inline const std::vector<Scene>& Scenes() {
  static const std::vector<Scene> scenes = {
      {"brock_entrance", "Brock Lesnar's entrance", "bench/brock_entrance.txt", "343,324", false, "--wwe2k14_fixed_seed=1 --vulkan_stored_pipelines_at_boot=true"},
      {"bryan_entrance", "Daniel Bryan's entrance", "bench/bryan_entrance.txt", "324,343", false, "--wwe2k14_fixed_seed=1 --vulkan_stored_pipelines_at_boot=true"},
      {"match", "Lesnar vs Bryan, the match", "bench/match.txt", "343,324", true, "--wwe2k14_fixed_seed=1 --vulkan_stored_pipelines_at_boot=true"},
  };
  return scenes;
}
// A scene's own flags may not pick a launch scenario: lesnar_bryan is a local capture, only "@quick" ships (the launcher sets that itself through the settings).
inline std::string SceneFlagsError(const Scene& s) {
  const std::string f = s.flags;
  const std::string key = "--wwe2k14_launch=";
  for (size_t at = f.find(key); at != std::string::npos; at = f.find(key, at + 1)) {
    const std::string value = f.substr(at + key.size(), f.find(' ', at) == std::string::npos ? std::string::npos : f.find(' ', at) - at - key.size());
    if (value != "@quick") return std::string("scene ") + s.id + ": " + key + value + " is not @quick";
  }
  return {};
}
// Files the button needs next to the exe: the three routes, the nav snippet they `include`, and the states.toml their wait_state lines read.
inline std::vector<std::string> RequiredFiles() {
  std::vector<std::string> f;
  for (const Scene& s : Scenes()) f.emplace_back(s.route);
  f.emplace_back("bench/nav/boot_to_menu.txt");
  f.emplace_back("bench/states.toml");
  return f;
}
// The files of RequiredFiles() missing under `exe_dir` (empty = the button may show).
inline std::vector<std::string> MissingFiles(const std::filesystem::path& exe_dir) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const std::string& f : RequiredFiles())
    if (!std::filesystem::is_regular_file(exe_dir / f, ec)) out.push_back(f);
  return out;
}

// ---- the run's log: marks, the done line, the failure line, which log is new
struct Mark {
  long id = 0;
  long frame = 0;
};
// "[wwe2k14][input] mark 1 tron_343 (frame 1234)" -> {1, 1234}
inline std::vector<Mark> ParseMarks(const std::string& log) {
  std::vector<Mark> marks;
  const std::string key = "[wwe2k14][input] mark ";
  for (size_t at = log.find(key); at != std::string::npos; at = log.find(key, at + 1)) {
    const size_t eol = log.find('\n', at);
    const std::string line = log.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
    const size_t fr = line.rfind("(frame ");
    if (fr == std::string::npos) continue;
    Mark m;
    m.id = std::atol(line.c_str() + key.size());
    m.frame = std::atol(line.c_str() + fr + 7);
    marks.push_back(m);
  }
  return marks;
}
inline bool HasDone(const std::string& log) { return log.find("bench: done") != std::string::npos; }
// The last line of the log that says a route step failed (the input script's own words), else the generic reason.
inline std::string FailureLine(const std::string& log) {
  std::string best;
  size_t pos = 0;
  while (pos < log.size()) {
    size_t eol = log.find('\n', pos);
    if (eol == std::string::npos) eol = log.size();
    const std::string line = log.substr(pos, eol - pos);
    pos = eol + 1;
    if (line.find("[input]") == std::string::npos && line.find("input-script") == std::string::npos) continue;
    std::string low = line;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (low.find("fail") != std::string::npos || low.find("timed out") != std::string::npos || low.find("timeout") != std::string::npos) best = line;
  }
  while (!best.empty() && (best.back() == '\r' || best.back() == ' ')) best.pop_back();
  return best.empty() ? std::string("the route never logged 'bench: done' (the game ended before the scene finished)") : "the route stopped: " + best;
}
// "wwe2k14_123.log" -> 123 (-1 if it is not one); a benchmark scene needs a log whose number is strictly above the newest one from before its start.
inline long LogNumber(const std::string& name) {
  const std::string pre = "wwe2k14_";
  if (name.compare(0, pre.size(), pre) != 0 || name.size() < pre.size() + 5) return -1;
  const std::string digits = name.substr(pre.size(), name.size() - pre.size() - 4);
  if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) return -1;
  return std::atol(digits.c_str());
}
inline bool LogIsNew(long before, const std::string& name) { return LogNumber(name) > before; }
inline bool OverLimit(double elapsed_s, double limit_s = kSceneLimitSeconds) { return elapsed_s > limit_s; }

// ---- the frames CSV
struct Row {
  long frame = 0;
  double frame_ms = -1, game_cpu_ms = -1, render_ms = -1, gpu_ms = -1, gap_ms = -1;
  int presented = 0;
};

inline std::vector<std::string> SplitCsvLine(const std::string& line) {
  std::vector<std::string> f;
  size_t a = 0;
  for (size_t b; (b = line.find(',', a)) != std::string::npos; a = b + 1) f.push_back(line.substr(a, b - a));
  f.push_back(line.substr(a));
  return f;
}

// Reads by header name. Returns false with `error` when the header lacks frame or frame_ms, or no row parses (a reader that quietly reads nothing is the failure).
inline bool ParseFramesCsv(const std::string& csv, std::vector<Row>& rows, std::string& error) {
  rows.clear();
  size_t pos = 0;
  std::vector<std::string> head;
  int c_frame = -1, c_ms = -1, c_game = -1, c_render = -1, c_gpu = -1, c_pres = -1, c_gap = -1;
  while (pos < csv.size()) {
    size_t end = csv.find('\n', pos);
    if (end == std::string::npos) end = csv.size();
    std::string line = csv.substr(pos, end - pos);
    pos = end + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    const std::vector<std::string> f = SplitCsvLine(line);
    if (head.empty()) {
      head = f;
      for (size_t i = 0; i < f.size(); ++i) {
        const int at = int(i);
        if (f[i] == "frame") c_frame = at;
        else if (f[i] == "frame_ms") c_ms = at;
        else if (f[i] == "game_cpu_ms") c_game = at;
        else if (f[i] == "gpu_thread_cpu_ms") c_render = at;
        else if (f[i] == "gpu_ms") c_gpu = at;
        else if (f[i] == "presented") c_pres = at;
        else if (f[i] == "present_gap_ms") c_gap = at;
      }
      if (c_frame < 0 || c_ms < 0) {
        error = "the frames file has no frame / frame_ms columns";
        return false;
      }
      continue;
    }
    if (f.size() != head.size()) continue;  // a half-written last line
    auto num = [&](int col) { return col >= 0 && !f[size_t(col)].empty() ? std::atof(f[size_t(col)].c_str()) : -1.0; };
    Row r;
    r.frame = std::atol(f[size_t(c_frame)].c_str());
    r.frame_ms = num(c_ms);
    r.game_cpu_ms = num(c_game), r.render_ms = num(c_render), r.gpu_ms = num(c_gpu), r.gap_ms = num(c_gap);
    r.presented = c_pres >= 0 ? std::atoi(f[size_t(c_pres)].c_str()) : 0;
    if (r.frame_ms > 0) rows.push_back(r);
  }
  if (rows.empty()) {
    error = head.empty() ? "the frames file is empty" : "the frames file has a header but no timed frames";
    return false;
  }
  return true;
}

// The route's own window: frames from `mark 1` to the last mark. false + error when the log has no mark 1 or no mark after it.
inline bool WindowFromMarks(const std::vector<Mark>& marks, long& first, long& last, std::string& error) {
  const Mark* start = nullptr;
  for (const Mark& m : marks)
    if (m.id == 1) {
      start = &m;
      break;
    }
  if (!start) {
    error = "the log has no 'mark 1' (the route did not reach its scene)";
    return false;
  }
  long end = start->frame;
  for (const Mark& m : marks)
    if (m.frame > end) end = m.frame;
  if (end <= start->frame) {
    error = "the log has no mark after 'mark 1' (the route did not reach its end)";
    return false;
  }
  first = start->frame, last = end;
  return true;
}

struct Stat {
  size_t n = 0;
  double avg = -1, p99 = -1, max = -1;  // ms; -1 = no samples (the column was empty: not measured on this run)
};
inline Stat MakeStat(std::vector<double> v) {
  Stat s;
  v.erase(std::remove_if(v.begin(), v.end(), [](double x) { return x < 0; }), v.end());
  if (v.empty()) return s;
  std::sort(v.begin(), v.end());
  double sum = 0;
  for (double x : v) sum += x;
  s.n = v.size(), s.avg = sum / double(v.size()), s.p99 = v[std::min(v.size() - 1, size_t(0.99 * double(v.size())))], s.max = v.back();
  return s;
}

struct SceneResult {
  std::string id, label, error;  // error non-empty: no numbers (NO RESULT, with the reason)
  size_t frames = 0;
  double swap_fps = -1, swap_low1_fps = -1;    // every guest frame the game finished
  double shown_fps = -1, shown_low1_fps = -1;  // frames that reached a present (what the player saw); -1 if the CSV has no presented column
  Stat game_ms, render_ms, gpu_ms;             // the three sides
};

// The numbers of the frames in [first, last] (guest frame numbers).
inline SceneResult Measure(const std::string& id, const std::string& label, const std::string& csv, long first, long last) {
  SceneResult r;
  r.id = id, r.label = label;
  std::vector<Row> rows;
  if (!ParseFramesCsv(csv, rows, r.error)) return r;
  std::vector<double> ft, game, render, gpu, gaps;
  double total_ms = 0;
  long shown = 0;
  bool any_presented = false;
  for (const Row& w : rows) {
    if (w.frame < first || w.frame > last) continue;
    ft.push_back(w.frame_ms), game.push_back(w.game_cpu_ms), render.push_back(w.render_ms), gpu.push_back(w.gpu_ms);
    total_ms += w.frame_ms;
    shown += w.presented;
    any_presented |= w.presented > 0;
    if (w.presented && w.gap_ms > 0) gaps.push_back(w.gap_ms);
  }
  if (ft.size() < 30) {
    r.error = "fewer than 30 frames between the route's marks (" + std::to_string(ft.size()) + "): the scene did not run";
    return r;
  }
  r.frames = ft.size();
  const Stat all = MakeStat(ft);
  r.swap_fps = 1000.0 * double(ft.size()) / total_ms, r.swap_low1_fps = 1000.0 / all.p99;
  if (any_presented && total_ms > 0) {
    r.shown_fps = 1000.0 * double(shown) / total_ms;
    const Stat g = MakeStat(gaps);
    if (g.n) r.shown_low1_fps = 1000.0 / g.p99;
  }
  r.game_ms = MakeStat(game), r.render_ms = MakeStat(render), r.gpu_ms = MakeStat(gpu);
  return r;
}

// Everything a finished scene is judged on, in the order the launcher needs it. NO RESULT (error set) for: a scene ended at the time limit, a log that is not new, a log
// without the route's `bench: done` (quoting why the route stopped), a log without its marks, a CSV that does not read.
inline SceneResult MeasureRun(const std::string& id, const std::string& label, const std::string& csv, const std::string& log, bool timed_out, bool log_is_new) {
  SceneResult r;
  r.id = id, r.label = label;
  if (timed_out) {
    r.error = "the scene ran past its time limit (" + std::to_string(int(kSceneLimitSeconds)) + " s) and was ended";
    return r;
  }
  if (!log_is_new) {
    r.error = "the game wrote no new log for this scene (it did not start)";
    return r;
  }
  if (!HasDone(log)) {
    r.error = FailureLine(log);
    return r;
  }
  long first = 0, last = 0;
  if (!WindowFromMarks(ParseMarks(log), first, last, r.error)) return r;
  return Measure(id, label, csv, first, last);
}

// "[rdr] device: NVIDIA GeForce RTX 4090, Vulkan 1.3.0, driver 93D28000, ..." from a run's log: the card and driver as the renderer saw it ("" if absent).
inline std::string DeviceFromLog(const std::string& log) {
  const size_t at = log.find("[rdr] device: ");
  if (at == std::string::npos) return {};
  const size_t end = log.find('\n', at);
  std::string line = log.substr(at + 14, end == std::string::npos ? std::string::npos : end - at - 14);
  if (!line.empty() && line.back() == '\r') line.pop_back();
  return line;
}
// "[gfx][applied] tier=<text> render_scale=..." -> the tier text the game really used ("" if the log has no such line).
inline std::string TierFromLog(const std::string& log) {
  const size_t at = log.find("[gfx][applied] tier=");
  if (at == std::string::npos) return {};
  const size_t from = at + 20;
  const size_t to = log.find(" render_scale=", from);
  const size_t eol = log.find('\n', from);
  if (to == std::string::npos || (eol != std::string::npos && to > eol)) return {};
  return log.substr(from, to - from);
}

inline std::string Fx(double v, int digits = 1) {
  if (v < 0) return "n/a";
  char b[32];
  std::snprintf(b, sizeof b, "%.*f", digits, v);
  return b;
}
inline std::string StatText(const Stat& s) { return s.n ? "avg " + Fx(s.avg, 2) + " ms, 99th " + Fx(s.p99, 2) + " ms, worst " + Fx(s.max, 1) + " ms" : "not measured on this run"; }

struct Info {  // what the writer prints about the machine and the setup; empty fields print as "unknown"
  std::string version, made, gpu, driver_and_api, cpu, ram, windows, resolution, tier, settings_note, thermal;
};
inline std::string Or(const std::string& s) { return s.empty() ? "unknown" : s; }

inline std::string ResultsText(const Info& i, const std::vector<SceneResult>& results) {
  std::string t = "WWE 2K14 recomp benchmark: Lesnar vs Bryan (both entrances and the match)\n";
  t += "Version: " + Or(i.version) + "\nMade: " + Or(i.made) + " (local time)\n\n";
  t += "Hardware\n  GPU: " + Or(i.gpu) + "\n  Driver / API (as the renderer saw it): " + Or(i.driver_and_api) + "\n  CPU: " + Or(i.cpu) + "\n  RAM: " + Or(i.ram) +
       "\n  Windows: " + Or(i.windows) + "\n  Resolution: " + Or(i.resolution) + "\n  Quality tier: " + Or(i.tier) + "\n  Heat / power: " + Or(i.thermal.empty() ? std::string("not recorded") : i.thermal) + "\n";
  if (!i.settings_note.empty()) t += "  Settings: " + i.settings_note + "\n";
  t += "\nResults (the player's own settings; 1 % low = 1000 / the 99th-percentile frame time)\n";
  for (const SceneResult& r : results) {
    t += "\n" + r.label + " [" + r.id + "]\n";
    if (!r.error.empty()) {
      t += "  NO RESULT: " + r.error + "\n";
      continue;
    }
    t += "  frames measured: " + std::to_string(r.frames) + "\n";
    t += "  shown fps: " + Fx(r.shown_fps) + "   shown 1% low: " + Fx(r.shown_low1_fps) + "   (frames that reached the screen)\n";
    t += "  game frames: " + Fx(r.swap_fps) + " fps, 1% low " + Fx(r.swap_low1_fps) + "\n";
    t += "  game side (swap thread): " + StatText(r.game_ms) + "\n  our render thread: " + StatText(r.render_ms) + "\n  GPU: " + StatText(r.gpu_ms) + "\n";
  }
  return t;
}

inline std::string JsonStr(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += c;
    else if (c == '\n') o += "\\n";
    else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
    else o += c;
  }
  return o + "\"";
}
inline std::string JsonNum(double v) { return v < 0 ? "null" : Fx(v, 3); }
inline std::string JsonStat(const Stat& s) { return "{\"n\": " + std::to_string(s.n) + ", \"avg_ms\": " + JsonNum(s.avg) + ", \"p99_ms\": " + JsonNum(s.p99) + ", \"max_ms\": " + JsonNum(s.max) + "}"; }

inline std::string ResultsJson(const Info& i, const std::vector<SceneResult>& results) {
  std::string j = "{\n \"benchmark\": \"lesnar_bryan\",\n \"version\": " + JsonStr(i.version) + ",\n \"made\": " + JsonStr(i.made) + ",\n \"hardware\": {\"gpu\": " + JsonStr(i.gpu) +
                  ", \"driver_api\": " + JsonStr(i.driver_and_api) + ", \"cpu\": " + JsonStr(i.cpu) + ", \"ram\": " + JsonStr(i.ram) + ", \"windows\": " + JsonStr(i.windows) +
                  ", \"resolution\": " + JsonStr(i.resolution) + ", \"tier\": " + JsonStr(i.tier) + ", \"thermal\": " + JsonStr(i.thermal) + "},\n \"scenes\": [";
  for (size_t k = 0; k < results.size(); ++k) {
    const SceneResult& r = results[k];
    j += std::string(k ? "," : "") + "\n  {\"id\": " + JsonStr(r.id) + ", \"error\": " + JsonStr(r.error) + ", \"frames\": " + std::to_string(r.frames) + ", \"shown_fps\": " + JsonNum(r.shown_fps) +
         ", \"shown_low1_fps\": " + JsonNum(r.shown_low1_fps) + ", \"swap_fps\": " + JsonNum(r.swap_fps) + ", \"swap_low1_fps\": " + JsonNum(r.swap_low1_fps) + ", \"game\": " + JsonStat(r.game_ms) +
         ", \"render_thread\": " + JsonStat(r.render_ms) + ", \"gpu\": " + JsonStat(r.gpu_ms) + "}";
  }
  return j + "\n ]\n}\n";
}

}  // namespace launcher::bench
