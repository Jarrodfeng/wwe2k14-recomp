// wwe2k14 - per-frame guest memory hash for determinism tests (Session 8, task 8b). Default off.
//
// --wwe2k14_det_hash=SPEC     (default empty = off: one string-empty check per frame)
//   SPEC is a comma-separated list of items. An item is either the name of a built-in set (below) or a region
//     NAME=ADDR[>OFF...]:LEN[+KOFF[/KLEN]...][~MOFF[/MLEN]...]
//   ADDR is a guest address (hex, with or without 0x). Each '>OFF' (hex with 0x, else decimal) reads the big-endian
//   word at the current address and adds OFF, so "c0=0x836F38D0>0:3840" hashes 3840 bytes starting at the pointer
//   stored at 0x836F38D0. LEN is decimal (or hex with 0x), at most 1 MB.
//   '+KOFF/KLEN' keeps only the listed byte ranges (all other bytes of the region are hashed as 0); '~MOFF/MLEN'
//   zeroes a byte range before hashing (for pointer fields). KLEN / MLEN default to 4. Offsets are decimal or 0x hex.
//   Built-in sets:
//     default   values only, no pointers (the guidance in docs/systems/match_state.md "For task 8b"): RNG states,
//               match clock, globals 0x836F3900..0x836F39AF, match block G+472..G+14891, judge (pointer fields
//               masked), documented value fields of the slot 0/1/6 characters, their health data, and documented
//               value fields of the motion camera.
//     objects   whole objects including their heap pointers (as hashed in runs 244-251): glob 0x836F38D0..+576,
//               characters of slots 0/1/6 (3840 bytes each), motion camera (1024 bytes). They differ between two
//               boots whenever the heap layout differs, whatever the game state.
//     legacy    the exact 17-region set used by runs 244-251 (for reproducing them).
//   "1" / "true" = default. Example: --wwe2k14_det_hash=default,objects (what tools/determinism/run_det.ps1 uses).
// The spec is parsed at the first frame with the setting on (later changes are ignored). Once per guest frame
// (VdSwap, before the swap is forwarded) each region is copied (under __try/__except, so memory that is freed while
// it is read gives "-" instead of a crash), masked, hashed with 64-bit FNV-1a, and one CSV row is
// written. Output: out/harness/determinism/det_NNN.csv for the win-amd64-release exe, det_<exe folder>_NNN.csv for
// any other exe folder (NNN = this run's wwe2k14_NNN.log number; the folder is found from wwe2k14.exe, not the
// working directory). A file of the same name from an older run is renamed to <name>_replaced_<date>_<time> first.
// Line 1 is "# det_hash exe=<path> size=<bytes> mtime=<local time> spec=<spec>" (the build identity that
// tools/determinism/det_compare.py checks), line 2 "frame,<name>...", then one row per frame with 16 hex digits per
// region, "-" when a pointer is null or the memory is not readable.
// --wwe2k14_det_hash_raw_every=N (default 0 = off) also appends the masked bytes of every region every N frames to
// det_NNN.bin (record: "DETR", u64 frame, u32 region count, then per region u32 length + bytes; length 0 when
// unreadable), so a divergence can be traced to byte offsets with tools/determinism/det_compare.py.
//
// Region sources (addresses from db/rng.json, docs/systems/match_state.md, db/match_state.json and
// docs/systems/camera.md; nothing here is guessed):
//   mt1/mt1i  MT #1 state (624 words) 0x83940270 and its index 0x8362D3BC          (rng.md row 1)
//   mt2/mt2i  MT #2 state 0x83945848 and its index 0x8362D428                       (rng.md row 2)
//   cur       random-table cursors A/B 0x839456D4/0x839456D5                        (rng.md row 3)
//   alcg      audio LCG 0x8395C860                                                  (rng.md row 5)
//   lcg2/lcg3 module-0x8269 LCGs 0x836229F8 / 0x83622AC8                            (rng.md row 6)
//   lcg4      module-0x823B LCG 0x835E1984                                          (rng.md row 7)
//   clk       match clock (frames) 0x836F3928                                       (match_state.md key data)
//   gvals     globals 0x836F3900..0x836F39AF (match_state.md "For task 8b"; no pointer-like word there in the
//             368 raw dumps of run 244: the slot table before it and the heap pointers from 0x836F3A50 are outside)
//   gm        G+472..G+14891 = 0x8376ACCC, 14420 bytes (the range match_state_reinit 0x82770108 clears,
//             db/match_state.json)
//   judge     judge object [0x83767B6C], 272 bytes (db/match_state.json), vtables +0/+24/+40, self +64 and the
//             counted-character pointer +140 masked
//   c0v/c1v/refv  characters [0x836F38D0] / [0x836F38D4] / [0x836F38E8] (slot 6 = referee in run 236): only the
//             documented value fields +1020..+1023, +1220, +1400, +1459, +1480, +3720 (match_state.md)
//   c0hp/c1hp health component H of slots 0/1 (chain char+3728 >20 x5 >24, match_state.md; the 5th-child assumption
//             is checked only for wrestler 0): H+52..H+67 (health and maxima) and H+132
//   camv      motion camera [0x836F3ABC]: +144, +292, +300, +304, +332, +336, +416..+495 (matrix, position),
//             +1424..+1431, +1440 (camera.md key data)
//   timing    timing struct 0x8392DC38 (db/ai_decision.json timing_struct): +4 lens-mode selector, +8 frame rate in
//             Hz, +60 tick divisor (60 / 30)
//   glob / c0 / c1 / ref / cam (set "objects"): the same objects in full, pointers included
// With the setting on, the cost on the VdSwap thread is logged every 600 frames ("[wwe2k14][dethash] cost ...").
//
// Read on the render thread at VdSwap without synchronising with the guest threads that write these regions, so a
// region another thread is writing during the swap can differ between runs for timing reasons alone.

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#if defined(_WIN32)  // Session 63 (Android port): Windows / x86 only
#include <share.h>
#endif
#include "platform/compat.h"  // <windows.h> on Windows (Session 63)
#endif

