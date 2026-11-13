// Session 101 lane 164: fview's constant override. A TEST-ONLY tool: it changes a draw's packed constants (and can drop draws) at bind time, AFTER the stream's
// values, so a picture can be rendered "as captured" and "with this register forced". It is never switched on by the game or the live renderer: only fview's
// --const-override / --draw-drop set g_const_override.active (so the cost everywhere else is one false test per draw).
//
//   SPEC   = STAGE:REGS[.COMPS]=VALUES[@filter...]       ps:170.w=1    vs:168-171=0    ps:170=1,1,1,1    ps:0-4.xyz=0.5,0.5,0.5@class=all
//   STAGE  = ps | vs        REGS = N or N-M (vec4 registers, 0..255)        COMPS = a subset of xyzw (or rgba), default all four
//   VALUES = one float (every selected component) or one float per selected component (a range repeats the set in every register)
//   filter = @class=wrestlers (DEFAULT: draws whose vertex or pixel program is a yCh_* effect) | @class=all | @class=model_part|crowd|...  (DrawClassName)
//            @effect=PREFIX (program name prefix)   @site=0xADDR (the origin's call_site or pass_site)   @object=0xADDR   @group=N
//            @group=N: the wrestlers are told apart by their sphere constants (VS c168-171, identical on every draw of one wrestler in a frame); N counts the
//            distinct non-zero sets in the order the frame's draws first meet them (0, 1, ...). Which group is which wrestler is in the census.
//   --draw-drop FILTER leaves out the draws the filter matches (a test arm: a wrestler's pixels = the picture with minus the picture without).
// The override edits a COPY of the block the draw is pushed with (the constant bank is never written). A rule changes a draw only when its program reads a
// register in the range and a value differs; the counters say how many draws that was, so a no-op override is visible (Report()).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "../renderer/shaders/spv/fx_registry.h"
#include "capture_reader.h"

