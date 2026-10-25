// wwe2k14 - named guest-memory states for input scripts (Session 8 task 9a).
//
// Used by the input-script commands wait_state / assert_state / log_state (src/input_script.h). Nothing here runs
// unless a script (--wwe2k14_input_script, default off) uses one of those commands.
//
// Definitions live in a data file, tools/input_scripts/states.toml. The script loader reads it the first time a
// script line names a state: states.toml in the script's own folder if there is one, else the first
// tools/input_scripts/states.toml found walking up from the script's folder (so routes kept elsewhere in the repo,
// e.g. tools/review/routes or out/harness/..., find it too). Format (a small TOML subset, one entry per state):
//   [NAME]
//   chain = "0x839400F8>0>48"  # guest address (hex), then each >OFF (decimal, or 0x hex) treats the value read so
//                              # far as a guest pointer and reads at value + OFF (same syntax as --wwe2k14_peek)
//   size = 4                   # bytes of the LAST read: 1, 2 or 4 (big-endian); optional, default 4. Pointer
//                              # reads are always 4-byte words. Unquoted.
//   signed = false             # compare as a signed number (matters for < > <= >=); optional, default false.
//                              # Unquoted true/false.
//   meaning / evidence / confidence / source = "..."   documentation only (one-line strings, no escapes)
// '#' starts a comment outside quotes. No arrays, no multi-line values. A leading UTF-8 BOM is skipped. Numbers
// are plain digits (no sign, no spaces). A null pointer anywhere in the chain, an unaligned address or an address
// the host can't read makes the state "unreadable" (it never matches). In a script, a NAME that starts with 0x is
// an inline chain (size 4, unsigned), so any verified address can be used without a states.toml entry.
//
// Reads happen on the render thread without synchronisation with the guest threads that write the words. Each read
// is one aligned load of the whole value (so it can't mix bytes from before and after a guest store), but a value
// the game only holds for a moment can still be seen, so wait_state can require the value to hold for a number of
// consecutive frames (default 2).

#pragma once

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "guest_read.h"  // Session 15: --wwe2k14_fast_guest_reads

#include <bit>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "platform/compat.h"  // <windows.h> on Windows (Session 63)
#endif

namespace wwe2k14 {

struct GuestState {
  std::string name;
  std::string chain;  // as written, for logs
  uint32_t address = 0;
  std::vector<uint32_t> offsets;
  uint32_t size = 4;
  bool is_signed = false;
  std::string meaning;
};

// A comparison from a script: VALUE, or !=VALUE / >VALUE / <VALUE / >=VALUE / <=VALUE (also "!VALUE" for !=).
// VALUE is decimal (may be negative) or 0x hex.
struct StateCompare {
  enum class Op { kEq, kNe, kLt, kLe, kGt, kGe };
  Op op = Op::kEq;
  int64_t value = 0;
  std::string text;  // as written, for logs
};

namespace game_state_detail {

// Plain digits only: decimal, or 0x hex. No sign, no spaces (strtoull alone would accept " +5" or "-0x1").
inline bool ParseUnsigned(const std::string& s, uint32_t* out) {
  if (s.empty() || !std::isdigit(static_cast<unsigned char>(s[0]))) return false;
  const bool hex = s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
  if (hex && !std::isxdigit(static_cast<unsigned char>(s[2]))) return false;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(s.c_str(), &end, hex ? 16 : 10);
  if (!end || *end != '\0' || v > 0xFFFFFFFFull) return false;
  *out = uint32_t(v);
  return true;
}

inline std::string Trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

// Strips a '#' comment that is outside double quotes.
inline std::string StripComment(const std::string& line) {
  bool quoted = false;
  for (size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '"') quoted = !quoted;
    else if (line[i] == '#' && !quoted) return line.substr(0, i);
  }
  return line;
}

}  // namespace game_state_detail

// "ADDR[>OFF...]": ADDR hex (with or without 0x), offsets decimal or 0x hex.
inline bool ParseStateChain(const std::string& text, GuestState& state) {
  using namespace game_state_detail;
  state.chain = text;
  state.offsets.clear();
  size_t start = 0;
  bool first = true;
  while (true) {
    const size_t gt = text.find('>', start);
    const std::string part = text.substr(start, gt == std::string::npos ? std::string::npos : gt - start);
    uint32_t v = 0;
    if (first) {
      const std::string hex =
          (part.size() > 2 && part[0] == '0' && (part[1] == 'x' || part[1] == 'X')) ? part : "0x" + part;
      if (!ParseUnsigned(hex, &v) || v == 0) return false;
      state.address = v;
      first = false;
    } else {
      if (!ParseUnsigned(part, &v)) return false;
      state.offsets.push_back(v);
    }
    if (gt == std::string::npos) break;
    start = gt + 1;
  }
  return true;
}

