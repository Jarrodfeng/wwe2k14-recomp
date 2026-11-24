// S105 lane T3 (docs\research\s105\T3_attire.md): host unit of the launcher's participant token (src\launch_participant.h).
// 0 game runs, no game file.
//   launch_participant_unit              every case; exit 0 only when all pass
//   launch_participant_unit --plant NAME exit 0 only when the cases FAIL on the planted twin
//                                         (old_first_colon: "397::1" parsed as pad 0; third_part_ignored: attire dropped;
//                                          caw_attire_unchecked: a CAW attire above 3 accepted; empty_set_not_refused: a stock set with word 0xFFFFFFFF is used)
// entry: wwe2k14::launch::PlanParticipant, ParseParticipantToken  (the production caller, FireLaunch's participant loop in src\scenario.h, passes the
// comma-split token to it, then range-checks id / pad, then ResolveSet before sub_822C6808)
// S109 ENT (docs\research\s109\ENT_plan.md): the match builder's spec, through the production parser wwe2k14::scenario::ParseBuildSpec
// (src\build_spec.h; its caller FireBuild in src\scenario.h writes the seat record from spec.attire and arms the team-table skip from spec.entrances).
// Builder plants (each must be CAUGHT): builder_old_parse (a third ':' part refused: the pre-ENT parse), builder_no_entrances_key (entrances=
// unknown), builder_set_accepted (a colour set accepted on the builder).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/launch_participant.h"
#include "../../src/build_spec.h"

using namespace wwe2k14::launch;

enum Plant { kNone, kOldFirstColon, kThirdIgnored, kEmptySetUsed, kCawAttire, kBuilderOld, kBuilderNoEntrances, kBuilderSetAccepted };
static Plant g_plant = kNone;
static int g_fail = 0;

static bool Parse(const std::string& t, ParticipantToken& o, std::string& why) {
  if (g_plant == kOldFirstColon) {  // the pre-T3 parse
    o = ParticipantToken{};
    why.clear();
    if (t.empty()) return false;
    const size_t colon = t.find(':');
    o.id = std::strtol(t.substr(0, colon).c_str(), nullptr, 10);
    o.pad = -1;
    if (colon != std::string::npos) o.pad = std::strtol(t.substr(colon + 1).c_str(), nullptr, 10);
    return true;
  }
  const bool ok = ParseParticipantToken(t, o, why);
  if (g_plant == kThirdIgnored && ok) o.has_attire = o.has_set = o.saved = false;
  if (g_plant == kCawAttire && ok && o.id < 100 && !o.has_attire && !why.empty()) {  // a CAW attire above 3 accepted
    o.has_attire = o.has_set = true;
    o.attire = 4;
    why.clear();
  }
  return ok;
}

// the builder's spec parse, with the plants emulating the faults they name
static bool Spec(const std::string& text, wwe2k14::scenario::BuildSpec& out, std::string& why) {
  out = wwe2k14::scenario::BuildSpec{};
  why.clear();
  if (g_plant == kBuilderOld) {  // pre-ENT: a p item is A:B, a third part fails the number check
    const size_t p = text.find("p=");
    if (p != std::string::npos) {
      const std::string v = text.substr(p + 2, text.find(';', p) == std::string::npos ? std::string::npos : text.find(';', p) - p - 2);
      size_t a = 0;
      while (a <= v.size()) {
        size_t b = v.find(',', a);
        if (b == std::string::npos) b = v.size();
        const std::string item = v.substr(a, b - a);
        const size_t c = item.find(':');
        if (c != std::string::npos && item.find(':', c + 1) != std::string::npos) return why = "'" + item + "': A:B", false;
        a = b + 1;
      }
    }
  }
  if (g_plant == kBuilderNoEntrances && text.find("entrances=") != std::string::npos) return why = "unknown key 'entrances'", false;
  const bool ok = wwe2k14::scenario::ParseBuildSpec(text, out, why);
  if (!ok && g_plant == kBuilderSetAccepted && why.find("colour sets") != std::string::npos) {
    out = wwe2k14::scenario::BuildSpec{};
    std::string t = text;
    for (size_t s = t.find('/'); s != std::string::npos; s = t.find('/')) {  // N/S read as N
      size_t e = s;
      while (e < t.size() && t[e] != ',' && t[e] != ';') ++e;
      t.erase(s, e - s);
    }
    why.clear();
    return wwe2k14::scenario::ParseBuildSpec(t, out, why);
  }
  return ok;
}

static int Resolve(long id, int set, uint32_t word, std::string& why) {
  if (g_plant == kEmptySetUsed) word = 0;
  return ResolveSet(id, set, word, why);
}