#include "frame_stats.h"

REXCVAR_DEFINE_STRING(wwe2k14_det_hash, "", "wwe2k14",
                      "Debug (determinism tests): hash guest memory regions once per frame and write "
                      "out/harness/determinism/det_NNN.csv. 'default' = built-in pointer-free set (RNG states, match "
                      "globals, characters' value fields, judge, camera), 'objects' / 'legacy' = other built-in sets, "
                      "or NAME=0xADDR[>OFF...]:LEN[+KEEP/LEN...][~MASK/LEN...], comma-separated. Empty = off");
REXCVAR_DEFINE_INT32(wwe2k14_det_hash_raw_every, 0, "wwe2k14",
                     "Debug: with --wwe2k14_det_hash, also dump the (masked) region bytes every N frames to "
                     "det_NNN.bin (0 = off)");

namespace wwe2k14 {

class FrameHash {
 public:
  void OnFrame(uint64_t frame) {
    const std::string& spec = REXCVAR_GET(wwe2k14_det_hash);
    if (spec.empty()) return;
    if (!opened_) Open(spec);
    if (!csv_) return;
    const auto t0 = std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration t_read{};
    std::string row = std::to_string(frame);
    const int32_t raw_every = REXCVAR_GET(wwe2k14_det_hash_raw_every);
    const bool raw = bin_ && raw_every > 0 && (frame % uint64_t(raw_every)) == 0;
    if (raw) {
      const uint32_t count = uint32_t(regions_.size());
      std::fwrite("DETR", 1, 4, bin_);
      std::fwrite(&frame, sizeof(frame), 1, bin_);
      std::fwrite(&count, sizeof(count), 1, bin_);
    }
    char hex[24];
    if (region_us_.size() != regions_.size()) region_us_.assign(regions_.size(), 0.0);
    for (size_t ri = 0; ri < regions_.size(); ++ri) {
      Region& r = regions_[ri];
      const auto tr = std::chrono::steady_clock::now();
      const bool ok = Read(r, frame);
      const auto dr = std::chrono::steady_clock::now() - tr;
      t_read += dr;
      region_us_[ri] += std::chrono::duration<double, std::micro>(dr).count();
      if (ok) {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t i = 0; i < r.length; ++i) {
          h ^= buf_[i];
          h *= 1099511628211ull;
        }
        std::snprintf(hex, sizeof(hex), ",%016llx", static_cast<unsigned long long>(h));
        row += hex;
      } else {
        row += ",-";
      }
      if (raw) {
        const uint32_t len = ok ? r.length : 0;
        std::fwrite(&len, sizeof(len), 1, bin_);
        if (len) std::fwrite(buf_.data(), 1, len, bin_);
      }
    }
    row += '\n';
    std::fwrite(row.data(), 1, row.size(), csv_);
    if ((frame % 60) == 0) {
      std::fflush(csv_);
      if (bin_) std::fflush(bin_);
    }
    // Cost on the VdSwap thread, logged every 600 frames (copying = Read, the rest = hashing and file writes).
    const auto total = std::chrono::steady_clock::now() - t0;
    const double us = std::chrono::duration<double, std::micro>(total).count();
    cost_sum_us_ += us;
    cost_read_us_ += std::chrono::duration<double, std::micro>(t_read).count();
    if (us > cost_max_us_) cost_max_us_ = us;
    if (++cost_frames_ == 600) {
      std::string top;  // the three regions that took longest to copy
      std::vector<size_t> order(regions_.size());
      for (size_t i = 0; i < order.size(); ++i) order[i] = i;
      std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return region_us_[x] > region_us_[y]; });
      for (size_t i = 0; i < order.size() && i < 3; ++i) {
        char part[96];
        std::snprintf(part, sizeof(part), "%s%s %.0f us", i ? ", " : "", regions_[order[i]].name.c_str(),
                      region_us_[order[i]] / 600.0);
        top += part;
      }
      REXLOG_INFO("[wwe2k14][dethash] cost frames {}-{}: {:.0f} us avg per frame ({:.0f} us copying; slowest {}), "
                  "{:.0f} us max", frame - 599, frame, cost_sum_us_ / 600.0, cost_read_us_ / 600.0, top,
                  cost_max_us_);
      cost_frames_ = 0;
      cost_sum_us_ = cost_read_us_ = cost_max_us_ = 0.0;
      region_us_.assign(regions_.size(), 0.0);
    }
  }

 private:
  struct Region {
    std::string name;
    uint32_t address = 0;
    std::vector<uint32_t> offsets;
    uint32_t length = 4;
    std::vector<uint8_t> mask;  // empty = hash every byte; else 0xFF = keep, 0x00 = hashed as 0
    uint64_t careful_until = 0;  // after a faulting copy: VirtualQuery before each copy up to this frame
  };

  // VirtualQuery check: committed, not no-access, not guard. Only used for a region that has faulted before (see
  // Read): the call is slow while the game runs (runs 263-266: ~15 us per call in menus and 0.5-1 ms in the
  // entrances and the match, presumably waiting on the address-space lock that the GPU write-watch protects take;
  // unconfirmed), and ~50 calls per frame made the match run at ~45 fps.
  static bool Readable(const uint8_t* host, uint32_t length) {
#ifdef _WIN32
    const uint8_t* p = host;
    const uint8_t* end = host + length;
    while (p < end) {
      MEMORY_BASIC_INFORMATION mbi{};
      if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
          (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
      }
      p = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    }
#endif
    return true;
  }

  // memcpy that returns false instead of crashing when the source faults (unmapped, uncommitted or freed by a guest
  // thread). The copy is a call through a volatile pointer, so the faulting instruction is inside a callee of the
  // __try body (clang's synchronous SEH catches faults in calls). The SDK's own handler sees the fault first: it is
  // not an MMIO range (those are refused below) and not a write, so it passes it on (and may log "Unhandled guest
  // access violation" once per fault).
  static bool SafeCopy(void* dst, const void* src, size_t n) {
    static void* (*volatile copy)(void*, const void*, size_t) = std::memcpy;
#ifdef _WIN32
    __try {
      copy(dst, src, n);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_GUARD_PAGE
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
      return false;
    }
    return true;
#else
    copy(dst, src, n);
    return true;
#endif
  }

  // Copies guest memory into dst. Refuses 0x7F000000..0x7FFFFFFF (the GPU writeback / MMIO heap in the SDK's heap
  // table, src/system/xmemory.cpp): an MMIO read would be emulated by the SDK's MMIO handler instead of faulting.
  static bool CopyGuest(uint32_t guest, uint32_t length, uint8_t* dst, bool careful) {
    if (guest == 0) return false;
    const uint64_t end = uint64_t(guest) + length;
    if (end > 0x100000000ull) return false;
    if (end > 0x7F000000ull && guest < 0x80000000u) return false;
    auto* host = REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(guest);
    if (!host) return false;
    if (careful && !Readable(host, length)) return false;
    return SafeCopy(dst, host, length);
  }

  // Resolves the region's pointer chain and copies its (masked) bytes into buf_. After a faulting copy the region
  // is checked with VirtualQuery for the next 600 frames (a bad chain faults at most once per 600 frames).
  bool Read(Region& r, uint64_t frame) {
    const bool careful = frame < r.careful_until;
    uint32_t address = r.address;
    for (uint32_t off : r.offsets) {
      if ((address & 3) != 0) return false;
      uint8_t w[4];
      if (!CopyGuest(address, 4, w, careful)) {
        if (!careful) r.careful_until = frame + 600;
        return false;
      }
      const uint32_t value = (uint32_t(w[0]) << 24) | (uint32_t(w[1]) << 16) | (uint32_t(w[2]) << 8) | uint32_t(w[3]);
      if (value == 0) return false;
      address = value + off;
    }
    if (buf_.size() < r.length) buf_.resize(r.length);
    if (!CopyGuest(address, r.length, buf_.data(), careful)) {
      if (!careful) r.careful_until = frame + 600;
      return false;
    }
    if (!r.mask.empty()) {
      for (uint32_t i = 0; i < r.length; ++i) buf_[i] &= r.mask[i];
    }
    return true;
  }

  static bool ParseNumber(const std::string& s, bool hex_default, uint32_t* out) {
    if (s.empty()) return false;
    const bool has_0x = s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &end, (has_0x || hex_default) ? 16 : 10);
    if (!end || *end != '\0' || v > 0xFFFFFFFFull) return false;
    *out = uint32_t(v);
    return true;
  }

  // Values only (docs/systems/match_state.md "For task 8b": hash values, never pointers). Sources in the header.
  static constexpr const char* kDefaultSpec =
      "mt1=0x83940270:2496,mt1i=0x8362D3BC:4,mt2=0x83945848:2496,mt2i=0x8362D428:4,cur=0x839456D4:2,"
      "alcg=0x8395C860:4,lcg2=0x836229F8:4,lcg3=0x83622AC8:4,lcg4=0x835E1984:4,clk=0x836F3928:4,"
      "gvals=0x836F3900:176,gm=0x8376ACCC:14420,judge=0x83767B6C>0:272~0~24~40~64~140,"
      "c0v=0x836F38D0>0:3724+1020+1220+1400/1+1459/1+1480+3720,"
      "c1v=0x836F38D4>0:3724+1020+1220+1400/1+1459/1+1480+3720,"
      "refv=0x836F38E8>0:3724+1020+1220+1400/1+1459/1+1480+3720,"
      "c0hp=0x836F38D0>3728>20>20>20>20>20>24>0:136+52/16+132,"
      "c1hp=0x836F38D4>3728>20>20>20>20>20>24>0:136+52/16+132,"
      "camv=0x836F3ABC>0:1444+144+292+300+304+332+336+416/80+1424/8+1440,"
      "timing=0x8392DC38:64+4+8+60";
  // Whole objects, pointers included.
  static constexpr const char* kObjectsSpec =
      "glob=0x836F38D0:576,c0=0x836F38D0>0:3840,c1=0x836F38D4>0:3840,ref=0x836F38E8>0:3840,cam=0x836F3ABC>0:1024";
  // The set used by runs 244-251 (before the review).
  static constexpr const char* kLegacySpec =
      "mt1=0x83940270:2496,mt1i=0x8362D3BC:4,mt2=0x83945848:2496,mt2i=0x8362D428:4,cur=0x839456D4:2,"
      "alcg=0x8395C860:4,lcg2=0x836229F8:4,lcg3=0x83622AC8:4,lcg4=0x835E1984:4,clk=0x836F3928:4,"
      "glob=0x836F38D0:576,gm=0x8376AAF4:14600,judge=0x83767B6C>0:512,c0=0x836F38D0>0:3840,"
      "c1=0x836F38D4>0:3840,ref=0x836F38E8>0:3840,cam=0x836F3ABC>0:1024";

  static const char* NamedSet(const std::string& item) {
    if (item == "default" || item == "1" || item == "true") return kDefaultSpec;
    if (item == "objects") return kObjectsSpec;
    if (item == "legacy") return kLegacySpec;
    return nullptr;
  }

  // "LEN[+KOFF[/KLEN]...][~MOFF[/MLEN]...]" -> length and mask.
  static bool ParseLengthAndMask(const std::string& tail, Region* r) {
    size_t pos = tail.find_first_of("+~");
    if (!ParseNumber(tail.substr(0, pos), false, &r->length) || r->length == 0 || r->length > (1u << 20)) return false;
    std::vector<std::pair<char, std::pair<uint32_t, uint32_t>>> ranges;
    bool any_keep = false;
    while (pos != std::string::npos) {
      const char kind = tail[pos];
      const size_t next = tail.find_first_of("+~", pos + 1);
      const std::string tok = tail.substr(pos + 1, next == std::string::npos ? std::string::npos : next - pos - 1);
      const size_t slash = tok.find('/');
      uint32_t off = 0, len = 4;
      if (!ParseNumber(tok.substr(0, slash), false, &off)) return false;
      if (slash != std::string::npos && !ParseNumber(tok.substr(slash + 1), false, &len)) return false;
      if (len == 0 || uint64_t(off) + len > r->length) return false;
      ranges.push_back({kind, {off, len}});
      any_keep = any_keep || kind == '+';
      pos = next;
    }
    if (ranges.empty()) return true;
    r->mask.assign(r->length, any_keep ? 0x00 : 0xFF);
    for (const auto& k : ranges) {  // keeps first, then masks win
      if (k.first == '+') std::memset(r->mask.data() + k.second.first, 0xFF, k.second.second);
    }
    for (const auto& k : ranges) {
      if (k.first == '~') std::memset(r->mask.data() + k.second.first, 0x00, k.second.second);
    }
    return true;
  }

  void Parse(const std::string& spec, int depth) {
    std::stringstream items(spec);
    std::string item;
    while (std::getline(items, item, ',')) {
      if (item.empty()) continue;
      if (const char* set = NamedSet(item)) {
        if (depth == 0) Parse(set, depth + 1);
        continue;
      }
      const size_t eq = item.find('=');
      const size_t colon = item.rfind(':');
      Region r;
      bool good = eq != std::string::npos && eq > 0 && colon != std::string::npos && colon > eq;
      if (good) {
        r.name = item.substr(0, eq);
        good = ParseLengthAndMask(item.substr(colon + 1), &r);
        std::stringstream parts(item.substr(eq + 1, colon - eq - 1));
        std::string part;
        bool first = true;
        while (good && std::getline(parts, part, '>')) {
          uint32_t v = 0;
          good = ParseNumber(part, first, &v);
          if (first) {
            r.address = v;
            first = false;
          } else {
            r.offsets.push_back(v);
          }
        }
        good = good && !first;
      }
      if (!good) {
        REXLOG_WARN("[wwe2k14][dethash] ignoring '{}' (want NAME=ADDR[>OFF...]:LEN[+KEEP/LEN...][~MASK/LEN...])",
                    item);
        continue;
      }
      bool duplicate = false;
      for (const Region& other : regions_) duplicate = duplicate || other.name == r.name;
      if (duplicate) {
        REXLOG_WARN("[wwe2k14][dethash] ignoring '{}': region name {} already used", item, r.name);
        continue;
      }
      size_t kept = r.length;
      if (!r.mask.empty()) {
        kept = 0;
        for (uint8_t m : r.mask) kept += m ? 1 : 0;
      }
      REXLOG_INFO("[wwe2k14][dethash] region {} = 0x{:08X} with {} dereference(s), {} bytes ({} hashed as is)",
                  r.name, r.address, r.offsets.size(), r.length, kept);
      regions_.push_back(std::move(r));
    }
  }

  static std::filesystem::path ExePath() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return std::filesystem::path(buf);
