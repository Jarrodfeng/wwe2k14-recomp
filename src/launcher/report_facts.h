// S108 lane RPT (docs\systems\problem_report.md): the problem report's IN PLAY / PROBLEMS / SHORTFALLS / ERRORS / REPRODUCE blocks, printed at the
// top of each log's section of findings.txt (both launchers). The C++ twin of tools\report\report_facts.py: both read the ONE table
// report_facts.inc (compiled in here; read as text there) and print the same text; tools\report\findings_compare.py fails on any difference.
// report_findings.h ReadLog feeds every line to Facts::Feed in its one pass; Render prints. Every list is capped (bounded memory, a log can be
// hundreds of MB); every printed value goes through Scrub (a drive, UNC or absolute POSIX path keeps only its last name), because the phone's
// zip has no Redact pass. Only ASCII decisions: a non-ASCII byte is never a separator, digit or letter. Std only (both launchers + the test).
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace report_findings::facts {

inline const char* TableText() {
  static const char* const t =
#include "report_facts.inc"
      ;
  return t;
}

struct Row {
  std::string kind, arg, text, writer;
};

inline std::string Trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (unsigned char)s[a] <= ' ') ++a;
  while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
  return s.substr(a, b - a);
}

// The table's rows (the raw string's text between its wrapper lines). Throws on a malformed row (a build-time file: the selftest reads it too).
inline const std::vector<Row>& Table() {
  static const std::vector<Row> rows = [] {
    std::vector<Row> out;
    const std::string all = TableText();
    size_t pos = 0;
    while (pos <= all.size()) {
      size_t nl = all.find('\n', pos);
      if (nl == std::string::npos) nl = all.size();
      const std::string s = Trim(all.substr(pos, nl - pos));
      pos = nl + 1;
      if (s.empty() || s[0] == '#') continue;
      std::vector<std::string> f;
      size_t a = 0;
      for (int k = 0; k < 3; ++k) {
        const size_t b = s.find(" | ", a);
        if (b == std::string::npos) break;
        f.push_back(Trim(s.substr(a, b - a)));
        a = b + 3;
      }
      f.push_back(Trim(s.substr(a)));
      if (f.size() != 4) throw std::runtime_error("report_facts.inc: a row needs KIND | ARG | TEXT | WRITER: " + s);
      out.push_back({f[0], f[1], f[2], f[3]});
    }
    return out;
  }();
  return rows;
}

constexpr size_t kTextMax = 4000;
constexpr size_t kCapFlags = 300, kCapRoute = 12, kCapChars = 200, kCapArenas = 50, kCapProps = 200, kCapMade = 200, kCapDlc = 50, kCapProblems = 100,
                 kCapErrors = 100, kCapShortfalls = 300, kCapCast = 200, kCapSeats = 64;

inline bool Letter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
inline bool Digit(char c) { return c >= '0' && c <= '9'; }
inline bool Alnum(char c) { return Letter(c) || Digit(c); }
inline bool WordCh(char c) { return Alnum(c) || c == '_' || c == '-'; }
inline bool In(char c, const char* set) {
  for (; *set; ++set)
    if (*set == c) return true;
  return false;
}
inline bool StartsAt(const std::string& s, size_t i, const char* p) {
  for (size_t k = 0; p[k]; ++k)
    if (i + k >= s.size() || s[i + k] != p[k]) return false;
  return true;
}
// the first n code points (Python's s[:n])
inline std::string CutCp(const std::string& s, size_t n) {
  size_t i = 0, count = 0;
  while (i < s.size() && count < n) {
    ++i;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    ++count;
  }
  return s.substr(0, i);
}
inline std::string Fmt(const char* f, ...) {
  char buf[8192];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}
inline long long ToLL(const std::string& s) {
  try {
    return std::stoll(s);
  } catch (...) {
    return 0;
  }
}
inline std::string I(long long v) { return std::to_string(v); }

inline size_t PathEnd(const std::string& s, size_t j, const char* stop, bool posix) {
  while (j < s.size()) {
    if (In(s[j], stop)) break;
    if (!posix && (StartsAt(s, j, " (") || StartsAt(s, j, " [") || StartsAt(s, j, " -> "))) break;
    ++j;
  }
  return j;
}

// S108 RPT review: a path that IS a profile folder (...\Users\NAME, /home/NAME): its last name is the person's name. k = seg's last separator.
inline bool ProfileTail(const std::string& seg, size_t k) {
  if (k == 0) return false;
  const size_t j = seg.find_last_of("\\/", k - 1);
  const size_t a = j == std::string::npos ? 0 : j + 1;
  std::string parent = seg.substr(a, k - a);
  for (char& c : parent)
    if (c >= 'A' && c <= 'Z') c = char(c + 32);
  return parent == "users" || parent == "home";
}

