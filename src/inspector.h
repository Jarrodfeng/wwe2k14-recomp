// wwe2k14 - ImGui live inspector (Session 8 task 15). Debug tool, default OFF.
//
// --wwe2k14_inspector=true            install it: registers the hotkey (F6, setting bind_wwe2k14_inspector) and a
//                                     change callback on wwe2k14_inspector_show. Off (the default): nothing is
//                                     registered or created, OnCreateDialogs returns at the first line.
// --wwe2k14_inspector_show=true       show the window at boot. Also settable in-run (an input script's
//                                     `cvar wwe2k14_inspector_show true|false`, or the settings panel): true shows it,
//                                     false hides it. F6 toggles it either way (F6 does not change the setting).
// --wwe2k14_inspector_addr=CHAIN      the memory viewer's start address: 0xADDR or a chain 0xADDR>OFF>OFF... (same
//                                     syntax as --wwe2k14_peek / states.toml; the view starts at the address the
//                                     chain's last read would read). Empty = nothing shown until one is typed.
// Hidden = the dialog is destroyed, so the ImGui drawer has nothing extra to draw (no per-frame cost); the panel
// state (address, filters) is kept for the next show.
//
// Window "wwe2k14 memory": guest memory viewer. Address box (address or chain, re-resolved on every refresh), N rows
// of 16 bytes as big-endian words, the same words as floats, ASCII, and labels. Words that changed in the last
// ~0.5 s are highlighted; clicking a word that holds a readable guest address follows it (Back returns). Reads are
// from the UI thread without synchronisation with the guest (each word is one aligned 4-byte load); pages the host
// can't read show as ????????.
//   Labels: loaded on first show from the repo the exe sits in (walks up from the exe folder to the folder holding
//   db/ and docs/systems/): db/*.json entries (every 0x8xxxxxxx token in "address": single addresses, A..B ranges;
//   "A -> +OFF" and the bracket form "[[A]+o1]+o2" / "[[A]+o]" become chains, never labels on A), docs/systems/*.md
//   tables (only tables with an "Address" / "Where" column: its data addresses inside the XEX image, labelled with
//   the row's name/label column or first text column; heuristic, marked "docs table", and dropped where a db entry
//   names the address) and --wwe2k14_peek specs in those docs (NAME=0xADDR>OFF..., become named chains), and
//   tools/input_scripts/states.toml (named chains, read with their size). Tabs "Globals" (data labels; tick
//   "functions" to include code addresses) and "Chains" (named chains with live values). Nothing in those files is
//   game code: names, addresses and our summaries only.
//   --wwe2k14_inspector_dump_labels=PATH writes all labels and chains (with chain values read then) as TSV when
//   they load: a self-test of the label sources for unattended runs.
// Window "wwe2k14 settings": toggles for every wwe2k14 setting plus a list of ReXGlue settings this app sets
//   (OnPreSetup), or every setting ("all"). Applied with rex::cvar::SetFlagByName (as the input script's `cvar`
//   command does) and logged. Init-only settings are read-only. The "when" column: "restart" as declared; our own
//   settings declare no lifecycle, so they show "boot only" (checked in the code to be read once at startup, see
//   BootOnlyWhere) or "not declared" (may still be read only at startup); SDK ones say what the SDK declares.
//   String settings are replaced without a lock (rex::cvar has none for strings): a thread reading one at that
//   moment (wwe2k14_peek every frame, wwe2k14_file_trace_filter per file open) could read a freed buffer. Unlikely;
//   the input script's `cvar` command has the same race. A real fix belongs in the cvar layer.
// Window "wwe2k14 coverage" (coverage build only, preset win-amd64-coverage, with --wwe2k14_coverage=true): the
//   current coverage window's counts and its newest first-called functions (src/coverage.h keeps "first call per
//   window", not call counts), with db names, and a lookup "did 0x... run in this window / this run".
// Each window logs one "[wwe2k14][inspector] panel ... drawn" line the first time it is drawn.
//
// Game input is not paused while the inspector is open (ReXGlue's input gate only knows its own overlays); with
// the default mnk_mode=false the keyboard doesn't drive the game, so typing in the boxes is safe.

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

#include <imgui.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "frame_stats.h"
#include "game_state.h"
#if defined(WWE2K14_COVERAGE)
#include "coverage.h"
#endif

REXCVAR_DEFINE_BOOL(wwe2k14_inspector, false, "wwe2k14",
                    "Debug: ImGui live inspector (memory viewer, settings, coverage); F6 shows/hides it");
REXCVAR_DEFINE_BOOL(wwe2k14_inspector_show, false, "wwe2k14",
                    "Inspector (needs wwe2k14_inspector): show it at boot; set in-run: true shows, false hides");
REXCVAR_DEFINE_STRING(wwe2k14_inspector_addr, "", "wwe2k14",
                      "Inspector: memory viewer start address or chain 0xADDR[>OFF...] (empty = none)");
REXCVAR_DEFINE_STRING(wwe2k14_inspector_test_set, "", "wwe2k14",
                      "Inspector self-test: NAME=VALUE applied once through the settings panel's apply path when the "
                      "panel first draws, then read back and logged (empty = off)");
REXCVAR_DEFINE_STRING(wwe2k14_inspector_dump_labels, "", "wwe2k14",
                      "Inspector self-test: write every label and named chain (with the chain's value read then) to "
                      "this TSV file when the labels load (relative = to the working folder; empty = off)");

