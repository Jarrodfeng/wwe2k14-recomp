// S105 C0b: findings.txt for every report zip - the C++ port of tools\report\triage.py (the report reader; its CLASSES table is the spec).
// Players have no Python, so the launchers (PC: launcher_main.cpp MakeReport; phone: launcher_mobile\mobile_main.cpp ShareZip) compute the same
// per-mode "not drawn" summary at pack time and put it in the zip as findings.txt. Header only, std only (no SDK includes): used by both launchers
// and by tools\report\findings_test.cpp (compared against triage.py's text on the same logs by tools\report\findings_compare.py).
//
// It reads the RAW game log (before the report's CondenseLog), parts oldest first, streaming line by line. It never throws: any failure becomes
// the text "could not read the log: <why>" (Build). Keep it in step with triage.py: a class is added in BOTH tables (docs\agents\report_triage.md).
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "report_facts.h"  // S108 RPT: the IN PLAY / PROBLEMS / SHORTFALLS / ERRORS / REPRODUCE blocks (table: report_facts.inc)

namespace report_findings {

// ---------------------------------------------------------------------------------------------------------------------------------
// THE PATTERN TABLE (a port of triage.CLASSES; group names are listed in order: group i+1 of the regex).
// how: one | sum | max | count_yes | delta | set (read), first / first_hole / first_nz / first_if_yes / pairs (reason text only, not aggregated).
struct Field {
  const char* group;
  const char* how;
};
struct Cls {
  std::string name;
  int prio;
  std::vector<std::string> markers;
  std::string pattern;
  std::vector<std::string> groups;
  std::vector<Field> fields;
  std::string finding;  // empty = INFO (never a finding)
  std::string text;
  bool periodic = false, cut_totals = false, scene_attr = false;
  std::string why_const;
  std::regex rx;
  std::string detail;  // S105 H: continuation lines of a multi-line class: counted as "det", never a finding, never UNPARSED (empty = none)
  std::regex detail_rx;
};

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
inline std::vector<Cls>& Classes() {
  static std::vector<Cls> table = [] {
    std::vector<Cls> t;
    auto add = [&](Cls c) {
      c.rx = std::regex(c.pattern);
      if (!c.detail.empty()) c.detail_rx = std::regex(c.detail);
      t.push_back(std::move(c));
    };
    add({"problem", 0, {"[rdr][PROBLEM]"}, R"(\[rdr\]\[PROBLEM\] (\w+): (.*)$)", {"name", "why"}, {{"n", "one"}, {"why", "first"}}, "n", "{n} [rdr][PROBLEM] line(s)"});
    add({"load_stall", 0, {"[load] STALL"}, R"(\[wwe2k14\]\[load\] STALL: (.*)$)", {"why"}, {{"n", "one"}, {"why", "first"}}, "n", "{n} loader STALL line(s)"});
    {
      // S105 H: the WATCH dump is several lines; the main line is the finding, request / read-end words and guest stacks are detail
      Cls c{"load_watch", 0, {"[load] WATCH"}, R"(\[wwe2k14\]\[load\] WATCH: (.*)$)", {"why"}, {{"n", "one"}, {"why", "first"}, {"det", "detail"}}, "n",
            "{n} loader WATCH line(s) (+{det} detail line(s): request words, guest stacks)"};
      c.detail = R"(\[wwe2k14\]\[load\] WATCH (?:request|read-end job|read-end object) 0x[0-9A-Fa-f]+ words \d+\.\.\d+:|\[wwe2k14\]\[load\] WATCH stacks?[ :]|\[wwe2k14\]\[load\] WATCH (?:h\d+ fields|loader heap|where):)";   // S106 0F: the A6W2 dump lines, as triage.py
      add(std::move(c));
    }
    {
      Cls c{"holes", 2, {"[scene][ready] calls"}, R"(\[scene\]\[ready\] calls \d+ .*?; holes (\d+) skipped \[(.*)\]; no stage)", {"n", "rest"},
            {{"n", "sum"}, {"rest", "first_hole"}}, "n", "{n} calls with a hole (a model the game draws and ours does not; counts frames, not models)"};
      c.periodic = true;
      add(std::move(c));
    }
    add({"hole_skip", 2, {"skip frame"}, R"(\[scene\]\[wrestlers\] skip frame (\d+) obj (\S+) reason (.*?) hole (yes|no)\s*$)", {"frame", "obj", "why", "hole"},
         {{"hole", "count_yes"}, {"why", "first_if_yes"}}, "hole", "{hole} wrestler skip line(s) marked hole yes"});
    {
      Cls c{"caw_holes", 2, {"[rdr][caw] guest draw: models built"}, R"(guest draw: models built (\d+), holes (\d+),.*?own-skeleton piece holes of ALL wrestlers (\d+)\))",
            {"built", "holes", "osh"}, {{"osh", "delta"}, {"holes", "delta"}}, "osh",
            "{osh} own-skeleton piece holes (counted over every wrestler drawn from guest rows, stock ones too, not CAW only)"};
      c.periodic = true;
      c.why_const = "[rdr][caw] guest draw summary";
      add(std::move(c));
    }
    add({"caw_textures", 9, {"[rdr][caw] guest draw: models built"},
         R"(textures bound (\d+) from override (\d+) missing (\d+) engine_default_cube (\d+) \(parts without a diffuse (\d+)\))", {"tb", "ov", "miss", "edc", "pwd"},
         {{"miss", "delta"}, {"pwd", "delta"}}, "", "missing textures +{miss}, parts without a diffuse +{pwd}"});
    {
      Cls c{"pic_refusals", 3, {"picture refusals"},
            R"(picture refusals (\d+): ([^;]*)(?:; first refused key (0x[0-9A-Fa-f]+) \((\w+)\): (\w+), birth site (.*0x[0-9A-Fa-f]+))?)", {"n", "by", "key", "kwhy", "creator", "site"},
            {{"n", "delta"}}, "n", "{n} picture refusals (a 2D sprite whose picture the host has no source for)"};
      c.periodic = true;
      add(std::move(c));
    }
    {
      Cls c{"black_no_3d", 3, {"[host] frames"}, R"(\[host\] frames \d+,.*? black_no_3d (\d+) \(mid_match (\d+)\))", {"n", "mm"}, {{"n", "delta"}, {"mm", "delta"}}, "n",
            "{n} frames with no 3D picture (black_no_3d, {mm} mid-match)"};
      c.periodic = true;
      c.why_const = "[host] frames census";
      add(std::move(c));
    }
    {
      Cls c{"rows_not_drawn", 4, {"[host] frames"}, R"(\[host\] frames \d+,.*? 2d_rows \d+ drawn \d+ not_drawn (\d+) \(([^)]*)\))", {"n", "why"}, {{"n", "delta"}, {"why", "pairs"}}, "n",
            "{n} 2D rows not drawn"};
      c.periodic = true;
      add(std::move(c));
    }
    {
      // S106 0F: the [host] frames line's `holes N` (host_core.h:527), as triage.py's holes_2d row
      Cls c{"holes_2d", 2, {"[host] frames"}, R"(\[host\] frames \d+,.*? 2d_rows \d+ drawn \d+ not_drawn \d+ .*?holes (\d+))", {"n"}, {{"n", "delta"}}, "n",
            "{n} 2D holes (sprite rows the game draws and the host has no draw for)"};
      c.periodic = true;
      c.why_const = "[host] frames census holes";
      add(std::move(c));
    }
    {
      Cls c{"strikes", 4, {"[host][strikes]"}, R"(\[host\]\[strikes\] (\d+) strikes: .*\| why: (.*)$)", {"n", "why"}, {{"n", "delta"}, {"why", "first"}}, "n",
            "{n} strikes (frames the host could not present as a built frame)"};
      c.periodic = true;
      add(std::move(c));
    }
    {
      Cls c{"fallback_2d", 4, {"[scene][2d] frames", "[scene][2d] exit:"}, R"(\[scene\]\[2d\] (?:exit: )?frames (\d+), 2d-whole (\d+), fallback (\d+) \(by reason: (.*?)\); rows)",
            {"frames", "whole", "n", "why"}, {{"n", "sum"}, {"why", "first_nz"}}, "n", "{n} 2D frames fell back (not whole 2D)"};
      c.cut_totals = c.scene_attr = true;
      add(std::move(c));
    }
    add({"technique_fallback", 6, {"technique fallback:"}, R"(\[scene\]\[front_end\] technique fallback: (\S+ technique \d+))", {"why"}, {{"n", "one"}, {"why", "first"}}, "n",
         "{n} technique fallback line(s)"});
    add({"reflect_fallback", 6, {"reflect fallback:"}, R"(\[scene\]\[front_end\] reflect fallback: (\S+ technique \d+:.*)$)", {"why"}, {{"n", "one"}, {"why", "first"}}, "n",
         "{n} reflect fallback line(s)"});
    add({"mem_alloc_failed", 9, {"MmAllocatePhysicalMemoryEx failed"}, R"(MmAllocatePhysicalMemoryEx failed #(\d+).*?size=(0x[0-9A-Fa-f]+))", {"k", "size"},
         {{"n", "one"}, {"size", "set"}}, "", "{n} MmAllocatePhysicalMemoryEx failures (sizes {size})"});
    add({"heap_alloc_failed", 9, {"BaseHeap::Alloc failed"}, R"(BaseHeap::Alloc failed (.*)$)", {"why"}, {{"n", "one"}}, "", "{n} BaseHeap::Alloc failures"});
    add({"frames_census", 9, {"[host] frames"}, R"(\[host\] frames (\d+), ours (\d+), 2d_only (\d+), repeat_3d (\d+) \(loading (\d+)\))", {"frames", "ours", "only2d", "rep", "loading"},
         {{"frames", "delta"}, {"ours", "delta"}, {"only2d", "delta"}, {"rep", "delta"}}, "", "frames +{frames}: ours {ours}, 2d only {only2d}, repeat 3d {rep}"});
    add({"load_adopted", 9, {"[scene][load] adopted"}, R"(\[scene\]\[load\] adopted #\d+: (.*), waited (\d+) ms)", {"what", "ms"}, {{"ms", "max"}, {"what", "first"}}, "",
         "slowest scene load {ms} ms"});
    // S106 0FF B1 / F7: the crash handler's lines and the [host] frames counters that had no class (host_core.h:526-527), as triage.py's last rows (same order)
    // S106 0FFN N2: the hang guard's lines (hang_guard.h:326 / :344 / :314 / :382), as triage.py's row before crash (same order)
    add({"hang", 0, {"[hang] HANG?", "[hang] HANG:", "[hang] BLACK:"}, R"(\[hang\] ((?:HANG\?|HANG:|BLACK:) .*)$)", {"why"}, {{"n", "one"}, {"why", "first"}}, "n",
         "{n} [hang] line(s) (the hang guard fired: HANG / BLACK)"});
    add({"crash", 0, {"[wwe2k14][crash]"}, R"(\[wwe2k14\]\[crash\] (.*)$)", {"why"}, {{"n", "one"}, {"why", "first"}}, "n", "{n} [crash] line(s) (the game crashed)"});
    for (const char* ctr : {"no_record", "no_picture", "no_size", "resize_black", "repeat_full", "particles_not_drawn"}) {
      Cls c{ctr, 4, {"[host] frames"}, std::string(R"(\[host\] frames \d+,.*? )") + ctr + R"( (\d+))", {"n"}, {{"n", "delta"}}, "n",
            std::string("{n} frames counted as ") + ctr + " (a [host] frames counter that stays 0 in a clean run)"};
      c.periodic = true;
      add(std::move(c));
    }
    return t;
  }();
  return table;
}

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
inline const char* const kProblemNote =
    "problems are first-of-name per run: the game logs only the FIRST [rdr][PROBLEM] line of each name (src/renderer/problem.cpp:64), so a count here is not an occurrence count";

namespace detail {

using Key = std::pair<std::string, std::string>;  // (class, field)

inline bool Has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }
inline std::string Strip(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (unsigned char)s[a] <= ' ') ++a;
  while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
  return s.substr(a, b - a);
}
// the first n characters (UTF-8 code points, like Python's s[:n])
inline std::string Cut(const std::string& s, size_t n) {
  size_t i = 0, count = 0;
  while (i < s.size() && count < n) {
    ++i;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    ++count;
  }
  return s.substr(0, i);
}
inline std::string Fmt(const char* f, ...) {
  char buf[4096];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}