// Every drive (X:\ or X:/), UNC (\\host) or absolute POSIX (/a/b, at a token start) path -> "<path>" + its last separator and name (USER for a
// profile folder's own name).
inline std::string Scrub(const std::string& s) {
  static const char* kWinStop = "'\"\t|<>,;)";
  static const char* kPosixStop = " \t'\",;)]";
  static const char* kPosixStartPrev = " \t'\"(=[:,";
  std::string out;
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    const char c = s[i];
    size_t j = std::string::npos;
    if (Letter(c) && i + 2 < n && s[i + 1] == ':' && (s[i + 2] == '\\' || s[i + 2] == '/') && (i == 0 || (!Alnum(s[i - 1]) && s[i - 1] != '_'))) {
      j = PathEnd(s, i + 3, kWinStop, false);
      while (j > i + 3 && s[j - 1] == ' ') --j;
    } else if (c == '\\' && i + 2 < n && s[i + 1] == '\\' && Alnum(s[i + 2]) && (i == 0 || s[i - 1] != '\\')) {
      j = PathEnd(s, i + 2, kWinStop, false);
      while (j > i + 2 && s[j - 1] == ' ') --j;
    } else if (c == '/' && i + 1 < n && Letter(s[i + 1]) && (i == 0 || In(s[i - 1], kPosixStartPrev))) {
      j = PathEnd(s, i + 1, kPosixStop, true);
      size_t slashes = 0;
      for (size_t k = i; k < j; ++k) slashes += s[k] == '/';
      if (slashes < 2) j = std::string::npos;
    }
    if (j != std::string::npos && j > i) {
      const std::string seg = s.substr(i, j - i);
      const size_t k = seg.find_last_of("\\/");
      out += "<path>" + (ProfileTail(seg, k) ? seg.substr(k, 1) + "USER" : seg.substr(k));
      i = j;
    } else {
      out += c;
      ++i;
    }
  }
  return out;
}
inline std::string Clean(const std::string& s, size_t n = kTextMax) { return CutCp(Scrub(CutCp(s, kTextMax)), n); }

inline std::string FoldDigits(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size();) {
    if (Digit(s[i])) {
      o += '#';
      while (i < s.size() && Digit(s[i])) ++i;
    } else {
      o += s[i++];
    }
  }
  return o;
}

// The SDK pattern "[stamp] [level] [logger] [tN] message": the level of a stamped line, and its message.
inline std::string LevelOf(const std::string& line, std::string* msg) {
  if (msg) *msg = line;
  if (line.size() < 27 || line[25] != ' ' || line[26] != '[') return {};
  const size_t j = line.find(']', 27);
  if (j == std::string::npos) return {};
  const std::string level = line.substr(27, j - 27);
  std::string rest = (j + 1 < line.size() && line[j + 1] == ' ') ? line.substr(j + 2) : line.substr(j + 1);
  if (!rest.empty() && rest[0] == '[') {
    const size_t k = rest.find("] ");
    if (k != std::string::npos && k > 0 && StartsAt(rest, k + 2, "[t")) {
      const size_t k2 = rest.find("] ", k + 2);
      if (k2 != std::string::npos && k2 > k + 4) {
        bool dg = true;
        for (size_t q = k + 4; q < k2; ++q) dg = dg && Digit(rest[q]);
        if (dg) rest = rest.substr(k2 + 2);
      }
    }
  }
  if (msg) *msg = rest;
  return level;
}

inline std::optional<std::string> FrameOf(const std::string& line) {
  for (size_t i = line.find("frame"); i != std::string::npos; i = line.find("frame", i + 1)) {
    if ((i == 0 || (!Alnum(line[i - 1]) && line[i - 1] != '_')) && i + 6 < line.size() && (line[i + 5] == ' ' || line[i + 5] == '=') && Digit(line[i + 6])) {
      size_t j = i + 6;
      while (j < line.size() && Digit(line[j])) ++j;
      return line.substr(i + 6, j - i - 6);
    }
  }
  return std::nullopt;
}

inline std::string ErrKey(const std::string& msg) {
  std::string out;
  for (size_t i = 0; i < msg.size();) {
    if (msg[i] == '0' && i + 1 < msg.size() && msg[i + 1] == 'x') {
      i += 2;
      while (i < msg.size() && (Digit(msg[i]) || In(msg[i], "abcdefABCDEF"))) ++i;
      out += ' ';
      continue;
    }
    out += Letter(msg[i]) ? msg[i] : ' ';
    ++i;
  }
  std::string col;
  for (size_t i = 0; i < out.size();) {  // " ".join(s.split())
    while (i < out.size() && out[i] == ' ') ++i;
    size_t j = i;
    while (j < out.size() && out[j] != ' ') ++j;
    if (j > i) col += (col.empty() ? "" : " ") + out.substr(i, j - i);
    i = j;
  }
  return col.substr(0, 60);
}

inline size_t WordBr(const std::string& s, size_t i) {  // "[word]" at i -> its end, else npos
  if (i >= s.size() || s[i] != '[') return std::string::npos;
  size_t j = i + 1;
  while (j < s.size() && (Alnum(s[j]) || s[j] == '_')) ++j;
  if (j == i + 1 || j >= s.size() || s[j] != ']') return std::string::npos;
  return j + 1;
}
inline size_t TagAt(const std::string& s, size_t i) {
  for (const char* h : {"scene", "rdr", "host", "native", "wwe2k14"}) {
    const std::string head = std::string("[") + h + "]";
    if (StartsAt(s, i, head.c_str())) {
      const size_t e = WordBr(s, i + head.size());
      if (e == std::string::npos) return std::string::npos;
      const size_t e2 = WordBr(s, e);
      return e2 != std::string::npos ? e2 : e;
    }
  }
  return std::string::npos;
}
struct Tag {
  size_t start, end;
};
inline std::vector<Tag> TagsOf(const std::string& line) {
  std::vector<Tag> out;
  for (size_t i = 0; i < line.size();) {
    if (line[i] == '[') {
      const size_t e = TagAt(line, i);
      if (e != std::string::npos) {
        out.push_back({i, e});
        i = e;
        continue;
      }
    }
    ++i;
  }
  return out;
}

