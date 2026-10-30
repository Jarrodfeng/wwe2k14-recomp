#pragma once
// Session 73 (Jarrod: one file per tester report, the most important first): report.txt, the first entry of the zip
// that SHARE THE LAST GAME LOG sends (mobile_main.cpp ShareLogs). Plain text, built from what the last game run left:
//   1. the build, the phone, Android's reason for the run's end and any crash (logs/startup.txt, last_game_exit.txt)
//   2. the settings the launcher passed (the game log's "modified: NAME = 'VALUE' (source: command line)" lines)
//   3. the run: its length and the 5-second windows at normal speed (the game log's "[wwe2k14][frames] t=..." lines)
//   4. the worst 5 moments (frames_NNN.csv, plus the game log's "[wwe2k14][frames] slow frame N:" lines)
//   5. heat ("[wwe2k14][thermal]" lines)
// Every part copes with logs that lack its lines (older builds): it says "not in this log".
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace mlauncher::report {

namespace fs = std::filesystem;

inline std::string Fmt(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return buf;
}

inline std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
  size_t a = 0;
  while (a < s.size() && s[a] == ' ') ++a;
  return s.substr(a);
}

// The text after `key` on the first line of `file` that has it (empty when none).
inline std::string FirstAfter(const fs::path& file, const std::string& key, bool at_start = false) {
  std::ifstream in(file);
  for (std::string line; std::getline(in, line);) {
    const size_t at = line.find(key);
    if (at == std::string::npos || (at_start && at != 0)) continue;
    return Trim(line.substr(at + key.size()));
  }
  return std::string();
}

// One "[wwe2k14][frames] t=25 s, frames 4799-5312: 102.7 fps, avg 9.7 ms, p95 37.7 ms, worst 167 ms, hitches 10".
struct Window {
  double t = 0, fps = 0, avg = 0, p95 = 0, worst = 0;
  long first = 0, last = 0;
  int hitches = 0;
};

// One "[wwe2k14][thermal] t=125 s: status 2 (moderate), headroom 0.83; CPU 0-5 2.21 GHz, 6-7 3.53 GHz".
struct Heat {
  double t = 0;
  int status = -1;
  std::string status_name;
  double headroom = -1;
  std::string top_cores;
  double top_ghz = -1;
  int gpu_mhz = -1, gpu_busy = -1;  // b77: "; GPU 680 MHz, busy 45%" where the phone lets the game read them
};

struct GameLog {
  std::string version;
  std::vector<std::pair<std::string, std::string>> settings;
  std::vector<Window> windows;
  std::vector<Heat> heat;
  std::vector<std::string> gpu_notes;  // b77: the GPU clock source, GPU time reports, keep-warm switches
  int warm_switches = 0;
  std::map<long, std::string> slow;  // "slow frame N:" -> the text after "ms (t=...):"
};