inline std::string I(long long v) { return std::to_string(v); }
inline long long ToInt(const std::string& s) {
  try {
    return std::stoll(s);
  } catch (...) {
    return 0;
  }
}
inline std::string Slug(const std::string& name) {
  std::string out;
  bool pend = false;
  for (char ch : name) {
    const char c = (ch >= 'A' && ch <= 'Z') ? char(ch + 32) : ch;
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      if (pend && !out.empty()) out += '_';
      pend = false;
      out += c;
    } else {
      pend = true;
    }
  }
  return out;
}

struct Stamp {
  double t = 0;
  std::string hm;
};
// "[YYYY-MM-DD HH:MM:SS.mmm]" at the start of the line (25 characters)
inline bool ParseStamp(const std::string& s, Stamp& out) {
  if (s.size() < 25 || s[0] != '[') return false;
  auto dig = [&](size_t i, size_t n) {
    for (size_t k = i; k < i + n; ++k)
      if (s[k] < '0' || s[k] > '9') return false;
    return true;
  };
  if (!dig(1, 4) || s[5] != '-' || !dig(6, 2) || s[8] != '-' || !dig(9, 2) || s[11] != ' ' || !dig(12, 2) || s[14] != ':' || !dig(15, 2) || s[17] != ':' ||
      !dig(18, 2) || s[20] != '.' || !dig(21, 3) || s[24] != ']')
    return false;
  const int y = std::stoi(s.substr(1, 4)), mo = std::stoi(s.substr(6, 2)), da = std::stoi(s.substr(9, 2));
  const int h = std::stoi(s.substr(12, 2)), mi = std::stoi(s.substr(15, 2)), se = std::stoi(s.substr(18, 2)), ms = std::stoi(s.substr(21, 3));
  out.t = double((long long)(y * 372 + mo * 31 + da) * 86400) + h * 3600 + mi * 60 + se + ms / 1000.0;
  out.hm = s.substr(12, 8);
  return true;
}