#define CHECK(cond, ...)                          \
  do {                                            \
    if (!(cond)) {                                \
      ++g_fail;                                   \
      std::printf("FAIL %s: ", #cond);            \
      std::printf(__VA_ARGS__);                   \
      std::printf("\n");                          \
    }                                             \
  } while (0)

struct Case {
  const char* tok;
  bool ok;
  long id, pad;
  bool has_attire, saved, has_set;
  int attire, set;
  bool warn;  // `why` non-empty
};

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--plant") == 0) {
      const std::string n = argv[i + 1];
      g_plant = n == "old_first_colon" ? kOldFirstColon : n == "third_part_ignored" ? kThirdIgnored
                : n == "empty_set_not_refused" ? kEmptySetUsed : n == "caw_attire_unchecked" ? kCawAttire
                : n == "builder_old_parse" ? kBuilderOld : n == "builder_no_entrances_key" ? kBuilderNoEntrances
                : n == "builder_set_accepted" ? kBuilderSetAccepted : kNone;
      if (g_plant == kNone) {
        std::printf("unknown plant %s\n", n.c_str());
        return 2;
      }
    }
  }
  static const Case cases[] = {
      // today's meanings, 1 and 2 parts (the old strtol reading, exactly)
      {"397", true, 397, -1, false, false, false, 0, -1, false},
      {"397:0", true, 397, 0, false, false, false, 0, -1, false},
      {"397:2", true, 397, 2, false, false, false, 0, -1, false},
      {"397:-1", true, 397, -1, false, false, false, 0, -1, false},
      {"397:", true, 397, 0, false, false, false, 0, -1, false},      // old: strtol("") = 0
      {"397:x", true, 397, 0, false, false, false, 0, -1, false},     // old: junk = 0
      {"abc", true, 0, -1, false, false, false, 0, -1, false},        // old: junk id = 0
      // third part
      {"397::1", true, 397, -1, true, false, true, 1, -1, false},     // empty PAD = CPU (old parse said pad 0)
      {"397:-1:1", true, 397, -1, true, false, true, 1, -1, false},
      {"397:0:1", true, 397, 0, true, false, true, 1, -1, false},
      {"13:0:2/1", true, 13, 0, true, false, true, 2, 1, false},
      {"13:0:t0", true, 13, 0, true, false, true, 0, 0, false},
      {"13:0:t2", true, 13, 0, true, false, true, 0, 2, false},
      {"13:0:t-1", true, 13, 0, true, false, true, 0, -1, false},
      {"13:0:s", true, 13, 0, true, true, true, 0, -1, false},
      {"397:1:9/3", true, 397, 1, true, false, true, 9, 3, false},
      {"5:0:3", true, 5, 0, true, false, true, 3, -1, false},       // CAW attire 0..3
      {"5:0:4", true, 5, 0, false, false, false, 0, -1, true},      // CAW attire 4 refused
      {"13:0:9", true, 13, 0, false, false, false, 0, -1, true},
      {"5:0:0/3", true, 5, 0, true, false, true, 0, 3, false},
      // a bad attire part keeps id and pad, drops the attire, says why
      {"397:0:10", true, 397, 0, false, false, false, 0, -1, true},
      {"13:0:-1", true, 13, 0, false, false, false, 0, -1, true},
      {"13:0:x", true, 13, 0, false, false, false, 0, -1, true},
      {"13:0:", true, 13, 0, false, false, false, 0, -1, true},
      {"13:0:t", true, 13, 0, false, false, false, 0, -1, true},
      {"13:0:t4", true, 13, 0, false, false, false, 0, -1, true},
      {"13:0:2/4", true, 13, 0, false, false, false, 0, -1, true},
      {"13:0:2/", true, 13, 0, false, false, false, 0, -1, true},
      {"13:0:/1", true, 13, 0, false, false, false, 0, -1, true},
      {"13:0:1/2/3", true, 13, 0, false, false, false, 0, -1, true},
      {"13:0:S", true, 13, 0, false, false, false, 0, -1, true},
      // unusable tokens
      {"", false, 0, -1, false, false, false, 0, -1, true},
      {"13:0:1:2", false, 0, -1, false, false, false, 0, -1, true},
  };
  for (const Case& c : cases) {
    ParticipantToken o;
    std::string why;
    const bool ok = Parse(c.tok, o, why);
    CHECK(ok == c.ok, "'%s' usable %d want %d", c.tok, ok, c.ok);
    if (!c.ok || !ok) continue;
    CHECK(o.id == c.id, "'%s' id %ld want %ld", c.tok, o.id, c.id);
    CHECK(o.pad == c.pad, "'%s' pad %ld want %ld", c.tok, o.pad, c.pad);
    CHECK(o.has_attire == c.has_attire, "'%s' has_attire %d want %d", c.tok, o.has_attire, c.has_attire);
    CHECK(o.saved == c.saved, "'%s' saved %d want %d", c.tok, o.saved, c.saved);
    CHECK(o.has_set == c.has_set, "'%s' has_set %d want %d", c.tok, o.has_set, c.has_set);
    CHECK(o.attire == c.attire, "'%s' attire %d want %d", c.tok, o.attire, c.attire);
    CHECK(o.set == c.set, "'%s' set %d want %d", c.tok, o.set, c.set);
    CHECK(why.empty() != c.warn, "'%s' why '%s' want warn %d", c.tok, why.c_str(), c.warn);
  }
  // the colour set rules (the game predicate sub_827DAD00's empty check; stock 0..2, CAW 0..3)
  std::string why;
  CHECK(Resolve(397, 0, 5, why) == 0 && why.empty(), "stock set 0 usable");
  CHECK(Resolve(397, 2, 5, why) == 2, "stock set 2 usable");
  CHECK(Resolve(397, 3, 5, why) == -1 && !why.empty(), "stock set 3 refused");
  CHECK(Resolve(397, 1, 0xFFFFFFFFu, why) == -1 && !why.empty(), "stock empty set refused");
  CHECK(Resolve(397, 0, 0, why) == 0, "stock base attire 0 is not empty");
  CHECK(Resolve(397, -1, 0xFFFFFFFFu, why) == -1 && why.empty(), "set -1 is no set, no warning");
  CHECK(Resolve(5, 3, 0xFFFFFFFFu, why) == 3 && why.empty(), "CAW set 3 accepted (the word is not read)");
  CHECK(Resolve(5, 4, 0, why) == -1 && !why.empty(), "CAW set 4 refused");
  CHECK(Resolve(99, 0, 0, why) == 0 && Resolve(100, 3, 0, why) == -1, "id 99 CAW / id 100 stock boundary");

  // PlanParticipant: the whole per-participant decision FireLaunch makes (S105 T3 F2/F5)
  {
    const auto plan = [&](const char* tok, const SavedDefaults* sv, uint32_t word, int& calls) {
      ParticipantToken t;
      std::string w;
      ParseParticipantToken(tok, t, w);
      calls = 0;
      return PlanParticipant(t, sv, [&](int) { ++calls; return word; });
    };
    int calls = 0;
    ParticipantPlan p = plan("397:0", nullptr, 0, calls);
    CHECK(!p.set_call && !p.alt_call && p.attire == 0 && calls == 0, "plain token: no set call, no alt call, word not read");
    p = plan("397:0:2", nullptr, 0, calls);
    CHECK(p.set_call && p.set == -1 && p.attire == 2 && calls == 0, "attire only: set setter with -1");
    p = plan("397:0:1/1", nullptr, 0xFFFFFFFFu, calls);
    CHECK(p.set_call && p.set == (g_plant == kEmptySetUsed ? 1 : -1) && !p.set_why.empty() == (g_plant != kEmptySetUsed) && calls == 1,
          "stock empty set refused through the plan (word read once)");
    p = plan("397:0:1/1", nullptr, 7, calls);
    CHECK(p.set_call && p.set == 1 && p.set_why.empty(), "stock set with a base attire accepted");
    p = plan("13:0:0/3", nullptr, 0xFFFFFFFFu, calls);
    CHECK(p.set == 3 && calls == 0, "CAW set 3 accepted, word not read");
    p = plan("397:0:2/3", nullptr, 0, calls);
    CHECK(p.set_call && p.set == -1 && !p.set_why.empty() && calls == 0, "stock set 3 refused, word not read");
    SavedDefaults sv;
    sv.attire = 4, sv.set = 1, sv.alt = 7;
    p = plan("397:0:s", &sv, 5, calls);
    CHECK(p.attire == 4 && p.set == 1 && p.alt_call && p.alt == 7 && p.set_call, "s: attire / set / alt from the saved copy");
    sv.alt = 512;
    p = plan("397:0:s", &sv, 5, calls);
    CHECK(!p.alt_call, "s: alt 512 = no alternate call");
    sv.attire = 9, sv.alt = 512;
    p = plan("13:0:s", &sv, 5, calls);
    CHECK(p.attire == 0, "s: a CAW's saved attire 9 clamps to 0");
    p = plan("397:0:s", nullptr, 5, calls);
    CHECK(p.attire == 0 && p.set == -1 && !p.alt_call && p.set_call, "s without a copy: attire 0, set -1");
  }

  // S109 ENT: the match builder's spec (src\build_spec.h ParseBuildSpec, the production parser FireBuild calls)
  {
    using wwe2k14::scenario::BuildSpec;
    BuildSpec b;
    std::string w;
    // a pre-ENT spec parses as before: two-part p items, no attire written, entrances play
    bool ok = Spec("type=0;p=139:-1,343:0;arena=5;o=81:1", b, w);
    CHECK(ok && b.parts.size() == 2 && b.parts[0].first == 139 && b.parts[0].second == -1 && b.parts[1].second == 0 && b.attire.size() == 2 &&
              !b.attire[0].write && !b.attire[1].write && b.entrances && b.opts.size() == 1,
          "pre-ENT spec unchanged (%s)", w.c_str());
    ok = Spec("type=0;p=139:,343:0", b, w);
    CHECK(!ok, "two-part '139:' still refused (strict numbers, as before)");
    ok = Spec("type=0;p=139", b, w);
    CHECK(!ok, "one-part p item still refused");
    // the third part: attire N (empty PAD = CPU), s = nothing written
    ok = Spec("type=0;p=139::2,343:-1", b, w);
    CHECK(ok && b.parts[0].second == -1 && b.attire[0].write && b.attire[0].attire == 2 && !b.attire[1].write, "139::2 = CPU, attire 2 written (%s)", w.c_str());
    ok = Spec("type=0;p=139:1:9", b, w);
    CHECK(ok && b.parts[0].second == 1 && b.attire[0].write && b.attire[0].attire == 9, "139:1:9 = pad 1, attire 9 (%s)", w.c_str());
    ok = Spec("type=0;p=139:-1:s", b, w);
    CHECK(ok && !b.attire[0].write, "s = the pick's saved defaults: nothing written (%s)", w.c_str());
    ok = Spec("type=0;p=5:-1:3", b, w);
    CHECK(ok && b.attire[0].write && b.attire[0].attire == 3, "CAW attire 3 (%s)", w.c_str());
    ok = Spec("type=0;p=5:-1:4", b, w);
    CHECK(!ok, "CAW attire 4 refused");
    ok = Spec("type=0;p=139:-1:10", b, w);
    CHECK(!ok, "attire 10 refused (never dropped)");
    ok = Spec("type=0;p=139:-1:", b, w);
    CHECK(!ok, "empty attire part refused");
    ok = Spec("type=0;p=139:x:2", b, w);
    CHECK(!ok, "a junk PAD with an attire refused (strict)");
    ok = Spec("type=0;p=139:-1:2:1", b, w);
    CHECK(!ok, "four parts refused");
    ok = Spec("type=0;p=139:4:2", b, w);
    CHECK(!ok, "pad 4 refused by the range check");
    // colour sets: launcher only
    ok = Spec("type=0;p=139:-1:2/1", b, w);
    CHECK(!ok && w.find("colour sets") != std::string::npos, "N/S refused on the builder (%s)", w.c_str());
    ok = Spec("type=0;p=139:-1:t1", b, w);
    CHECK(!ok && w.find("colour sets") != std::string::npos, "tS refused on the builder (%s)", w.c_str());
    // entrances
    ok = Spec("type=0;p=139:-1,343:-1;entrances=0", b, w);
    CHECK(ok && !b.entrances, "entrances=0 skips (%s)", w.c_str());
    ok = Spec("type=0;entrances=1", b, w);
    CHECK(ok && b.entrances, "entrances=1 plays");
    ok = Spec("type=0;entrances=off", b, w);
    CHECK(!ok, "entrances=off refused (0 | 1)");
    // the request the main session runs: blood on + no entrances + an attire
    ok = Spec("type=0;p=139:-1:2,343:-1;arena=5;o=81:1;entrances=0", b, w);
    CHECK(ok && !b.entrances && b.attire[0].write && b.attire[0].attire == 2 && b.opts[0].first == 81 && b.opts[0].second == 1,
          "blood + no entrances + attire in one spec (%s)", w.c_str());
  }

  if (g_plant != kNone) {
    std::printf("plant: %d check(s) failed -> %s\n", g_fail, g_fail ? "CAUGHT" : "NOT CAUGHT");
    return g_fail ? 0 : 1;
  }
  std::printf("%s (%d failed)\n", g_fail ? "FAIL" : "ok", g_fail);
  return g_fail ? 1 : 0;
}