inline std::string NameBefore(const std::string& b, size_t end) {
  std::vector<std::string> words;
  size_t j = end;
  while (true) {
    size_t k = j;
    while (k > 0 && WordCh(b[k - 1])) --k;
    if (k == j || (!Letter(b[k]) && b[k] != '_')) break;
    words.insert(words.begin(), b.substr(k, j - k));
    if (k >= 2 && b[k - 1] == ' ' && WordCh(b[k - 2])) {
      j = k - 1;
      continue;
    }
    break;
  }
  std::string o;
  for (const auto& w : words) o += (o.empty() ? "" : " ") + w;
  return o;
}

// (name, number text) of "WORDS N" / "WORDS=N" / "WORDS: N" / "WORDS:N" in one census block, in order.
inline std::vector<std::pair<std::string, std::string>> CountersIn(const std::string& b) {
  std::vector<std::pair<std::string, std::string>> out;
  size_t i = 0;
  while (i < b.size()) {
    size_t st;
    if (b[i] == '-' && i + 1 < b.size() && Digit(b[i + 1])) st = i;
    else if (Digit(b[i])) st = i;
    else {
      ++i;
      continue;
    }
    size_t e = st + (b[st] == '-' ? 1 : 0);
    while (e < b.size() && Digit(b[e])) ++e;
    if (e + 1 < b.size() && b[e] == '.' && Digit(b[e + 1])) {
      ++e;
      while (e < b.size() && Digit(b[e])) ++e;
    }
    i = e;
    if (e < b.size() && (Alnum(b[e]) || b[e] == '_' || b[e] == '.')) continue;
    size_t ne;
    const size_t p = st;
    if (p > 0 && b[p - 1] == '=') ne = p - 1;
    else if (p > 1 && b[p - 1] == ' ' && b[p - 2] == ':') ne = p - 2;
    else if (p > 0 && (b[p - 1] == ':' || b[p - 1] == ' ')) ne = p - 1;
    else continue;
    const std::string nm = NameBefore(b, ne);
    if (!nm.empty()) out.push_back({nm, b.substr(st, e - st)});
  }
  return out;
}

inline std::string Norm(const std::string& name) {  // lower, runs of space / dash -> '_'
  std::string o;
  bool run = false;
  for (char c : name) {
    if (c == ' ' || c == '-') {
      if (!run) o += '_';
      run = true;
      continue;
    }
    run = false;
    o += (c >= 'A' && c <= 'Z') ? char(c + 32) : c;
  }
  return o;
}
inline std::string AsciiUpper(std::string s) {
  for (char& c : s)
    if (c >= 'a' && c <= 'z') c = char(c - 32);
  return s;
}

// an insertion-ordered, capped counter of texts (a Python dict of counts)
struct Counted {
  std::vector<std::pair<std::string, long long>> items;
  std::unordered_map<std::string, size_t> at;
  long long more = 0;
  void Bump(const std::string& k, size_t cap) {
    auto it = at.find(k);
    if (it != at.end()) {
      ++items[it->second].second;
      return;
    }
    if (items.size() >= cap) {
      ++more;
      return;
    }
    at.emplace(k, items.size());
    items.push_back({k, 1});
  }
};

struct Seat {
  long long seat, id;
  std::optional<std::string> pad;  // nullopt = CPU (setup) / not known (card)
  std::string attire;              // digits or "?"
  std::optional<std::string> set;
};

class Facts {
 public:
  Facts() {
    for (const Row& r : Table()) {
      if (r.kind == "build") builds_.push_back({r.arg, r.text});
      else if (r.kind == "renderer") rends_.push_back({r.arg, r.text});
      else if (r.kind == "mod") mods_.push_back({r.arg, r.text});
      else if (r.kind == "modline") modlines_.push_back({r.arg, r.text});
      else if (r.kind == "problem" || r.kind == "problem#") probs_.push_back({r.arg, r.kind == "problem#"});
      else if (r.kind == "shortfall") shorts_.push_back({Norm(r.arg), r.text});
      else if (r.kind == "line") role_[r.text].push_back(r.arg);
      else if (r.kind == "switch") switch_[r.arg] = r.text;
    }
    build_.resize(builds_.size());
    rend_.resize(rends_.size());
    modn_.resize(modlines_.size());
  }