struct First {
  std::optional<std::string> text;
  std::optional<std::string> loc;
  std::string hm;
};

struct Span {
  long long seq;
  std::string name;
  long long frame0;
  double t0, t1;
  std::string hm0;
  std::map<Key, long long> ints;
  std::map<Key, std::set<std::string>> sets;
  std::map<Key, First> firsts;  // (class, "reason"|"line")
  std::map<Key, long long> base, last, carry;
  std::vector<std::pair<std::string, std::pair<long long, std::string>>> unparsed;  // class -> (count, first loc), insertion ordered
  std::vector<std::pair<double, long long>> opens;
  std::optional<long long> frames_at_start, frames_at_end;
  Span(long long s, std::string n, long long f, double t, std::string hm) : seq(s), name(std::move(n)), frame0(f), t0(t), t1(t), hm0(std::move(hm)) {}
};

struct Cross {
  std::string cls;
  long long amount;
  size_t from_idx, to_idx;
  std::optional<std::string> reason;
  std::string loc, hm;
};

struct Result {
  std::string name;
  long long lines = 0;
  std::map<std::string, std::string> machine;
  std::vector<Span> spans;
  std::vector<std::pair<std::string, std::pair<long long, std::string>>> unparsed_all;  // class -> (count, loc)
  long long file_lines = 0;
  std::vector<std::string> notes, parts, classes_not_read;
  double t_first = 0, t_last = 0;
  long long failed_opens = 0, failed_open_paths = 0;
  std::vector<Cross> crossings;
  facts::Facts facts;  // S108 RPT: fed every line by ReadLog, printed under the LOG line
};

inline void AddUnparsed(std::vector<std::pair<std::string, std::pair<long long, std::string>>>& v, const std::string& cls, const std::string& loc) {
  for (auto& e : v)
    if (e.first == cls) {
      ++e.second.first;
      return;
    }
  v.push_back({cls, {1, loc}});
}

using Groups = std::vector<std::optional<std::string>>;
inline const std::string* Grp(const Cls& c, const Groups& g, const char* name) {
  for (size_t i = 0; i < c.groups.size(); ++i)
    if (c.groups[i] == name) return g[i] ? &*g[i] : nullptr;
  return nullptr;
}
inline std::string GrpS(const Cls& c, const Groups& g, const char* name) {
  const std::string* p = Grp(c, g, name);
  return p ? *p : std::string();
}

// THE regex entry: every regex_search of this file goes through here. The text is capped at 4000 characters (the MS STL regex recurses per character:
// a huge line must not blow its stack) and a regex_error (complexity / stack) is caught: both read as "no match" (= UNPARSED for a class line, loud),
// never as an exception that would turn the whole log into "could not read the log". g[0] = the whole match, g[i] = group i (nullopt = did not take part).
using Match = std::vector<std::optional<std::string>>;
inline bool Rx(const std::regex& rx, const std::string& s, Match& g) {
  g.clear();
  try {
    std::smatch m;
    if (s.size() > 4000) {
      const std::string cut = s.substr(0, 4000);
      if (!std::regex_search(cut, m, rx)) return false;
      for (size_t i = 0; i < m.size(); ++i) g.push_back(m[i].matched ? std::optional<std::string>(m[i].str()) : std::nullopt);
      return true;
    }
    if (!std::regex_search(s, m, rx)) return false;
    for (size_t i = 0; i < m.size(); ++i) g.push_back(m[i].matched ? std::optional<std::string>(m[i].str()) : std::nullopt);
    return true;
  } catch (const std::regex_error&) {
    g.clear();
    return false;
  }
}

using Pairs = std::vector<std::pair<std::string, long long>>;

// 'picture 1568, page_rows 30' against the previous line's pairs -> the pairs that rose ("page_rows +30"), nullopt when none
inline std::optional<std::string> PairDiff(const Pairs& prev, const std::string& text, Pairs& cur_out) {
  static const std::regex rx(R"(([A-Za-z_]\w*) (\d+))");
  Pairs cur;
  const std::string capped = text.size() > 4000 ? text.substr(0, 4000) : text;
  std::vector<std::pair<std::string, std::string>> found_pairs;
  try {
    for (auto it = std::sregex_iterator(capped.begin(), capped.end(), rx); it != std::sregex_iterator(); ++it) found_pairs.push_back({(*it)[1].str(), (*it)[2].str()});
  } catch (const std::regex_error&) {
  }
  for (const auto& fp : found_pairs) {
    const std::string k = fp.first;
    const long long v = ToInt(fp.second);
    bool found = false;
    for (auto& p : cur)
      if (p.first == k) p.second = v, found = true;
    if (!found) cur.push_back({k, v});
  }
  std::string out;
  for (const auto& p : cur) {
    long long pv = 0;
    for (const auto& q : prev)
      if (q.first == p.first) pv = q.second;
    if (p.second > pv) out += (out.empty() ? "" : ", ") + p.first + " +" + I(p.second - pv);
  }
  cur_out = cur;
  if (out.empty()) return std::nullopt;
  return out;
}