namespace wwe2k14::inspector {

// ---------------------------------------------------------------------------------------------------------------
// Guest reads (UI thread). One VirtualQuery per host page, then one aligned 4-byte load per word.
// ---------------------------------------------------------------------------------------------------------------

inline rex::memory::Memory* GuestMemory() {
  auto* ks = rex::system::kernel_state();
  return ks ? ks->memory() : nullptr;
}

struct PageCheck {
  uintptr_t page = ~uintptr_t(0);
  bool ok = false;
};

inline bool HostReadable(const void* host, PageCheck& cache) {
#ifdef _WIN32
  const uintptr_t page = reinterpret_cast<uintptr_t>(host) & ~uintptr_t(0xFFF);
  if (page == cache.page) return cache.ok;
  cache.page = page;
  MEMORY_BASIC_INFORMATION mbi{};
  cache.ok = VirtualQuery(reinterpret_cast<const void*>(page), &mbi, sizeof(mbi)) != 0 && mbi.State == MEM_COMMIT &&
             (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
  return cache.ok;
#else
  (void)host;
  (void)cache;
  return true;
#endif
}

inline uint32_t LoadBE32(const volatile uint32_t* p) { return std::byteswap(*p); }

// One aligned 4-byte load that returns false instead of crashing if the page goes away between the VirtualQuery and
// the load (a guest thread freeing memory). Called through a volatile pointer so the faulting instruction is inside
// a callee of the __try body (as SafeCopy in src/frame_hash.h).
inline bool SafeLoad(const void* host, uint32_t* out) {
  static uint32_t (*volatile load)(const volatile uint32_t*) = LoadBE32;
#ifdef _WIN32
  __try {
    *out = load(static_cast<const volatile uint32_t*>(host));
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_GUARD_PAGE
                  ? EXCEPTION_EXECUTE_HANDLER
                  : EXCEPTION_CONTINUE_SEARCH) {
    return false;
  }
  return true;
#else
  *out = load(static_cast<const volatile uint32_t*>(host));
  return true;
#endif
}

inline bool ReadWord(uint32_t guest, uint32_t* out, PageCheck& cache) {
  if (guest == 0 || (guest & 3) != 0) return false;
  // 0x7F000000..0x7FFFFFFF is the GPU writeback / MMIO heap (src/frame_hash.h CopyGuest): never read it.
  if (guest >= 0x7F000000u && guest < 0x80000000u) return false;
  auto* memory = GuestMemory();
  if (!memory) return false;
  auto* host = memory->TranslateVirtual<const uint8_t*>(guest);
  if (!host || (reinterpret_cast<uintptr_t>(host) & 3) != 0 || !HostReadable(host, cache)) return false;
  return SafeLoad(host, out);
}

// The address a chain's last read would read ("0x836F3AA8>1392" -> [0x836F3AA8] + 1392). False on a null pointer
// or an unreadable link.
inline bool ResolveChain(const GuestState& st, uint32_t* out) {
  PageCheck cache;
  uint32_t addr = st.address;
  for (uint32_t off : st.offsets) {
    uint32_t p = 0;
    if (!ReadWord(addr, &p, cache) || p == 0) return false;
    addr = p + off;
  }
  *out = addr;
  return true;
}

// The chain's value with its states.toml size (1, 2, 4; same alignment rule as game_state.h ReadGuestBytes),
// taken from the aligned big-endian word that holds it (one fault-safe load). Raw, zero-extended; *at = its address.
inline bool ReadChainValue(const GuestState& st, uint32_t* out, uint32_t* at = nullptr) {
  uint32_t addr = 0, w = 0;
  const uint32_t size = (st.size == 1 || st.size == 2) ? st.size : 4;
  if (!ResolveChain(st, &addr) || addr % size != 0) return false;
  PageCheck cache;
  if (!ReadWord(addr & ~3u, &w, cache)) return false;
  const uint32_t shift = (4 - size - (addr & 3)) * 8;
  *out = size == 4 ? w : (w >> shift) & ((1u << (size * 8)) - 1);
  if (at) *at = addr;
  return true;
}

// For display: "0x.. (n)". n: a word as int32 (as before); a byte or halfword sign-extended only when the state
// says signed (game_state.h StateNumber).
inline std::string ChainValueText(const GuestState& st, uint32_t raw) {
  char b[48];
  const uint32_t size = (st.size == 1 || st.size == 2) ? st.size : 4;
  std::snprintf(b, sizeof(b), "0x%0*X (%lld)", int(size * 2), raw,
                (long long)(size == 4 ? int64_t(int32_t(raw)) : StateNumber(st, raw)));
  return b;
}

// ---------------------------------------------------------------------------------------------------------------
// Labels: db/*.json, docs/systems/*.md, tools/input_scripts/states.toml.
// ---------------------------------------------------------------------------------------------------------------

struct Label {
  uint32_t addr = 0;
  uint32_t end = 0;  // exclusive
  bool is_data = true;
  bool from_db = true;
  std::string name, system, confidence, source, summary;
};

struct NamedChain {
  std::string name, chain, system, source, meaning;
  GuestState st;
};

namespace json {
// Minimal JSON reader: enough for db/*.json (arrays of objects with string fields).
struct Value {
  enum class T { kNull, kBool, kNum, kStr, kArr, kObj } t = T::kNull;
  std::string s;
  double n = 0;
  bool b = false;
  std::vector<Value> a;
  std::vector<std::pair<std::string, Value>> o;
  const Value* Get(std::string_view key) const {
    for (const auto& kv : o) {
      if (kv.first == key) return &kv.second;
    }
    return nullptr;
  }
  std::string Str(std::string_view key) const {
    const Value* v = Get(key);
    return v && v->t == T::kStr ? v->s : std::string();
  }
};

struct Parser {
  const std::string& text;
  size_t i = 0;
  int depth = 0;
  void Ws() {
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) ++i;
  }
  static void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
      out += char(cp);
    } else if (cp < 0x800) {
      out += char(0xC0 | (cp >> 6));
      out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += char(0xE0 | (cp >> 12));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    } else {
      out += char(0xF0 | (cp >> 18));
      out += char(0x80 | ((cp >> 12) & 0x3F));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    }
  }
  bool Hex4(uint32_t* cp) {
    if (i + 4 > text.size()) return false;
    uint32_t v = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = text[i++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
      else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
      else return false;
    }
    *cp = v;
    return true;
  }
  bool Str(std::string& out) {
    if (i >= text.size() || text[i] != '"') return false;
    ++i;
    while (i < text.size()) {
      const char c = text[i++];
      if (c == '"') return true;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i >= text.size()) return false;
      const char e = text[i++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          uint32_t cp = 0;
          if (!Hex4(&cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < text.size() && text[i] == '\\' && text[i + 1] == 'u') {
            i += 2;
            uint32_t lo = 0;
            if (!Hex4(&lo)) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          AppendUtf8(out, cp);
          break;
        }
        default: return false;
      }
    }
    return false;
  }
  bool Val(Value& v) {
    if (++depth > 64) return false;
    Ws();
    if (i >= text.size()) return false;
    const char c = text[i];
    bool ok = false;
    if (c == '{') {
      v.t = Value::T::kObj;
      ++i;
      Ws();
      if (i < text.size() && text[i] == '}') {
        ++i;
        ok = true;
      } else {
        while (true) {
          Ws();
          std::string key;
          if (!Str(key)) break;
          Ws();
          if (i >= text.size() || text[i] != ':') break;
          ++i;
          v.o.emplace_back(std::move(key), Value{});
          if (!Val(v.o.back().second)) break;
          Ws();
          if (i < text.size() && text[i] == ',') {
            ++i;
            continue;
          }
          if (i < text.size() && text[i] == '}') {
            ++i;
            ok = true;
          }
          break;
        }
      }
    } else if (c == '[') {
      v.t = Value::T::kArr;
      ++i;
      Ws();
      if (i < text.size() && text[i] == ']') {
        ++i;
        ok = true;
      } else {
        while (true) {
          v.a.emplace_back();
          if (!Val(v.a.back())) break;
          Ws();
          if (i < text.size() && text[i] == ',') {
            ++i;
            continue;
          }
          if (i < text.size() && text[i] == ']') {
            ++i;
            ok = true;
          }
          break;
        }
      }
    } else if (c == '"') {
      v.t = Value::T::kStr;
      ok = Str(v.s);
    } else if (text.compare(i, 4, "true") == 0) {
      v.t = Value::T::kBool;
      v.b = true;
      i += 4;
      ok = true;
    } else if (text.compare(i, 5, "false") == 0) {
      v.t = Value::T::kBool;
      i += 5;
      ok = true;
    } else if (text.compare(i, 4, "null") == 0) {
      i += 4;
      ok = true;
    } else {
      const size_t start = i;
      while (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) || text[i] == '-' ||
                                 text[i] == '+' || text[i] == '.' || text[i] == 'e' || text[i] == 'E')) {
        ++i;
      }
      if (i > start) {
        v.t = Value::T::kNum;
        v.n = std::strtod(text.substr(start, i - start).c_str(), nullptr);
        ok = true;
      }
    }
    --depth;
    return ok;
  }
};

inline bool Parse(const std::string& text, Value& out) {
  Parser p{text};
  if (text.rfind("\xEF\xBB\xBF", 0) == 0) p.i = 3;
  if (!p.Val(out)) return false;
  p.Ws();
  return p.i == text.size();
}
}  // namespace json

inline bool IsHex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

// "0x" + exactly 8 hex digits at text[pos] (not part of a longer hex run): the value, else nullopt.
inline bool HexToken(std::string_view text, size_t pos, uint32_t* out) {
  if (pos + 10 > text.size() || text[pos] != '0' || (text[pos + 1] != 'x' && text[pos + 1] != 'X')) return false;
  if (pos > 0 && (std::isalnum(static_cast<unsigned char>(text[pos - 1])) || text[pos - 1] == '_')) return false;
  for (size_t k = pos + 2; k < pos + 10; ++k) {
    if (!IsHex(text[k])) return false;
  }
  if (pos + 10 < text.size() && IsHex(text[pos + 10])) return false;
  *out = uint32_t(std::strtoul(std::string(text.substr(pos + 2, 8)).c_str(), nullptr, 16));
  return true;
}