  void Feed(const std::string& line, bool stamped, const std::string& hm, bool last_part) {
    std::string level;
    if (stamped) {
      const std::string ts = line.substr(1, 19);
      if (!first_ts_) first_ts_ = ts;
      last_ts_ = ts;
      level = LevelOf(line, nullptr);
    }
    for (size_t i = 0; i < builds_.size(); ++i) {
      if (build_[i]) continue;
      const size_t p = line.find(builds_[i].first);
      if (p != std::string::npos) build_[i] = Clean(Trim(line.substr(p + builds_[i].first.size())), 300);
    }
    for (size_t i = 0; i < rends_.size(); ++i)
      if (line.find(rends_[i].first) != std::string::npos) rend_[i] = true;
    for (size_t i = 0; i < modlines_.size(); ++i)
      if (line.find(modlines_[i].first) != std::string::npos) ++modn_[i];
    if (last_part)
      for (const std::string& mk : role_["end"])
        if (line.find(mk) != std::string::npos) ended_ = true;
    size_t p = line.find(M("modified"));
    if (p != std::string::npos) Flag(line.substr(p + M("modified").size()));
    if (stamped && line.find("[wwe2k14][scene] sequence ") != std::string::npos) {
      static const std::regex rx(R"(\[wwe2k14\]\[scene\] sequence (\d+) -> (\d+) \((.*?)\) at frame (\d+))");
      std::smatch m;
      const std::string cut = CutCp(line, kTextMax);
      if (std::regex_search(cut, m, rx)) Seq(ToLL(m[2].str()), m[3].str());
    }
    p = line.find(M("scene") + " ");
    if (stamped && p != std::string::npos && line.find(" at frame ") != std::string::npos) {
      const std::string t = line.substr(p);
      const size_t a = t.find(" at frame ");
      const size_t c = a == std::string::npos ? std::string::npos : t.find(", ", a);
      scene_mark_ = Clean(c != std::string::npos ? t.substr(0, c) : t, 300);
      scene_hm_ = hm;
    }
    if ((p = line.find(M("card"))) != std::string::npos) Card(CutCp(line.substr(p), kTextMax), hm);
    if ((p = line.find(M("titles"))) != std::string::npos) titles_ = Clean(line.substr(p), 600);
    if ((p = line.find(M("setup"))) != std::string::npos) Setup(CutCp(line.substr(p), kTextMax), hm);
    if ((p = line.find(M("setup_more"))) != std::string::npos) SetupMore(CutCp(line.substr(p), kTextMax));
    if ((p = line.find(M("dlc"))) != std::string::npos) dlc_.Bump(Clean(line.substr(line.find("pack", p)), 300), kCapDlc);
    if (line.find(M("prop")) != std::string::npos) {
      static const std::regex rx(R"(event prop id (\d+) -> PACH (\S+) \(entry (\d+)\))");
      std::smatch m;
      const std::string cut = CutCp(line, kTextMax);
      if (std::regex_search(cut, m, rx)) props_.Bump("event prop id " + m[1].str() + " -> PACH " + Clean(m[2].str(), 40) + " (entry " + m[3].str() + ")", kCapProps);
    }
    if (line.find(M("prop_made")) != std::string::npos) {
      static const std::regex rx(R"(event prop made: id (\d+) k (\d+))");
      std::smatch m;
      const std::string cut = CutCp(line, kTextMax);
      if (std::regex_search(cut, m, rx)) made_.Bump("event prop made: id " + m[1].str() + " k " + m[2].str(), kCapMade);
    }
    {
      std::string path, status;
      if (ParseFileOpen(line, path, status) && status == "00000000") {
        const size_t s = path.find_last_of("\\/");
        const std::string base = AsciiUpper(s == std::string::npos ? path : path.substr(s + 1));
        if (base.size() >= 4 && base.compare(base.size() - 4, 4, ".PAC") == 0 && base.size() > 2 && Digit(base[2])) {
          if (base.compare(0, 2, "CH") == 0) chars_.Bump(Clean(base, 80), kCapChars);
          else if (base.compare(0, 2, "BG") == 0) arenas_.Bump(Clean(base, 80), kCapArenas);
        }
      }
    }
    bool hit = false;
    for (const auto& pr : probs_) {
      const size_t q = line.find(pr.first);
      if (q == std::string::npos) continue;
      hit = true;
      const std::string t = CutCp(line.substr(q), kTextMax);
      const std::string key = pr.second ? FoldDigits(t) : t;
      auto it = prob_at_.find(key);
      if (it == prob_at_.end()) {
        if (problems_.size() >= kCapProblems) {
          ++problems_more_;
          break;
        }
        it = prob_at_.emplace(key, problems_.size()).first;
        problems_.push_back({0, hm, FrameOf(line), mode_, Clean(t)});
      }
      ++problems_[it->second].n;
      break;
    }
    if (!hit && (level == "error" || level == "critical")) {
      std::string msg;
      LevelOf(line, &msg);
      const std::string key = ErrKey(CutCp(msg, kTextMax));
      auto it = err_at_.find(key);
      if (it == err_at_.end()) {
        if (errors_.size() >= kCapErrors) ++errors_more_;
        else it = err_at_.emplace(key, errors_.size()).first, errors_.push_back({0, hm, std::nullopt, "", Clean(msg, 300)});
      }
      if (it != err_at_.end()) ++errors_[it->second].n;
    }
    Census(line, stamped, hm);
  }

