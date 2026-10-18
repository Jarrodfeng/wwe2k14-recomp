// wwe2k14 - live memory scanner, Cheat Engine style (Session 29, test harness; nothing runs unless a script asks).
//
// Input-script commands (src/input_script.h; also over the live channel, src/live_control.h), run on the game thread
// (a scan pauses the game for a moment, so a value cannot change halfway through a scan):
//   scan_start [1|2|4]        copy every writable guest page (the 4 KB heap 0x00000000, the 64 KB heap 0x40000000,
//                             the XEX image + 0x90000000 heap 0x80000000-0x9FFFFFFF, physical memory once through its
//                             0xA0000000 view; code and read-only data are skipped) and make every aligned unit of that
//                             size (default 4 bytes, big-endian) a candidate
//   scan_eq V / scan_ne V / scan_gt V / scan_lt V      keep candidates whose value now compares so with V
//   scan_changed / scan_unchanged / scan_inc [N] / scan_dec [N]
//                             keep candidates that changed / did not change / went up / down (by exactly N if given)
//                             since the previous scan step (the step then stores the values now)
//   scan_range LO HI          keep candidates at LO <= address < HI (0x82000000 0x90000000 = static data: same address
//                             in every run, no pointer chain needed)
//   scan_show [N]             log up to N candidates (default 20): address, value now, value at the previous step
//   scan_end                  free the copies
// Every step logs "[wwe2k14][scan] STEP: A -> B candidates (T s)". V and N: decimal (may be negative) or 0x hex.
// A candidate address works directly as a state: `log_state 0xADDR`, `wait_state 0xADDR 3` (states.toml for a name).
// Next steps for a found variable: `watch 0xADDR` (which function writes it, with call stacks), then
// --wwe2k14_trace on that function (coverage build). A heap address can move between runs: find a static pointer to
// it with tools/menus/pointer_scan.py on a snapshot, as in Session 27.
// Memory: the copies take as much host memory as the game's writable guest memory (a few hundred MB); harness only.

#pragma once

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace wwe2k14::scan {

struct Chunk {
  uint32_t addr = 0;              // guest address of the chunk (one heap page)
  uint32_t size = 0;              // bytes
  std::vector<uint8_t> prev;      // the bytes at the previous step
  std::vector<uint64_t> alive;    // one bit per unit
  uint32_t count = 0;             // alive units
};

struct Scanner {
  bool active = false;
  uint32_t unit = 4;
  std::vector<Chunk> chunks;
  uint64_t total = 0;
};

inline Scanner& Sc() {
  static Scanner s;
  return s;
}

inline uint8_t* Base() { return REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(0); }

// Big-endian unit at p (unit 1 / 2 / 4) as an unsigned value.
inline uint32_t Load(const uint8_t* p, uint32_t unit) {
  if (unit == 1) return p[0];
  if (unit == 2) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return std::byteswap(v);
  }
  uint32_t v;
  std::memcpy(&v, p, 4);
  return std::byteswap(v);
}

inline bool ParseValue(const std::string& text, int64_t& out) {
  if (text.empty()) return false;
  const bool neg = text[0] == '-';
  std::string_view t(text);
  if (neg) t.remove_prefix(1);
  int base = 10;
  if (t.size() > 2 && (t.substr(0, 2) == "0x" || t.substr(0, 2) == "0X")) {
    base = 16;
    t.remove_prefix(2);
  }
  uint64_t v = 0;
  auto [ptr, ec] = std::from_chars(t.data(), t.data() + t.size(), v, base);
  if (ec != std::errc() || ptr != t.data() + t.size()) return false;
  out = neg ? -int64_t(v) : int64_t(v);
  return true;
}