inline bool ReadFile(const std::filesystem::path& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

inline std::string TrimCell(std::string s) {
  std::string out;
  for (char c : s) {
    if (c != '`' && c != '*') out += c;
  }
  const size_t a = out.find_first_not_of(" \t");
  if (a == std::string::npos) return {};
  const size_t b = out.find_last_not_of(" \t");
  out = out.substr(a, b - a + 1);
  if (out.size() > 60) out = out.substr(0, 57) + "...";
  return out;
}

struct Labels {
  bool loaded = false;
  std::filesystem::path root;
  std::vector<Label> labels;  // sorted by addr
  std::vector<NamedChain> chains;
  int db_files = 0, db_entries = 0, db_chains = 0, db_unparsed = 0, doc_files = 0, doc_labels = 0, doc_chains = 0,
      state_chains = 0;
  std::vector<std::string> errors;
  double load_ms = 0;
  uint32_t code_base = 0, code_size = 0, image_base = 0, image_size = 0;

  bool IsCode(uint32_t a) const { return a >= code_base && a - code_base < code_size; }
  // Inside the loaded XEX image (PPCImageConfig image_base/image_size, from the generated config) but not its code.
  bool IsImageData(uint32_t a) const { return a >= image_base && a - image_base < image_size && !IsCode(a); }

  // `st` (optional) keeps a states.toml entry's size and signedness; otherwise the chain is read as a 4-byte word.
  void AddChain(std::string name, std::string chain, std::string system, std::string source, std::string meaning,
                const GuestState* st = nullptr) {
    for (const auto& c : chains) {
      if (c.name == name && c.chain == chain) return;
    }
    NamedChain nc{std::move(name), std::move(chain), std::move(system), std::move(source), std::move(meaning), {}};
    if (st) {
      nc.st = *st;
    } else if (!ParseStateChain(nc.chain, nc.st)) {
      return;
    }
    chains.push_back(std::move(nc));
  }

  // "+OFF" at t[k] (k just past the '+'): decimal or 0x hex. Advances k; false when there is no number.
  static bool ParseOffset(std::string_view t, size_t& k, uint32_t* out) {
    size_t j = k;
    const bool hex = t.compare(j, 2, "0x") == 0 || t.compare(j, 2, "0X") == 0;
    if (hex) j += 2;
    const size_t digits = j;
    while (j < t.size() && (hex ? IsHex(t[j]) : std::isdigit(static_cast<unsigned char>(t[j])) != 0)) ++j;
    if (j == digits || j - digits > 8) return false;
    *out = uint32_t(std::strtoul(std::string(t.substr(digits, j - digits)).c_str(), nullptr, hex ? 16 : 10));
    k = j;
    return true;
  }

  // Bracket notation in a db "address" field (db/menu_ui.json): "[[A]+o1]+o2" and "[[A]+o]", where "[X]" reads the
  // word at X. Converted to a chain A>o1>... (same meaning as states.toml / --wwe2k14_peek): each "+OFF" after a
  // "]" is one pointer step ">OFF", a "]" right after another "]" is a step of 0, a "+OFF" before any "]" moves A
  // itself, and a "]" that ends the expression is the read of the value. "[[A]+0]+48" -> A>0>48 and "[[A]+364]" ->
  // A>364, the chains states.toml has as seq and menu (both seen live, run 239). `opens` = the '[' before A; `k`
  // starts just past A's token and ends past the expression. False when the brackets don't balance.
  static bool ParseBracketChain(std::string_view t, size_t opens, uint32_t a, size_t& k, std::string* chain) {
    std::vector<uint32_t> steps;
    bool deref = false;
    size_t closes = 0;
    while (k < t.size()) {
      if (t[k] == ' ') {
        ++k;
      } else if (t[k] == ']') {
        if (deref) steps.push_back(0);
        deref = true;
        ++closes;
        ++k;
      } else if (t[k] == '+') {
        size_t j = k + 1;
        while (j < t.size() && t[j] == ' ') ++j;
        uint32_t off = 0;
        if (!ParseOffset(t, j, &off)) break;
        k = j;
        if (deref) {
          steps.push_back(off);
          deref = false;
        } else if (steps.empty()) {
          a += off;
        } else {
          steps.back() += off;
        }
      } else {
        break;
      }
    }
    if (closes != opens) return false;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", a);
    *chain = buf;
    for (uint32_t s : steps) *chain += ">" + std::to_string(s);
    return true;
  }

  // Every 0x8xxxxxxx token in a db "address" field.
  void AddDbEntry(const json::Value& e, const std::string& file_stem) {
    const std::string field = e.Str("address");
    const std::string name = e.Str("name");
    if (name.empty()) return;
    std::string_view t = field;
    for (size_t pos = 0; pos < t.size(); ++pos) {
      uint32_t a = 0;
      if (!HexToken(t, pos, &a) || a < 0x80000000u) continue;
      size_t after = pos + 10;
      // "[[A]+o1]+o2": a chain (the entry names a field reached through A), never a label for A itself. A bracket
      // expression that doesn't parse gives nothing rather than a label on the wrong address.
      size_t opens = 0;
      while (opens < pos && t[pos - 1 - opens] == '[') ++opens;
      if (opens > 0) {
        size_t k = after;
        std::string chain;
        if (ParseBracketChain(t, opens, a, k, &chain)) {
          AddChain(name, chain, e.Str("system"), "db/" + file_stem + ".json", e.Str("summary"));
          ++db_chains;
        } else {
          ++db_unparsed;
        }
        pos = k - 1;
        continue;
      }
      // "A..B": a range.
      uint32_t end = a + 4;
      uint32_t b = 0;
      if (t.compare(after, 2, "..") == 0 && HexToken(t, after + 2, &b) && b >= a) {
        end = b + 4;
        after += 12;
      }
      // "A -> +OFF": a chain, not a label for A itself.
      size_t k = after;
      while (k < t.size() && t[k] == ' ') ++k;
      if (t.compare(k, 2, "->") == 0) {
        k += 2;
        while (k < t.size() && (t[k] == ' ' || t[k] == '+')) ++k;
        size_t j = k;
        if (t.compare(j, 2, "0x") == 0) j += 2;
        while (j < t.size() && IsHex(t[j])) ++j;
        if (j > k) {
          char buf[16];
          std::snprintf(buf, sizeof(buf), "0x%08X", a);
          AddChain(name, std::string(buf) + ">" + std::string(t.substr(k, j - k)), e.Str("system"),
                   "db/" + file_stem + ".json", e.Str("summary"));
          ++db_chains;
        }
        pos = j;
        continue;
      }
      Label l;
      l.addr = a;
      l.end = end;
      l.is_data = !IsCode(a);
      l.from_db = true;
      l.name = name;
      // "0xADDR (+OFF)": the entry is about a field reached from ADDR (inline or through a pointer: the text
      // doesn't say), so mark the label as the base, not the field itself.
      if (t.compare(k, 2, "(+") == 0) l.name += " [base]";
      l.system = e.Str("system");
      l.confidence = e.Str("confidence");
      l.source = "db/" + file_stem + ".json";
      l.summary = e.Str("summary");
      labels.push_back(std::move(l));
      pos = after - 1;
    }
  }

  // A table label must be a name, not a row number or a function name.
  static bool UsableDocLabel(const std::string& s) {
    if (s.empty()) return false;
    if (std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
      return false;
    }
    if (s.size() == 12 && s.rfind("sub_", 0) == 0 && std::all_of(s.begin() + 4, s.end(), IsHex)) return false;
    return true;
  }

  static std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
  }

  void LoadDoc(const std::string& text, const std::string& stem) {
    std::istringstream in(text);
    // Table header of the table being read (the row above its |---| line), lower-cased and trimmed; empty outside
    // a table.
    std::vector<std::string> header, prev_cells;
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      std::string_view t = line;
      // --wwe2k14_peek specs: NAME=0xADDR[>OFF...]
      for (size_t pos = 0; pos < t.size(); ++pos) {
        uint32_t a = 0;
        if (t[pos] != '=' || !HexToken(t, pos + 1, &a)) continue;
        size_t s = pos;
        while (s > 0 && (std::isalnum(static_cast<unsigned char>(t[s - 1])) || t[s - 1] == '_')) --s;
        if (s == pos) continue;
        size_t j = pos + 11;
        while (j < t.size() && (t[j] == '>' || t[j] == 'x' || t[j] == 'X' || IsHex(t[j]))) ++j;
        std::string chain(t.substr(pos + 1, j - pos - 1));
        while (!chain.empty() && chain.back() == '>') chain.pop_back();
        AddChain(std::string(t.substr(s, pos - s)), chain, stem, "docs/systems/" + stem + ".md (peek spec)", "");
        ++doc_chains;
        pos = j - 1;
      }
      // Table rows. Only tables with a header column named like "Address" (else "Where"): the data addresses in
      // that column (inside the XEX image, not code: no packet headers, masks or heap values), labelled with the
      // row's "name"/"label" column, else its first other text column (never a "#" column). Labels that are only
      // digits or a sub_XXXXXXXX function name are dropped.
      if (line.size() < 3 || line[0] != '|') {
        header.clear();
        prev_cells.clear();
        continue;
      }
      std::vector<std::string> cells;
      std::string cell;
      for (size_t k = 1; k < line.size(); ++k) {
        if (line[k] == '|') {
          cells.push_back(cell);
          cell.clear();
        } else {
          cell += line[k];
        }
      }
      if (line.find_first_not_of("|-: \t") == std::string::npos) {  // the |---|---| line under a header
        header.clear();
        for (const auto& h : prev_cells) header.push_back(Lower(TrimCell(h)));
        continue;
      }
      prev_cells = cells;
      if (header.empty()) continue;
      int addr_cell = -1;
      for (const char* want : {"address", "where"}) {
        for (size_t c = 0; c < header.size() && c < cells.size() && addr_cell < 0; ++c) {
          if (header[c].find(want) != std::string::npos) addr_cell = int(c);
        }
        if (addr_cell >= 0) break;
      }
      if (addr_cell < 0) continue;
      int label_cell = -1;
      for (size_t c = 0; c < header.size() && c < cells.size() && label_cell < 0; ++c) {
        if (int(c) != addr_cell &&
            (header[c].find("name") != std::string::npos || header[c].find("label") != std::string::npos)) {
          label_cell = int(c);
        }
      }
      for (size_t c = 0; c < header.size() && c < cells.size() && label_cell < 0; ++c) {
        if (int(c) != addr_cell && !header[c].empty() && header[c] != "#") label_cell = int(c);
      }
      if (label_cell < 0) continue;
      const std::string text_label = TrimCell(cells[size_t(label_cell)]);
      if (!UsableDocLabel(text_label)) continue;
      std::string_view ac = cells[size_t(addr_cell)];
      for (size_t pos = 0; pos < ac.size(); ++pos) {
        uint32_t a = 0;
        if (!HexToken(ac, pos, &a) || !IsImageData(a)) continue;
        size_t after = pos + 10;
        uint32_t end = a + 4, b = 0;
        if (ac.compare(after, 2, "..") == 0 && HexToken(ac, after + 2, &b) && b >= a) {
          end = b + 4;
          after += 12;
        }
        // Skip a token that is the base of a chain or a field (0xADDR>OFF, 0xADDR -> +OFF, [[0xADDR]+OFF],
        // 0xADDR+OFF): that row describes something reached from ADDR, not ADDR.
        size_t k = after;
        while (k < ac.size() && ac[k] == ' ') ++k;
        if (k < ac.size() && (ac[k] == '>' || ac[k] == '+' || ac[k] == ']' || ac.compare(k, 2, "->") == 0)) continue;
        if (pos > 0 && ac[pos - 1] == '[') continue;
        Label l;
        l.addr = a;
        l.end = end;
        l.is_data = true;
        l.from_db = false;
        l.name = text_label;
        l.system = stem;
        l.confidence = "docs table";
        l.source = "docs/systems/" + stem + ".md";
        labels.push_back(std::move(l));
        ++doc_labels;
        pos = after - 1;
      }
    }
  }

  void Load(uint32_t cb, uint32_t cs, uint32_t ib, uint32_t is) {
    if (loaded) return;
    loaded = true;
    code_base = cb;
    code_size = cs;
    image_base = ib;
    image_size = is;
    const auto t0 = std::chrono::steady_clock::now();
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir;
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) dir = fs::path(buf).parent_path();
#endif
    if (dir.empty()) dir = fs::current_path(ec);
    for (int level = 0; level < 8 && !dir.empty(); ++level) {
      if (fs::is_directory(dir / "db", ec) && fs::is_directory(dir / "docs" / "systems", ec)) {
        root = dir;
        break;
      }
      if (dir.parent_path() == dir) break;
      dir = dir.parent_path();
    }
    if (root.empty()) {
      errors.push_back("no db/ + docs/systems/ folder above the exe: no labels");
      return;
    }
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(root / "db", ec)) {
      if (e.path().extension() == ".json") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
      std::string text;
      json::Value v;
      if (!ReadFile(f, text) || !json::Parse(text, v) || v.t != json::Value::T::kArr) {
        errors.push_back("can't parse " + f.filename().string());
        continue;
      }
      ++db_files;
      for (const auto& e : v.a) {
        if (e.t != json::Value::T::kObj) continue;
        ++db_entries;
        AddDbEntry(e, f.stem().string());
      }
    }
    files.clear();
    for (const auto& e : fs::directory_iterator(root / "docs" / "systems", ec)) {
      if (e.path().extension() == ".md") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
      std::string text;
      if (!ReadFile(f, text)) continue;
      ++doc_files;
      LoadDoc(text, f.stem().string());
    }
    const fs::path states = root / "tools" / "input_scripts" / "states.toml";
    if (fs::is_regular_file(states, ec)) {
      std::vector<GuestState> st;
      std::string error;
      if (LoadGuestStates(states.string(), st, error)) {
        for (auto& s : st) {
          AddChain(s.name, s.chain, "states.toml", "tools/input_scripts/states.toml", s.meaning, &s);
          ++state_chains;
        }
      } else {
        errors.push_back(error);
      }
    }
    // db labels sort before docs labels at the same address, so lookups prefer them.
    std::stable_sort(labels.begin(), labels.end(), [](const Label& x, const Label& y) {
      if (x.addr != y.addr) return x.addr < y.addr;
      return x.from_db && !y.from_db;
    });
    DropShadowedDocLabels();
    load_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }

  // The best label for a word at `a`: an exact db label, else a range covering it (name+0xOFF).
  std::string For(uint32_t a, bool include_code = true) const {
    auto it = std::lower_bound(labels.begin(), labels.end(), a, [](const Label& l, uint32_t v) { return l.addr < v; });
    if (it != labels.end() && it->addr == a && (include_code || it->is_data)) return it->name;
    // Ranges that start before `a` (scan back a little: ranges are few and short).
    const Label* best = nullptr;
    for (auto r = std::make_reverse_iterator(it); r != labels.rend() && a - r->addr < 0x10000; ++r) {
      if (r->end > a && r->end - r->addr > 4 && (include_code || r->is_data)) {
        best = &*r;
        break;
      }
    }
    if (!best) return {};
    char off[16];
    std::snprintf(off, sizeof(off), "+0x%X", a - best->addr);
    return best->name + off;
  }
  const Label* Exact(uint32_t a) const {
    auto it = std::lower_bound(labels.begin(), labels.end(), a, [](const Label& l, uint32_t v) { return l.addr < v; });
    return it != labels.end() && it->addr == a ? &*it : nullptr;
  }
  // Every distinct name at exactly `a` (several db systems often name the same global differently, e.g. the timing
  // struct), joined with " / "; else For(a).
  std::string NamesAt(uint32_t a) const {
    auto it = std::lower_bound(labels.begin(), labels.end(), a, [](const Label& l, uint32_t v) { return l.addr < v; });
    std::string out;
    int n = 0;
    for (; it != labels.end() && it->addr == a; ++it) {
      if (out.find(it->name) != std::string::npos) continue;
      if (++n > 4) {
        out += " / ...";
        break;
      }
      if (!out.empty()) out += " / ";
      out += it->name;
    }
    return out.empty() ? For(a) : out;
  }
  // Drops docs-table labels at addresses a db entry already names (the db is the curated source; table rows are a
  // heuristic and often say "Confidence only" or similar). Call after sorting.
  void DropShadowedDocLabels() {
    std::vector<Label> kept;
    kept.reserve(labels.size());
    for (size_t i = 0; i < labels.size(); ++i) {
      const Label& l = labels[i];
      if (!l.from_db && !kept.empty() && kept.back().addr == l.addr && kept.back().from_db) {
        --doc_labels;
        continue;
      }
      kept.push_back(l);
    }
    labels.swap(kept);
  }
};

