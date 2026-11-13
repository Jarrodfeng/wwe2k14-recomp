// wwe2k14 - the `!wm30` argument rule, in a header of its own with no game or SDK includes (S101 goto, review fix).
//
// Why a header: tools/goto/args_test.cpp compiles this exact file and feeds it the strings tools/goto/resolve.py builds
// (`arm_arg`), so the parse rule the game runs is the one the selftest checks (the earlier selftest only grepped the source text).
//
//   N          match index, digits only, 0-45
//   N CODE     CODE an SCEX moment code, digits only, 10000-65535 (the game keeps 16 bits: sub_82AA2FF0, sub_82C535B8)
//   off        disarm
// Anything else (a third word, letters after a number, a sign, a code over 65535) is an error, never a quiet partial arm.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wwe2k14::wm30 {

struct ArmArgs {
  bool off = false;
  int n = -1;
  int code = 0;  // 0 = the match's own first code (X00)
};

constexpr int kMaxMatch = 45;
constexpr int kMinCode = 10000;
constexpr int kMaxCode = 65535;

// The live arm is one 64-bit word so the hook reads match and code together: -1 = not armed, else (n << 32) | code.
constexpr int64_t kNotArmed = -1;
inline int64_t PackArm(int n, int code) { return (int64_t(n) << 32) | int64_t(uint32_t(code)); }
inline int ArmMatch(int64_t v) { return int(v >> 32); }
inline int ArmCode(int64_t v) { return int(v & 0xFFFFFFFF); }

inline bool AllDigits(const std::string& s, size_t max_len) {
  if (s.empty() || s.size() > max_len) return false;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

// "" on success (out is filled), else the error text.
inline std::string ParseArmArgs(const std::string& args, ArmArgs& out) {
  out = ArmArgs{};
  std::vector<std::string> w;
  std::string cur;
  for (char c : args) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      if (!cur.empty()) w.push_back(cur), cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) w.push_back(cur);
  const char* usage = "usage !wm30 N [CODE] (N 0-45, CODE an SCEX moment code 10000-65535) or !wm30 off";
  if (w.size() == 1 && w[0] == "off") {
    out.off = true;
    return "";
  }
  if (w.empty() || w.size() > 2) return std::string("error: ") + usage;
  if (!AllDigits(w[0], 3)) return std::string("error: match '") + w[0] + "' is not a number; " + usage;
  out.n = std::stoi(w[0]);
  if (out.n > kMaxMatch) return std::string("error: match ") + w[0] + " is over 45; " + usage;
  if (w.size() == 2) {
    if (!AllDigits(w[1], 6)) return std::string("error: code '") + w[1] + "' is not a number; " + usage;
    out.code = std::stoi(w[1]);
    if (out.code < kMinCode || out.code > kMaxCode) {
      return std::string("error: code ") + w[1] + " is outside 10000-65535 (the game keeps 16 bits); " + usage;
    }
  }
  return "";
}

}  // namespace wwe2k14::wm30
