#pragma once
// Session 95 (LADDER): a repeat failure offers a way out by itself. How the last game run ended is classified from the
// files the game and Android leave (logs/startup.txt = src\blackbox.h, logs/stderr.txt, logs/last_game_exit.txt =
// LauncherActivity.writeLastGameExit), one row per run goes into logs/run_history.txt, and the rows decide which card the
// launcher shows: SAFE MODE after two named bad ends in a row, "did the picture look right?" after an early quit, the
// memory card after two foreground low-memory kills. Pure C++ (no SDL, no ImGui) so the PC test (tools\android\ladder_test.cpp)
// runs it on planted files.
//
// What counts as a bad end (the plan's section 7, piece 6): only NAMED ones - a CRASH line, a fatal (assert.h's FATAL ERROR,
// "FATAL: GPU lacks"), HANG: / BLACK: lines (HANG's piece writes them), a crash signal in Android's exit status, ANR. UNKNOWN,
// a swipe-away, "ended unexpectedly" and an exit after the quit marker never count (r021 is UNKNOWN status 0 on a modern phone;
// r048 / r192 are SIGKILL after "the player quit to the launcher": the game ends itself that way).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace mlauncher::ladder {

namespace fs = std::filesystem;

inline constexpr int kMaxRung = 4;
inline constexpr double kEarlyQuitSeconds = 120.0;

struct RunEnd {
  enum Kind { kNone, kGood, kBad, kMemory, kUnknown, kPrecompile };
  Kind kind = kNone;
  std::string name;      // bad: CRASH / FATAL / HANG / BLACK / ANR; memory: LOW_MEMORY
  std::string key;       // one run: "<log number>@<startup.txt seconds>"
  bool played = false;   // the first frame was reached
  bool boot_note = false;  // a start-up step line exists
  bool hang_pending = false;  // a "HANG?" line (HANG's maybe-frozen warning) with no verdict after it
  bool foreground = false;    // Android's importance for the process was foreground when it ended
  double play_seconds = -1;   // first frame to the quit marker; -1 when unknown
};

inline std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
  size_t i = 0;
  while (i < s.size() && s[i] == ' ') ++i;
  return s.substr(i);
}

// A black box line is "%8.3f  text" (native) or the same with "java" notes; the time or -1.
inline double LineTime(const std::string& line, std::string* text = nullptr) {
  const char* p = line.c_str();
  char* end = nullptr;
  const double t = std::strtod(p, &end);
  if (end == p) {
    if (text) *text = Trim(line);
    return -1;
  }
  if (text) *text = Trim(std::string(end));
  return t;
}

inline std::string NewestLogNumber(const fs::path& logs) {
  std::error_code ec;
  std::string best;
  for (const auto& e : fs::directory_iterator(logs, ec)) {
    const std::string n = e.path().filename().string();
    if (n.rfind("wwe2k14_", 0) != 0 || e.path().extension() != ".log") continue;
    const std::string stem = e.path().stem().string();
    if (stem.find('.') != std::string::npos) continue;
    if (stem > best) best = stem;  // (zero padded: the text order is the number order)
  }
  return best.empty() ? std::string("none") : best.substr(8);
}