#endif
    return std::filesystem::current_path() / "wwe2k14.exe";
  }

  // "exe=<path> size=<bytes> mtime=<local time>" of the running exe.
  static std::string ExeIdentity(const std::filesystem::path& exe) {
    std::string out = "exe=" + exe.string();
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(exe.wstring().c_str(), GetFileExInfoStandard, &fad)) {
      const unsigned long long size = (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
      FILETIME local{};
      SYSTEMTIME st{};
      char when[48] = "?";
      if (FileTimeToLocalFileTime(&fad.ftLastWriteTime, &local) && FileTimeToSystemTime(&local, &st)) {
        std::snprintf(when, sizeof(when), "%04u-%02u-%02uT%02u:%02u:%02u.%03u", st.wYear, st.wMonth, st.wDay,
                      st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
      }
      out += " size=" + std::to_string(size) + " mtime=" + when;
    }
#endif
    return out;
  }

  // An existing file of the same name belongs to an older run (log numbers restarted): keep it, renamed.
  static bool MoveAside(const std::filesystem::path& path, const std::string& stamp) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return true;
    std::filesystem::path old = path;
    old.replace_filename(path.stem().string() + stamp + path.extension().string());
    std::filesystem::rename(path, old, ec);
    if (ec) {
      REXLOG_ERROR("[wwe2k14][dethash] {} is from an older run and cannot be renamed ({}); hashing off", path.string(),
                   ec.message());
      return false;
    }
    REXLOG_WARN("[wwe2k14][dethash] {} was from an older run with the same log number: renamed to {}", path.string(),
                old.string());
    return true;
  }

  void Open(const std::string& spec) {
    opened_ = true;
    Parse(spec, 0);
    if (regions_.empty()) return;
    const int number = CurrentLogNumber();
    const std::filesystem::path exe = ExePath();
    const std::string folder = exe.parent_path().filename().string();
    std::error_code ec;
    const std::filesystem::path dir =
        std::filesystem::weakly_canonical(exe.parent_path() / ".." / ".." / "harness" / "determinism", ec);
    std::filesystem::create_directories(dir, ec);
    char num[16];
    std::snprintf(num, sizeof(num), "%03d", number < 0 ? 0 : number);
    const std::string stem = folder == "win-amd64-release" ? "det_" + std::string(num)
                                                           : "det_" + folder + "_" + std::string(num);
    const std::filesystem::path csv_path = dir / (stem + ".csv");
    const std::filesystem::path bin_path = dir / (stem + ".bin");
    const std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "_replaced_%Y%m%d_%H%M%S", std::localtime(&now));
    if (!MoveAside(csv_path, stamp) || !MoveAside(bin_path, stamp)) return;
