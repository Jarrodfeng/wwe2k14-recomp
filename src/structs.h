// wwe2k14 - typed views of known guest objects (Session 30, #5; test harness only).
//
// tools/input_scripts/structs.toml (found like states.toml: beside the script, else tools/input_scripts above it) names
// objects by a base chain and lists their fields as "+OFFSET TYPE | meaning | evidence". The input-script command
// `dump_struct NAME [ADDR]` logs every field (one line each); live `py tools\live\live.py struct NAME [ADDR]` sends it;
// the bookmark bundle (src/bookmark.h) dumps every struct whose base reads as a non-null pointer. Every read is checked
// (ReadGuestBytes: an unreadable field says so instead of faulting).

#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "game_state.h"

namespace wwe2k14::structs {

using game_state_detail::ParseUnsigned;
using game_state_detail::StripComment;
using game_state_detail::Trim;

struct Field {
  std::string name;
  uint32_t offset = 0;
  std::string type;  // u8 u16 u32 s8 s16 s32 f32 ptr str
  std::string meaning;
  std::string evidence;
};

struct Def {
  std::string name;
  GuestState base;  // its value = the object's address
  std::string meaning;
  std::vector<Field> fields;
};

inline std::string FindStructsFile(const std::string& near_path) {
  const std::string states = FindStatesFile(near_path);
  if (states.empty()) return {};
  const auto p = std::filesystem::path(states).parent_path() / "structs.toml";
  std::error_code ec;
  return std::filesystem::is_regular_file(p, ec) ? p.string() : std::string();
}

inline std::string Unquote(std::string v) {
  v = Trim(v);
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
  return v;
}

inline bool ParseField(const std::string& name, const std::string& value, Field& f, std::string& error) {
  // "+OFFSET TYPE | meaning | evidence"
  std::vector<std::string> parts;
  size_t start = 0;
  for (;;) {
    const size_t bar = value.find('|', start);
    parts.push_back(Trim(value.substr(start, bar == std::string::npos ? std::string::npos : bar - start)));
    if (bar == std::string::npos) break;
    start = bar + 1;
  }
  std::istringstream head(parts[0]);
  std::string off, type;
  head >> off >> type;
  if (off.empty() || off[0] != '+' || type.empty()) {
    error = "field " + name + ": expected \"+OFFSET TYPE | meaning | evidence\"";
    return false;
  }
  uint32_t o = 0;
  if (!ParseUnsigned(off.substr(1), &o)) {
    error = "field " + name + ": bad offset " + off;
    return false;
  }
  static const char* kTypes[] = {"u8", "u16", "u32", "s8", "s16", "s32", "f32", "ptr", "str"};
  if (std::find(std::begin(kTypes), std::end(kTypes), type) == std::end(kTypes)) {
    error = "field " + name + ": unknown type " + type;
    return false;
  }
  f.name = name;
  f.offset = o;
  f.type = type;
  f.meaning = parts.size() > 1 ? parts[1] : "";
  f.evidence = parts.size() > 2 ? parts[2] : "";
  return true;
}

inline bool Load(const std::string& path, std::vector<Def>& out, std::string& error) {
  std::ifstream in(path);
  if (!in) {
    error = "can't open " + path;
    return false;
  }
  out.clear();
  int n = 0;
  for (std::string line; std::getline(in, line);) {
    ++n;
    line = Trim(StripComment(line));
    if (line.empty()) continue;
    if (line.front() == '[' && line.back() == ']') {
      out.push_back({});
      out.back().name = Trim(line.substr(1, line.size() - 2));
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos || out.empty()) {
      error = fmt::format("{} line {}: expected [Name] or key = \"value\"", path, n);
      return false;
    }
    const std::string key = Trim(line.substr(0, eq)), value = Unquote(line.substr(eq + 1));
    Def& d = out.back();
    if (key == "base") {
      if (!ParseStateChain(value, d.base)) {
        error = fmt::format("{} line {}: bad base chain '{}'", path, n, value);
        return false;
      }
      d.base.name = d.name;
      d.base.size = 4;
    } else if (key == "meaning") {
      d.meaning = value;
    } else if (key == "evidence" || key == "confidence" || key == "source") {
      // documentation only
    } else {
      Field f;
      if (!ParseField(key, value, f, error)) {
        error = fmt::format("{} line {}: {}", path, n, error);
        return false;
      }
      d.fields.push_back(std::move(f));
    }
  }
  for (const Def& d : out) {
    if (d.base.address == 0) {
      error = fmt::format("{}: struct {} has no base", path, d.name);
      return false;
    }
  }
  return true;
}

// The object's address from its base chain (0 = unreadable or null).
inline uint32_t BaseAddress(const Def& d) {
  uint32_t v = 0;
  return ReadGuestState(d.base, &v) ? v : 0;
}

inline std::string ReadText(uint32_t addr) {
  std::string s;
  for (uint32_t i = 0; i < 64; i += 4) {
    uint32_t w = 0;
    if (!ReadGuestBytes(addr + i, 4, &w)) return s.empty() ? "(unreadable)" : s;
    for (int b = 3; b >= 0; --b) {
      const char c = char((w >> (8 * b)) & 0xFF);
      if (!c) return s;
      s += (c < 32 || c > 126) ? '?' : c;
    }
  }
  return s + "...";
}

inline std::string FieldValue(uint32_t base, const Field& f) {
  const uint32_t a = base + f.offset;
  const uint32_t size = (f.type == "u8" || f.type == "s8") ? 1 : (f.type == "u16" || f.type == "s16") ? 2 : 4;
  uint32_t raw = 0;
  if (!ReadGuestBytes(a, size, &raw)) return "(unreadable)";
  if (f.type == "u8" || f.type == "u16" || f.type == "u32") return fmt::format("{} (0x{:X})", raw, raw);
  if (f.type == "s8") return std::to_string(int8_t(raw));
  if (f.type == "s16") return std::to_string(int16_t(raw));
  if (f.type == "s32") return std::to_string(int32_t(raw));
  if (f.type == "f32") return fmt::format("{:g}", std::bit_cast<float>(raw));
  if (f.type == "ptr") return fmt::format("0x{:08X}", raw);
  return raw ? fmt::format("0x{:08X} \"{}\"", raw, ReadText(raw)) : "0x00000000 (null)";  // str
}

// Lines for one struct: a header, then "  name +OFF type = value  (meaning)".
inline std::vector<std::string> Dump(const Def& d, uint32_t addr) {
  std::vector<std::string> lines;
  if (!addr) {
    lines.push_back(fmt::format("struct {}: base {} unreadable or null", d.name, d.base.chain));
    return lines;
  }
  lines.push_back(fmt::format("struct {} @ 0x{:08X} (base {}): {}", d.name, addr, d.base.chain, d.meaning));
  for (const Field& f : d.fields) {
    lines.push_back(fmt::format("  {} +{} {} = {}  ({})", f.name, f.offset, f.type, FieldValue(addr, f), f.meaning));
  }
  return lines;
}

}  // namespace wwe2k14::structs
