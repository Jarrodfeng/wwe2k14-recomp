// Session 101 lane 2 (fview): a small JSON reader (for the effects' .fx.json) and a small JSON writer (for the headless outputs). No dependencies.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fview {

struct JsonValue {
  enum Type { Null, Bool, Number, String, Array, Object } type = Null;
  bool boolean = false;
  double number = 0;
  std::string string;
  std::vector<JsonValue> array;
  std::vector<std::pair<std::string, JsonValue>> object;  // in file order

  const JsonValue* Get(const char* key) const {
    if (type != Object) return nullptr;
    for (const auto& item : object)
      if (item.first == key) return &item.second;
    return nullptr;
  }
  std::string Str(const char* key, const std::string& fallback = {}) const {
    const JsonValue* v = Get(key);
    return v && v->type == String ? v->string : fallback;
  }
  double Num(const char* key, double fallback = 0) const {
    const JsonValue* v = Get(key);
    return v && v->type == Number ? v->number : fallback;
  }
  bool Flag(const char* key, bool fallback) const {
    const JsonValue* v = Get(key);
    return v && v->type == Bool ? v->boolean : fallback;
  }
};

class JsonParser {
 public:
  JsonParser(const char* begin, const char* end) : p_(begin), end_(end) {}
  bool Parse(JsonValue& out) {
    if (!Value(out)) return false;
    Skip();
    return p_ == end_;
  }

 private:
  void Skip() {
    while (p_ < end_ && (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t')) ++p_;
  }
  bool Literal(const char* text) {
    const char* q = p_;
    while (*text) {
      if (q >= end_ || *q != *text) return false;
      ++q, ++text;
    }
    p_ = q;
    return true;
  }
  bool Str(std::string& out) {
    if (p_ >= end_ || *p_ != '"') return false;
    ++p_;
    out.clear();
    while (p_ < end_ && *p_ != '"') {
      if (*p_ == '\\' && p_ + 1 < end_) {
        ++p_;
        switch (*p_) {
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'r': out += '\r'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'u': {
            unsigned code = 0;
            for (int i = 0; i < 4 && p_ + 1 < end_; ++i) {
              ++p_;
              code = code * 16 + unsigned(*p_ >= 'a' ? *p_ - 'a' + 10 : *p_ >= 'A' ? *p_ - 'A' + 10 : *p_ - '0');
            }
            if (code < 0x80) out += char(code);
            else if (code < 0x800) out += char(0xC0 | (code >> 6)), out += char(0x80 | (code & 63));
            else out += char(0xE0 | (code >> 12)), out += char(0x80 | ((code >> 6) & 63)), out += char(0x80 | (code & 63));
            break;
          }
          default: out += *p_; break;
        }
        ++p_;
      } else {
        out += *p_++;
      }
    }
    if (p_ >= end_) return false;
    ++p_;
    return true;
  }
  bool Value(JsonValue& out) {
    Skip();
    if (p_ >= end_) return false;
    switch (*p_) {
      case '{': {
        ++p_;
        out.type = JsonValue::Object;
        Skip();
        if (p_ < end_ && *p_ == '}') return ++p_, true;
        for (;;) {
          Skip();
          std::string key;
          if (!Str(key)) return false;
          Skip();
          if (p_ >= end_ || *p_ != ':') return false;
          ++p_;
          out.object.emplace_back(std::move(key), JsonValue{});
          if (!Value(out.object.back().second)) return false;
          Skip();
          if (p_ < end_ && *p_ == ',') {
            ++p_;
            continue;
          }
          if (p_ < end_ && *p_ == '}') return ++p_, true;
          return false;
        }
      }
      case '[': {
        ++p_;
        out.type = JsonValue::Array;
        Skip();
        if (p_ < end_ && *p_ == ']') return ++p_, true;
        for (;;) {
          out.array.emplace_back();
          if (!Value(out.array.back())) return false;
          Skip();
          if (p_ < end_ && *p_ == ',') {
            ++p_;
            continue;
          }
          if (p_ < end_ && *p_ == ']') return ++p_, true;
          return false;
        }
      }
      case '"': out.type = JsonValue::String; return Str(out.string);
      case 't': out.type = JsonValue::Bool; out.boolean = true; return Literal("true");
      case 'f': out.type = JsonValue::Bool; out.boolean = false; return Literal("false");
      case 'n': out.type = JsonValue::Null; return Literal("null");
      default: {
        char* stop = nullptr;
        out.type = JsonValue::Number;
        out.number = std::strtod(p_, &stop);
        if (stop == p_) return false;
        p_ = stop;
        return true;
      }
    }
  }
  const char* p_;
  const char* end_;
};

inline bool ParseJson(const std::string& text, JsonValue& out) { return JsonParser(text.data(), text.data() + text.size()).Parse(out); }

// A writer that keeps commas straight. Usage: w.Begin('{'); w.Key("a"); w.Int(1); w.End();
class JsonWriter {
 public:
  void Begin(char open) {
    Sep();
    out_ += open;
    stack_.push_back(open == '{' ? '}' : ']');
    first_ = true;
  }
  void End() {
    out_ += stack_.back();
    stack_.pop_back();
    first_ = false;
  }
  void Key(const char* key) {
    Sep();
    Quote(key);
    out_ += ':';
    first_ = true;  // (the value follows without a comma)
  }
  void Str(const std::string& v) { Sep(), Quote(v); }
  void Int(long long v) { Sep(), out_ += std::to_string(v); }
  void Bool(bool v) { Sep(), out_ += v ? "true" : "false"; }
  void Num(double v) {
    Sep();
    if (!std::isfinite(v)) {
      out_ += "null";
      return;
    }
    char text[40];
    std::snprintf(text, sizeof(text), "%.9g", v);
    out_ += text;
  }
  void Null() { Sep(), out_ += "null"; }
  const std::string& text() const { return out_; }

 private:
  void Sep() {
    if (!first_ && !out_.empty()) out_ += ',';
    first_ = false;
  }
  void Quote(const std::string& v) {
    out_ += '"';
    for (unsigned char c : v) {
      switch (c) {
        case '"': out_ += "\\\""; break;
        case '\\': out_ += "\\\\"; break;
        case '\n': out_ += "\\n"; break;
        case '\r': out_ += "\\r"; break;
        case '\t': out_ += "\\t"; break;
        default:
          if (c < 0x20) {
            char esc[8];
            std::snprintf(esc, sizeof(esc), "\\u%04x", c);
            out_ += esc;
          } else {
            out_ += char(c);
          }
      }
    }
    out_ += '"';
  }
  std::string out_;
  std::vector<char> stack_;
  bool first_ = true;
};

}  // namespace fview
