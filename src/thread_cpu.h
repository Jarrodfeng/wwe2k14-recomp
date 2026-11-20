#pragma once
// wwe2k14 - S105 A10: which thread of this process holds a core, one log line every ~10 s (the game: next to [perf][sys] on the perf_mem thread;
// the phone launcher: its own sampler thread, "threads:" in launcher.log), and the "Threads" section of the phone report.
//
//   [wwe2k14][perf][threads] t=22.2 s win=10.0 s proc=187% threads=63 top: SDLThread#29210 52.0% w=1004/s; XThreadE67A8480#29230 41.3% w=12/s; ... (+6 under 1%: 6.2%)
//
// proc     the summed CPU of the process's threads over the window, 100 = one full core (can pass 100)
// name#tid a thread id is kept because Android cuts comm to 15 characters (three guest threads read "XThreadE67A8480")
// 52.0%    that thread's CPU in the window, 100% = one full core
// w=1004/s context switches per second (voluntary + involuntary, /proc/self/task/TID/status): a thread parked in a 1 ms polled wait reads ~1000/s
//          at a low %. Left out where the platform has no cheap count (Windows).
// Listed: threads at >= 1 % or >= 300 wakes/s, at most 10, busiest first; the rest are summed in "(+N under 1%: X%)".
//
// Android / Linux: /proc/self/task/TID/stat (utime + stime in _SC_CLK_TCK ticks; comm = between the first '(' and the last ')') and .../status.
// Windows (thread_cpu_win.h, included by perf_mem.h): Toolhelp + GetThreadTimes. Cost: one read per thread per window; no thread of its own.
// The pure parts (ParseStat, ParseStatus, Compute, Format, ParseLine, Section) compile everywhere and are what tools\tests\thread_cpu_test.cpp checks.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <dirent.h>
#include <unistd.h>
#endif

namespace wwe2k14::thread_cpu {

struct Raw {
  std::string name;
  uint64_t ticks = 0;  // CPU time in 1/hz seconds
  uint64_t start = 0;  // stat field 22 (start time): with the name it tells a reused tid from the old thread
  int64_t ctx = -1;    // voluntary + involuntary context switches; -1 = not known
};
struct Snapshot {
  std::map<int32_t, Raw> t;
  double hz = 100.0;
};
struct Row {
  std::string name;
  int32_t tid = 0;
  double pct = 0;     // 100 = one full core
  double wakes = -1;  // per second; -1 = not known
};
struct Window {
  double t = 0, win = 0;
  double proc = 0;  // % of one core, summed over every thread
  int threads = 0;
  std::vector<Row> top;
  int rest_n = 0;
  double rest_pct = 0;
};

// "tid (comm) state ppid ..." - comm may hold spaces and parentheses, so the fields start after the LAST ')'. utime is field 14, stime 15.
inline bool ParseStat(const char* text, std::string& name, uint64_t& ticks, uint64_t* start = nullptr) {
  const char* open = std::strchr(text, '(');
  const char* close = std::strrchr(text, ')');
  if (!open || !close || close < open || !close[1] || !close[2]) return false;
  unsigned long long utime = 0, stime = 0, starttime = 0;
  const int got = std::sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu %*d %*d %*d %*d %*d %*d %llu", &utime, &stime, &starttime);
  if (got < 2) return false;
  if (start) *start = got == 3 ? starttime : 0;
  name.assign(open + 1, close);
  ticks = utime + stime;
  return true;
}

// "voluntary_ctxt_switches:\t123\nnonvoluntary_ctxt_switches:\t4\n" -> 127. false when neither line is there.
inline bool ParseStatus(const char* text, int64_t& ctx) {
  int64_t sum = 0;
  int found = 0;
  for (const char* key : {"voluntary_ctxt_switches:", "nonvoluntary_ctxt_switches:"}) {
    // "voluntary" is a suffix of "nonvoluntary": match at a line start only.
    for (const char* p = text; (p = std::strstr(p, key)) != nullptr; p += 1) {
      if (p != text && p[-1] != '\n') continue;
      sum += std::strtoll(p + std::strlen(key), nullptr, 10);
      ++found;
      break;
    }
  }
  if (!found) return false;
  ctx = sum;
  return true;
}

// The change between two snapshots over `win_s` seconds. A thread that is new (or whose id was reused under another name) has no baseline: it
// is left out this window. A thread that left is left out too.
inline Window Compute(const Snapshot& prev, const Snapshot& cur, double win_s, double t) {
  Window w;
  w.t = t, w.win = win_s, w.threads = int(cur.t.size());
  if (win_s <= 0 || cur.hz <= 0) return w;
  std::vector<Row> rows;
  for (const auto& [tid, c] : cur.t) {
    const auto it = prev.t.find(tid);
    if (it == prev.t.end() || it->second.name != c.name || it->second.start != c.start) continue;
    const Raw& p = it->second;
    Row r;
    r.name = c.name, r.tid = tid;
    const uint64_t d = c.ticks >= p.ticks ? c.ticks - p.ticks : 0;
    r.pct = double(d) / cur.hz / win_s * 100.0;
    if (c.ctx >= 0 && p.ctx >= 0) r.wakes = double(c.ctx >= p.ctx ? c.ctx - p.ctx : 0) / win_s;
    w.proc += r.pct;
    rows.push_back(std::move(r));
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.pct > b.pct; });
  for (Row& r : rows) {
    if ((r.pct >= 1.0 || r.wakes >= 300.0) && w.top.size() < 10) w.top.push_back(std::move(r));
    else ++w.rest_n, w.rest_pct += r.pct;
  }
  return w;
}