inline std::optional<std::string> ReasonOf(const Cls& c, const Groups& g, const std::optional<std::string>& diff) {
  const std::string& nm = c.name;
  auto nonempty = [](const std::string& s) { return s.empty() ? std::optional<std::string>() : std::optional<std::string>(s); };
  if (nm == "holes") {
    const std::string rest = GrpS(c, g, "rest");
    const std::string st = Strip(rest);
    if (st == "-" || st.empty()) return std::nullopt;
    static const std::regex rx(R"(\(first: (.*)\)\s*$)");
    Match m;
    if (Rx(rx, rest, m) && m[1]) return *m[1];
    return rest;
  }
  if (nm == "pic_refusals") {
    std::string r = diff ? *diff : Strip(GrpS(c, g, "by"));
    const std::string key = GrpS(c, g, "key");
    if (!key.empty()) r += " | first refused key " + key + " (" + GrpS(c, g, "kwhy") + "): " + GrpS(c, g, "creator") + ", birth site " + GrpS(c, g, "site");
    return nonempty(r);
  }
  if (nm == "rows_not_drawn") {
    if (diff) return diff;
    const std::string w = Strip(GrpS(c, g, "why"));
    return (w == "-" || w.empty()) ? std::nullopt : std::optional<std::string>(w);
  }
  if (nm == "strikes") {
    const std::string why = GrpS(c, g, "why");
    return nonempty(Strip(why.substr(0, why.find(';'))));
  }
  if (nm == "fallback_2d") {
    const std::string why = GrpS(c, g, "why");
    std::string out;
    size_t pos = 0;
    while (true) {
      const size_t semi = why.find(';', pos);
      const std::string p = Strip(why.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos));
      const bool dash = p.size() >= 2 && p.compare(p.size() - 2, 2, " -") == 0;
      if (!p.empty() && !dash) out += (out.empty() ? "" : "; ") + p;
      if (semi == std::string::npos) break;
      pos = semi + 1;
    }
    return nonempty(out);
  }
  if (nm == "hole_skip") return GrpS(c, g, "hole") == "yes" ? nonempty(GrpS(c, g, "why")) : std::nullopt;
  if (nm == "black_no_3d" || nm == "caw_holes" || nm == "holes_2d") return nonempty(c.why_const);
  const std::string why = GrpS(c, g, "why");
  if (!why.empty()) return nm == "problem" ? GrpS(c, g, "name") + ": " + why : why;
  return std::nullopt;
}

// one matched line of class c into the span; returns the finding field's rise / count on this line (and the reason)
inline void Feed(Span& span, const Cls& c, const Groups& g, const std::string& loc, const std::string& hm, std::map<Key, long long>& cum,
                 std::map<std::string, Pairs>& pairs, bool crossing, long long& amount_out, std::optional<std::string>& reason_out) {
  long long amount = 0;
  std::optional<std::string> diff;
  bool whyPairs = false;
  for (const auto& f : c.fields)
    if (std::string(f.group) == "why" && std::string(f.how) == "pairs") whyPairs = true;
  if (whyPairs || c.name == "pic_refusals") {
    const std::string src = c.name == "rows_not_drawn" ? GrpS(c, g, "why") : GrpS(c, g, "by");
    Pairs cur;
    diff = PairDiff(pairs[c.name], src, cur);
    pairs[c.name] = cur;
  }
  for (const auto& f : c.fields) {
    const std::string fld = f.group, how = f.how;
    const Key key{c.name, fld};
    const bool isf = fld == c.finding;
    if (how == "one") {
      if (isf) amount = 1;
      if (!crossing) span.ints[key] += 1;
    } else if (how == "sum") {
      const long long v = ToInt(GrpS(c, g, f.group));
      if (isf) amount = v;
      if (!crossing) span.ints[key] += v;
    } else if (how == "max") {
      auto it = span.ints.find(key);
      span.ints[key] = std::max(it == span.ints.end() ? 0LL : it->second, ToInt(GrpS(c, g, f.group)));
    } else if (how == "count_yes") {
      const long long y = GrpS(c, g, f.group) == "yes" ? 1 : 0;
      if (isf) amount = y;
      span.ints[key] += y;
    } else if (how == "delta") {
      const long long v = ToInt(GrpS(c, g, f.group));
      auto cit = cum.find(key);
      const long long b = cit == cum.end() ? 0 : cit->second;
      const long long prev = v < b ? 0 : b;  // a counter that fell = a new run: its rise is the new value
      if (isf) amount = v - prev;
      if (!span.base.count(key)) {
        span.base[key] = crossing ? v : prev;
        span.last[key] = span.base[key];
      }
      if (!crossing) {
        if (v < span.last[key]) {  // fell mid-span: bank the rise so far, count again from 0
          span.carry[key] += span.last[key] - span.base[key];
          span.base[key] = 0;
        }
        span.last[key] = v;
      }
      cum[key] = v;
    } else if (how == "set") {
      span.sets[key].insert(GrpS(c, g, f.group));
    }
  }
  std::optional<std::string> reason = ReasonOf(c, g, diff);
  const bool rose = c.finding.empty() ? true : amount > 0;
  if (!crossing && rose) {
    if (reason && !span.firsts.count({c.name, "reason"})) span.firsts[{c.name, "reason"}] = First{reason, loc, hm};
    if (!span.firsts.count({c.name, "line"})) span.firsts[{c.name, "line"}] = First{std::nullopt, loc, hm};
  }
  amount_out = amount;
  reason_out = reason;
}

inline void FinishSpan(Span& s) {
  std::set<Key> keys;
  for (auto& e : s.last) keys.insert(e.first);
  for (auto& e : s.carry) keys.insert(e.first);
  for (const Key& k : keys) {
    const long long c = s.carry.count(k) ? s.carry[k] : 0, l = s.last.count(k) ? s.last[k] : 0, b = s.base.count(k) ? s.base[k] : 0;
    s.ints[k] = c + std::max(0LL, l - b);
  }
}

struct PartIn {
  std::string label;
  std::filesystem::path path;
};

inline std::string RxFirst(const std::regex& rx, const std::string& s, int group) {
  Match m;
  if (Rx(rx, s, m) && size_t(group) < m.size() && m[group]) return *m[group];
  return {};
}

