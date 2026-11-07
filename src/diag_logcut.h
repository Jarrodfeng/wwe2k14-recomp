// wwe2k14 - Session 95 (diag4): shrinks a game log for the diagnostic zip (both launchers; pure standard C++).
//
// The first full PC run's logs were 12.5 MB each (72k lines, 99.8% of them one line repeated every draw: "[native][seg] frame N draw
// textures: ..."). Rule: a line's SHAPE is the line without its leading "[timestamp]" and with digits (and 0x hex runs) turned into '#'.
// A shape is kept in full for its first kShapeKeep lines and then only counted; the counts go at the end as one summary block.
// Always kept whatever the shape count (up to kPriorityKeep per shape, so a spammed error cannot fill the zip): error / warning /
// critical lines, "[diag" lines, "diag-stats", "storage-window" (diag_read.py's "over the window" rule reads them), "HOIST",
// failures, and the first kHead and last kTail lines (the start: GPU, driver, flags; the end: how it died).
// Nothing a diag_read.py rule uses is lost: its log rules read only "[diag-stats" + storage-window / HOIST lines,
// "[storage-window]" lines with "over the window N", and "[vinput]" lines (vinput_table sums every window).
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace wwe2k14::diag_logcut {

constexpr size_t kHead = 400, kTail = 400, kShapeKeep = 6, kPriorityKeep = 300, kSummaryShapes = 40;

inline bool Has(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

inline bool IsPriority(const std::string& line) {
  return Has(line, "[error]") || Has(line, "[err") || Has(line, "[warning]") || Has(line, "[warn]") || Has(line, "[critical]") ||
         Has(line, "[diag") || Has(line, "storage-window") || Has(line, "[vinput]") || Has(line, "HOIST") || Has(line, "ailed") || Has(line, "xception") ||
         Has(line, "VK_ERROR") || Has(line, "validation");
}

// The line without the first "[...]" group, digits as '#', 0x runs as "0x#".
inline std::string Shape(const std::string& line) {
  size_t i = 0;
  if (!line.empty() && line[0] == '[') {
    const size_t close = line.find(']');
    if (close != std::string::npos) i = close + 1;
  }
  std::string out;
  out.reserve(line.size() - i);
  for (; i < line.size(); ++i) {
    const char c = line[i];
    if (c == '0' && i + 1 < line.size() && (line[i + 1] == 'x' || line[i + 1] == 'X')) {
      out += "0x#";
      i += 2;
      while (i < line.size() && std::isxdigit(static_cast<unsigned char>(line[i]))) ++i;
      --i;
    } else if (c >= '0' && c <= '9') {
      if (out.empty() || out.back() != '#') out += '#';
    } else {
      out += c;
    }
  }
  return out;
}

// `text` is the whole log. Returns the cut log, at most about `limit` bytes: past the limit the ordinary lines of the middle go
// first, then its priority lines; the head and tail stay.
inline std::string Cut(const std::string& text, size_t limit = size_t(3) << 20) {
  std::vector<std::pair<size_t, size_t>> lines;  // [begin, end) without the newline
  for (size_t at = 0; at < text.size();) {
    size_t nl = text.find('\n', at);
    if (nl == std::string::npos) nl = text.size();
    lines.emplace_back(at, nl);
    at = nl + 1;
  }
  struct Count {
    uint64_t seen = 0, priority = 0;
    std::string sample;
  };
  std::unordered_map<std::string, Count> shapes;
  std::vector<uint8_t> keep(lines.size(), 0);  // 0 folded, 1 kept (ordinary), 2 kept (priority / head / tail)
  size_t total = 0;
  for (size_t n = 0; n < lines.size(); ++n) {
    const std::string line = text.substr(lines[n].first, lines[n].second - lines[n].first);
    const bool edge = n < kHead || n + kTail >= lines.size();
    Count& c = shapes[Shape(line)];
    if (c.sample.empty()) c.sample = line.substr(0, 200);
    ++c.seen;
    if (edge) {
      keep[n] = 2;
    } else if (IsPriority(line)) {
      if (c.priority++ < kPriorityKeep) keep[n] = 2;
    } else if (c.seen <= kShapeKeep) {
      keep[n] = 1;
    }
    if (keep[n]) total += line.size() + 1;
  }
  const size_t summary_reserve = 16 * 1024;
  for (int level = 1; level <= 2 && total + summary_reserve > limit; ++level) {
    for (size_t n = kHead; n + kTail < lines.size() && total + summary_reserve > limit; ++n) {
      if (keep[n] != level) continue;
      keep[n] = 0;
      total -= lines[n].second - lines[n].first + 1;
    }
  }
  std::string out;
  out.reserve(total + summary_reserve);
  uint64_t folded = 0;
  for (size_t n = 0; n < lines.size(); ++n) {
    if (!keep[n]) {
      ++folded;
      continue;
    }
    out.append(text, lines[n].first, lines[n].second - lines[n].first);
    out += '\n';
  }
  if (folded) {
    std::vector<std::pair<uint64_t, const Count*>> top;
    for (const auto& kv : shapes)
      if (kv.second.seen > kShapeKeep) top.emplace_back(kv.second.seen, &kv.second);
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    out += "[diag-logcut] " + std::to_string(folded) + " of " + std::to_string(lines.size()) +
           " lines folded (the same line shape repeated; the first " + std::to_string(kShapeKeep) + " of each kept). Most repeated shapes (count, sample):\n";
    for (size_t i = 0; i < top.size() && i < kSummaryShapes; ++i)
      out += "[diag-logcut]   " + std::to_string(top[i].first) + "x  " + top[i].second->sample + "\n";
  }
  return out;
}

// ---- which files are this arm's own: wwe2k14_NNN.log / wwe2k14_NNN.K.log / frames_NNN.csv carry the run's number NNN (a counter,
// one game process per arm). The mtime window alone also took the arm before (the next launch touches the older log): an arm
// takes only the NEWEST number above every number already collected in the run folder.
inline long NumberOf(const std::string& name) {
  size_t p = 0;
  if (name.rfind("wwe2k14_", 0) == 0) p = 8;
  else if (name.rfind("frames_", 0) == 0) p = 7;
  else if (name.rfind("mem_", 0) == 0) p = 4;
  else return -1;
  if (p >= name.size() || !std::isdigit(static_cast<unsigned char>(name[p]))) return -1;
  long v = 0;
  for (; p < name.size() && std::isdigit(static_cast<unsigned char>(name[p])); ++p) v = v * 10 + (name[p] - '0');
  return v;
}

}  // namespace wwe2k14::diag_logcut