inline RunEnd Classify(const fs::path& files, int sdk_level) {
  RunEnd r;
  const fs::path logs = files / "logs";
  std::ifstream box(logs / "startup.txt");
  if (!box) return r;
  std::error_code ec1, ec2;
  const auto start_time = fs::last_write_time(logs / "startup.txt", ec1);
  long long stamp = 0;
  if (!ec1) stamp = (long long)std::chrono::duration_cast<std::chrono::seconds>(start_time.time_since_epoch()).count();
  r.key = NewestLogNumber(logs) + "@" + std::to_string(stamp);
  std::string fatal, crash, hang, black, dark, gpu_fatal, line, text;
  bool quit = false, precompile = false;
  double t_first = -1, t_quit = -1;
  while (std::getline(box, line)) {
    const double t = LineTime(line, &text);
    if (text.rfind("step: ", 0) == 0) r.boot_note = true;
    if (text.find("first frame") != std::string::npos) r.played = true, t_first = t;
    if (text.find("quit to the launcher") != std::string::npos) {
      quit = true, t_quit = t;
      if (text.find("shader precompile ended") != std::string::npos) precompile = true;
    }
    if (const size_t at = text.find("CRASH: "); at != std::string::npos) crash = text.substr(at + 7);
    if (const size_t at = text.find("FATAL: GPU lacks "); at != std::string::npos) gpu_fatal = text.substr(at + 17);
    if (const size_t at = text.find("HANG: "); at != std::string::npos) hang = text.substr(at + 6), r.hang_pending = false;
    else if (text.find("HANG?") != std::string::npos) r.hang_pending = true;
    else if (text.find("HANG over") != std::string::npos) hang.clear(), r.hang_pending = false;  // it went on after all
    // The same reading as check::LastRun: a black that ended is gone, a dark picture (a guess from 3 captures) counts
    // only when the run did not end in a quit.
    if (const size_t at = text.find("BLACK: dark picture"); at != std::string::npos) dark = text.substr(at + 7);
    else if (const size_t at2 = text.find("BLACK: "); at2 != std::string::npos) black = text.substr(at2 + 7);
    else if (text.find("BLACK over") != std::string::npos) black.clear();
  }
  {
    std::ifstream err(logs / "stderr.txt");
    std::string e;
    while (std::getline(err, e))
      if (const size_t at = e.find("FATAL ERROR: "); at != std::string::npos) fatal = Trim(e.substr(at + 13));
  }
  std::string reason;
  int status = 0, importance = 1000;
  {
    const auto exit_time = fs::last_write_time(logs / "last_game_exit.txt", ec2);
    std::ifstream exit;
    if (!ec2 && (ec1 || exit_time >= start_time)) exit.open(logs / "last_game_exit.txt");
    while (std::getline(exit, line)) {
      if (line.rfind("reason: ", 0) == 0) reason = Trim(line.substr(8));
      if (line.rfind("status: ", 0) == 0) status = std::atoi(line.c_str() + 8);
      if (line.rfind("importance: ", 0) == 0) importance = std::atoi(line.c_str() + 12);
    }
  }
  r.foreground = importance <= 125;
  if (t_first >= 0 && t_quit >= t_first) r.play_seconds = t_quit - t_first;
  (void)sdk_level;
  auto bad = [&](const char* name) {
    r.name = name;
    // A boot verdict counts only with a boot-stage note (an empty record says nothing about where it died).
    r.kind = (!r.played && !r.boot_note) ? RunEnd::kUnknown : RunEnd::kBad;
  };
  if (precompile) { r.kind = RunEnd::kPrecompile; return r; }
  if (!gpu_fatal.empty()) { bad("FATAL"); return r; }
  if (!hang.empty()) { bad("HANG"); return r; }
  if (!black.empty()) { bad("BLACK"); return r; }
  if (quit) { r.kind = RunEnd::kGood; return r; }
  if (!fatal.empty()) { bad("FATAL"); return r; }
  if (!crash.empty()) { bad("CRASH"); return r; }
  const bool java_crash = reason.rfind("CRASH", 0) == 0;  // (a Java crash in the game process has no signal status)
  const bool signal_crash = java_crash || (reason.rfind("SIGNALED", 0) == 0 &&
                            (status == 4 || status == 5 || status == 6 || status == 7 || status == 8 || status == 11 ||
                             status == 31));
  if (signal_crash) { bad("CRASH"); return r; }
  if (!dark.empty() && reason.rfind("LOW_MEMORY", 0) != 0) { bad("BLACK"); return r; }  // (a memory kill after a dark guess is a memory row, not BLACK)
  if (reason.rfind("ANR", 0) == 0) { bad("ANR"); return r; }
  if (reason.rfind("LOW_MEMORY", 0) == 0) { r.kind = RunEnd::kMemory, r.name = "LOW_MEMORY"; return r; }
  r.kind = RunEnd::kUnknown;
  return r;
}

// ---------------------------------------------------------------------------------------------- the history file
struct Row {
  std::string key, version, driver, hash;
  int rung = 0;
  char cls = 'G';      // G good, B bad (named), M foreground low-memory kill
  std::string name;
  char early = '-';    // E = an early quit, the "did the picture look right?" card is pending; A = answered; - = none
};