// The text after the marker: "t=22.2 s win=10.0 s proc=187% threads=63 top: ...".
inline std::string Format(const Window& w) {
  char head[128];
  std::snprintf(head, sizeof(head), "t=%.1f s win=%.1f s proc=%.0f%% threads=%d top: ", w.t, w.win, w.proc, w.threads);
  std::string s = head;
  if (w.top.empty()) s += "(none)";
  for (size_t i = 0; i < w.top.size(); ++i) {
    const Row& r = w.top[i];
    char one[256];
    if (r.wakes >= 0) std::snprintf(one, sizeof(one), "%s#%d %.1f%% w=%.0f/s", r.name.c_str(), r.tid, r.pct, r.wakes);
    else std::snprintf(one, sizeof(one), "%s#%d %.1f%%", r.name.c_str(), r.tid, r.pct);
    if (i) s += "; ";
    s += one;
  }
  if (w.rest_n > 0) {
    char tail[64];
    std::snprintf(tail, sizeof(tail), " (+%d under 1%%: %.1f%%)", w.rest_n, w.rest_pct);
    s += tail;
  }
  return s;
}

// Reads a log line that carries `marker` ("[wwe2k14][perf][threads] " or "threads: ") + Format's text. false when the line is not one.
inline bool ParseLine(const std::string& line, const char* marker, Window& w) {
  const size_t m = line.find(marker);
  if (m == std::string::npos) return false;
  const size_t after = m + std::strlen(marker);
  if (line.compare(after, 2, "t=") != 0) return false;
  w = Window{};
  w.t = std::atof(line.c_str() + after + 2);
  if (const size_t p = line.find("win=", after); p != std::string::npos) w.win = std::atof(line.c_str() + p + 4);
  if (const size_t p = line.find("proc=", after); p != std::string::npos) w.proc = std::atof(line.c_str() + p + 5);
  if (const size_t p = line.find("threads=", after); p != std::string::npos) w.threads = std::atoi(line.c_str() + p + 8);
  const size_t top = line.find(" top: ", after);
  if (top == std::string::npos) return true;
  std::string list = line.substr(top + 6);
  while (!list.empty() && (list.back() == '\r' || list.back() == '\n')) list.pop_back();
  if (const size_t tail = list.rfind(" (+"); tail != std::string::npos) {
    w.rest_n = std::atoi(list.c_str() + tail + 3);
    if (const size_t c = list.find(": ", tail); c != std::string::npos) w.rest_pct = std::atof(list.c_str() + c + 2);
    list.resize(tail);
  }
  if (list == "(none)") return true;
  size_t from = 0;
  while (from <= list.size()) {
    size_t to = list.find("; ", from);
    if (to == std::string::npos) to = list.size();
    std::string e = list.substr(from, to - from);
    from = to + 2;
    Row r;
    if (const size_t wk = e.rfind(" w="); wk != std::string::npos) {  // " w=1004/s"
      r.wakes = std::atof(e.c_str() + wk + 3);
      e.resize(wk);
    }
    const size_t sp = e.rfind(' ');
    if (sp == std::string::npos || e.empty() || e.back() != '%') continue;
    r.pct = std::atof(e.c_str() + sp + 1);
    const std::string key = e.substr(0, sp);
    const size_t hash = key.rfind('#');
    if (hash == std::string::npos) continue;
    r.name = key.substr(0, hash), r.tid = std::atoi(key.c_str() + hash + 1);
    w.top.push_back(std::move(r));
  }
  return true;
}

struct ThreadSummary {
  std::string key;  // name#tid
  double max_pct = 0, sum_pct = 0, max_wakes = -1;
  int listed = 0;
  bool spinning = false;
  double spin_t = 0;  // t of the second window of the first >= 90 % pair
};

constexpr double kSpinPct = 90.0;

// Per thread: highest and mean % over the windows that listed it, the highest wake rate, and SPINNING = >= 90 % of a core in two windows in a row.
inline std::vector<ThreadSummary> Summarize(const std::vector<Window>& wins) {
  std::map<std::string, ThreadSummary> by;
  std::map<std::string, bool> was_hot;  // in the previous window
  for (const Window& w : wins) {
    std::map<std::string, bool> hot_now;
    for (const Row& r : w.top) {
      const std::string key = r.name + "#" + std::to_string(r.tid);
      ThreadSummary& s = by[key];
      s.key = key;
      s.max_pct = std::max(s.max_pct, r.pct), s.sum_pct += r.pct, ++s.listed;
      s.max_wakes = std::max(s.max_wakes, r.wakes);
      const bool hot = r.pct >= kSpinPct;
      hot_now[key] = hot;
      if (hot && was_hot[key] && !s.spinning) s.spinning = true, s.spin_t = w.t;
    }
    was_hot = hot_now;
  }
  std::vector<ThreadSummary> out;
  for (auto& kv : by) out.push_back(kv.second);
  std::sort(out.begin(), out.end(), [](const ThreadSummary& a, const ThreadSummary& b) { return a.max_pct > b.max_pct; });
  return out;
}