// Reads SIZE (1, 2, 4) big-endian bytes at a guest address. False for null, unaligned or unreadable addresses.
inline bool ReadGuestBytes(uint32_t guest, uint32_t size, uint32_t* out) {
  if (guest == 0 || (guest % size) != 0) return false;
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) return false;
  auto* host = memory->TranslateVirtual<const uint8_t*>(guest);
  if (!host) return false;
  // Session 15: the guest page table check (guest_read.h). An aligned read of 1-4 bytes never crosses a page.
  if (REXCVAR_GET(wwe2k14_fast_guest_reads) && !REXCVAR_GET(wwe2k14_fast_guest_reads_verify)) {
    if (!GuestWordReadable(guest, host)) return false;
  } else {
#ifdef _WIN32
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(host, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
      (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
    return false;
  }
  // The read must not run past the end of the committed region.
  if (reinterpret_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize < host + size) return false;
#endif
    if (REXCVAR_GET(wwe2k14_fast_guest_reads)) GuestWordReadable(guest, host);  // verify mode: compare and log only
  }
  // One aligned load of the whole value (single-copy atomic on x86-64), then big-endian to host order. Byte-by-byte
  // loads could mix bytes from before and after a guest store on another thread (e.g. 0x000000FF -> 0x00000100
  // read as 0x000001FF). The guest address is aligned (checked above); the host mapping keeps that alignment, but
  // check anyway rather than assume it.
  if (reinterpret_cast<uintptr_t>(host) % size != 0) return false;
  uint32_t v = 0;
  if (size == 4) {
    v = std::byteswap(*reinterpret_cast<const volatile uint32_t*>(host));
  } else if (size == 2) {
    v = std::byteswap(*reinterpret_cast<const volatile uint16_t*>(host));
  } else {
    v = *reinterpret_cast<const volatile uint8_t*>(host);
  }
  *out = v;
  return true;
}

// Follows the chain; the final read uses state.size. The raw (zero-extended) value goes to *out. A pointer read as 0
// anywhere in the chain makes the state unreadable (without this, 0 + a large offset would read low guest memory).
inline bool ReadGuestState(const GuestState& state, uint32_t* out) {
  const bool chained = !state.offsets.empty();
  uint32_t v = 0;
  if (!ReadGuestBytes(state.address, chained ? 4 : state.size, &v)) return false;
  for (size_t i = 0; i < state.offsets.size(); ++i) {
    if (v == 0) return false;  // null pointer
    const bool last = i + 1 == state.offsets.size();
    if (!ReadGuestBytes(v + state.offsets[i], last ? state.size : 4, &v)) return false;
  }
  *out = v;
  return true;
}

// The raw value as a number for comparisons: sign-extended from its size when the state is signed.
inline int64_t StateNumber(const GuestState& state, uint32_t raw) {
  if (!state.is_signed) return int64_t(raw);
  if (state.size == 1) return int64_t(int8_t(raw));
  if (state.size == 2) return int64_t(int16_t(raw));
  return int64_t(int32_t(raw));
}

inline bool ParseStateCompare(const std::string& text, StateCompare& out) {
  out = {};
  out.text = text;
  size_t skip = 0;
  if (text.rfind("!=", 0) == 0) out.op = StateCompare::Op::kNe, skip = 2;
  else if (text.rfind(">=", 0) == 0) out.op = StateCompare::Op::kGe, skip = 2;
  else if (text.rfind("<=", 0) == 0) out.op = StateCompare::Op::kLe, skip = 2;
  else if (text.rfind("!", 0) == 0) out.op = StateCompare::Op::kNe, skip = 1;
  else if (text.rfind(">", 0) == 0) out.op = StateCompare::Op::kGt, skip = 1;
  else if (text.rfind("<", 0) == 0) out.op = StateCompare::Op::kLt, skip = 1;
  else if (text.rfind("=", 0) == 0) out.op = StateCompare::Op::kEq, skip = 1;
  std::string number = text.substr(skip);
  if (number.empty()) return false;
  const bool negative = number[0] == '-';
  if (negative) number = number.substr(1);
  uint32_t v = 0;
  if (!game_state_detail::ParseUnsigned(number, &v)) return false;
  if (negative && v > 0x80000000u) return false;  // below INT32_MIN: no 32-bit value can mean it
  out.value = negative ? -int64_t(v) : int64_t(v);
  return true;
}