// a game log: parts oldest first, streamed. Throws std::runtime_error when no line looks like a game log.
inline Result ReadLog(const std::string& name, const std::vector<PartIn>& parts) {
  static const std::regex rxSeq(R"(\[wwe2k14\]\[scene\] sequence (\d+) -> (\d+) \((.*?)\) at frame (\d+))");
  static const std::regex rxVer(R"(\[wwe2k14\] version (.*)$)"), rxNum(R"(\[wwe2k14\] run log number: (\d+))"), rxTier(R"(\[wwe2k14\] quality tier: (.*)$)");
  static const std::regex rxGpu(R"(\[wwe2k14\]\[gpu\] device 0: (.*)$)");
  static const std::regex rxScene(R"(; in scene ([A-Za-z0-9_]+))");
  static const std::regex rxHostFrames(R"(\[host\] frames (\d+),)");
  std::vector<Cls>& classes = Classes();
  Result res;
  res.name = name;
  std::map<Key, long long> cum;
  std::map<std::string, Pairs> pairs;
  std::map<std::string, long long> cstate;  // class -> index of the span of its last periodic line
  res.spans.emplace_back(-1, "(before the first sequence line)", 0, 0.0, "");
  std::map<std::string, long long> slugs;
  std::map<std::string, long long> seen, unstamped, unplaced;
  for (const Cls& c : classes) seen[c.name] = 0;
  long long ts_seen = 0, n_lines = 0;
  double t_first = 0, t_last = 0, last_t = 0;
  bool have_first = false, have_last_hm = false;
  std::string last_hm;
  long long file_lines = 0, trailer = 0, host_frames = 0, seq_lines = 0, seq_bad = 0;
  bool condensed = false, cut = false, in_trailer = false;
  std::unordered_map<std::string, long long> fails;
  std::vector<std::pair<const std::string*, const Cls*>> markers;
  for (const Cls& c : classes)
    for (const std::string& mk : c.markers) markers.push_back({&mk, &c});
  const bool multi = parts.size() > 1;
  size_t cur = 0;  // index of the current span

  size_t part_i = 0;
  for (const PartIn& part : parts) {
    const bool last_part = ++part_i == parts.size();
    std::ifstream f(part.path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + part.label);
    std::string line;
    long long i = 0;
    while (std::getline(f, line)) {
      ++i;
      ++n_lines;
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const std::string loc = multi ? part.label + ":" + I(i) : I(i);
      Stamp st;
      bool stamped = false;
      double t;
      if (ParseStamp(line, st)) {
        in_trailer = false;
        t = st.t;
        last_t = t;
        last_hm = st.hm;
        have_last_hm = true;
        if (!have_first) {
          have_first = true;
          t_first = t;
          res.spans[cur].t0 = res.spans[cur].t1 = t;
        }
        t_last = t;
        ++ts_seen;
        stamped = true;
      } else {
        if (line.compare(0, 8, "[report]") == 0) {
          if (Has(line, "lines left out after their first 30 copies")) condensed = in_trailer = true;
          else if (Has(line, "bytes left out")) cut = true;
          continue;
        }
        if (in_trailer) {
          ++trailer;
          continue;
        }
        if (Strip(line).empty()) continue;
        t = last_t;
      }
      const std::string& hm = last_hm;
      res.facts.Feed(line, stamped, hm, last_part);  // S108 RPT: every line, the same pass
      if (stamped) {
        if (Has(line, "[wwe2k14] ") && (Has(line, "version ") || Has(line, "run log number") || Has(line, "quality tier"))) {
          std::string v;
          if (!(v = RxFirst(rxVer, line, 1)).empty()) res.machine.emplace("version", v);
          if (!(v = RxFirst(rxNum, line, 1)).empty()) res.machine.emplace("run_log", v);
          if (!(v = RxFirst(rxTier, line, 1)).empty()) res.machine.emplace("quality_tier", v);
        }
        if (Has(line, "][gpu] device 0:") && !res.machine.count("gpu")) {
          const std::string v = RxFirst(rxGpu, line, 1);
          if (!v.empty()) res.machine["gpu"] = v;
        }
        if (Has(line, "[wwe2k14][scene] sequence ")) {
          Match ms;
          if (Rx(rxSeq, line, ms)) {
            ++seq_lines;
            res.spans[cur].t1 = t;
            res.spans.emplace_back(ToInt(*ms[2]), *ms[3], ToInt(*ms[4]), t, hm);
            cur = res.spans.size() - 1;
            res.spans[cur].frames_at_start = host_frames;
            slugs[Slug(*ms[3])] = ToInt(*ms[2]);
          } else {
            ++seq_bad;
            AddUnparsed(res.unparsed_all, "sequence", loc);
          }
          continue;
        }
      }
      {  // the file open line (src\kernel_overrides.h:225); the parse is shared with the facts block (report_facts.h ParseFileOpen)
        std::string path, status;
        if (facts::Facts::ParseFileOpen(line, path, status)) {
          ++file_lines;
          res.spans[cur].opens.push_back({t, host_frames});
          if (status != "00000000") ++fails[path];
          continue;
        }
      }
      if (Has(line, "[host] frames")) {
        Match mh;
        if (Rx(rxHostFrames, line, mh)) {
          host_frames = ToInt(*mh[1]);
          res.spans[cur].frames_at_end = host_frames;
        }
      }
      for (const auto& mk : markers) {
        const Cls& c = *mk.second;
        if (!Has(line, mk.first->c_str())) continue;
        if (!c.detail.empty()) {
          Match dm;
          if (Rx(c.detail_rx, line, dm)) {  // a continuation line of a multi-line class: detail, not a finding, not UNPARSED
            ++res.spans[cur].ints[{c.name, "det"}];
            continue;
          }
        }
        ++seen[c.name];
        if (!stamped) {
          if (!have_last_hm) {
            ++unplaced[c.name];
            continue;
          }
          ++unstamped[c.name];
        }
        std::string body = line;
        if (c.cut_totals) body = line.substr(0, line.find("; run totals:"));
        Match m;
        if (!Rx(c.rx, body, m)) {  // Rx caps at 4000 and catches regex_error: such a line reads as UNPARSED (loud), never as 0
          auto& up = res.spans[cur].unparsed;
          bool found = false;
          for (auto& e : up)
            if (e.first == c.name) ++e.second.first, found = true;
          if (!found) up.push_back({c.name, {1, loc}});
          AddUnparsed(res.unparsed_all, c.name, loc);
          continue;
        }
        Groups g(c.groups.size());
        for (size_t k = 0; k < c.groups.size(); ++k)
          if (k + 1 < m.size() && m[k + 1]) g[k] = *m[k + 1];
        size_t target = cur;
        if (c.scene_attr) {
          Match sm;
          if (Rx(rxScene, body, sm) && sm[1] && slugs.count(*sm[1])) {
            const long long want = slugs[*sm[1]];
            for (size_t k = res.spans.size(); k-- > 0;)
              if (res.spans[k].seq == want) {
                target = k;
                break;
              }
          }
        }
        bool crossing = false;
        size_t prev_idx = 0;
        if (c.periodic) {
          auto it = cstate.find(c.name);
          const size_t here = res.spans.size() - 1;
          crossing = it != cstate.end() && size_t(it->second) != here;
          if (it != cstate.end()) prev_idx = size_t(it->second);
          cstate[c.name] = (long long)here;
        }
        long long amount;
        std::optional<std::string> reason;
        Feed(res.spans[target], c, g, loc, hm, cum, pairs, crossing, amount, reason);
        if (crossing && amount > 0) res.crossings.push_back({c.name, amount, prev_idx, res.spans.size() - 1, reason, loc, hm});
      }
    }
  }
  if (ts_seen == 0) throw std::runtime_error(name + ": no line starts with a [YYYY-MM-DD HH:MM:SS.mmm] stamp: not a game log");
  res.spans[cur].t1 = t_last;
  for (Span& s : res.spans) FinishSpan(s);
  if (condensed)
    res.notes.push_back("LOG CONDENSED: the launcher left out repeated lines after their first 30 copies (" + I(trailer) +
                        " trailer line(s) not read); counts of repeated lines are LOW");
  if (cut)
    res.notes.push_back(
        "LOG CUT: the launcher kept only the first 4 MB and last 20 MB of an over-24 MB log; counters between them are missing (cumulative counters still rise correctly across the cut)");
  for (const auto& e : unplaced) res.notes.push_back(I(e.second) + " line(s) of class " + e.first + " came before the first stamped line: not placed in a mode, not counted");
  for (const auto& e : unstamped)
    res.notes.push_back(I(e.second) + " unstamped line(s) of class " + e.first + " (a continuation or a broken stamp) were taken into the previous stamp's mode");
  if (seq_lines == 0)
    res.notes.push_back("NO MODE READ: no `sequence A -> B (Name)` line parsed" +
                        std::string(seq_bad ? " (" + I(seq_bad) + " line(s) of the sequence format changed: UNPARSED)" : "") +
                        ", so every counter below is for the whole log, not per mode");
  for (const Cls& c : classes)
    if (seen[c.name] == 0) res.classes_not_read.push_back(c.name);
  if (!file_lines) res.classes_not_read.push_back("no_file_open");
  res.file_lines = file_lines;
  res.t_first = t_first;
  res.t_last = t_last;
  res.lines = n_lines;
  res.failed_open_paths = (long long)fails.size();
  for (const auto& e : fails) res.failed_opens += e.second;
  return res;
}