// The report.txt section. `game` / `launcher` are the windows read from the game log and launcher.log (either may be empty).
inline std::string Section(const std::vector<Window>& game, const std::vector<Window>& launcher) {
  std::string r = "\nThreads (share of one core)\n---------------------------\n";
  auto part = [&](const char* title, const std::vector<Window>& wins) {
    char buf[256];
    if (wins.empty()) {
      std::snprintf(buf, sizeof(buf), "%s: no thread lines in this log\n", title);
      r += buf;
      return;
    }
    double max_proc = 0;
    for (const Window& w : wins) max_proc = std::max(max_proc, w.proc);
    std::snprintf(buf, sizeof(buf), "%s: %zu windows of ~%.0f s, process total up to %.0f%% of one core\n", title, wins.size(),
                  wins.front().win, max_proc);
    r += buf;
    const std::vector<ThreadSummary> sum = Summarize(wins);
    int shown = 0;
    for (const ThreadSummary& s : sum) {
      if (shown++ >= 6) break;
      std::snprintf(buf, sizeof(buf), "  %s: highest %.1f%%, mean %.1f%% over %d windows listed", s.key.c_str(), s.max_pct,
                    s.sum_pct / std::max(1, s.listed), s.listed);
      r += buf;
      if (s.max_wakes >= 0) {
        std::snprintf(buf, sizeof(buf), ", wakes up to %.0f/s", s.max_wakes);
        r += buf;
      }
      r += "\n";
    }
    double wake_max = -1;
    std::string wake_key;
    for (const ThreadSummary& s : sum)
      if (s.max_wakes > wake_max) wake_max = s.max_wakes, wake_key = s.key;
    if (wake_max >= 300) {
      std::snprintf(buf, sizeof(buf), "  most wakes: %s at %.0f/s (a thread parked in a polled wait wakes ~1000/s at a low %%)\n",
                    wake_key.c_str(), wake_max);
      r += buf;
    }
    for (const ThreadSummary& s : sum) {
      if (!s.spinning) continue;
      std::snprintf(buf, sizeof(buf), "  SPINNING (%s): %s at up to %.0f%% of a core, 2 windows in a row by t=%.0f s\n", title,
                    s.key.c_str(), s.max_pct, s.spin_t);
      r += buf;
    }
  };
  part("game", game);
  part("launcher", launcher);
  return r;
}

// Reads a whole small file (a /proc status file is ~1.3 KB and its context-switch counters are the LAST lines: one short fread lost them).
// Returns the bytes read, NUL-terminated; 0 when the file cannot be opened.
inline size_t ReadProcFile(const char* path, char* buf, size_t cap) {
  FILE* f = std::fopen(path, "r");
  if (!f) return 0;
  size_t n = 0, got;
  while (n + 1 < cap && (got = std::fread(buf + n, 1, cap - 1 - n, f)) > 0) n += got;
  std::fclose(f);
  buf[n] = 0;
  return n;
}

#if !defined(_WIN32)
// Android / Linux: this process's threads.
inline Snapshot TakeSnapshot() {
  Snapshot s;
  const long hz = sysconf(_SC_CLK_TCK);
  s.hz = hz > 0 ? double(hz) : 100.0;
  DIR* dir = opendir("/proc/self/task");
  if (!dir) return s;
  while (dirent* e = readdir(dir)) {
    const int32_t tid = std::atoi(e->d_name);
    if (tid <= 0) continue;
    char path[64], buf[4096];
    Raw r;
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
    if (!ReadProcFile(path, buf, sizeof(buf)) || !ParseStat(buf, r.name, r.ticks, &r.start)) continue;
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/status", tid);
    if (ReadProcFile(path, buf, sizeof(buf))) ParseStatus(buf, r.ctx);
    s.t[tid] = std::move(r);
  }
  closedir(dir);
  return s;
}
#endif

// One reading per call; the first call only takes the baseline.
class Meter {
 public:
  // `at_s` = a steady clock in seconds, `t_log` = the number the line prints as t=.
  bool Step(const Snapshot& cur, double at_s, double t_log, Window& out) {
    const bool had = have_;
    if (had) out = Compute(prev_, cur, at_s - prev_at_, t_log);
    prev_ = cur, prev_at_ = at_s, have_ = true;
    return had;
  }

 private:
  Snapshot prev_;
  double prev_at_ = 0;
  bool have_ = false;
};

inline double SteadySeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

#if !defined(_WIN32)
inline bool StepNow(Meter& m, double t_log, Window& out) { return m.Step(TakeSnapshot(), SteadySeconds(), t_log, out); }
#endif

}  // namespace wwe2k14::thread_cpu