// Committed, readable and writable memory of the scanned ranges, in chunks of at most 64 KB. Two checks, because a
// page the guest page table calls read/write can still be only reserved (run 2200 faulted at 0x40010000 when this
// asked QueryProtect alone): the heap's region info (state = committed) and the host view (VirtualQuery: committed,
// not no-access / guard).
template <typename F>
inline void ForEachWritablePage(F&& f) {
  static const std::pair<uint64_t, uint64_t> kRanges[] = {
      {0x00000000ull, 0x40000000ull}, {0x40000000ull, 0x7F000000ull}, {0x80000000ull, 0xA0000000ull},
      {0xA0000000ull, 0xC0000000ull}};
  auto* memory = REX_KERNEL_MEMORY();
  uint8_t* base = Base();
  for (const auto& [lo, hi] : kRanges) {
    uint64_t a = lo;
    while (a < hi) {
      auto* heap = memory->LookupHeap(uint32_t(a));
      if (!heap) {
        a += 0x10000;
        continue;
      }
      rex::memory::HeapAllocationInfo info{};
      if (!heap->QueryRegionInfo(uint32_t(a), &info) || !info.region_size) {
        a += std::max<uint32_t>(heap->page_size(), 0x1000);
        continue;
      }
      const uint64_t region_end = std::min<uint64_t>(hi, uint64_t(info.base_address) + info.region_size);
      const bool usable = (info.state & rex::memory::kMemoryAllocationCommit) &&
                          (info.protect & rex::memory::kMemoryProtectRead) &&
                          (info.protect & rex::memory::kMemoryProtectWrite);
      for (uint64_t p = std::max<uint64_t>(a, info.base_address); usable && p < region_end;) {
#ifdef _WIN32
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(base + p, &mbi, sizeof(mbi))) break;
        const uint64_t host_end = std::min<uint64_t>(
            region_end, p + (uint64_t(reinterpret_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize - (base + p))));
        const bool host_ok = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
#else
        const uint64_t host_end = region_end;
        const bool host_ok = true;
#endif
        for (; host_ok && p < host_end;) {
          const uint32_t n = uint32_t(std::min<uint64_t>(0x10000 - (p & 0xFFFF), host_end - p));
          f(uint32_t(p), n);
          p += n;
        }
        p = std::max(p, host_end);
      }
      a = std::max<uint64_t>(region_end, a + 0x1000);
    }
  }
}