struct Mode {
  long long seq;
  std::string name;
  long long visits = 0;
  std::string first_t;
  long long first_frame = 0;
  double seconds = 0;
  long long frames = 0;
  std::map<Key, long long> ints;
  std::map<Key, std::set<std::string>> sets;
  std::map<Key, First> firsts;
  std::vector<std::pair<std::string, std::pair<long long, std::string>>> unparsed;
  struct Gap {
    double dur;
    long long frames;
    double at;
  };
  std::optional<Gap> gap, trail;
};

inline std::vector<Mode> ModeTable(const Result& res, double gap_s) {
  std::vector<Mode> modes;
  std::map<long long, size_t> idx;
  const Span* last_span = &res.spans.back();
  for (const Span& s : res.spans) {
    if (s.seq == -1 && s.ints.empty() && s.sets.empty() && s.firsts.empty() && s.opens.empty() && s.base.empty() && s.unparsed.empty()) continue;
    auto it = idx.find(s.seq);
    if (it == idx.end()) {
      Mode m;
      m.seq = s.seq;
      m.name = s.name;
      m.first_t = s.hm0;
      m.first_frame = s.frame0;
      modes.push_back(std::move(m));
      it = idx.emplace(s.seq, modes.size() - 1).first;
    }
    Mode& md = modes[it->second];
    ++md.visits;
    md.seconds += std::max(0.0, s.t1 - s.t0);
    if (s.frames_at_start && s.frames_at_end) md.frames += std::max(0LL, *s.frames_at_end - *s.frames_at_start);
    for (const auto& e : s.ints) md.ints[e.first] += e.second;
    for (const auto& e : s.sets) md.sets[e.first].insert(e.second.begin(), e.second.end());
    for (const auto& e : s.firsts) md.firsts.emplace(e.first, e.second);
    for (const auto& e : s.unparsed) {
      bool found = false;
      for (auto& o : md.unparsed)
        if (o.first == e.first) o.second.first += e.second.first, found = true;
      if (!found) md.unparsed.push_back(e);
    }
    if (res.file_lines && s.frames_at_start) {
      std::vector<std::pair<double, long long>> pts;
      pts.push_back({s.t0, *s.frames_at_start});
      for (const auto& o : s.opens) pts.push_back(o);
      pts.push_back({s.t1, s.frames_at_end ? *s.frames_at_end : *s.frames_at_start});
      for (size_t k = 0; k + 1 < pts.size(); ++k) {
        const double ta = pts[k].first, tb = pts[k + 1].first;
        const long long fa = pts[k].second, fb = pts[k + 1].second;
        if (tb - ta >= gap_s && fb - fa >= 60) {
          if (!md.gap || tb - ta > md.gap->dur) md.gap = Mode::Gap{tb - ta, fb - fa, ta};
          if (&s == last_span && k == pts.size() - 2) md.trail = Mode::Gap{tb - ta, fb - fa, ta};
        }
      }
    }
  }
  return modes;
}

struct Finding {
  std::string mode, cls, text, reason;
  long long count;
  int prio;
  std::optional<std::string> line;
  std::string at;
};

// the text template: "{n} ..." with the values; false when it names a field the class has none of (Python's KeyError)
inline bool Template(const std::string& t, const std::map<std::string, std::string>& v, std::string& out) {
  out.clear();
  for (size_t i = 0; i < t.size();) {
    if (t[i] == '{') {
      const size_t e = t.find('}', i);
      if (e == std::string::npos) return false;
      auto it = v.find(t.substr(i + 1, e - i - 1));
      if (it == v.end()) return false;
      out += it->second;
      i = e + 1;
    } else {
      out += t[i++];
    }
  }
  return true;
}

inline std::string JoinSet(const std::set<std::string>& s) {
  std::string o;
  for (const auto& x : s) o += (o.empty() ? "" : ",") + x;
  return o;
}

// the value a (class, field) has in a mode: int, or the set joined; truthy = nonzero / nonempty
inline std::string ModeVal(const Mode& md, const Key& k, bool* truthy = nullptr) {
  auto s = md.sets.find(k);
  if (s != md.sets.end()) {
    if (truthy) *truthy = !s->second.empty();
    return JoinSet(s->second);
  }
  auto i = md.ints.find(k);
  const long long v = i == md.ints.end() ? 0 : i->second;
  if (truthy) *truthy = v != 0;
  return I(v);
}