  // "[wwe2k14][file] NtCreateFile 'PATH'[ (relative to handle 0xH)] KIND access=X -> 0xSTATUS" (src\kernel_overrides.h:225): the path and status.
  static bool ParseFileOpen(const std::string& line, std::string& path, std::string& status) {
    if (line.find("NtCreateFile") == std::string::npos && line.find("NtOpenFile") == std::string::npos) return false;
    if (line.find("[file]") == std::string::npos) return false;
    const size_t a = line.find("[wwe2k14][file] Nt");
    if (a == std::string::npos) return false;
    size_t p = a + 18;
    const bool cr = line.compare(p, 10, "CreateFile") == 0, op = line.compare(p, 8, "OpenFile") == 0;
    if (!cr && !op) return false;
    p += cr ? 10 : 8;
    if (line.compare(p, 2, " '") != 0) return false;
    const size_t q = line.find('\'', p + 2);
    if (q == std::string::npos) return false;
    path = line.substr(p + 2, q - (p + 2));
    p = q + 1;
    if (line.compare(p, 21, " (relative to handle ") == 0 && line.compare(p + 21, 2, "0x") == 0) {
      const size_t h = p + 23;
      size_t e = h;
      while (e < line.size() && std::isxdigit((unsigned char)line[e])) ++e;
      if (e > h && e < line.size() && line[e] == ')') p = e + 1;
    }
    if (p >= line.size() || line[p] != ' ') return false;
    size_t e = p + 1;
    while (e < line.size() && !std::isspace((unsigned char)line[e])) ++e;
    if (e == p + 1 || line.compare(e, 8, " access=") != 0) return false;
    size_t e2 = e + 8;
    while (e2 < line.size() && !std::isspace((unsigned char)line[e2])) ++e2;
    if (line.compare(e2, 6, " -> 0x") != 0) return false;
    const size_t h = e2 + 6;
    size_t e3 = h;
    while (e3 < line.size() && std::isxdigit((unsigned char)line[e3])) ++e3;
    if (e3 == h) return false;
    status = line.substr(h, e3 - h);
    return true;
  }

  std::vector<std::string> Render(long long n_lines) const {
    std::vector<std::string> o;
    auto P = [&](const std::string& s) { o.push_back(s); };
    auto more = [](long long k) { return k ? " (+" + I(k) + " more line(s), not listed)" : std::string(); };
    auto join = [](const std::vector<std::string>& v, const char* sep) {
      std::string s;
      for (size_t i = 0; i < v.size(); ++i) s += (i ? sep : "") + v[i];
      return s;
    };
    P("  IN PLAY (what this log says was in play; ids, names and option values only)");
    std::vector<std::string> bits;
    for (size_t i = 0; i < builds_.size(); ++i)
      if (build_[i]) bits.push_back(builds_[i].second + " " + *build_[i]);
    P("    build: " + (bits.empty() ? std::string("no version line in this log") : join(bits, "; ")));
    std::vector<std::string> rb;
    for (size_t i = 0; i < rends_.size(); ++i)
      if (rend_[i]) rb.push_back(rends_[i].second);
    P("    renderer: " + (rb.empty() ? std::string("no stream-free marker: a stream build (an older release or a dev build)") : join(rb, "; ")));
    P("    run: " + I(n_lines) + " lines, first " + first_ts_.value_or("none") + ", last " + last_ts_.value_or("none") + "; ended: " +
      (ended_ ? "yes (an end line in the newest part)" : "no (no Execution complete / Title terminated in the newest part: cut, killed, crashed or still running)"));
    P("    settings that differ from default: " + (flags_.empty() ? std::string("none logged") : I((long long)flags_.size()) + more(flags_more_)));
    for (const auto& f : flags_) P("      " + f.first + " = " + f.second);
    std::vector<std::string> on;
    for (const auto& md : mods_) {
      const std::string* v = FlagVal(md.first);
      if (v && *v != "" && *v != "false" && *v != "0") on.push_back(md.second + " (" + md.first + ")");
    }
    for (size_t i = 0; i < modlines_.size(); ++i)
      if (modn_[i]) on.push_back(modlines_[i].second + ": " + I(modn_[i]) + " line(s)");
    P("    mods on: " + (on.empty() ? std::string("none") : join(on, ", ")));
    P("    DLC packs: " + (dlc_.items.empty() ? std::string("none") : I((long long)dlc_.items.size())) + more(dlc_.more));
    for (const auto& d : dlc_.items) P("      " + d.first + (d.second > 1 ? " (x" + I(d.second) + ")" : ""));
    if (route_n_) {
      std::vector<std::string> r(route_head_.begin(), route_head_.end());
      const long long mid = route_n_ - (long long)route_head_.size() - (long long)route_tail_.size();
      if (mid > 0) r.push_back("(" + I(mid) + " more)");
      r.insert(r.end(), route_tail_.begin(), route_tail_.end());
      P("    route (" + I(seq_lines_) + " sequence line(s)): " + join(r, " -> "));
    } else {
      P("    route: none (no sequence line)");
    }
    P("    last scene mark: " + (scene_mark_ ? *scene_mark_ + " (at " + (scene_hm_.empty() ? std::string("None") : scene_hm_) + ")" : std::string("none")));
    if (card_) {
      P("    matches: " + I(cards_) + " card line(s); last (" + card_->when + ", at " + (card_->hm.empty() ? std::string("?") : card_->hm) + "): mode " + I(card_->mode) + " type " + I(card_->type) + " arena " +
        I(card_->arena));
    } else {
      P("    matches: none (no card line)");
    }
    std::vector<std::string> cast;
    for (const auto& c : cast_) cast.push_back(I(c.first) + " attire " + c.second + (c.first < 100 ? " (a created superstar)" : ""));
    P("    characters in the cards: " + (cast.empty() ? std::string("none") : join(cast, ", ")) + more(cast_more_));
    std::vector<std::string> names;
    for (const auto& c : chars_.items) names.push_back(c.first);
    P("    characters / attires opened: " + (names.empty() ? std::string("none") : join(names, ", ")) + more(chars_.more));
    names.clear();
    for (const auto& c : arenas_.items) names.push_back(c.first);
    P("    arena opened: " + (names.empty() ? std::string("none") : join(names, ", ")) + more(arenas_.more));
    for (const auto* d : {&props_, &made_}) {
      P(std::string("    ") + (d == &props_ ? "event props" : "event props made") + ": " + (d->items.empty() ? std::string("none") : I((long long)d->items.size()) + " distinct") + more(d->more));
      for (const auto& x : d->items) P("      " + x.first + (x.second > 1 ? " (x" + I(x.second) + ")" : ""));
    }
    P("  PROBLEMS (whole lines from their marker on, deduplicated: count, first time / frame / mode)");
    for (const auto& e : problems_)
      P("    x" + I(e.n) + " first " + (e.hm.empty() ? std::string("?") : e.hm) + (e.frame ? " frame " + *e.frame : std::string()) + " in " + e.mode + ": " + e.text);
    if (problems_.empty()) P("    none");
    if (problems_more_) P("    (+" + I(problems_more_) + " more line(s) of other problems, not listed)");
    P("  SHORTFALLS (census counters whose last value is not 0; not all are faults)");
    bool any = false;
    for (const auto& c : counters_) {
      if (std::strtod(c.second.last.c_str(), nullptr) == 0.0) continue;
      any = true;
      P("    " + Clean(c.first.first, 120) + " " + c.first.second + ": first non-zero " + c.second.nz.value_or("None") + " at " +
        (c.second.nz_hm.empty() ? std::string("?") : c.second.nz_hm) + ", last " + c.second.last + "  (" + c.second.why + ")");
    }
    if (!any) P("    none");
    if (counters_more_) P("    (+" + I(counters_more_) + " more counter reading(s), not read)");
    P("  ERRORS (error / critical lines, grouped by their words)");
    for (const auto& e : errors_) P("    x" + I(e.n) + " first " + (e.hm.empty() ? std::string("?") : e.hm) + ": " + e.text);
    if (errors_.empty()) P("    none");
    if (errors_more_) P("    (+" + I(errors_more_) + " more line(s) of other groups, not listed)");
    Reproduce(P);
    return o;
  }