inline std::string Clean(std::string s) {
  for (char& c : s)
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  return s.empty() ? std::string("-") : s;
}

inline std::vector<Row> ReadHistory(const fs::path& path) {
  std::vector<Row> rows;
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    std::vector<std::string> p;
    std::stringstream ss(line);
    for (std::string t; std::getline(ss, t, '\t');) p.push_back(t);
    if (p.size() < 8) continue;
    Row r;
    r.key = p[0], r.version = p[1], r.driver = p[2] == "-" ? "" : p[2], r.hash = p[3];
    r.rung = std::atoi(p[4].c_str());
    r.cls = p[5].empty() ? 'G' : p[5][0];
    r.name = p[6] == "-" ? "" : p[6];
    r.early = p[7].empty() ? '-' : p[7][0];
    rows.push_back(r);
  }
  return rows;
}

inline bool WriteHistory(const fs::path& path, std::vector<Row> rows) {
  if (rows.size() > 40) rows.erase(rows.begin(), rows.end() - 40);
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream f(path, std::ios::trunc);
  if (!f) return false;
  for (const Row& r : rows)
    f << Clean(r.key) << '\t' << Clean(r.version) << '\t' << Clean(r.driver) << '\t' << Clean(r.hash) << '\t' << r.rung << '\t'
      << r.cls << '\t' << Clean(r.name) << '\t' << r.early << '\n';
  return bool(f);
}

inline std::string Hash(const std::string& text) {  // FNV-1a, 32 bits: only "did the settings change"
  unsigned h = 2166136261u;
  for (unsigned char c : text) h = (h ^ c) * 16777619u;
  char b[16];
  std::snprintf(b, sizeof(b), "%08x", h);
  return b;
}

// Written once per run (keyed on RunEnd::key): the launcher classifies at every start, and the same run must not count twice.
// Returns true when a row was added.
inline bool Record(const fs::path& history, const RunEnd& run, const std::string& version, const std::string& driver,
                   const std::string& hash, int rung) {
  if (run.kind != RunEnd::kGood && run.kind != RunEnd::kBad && run.kind != RunEnd::kMemory) return false;
  std::vector<Row> rows = ReadHistory(history);
  for (const Row& r : rows)
    if (r.key == run.key) return false;
  Row n;
  n.key = run.key, n.version = version, n.driver = driver, n.hash = hash, n.rung = rung;
  n.cls = run.kind == RunEnd::kGood ? 'G' : run.kind == RunEnd::kBad ? 'B' : 'M';
  n.name = run.name;
  if (run.kind == RunEnd::kGood && (run.hang_pending || (run.played && run.play_seconds >= 0 && run.play_seconds < kEarlyQuitSeconds)))
    n.early = 'E';
  // A memory kill counts only when the app was in front (a background kill is normal Android).
  if (run.kind == RunEnd::kMemory && !run.foreground) return false;
  rows.push_back(n);
  return WriteHistory(history, rows);
}

// ---------------------------------------------------------------------------------------------- what the rows say
struct Advice {
  int safe_next = -1;        // the rung to offer (SAFE MODE card), -1 = none
  int bad_in_row = 0;
  bool memory_card = false;  // two foreground low-memory kills in a row
  std::string early_key;     // the run whose "did the picture look right?" card is pending, empty = none
};

// Only rows of this app version, driver and settings count (an update, a driver change or a settings change starts clean).
inline Advice Decide(const std::vector<Row>& rows, const std::string& version, const std::string& driver,
                     const std::string& hash, int rung) {
  Advice a;
  std::vector<const Row*> mine;
  for (const Row& r : rows) mine.push_back(&r);
  // The tail that matches: stop at the first non-matching row going back.
  std::vector<const Row*> tail;
  for (size_t i = mine.size(); i-- > 0;) {
    const Row& r = *mine[i];
    if (r.version != version || r.driver != driver || r.hash != hash) break;
    tail.push_back(&r);  // newest first
  }
  for (const Row* r : tail) {  // safe mode: consecutive B at this rung (M rows are skipped, a G or another rung ends it)
    if (r->cls == 'M') continue;
    if (r->cls == 'B' && r->rung == rung) { ++a.bad_in_row; continue; }
    break;
  }
  if (a.bad_in_row >= 2 && rung < kMaxRung) a.safe_next = rung + 1;
  int mem = 0;
  for (const Row* r : tail) {
    if (r->cls == 'M') { ++mem; continue; }
    if (r->cls == 'G') break;
    // a B row neither counts nor ends the chain
  }
  a.memory_card = mem >= 2;
  if (!tail.empty() && tail.front()->early == 'E') a.early_key = tail.front()->key;  // (only the newest run: an older unanswered card is stale)
  return a;
}