inline std::string SpanLabel(const Result& res, size_t i0, size_t i1) {
  std::vector<std::string> names;
  for (size_t i = i0; i <= i1 && i < res.spans.size(); ++i) {
    const std::string n = I(res.spans[i].seq) + " " + res.spans[i].name;
    if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
  }
  std::string o;
  for (const auto& n : names) o += (o.empty() ? "" : " / ") + n;
  return o;
}

inline std::vector<Finding> FindingsFor(const Result& res, const std::vector<Mode>& modes) {
  std::vector<Finding> out;
  for (const Mode& md : modes) {
    const std::string modeName = I(md.seq) + " " + md.name;
    for (const Cls& c : Classes()) {
      if (c.finding.empty()) continue;
      auto vi = md.ints.find({c.name, c.finding});
      const long long n = vi == md.ints.end() ? 0 : vi->second;
      if (!n) continue;
      First reason, line;
      if (auto r = md.firsts.find({c.name, "reason"}); r != md.firsts.end()) reason = r->second;
      if (auto l = md.firsts.find({c.name, "line"}); l != md.firsts.end()) line = l->second;
      std::map<std::string, std::string> vals;
      for (const auto& f : c.fields) vals[f.group] = ModeVal(md, {c.name, f.group});
      Finding f;
      f.mode = modeName;
      f.cls = c.name;
      f.count = n;
      f.prio = c.prio;
      Template(c.text, vals, f.text);
      f.reason = reason.text ? *reason.text : c.why_const;
      f.line = reason.loc ? reason.loc : line.loc;
      f.at = !reason.hm.empty() ? reason.hm : line.hm;
      out.push_back(std::move(f));
    }
    if (md.trail) {
      Finding f;
      f.mode = modeName;
      f.cls = "no_file_open";
      f.count = (long long)md.trail->dur;
      f.prio = 1;
      f.text = Fmt("no file opened for %d s while %lld frames advanced", (int)std::nearbyint(md.trail->dur), md.trail->frames);
      f.reason =
          "the gap runs to the END of the log: a load that never finished, OR a normal quit after an idle menu (the loader WATCH line, when present, is the finding; this is supporting evidence)";
      out.push_back(std::move(f));
    }
    for (const auto& u : md.unparsed) {
      Finding f;
      f.mode = modeName;
      f.cls = u.first;
      f.count = u.second.first;
      f.prio = -1;
      f.text = "UNPARSED: " + I(u.second.first) + " line(s) of class " + u.first + " present, format not read";
      f.reason = "the line's format changed: update the pattern table";
      f.line = u.second.second;
      out.push_back(std::move(f));
    }
  }
  // crossings, merged by (class, span label), keeping the first one's reason / line / time
  std::vector<std::pair<std::pair<std::string, std::string>, Cross>> merged;
  for (const Cross& cr : res.crossings) {
    const std::pair<std::string, std::string> k{cr.cls, SpanLabel(res, cr.from_idx, cr.to_idx)};
    bool found = false;
    for (auto& m : merged)
      if (m.first == k) m.second.amount += cr.amount, found = true;
    if (!found) merged.push_back({k, cr});
  }
  for (const auto& m : merged) {
    const Cls* c = nullptr;
    for (const Cls& x : Classes())
      if (x.name == m.first.first) c = &x;
    if (!c) continue;
    std::map<std::string, std::string> vals;
    for (const auto& f : c->fields) vals[f.group] = "?";
    vals[c->finding] = I(m.second.amount);
    Finding f;
    f.mode = "mode not known: " + m.first.second;
    f.cls = c->name;
    f.count = m.second.amount;
    f.prio = c->prio;
    Template(c->text, vals, f.text);
    f.text += " [rise between census lines that straddle a mode change]";
    f.reason = m.second.reason ? *m.second.reason : std::string();
    f.line = m.second.loc;
    f.at = m.second.hm;
    out.push_back(std::move(f));
  }
  for (const auto& u : res.unparsed_all) {
    if (u.first != "sequence") continue;
    Finding f;
    f.mode = "(whole log)";
    f.cls = "sequence";
    f.count = u.second.first;
    f.prio = -1;
    f.text = "UNPARSED: " + I(u.second.first) + " line(s) of the sequence format present, not read";
    f.reason = "the sequence line's format changed: update RX_SEQ";
    f.line = u.second.second;
    out.push_back(std::move(f));
  }
  std::stable_sort(out.begin(), out.end(), [](const Finding& a, const Finding& b) { return a.prio != b.prio ? a.prio < b.prio : a.count > b.count; });
  return out;
}

struct LogFile {
  std::string label;
  std::optional<int> k;  // nullopt = the newest (base) file
  std::filesystem::path path;
};

inline std::vector<std::string> PartNotes(const std::vector<LogFile>& lst) {
  std::vector<int> ks;
  bool have_base = false;
  for (const auto& p : lst) {
    if (p.k) ks.push_back(*p.k);
    else have_base = true;
  }
  std::sort(ks.begin(), ks.end());
  std::vector<std::string> notes;
  if (!ks.empty()) {
    std::vector<int> gaps;
    for (int k = 1; k <= ks.back(); ++k)
      if (!std::count(ks.begin(), ks.end(), k)) gaps.push_back(k);
    if (!gaps.empty() || !have_base) {
      std::string present, miss;
      for (int k : ks) present += (present.empty() ? "." : ", .") + std::to_string(k);
      if (have_base) present += ", (newest)";
      for (int k : gaps) miss += (miss.empty() ? "." : ", .") + std::to_string(k);
      notes.push_back("PARTS MISSING: parts " + present + " present" + (have_base ? "" : " but not the newest file") + "; missing between/after: " +
                      (miss.empty() ? "the newest file" : miss) + ": counters across the missing part are not read");
    }
  }
  return notes;
}

// wwe2k14_NNN.log / wwe2k14_NNN.K.log (a rotated part)
inline bool ParsePartName(const std::string& n, std::string& num, std::optional<int>& k) {
  if (n.compare(0, 8, "wwe2k14_") != 0 || n.size() < 12 || n.compare(n.size() - 4, 4, ".log") != 0) return false;
  const std::string mid = n.substr(8, n.size() - 12);
  size_t d = 0;
  while (d < mid.size() && mid[d] >= '0' && mid[d] <= '9') ++d;
  if (d == 0) return false;
  num = mid.substr(0, d);
  if (d == mid.size()) {
    k = std::nullopt;
    return true;
  }
  if (mid[d] != '.' || d + 1 >= mid.size()) return false;
  for (size_t i = d + 1; i < mid.size(); ++i)
    if (mid[i] < '0' || mid[i] > '9') return false;
  k = (int)ToInt(mid.substr(d + 1));
  return true;
}