// Is the chunk still committed and readable (guest region info and host view)? Pages can be freed between steps.
inline bool ChunkReadable(uint32_t addr, uint32_t size) {
  auto* heap = REX_KERNEL_MEMORY()->LookupHeap(addr);
  rex::memory::HeapAllocationInfo info{};
  if (!heap || !heap->QueryRegionInfo(addr, &info)) return false;
  if (!(info.state & rex::memory::kMemoryAllocationCommit) || !(info.protect & rex::memory::kMemoryProtectRead)) {
    return false;
  }
  if (uint64_t(addr) + size > uint64_t(info.base_address) + info.region_size) return false;
#ifdef _WIN32
  MEMORY_BASIC_INFORMATION mbi{};
  uint8_t* host = Base() + addr;
  if (!VirtualQuery(host, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
    return false;
  }
  if (host + size > reinterpret_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize) return false;
#endif
  return true;
}

inline std::string Region(uint32_t a) {
  if (a < 0x40000000u) return "heap4k";
  if (a < 0x80000000u) return "heap64k";
  if (a < 0x90000000u) return "xex";
  if (a < 0xA0000000u) return "heap90";
  return "phys";
}

inline void Clear() {
  Scanner& s = Sc();
  s.chunks.clear();
  s.chunks.shrink_to_fit();
  s.total = 0;
  s.active = false;
}

inline bool Start(uint32_t unit, std::string& error) {
  Clear();
  Scanner& s = Sc();
  s.unit = unit;
  uint8_t* base = Base();
  ForEachWritablePage([&](uint32_t addr, uint32_t size) {
    Chunk c;
    c.addr = addr;
    c.size = size;
    c.prev.assign(base + addr, base + addr + size);
    const uint32_t units = size / unit;
    c.alive.assign((units + 63) / 64, ~0ull);
    if (units % 64) c.alive.back() = (1ull << (units % 64)) - 1;
    c.count = units;
    s.total += units;
    s.chunks.push_back(std::move(c));
  });
  if (s.chunks.empty()) {
    error = "no writable guest pages found";
    return false;
  }
  s.active = true;
  return true;
}

enum class Test { kEq, kNe, kGt, kLt, kChanged, kUnchanged, kInc, kDec };

// One filter step; drops chunks that become empty or unreadable.
inline void Filter(Test test, int64_t value, bool has_value) {
  Scanner& s = Sc();
  uint8_t* base = Base();
  const uint32_t unit = s.unit;
  const uint32_t mask = unit == 4 ? 0xFFFFFFFFu : unit == 2 ? 0xFFFFu : 0xFFu;
  const uint32_t v = uint32_t(value) & mask;
  uint64_t total = 0;
  std::vector<Chunk> kept;
  kept.reserve(s.chunks.size());
  for (Chunk& c : s.chunks) {
    if (!ChunkReadable(c.addr, c.size)) continue;  // freed since the last step: its candidates are gone
    const uint8_t* now = base + c.addr;
    uint32_t count = 0;
    for (size_t w = 0; w < c.alive.size(); ++w) {
      uint64_t bits = c.alive[w];
      while (bits) {
        const int b = std::countr_zero(bits);
        bits &= bits - 1;
        const size_t i = w * 64 + size_t(b);
        const uint32_t cur = Load(now + i * unit, unit), old = Load(c.prev.data() + i * unit, unit);
        bool keep = false;
        switch (test) {
          case Test::kEq: keep = cur == v; break;
          case Test::kNe: keep = cur != v; break;
          case Test::kGt: keep = cur > v; break;
          case Test::kLt: keep = cur < v; break;
          case Test::kChanged: keep = cur != old; break;
          case Test::kUnchanged: keep = cur == old; break;
          case Test::kInc: keep = has_value ? ((cur - old) & mask) == v : cur > old; break;
          case Test::kDec: keep = has_value ? ((old - cur) & mask) == v : cur < old; break;
        }
        if (keep) {
          ++count;
        } else {
          c.alive[w] &= ~(1ull << b);
        }
      }
    }
    if (!count) continue;
    std::memcpy(c.prev.data(), now, c.size);  // the values now, for the next relative step
    c.count = count;
    total += count;
    kept.push_back(std::move(c));
  }
  s.chunks.swap(kept);
  s.total = total;
}

// Keep candidates with lo <= address < hi (e.g. the XEX image's static data, 0x82000000 0x90000000: the same address in
// every run, usable in a route without a pointer chain).
inline void Range(uint32_t lo, uint64_t hi) {
  Scanner& s = Sc();
  uint64_t total = 0;
  std::vector<Chunk> kept;
  for (Chunk& c : s.chunks) {
    uint32_t count = 0;
    for (size_t w = 0; w < c.alive.size(); ++w) {
      uint64_t bits = c.alive[w];
      while (bits) {
        const int b = std::countr_zero(bits);
        bits &= bits - 1;
        const uint64_t addr = uint64_t(c.addr) + (w * 64 + size_t(b)) * s.unit;
        if (addr >= lo && addr < hi) {
          ++count;
        } else {
          c.alive[w] &= ~(1ull << b);
        }
      }
    }
    if (!count) continue;
    c.count = count;
    total += count;
    kept.push_back(std::move(c));
  }
  s.chunks.swap(kept);
  s.total = total;
}

inline std::vector<std::string> Show(size_t max_lines) {
  Scanner& s = Sc();
  uint8_t* base = Base();
  std::vector<std::string> out;
  for (const Chunk& c : s.chunks) {
    if (out.size() >= max_lines) break;
    if (!ChunkReadable(c.addr, c.size)) continue;
    for (size_t w = 0; w < c.alive.size() && out.size() < max_lines; ++w) {
      uint64_t bits = c.alive[w];
      while (bits && out.size() < max_lines) {
        const int b = std::countr_zero(bits);
        bits &= bits - 1;
        const size_t i = w * 64 + size_t(b);
        const uint32_t addr = c.addr + uint32_t(i * s.unit);
        out.push_back(fmt::format("0x{:08X} = {} (prev {}) [{}]", addr, Load(base + addr, s.unit),
                                  Load(c.prev.data() + i * s.unit, s.unit), Region(addr)));
      }
    }
  }
  return out;
}

inline bool IsScanCommand(const std::string& cmd) { return cmd.rfind("scan_", 0) == 0; }

// Parse-time check of a scan line (cmd, the rest of the line).
inline bool Check(const std::string& cmd, const std::string& args, std::string& why) {
  std::istringstream words(args);
  std::string a, extra;
  words >> a;
  const bool more = bool(words >> extra);
  int64_t n = 0;
  if (cmd == "scan_start") {
    if (a.empty()) return true;
    if (a != "1" && a != "2" && a != "4") return why = "scan_start [1|2|4]", false;
  } else if (cmd == "scan_eq" || cmd == "scan_ne" || cmd == "scan_gt" || cmd == "scan_lt") {
    if (!ParseValue(a, n)) return why = cmd + " VALUE", false;
  } else if (cmd == "scan_inc" || cmd == "scan_dec") {
    if (!a.empty() && !ParseValue(a, n)) return why = cmd + " [N]", false;
  } else if (cmd == "scan_show") {
    if (!a.empty() && (!ParseValue(a, n) || n <= 0)) return why = "scan_show [N]", false;
  } else if (cmd == "scan_range") {
    int64_t hi = 0;
    if (!ParseValue(a, n) || !ParseValue(extra, hi) || n < 0 || hi <= n) return why = "scan_range LO HI", false;
    std::string more_words;
    if (words >> more_words) return why = "unexpected '" + more_words + "'", false;
    return true;
  } else if (cmd == "scan_changed" || cmd == "scan_unchanged" || cmd == "scan_end") {
    if (!a.empty()) return why = cmd + " takes no value", false;
  } else {
    return why = "unknown scan command (scan_start, scan_eq/ne/gt/lt, scan_changed/unchanged, scan_inc/dec, scan_range, "
                 "scan_show, scan_end)",
           false;
  }
  if (more) return why = "unexpected '" + extra + "'", false;
  return true;
}

// Runs a scan line (game thread). False with `error` when it cannot run (no scan started).
inline bool Run(const std::string& cmd, const std::string& args, std::string& error) {
  using clock = std::chrono::steady_clock;
  const auto t0 = clock::now();
  Scanner& s = Sc();
  std::istringstream words(args);
  std::string a;
  words >> a;
  int64_t value = 0;
  const bool has_value = !a.empty() && ParseValue(a, value);
  if (cmd == "scan_start") {
    const uint32_t unit = a.empty() ? 4 : uint32_t(std::stoul(a));
    if (!Start(unit, error)) return false;
    size_t bytes = 0;
    for (const Chunk& c : s.chunks) bytes += c.size;
    REXLOG_INFO("[wwe2k14][scan] start ({}-byte units): {} candidates in {} writable pages ({} MB) ({:.2f} s)", unit,
                s.total, s.chunks.size(), bytes >> 20,
                std::chrono::duration<double>(clock::now() - t0).count());
    return true;
  }
  if (cmd == "scan_end") {
    Clear();
    REXLOG_INFO("[wwe2k14][scan] end: copies freed");
    return true;
  }
  if (!s.active) {
    error = "no scan started (scan_start first)";
    return false;
  }
  if (cmd == "scan_range") {
    std::string b;
    words >> b;
    int64_t hi = 0;
    ParseValue(b, hi);
    const uint64_t before = s.total;
    Range(uint32_t(value), uint64_t(hi));
    REXLOG_INFO("[wwe2k14][scan] range {} {}: {} -> {} candidates ({:.2f} s)", a, b, before, s.total,
                std::chrono::duration<double>(clock::now() - t0).count());
    return true;
  }
  if (cmd == "scan_show") {
    const size_t n = has_value ? size_t(value) : 20;
    const auto lines = Show(n);
    REXLOG_INFO("[wwe2k14][scan] show: {} candidates{}", s.total,
                s.total > lines.size() ? fmt::format(", first {}", lines.size()) : std::string());
    for (const std::string& line : lines) REXLOG_INFO("[wwe2k14][scan]   {}", line);
    return true;
  }
  static const std::pair<const char*, Test> kTests[] = {
      {"scan_eq", Test::kEq},          {"scan_ne", Test::kNe},   {"scan_gt", Test::kGt},
      {"scan_lt", Test::kLt},          {"scan_changed", Test::kChanged},
      {"scan_unchanged", Test::kUnchanged}, {"scan_inc", Test::kInc}, {"scan_dec", Test::kDec}};
  for (const auto& [name, test] : kTests) {
    if (cmd != name) continue;
    const uint64_t before = s.total;
    Filter(test, value, has_value);
    REXLOG_INFO("[wwe2k14][scan] {}{}: {} -> {} candidates ({:.2f} s)", cmd.substr(5), a.empty() ? "" : " " + a,
                before, s.total, std::chrono::duration<double>(clock::now() - t0).count());
    if (s.total == 0) REXLOG_WARN("[wwe2k14][scan] no candidates left (scan_start to begin again)");
    return true;
  }
  error = "unknown scan command";
  return false;
}

}  // namespace wwe2k14::scan
