// S102 M7: --wwe2k14_ab_flip (default empty = off): an in-run A/B. The pure part: the spec parser, the PerFrame / armed / bool checks and the schedule. No game or SDK
// includes (tools\ab_flip\ab_flip_test.cpp compiles it with rdr_switches.h alone); src\ab_flip.h is the cvar glue called from frame_stats.h.
//
// Why: on the Surface Pro 6 the CPU and GPU share one power budget that drifts, so two separate runs differ by a whole-run offset (R19: round 3's B run was
// 2.46 ms of gpu_ms slower as a whole). One run that flips the switches on a fixed schedule gives every phase the same power, heat and GPU clock, interleaved
// in time; tools\surface\flip_line.py cuts the run into phases and compares them. A job is TWO runs with complementary schedules so every frame of the
// route is measured both ways (docs\renderer\IN_RUN_AB.md).
//
// SPEC (one string, no ';' ',' '|' ' ' so it survives bench_args.txt, SceneFlags and the command line; the cvar parser splits at the FIRST '=' only):
//   period=100/settle=15/anchor=2/start=0/off=NAME=V+NAME=V/on=NAME=V+NAME=V[/order=off:on][/first=on]
//   period  guest frames per phase (>= 2, default 300)            settle  frames after each flip the reader drops (< period, default 30)
//   anchor  a ROUTE MARK id (the input script's `mark N`, 1..63): phase 0 starts at the guest frame where that mark fired + start. Content follows the route's marks,
//           not the absolute guest frame (the tron mark fell 77 frames apart across 12 runs), so a job that pairs two runs MUST anchor. Before the anchor the game
//           runs the REFERENCE phase's values (set at frame 2). 0 / absent = no anchor: start is an absolute guest frame (default 2).
//   start   offset in frames from the anchor (with anchor), else the absolute first guest frame of phase 0 (default 2: frame 1's switch refresh is silent)
//   <phase> any other key is a phase: a name (letters / digits, 1-12 chars) and its settings NAME=V joined by '+'. At least two phases; every phase sets the same switches.
//   order   the phase cycle, names joined by ':' ; default = the phases in the order written. A phase not in it is never entered.
//   first   the phase the cycle STARTS with (default: the first of the order). The second run of a pair adds /first=on.
//   The reader compares every phase against the REFERENCE = the FIRST phase WRITTEN (`off=...` first); the start line logs it.
// Phase k covers guest frames base + k*period .. base + (k+1)*period - 1 (base = anchor frame + start, or start) and is phase order[k % order.size()].
// ONLY switches marked PerFrame in rdr_switches.h's table are allowed (a start-only switch cannot change mid-run; a name outside the table has no refresh and no
// reader acknowledgement). A bool setting takes 0 / 1 / true / false only. A switch that only works when ARMED at start (kArmed below) must already hold the
// non-reference phase's value on the command line (the bench passes it). Any problem refuses the WHOLE spec: nothing is ever applied.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "renderer_adapter/rdr_switches.h"

namespace ab_flip {

// PerFrame in the table, but the cvar is also taken at START (a device feature, a pipeline, a LiveOptions copy): flipping it mid-run does nothing unless the start value
// already arms it. tools\ab_flip\audit_perframe.py lists every PerFrame row with a cvar read outside the table; docs\renderer\IN_RUN_AB.md carries the audited result.
inline const char* const kArmed[] = {
    "wwe2k14_rdr_prelight_layered",   // reader.h takes the start value; device.cpp enables multiview only then
    "wwe2k14_rdr_prelight_compute",   // reader.h takes the start value (L1b review F2)
    "wwe2k14_rdr_card_cut",           // reader.h arms the thin probe at start
    "wwe2k14_rdr_max_queued",         // reader.h LiveOptions copy at start
    "wwe2k14_rdr_skip_keeps_state",   // reader.h LiveOptions copy at start
    "wwe2k14_rdr_skip_prune",         // reader.h LiveOptions copy at start
    "wwe2k14_rdr_perframe_by_image",  // reader.h LiveOptions copy at start
    "wwe2k14_rdr_catch_up_state",     // reader.h LiveOptions copy at start
};

struct Phase {
  std::string name;
  std::vector<std::pair<std::string, std::string>> sets;  // cvar name, value text
};

struct Spec {
  bool ok = false;
  std::string error;
  uint32_t period = 300, settle = 30, anchor = 0;
  uint64_t start = 0;
  bool start_given = false;
  std::vector<Phase> phases;   // as written
  std::vector<int> order;      // indices into phases, the cycle
};

// The environment the glue hands in (nullptr members = not checked, for the offline tests): is the cvar a bool; its current text.
struct Env {
  bool (*is_bool)(const char* name) = nullptr;
  bool (*read)(const char* name, std::string& text) = nullptr;
};

inline std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t at = 0;
  for (;;) {
    const size_t e = s.find(sep, at);
    out.push_back(s.substr(at, e == std::string::npos ? std::string::npos : e - at));
    if (e == std::string::npos) break;
    at = e + 1;
  }
  return out;
}