// The player answered the early-quit card. looked_wrong turns that good row into a named bad end (REPORTED).
inline void AnswerEarly(const fs::path& history, const std::string& key, bool looked_wrong) {
  std::vector<Row> rows = ReadHistory(history);
  for (Row& r : rows)
    if (r.key == key && r.early == 'E') {
      r.early = 'A';
      if (looked_wrong) r.cls = 'B', r.name = "REPORTED";
    }
  WriteHistory(history, rows);
}

// ---------------------------------------------------------------------------------------------- the rung file
// logs are not the place for it: the launcher process dies at PLAY (LauncherActivity), so GameArgs reads <files>/rung.txt.
// Line 1 "rung N", then the app version and the driver it was chosen for (a new version or driver starts at rung 0), then
// "mediump_changed 0/1" (rung 3 turns mediump off as a saved setting; turning SAFE MODE off turns it back on).
struct RungState {
  int rung = 0;
  std::string version, driver;
  bool mediump_changed = false;
  bool stale_mediump = false;  // the file was for another version or driver but had changed mediump: the launcher gives it back
};

inline RungState ReadRung(const fs::path& path, const std::string& version, const std::string& driver) {
  RungState s;
  std::ifstream f(path);
  std::string a, b, c, d;
  std::getline(f, a), std::getline(f, b), std::getline(f, c), std::getline(f, d);
  if (a.rfind("rung ", 0) != 0) return s;
  if (Trim(b) != version || Trim(c) != driver) {
    s.stale_mediump = d.find("mediump_changed 1") != std::string::npos;
    return s;
  }  // an update or another driver: back to nothing
  s.rung = std::clamp(std::atoi(a.c_str() + 5), 0, kMaxRung);
  s.version = version, s.driver = driver;
  s.mediump_changed = d.find("mediump_changed 1") != std::string::npos;
  return s;
}

inline bool WriteRung(const fs::path& path, const RungState& s, const std::string& version, const std::string& driver) {
  std::ofstream f(path, std::ios::trunc);
  if (!f) return false;
  f << "rung " << std::clamp(s.rung, 0, kMaxRung) << "\n" << version << "\n" << driver << "\n"
    << "mediump_changed " << (s.mediump_changed ? 1 : 0) << "\n";
  return bool(f);
}

inline const char* RungWords(int rung) {
  switch (rung) {
    case 1: return "plain graphics mode, one shader worker";
    case 2: return "plain graphics mode, shadows off, crowd 30%, 30 FPS";
    case 3: return "everything in step 2, and the lower-precision colour maths off (the shaders are rebuilt: up to 10 minutes)";
    case 4: return "no further step is built in: share the report so the cause can be found";
    default: return "normal";
  }
}

// ---------------------------------------------------------------------------------------------- advice from frames_NNN.csv
// "Ran at 46 fps": the mean of the frames at normal speed (10 ms or more; a fast-forwarded stretch is shorter) after the first
// 10 s, for runs of at least 3000 such frames. Returns 0 when there is not enough to say.
inline double MeanFps(const fs::path& csv) {
  std::ifstream f(csv);
  std::string line;
  if (!std::getline(f, line)) return 0;
  double sum = 0;
  long n = 0;
  while (std::getline(f, line)) {
    std::stringstream ss(line);
    std::string col;
    double t = 0, ms = 0;
    for (int i = 0; i < 3 && std::getline(ss, col, ','); ++i) {
      if (i == 1) t = std::atof(col.c_str());
      if (i == 2) ms = std::atof(col.c_str());
    }
    if (t > 10 && ms >= 10) sum += ms, ++n;
  }
  return n >= 3000 ? 1000.0 / (sum / double(n)) : 0;
}

}  // namespace mlauncher::ladder