// ---------------------------------------------------------------------------------------------------------------
// Inspector state (UI thread only, except Post).
// ---------------------------------------------------------------------------------------------------------------

class Dialog;

struct State {
  rex::ui::ImGuiDrawer* drawer = nullptr;
  Dialog* dialog = nullptr;
  std::function<void(std::function<void()>)> post_ui;
  uint32_t code_base = 0, code_size = 0, image_base = 0, image_size = 0;
  Labels labels;
  uint64_t draws = 0;
  bool logged_window = false, logged_memory = false, logged_settings = false, logged_coverage = false;

  // memory viewer
  char addr_text[128] = {};
  std::string view_chain;  // what was last applied
  GuestState view_state;
  bool view_valid = false;
  uint32_t view_addr = 0;
  bool view_resolved = false;
  std::vector<std::string> history;
  int rows = 16;
  bool auto_refresh = true;
  int refresh_every = 1;
  uint64_t last_refresh = 0;
  bool refresh_now = true;
  uint32_t snap_base = 0;
  std::vector<uint32_t> words, prev_words;
  std::vector<uint8_t> word_ok;
  std::vector<uint64_t> changed_at;
  char globals_filter[64] = {};
  bool globals_code = false;
  char chains_filter[64] = {};

  // settings
  char settings_filter[64] = {};
  bool settings_all = false;
  std::vector<std::string> setting_names;
  uint64_t settings_listed_at = 0;
  bool settings_dirty = true;
  std::map<std::string, std::string> edit_buffers;
  std::string editing;
  std::string last_change;

  // coverage
  char cov_lookup[32] = {};
  std::string cov_looked_up;  // the text the cached answer is for
  uint64_t cov_lookup_at = 0;
  bool cov_in_window = false, cov_in_earlier = false;
};

inline State& S() {
  static State* s = new State;  // never destroyed (no static-destruction order issues at exit)
  return *s;
}

inline std::string Hex32(uint32_t v);