inline bool WordOk(const std::string& w, size_t max) {
  if (w.empty() || w.size() > max) return false;
  for (char c : w) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
  return true;
}

inline Spec Fail(Spec s, const std::string& why) {
  s.ok = false;
  s.error = why;
  return s;
}

inline bool Num(const std::string& t, uint64_t& v) {
  if (t.empty() || t.size() > 9) return false;
  v = 0;
  for (char c : t) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + uint64_t(c - '0');
  }
  return true;
}

// true / false / a number as one comparable text ("true" == "1"): the registry prints a bool as true / false, a spec may say 1.
inline std::string Norm(const std::string& t) {
  if (t == "true") return "1";
  if (t == "false") return "0";
  bool ok = false;
  const int64_t v = rdr_sw::Parse(t, &ok);
  return ok ? std::to_string((long long)v) : t;
}

inline bool IsArmed(const std::string& name) {
  for (const char* a : kArmed) if (name == a) return true;
  return false;
}

inline Spec Parse(const std::string& text, const Env* env = nullptr) {
  Spec s;
  if (text.empty()) return Fail(s, "empty spec");
  std::string order_text, first_text;
  for (const std::string& part : Split(text, '/')) {
    const size_t eq = part.find('=');
    if (eq == std::string::npos || eq == 0) return Fail(s, "'" + part + "' is not key=value");
    const std::string key = part.substr(0, eq), val = part.substr(eq + 1);
    uint64_t n = 0;
    if (key == "period" || key == "settle" || key == "start" || key == "anchor") {
      if (!Num(val, n)) return Fail(s, key + " wants a number, got '" + val + "'");
      if (key == "period") s.period = uint32_t(n);
      else if (key == "settle") s.settle = uint32_t(n);
      else if (key == "anchor") {
        if (n < 1 || n > 63) return Fail(s, "anchor wants a route mark id 1..63");
        s.anchor = uint32_t(n);
      } else {
        s.start = n;
        s.start_given = true;
      }
    } else if (key == "order") {
      order_text = val;
    } else if (key == "first") {
      first_text = val;
    } else {
      if (!WordOk(key, 12)) return Fail(s, "phase name '" + key + "' must be 1-12 letters / digits");
      for (const Phase& p : s.phases) if (p.name == key) return Fail(s, "phase '" + key + "' written twice");
      Phase ph;
      ph.name = key;
      if (val.empty()) return Fail(s, "phase '" + key + "' has no settings");
      for (const std::string& kv : Split(val, '+')) {
        const size_t e2 = kv.find('=');
        if (e2 == std::string::npos || e2 == 0 || e2 + 1 >= kv.size()) return Fail(s, "phase '" + key + "': '" + kv + "' is not NAME=VALUE");
        const std::string name = kv.substr(0, e2), value = kv.substr(e2 + 1);
        const int id = rdr_sw::Find(name.c_str());
        if (id < 0) return Fail(s, "'" + name + "' is not in the switch table (rdr_switches.h): only table switches have a per-frame refresh");
        if (rdr_sw::Infos()[id].kind == rdr_sw::Kind::kStartOnly) return Fail(s, "'" + name + "' is start-only: a mid-run change would be refused");
        bool parsed = false;
        (void)rdr_sw::Parse(value, &parsed);
        if (!parsed) return Fail(s, "'" + name + "=" + value + "': the value is not true / false / a number");
        if (env && env->is_bool && env->is_bool(name.c_str()) && value != "0" && value != "1" && value != "true" && value != "false")
          return Fail(s, "'" + name + "=" + value + "': a bool setting takes 0 / 1 / true / false only (the registry reads any other number as false)");
        for (const auto& have : ph.sets) if (have.first == name) return Fail(s, "phase '" + key + "' sets '" + name + "' twice");
        ph.sets.emplace_back(name, value);
      }
      s.phases.push_back(ph);
    }
  }
  if (s.period < 2) return Fail(s, "period must be at least 2");
  if (s.settle >= s.period) return Fail(s, "settle must be below period (nothing would be left to read)");
  if (s.phases.size() < 2) return Fail(s, "at least two phases are needed");
  if (order_text.empty()) {
    for (size_t i = 0; i < s.phases.size(); ++i) s.order.push_back(int(i));
  } else {
    for (const std::string& nm : Split(order_text, ':')) {
      int at = -1;
      for (size_t i = 0; i < s.phases.size(); ++i) if (s.phases[i].name == nm) at = int(i);
      if (at < 0) return Fail(s, "order names '" + nm + "', which is not a phase");
      s.order.push_back(at);
    }
    if (s.order.size() < 2) return Fail(s, "order needs at least two entries");
  }
  if (!first_text.empty()) {
    size_t at = s.order.size();
    for (size_t i = 0; i < s.order.size(); ++i) if (s.phases[size_t(s.order[i])].name == first_text) { at = i; break; }
    if (at == s.order.size()) return Fail(s, "first names '" + first_text + "', which is not in the cycle");
    std::vector<int> rot;
    for (size_t i = 0; i < s.order.size(); ++i) rot.push_back(s.order[(at + i) % s.order.size()]);
    s.order = rot;
  }
  // every cycle must cover the same switches in every phase, or a phase would inherit another's value (a flip that did not flip)
  const Phase& ref = s.phases[0];
  for (int idx : s.order) {
    const Phase& p = s.phases[size_t(idx)];
    if (p.sets.size() != ref.sets.size()) return Fail(s, "phase '" + p.name + "' sets a different number of switches than '" + ref.name + "' (every phase sets every switch)");
    for (const auto& kv : p.sets) {
      bool found = false;
      for (const auto& r : ref.sets) found = found || r.first == kv.first;
      if (!found) return Fail(s, "phase '" + p.name + "' sets '" + kv.first + "', which '" + ref.name + "' does not (every phase sets every switch)");
    }
  }
  // armed switches: the value at START must already be the non-reference phase's (a flip of an armed switch cannot arm it mid-run)
  if (env && env->read) {
    for (const auto& kv : ref.sets) {
      if (!IsArmed(kv.first)) continue;
      std::string cur;
      if (!env->read(kv.first.c_str(), cur)) return Fail(s, "'" + kv.first + "' cannot be read");
      for (int idx : s.order) {
        const Phase& p = s.phases[size_t(idx)];
        if (&p == &ref) continue;
        for (const auto& q : p.sets) {
          if (q.first == kv.first && Norm(q.second) != Norm(kv.second) && Norm(cur) != Norm(q.second))
            return Fail(s, "'" + kv.first + "' is armed at start (reader.h / device.cpp read it once): its value on the command line is '" + cur + "', phase '" + p.name +
                               "' wants '" + q.second + "'. Start with the non-reference value (the bench does)");
        }
      }
    }
  }
  s.ok = true;
  return s;
}