namespace rdr_adapter {
namespace const_override {

struct Filter {
  enum Cls { Wrestlers, All, Named };
  int cls = Wrestlers;
  DrawClass named = DrawClass::Unnamed;
  bool has_site = false, has_object = false, has_group = false;
  uint32_t site = 0, object = 0;
  int group = 0;
  std::string effect;
};

struct Rule {
  std::string text;      // the SPEC as given
  bool pixel = false;    // ps
  int first = 0, last = 0;
  uint8_t comps = 0xF;   // bit k = component k (x y z w)
  float values[4] = {};
  int value_count = 1;   // 1 = broadcast, else one per selected component in order
  Filter filter;
  // counters: this bind (the last bound frame) and over every bind
  uint32_t seen_frame = 0, unread_frame = 0, changed_frame = 0, same_frame = 0;
  uint64_t seen = 0, unread = 0, changed = 0, same = 0;
};

struct DropRule {
  std::string text;
  Filter filter;
  uint32_t dropped_frame = 0;
  uint64_t dropped = 0;
};

// One wrestler group of a frame (the census): sphere constants told them apart.
struct GroupInfo {
  uint64_t key = 0;
  uint32_t draws = 0;
  float weight_min = 1e30f, weight_max = -1e30f;  // PS c170.w over the group's draws that have a pixel bank
  float centre[2][4] = {};                        // VS c168 / c169 (sphere centre xyz, radius w)
  float strength[2][4] = {};                      // VS c170 / c171 (axis xyz, strength w)
  float ps_shadow[3][4] = {};                     // lane 210: PS c168 / c169 / c170 (ShadowPrm[0..2]) of the group's first draw
};
struct FrameCensus {
  uint64_t bind = 0;
  std::vector<GroupInfo> groups;
};

// What one draw is, computed once per BindDraw.
struct Ctx {
  uint32_t index = 0;
  const DrawOrigin* origin = nullptr;
  const char* vs_name = "";
  const char* ps_name = "";
  bool wrestler = false;
  int group = -1;
};

inline bool StartsWith(const char* s, const char* p) { return s && std::strncmp(s, p, std::strlen(p)) == 0; }

inline bool ParseFilter(const std::string& text, Filter& f, std::string& err) {
  size_t at = 0;
  while (at < text.size()) {
    size_t next = text.find('@', at);
    if (next == std::string::npos) next = text.size();
    const std::string term = text.substr(at, next - at);
    at = next + 1;
    if (term.empty()) continue;
    const size_t eq = term.find('=');
    if (eq == std::string::npos) { err = "filter term '" + term + "' is not key=value"; return false; }
    const std::string key = term.substr(0, eq), value = term.substr(eq + 1);
    char* end = nullptr;
    if (key == "class") {
      if (value == "wrestlers") f.cls = Filter::Wrestlers;
      else if (value == "all") f.cls = Filter::All;
      else {
        bool found = false;
        for (uint8_t c = 0; c < uint8_t(DrawClass::Count); ++c)
          if (value == DrawClassName(DrawClass(c))) { f.cls = Filter::Named, f.named = DrawClass(c), found = true; }
        if (!found) { err = "unknown class '" + value + "'"; return false; }
      }
    } else if (key == "effect") {
      if (value.empty()) { err = "empty effect prefix"; return false; }
      f.effect = value;
    } else if (key == "site" || key == "object") {
      const unsigned long v = std::strtoul(value.c_str(), &end, 0);
      if (value.empty() || *end) { err = "bad address '" + value + "'"; return false; }
      if (key == "site") f.has_site = true, f.site = uint32_t(v);
      else f.has_object = true, f.object = uint32_t(v);
    } else if (key == "group") {
      const long v = std::strtol(value.c_str(), &end, 10);
      if (value.empty() || *end || v < 0 || v > 63) { err = "bad group '" + value + "'"; return false; }
      f.has_group = true, f.group = int(v);
    } else {
      err = "unknown filter key '" + key + "'";
      return false;
    }
  }
  return true;
}

inline bool ParseSpec(const std::string& spec, Rule& r, std::string& err) {
  r.text = spec;
  const size_t at = spec.find('@');
  const std::string body = spec.substr(0, at);
  if (at != std::string::npos && !ParseFilter(spec.substr(at + 1), r.filter, err)) return false;
  const size_t colon = body.find(':'), eq = body.find('=');
  if (colon == std::string::npos || eq == std::string::npos || eq < colon) { err = "expected STAGE:REGS[.COMPS]=VALUES"; return false; }
  const std::string stage = body.substr(0, colon);
  if (stage == "ps") r.pixel = true;
  else if (stage == "vs") r.pixel = false;
  else { err = "stage must be ps or vs, got '" + stage + "'"; return false; }
  std::string regs = body.substr(colon + 1, eq - colon - 1);
  std::string comps;
  if (const size_t dot = regs.find('.'); dot != std::string::npos) comps = regs.substr(dot + 1), regs = regs.substr(0, dot);
  char* end = nullptr;
  const long a = std::strtol(regs.c_str(), &end, 10);
  long b = a;
  if (regs.empty() || (*end && *end != '-')) { err = "bad register '" + regs + "'"; return false; }
  if (*end == '-') {
    const char* rest = end + 1;
    b = std::strtol(rest, &end, 10);
    if (*rest == 0 || *end) { err = "bad register range '" + regs + "'"; return false; }
  }
  if (a < 0 || b < a || b > 255) { err = "register range " + regs + " is outside 0..255 (or backwards)"; return false; }
  r.first = int(a), r.last = int(b);
  if (!comps.empty()) {
    r.comps = 0;
    for (char c : comps) {
      int k = -1;
      for (int i = 0; i < 4; ++i)
        if (c == "xyzw"[i] || c == "rgba"[i]) k = i;
      if (k < 0) { err = std::string("bad component '") + c + "'"; return false; }
      if (r.comps & (1u << k)) { err = "component repeated in '" + comps + "'"; return false; }
      r.comps |= uint8_t(1u << k);
    }
  }
  const std::string vals = body.substr(eq + 1);
  if (vals.empty()) { err = "no values"; return false; }
  int count = 0;
  size_t pos = 0;
  while (pos <= vals.size()) {
    size_t comma = vals.find(',', pos);
    if (comma == std::string::npos) comma = vals.size();
    const std::string one = vals.substr(pos, comma - pos);
    pos = comma + 1;
    if (count >= 4) { err = "more than 4 values"; return false; }
    const float v = std::strtof(one.c_str(), &end);
    if (one.empty() || *end) { err = "bad value '" + one + "'"; return false; }
    r.values[count++] = v;
  }
  int selected = 0;
  for (int k = 0; k < 4; ++k) selected += (r.comps >> k) & 1;
  if (count != 1 && count != selected) { err = "need 1 value or " + std::to_string(selected) + " (one per selected component), got " + std::to_string(count); return false; }
  r.value_count = count;
  return true;
}

struct State {
  bool active = false;
  bool want_census = false;
  std::vector<Rule> rules;
  std::vector<DropRule> drops;
  std::vector<FrameCensus> census;  // one per bind, only while want_census
  uint64_t bind = 0;
  std::unordered_map<uint64_t, int> group_ids;  // this bind
  uint32_t frame_draws = 0, frame_wrestler_draws = 0, frame_changed_draws = 0;  // this bind: draws any rule changed
  uint64_t total_wrestler_draws = 0, total_changed_draws = 0;
  uint32_t last_changed_marker = 0xFFFFFFFFu;  // the draw index counted in frame_changed_draws last (a draw is counted once for all its rules / stages)