inline void ReadGameLog(const fs::path& file, GameLog& g) {
  std::ifstream in(file);
  static const char* const kSkip[] = {"game_data_root", "wwe2k14_android_lib_dir", "wwe2k14_android_tmp_dir"};
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (g.version.empty()) {
      if (const size_t at = line.find("[wwe2k14] version "); at != std::string::npos) g.version = Trim(line.substr(at + 18));
    }
    if (const size_t at = line.find("modified: "); at != std::string::npos &&
                                                   line.find("(source: command line)") != std::string::npos) {
      const std::string rest = line.substr(at + 10);
      const size_t eq = rest.find(" = '");
      const size_t end = rest.rfind("' (source:");
      if (eq != std::string::npos && end != std::string::npos && end >= eq + 4) {
        const std::string name = rest.substr(0, eq);
        if (std::find(std::begin(kSkip), std::end(kSkip), name) == std::end(kSkip)) {
          bool seen = false;
          for (auto& s : g.settings)
            if (s.first == name) s.second = rest.substr(eq + 4, end - eq - 4), seen = true;
          if (!seen) g.settings.emplace_back(name, rest.substr(eq + 4, end - eq - 4));
        }
      }
      continue;
    }
    if (const size_t at = line.find("[wwe2k14][frames] t="); at != std::string::npos) {
      Window w;
      if (std::sscanf(line.c_str() + at + 20, "%lf s, frames %ld-%ld: %lf fps, avg %lf ms, p95 %lf ms, worst %lf ms, hitches %d",
                      &w.t, &w.first, &w.last, &w.fps, &w.avg, &w.p95, &w.worst, &w.hitches) >= 4)
        g.windows.push_back(w);
      continue;
    }
    if (const size_t at = line.find("[wwe2k14][frames] slow frame "); at != std::string::npos) {
      const long n = std::strtol(line.c_str() + at + 29, nullptr, 10);
      const size_t colon = line.find("): ", at);
      if (n > 0 && colon != std::string::npos) g.slow[n] = Trim(line.substr(colon + 3));
      continue;
    }
    if (const size_t at = line.find("[wwe2k14][thermal] t="); at != std::string::npos) {
      Heat h;
      const char* p = line.c_str() + at + 21;
      h.t = std::atof(p);
      if (const size_t s = line.find("status ", at); s != std::string::npos) {
        h.status = std::atoi(line.c_str() + s + 7);
        const size_t open = line.find('(', s), close = line.find(')', s);
        if (open != std::string::npos && close != std::string::npos && close > open && open < s + 12)
          h.status_name = line.substr(open + 1, close - open - 1);
      }
      if (const size_t s = line.find("headroom ", at); s != std::string::npos) {
        char* endp = nullptr;
        const double v = std::strtod(line.c_str() + s + 9, &endp);
        if (endp != line.c_str() + s + 9) h.headroom = v;
      }
      // The top cluster: the last "CORES X.XX GHz" group of the line.
      if (const size_t ghz = line.rfind(" GHz"); ghz != std::string::npos && line.find("CPU ", at) < ghz) {
        size_t a = line.rfind(' ', ghz - 1);
        if (a != std::string::npos) {
          h.top_ghz = std::atof(line.c_str() + a + 1);
          size_t b = line.rfind(' ', a - 1);
          if (b != std::string::npos) h.top_cores = line.substr(b + 1, a - b - 1);
        }
      }
      if (const size_t s = line.find("; GPU ", at); s != std::string::npos && line.find(" MHz", s) != std::string::npos)
        h.gpu_mhz = std::atoi(line.c_str() + s + 6);
      if (const size_t s = line.find("busy ", at); s != std::string::npos) h.gpu_busy = std::atoi(line.c_str() + s + 5);
      g.heat.push_back(h);
      continue;
    }
    if (line.find("[wwe2k14][warm] keep-warm") != std::string::npos) {
      ++g.warm_switches;
      continue;
    }
    for (const char* key : {"[wwe2k14][frames] GPU clock from ", "[wwe2k14][frames] the GPU clock is not readable",
                            "[wwe2k14][perf_hint] GPU time reports", "[wwe2k14][perf_hint] reporting the GPU time",
                            "[warm] keep-warm buffer"}) {
      if (const size_t at = line.find(key); at != std::string::npos) {
        g.gpu_notes.push_back(Trim(line.substr(at)));
        break;
      }
    }
  }
}

// frames_NNN.csv: parsed by its header's names (frame,t_seconds,frame_ms[,game_cpu_ms,gpu_thread_cpu_ms,gpu_ms,
// draw_rows,scene]; the first test builds wrote frame,t,ms). A missing or empty value is NaN / empty text.
struct Frame {
  long frame = 0;
  double t = 0, ms = 0, game_cpu = NAN, gpu_thread = NAN, gpu = NAN, draws = NAN, gpu_mhz = NAN;
  std::string scene;
};