 private:
  struct Prob {
    long long n;
    std::string hm;
    std::optional<std::string> frame;
    std::string mode, text;
  };
  struct Ctr {
    std::optional<std::string> nz;
    std::string nz_hm, last, why;
  };
  struct CardT {
    std::string when;
    long long mode, type, arena;
    std::vector<Seat> parts;
    std::string hm;
  };
  struct SetupT {
    long long mode, type, pace, arena;
    std::optional<std::vector<int>> rules;
    std::vector<Seat> seats;
    std::string hm, frame;
  };

  const std::string& M(const std::string& role) {
    static const std::string none = "\x01no such marker\x01";
    auto it = role_.find(role);
    return it == role_.end() || it->second.empty() ? none : it->second[0];
  }
  const std::string* FlagVal(const std::string& name) const {
    for (const auto& f : flags_)
      if (f.first == name) return &f.second;
    return nullptr;
  }

  void Flag(std::string rest) {
    if (!rest.empty() && rest[0] == ' ') rest = rest.substr(1);
    const size_t a = rest.find(" = '");
    const size_t b = rest.rfind("' (source: ");
    if (a == std::string::npos || a == 0 || b == std::string::npos || b < a + 4) return;
    const std::string name = rest.substr(0, a), val = rest.substr(a + 4, b - (a + 4));
    if (flag_names_.count(name)) return;
    if (flags_.size() >= kCapFlags) {
      ++flags_more_;
      return;
    }
    flag_names_.insert(name);
    flags_.push_back({name, Clean(val, 300)});
  }

  void Seq(long long b, const std::string& name) {
    ++seq_lines_;
    mode_ = I(b) + " " + Clean(name, 120);
    const size_t c = name.find(": ");
    const std::string entry = I(b) + " " + Clean(c == std::string::npos ? name : name.substr(0, c), 80);
    if (route_last_ && *route_last_ == entry) return;
    route_last_ = entry;
    ++route_n_;
    if (route_head_.size() < kCapRoute) {
      route_head_.push_back(entry);
    } else {
      route_tail_.push_back(entry);
      if (route_tail_.size() > kCapRoute) route_tail_.pop_front();
    }
  }

