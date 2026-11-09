// Tiny JSON reader for the viewer's command lines (objects, arrays, strings, numbers, bools, null).
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace jl {
struct Value {
  enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
  double n = 0;
  bool b = false;
  std::string s;
  std::vector<Value> a;           // array items
  std::vector<std::string> keys;  // object keys (parallel to vals)
  std::vector<Value> vals;
  const Value* get(const char* k) const {
    for (size_t i = 0; i < keys.size(); i++)
      if (keys[i] == k) return &vals[i];
    return nullptr;
  }
  std::string str(const char* k, const std::string& d = "") const {
    auto* v = get(k);
    return v && v->t == Str ? v->s : d;
  }
  double num(const char* k, double d) const {
    auto* v = get(k);
    return v && v->t == Num ? v->n : d;
  }
  bool has(const char* k) const { return get(k) != nullptr; }
};

struct Parser {
  const std::string& s;
  size_t p = 0;
  std::string err;
  explicit Parser(const std::string& str) : s(str) {}
  void ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) p++; }
  bool fail(const char* m) { if (err.empty()) err = m; return false; }
  bool lit(const char* w) {
    size_t n = strlen(w);
    if (s.compare(p, n, w) == 0) { p += n; return true; }
    return false;
  }
  static void utf8(std::string& o, unsigned c) {
    if (c < 0x80) o += (char)c;
    else if (c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 63)); }
    else { o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 63)); o += (char)(0x80 | (c & 63)); }
  }
  bool str(std::string& o) {
    if (p >= s.size() || s[p] != '"') return fail("expected string");
    p++;
    while (p < s.size() && s[p] != '"') {
      char c = s[p++];
      if (c != '\\') { o += c; continue; }
      if (p >= s.size()) return fail("bad escape");
      char e = s[p++];
      switch (e) {
        case 'n': o += '\n'; break;
        case 't': o += '\t'; break;
        case 'r': o += '\r'; break;
        case 'b': o += '\b'; break;
        case 'f': o += '\f'; break;
        case 'u': {
          if (p + 4 > s.size()) return fail("bad \\u");
          unsigned c2 = (unsigned)strtoul(s.substr(p, 4).c_str(), nullptr, 16);
          p += 4;
          utf8(o, c2);
          break;
        }
        default: o += e;
      }
    }
    if (p >= s.size()) return fail("unterminated string");
    p++;
    return true;
  }
  bool value(Value& v, int depth = 0) {
    if (depth > 32) return fail("too deep");
    ws();
    if (p >= s.size()) return fail("unexpected end");
    char c = s[p];
    if (c == '{') {
      v.t = Value::Obj;
      p++;
      ws();
      if (p < s.size() && s[p] == '}') { p++; return true; }
      for (;;) {
        ws();
        std::string k;
        if (!str(k)) return false;
        ws();
        if (p >= s.size() || s[p] != ':') return fail("expected :");
        p++;
        Value x;
        if (!value(x, depth + 1)) return false;
        v.keys.push_back(k);
        v.vals.push_back(std::move(x));
        ws();
        if (p < s.size() && s[p] == ',') { p++; continue; }
        if (p < s.size() && s[p] == '}') { p++; return true; }
        return fail("expected , or }");
      }
    }
    if (c == '[') {
      v.t = Value::Arr;
      p++;
      ws();
      if (p < s.size() && s[p] == ']') { p++; return true; }
      for (;;) {
        Value x;
        if (!value(x, depth + 1)) return false;
        v.a.push_back(std::move(x));
        ws();
        if (p < s.size() && s[p] == ',') { p++; continue; }
        if (p < s.size() && s[p] == ']') { p++; return true; }
        return fail("expected , or ]");
      }
    }
    if (c == '"') { v.t = Value::Str; return str(v.s); }
    if (lit("true")) { v.t = Value::Bool; v.b = true; return true; }
    if (lit("false")) { v.t = Value::Bool; v.b = false; return true; }
    if (lit("null")) { v.t = Value::Null; return true; }
    char* end = nullptr;
    double d = strtod(s.c_str() + p, &end);
    if (end == s.c_str() + p) return fail("bad value");
    // JSON numbers only: no hex, inf or nan (strtod accepts all three)
    for (const char* q = s.c_str() + p; q < end; q++)
      if (!strchr("0123456789+-.eE", *q)) return fail("bad number");
    if (!std::isfinite(d)) return fail("number is not finite");
    p = (size_t)(end - s.c_str());
    v.t = Value::Num;
    v.n = d;
    return true;
  }
};

inline bool Parse(const std::string& s, Value& out, std::string& err) {
  Parser ps(s);
  if (!ps.value(out)) { err = ps.err; return false; }
  ps.ws();
  if (ps.p != s.size()) { err = "trailing characters"; return false; }
  return true;
}

inline std::string Esc(const std::string& in) {
  std::string o;
  for (unsigned char c : in) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20 || c >= 0x80) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
  }
  return o;
}
}  // namespace jl