// --wwe2k14_inspector_dump_labels=PATH: every label and named chain as TSV, once, right after the labels load
// (self-test of the label sources without clicking through the tabs). Chain values are read at that moment.
inline void DumpLabels(const Labels& l) {
  const std::string path = REXCVAR_GET(wwe2k14_inspector_dump_labels);
  if (path.empty()) return;
  std::error_code ec;
  const std::filesystem::path p = std::filesystem::absolute(path, ec);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary);
  if (!out) {
    REXLOG_WARN("[wwe2k14][inspector] can't write the label dump '{}'", p.string());
    return;
  }
  out << "kind\taddress\tend\tname\tsystem\tconfidence\tsource\tvalue_now\n";
  for (const Label& x : l.labels) {
    out << (x.is_data ? "data" : "code") << '\t' << Hex32(x.addr) << '\t' << Hex32(x.end) << '\t' << x.name << '\t'
        << x.system << '\t' << x.confidence << '\t' << x.source << "\t\n";
  }
  for (const NamedChain& c : l.chains) {
    uint32_t v = 0, at = 0;
    const bool ok = ReadChainValue(c.st, &v, &at);
    out << "chain\t" << c.chain << "\tsize " << c.st.size << (c.st.is_signed ? " signed" : "") << '\t' << c.name
        << '\t' << c.system << "\t\t" << c.source << '\t'
        << (ok ? Hex32(at) + " = " + ChainValueText(c.st, v) : std::string("unreadable")) << '\n';
  }
  REXLOG_INFO("[wwe2k14][inspector] label dump: {} labels, {} chains -> {}", l.labels.size(), l.chains.size(),
              p.string());
}

inline void Go(const std::string& text, bool push_history) {
  State& s = S();
  GuestState st;
  if (!ParseStateChain(text, st)) {
    s.view_valid = false;
    s.view_chain = text;
    return;
  }
  if (push_history && !s.view_chain.empty() && s.view_chain != text) s.history.push_back(s.view_chain);
  if (s.history.size() > 64) s.history.erase(s.history.begin());
  s.view_chain = text;
  s.view_state = st;
  s.view_valid = true;
  s.refresh_now = true;
  std::snprintf(s.addr_text, sizeof(s.addr_text), "%s", text.c_str());
}

inline std::string Hex32(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof(b), "0x%08X", v);
  return b;
}

inline const ImVec4 kChanged(1.0f, 0.85f, 0.2f, 1.0f);
inline const ImVec4 kDim(0.6f, 0.6f, 0.6f, 1.0f);
inline const ImVec4 kBad(1.0f, 0.4f, 0.4f, 1.0f);
inline const ImVec4 kGood(0.5f, 0.9f, 0.5f, 1.0f);

inline bool ContainsCI(const std::string& hay, const char* needle) {
  if (!needle || !*needle) return true;
  const std::string n(needle);
  auto it = std::search(hay.begin(), hay.end(), n.begin(), n.end(), [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
  });
  return it != hay.end();
}

// ---------------------------------------------------------------------------------------------------------------
// Panels
// ---------------------------------------------------------------------------------------------------------------

inline void RefreshMemory() {
  State& s = S();
  s.view_resolved = s.view_valid && ResolveChain(s.view_state, &s.view_addr);
  const uint32_t base = s.view_addr & ~3u;
  const size_t n = size_t(s.rows) * 4;
  if (base != s.snap_base || s.words.size() != n) {
    s.words.assign(n, 0);
    s.prev_words.assign(n, 0);
    s.word_ok.assign(n, 0);
    s.changed_at.assign(n, 0);
    s.snap_base = base;
  }
  PageCheck cache;
  for (size_t i = 0; i < n; ++i) {
    uint32_t v = 0;
    const bool ok = s.view_resolved && ReadWord(base + uint32_t(i * 4), &v, cache);
    if (ok && s.word_ok[i] && v != s.words[i]) s.changed_at[i] = s.draws;
    s.prev_words[i] = s.words[i];
    s.words[i] = v;
    s.word_ok[i] = ok;
  }
  s.last_refresh = s.draws;
}