  void Card(const std::string& t, const std::string& hm) {
    static const std::regex rx(R"(card \(([^)]*)\): mode (-?\d+) type (-?\d+) arena (-?\d+))"), rxp(R"((\d+):(\d+) \(slot (\d+)\))");
    std::smatch m;
    if (!std::regex_search(t, m, rx)) return;
    ++cards_;
    const size_t pi = t.find("participants ");
    const std::string plist = pi == std::string::npos ? std::string() : t.substr(pi + 13);
    CardT c{Clean(m[1].str(), 80), ToLL(m[2].str()), ToLL(m[3].str()), ToLL(m[4].str()), {}, hm};
    for (auto it = std::sregex_iterator(plist.begin(), plist.end(), rxp); it != std::sregex_iterator(); ++it) {
      const long long seat = ToLL((*it)[1].str()), id = ToLL((*it)[2].str()), key = ToLL((*it)[3].str());
      if (seat >= 40) continue;
      const std::string att = key / 100 == id ? I((key / 10) % 10) : std::string("?");
      c.parts.push_back({seat, id, std::nullopt, att, std::nullopt});
      const std::pair<long long, std::string> ck{id, att};
      if (!cast_seen_.count(ck)) {
        if (cast_.size() >= kCapCast) ++cast_more_;
        else cast_seen_.insert(ck), cast_.push_back(ck);
      }
    }
    card_ = std::move(c);
  }