inline std::string Render(const std::vector<std::pair<Result, std::vector<std::string>>>& results, const std::vector<std::string>& sources) {
  std::string out;
  size_t total = 0;
  auto line = [&](const std::string& s) { out += s + "\n"; };
  const std::string bar(100, '=');
  for (size_t ri = 0; ri < results.size(); ++ri) {
    const Result& res = results[ri].first;
    const std::vector<std::string>& pnotes = results[ri].second;
    const std::vector<Mode> modes = ModeTable(res, 10.0);
    const std::vector<Finding> finds = FindingsFor(res, modes);
    line(bar);
    std::string partsText;
    if (res.parts.size() > 1) {
      partsText = "  parts: ";
      for (size_t i = 0; i < res.parts.size(); ++i) partsText += (i ? ", " : "") + res.parts[i];
    }
    line("LOG " + res.name + "  (" + sources[ri] + ")" + partsText);
    for (const std::string& s : res.facts.Render(res.lines)) line(s);  // S108 RPT: what was in play, at the top of the log's section
    std::string bits;
    for (const char* k : {"version", "quality_tier", "gpu", "run_log"}) {
      auto it = res.machine.find(k);
      if (it != res.machine.end()) bits += (bits.empty() ? "" : "; ") + std::string(k) + " " + it->second;
    }
    if (!bits.empty()) line("  log start: " + bits);
    line(Fmt("  %lld lines, %.0f s of log, file opens logged: ", res.lines, res.t_last - res.t_first) +
         (res.file_lines ? I(res.file_lines) : std::string("none (class no_file_open not read)")));
    for (const auto& n : pnotes) line("  NOTE: " + n);
    for (const auto& n : res.notes) line("  NOTE: " + n);
    std::string nr;
    for (const auto& n : res.classes_not_read) nr += (nr.empty() ? "" : ", ") + n;
    line("  CLASSES NOT READ (their line never appears in this log: not the same as 0): " + (nr.empty() ? std::string("none") : nr));
    line("  MODES (sequence changes), then the not-drawn counters per mode:");
    for (const Mode& md : modes) {
      line(Fmt("   %3lld %-44s t=%s frame %-6lld visits %lld, %5.0f s, ~%lld frames", md.seq, Cut(md.name, 44).c_str(), md.first_t.c_str(), md.first_frame, md.visits,
               md.seconds, md.frames));
      for (const Cls& c : Classes()) {
        bool any = false;
        std::map<std::string, std::string> vv;
        for (const auto& f : c.fields) {
          bool tr = false;
          vv[f.group] = ModeVal(md, {c.name, f.group}, &tr);
          any = any || tr;
        }
        if (!any) continue;
        std::string txt;
        if (Template(c.text, vv, txt)) line(Fmt("         %-18s ", c.name.c_str()) + txt);
      }
      if (md.gap)
        line(Fmt("         %-18s longest gap with no file open %d s (%lld frames advanced)%s", "no_file_open", (int)std::nearbyint(md.gap->dur), md.gap->frames,
                 md.trail ? "  <- runs to the end of the log" : ""));
      for (const auto& u : md.unparsed) line(Fmt("         %-18s UNPARSED x%lld (first at log line %s)", u.first.c_str(), u.second.first, u.second.second.c_str()));
    }
    if (res.failed_opens)
      line("  INFO: " + I(res.failed_opens) + " file open attempts failed on " + I(res.failed_open_paths) +
           " distinct paths (the game probes optional files; not a finding by itself)");
    line("  FINDINGS (worst first):");
    if (finds.empty()) line("    none");
    bool anyProblem = false;
    for (const Finding& f : finds) {
      const std::string ln = f.line ? "log line " + *f.line : "no single line";
      const std::string at = f.at.empty() ? "" : " at " + f.at;
      line("    [" + f.cls + "] " + f.mode + ": " + f.text + "; first reason: " + Cut(f.reason.empty() ? "-" : f.reason, 200) + " (" + ln + at + ")");
      if (f.cls == "problem") anyProblem = true;
    }
    if (anyProblem) line(std::string("    NOTE: ") + kProblemNote);
    total += finds.size();
  }
  line(bar);
  line("TOTAL: " + I((long long)results.size()) + " log(s), " + I((long long)total) + " finding line(s)");
  return out;
}

}  // namespace detail

// The one entry point. `files` = the game's raw log files for the report (wwe2k14_NNN.log and rotated wwe2k14_NNN.K.log parts, any order);
// `source` is the text printed after the log's name. Returns the text for findings.txt; never throws.
inline std::string Build(const std::vector<std::filesystem::path>& files, const std::string& source = "this machine") noexcept {
  try {
    std::map<std::string, std::vector<detail::LogFile>> groups;
    for (const auto& f : files) {
      const std::string n = f.filename().string();
      std::string num;
      std::optional<int> k;
      if (detail::ParsePartName(n, num, k)) groups[num].push_back({n, k, f});
    }
    if (groups.empty()) return "could not read the log: no wwe2k14_NNN.log among the files given\n";
    std::vector<std::pair<detail::Result, std::vector<std::string>>> results;
    std::vector<std::string> sources;
    std::string errors;
    for (auto& g : groups) {
      auto& lst = g.second;
      std::sort(lst.begin(), lst.end(), [](const detail::LogFile& a, const detail::LogFile& b) {
        if (a.k.has_value() != b.k.has_value()) return a.k.has_value();  // a rotated part before the newest file
        return a.k.value_or(0) > b.k.value_or(0);  // highest K first: oldest first; the base file (newest) last
      });
      try {
        std::vector<detail::PartIn> parts;
        for (const auto& p : lst) parts.push_back({p.label, p.path});
        detail::Result r = detail::ReadLog("wwe2k14_" + g.first + ".log", parts);
        for (const auto& p : parts) r.parts.push_back(p.label);
        results.push_back({std::move(r), detail::PartNotes(lst)});
        sources.push_back(source);
      } catch (const std::exception& e) {
        errors += std::string("could not read the log: ") + e.what() + "\n";
      } catch (...) {
        errors += "could not read the log: unknown error\n";
      }
    }
    std::string text = results.empty() ? std::string() : detail::Render(results, sources);
    return errors + text;
  } catch (const std::exception& e) {
    return std::string("could not read the log: ") + e.what() + "\n";
  } catch (...) {
    return "could not read the log: unknown error\n";
  }
}

}  // namespace report_findings