// For == and != the literal is compared on the state's size (so -1 matches 0xFFFFFFFF on an unsigned word).
inline bool StateMatches(const GuestState& state, uint32_t raw, const StateCompare& c) {
  const uint32_t mask = state.size == 4 ? 0xFFFFFFFFu : ((1u << (state.size * 8)) - 1);
  const bool equal = (raw & mask) == (uint32_t(uint64_t(c.value)) & mask);
  const int64_t n = StateNumber(state, raw);
  switch (c.op) {
    case StateCompare::Op::kEq: return equal;
    case StateCompare::Op::kNe: return !equal;
    case StateCompare::Op::kLt: return n < c.value;
    case StateCompare::Op::kLe: return n <= c.value;
    case StateCompare::Op::kGt: return n > c.value;
    case StateCompare::Op::kGe: return n >= c.value;
  }
  return false;
}

// Loads definitions from a states.toml file into `out` (appending). False, with `error` set, on a bad file.
inline bool LoadGuestStates(const std::string& path, std::vector<GuestState>& out, std::string& error) {
  using namespace game_state_detail;
  std::ifstream in(path);
  if (!in) {
    error = "can't open " + path;
    return false;
  }
  GuestState* current = nullptr;
  bool has_chain = false;
  int line_number = 0;
  auto finish_entry = [&]() -> bool {
    if (current && !has_chain) {
      error = path + ": state '" + current->name + "' has no chain";
      return false;
    }
    return true;
  };
  for (std::string raw; std::getline(in, raw);) {
    ++line_number;
    if (line_number == 1 && raw.rfind("\xEF\xBB\xBF", 0) == 0) raw.erase(0, 3);  // UTF-8 BOM (some editors add it)
    const std::string line = Trim(StripComment(raw));
    if (line.empty()) continue;
    const std::string where = path + " line " + std::to_string(line_number);
    if (line.front() == '[') {
      if (line.back() != ']' || line.size() < 3) {
        error = where + ": bad section header";
        return false;
      }
      if (!finish_entry()) return false;
      const std::string name = Trim(line.substr(1, line.size() - 2));
      for (const GuestState& s : out) {
        if (s.name == name) {
          error = where + ": state '" + name + "' defined twice";
          return false;
        }
      }
      out.push_back({});
      current = &out.back();
      current->name = name;
      has_chain = false;
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos || !current) {
      error = where + (current ? ": want key = value" : ": key outside a [STATE] section");
      return false;
    }
    const std::string key = Trim(line.substr(0, eq));
    std::string value = Trim(line.substr(eq + 1));
    if (!value.empty() && (value.front() == '[' || value.front() == '{')) {
      error = where + ": arrays and tables are not supported (one-line key = value only)";
      return false;
    }
    const bool quoted = value.size() >= 2 && value.front() == '"' && value.back() == '"';
    if (quoted) value = value.substr(1, value.size() - 2);
    if ((key == "size" || key == "signed") && quoted) {
      error = where + ": " + key + " must not be quoted";
      return false;
    }
    if (key == "chain") {
      if (!ParseStateChain(value, *current)) {
        error = where + ": bad chain '" + value + "'";
        return false;
      }
      has_chain = true;
    } else if (key == "size") {
      uint32_t size = 0;
      if (!ParseUnsigned(value, &size) || (size != 1 && size != 2 && size != 4)) {
        error = where + ": size must be 1, 2 or 4";
        return false;
      }
      current->size = size;
    } else if (key == "signed") {
      if (value != "true" && value != "false") {
        error = where + ": signed must be true or false";
        return false;
      }
      current->is_signed = value == "true";
    } else if (key == "meaning") {
      current->meaning = value;
    }
    // Other keys (evidence, confidence, source, ...) are documentation only.
  }
  return finish_entry();
}

// The states file for a script: states.toml in the script's own folder, else the first
// tools/input_scripts/states.toml walking up from that folder (at most 12 levels). Empty if there is none.
inline std::string FindStatesFile(const std::string& script_path) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path dir = fs::absolute(fs::path(script_path), ec).parent_path();
  if (ec) dir = fs::path(script_path).parent_path();
  const fs::path beside = dir / "states.toml";
  if (fs::is_regular_file(beside, ec)) return beside.string();
  for (int level = 0; level < 12 && !dir.empty(); ++level) {
    const fs::path candidate = dir / "tools" / "input_scripts" / "states.toml";
    if (fs::is_regular_file(candidate, ec)) return candidate.lexically_normal().string();
    const fs::path parent = dir.parent_path();
    if (parent == dir) break;
    dir = parent;
  }
  return {};
}

}  // namespace wwe2k14