  void Setup(const std::string& t, const std::string& hm) {
    static const std::regex rx(R"(setup: mode (-?\d+) type (-?\d+) pace (-?\d+) arena (-?\d+); rules ([0-9A-Fa-f]*); seats (.*?) frame=(\d+))");
    std::smatch m;
    if (!std::regex_search(t, m, rx)) return;
    ++setups_;
    SetupT s{ToLL(m[1].str()), ToLL(m[2].str()), ToLL(m[3].str()), ToLL(m[4].str()), std::nullopt, {}, hm, m[7].str()};
    const std::string rules = m[5].str();
    if (rules.size() == 128) {
      std::vector<int> r;
      for (size_t i = 0; i < 128; i += 2) r.push_back((int)std::strtol(rules.substr(i, 2).c_str(), nullptr, 16));
      s.rules = r;
    }
    setup_ = std::move(s);
    Seats(m[6].str());
  }
  void SetupMore(const std::string& t) {
    static const std::regex rx(R"(setup\+: seats (.*?) frame=(\d+))");
    std::smatch m;
    if (setup_ && std::regex_search(t, m, rx)) Seats(m[1].str());
  }
  void Seats(const std::string& text) {
    static const std::regex rx(R"((\d+):(\d+)/(cpu|p\d+)/a(\d+|\?)(?:/s(-?\d+))?)");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), rx); it != std::sregex_iterator(); ++it) {
      if (setup_->seats.size() >= kCapSeats) break;
      const std::string pad = (*it)[3].str();
      setup_->seats.push_back({ToLL((*it)[1].str()), ToLL((*it)[2].str()), pad == "cpu" ? std::nullopt : std::optional<std::string>(pad.substr(1)), (*it)[4].str(),
                               (*it)[5].matched ? std::optional<std::string>((*it)[5].str()) : std::nullopt});
    }
  }

  void Census(const std::string& line, bool stamped, const std::string& hm) {
    const std::vector<Tag> marks = TagsOf(line);
    std::vector<std::pair<std::string, std::string>> segs;
    if (!marks.empty()) {
      if (marks[0].start > 0 && last_tag_ && !stamped) segs.push_back({*last_tag_, line.substr(0, marks[0].start)});
      for (size_t i = 0; i < marks.size(); ++i) {
        const size_t end = i + 1 < marks.size() ? marks[i + 1].start : line.size();
        segs.push_back({line.substr(marks[i].start, marks[i].end - marks[i].start), line.substr(marks[i].end, end - marks[i].end)});
      }
      last_tag_ = line.substr(marks.back().start, marks.back().end - marks.back().start);
    } else if (last_tag_ && !stamped) {
      segs.push_back({*last_tag_, line});
    }
    if (stamped && marks.empty()) last_tag_.reset();
    for (const auto& sg : segs) {
      std::map<std::string, int> seen;
      for (const auto& cn : CountersIn(sg.second)) {
        const std::string n = Norm(cn.first);
        const std::string* why = nullptr;
        for (const auto& ph : shorts_)
          if (("_" + n + "_").find("_" + ph.first + "_") != std::string::npos) {
            why = &ph.second;
            break;
          }
        if (!why) continue;
        const int k = ++seen[n];
        const std::pair<std::string, std::string> key{sg.first, k == 1 ? n : n + "#" + std::to_string(k)};
        auto it = counters_.find(key);
        if (it == counters_.end()) {
          if (counters_.size() >= kCapShortfalls) {
            ++counters_more_;
            continue;
          }
          it = counters_.emplace(key, Ctr{std::nullopt, "", cn.second, *why}).first;
        }
        it->second.last = cn.second;
        if (!it->second.nz && std::strtod(cn.second.c_str(), nullptr) != 0.0) it->second.nz = cn.second, it->second.nz_hm = hm;
      }
    }
  }

  template <class Out>
  void Reproduce(Out& P) const {
    P("  REPRODUCE (the last match set up in this log; ids and option values only)");
    if (!setup_ && !card_) {
      const std::string last = !route_tail_.empty() ? route_tail_.back() : (!route_head_.empty() ? route_head_.back() : std::string("this run"));
      P("    no match was set up in this log: the route above is the menu path; our launcher has no direct launch for " + last);
      return;
    }
    long long mode, type, arena;
    std::optional<long long> pace;
    std::optional<std::vector<int>> rules;
    std::vector<Seat> seats;
    if (setup_) {
      P("    from: the setup line at " + (setup_->hm.empty() ? std::string("?") : setup_->hm) + ", frame " + setup_->frame + " (" + I(setups_) + " setup line(s) in this log)");
      mode = setup_->mode, type = setup_->type, arena = setup_->arena, pace = setup_->pace, rules = setup_->rules, seats = setup_->seats;
    } else {
      P("    from: the card line at " + (card_->hm.empty() ? std::string("?") : card_->hm) +
        " (no setup line in this log: a build before S108; pads, pace and rules are not known)");
      mode = card_->mode, type = card_->type, arena = card_->arena, seats = card_->parts;
    }
    P("    mode " + I(mode) + " (" +
      (mode == 0 ? std::string("exhibition")
                 : std::string("not an exhibition: the launch line rebuilds the same match as an exhibition; the mode's own story / objectives are not rebuilt")) +
      ")");
    P("    type " + I(type) + ", pace " + (pace ? I(*pace) : std::string("?")) + ", arena " + I(arena) + (arena == 300 ? " (the type's own)" : ""));
    for (const Seat& s : seats) {
      const std::string who = s.pad ? "pad " + *s.pad : (setup_ ? std::string("CPU") : std::string("pad ?"));
      P("    seat " + I(s.seat) + ": " + I(s.id) + " " + who + ", attire " + s.attire + (s.set ? ", colour set " + *s.set : std::string()) +
        (s.id < 100 ? ": a created superstar: the player's own; id " + I(s.id) + " on our PC is our own save's slot" : std::string()));
    }
    if (titles_) P("    titles: " + *titles_);
    bool contiguous = true;
    for (size_t i = 0; i < seats.size(); ++i) contiguous = contiguous && seats[i].seat == (long long)i;
    if (!contiguous) P("    note: the seats are not 0..n-1: the launch line fills slots 0..n-1 in seat order");
    if (type == 23) {
      P("    launch line: none: a built Royal Rumble (type 23) does not load (docs\\systems\\match_setup.md:270): it needs a saved setup");
    } else {
      auto sw = [&](const char* role) {
        auto it = switch_.find(role);
        return it == switch_.end() ? std::string("?") : it->second;
      };
      std::string toks;
      for (size_t i = 0; i < seats.size() && i < 40; ++i) {
        const Seat& s = seats[i];
        // ID[:PAD[:ATTIRE]] (src\launch_participant.h): with no attire part, `ID:` alone is pad 0, so a CPU seat is the bare id (S108 RPT review)
        std::string t = I(s.id);
        if (s.attire != "?") t += ":" + (s.pad ? *s.pad : std::string()) + ":" + s.attire + (s.set ? "/" + *s.set : std::string());
        else if (s.pad) t += ":" + *s.pad;
        toks += (toks.empty() ? "" : ",") + t;
      }
      std::string ln = "--" + sw("launch") + "=@quick --" + sw("quick_type") + "=" + I(type);
      if (pace) ln += " --" + sw("quick_pace") + "=" + I(*pace);
      if (arena >= 0 && arena < 300) ln += " --" + sw("launch_arena") + "=" + I(arena);
      if (!toks.empty()) ln += " --" + sw("launch_participants") + "=" + toks;
      if (rules) {
        std::string r;
        for (size_t i = 0; i < rules->size(); ++i) r += (i ? "," : "") + I(12 + (long long)i) + "=" + I((*rules)[i]);
        ln += " --" + sw("quick_rules") + "=" + r;
      }
      P("    launch line: " + ln);
    }
    P(std::string("    not carried: titles held, alternate looks, a created superstar's or arena's own data, the random draws") +
      (rules ? "" : ", the rule bytes (the type's defaults are used)") + "; the run is not frame-exact");
  }

  std::vector<std::pair<std::string, std::string>> builds_, rends_, mods_, modlines_, shorts_;
  std::vector<std::pair<std::string, bool>> probs_;
  std::map<std::string, std::vector<std::string>> role_;
  std::map<std::string, std::string> switch_;
  std::vector<std::optional<std::string>> build_;
  std::vector<bool> rend_;
  std::vector<long long> modn_;
  std::optional<std::string> first_ts_, last_ts_;
  bool ended_ = false;
  std::vector<std::pair<std::string, std::string>> flags_;
  std::set<std::string> flag_names_;
  long long flags_more_ = 0;
  Counted dlc_, chars_, arenas_, props_, made_;
  std::vector<std::string> route_head_;
  std::deque<std::string> route_tail_;
  std::optional<std::string> route_last_;
  long long route_n_ = 0, seq_lines_ = 0;
  std::string mode_ = "(before the first sequence line)";
  std::optional<std::string> scene_mark_;
  std::string scene_hm_;
  long long cards_ = 0;
  std::optional<CardT> card_;
  std::vector<std::pair<long long, std::string>> cast_;
  std::set<std::pair<long long, std::string>> cast_seen_;
  long long cast_more_ = 0;
  std::optional<std::string> titles_;
  long long setups_ = 0;
  std::optional<SetupT> setup_;
  std::vector<Prob> problems_;
  std::unordered_map<std::string, size_t> prob_at_;
  long long problems_more_ = 0;
  std::vector<Prob> errors_;
  std::unordered_map<std::string, size_t> err_at_;
  long long errors_more_ = 0;
  std::map<std::pair<std::string, std::string>, Ctr> counters_;
  long long counters_more_ = 0;
  std::optional<std::string> last_tag_;
};

}  // namespace report_findings::facts