// The base frame of phase 0 (needs the anchor's frame when the spec has one; 0 = the mark has not fired yet).
inline uint64_t BaseFrame(const Spec& s, uint64_t anchor_frame) {
  if (s.anchor) return anchor_frame ? anchor_frame + s.start : 0;
  return s.start_given && s.start > 2 ? s.start : 2;
}

using SetFn = bool (*)(const char* name, const char* value);
using LogFn = void (*)(const char* line);

// Runtime state, driven once per guest frame from the swap thread BEFORE the switch refresh (so the refresh of the same frame sees the new values).
struct Runner {
  Spec spec;
  bool started = false, active = false, pre_done = false, anchor_logged = false;
  uint64_t last_phase_no = ~0ull, anchor_frame = 0;
  uint32_t flips = 0, set_failures = 0;
  // the first call: parse + validate, log the start line. A refused spec is logged and the runner stays inactive for the whole run.
  void Begin(const std::string& text, uint64_t frame, LogFn log, const Env* env = nullptr) {
    started = true;
    if (text.empty()) return;
    spec = Parse(text, env);
    char line[640];
    if (!spec.ok) {
      std::snprintf(line, sizeof(line), "[ab_flip] REFUSED at frame %llu: %s (nothing will be flipped this run)", (unsigned long long)frame, spec.error.c_str());
      if (log) log(line);
      return;
    }
    active = true;
    std::string cyc;
    for (int i : spec.order) cyc += (cyc.empty() ? "" : ",") + spec.phases[size_t(i)].name;
    std::snprintf(line, sizeof(line), "[ab_flip] on at frame %llu: period %u settle %u anchor %u start %llu cycle %s (reference = %s)", (unsigned long long)frame, spec.period, spec.settle,
                  spec.anchor, (unsigned long long)spec.start, cyc.c_str(), spec.phases[0].name.c_str());
    if (log) log(line);
    if (log) log(("[ab_flip] spec " + text).c_str());  // the reader (tools\surface\flip_line.py) checks the run's [switch] lines against it
  }
  void Apply(const Phase& p, uint64_t frame, SetFn set, LogFn log) {
    for (const auto& kv : p.sets) {
      if (!set(kv.first.c_str(), kv.second.c_str())) {
        ++set_failures;
        char bad[320];
        std::snprintf(bad, sizeof(bad), "[ab_flip] frame %llu: the setting %s=%s was REJECTED by the cvar registry (this phase is not what its name says)", (unsigned long long)frame,
                      kv.first.c_str(), kv.second.c_str());
        if (log) log(bad);
      }
    }
  }
  // Every guest frame; `anchor_in` = the frame the spec's anchor mark fired at (0 = not yet). Returns true when this frame started a new phase.
  bool AtFrame(uint64_t frame, uint64_t anchor_in, SetFn set, LogFn log) {
    if (!active || frame < 2) return false;
    if (spec.anchor && !anchor_frame && anchor_in) {
      anchor_frame = anchor_in;
      char l[160];
      std::snprintf(l, sizeof(l), "[ab_flip] anchor mark %u at frame %llu", spec.anchor, (unsigned long long)anchor_in);
      if (log) log(l);
    }
    const uint64_t base = BaseFrame(spec, anchor_frame);
    if (!base || frame < base) {
      // before phase 0: the REFERENCE phase's values from frame 2 (the command line carries the non-reference values so armed switches are armed)
      if (!pre_done) {
        pre_done = true;
        Apply(spec.phases[0], frame, set, log);
        char line[200];
        std::snprintf(line, sizeof(line), "[ab_flip] frame %llu phase %s no -1 rel %lld", (unsigned long long)frame, spec.phases[0].name.c_str(), (long long)frame - (long long)anchor_frame);
        if (log) log(line);
        return true;
      }
      return false;
    }
    const uint64_t no = (frame - base) / spec.period;
    if (no == last_phase_no) return false;
    last_phase_no = no;
    const Phase& p = spec.phases[size_t(spec.order[size_t(no % spec.order.size())])];
    Apply(p, frame, set, log);
    ++flips;
    char line[200];
    std::snprintf(line, sizeof(line), "[ab_flip] frame %llu phase %s no %llu rel %lld", (unsigned long long)frame, p.name.c_str(), (unsigned long long)no, (long long)frame - (long long)anchor_frame);
    if (log) log(line);
    return true;
  }
};

}  // namespace ab_flip