  bool AddRule(const std::string& spec, std::string& err) {
    Rule r;
    if (!ParseSpec(spec, r, err)) return false;
    rules.push_back(r);
    active = true;
    return true;
  }
  bool AddDrop(const std::string& spec, std::string& err) {
    DropRule d;
    d.text = spec;
    if (!ParseFilter(spec, d.filter, err)) return false;
    drops.push_back(d);
    active = true;
    return true;
  }

  void BeginBind() {
    ++bind;
    group_ids.clear();
    frame_draws = frame_wrestler_draws = frame_changed_draws = 0;
    last_changed_marker = 0xFFFFFFFFu;
    for (Rule& r : rules) r.seen_frame = r.unread_frame = r.changed_frame = r.same_frame = 0;
    for (DropRule& d : drops) d.dropped_frame = 0;
    if (want_census) census.push_back({bind, {}});
  }

  static uint64_t Hash(const uint32_t* w, int n) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < n; ++i) h = (h ^ w[i]) * 1099511628211ull;
    return h;
  }

  // Who is this draw. Called for EVERY draw of a bind while active, so the group numbers do not depend on the rules.
  Ctx Eval(const LoadedFrame& frame, uint32_t index, const Draw& draw, int vs, int ps) {
    Ctx c;
    c.index = index;
    c.origin = frame.OriginOf(index);
    c.vs_name = vs >= 0 ? rdr::fx::kVariants[vs].name : "";
    c.ps_name = ps >= 0 ? rdr::fx::kVariants[ps].name : "";
    c.wrestler = StartsWith(c.vs_name, "yCh_") || StartsWith(c.ps_name, "yCh_");
    ++frame_draws;
    if (!c.wrestler) return c;
    ++frame_wrestler_draws, ++total_wrestler_draws;
    const uint32_t* vb = frame.constant_banks[draw.vs_bank].dwords + 168 * 4;
    bool any = false;
    for (int i = 0; i < 16; ++i) any = any || vb[i] != 0;
    if (!any) return c;
    const uint64_t key = Hash(vb, 16);
    auto it = group_ids.find(key);
    if (it == group_ids.end()) it = group_ids.emplace(key, int(group_ids.size())).first;
    c.group = it->second;
    if (want_census && !census.empty()) {
      FrameCensus& fc = census.back();
      if (size_t(c.group) >= fc.groups.size()) fc.groups.resize(size_t(c.group) + 1);
      GroupInfo& g = fc.groups[size_t(c.group)];
      if (!g.draws) {
        g.key = key;
        std::memcpy(g.ps_shadow, frame.constant_banks[draw.ps_bank].dwords + 168 * 4, 48);  // lane 210
        std::memcpy(g.centre[0], vb, 16), std::memcpy(g.centre[1], vb + 4, 16), std::memcpy(g.strength[0], vb + 8, 16), std::memcpy(g.strength[1], vb + 12, 16);
      }
      ++g.draws;
      float w;
      std::memcpy(&w, frame.constant_banks[draw.ps_bank].dwords + 170 * 4 + 3, 4);
      if (w < g.weight_min) g.weight_min = w;
      if (w > g.weight_max) g.weight_max = w;
    }
    return c;
  }