inline std::vector<std::string> SplitCsv(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  bool quoted = false;
  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (quoted) {
      if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') cur += '"', ++i;
      else if (c == '"') quoted = false;
      else cur += c;
    } else if (c == '"') {
      quoted = true;
    } else if (c == ',') {
      out.push_back(cur), cur.clear();
    } else if (c != '\r') {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

inline std::vector<Frame> ReadFrames(const fs::path& file, bool& has_cpu_gpu) {
  std::vector<Frame> out;
  has_cpu_gpu = false;
  std::ifstream in(file);
  std::string line;
  if (!std::getline(in, line)) return out;
  const std::vector<std::string> head = SplitCsv(line);
  auto col = [&](std::initializer_list<const char*> names) {
    for (size_t i = 0; i < head.size(); ++i)
      for (const char* n : names)
        if (Trim(head[i]) == n) return int(i);
    return -1;
  };
  const int c_frame = col({"frame"}), c_t = col({"t_seconds", "t"}), c_ms = col({"frame_ms", "ms"});
  const int c_cpu = col({"game_cpu_ms"}), c_gt = col({"gpu_thread_cpu_ms"}), c_gpu = col({"gpu_ms"});
  const int c_draws = col({"draw_rows"}), c_scene = col({"scene"}), c_mhz = col({"gpu_mhz"});
  if (c_frame < 0 || c_ms < 0) return out;
  has_cpu_gpu = c_cpu >= 0 || c_gt >= 0 || c_gpu >= 0;
  auto num = [](const std::vector<std::string>& f, int c) {
    if (c < 0 || c >= int(f.size()) || Trim(f[c]).empty()) return double(NAN);
    return std::atof(f[c].c_str());
  };
  while (std::getline(in, line)) {
    if (line.empty() || line == "\r") continue;
    const std::vector<std::string> f = SplitCsv(line);
    Frame r;
    r.frame = long(num(f, c_frame));
    r.t = num(f, c_t), r.ms = num(f, c_ms);
    if (std::isnan(r.ms)) continue;
    r.game_cpu = num(f, c_cpu), r.gpu_thread = num(f, c_gt), r.gpu = num(f, c_gpu), r.draws = num(f, c_draws), r.gpu_mhz = num(f, c_mhz);
    if (c_scene >= 0 && c_scene < int(f.size())) r.scene = Trim(f[c_scene]);
    out.push_back(std::move(r));
  }
  return out;
}

struct Inputs {
  std::string launcher_version;
  std::string last_run;                // check::LastRun's plain words (empty when the run ended normally)
  fs::path logs_dir;                   // files/logs
  std::vector<fs::path> game_logs;     // oldest first (a rotated log's oldest kept part, then the newest)
  fs::path frames_csv;                 // may not exist
};

inline std::string Build(const Inputs& in) {
  std::string r;
  const std::string kNot = "not in this log";
  GameLog g;
  for (const auto& f : in.game_logs) ReadGameLog(f, g);

  // 1. Build, phone, end of the run.
  r += "WWE 2K14 phone report\n=====================\n";
  r += "Launcher: " + in.launcher_version + "\n";
  r += "Game log: " + (in.game_logs.empty() ? std::string("none yet") : in.game_logs.back().filename().string()) +
       (g.version.empty() ? "" : " (game " + g.version + ")") + "\n";
  const fs::path startup = in.logs_dir / "startup.txt", exit_file = in.logs_dir / "last_game_exit.txt";
  const std::string phone = FirstAfter(startup, "phone: ");
  r += "Phone: " + (phone.empty() ? std::string("not in startup.txt") : phone) + "\n";
  std::string reason = FirstAfter(exit_file, "reason: ", true);
  if (!reason.empty()) {
    if (const std::string d = FirstAfter(exit_file, "description: ", true); !d.empty()) reason += " (" + d + ")";
    if (const std::string s = FirstAfter(exit_file, "time: ", true); !s.empty()) reason += ", " + s;
  }
  r += "Android's exit reason: " + (reason.empty() ? std::string("none recorded") : reason) + "\n";
  {
    std::string crash, last_step;
    std::ifstream box(startup);
    for (std::string line; std::getline(box, line);) {
      if (const size_t at = line.find("CRASH: "); at != std::string::npos) crash = Trim(line.substr(at));
      if (const size_t at = line.find("step: "); at != std::string::npos) last_step = Trim(line.substr(at + 6));
    }
    if (!crash.empty()) r += crash + "\n";
    if (!last_step.empty()) r += "Last start-up step: " + last_step + "\n";
  }
  if (!in.last_run.empty()) r += "Launcher's verdict: " + in.last_run + "\n";

  // 2. Settings the launcher passed.
  r += "\nSettings (from the launcher)\n----------------------------\n";
  if (g.settings.empty()) r += kNot + "\n";
  for (const auto& s : g.settings) r += s.first + " = " + s.second + "\n";

  // 3. The run.
  r += "\nRun\n---\n";
  if (g.windows.empty()) {
    r += "frame rate lines: " + kNot + "\n";
  } else {
    const Window& lastw = g.windows.back();
    r += Fmt("Length: %.0f s of game, %ld frames (the log's 5 s windows: %zu)\n", lastw.t, lastw.last,
             g.windows.size());
    double frames = 0, secs = 0;
    int normal = 0, hitches = 0;
    const Window* worst = nullptr;
    for (const Window& w : g.windows) {
      if (w.fps > 70 || w.fps <= 0) continue;  // fast-forwarded loading screens
      ++normal, hitches += w.hitches;
      const double n = double(w.last - w.first + 1);
      frames += n, secs += n / w.fps;
      if (!worst || w.fps < worst->fps) worst = &w;
    }
    if (!normal) {
      r += "No window at normal speed (all over 70 fps: loading screens only)\n";
    } else {
      r += Fmt("At normal speed: %d windows (%zu fast-forwarded ones over 70 fps skipped), %.1f fps on average, "
               "%d hitches\n",
               normal, g.windows.size() - size_t(normal), secs > 0 ? frames / secs : 0.0, hitches);
      r += Fmt("Worst window: t=%.0f s (frames %ld-%ld): %.1f fps, avg %.1f ms, p95 %.1f ms, worst %.0f ms, "
               "hitches %d\n",
               worst->t, worst->first, worst->last, worst->fps, worst->avg, worst->p95, worst->worst, worst->hitches);
    }
  }

  // 4. Worst 5 moments.
  r += "\nWorst 5 moments\n---------------\n";
  bool has_cpu_gpu = false;
  std::error_code ec;
  const std::vector<Frame> fr = fs::is_regular_file(in.frames_csv, ec) ? ReadFrames(in.frames_csv, has_cpu_gpu)
                                                                      : std::vector<Frame>();
  if (fr.empty()) {
    r += in.frames_csv.filename().string() + ": " +
         (fs::is_regular_file(in.frames_csv, ec) ? std::string("no frames in it") : std::string("not found")) + "\n";
  } else {
    // Frames in a fast-forwarded stretch (their +-30 neighbours average under 10 ms) are loading screens: skipped.
    const size_t n = fr.size();
    std::vector<double> sum(n + 1, 0);
    for (size_t i = 0; i < n; ++i) sum[i + 1] = sum[i] + fr[i].ms;
    std::vector<size_t> cand;
    for (size_t i = 0; i < n; ++i) {
      const size_t a = i >= 30 ? i - 30 : 0, b = std::min(n, i + 31);
      const size_t count = b - a - 1;
      if (count == 0) continue;
      const double mean = (sum[b] - sum[a] - fr[i].ms) / double(count);
      if (mean >= 10) cand.push_back(i);
    }
    std::sort(cand.begin(), cand.end(), [&](size_t x, size_t y) { return fr[x].ms > fr[y].ms; });
    // A moment is a frame over 1.5x the usual frame time (the median at normal speed): no ordinary frames listed.
    const double usual = cand.empty() ? 0 : fr[cand[cand.size() / 2]].ms;
    std::vector<size_t> pick;
    for (size_t i : cand) {
      if (pick.size() >= 5 || fr[i].ms < usual * 1.5) break;
      bool close_by = false;  // ("near" is a macro in windows.h)
      for (size_t p : pick)
        if (std::labs(fr[p].frame - fr[i].frame) < 60) close_by = true;
      if (!close_by) pick.push_back(i);
    }
    if (pick.empty()) r += "no slow frames at normal speed in " + in.frames_csv.filename().string() + "\n";
    else r += Fmt("(usual frame %.1f ms; frames over %.0f ms, 60+ frames apart)\n", usual, usual * 1.5);
    int k = 0;
    for (size_t i : pick) {
      const Frame& f = fr[i];
      r += Fmt("%d. frame %ld at t=%.1f s: %.0f ms", ++k, f.frame, f.t, f.ms);
      if (has_cpu_gpu) {
        auto v = [](double x) { return std::isnan(x) ? std::string("?") : Fmt("%.1f", x); };
        r += " (game CPU " + v(f.game_cpu) + " ms, GPU thread " + v(f.gpu_thread) + " ms, GPU " + v(f.gpu) + " ms";
        if (!std::isnan(f.gpu_mhz)) r += Fmt(", GPU clock %.0f MHz", f.gpu_mhz);
        if (!std::isnan(f.draws)) r += Fmt(", %.0f draw rows", f.draws);
        r += ")";
      }
      for (size_t j = i + 1; j-- > 0;) {
        if (fr[j].scene.empty()) continue;
        r += Fmt(", %.1f s after ", f.t - fr[j].t) + fr[j].scene;
        break;
      }
      r += "\n";
      if (auto s = g.slow.find(f.frame); s != g.slow.end()) r += "   " + s->second + "\n";
    }
  }

  // 5. Heat.
  r += "\nHeat\n----\n";
  if (g.heat.empty()) {
    r += "thermal lines: " + kNot + "\n";
  } else {
    const Heat *st = nullptr, *hr = nullptr, *clk = nullptr;
    for (const Heat& h : g.heat) {
      if (h.status >= 0 && (!st || h.status > st->status)) st = &h;
      if (h.headroom >= 0 && (!hr || h.headroom < hr->headroom)) hr = &h;
      if (h.top_ghz > 0 && (!clk || h.top_ghz < clk->top_ghz)) clk = &h;
    }
    r += st ? Fmt("Worst status: %d%s at t=%.0f s\n", st->status,
                  st->status_name.empty() ? "" : (" (" + st->status_name + ")").c_str(), st->t)
            : "Worst status: " + kNot + "\n";
    r += hr ? Fmt("Lowest headroom: %.2f at t=%.0f s\n", hr->headroom, hr->t) : "Lowest headroom: " + kNot + "\n";
    r += clk ? Fmt("Lowest top-cluster clock: %.2f GHz (CPU %s) at t=%.0f s\n", clk->top_ghz, clk->top_cores.c_str(),
                   clk->t)
             : "Lowest top-cluster clock: " + kNot + "\n";
    r += Fmt("(%zu thermal lines)\n", g.heat.size());
  }

  // 6. The GPU's clock (b77: each entrance's first seconds ran the GPU slowly in Jarrod's b76 log).
  r += "\nGPU\n---\n";
  const Heat *lo = nullptr, *hi = nullptr;
  for (const Heat& h : g.heat) {
    if (h.gpu_mhz > 0 && (!lo || h.gpu_mhz < lo->gpu_mhz)) lo = &h;
    if (h.gpu_mhz > 0 && (!hi || h.gpu_mhz > hi->gpu_mhz)) hi = &h;
  }
  if (lo) {
    r += Fmt("GPU clock (every 5 s): lowest %d MHz at t=%.0f s, highest %d MHz at t=%.0f s\n", lo->gpu_mhz, lo->t,
             hi->gpu_mhz, hi->t);
  }
  for (const std::string& n : g.gpu_notes) r += n + "\n";
  if (g.warm_switches) r += Fmt("Keep-warm switched on / off %d times (the log's [warm] lines)\n", g.warm_switches);
  if (!lo && g.gpu_notes.empty()) r += "GPU lines: " + kNot + "\n";
  return r;
}

}  // namespace mlauncher::report