inline void DrawMemory() {
  State& s = S();
  if (!s.labels.loaded) {
    s.labels.Load(s.code_base, s.code_size, s.image_base, s.image_size);
    DumpLabels(s.labels);
  }
  ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(930, 430), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("wwe2k14 memory")) {
    ImGui::End();
    return;
  }
  ImGui::SetNextItemWidth(260);
  const bool enter = ImGui::InputText("##addr", s.addr_text, sizeof(s.addr_text),
                                      ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsNoBlank);
  ImGui::SetItemTooltip("0xADDR or a chain 0xADDR>OFF>OFF (each >OFF reads a pointer and adds OFF; decimal or 0x)");
  ImGui::SameLine();
  if (ImGui::Button("Go") || enter) Go(s.addr_text, true);
  ImGui::SameLine();
  ImGui::BeginDisabled(s.history.empty());
  if (ImGui::Button("Back")) {
    std::string prev = s.history.back();
    s.history.pop_back();
    Go(prev, false);
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::SetNextItemWidth(90);
  if (ImGui::InputInt("rows", &s.rows)) s.rows = std::clamp(s.rows, 1, 64);
  ImGui::SameLine();
  ImGui::Checkbox("auto", &s.auto_refresh);
  ImGui::SetItemTooltip("re-read every N draws");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(80);
  if (ImGui::InputInt("##every", &s.refresh_every)) s.refresh_every = std::clamp(s.refresh_every, 1, 600);
  ImGui::SameLine();
  if (ImGui::Button("Refresh")) s.refresh_now = true;

  if (s.refresh_now || (s.auto_refresh && s.draws - s.last_refresh >= uint64_t(s.refresh_every))) {
    s.refresh_now = false;
    RefreshMemory();
  }
  if (!GuestMemory()) {
    ImGui::TextColored(kDim, "guest memory not up yet");
  } else if (!s.view_valid) {
    ImGui::TextColored(s.view_chain.empty() ? kDim : kBad, "%s",
                       s.view_chain.empty() ? "type an address or pick a label below" : "bad address/chain");
  } else if (!s.view_resolved) {
    ImGui::TextColored(kBad, "%s: null pointer or unreadable link in the chain", s.view_chain.c_str());
  } else {
    const std::string lab = s.labels.NamesAt(s.view_addr);
    ImGui::Text("%s -> 0x%08X%s%s", s.view_chain.c_str(), s.view_addr, lab.empty() ? "" : "  ", lab.c_str());
  }

  const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit |
                             ImGuiTableFlags_ScrollY;
  const float table_h = std::min(float(s.rows) + 1.5f, 13.5f) * ImGui::GetTextLineHeightWithSpacing();
  int readable = 0;
  if (ImGui::BeginTable("mem", 11, tf, ImVec2(0, table_h))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("address");
    for (const char* h : {"+0", "+4", "+8", "+C"}) ImGui::TableSetupColumn(h);
    for (const char* h : {"f+0", "f+4", "f+8", "f+C"}) ImGui::TableSetupColumn(h);
    ImGui::TableSetupColumn("ascii");
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    const bool have = s.view_resolved && !s.words.empty();
    for (int r = 0; have && r < s.rows && size_t(r) * 4 + 3 < s.words.size(); ++r) {
      const uint32_t row_addr = s.snap_base + uint32_t(r) * 16;
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(Hex32(row_addr).c_str());
      std::string ascii, row_label;
      for (int c = 0; c < 4; ++c) {
        const size_t i = size_t(r) * 4 + c;
        const uint32_t v = s.words[i];
        const bool ok = s.word_ok[i] != 0;
        readable += ok ? 1 : 0;
        const bool changed = ok && s.changed_at[i] && s.draws - s.changed_at[i] < 30;
        ImGui::TableSetColumnIndex(1 + c);
        char cellid[48];
        if (ok) {
          std::snprintf(cellid, sizeof(cellid), "%08X##w%zu", v, i);
        } else {
          std::snprintf(cellid, sizeof(cellid), "????????##w%zu", i);
        }
        if (changed) ImGui::PushStyleColor(ImGuiCol_Text, kChanged);
        if (ImGui::Selectable(cellid, false) && ok && v != 0) {
          PageCheck pc;
          uint32_t probe = 0;
          if (ReadWord(v & ~3u, &probe, pc)) Go(Hex32(v & ~3u), true);
        }
        if (changed) ImGui::PopStyleColor();
        if (ok && ImGui::IsItemHovered()) {
          const std::string vl = s.labels.For(v & ~3u);
          ImGui::SetTooltip("0x%08X = %u (signed %d), float %g%s%s\nclick: follow it as a pointer", v, v, int32_t(v),
                            double(std::bit_cast<float>(v)), vl.empty() ? "" : "\nas address: ", vl.c_str());
        }
        ImGui::TableSetColumnIndex(5 + c);
        if (ok) {
          ImGui::Text("%.5g", double(std::bit_cast<float>(v)));
        } else {
          ImGui::TextColored(kDim, "-");
        }
        for (int b = 3; b >= 0; --b) {
          const uint8_t ch = ok ? uint8_t(v >> (b * 8)) : uint8_t('?');
          ascii += (ch >= 0x20 && ch < 0x7F) ? char(ch) : '.';
        }
        const Label* ex = s.labels.Exact(row_addr + uint32_t(c) * 4);
        if (ex) {
          if (!row_label.empty()) row_label += ", ";
          char off[8];
          std::snprintf(off, sizeof(off), "+%X:", c * 4);
          row_label += off + ex->name;
        }
      }
      if (row_label.empty()) row_label = s.labels.For(row_addr, false);
      ImGui::TableSetColumnIndex(9);
      ImGui::TextUnformatted(ascii.c_str());
      ImGui::TableSetColumnIndex(10);
      ImGui::TextColored(kDim, "%s", row_label.c_str());
    }
    ImGui::EndTable();
  }

  if (ImGui::BeginTabBar("labels")) {
    char tab[64];
    size_t data_labels = 0;
    for (const auto& l : s.labels.labels) data_labels += l.is_data ? 1 : 0;
    std::snprintf(tab, sizeof(tab), "Globals (%zu)###globals", data_labels);
    if (ImGui::BeginTabItem(tab)) {
      ImGui::SetNextItemWidth(200);
      ImGui::InputTextWithHint("##gf", "filter", s.globals_filter, sizeof(s.globals_filter));
      ImGui::SameLine();
      ImGui::Checkbox("functions", &s.globals_code);
      ImGui::SameLine();
      ImGui::TextColored(kDim, "db %d files / %d entries, docs %d files (%d table labels), %.0f ms",
                         s.labels.db_files, s.labels.db_entries, s.labels.doc_files, s.labels.doc_labels,
                         s.labels.load_ms);
      for (const auto& e : s.labels.errors) ImGui::TextColored(kBad, "%s", e.c_str());
      if (ImGui::BeginTable("globals", 5, tf, ImVec2(0, -FLT_MIN))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("address");
        ImGui::TableSetupColumn("name");
        ImGui::TableSetupColumn("system");
        ImGui::TableSetupColumn("confidence");
        ImGui::TableSetupColumn("source", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        std::vector<int> rows;
        for (size_t k = 0; k < s.labels.labels.size(); ++k) {
          const Label& l = s.labels.labels[k];
          if (!l.is_data && !s.globals_code) continue;
          if (s.globals_filter[0] && !ContainsCI(l.name, s.globals_filter) && !ContainsCI(l.system, s.globals_filter) &&
              !ContainsCI(Hex32(l.addr), s.globals_filter)) {
            continue;
          }
          rows.push_back(int(k));
        }
        ImGuiListClipper clipper;
        clipper.Begin(int(rows.size()));
        while (clipper.Step()) for (int id = clipper.DisplayStart; id < clipper.DisplayEnd; ++id) {
          const Label& l = s.labels.labels[size_t(rows[size_t(id)])];
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          char sel[40];
          std::snprintf(sel, sizeof(sel), "0x%08X##g%d", l.addr, id);
          if (ImGui::Selectable(sel, false, ImGuiSelectableFlags_SpanAllColumns)) Go(Hex32(l.addr), true);
          if (ImGui::IsItemHovered() && !l.summary.empty()) {
            ImGui::SetTooltip("%s", l.summary.substr(0, 400).c_str());
          }
          ImGui::TableSetColumnIndex(1);
          if (l.end - l.addr > 4) {
            ImGui::Text("%s (..0x%08X)", l.name.c_str(), l.end - 4);
          } else {
            ImGui::TextUnformatted(l.name.c_str());
          }
          ImGui::TableSetColumnIndex(2);
          ImGui::TextUnformatted(l.system.c_str());
          ImGui::TableSetColumnIndex(3);
          ImGui::TextUnformatted(l.confidence.c_str());
          ImGui::TableSetColumnIndex(4);
          ImGui::TextColored(kDim, "%s", l.source.c_str());
        }
        ImGui::EndTable();
      }
      ImGui::EndTabItem();
    }
    std::snprintf(tab, sizeof(tab), "Chains (%zu)###chains", s.labels.chains.size());
    if (ImGui::BeginTabItem(tab)) {
      ImGui::SetNextItemWidth(200);
      ImGui::InputTextWithHint("##cf", "filter", s.chains_filter, sizeof(s.chains_filter));
      ImGui::SameLine();
      ImGui::TextColored(kDim,
                         "states.toml %d, db %d, doc peek specs %d; value = what the chain's last read reads now "
                         "(states.toml size, else a word)",
                         s.labels.state_chains, s.labels.db_chains, s.labels.doc_chains);
      if (ImGui::BeginTable("chains", 5, tf, ImVec2(0, -FLT_MIN))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("name");
        ImGui::TableSetupColumn("chain");
        ImGui::TableSetupColumn("value");
        ImGui::TableSetupColumn("system");
        ImGui::TableSetupColumn("source", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        std::vector<int> rows;
        for (size_t k = 0; k < s.labels.chains.size(); ++k) {
          const NamedChain& c = s.labels.chains[k];
          if (s.chains_filter[0] && !ContainsCI(c.name, s.chains_filter) && !ContainsCI(c.chain, s.chains_filter) &&
              !ContainsCI(c.system, s.chains_filter)) {
            continue;
          }
          rows.push_back(int(k));
        }
        ImGuiListClipper clipper;
        clipper.Begin(int(rows.size()));
        while (clipper.Step()) for (int id = clipper.DisplayStart; id < clipper.DisplayEnd; ++id) {
          const NamedChain& c = s.labels.chains[size_t(rows[size_t(id)])];
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          char sel[96];
          std::snprintf(sel, sizeof(sel), "%s##c%d", c.name.c_str(), id);
          if (ImGui::Selectable(sel, false, ImGuiSelectableFlags_SpanAllColumns)) Go(c.chain, true);
          if (ImGui::IsItemHovered() && !c.meaning.empty()) ImGui::SetTooltip("%s", c.meaning.substr(0, 400).c_str());
          ImGui::TableSetColumnIndex(1);
          ImGui::TextUnformatted(c.chain.c_str());
          ImGui::TableSetColumnIndex(2);
          uint32_t v = 0;
          if (ReadChainValue(c.st, &v)) {
            ImGui::TextUnformatted(ChainValueText(c.st, v).c_str());
          } else {
            ImGui::TextColored(kDim, "unreadable");
          }
          ImGui::TableSetColumnIndex(3);
          ImGui::TextUnformatted(c.system.c_str());
          ImGui::TableSetColumnIndex(4);
          ImGui::TextColored(kDim, "%s", c.source.c_str());
        }
        ImGui::EndTable();
      }
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::End();

  if (!s.logged_memory) {
    s.logged_memory = true;
    REXLOG_INFO("[wwe2k14][inspector] panel memory drawn (first time): guest frame {}, view '{}' -> {} ({}), {} of {} "
                "words readable, first word {}; labels {} (db {} files / {} entries, docs {} files / {} table labels), "
                "chains {} (states.toml {}, db {}, doc peek specs {}; {} db bracket fields unparsed), root '{}', "
                "loaded in {:.1f} ms{}",
                g_guest_frame.load(), s.view_chain, Hex32(s.view_addr),
                s.view_resolved ? s.labels.NamesAt(s.view_addr) : std::string("unresolved"), readable, s.words.size(),
                !s.words.empty() && s.word_ok[0] ? Hex32(s.words[0]) : std::string("-"), s.labels.labels.size(),
                s.labels.db_files, s.labels.db_entries, s.labels.doc_files, s.labels.doc_labels,
                s.labels.chains.size(), s.labels.state_chains, s.labels.db_chains, s.labels.doc_chains,
                s.labels.db_unparsed, s.labels.root.string(), s.labels.load_ms,
                s.labels.errors.empty() ? "" : "; errors: " + s.labels.errors.front());
  }
}

// ReXGlue settings this app sets in OnPreSetup (wwe2k14_app.h), plus a few useful ones. Missing names are skipped.
inline const std::vector<std::string>& SelectedSdkSettings() {
  static const std::vector<std::string> names = {
      "vsync", "readback_resolve", "resolution_scale", "gpu_allow_invalid_fetch_constants",
      "render_target_path_vulkan", "gpu_wait_reg_mem_spin", "vulkan_async_submit", "vulkan_submit_on_wait",
      "vulkan_deferred_descriptor_writes", "gpu_type0_bulk_writes", "gpu_type0_block_split",
      "gpu_request_ranges_fast", "vulkan_reuse_texture_bindings", "vulkan_sampler_param_cache",
      "vulkan_constants_dynamic_offsets", "gpu_rt_cache_skip_unchanged", "vulkan_system_constants_skip",
      "vulkan_viewport_info_cache", "vulkan_vertex_residency_cache_off", "vulkan_async_swap",
      "vulkan_pipeline_no_submit_wait", "gpu_vblank_precise_timer", "gpu_request_textures_skip_unchanged",
      "gpu_upload_skip_unchanged", "vulkan_texture_load_direct", "mnk_mode", "log_level"};
  return names;
}

inline std::string_view SourceText(rex::cvar::Source source) {
  switch (source) {
    case rex::cvar::Source::kDefault: return "default";
    case rex::cvar::Source::kConfig: return "config";
    case rex::cvar::Source::kEnvironment: return "env";
    case rex::cvar::Source::kCommandLine: return "cmdline";
    case rex::cvar::Source::kRuntime: return "runtime";
  }
  return "?";
}

// Our settings are all REXCVAR_DEFINE_* with no .lifecycle(), so the registry says kHotReload for every one of
// them whether or not anything re-reads it. These are the ones checked (t15 fixer, from the code) to be read only
// once, at startup or at the inspector's first show / first draw: changing them in-run does nothing until a restart.
// Where each is read: wwe2k14_app.h OnPreSetup (gpu_backend), OnPostSetup (mount_cache; timeline via
// gpu_timeline.h StartTimelineIfRequested; input_script / input_record via input_script.h InputScript::Init, which
// also runs coverage.h Init for coverage / coverage_dir), OnPreLaunchModule (dump_words, dump_image),
// OnCreateDialogs (inspector); inspector.h Show (inspector_addr, only before the first view), DrawSettings first
// draw (inspector_test_set), DrawMemory label load (inspector_dump_labels).
inline const char* BootOnlyWhere(std::string_view name) {
  static const std::pair<std::string_view, const char*> kBootOnly[] = {
      {"wwe2k14_gpu_backend", "read once in OnPreSetup"},
      {"wwe2k14_mount_cache", "read once in OnPostSetup"},
      {"wwe2k14_timeline", "read once in OnPostSetup"},
      {"wwe2k14_input_script", "read once at startup (InputScript::Init)"},
      {"wwe2k14_input_record", "read once at startup (InputScript::Init)"},
      {"wwe2k14_coverage", "read once at startup (coverage Init)"},
      {"wwe2k14_coverage_dir", "read once at startup (coverage Init)"},
      {"wwe2k14_dump_words", "read once in OnPreLaunchModule"},
      {"wwe2k14_dump_image", "read once in OnPreLaunchModule"},
      {"wwe2k14_inspector", "read once in OnCreateDialogs"},
      {"wwe2k14_inspector_addr", "read at the inspector's first show only"},
      {"wwe2k14_inspector_test_set", "read once, when the settings panel first draws"},
      {"wwe2k14_inspector_dump_labels", "read once, when the labels load"},
  };
  for (const auto& [n, where] : kBootOnly) {
    if (n == name) return where;
  }
  return nullptr;
}

// What the "when" column says. Our own settings never declare a lifecycle (always kHotReload in the registry), so
// for them "live" would be a claim nobody checked.
inline std::string WhenText(const std::string& name, const std::string& category, rex::cvar::Lifecycle lifecycle) {
  if (lifecycle == rex::cvar::Lifecycle::kInitOnly) return "init only (read-only)";
  if (lifecycle == rex::cvar::Lifecycle::kRequiresRestart) return "restart";
  if (const char* where = BootOnlyWhere(name)) return std::string("boot only: ") + where + "; restart to apply";
  if (category == "wwe2k14") return "not declared: may only be read at startup";
  return "hot reload (as the SDK declares it; not checked)";
}

inline void ApplySetting(const std::string& name, const std::string& value) {
  State& s = S();
  const std::string old = rex::cvar::GetFlagByName(name);
  const bool ok = rex::cvar::SetFlagByName(name, value);
  const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(name);
  const bool restart = info && (info->lifecycle == rex::cvar::Lifecycle::kRequiresRestart || BootOnlyWhere(name));
  const bool undeclared = info && !restart && info->category == "wwe2k14";
  const char* note = !ok ? "" : restart ? " (takes effect after a restart)"
                              : undeclared ? " (when it is read is not declared: may need a restart)" : "";
  s.last_change = name + " = '" + value + "' (was '" + old + "') -> " + (ok ? "ok" : "REJECTED") + note;
  REXLOG_INFO("[wwe2k14][inspector] set {} = '{}' (was '{}', guest frame {}) -> {}{}", name, value, old,
              g_guest_frame.load(), ok ? "ok" : "rejected", note);
}

inline void DrawSettings() {
  State& s = S();
  ImGui::SetNextWindowPos(ImVec2(8, 446), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(930, 266), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("wwe2k14 settings")) {
    ImGui::End();
    return;
  }
  ImGui::SetNextItemWidth(200);
  if (ImGui::InputTextWithHint("##sf", "filter", s.settings_filter, sizeof(s.settings_filter))) s.settings_dirty = true;
  ImGui::SameLine();
  if (ImGui::Checkbox("all settings", &s.settings_all)) s.settings_dirty = true;
  ImGui::SameLine();
  ImGui::TextColored(kDim, "%s", s.last_change.empty() ? "Enter applies a value" : s.last_change.c_str());
  if (s.settings_dirty || s.draws - s.settings_listed_at > 120) {
    s.settings_dirty = false;
    s.settings_listed_at = s.draws;
    std::vector<std::string> names;
    if (s.settings_all) {
      names = rex::cvar::ListFlags();
    } else {
      names = rex::cvar::ListFlagsByCategory("wwe2k14");
      for (const auto& n : SelectedSdkSettings()) {
        if (rex::cvar::GetFlagInfo(n)) names.push_back(n);
      }
    }
    s.setting_names.clear();
    for (const auto& n : names) {
      const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(n);
      if (!info || info->type == rex::cvar::FlagType::Command) continue;  // commands have no value
      if (ContainsCI(n, s.settings_filter)) s.setting_names.push_back(n);
    }
  }
  int shown = 0;
  const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit |
                             ImGuiTableFlags_ScrollY;
  if (ImGui::BeginTable("settings", 4, tf, ImVec2(0, -FLT_MIN))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("setting");
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, 260);
    ImGui::TableSetupColumn("source");
    ImGui::TableSetupColumn("when", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGuiListClipper clipper;
    clipper.Begin(int(s.setting_names.size()));
    std::string pending_name, pending_value;
    while (clipper.Step()) {
      for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
        const std::string& name = s.setting_names[size_t(row)];
        const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(name);
        if (!info) continue;
        // Copy what's needed now: the registry can grow (and move) when a setting registers later.
        const rex::cvar::FlagType type = info->type;
        const rex::cvar::Lifecycle lifecycle = info->lifecycle;
        const std::string description = info->description, category = info->category, def = info->default_value;
        if (type == rex::cvar::FlagType::Command) continue;
        ++shown;
        const std::string value = rex::cvar::GetFlagByName(name);
        const rex::cvar::Source source = rex::cvar::GetFlagSource(name);
        ImGui::PushID(row);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(name.c_str());
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("%s\ncategory %s, default '%s'", description.c_str(), category.c_str(), def.c_str());
        }
        ImGui::TableSetColumnIndex(1);
        const bool init_only = lifecycle == rex::cvar::Lifecycle::kInitOnly;
        ImGui::BeginDisabled(init_only);
        if (type == rex::cvar::FlagType::Boolean) {
          bool b = value == "true";
          if (ImGui::Checkbox("##b", &b)) {
            pending_name = name;
            pending_value = b ? "true" : "false";
          }
        } else {
          std::string& buf = s.edit_buffers[name];
          if (s.editing != name || buf.size() != 256) {
            buf.assign(256, '\0');
            std::memcpy(buf.data(), value.data(), std::min<size_t>(value.size(), 255));
          }
          ImGui::SetNextItemWidth(-FLT_MIN);
          const bool enter = ImGui::InputText("##v", buf.data(), buf.size(), ImGuiInputTextFlags_EnterReturnsTrue);
          if (ImGui::IsItemActive()) {
            s.editing = name;
          } else if (s.editing == name) {
            s.editing.clear();
          }
          if (enter) {
            pending_name = name;
            pending_value = std::string(buf.c_str());
            s.editing.clear();
          }
        }
        ImGui::EndDisabled();
        ImGui::TableSetColumnIndex(2);
        ImGui::TextColored(source == rex::cvar::Source::kDefault ? kDim : kGood, "%s",
                           std::string(SourceText(source)).c_str());
        ImGui::TableSetColumnIndex(3);
        ImGui::TextColored(kDim, "%s", WhenText(name, category, lifecycle).c_str());
        if (type == rex::cvar::FlagType::String && ImGui::IsItemHovered()) {
          ImGui::SetTooltip("String settings are replaced without a lock: a thread that reads this one at that moment "
                            "(wwe2k14_peek every frame, wwe2k14_file_trace_filter on every file open) could read a "
                            "freed string. Unlikely, and the input script's cvar command has the same race.");
        }
        ImGui::PopID();
      }
    }
    ImGui::EndTable();
    if (!pending_name.empty()) ApplySetting(pending_name, pending_value);
  }
  ImGui::End();
  if (!s.logged_settings) {
    s.logged_settings = true;
    REXLOG_INFO("[wwe2k14][inspector] panel settings drawn (first time): guest frame {}, {} settings listed ({} "
                "visible rows), mode {}",
                g_guest_frame.load(), s.setting_names.size(), shown, s.settings_all ? "all" : "wwe2k14 + selected");
    // Unattended runs can't click: --wwe2k14_inspector_test_set=NAME=VALUE goes through the same ApplySetting the
    // checkbox / Enter path calls.
    const std::string test = REXCVAR_GET(wwe2k14_inspector_test_set);
    const size_t eq = test.find('=');
    if (eq != std::string::npos && eq > 0) {
      const std::string name = test.substr(0, eq);
      ApplySetting(name, test.substr(eq + 1));
      REXLOG_INFO("[wwe2k14][inspector] self-test: {} now reads '{}' (source {})", name,
                  rex::cvar::GetFlagByName(name), std::string(SourceText(rex::cvar::GetFlagSource(name))));
    }
  }
}

#if defined(WWE2K14_COVERAGE)
inline void DrawCoverage() {
  State& s = S();
  ImGui::SetNextWindowPos(ImVec2(946, 8), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(326, 704), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("wwe2k14 coverage")) {
    ImGui::End();
    return;
  }
  auto& cs = ::wwe2k14::coverage::St();
  bool enabled = false;
  int windows = 0;
  uint64_t start = 0;
  size_t window_hits = 0, ever = 0;
  std::vector<const char*> newest;
  bool lookup_done = false;
  uint32_t lookup_addr = 0;
  {
    char* end = nullptr;
    lookup_addr = uint32_t(std::strtoul(s.cov_lookup, &end, 16));
    lookup_done = s.cov_lookup[0] && end && *end == '\0' && lookup_addr;
  }
  // The lookup scans every recorded name under the lock guest threads take on a first call: only when the text
  // changes or every 60 draws, not every frame.
  const bool rescan = lookup_done && (s.cov_looked_up != s.cov_lookup || s.draws - s.cov_lookup_at >= 60);
  char want[24] = {};
  if (rescan) std::snprintf(want, sizeof(want), "__imp__sub_%08X", lookup_addr);
  {
    std::lock_guard<std::mutex> lock(cs.mutex);
    enabled = cs.enabled;
    windows = cs.windows;
    start = cs.window_start_frame;
    window_hits = cs.hits.size();
    ever = cs.ever.size();
    const size_t take = std::min<size_t>(window_hits, 200);
    newest.assign(cs.hits.end() - ptrdiff_t(take), cs.hits.end());
    if (rescan) {
      s.cov_in_window = s.cov_in_earlier = false;
      for (const char* f : cs.hits) {
        if (std::strcmp(f, want) == 0) {
          s.cov_in_window = true;
          break;
        }
      }
      for (const char* f : cs.ever) {
        if (std::strcmp(f, want) == 0) {
          s.cov_in_earlier = true;
          break;
        }
      }
    }
  }
  if (rescan) {
    s.cov_looked_up = s.cov_lookup;
    s.cov_lookup_at = s.draws;
  }
  const bool lookup_window = s.cov_in_window, lookup_ever = s.cov_in_earlier;
  if (!enabled) {
    ImGui::TextColored(kDim, "coverage build, recording off (--wwe2k14_coverage=true)");
  } else {
    ImGui::Text("window %d (frames %llu..%llu)", windows + 1, (unsigned long long)start,
                (unsigned long long)g_guest_frame.load());
    ImGui::Text("first-called this window: %zu", window_hits);
    ImGui::Text("seen in earlier windows: %zu", ever);
    ImGui::TextColored(kDim, "counts distinct functions (first call per window), not calls");
  }
  ImGui::SetNextItemWidth(120);
  ImGui::InputTextWithHint("##cl", "8xxxxxxx", s.cov_lookup, sizeof(s.cov_lookup), ImGuiInputTextFlags_CharsHexadecimal);
  ImGui::SameLine();
  if (lookup_done) {
    const std::string n = s.labels.For(lookup_addr);
    ImGui::Text("%s%s%s: window %s, earlier %s", n.empty() ? "" : n.c_str(), n.empty() ? "" : " ",
                Hex32(lookup_addr).c_str(), lookup_window ? "yes" : "no", lookup_ever ? "yes" : "no");
  } else {
    ImGui::TextColored(kDim, "did it run?");
  }
  if (ImGui::BeginTable("cov", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
                        ImVec2(0, -FLT_MIN))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("newest first calls");
    ImGui::TableSetupColumn("db name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (auto it = newest.rbegin(); it != newest.rend(); ++it) {
      const std::string g = ::wwe2k14::coverage::GuestName(*it);
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(g.c_str());
      ImGui::TableSetColumnIndex(1);
      if (g.rfind("0x", 0) == 0) {
        const Label* l = s.labels.Exact(uint32_t(std::strtoul(g.c_str() + 2, nullptr, 16)));
        if (l) ImGui::TextUnformatted(l->name.c_str());
      }
    }
    ImGui::EndTable();
  }
  ImGui::End();
  if (!s.logged_coverage) {
    s.logged_coverage = true;
    REXLOG_INFO("[wwe2k14][inspector] panel coverage drawn (first time): guest frame {}, recording {}, window {}, {} "
                "first-called this window, {} in earlier windows",
                g_guest_frame.load(), enabled ? "on" : "off", windows + 1, window_hits, ever);
  }
}
#endif

class Dialog final : public rex::ui::ImGuiDialog {
 public:
  explicit Dialog(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;
    State& s = S();
    ++s.draws;
    if (!s.logged_window) {
      s.logged_window = true;
      REXLOG_INFO("[wwe2k14][inspector] window shown (first draw): guest frame {}, display {}x{}", g_guest_frame.load(),
                  int(io.DisplaySize.x), int(io.DisplaySize.y));
    }
    DrawMemory();
    DrawSettings();
#if defined(WWE2K14_COVERAGE)
    DrawCoverage();
#endif
  }
};

// ---------------------------------------------------------------------------------------------------------------
// Show / hide (UI thread)
// ---------------------------------------------------------------------------------------------------------------

inline void Show() {
  State& s = S();
  if (s.dialog || !s.drawer) return;
  if (s.view_chain.empty() && !REXCVAR_GET(wwe2k14_inspector_addr).empty()) {
    Go(REXCVAR_GET(wwe2k14_inspector_addr), false);
  }
  s.dialog = new Dialog(s.drawer);
  REXLOG_INFO("[wwe2k14][inspector] shown (guest frame {})", g_guest_frame.load());
}

inline void Hide() {
  State& s = S();
  if (!s.dialog) return;
  delete s.dialog;  // removes it from the drawer; with no other overlay open the drawer draws nothing
  s.dialog = nullptr;
  REXLOG_INFO("[wwe2k14][inspector] hidden (guest frame {})", g_guest_frame.load());
}

// From ReXApp::OnCreateDialogs (UI thread). Returns at once unless --wwe2k14_inspector=true.
// code/image ranges: PPCImageConfig (generated config; code for "is this a function", image for docs-table labels).
inline void Install(rex::ui::ImGuiDrawer* drawer, uint32_t code_base, uint32_t code_size, uint32_t image_base,
                    uint32_t image_size, std::function<void(std::function<void()>)> post_ui) {
  if (!REXCVAR_GET(wwe2k14_inspector) || !drawer) return;
  State& s = S();
  s.drawer = drawer;
  s.code_base = code_base;
  s.code_size = code_size;
  s.image_base = image_base;
  s.image_size = image_size;
  s.post_ui = std::move(post_ui);
  rex::ui::RegisterBind("bind_wwe2k14_inspector", "F6", "Toggle the wwe2k14 inspector", [] {
    if (S().dialog) {
      Hide();
    } else {
      Show();
    }
  });
  // Set in-run (input script `cvar`, settings panel, console): show/hide on the UI thread. The callback runs under
  // the cvar registry lock on the setter's thread, so it only posts.
  rex::cvar::RegisterChangeCallback("wwe2k14_inspector_show", [](std::string_view, std::string_view value) {
    const bool show = value == "true" || value == "1";
    if (S().post_ui) S().post_ui([show] { show ? Show() : Hide(); });
  });
  REXLOG_INFO("[wwe2k14][inspector] installed: F6 (bind_wwe2k14_inspector) shows/hides it; code range 0x{:08X}+0x{:X}, "
              "image 0x{:08X}+0x{:X}",
              code_base, code_size, image_base, image_size);
  if (REXCVAR_GET(wwe2k14_inspector_show)) Show();
}

// From ReXApp::OnShutdown (UI thread), before ReXGlue destroys the ImGui drawer.
inline void Shutdown() {
  State& s = S();
  if (!s.drawer) return;
  rex::ui::UnregisterBind("bind_wwe2k14_inspector");
  rex::cvar::UnregisterChangeCallbacks("wwe2k14_inspector_show");
  Hide();
  s.drawer = nullptr;
  s.post_ui = nullptr;
}

}  // namespace wwe2k14::inspector