  static bool Matches(const Filter& f, const Ctx& c) {
    if (f.cls == Filter::Wrestlers && !c.wrestler) return false;
    if (f.cls == Filter::Named && (!c.origin || c.origin->klass != f.named)) return false;
    if (!f.effect.empty() && !StartsWith(c.vs_name, f.effect.c_str()) && !StartsWith(c.ps_name, f.effect.c_str())) return false;
    if (f.has_site && (!c.origin || (c.origin->call_site != f.site && c.origin->pass_site != f.site))) return false;
    if (f.has_object && (!c.origin || c.origin->object != f.object)) return false;
    if (f.has_group && c.group != f.group) return false;
    return true;
  }

  bool Drops(const Ctx& c) {
    bool drop = false;
    for (DropRule& d : drops)
      if (Matches(d.filter, c)) ++d.dropped_frame, ++d.dropped, drop = true;
    return drop;
  }

  // Edit the packed block of this stage in place. Returns true when a byte changed.
  bool Apply(const Ctx& c, bool pixel, const rdr::fx::Variant& variant, uint8_t* block) {
    bool changed_any = false;
    for (Rule& r : rules) {
      if (r.pixel != pixel || !Matches(r.filter, c)) continue;
      ++r.seen_frame, ++r.seen;
      bool read = false, changed = false;
      for (int reg = r.first; reg <= r.last; ++reg) {
        for (uint32_t k = 0; k < variant.constant_count; ++k) {
          const rdr::fx::ConstantRange& range = variant.constants[k];
          if (reg < range.reg || reg >= range.reg + range.count) continue;
          read = true;
          uint8_t* at = block + range.offset + uint32_t(reg - range.reg) * 16;
          int vi = 0;
          for (int comp = 0; comp < 4; ++comp) {
            if (!(r.comps >> comp & 1)) continue;
            const float v = r.value_count == 1 ? r.values[0] : r.values[vi];
            ++vi;
            uint32_t old, now;
            std::memcpy(&old, at + comp * 4, 4);
            std::memcpy(&now, &v, 4);
            if (old != now) std::memcpy(at + comp * 4, &v, 4), changed = true;
          }
        }
      }
      if (!read) ++r.unread_frame, ++r.unread;
      else if (changed) ++r.changed_frame, ++r.changed;
      else ++r.same_frame, ++r.same;
      changed_any = changed_any || changed;
    }
    if (changed_any && last_changed_marker != c.index) last_changed_marker = c.index, ++frame_changed_draws, ++total_changed_draws;
    return changed_any;
  }

  std::string Report() const {
    std::string out;
    char buf[512];
    std::snprintf(buf, sizeof buf, "[fview] const-override: last bound frame: %u draws, %u wrestler draws, %u draws changed; over %llu binds: %llu wrestler draws, %llu draws changed\n", frame_draws,
                  frame_wrestler_draws, frame_changed_draws, (unsigned long long)bind, (unsigned long long)total_wrestler_draws, (unsigned long long)total_changed_draws);
    out += buf;
    for (const Rule& r : rules) {
      std::snprintf(buf, sizeof buf, "[fview] const-override rule '%s': last frame: %u matched, %u changed, %u already equal, %u whose program reads none of the registers%s\n", r.text.c_str(),
                    r.seen_frame, r.changed_frame, r.same_frame, r.unread_frame, r.changed_frame == 0 ? "  -> NO-OP on this frame" : "");
      out += buf;
    }
    for (const DropRule& d : drops) {
      std::snprintf(buf, sizeof buf, "[fview] draw-drop '%s': last frame: %u draws dropped (%llu over every bind)%s\n", d.text.c_str(), d.dropped_frame, (unsigned long long)d.dropped,
                    d.dropped_frame == 0 ? "  -> NO-OP on this frame" : "");
      out += buf;
    }
    if (!census.empty() || want_census) {
      // (the group table of the last bound frame, when a census is on)
    }
    return out;
  }
};

inline State g_state;

}  // namespace const_override
inline const_override::State& g_const_override = const_override::g_state;
}  // namespace rdr_adapter