#ifdef _WIN32
    csv_ = _fsopen(csv_path.string().c_str(), "w", _SH_DENYNO);
#else
    csv_ = std::fopen(csv_path.string().c_str(), "w");
#endif
    if (!csv_) {
      REXLOG_WARN("[wwe2k14][dethash] cannot write {}", csv_path.string());
      return;
    }
    std::string header = "# det_hash " + ExeIdentity(exe) + " spec=" + spec + "\nframe";
    for (const Region& r : regions_) header += "," + r.name;
    header += '\n';
    std::fwrite(header.data(), 1, header.size(), csv_);
    if (REXCVAR_GET(wwe2k14_det_hash_raw_every) > 0) {
#ifdef _WIN32
      bin_ = _fsopen(bin_path.string().c_str(), "wb", _SH_DENYNO);
#else
      bin_ = std::fopen(bin_path.string().c_str(), "wb");
#endif
    }
    REXLOG_INFO("[wwe2k14][dethash] writing {} ({} regions, raw every {} frames)", csv_path.string(), regions_.size(),
                REXCVAR_GET(wwe2k14_det_hash_raw_every));
  }

  bool opened_ = false;
  std::FILE* csv_ = nullptr;
  std::FILE* bin_ = nullptr;
  std::vector<Region> regions_;
  std::vector<uint8_t> buf_;
  int cost_frames_ = 0;
  double cost_sum_us_ = 0.0, cost_read_us_ = 0.0, cost_max_us_ = 0.0;
  std::vector<double> region_us_;
};

inline FrameHash g_frame_hash;

// Called from VdSwapOverride (render thread, once per guest frame).
inline void FrameHashOnFrame(uint64_t frame) {
  if (REXCVAR_GET(wwe2k14_det_hash).empty()) return;
  g_frame_hash.OnFrame(frame);
}

}  // namespace wwe2k14
