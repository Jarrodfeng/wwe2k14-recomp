#pragma once
// wwe2k14 - S110 BUSWAIT: a route's view of the event bus (src/event_bus.h). Pure: no SDK, no game types (tools/bus/event_wait_test.cpp
// compiles it). docs/research/s110/BUSWAIT.md.
//   writer  src/event_bus.h Emit, on whatever thread emits (the owner update's Tick, the file-open path, the replay hooks of
//           src/replay_fx_skip.h): Global().Add(frame, kind, fields) gives the event the next sequence number under the log's mutex.
//   reader  src/input_script.h on the VdSwap thread (under InputScript::mutex_): Poll(scan, filter) once a frame / Last(key) / Newest().
//           A wait's scan point starts at its mark and moves past every event it has examined, so each event is read once and a long
//           wait over more than kRing events loses only events it never read; when the ring dropped such events the wait is told how many
//           (src/input_script.h fails the wait then: the event may have been among them).
// The ring keeps the newest kRing events. A route looks once a frame, from its mark (the seq of its last matched event or its last
// input), at every event after it in order: an event that fired between two of its frames, or several in one frame, is never missed;
// an event is never matched twice (the mark moves to the matched seq). Lock order: InputScript::mutex_ -> Log::mu_; Emit takes only mu_.
// g_wanted: the route parser sets it when a script names a bus wait, and src/event_bus.h On() is then true without --wwe2k14_events.
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace wwe2k14::event_wait {

inline std::atomic<bool> g_wanted{false};

struct Event {
  uint64_t seq = 0, frame = 0;
  std::string kind, fields;
};

// KEY OP VALUE; op is one of = != >= <= > <.
struct Cond {
  std::string key, op, value;
};
struct Filter {
  std::string kind;
  std::vector<Cond> conds;
  std::string text;  // as the logs show it: "match what=finish finish_name=pinfall"
};

// The value of `key` at the top level of a flat JSON fragment (`"a": 1, "b": "x y", "w": [{...}]`): a string unquoted (\" and \\ undone),
// a number / true / false as written, an array / object as its raw text. False when the key is not there.
inline bool FieldValue(const std::string& f, const std::string& key, std::string& out) {
  size_t i = 0;
  const size_t n = f.size();
  auto skip = [&](const char* set) {
    while (i < n && std::strchr(set, f[i]) != nullptr && f[i] != '\0') ++i;
  };
  auto quoted = [&](std::string& s) {
    if (i >= n || f[i] != '"') return false;
    ++i;
    s.clear();
    while (i < n && f[i] != '"') {
      if (f[i] == '\\' && i + 1 < n) ++i;
      s += f[i++];
    }
    if (i >= n) return false;
    ++i;
    return true;
  };
  for (;;) {
    skip(" ,\t\r\n");
    if (i >= n) return false;
    std::string k, v;
    if (!quoted(k)) return false;
    skip(" :\t");
    if (i < n && f[i] == '"') {
      if (!quoted(v)) return false;
    } else if (i < n && (f[i] == '[' || f[i] == '{')) {
      const size_t s0 = i;
      int depth = 0;
      while (i < n) {
        if (f[i] == '"') {
          std::string skipped;
          if (!quoted(skipped)) return false;
          continue;
        }
        if (f[i] == '[' || f[i] == '{') ++depth;
        if (f[i] == ']' || f[i] == '}') {
          if (--depth == 0) {
            ++i;
            break;
          }
        }
        ++i;
      }
      v = f.substr(s0, i - s0);
    } else {
      const size_t s0 = i;
      while (i < n && f[i] != ',') ++i;
      v = f.substr(s0, i - s0);
      while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
    }
    if (k == key) {
      out = v;
      return true;
    }
  }
}

inline bool ParseInt(const std::string& s, int64_t& v) {
  if (s.empty()) return false;
  char* end = nullptr;
  v = std::strtoll(s.c_str(), &end, 10);
  return end && *end == '\0';
}

// Strings compare without case, and '_' in either side matches a space ("last_one_left" = "last one left": a route word has no spaces).
inline std::string Norm(const std::string& s) {
  std::string o;
  for (char c : s) o += c == '_' ? ' ' : char(std::tolower(static_cast<unsigned char>(c)));
  return o;
}

inline bool CondHolds(const Cond& c, const std::string& fields) {
  std::string v;
  if (!FieldValue(fields, c.key, v)) return false;
  int64_t a = 0, b = 0;
  if (ParseInt(v, a) && ParseInt(c.value, b)) {
    if (c.op == "=") return a == b;
    if (c.op == "!=") return a != b;
    if (c.op == ">=") return a >= b;
    if (c.op == "<=") return a <= b;
    if (c.op == ">") return a > b;
    if (c.op == "<") return a < b;
    return false;
  }
  if (c.op == "=") return Norm(v) == Norm(c.value);
  if (c.op == "!=") return Norm(v) != Norm(c.value);
  return false;  // an order test on a word
}

inline bool Matches(const Filter& f, const Event& e) {
  if (e.kind != f.kind) return false;
  for (const Cond& c : f.conds) {
    if (!CondHolds(c, e.fields)) return false;
  }
  return true;
}

// The bus's kinds (src/event_bus.h) and the "what" words of kind match (src/event_bus.h MatchTick / Tick, src/event_bus_rumble.h): a
// route may name a what as the kind, `wait_event pin` = `wait_event match what=pin`.
inline constexpr const char* kKinds[] = {"match", "move", "entrance", "seq", "screen", "exit", "msgbox",
                                         "state", "load", "render", "break", "replay"};
inline constexpr const char* kMatchWhats[] = {"pin", "count", "kickout", "finish", "fall", "eliminated", "clock_started", "dq_count",
                                              "count_out_count", "scene", "entered", "entered_id", "resync"};  // fall: S110 ELIMFIN

template <size_t N>
inline bool In(const char* const (&list)[N], const std::string& w) {
  for (const char* x : list) {
    if (w == x) return true;
  }
  return false;
}

// The field a bare word after the kind names ("" = the kind takes none): match / render / break carry "what", replay "state".
inline const char* BareKey(const std::string& kind) {
  if (kind == "match" || kind == "render" || kind == "break") return "what";
  if (kind == "replay") return "state";
  return "";
}

struct Spec {
  Filter filter;
  int64_t timeout = -1;  // -1 = not given
  bool optional = false;
  Filter until;          // S110 SCENESRC: the end clause (kind "" = none)
};

// The words after `wait_event` / `log_event`: KIND [WORD] [KEY OP VALUE ...] [TIMEOUT] [until KIND [WORD] [KEY OP VALUE ...]] [optional].
// S110 SCENESRC: `until ...` (the end clause) is cut off first and read as a spec of its own (no timeout, no optional, no second until):
// the wait ENDS (fails, or warns when optional) at the first event that matches it and not the main spec (both = the match).
inline bool ParseSpec(const std::vector<std::string>& words, Spec& out, std::string& why) {
  out = Spec{};
  const auto cut = std::find(words.begin(), words.end(), std::string("until"));
  if (cut != words.end()) {
    std::vector<std::string> head(words.begin(), cut), tail(cut + 1, words.end());
    if (std::find(head.begin(), head.end(), std::string("optional")) != head.end()) {
      why = "'optional' must be the last word (after the until clause)";
      return false;
    }
    if (!tail.empty() && tail.back() == "optional") {  // the line's last word: it belongs to the wait, not to its end clause
      tail.pop_back();
      head.push_back("optional");
    }
    if (tail.empty()) {
      why = "'until' needs an event kind: until KIND [WORD] [KEY=VALUE ...]";
      return false;
    }
    Spec end;
    if (!ParseSpec(tail, end, why)) {
      why = "until: " + why;
      return false;
    }
    if (end.timeout >= 0 || end.optional || !end.until.kind.empty()) {
      why = "until KIND [WORD] [KEY=VALUE ...] takes no timeout, 'optional' or second 'until' of its own (the timeout goes before 'until', "
            "'optional' last)";
      return false;
    }
    if (!ParseSpec(head, out, why)) return false;
    out.until = end.filter;
    return true;
  }
  if (words.empty()) {
    why = "expected an event kind";
    return false;
  }
  std::string kind = words[0];
  for (char& c : kind) c = char(std::tolower(static_cast<unsigned char>(c)));
  Filter& f = out.filter;
  if (In(kMatchWhats, kind)) {
    f.kind = "match";
    f.conds.push_back({"what", "=", kind});
  } else if (In(kKinds, kind)) {
    f.kind = kind;
  } else {
    why = "unknown event kind '" + words[0] + "' (match move entrance seq screen exit msgbox state load render break replay, or a match what: "
          "pin count kickout finish eliminated clock_started ...)";
    return false;
  }
  bool bare_used = !f.conds.empty();
  for (size_t i = 1; i < words.size(); ++i) {
    const std::string& w = words[i];
    const bool last = i + 1 == words.size();
    if (w == "optional") {
      if (!last) {
        why = "'optional' must be the last word";
        return false;
      }
      out.optional = true;
      continue;
    }
    int64_t t = 0;
    if (ParseInt(w, t) && w[0] != '-') {
      const bool then_optional = i + 2 == words.size() && words[i + 1] == "optional";
      if (!(last || then_optional) || t <= 0 || out.timeout >= 0) {
        why = "the timeout (frames, > 0) goes after the fields, before 'optional': '" + w + "'";
        return false;
      }
      out.timeout = t;
      continue;
    }
    const size_t at = w.find_first_of("=!<>");
    if (at != std::string::npos) {
      std::string op(1, w[at]);
      if (at + 1 < w.size() && w[at + 1] == '=' && w[at] != '=') op += '=';
      if (op == "!") {
        why = "expected != in '" + w + "'";
        return false;
      }
      const std::string key = w.substr(0, at), value = w.substr(at + op.size());
      if (key.empty() || value.empty() || out.timeout >= 0) {
        why = out.timeout >= 0 ? "a field after the timeout: '" + w + "'" : "expected KEY" + op + "VALUE, got '" + w + "'";
        return false;
      }
      f.conds.push_back({key, op, value});
      continue;
    }
    const std::string bk = BareKey(f.kind);
    if (bare_used || bk.empty() || i != 1) {
      why = "unexpected word '" + w + "' (fields are KEY=VALUE" + (bk.empty() ? std::string(")") : "; one bare word = " + bk + "=WORD)");
      return false;
    }
    f.conds.push_back({bk, "=", w});
    bare_used = true;
  }
  f.text = f.kind;
  for (const Cond& c : f.conds) f.text += " " + c.key + c.op + c.value;
  return true;
}

// The key Log::Last files an event under: its kind, and kind:WORD for an event with a "what" or a "state" (`log_event finish` =
// "match:finish", `log_event replay start` = "replay:start").
inline std::string LastKey(const Filter& f) {
  for (const Cond& c : f.conds) {
    if ((c.key == "what" || c.key == "state") && c.op == "=") return f.kind + ":" + c.value;
  }
  return f.kind;
}

class Log {
 public:
  static constexpr size_t kRing = 512;

  uint64_t Add(uint64_t frame, const std::string& kind, const std::string& fields) {
    std::lock_guard<std::mutex> lock(mu_);
    const uint64_t s = ++newest_;
    Event& e = ring_[s % kRing];
    e.seq = s;
    e.frame = frame;
    e.kind = kind;
    e.fields = fields;
    last_[kind] = e;
    for (const char* k : {"what", "state"}) {
      if (std::string w; FieldValue(fields, k, w)) last_[kind + ":" + w] = e;
    }
    return s;
  }

  uint64_t Newest() {
    std::lock_guard<std::mutex> lock(mu_);
    return newest_;
  }

  // What a poll found: nothing yet, an event that matches the wait, or (S110 SCENESRC) one that matches its until clause and not the wait.
  enum class Hit { kNone, kMatch, kEnd };

  // The first event with seq > after that matches f. lost = how many events after `after` the ring no longer holds (0 unless the
  // reader fell more than kRing events behind); the search starts at the oldest one kept.
  bool FindAfter(uint64_t after, const Filter& f, Event& out, uint64_t& lost) {
    std::lock_guard<std::mutex> lock(mu_);
    return FindLocked(after, f, nullptr, out, lost) == Hit::kMatch;
  }

  // A wait's poll: the first event after scan, in seq order, that matches f (kMatch) or, with an until clause, *until (kEnd; an event that
  // matches both is kMatch); scan = that event's seq, or the newest when neither came (every event up to it was examined once, against
  // both). lost = events after scan that the ring dropped before they were examined.
  Hit Poll(uint64_t& scan, const Filter& f, const Filter* until, Event& out, uint64_t& lost) {
    std::lock_guard<std::mutex> lock(mu_);
    const Hit hit = FindLocked(scan, f, until, out, lost);
    if (hit != Hit::kNone) scan = out.seq;
    if (hit == Hit::kNone) scan = newest_;
    return hit;
  }
  bool Poll(uint64_t& scan, const Filter& f, Event& out, uint64_t& lost) { return Poll(scan, f, nullptr, out, lost) == Hit::kMatch; }

  // The newest event filed under key (LastKey), however old.
  bool Last(const std::string& key, Event& out) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = last_.find(key);
    if (it == last_.end()) return false;
    out = it->second;
    return true;
  }

 private:
  Hit FindLocked(uint64_t after, const Filter& f, const Filter* until, Event& out, uint64_t& lost) {
    lost = 0;
    uint64_t from = after + 1;
    const uint64_t oldest = newest_ >= kRing ? newest_ - kRing + 1 : 1;
    if (from < oldest) {
      lost = oldest - from;
      from = oldest;
    }
    for (uint64_t s = from; s <= newest_; ++s) {
      const Event& e = ring_[s % kRing];
      if (Matches(f, e)) {
        out = e;
        return Hit::kMatch;
      }
      if (until && Matches(*until, e)) {
        out = e;
        return Hit::kEnd;
      }
    }
    return Hit::kNone;
  }

  std::mutex mu_;
  std::array<Event, kRing> ring_{};
  uint64_t newest_ = 0;
  std::map<std::string, Event> last_;
};

inline Log& Global() {
  static Log log;
  return log;
}

}  // namespace wwe2k14::event_wait
