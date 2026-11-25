// S110 BUSWAIT: the unit test of src/event_wait_core.h (the route's view of the event bus) on synthetic events. No game, no SDK.
// Built and run by tools/bus/event_wait_selftest.py (clang++ -std=c++17 -I src -DHDR=\"...\" tools/bus/event_wait_test.cpp).
// Prints one line per failed check and "event_wait: N checks, F failed"; exit 1 when any failed.
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include HDR

using namespace wwe2k14::event_wait;

static int g_checks = 0, g_failed = 0;
static void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

static std::vector<std::string> Words(const std::string& s) {
  std::vector<std::string> w;
  std::string cur;
  for (char c : s) {
    if (c == ' ') {
      if (!cur.empty()) w.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) w.push_back(cur);
  return w;
}

static Spec P(const std::string& line, bool want_ok = true) {
  Spec s;
  std::string why;
  const bool ok = ParseSpec(Words(line), s, why);
  Check(ok == want_ok, "parse '" + line + "' -> " + (ok ? "ok" : "error: " + why));
  return s;
}

static Event E(const std::string& kind, const std::string& fields) {
  Event e;
  e.kind = kind;
  e.fields = fields;
  return e;
}

int main() {
  // ---- the route line
  {
    const Spec s = P("pin");
    Check(s.filter.kind == "match" && s.filter.conds.size() == 1 && s.filter.text == "match what=pin" && s.timeout == -1 && !s.optional,
          "pin = match what=pin: " + s.filter.text);
  }
  {
    const Spec s = P("finish finish_name=pinfall 20000");
    Check(s.filter.text == "match what=finish finish_name=pinfall" && s.timeout == 20000, "finish + field + timeout: " + s.filter.text);
  }
  {
    const Spec s = P("entrance slot=1 step>=5 3000 optional");
    Check(s.filter.text == "entrance slot=1 step>=5" && s.timeout == 3000 && s.optional, "entrance >= + optional: " + s.filter.text);
  }
  {
    const Spec s = P("match kickout at_count!=2");
    Check(s.filter.text == "match what=kickout at_count!=2", "bare what after match: " + s.filter.text);
  }
  Check(P("move id=40 6000").filter.text == "move id=40", "move with a field");
  Check(P("break ring").filter.text == "break what=ring", "break's bare word = what");
  Check(P("replay skipped 600").filter.text == "replay state=skipped", "replay's bare word = state");
  Check(P("MATCH finish").filter.kind == "match", "the kind ignores case");
  P("bogus", false);
  P("", false);
  P("pin pin2", false);              // a second bare word
  P("move slam", false);             // move takes no bare word
  P("match 100 what=pin", false);    // the timeout must come last
  P("match pin optional 100", false);  // optional must be the last word
  P("match pin 0", false);           // a timeout of 0
  P("match what=", false);
  P("match =pin", false);
  P("match a!b", false);
  P("match pin 10 20", false);       // two timeouts
  Check(LastKey(P("finish").filter) == "match:finish" && LastKey(P("load").filter) == "load" && LastKey(P("replay").filter) == "replay" &&
            LastKey(P("replay end").filter) == "replay:end",
        "LastKey (a replay's bare word is its state: replay:end)");

  // ---- the fields of one bus line
  const std::string state = "\"clock\": 512, \"finish_name\": \"last one left\", \"wrestlers\": [{\"slot\": 0, \"id\": 9}, {\"slot\": 1}], "
                            "\"file\": \"GAME:\\\\a \\\"b\\\".pac\", \"ok\": true";
  std::string v;
  Check(FieldValue(state, "clock", v) && v == "512", "a number: " + v);
  Check(FieldValue(state, "finish_name", v) && v == "last one left", "a string with spaces: " + v);
  Check(FieldValue(state, "wrestlers", v) && v == "[{\"slot\": 0, \"id\": 9}, {\"slot\": 1}]", "an array, raw: " + v);
  Check(!FieldValue(state, "slot", v), "a key only inside the array is not a top-level field");
  Check(FieldValue(state, "file", v) && v == "GAME:\\a \"b\".pac", "escapes undone: " + v);
  Check(FieldValue(state, "ok", v) && v == "true", "the last field: " + v);
  Check(!FieldValue(state, "nope", v), "an absent key");
  Check(!FieldValue("", "clock", v), "an empty line");

  // ---- the matcher
  const Event fin = E("match", "\"what\": \"finish\", \"finish_type\": 7, \"finish_name\": \"last one left\", \"winner_slot\": 1, \"loser_slot\": -1");
  Check(Matches(P("finish").filter, fin), "finish matches");
  Check(Matches(P("finish finish_name=last_one_left").filter, fin), "'_' matches a space");
  Check(Matches(P("finish finish_name=LAST_ONE_LEFT").filter, fin), "words ignore case");
  Check(!Matches(P("finish finish_name=pinfall").filter, fin), "another name does not match");
  Check(Matches(P("finish finish_type>=7 winner_slot=1 loser_slot<0").filter, fin), "numbers compare as numbers (>=, =, < on a negative)");
  Check(!Matches(P("finish finish_type>7").filter, fin), "> is strict");
  Check(!Matches(P("finish clock>0").filter, fin), "an absent field never matches");
  Check(!Matches(P("pin").filter, fin), "another what");
  Check(!Matches(P("entrance").filter, fin), "another kind");
  Check(!Matches(P("finish finish_name>a").filter, fin), "an order test on a word never matches");

  // ---- the log: sequence numbers, the mark, two events between two reader ticks
  {
    Log log;
    Check(log.Newest() == 0, "an empty log");
    log.Add(100, "match", "\"what\": \"clock_started\", \"clock\": 1");
    const uint64_t mark = log.Newest();  // the route's mark (its last input)
    // between two reader ticks the bus emits a pin, a count and a move
    const uint64_t s_pin = log.Add(200, "match", "\"what\": \"pin\", \"pinning_slot\": 1");
    const uint64_t s_cnt = log.Add(200, "match", "\"what\": \"count\", \"count\": 1");
    log.Add(201, "move", "\"slot\": 1, \"id\": 9, \"waza\": 17, \"move\": 2300, \"class\": \"finisher\"");
    Event e;
    uint64_t lost = 9;
    Check(!log.FindAfter(s_cnt + 1, P("pin").filter, e, lost), "nothing after the newest");
    Check(log.FindAfter(mark, P("pin").filter, e, lost) && e.seq == s_pin && e.frame == 200 && lost == 0, "the pin, though the count came after it");
    const uint64_t m2 = e.seq;  // the wait matched: the mark moves to the matched event
    Check(log.FindAfter(m2, P("count count=1").filter, e, lost) && e.seq == s_cnt, "the count of the same tick, found by the next wait");
    Check(!log.FindAfter(m2, P("pin").filter, e, lost), "the pin is never matched twice");
    Check(log.FindAfter(0, P("clock_started").filter, e, lost) && e.seq == 1, "the mark 0 sees every event since the start");
    Check(!log.FindAfter(mark, P("clock_started").filter, e, lost), "an event before the mark is not seen");
    Check(log.FindAfter(mark, P("move class=finisher").filter, e, lost) && e.frame == 201, "a move by its class");
    // Last: by kind and by kind:what, however old
    log.Add(300, "load", "\"file\": \"GAME:\\\\M3.PAC\"");
    Check(log.Last("match:pin", e) && e.seq == s_pin, "Last(match:pin)");
    Check(log.Last("match", e) && e.seq == s_cnt, "Last(match) = the newest match line");
    Check(log.Last("load", e) && e.frame == 300, "Last(load)");
    Check(!log.Last("replay", e), "Last of a kind never emitted");
  }
  // ---- the ring: a reader more than kRing events behind is told how many it lost and starts at the oldest kept
  {
    Log log;
    for (uint64_t i = 1; i <= Log::kRing + 10; ++i) log.Add(i, "load", "\"n\": " + std::to_string(i));
    Event e;
    uint64_t lost = 0;
    Check(log.FindAfter(0, P("load").filter, e, lost) && lost == 10 && e.seq == 11, "lost 10, the oldest kept is seq 11: lost " + std::to_string(lost));
    Check(log.FindAfter(0, P("load n=11").filter, e, lost) && e.frame == 11, "the oldest kept by its field");
    Check(!log.FindAfter(0, P("load n=5").filter, e, lost), "an overwritten event is gone");
    Event l;
    Check(log.Last("load", l) && l.seq == Log::kRing + 10, "Last survives the ring");
  }
  // ---- log_event replay WORD: the newest event of that state, not the newest replay line (review: Last filed replay under its kind only)
  {
    Log log;
    log.Add(10, "replay", "\"state\": \"start\"");
    log.Add(20, "replay", "\"state\": \"end\", \"skipped\": true");
    Event e;
    Check(log.Last(LastKey(P("replay start").filter), e) && e.frame == 10, "log_event replay start = the start line");
    Check(log.Last(LastKey(P("replay").filter), e) && e.frame == 20, "log_event replay = the newest replay line");
  }
  // ---- a wait's poll (review): the scan point moves past every event examined, so a long wait over more than kRing non-matching
  // events loses nothing it has read; only events it never examined can be lost, and the wait is told how many
  {
    static Log log;
    log.Add(1, "match", "\"what\": \"clock_started\", \"clock\": 1");
    uint64_t scan = log.Newest();  // the wait's start: its mark
    const Filter pin = P("pin").filter;
    Event e;
    uint64_t lost = 9;
    bool found_early = false, any_lost = false;
    for (int tick = 0; tick < 4; ++tick) {   // four reader ticks with 300 moves before each: 1200 events, more than the ring
      for (int i = 0; i < 300; ++i) log.Add(2, "move", "\"slot\": 0, \"class\": \"finisher\"");
      found_early |= log.Poll(scan, pin, e, lost);
      any_lost |= lost != 0;
    }
    Check(!found_early && !any_lost && scan == log.Newest(), "a long wait over 1200 moves read tick by tick: nothing lost, scan at the newest");
    const uint64_t s_pin = log.Add(3, "match", "\"what\": \"pin\", \"pinning_slot\": 0");
    Check(log.Poll(scan, pin, e, lost) && e.seq == s_pin && lost == 0 && scan == s_pin, "then the pin is found");
    for (uint64_t i = 0; i < Log::kRing + 5; ++i) log.Add(4, "move", "\"slot\": 1");
    Check(!log.Poll(scan, pin, e, lost) && lost == 5, "events the wait never examined are counted lost: " + std::to_string(lost));
  }
  // ---- S110 SCENESRC: the end clause `until KIND ...`: the line
  {
    const Spec s = P("finish finish_name=pinfall 72000 until finish");
    Check(s.filter.text == "match what=finish finish_name=pinfall" && s.timeout == 72000 && !s.optional && s.until.text == "match what=finish",
          "finish + until finish: " + s.filter.text + " | " + s.until.text);
    const Spec o = P("pin 72000 until finish optional");
    Check(o.filter.text == "match what=pin" && o.timeout == 72000 && o.optional && o.until.text == "match what=finish",
          "pin until finish optional: optional belongs to the wait: " + o.until.text);
    const Spec k = P("pin until match what=finish finish_type>=7");
    Check(k.timeout == -1 && k.until.kind == "match" && k.until.conds.size() == 2 && k.until.text == "match what=finish finish_type>=7",
          "an until with fields, no timeout: " + k.until.text);
    Check(P("pin 100").until.kind.empty(), "no until clause: until.kind is empty");
    P("pin 100 until", false);                    // until needs a kind
    P("pin until finish 100", false);             // the timeout goes before until
    P("pin 100 optional until finish", false);    // optional is the line's last word
    P("pin until finish until count", false);     // one until
    P("pin until bogus", false);                  // an unknown kind after until
    P("pin until finish optional optional", false);
  }
  // ---- ended by: the submission finish ends a pinfall wait; the events after it are not reached; the fields are the ending event's
  {
    static Log log;
    log.Add(1, "match", "\"what\": \"clock_started\", \"clock\": 1");
    uint64_t scan = log.Newest();
    const Spec s = P("finish finish_name=pinfall 72000 until finish");
    Event e;
    uint64_t lost = 9;
    log.Add(2, "match", "\"what\": \"pin\", \"pinning_slot\": 0");
    Check(log.Poll(scan, s.filter, &s.until, e, lost) == Log::Hit::kNone && scan == log.Newest() && lost == 0, "a pin: neither the wait nor its end");
    const uint64_t s_sub = log.Add(3, "match", "\"what\": \"finish\", \"finish_type\": 2, \"finish_name\": \"submission\", \"clock\": 2675");
    log.Add(4, "match", "\"what\": \"finish\", \"finish_name\": \"pinfall\"");
    Check(log.Poll(scan, s.filter, &s.until, e, lost) == Log::Hit::kEnd && e.seq == s_sub && scan == s_sub && lost == 0,
          "the submission finish ends the pinfall wait at its own seq");
    std::string v;
    Check(FieldValue(e.fields, "finish_name", v) && v == "submission", "the ending event's fields: " + v);
    // without the clause the same wait reads past the submission to the pinfall (the old two-exit wait)
    uint64_t scan0 = 1;
    Check(log.Poll(scan0, s.filter, e, lost) && e.frame == 4, "no until: the submission is passed over");
  }
  // ---- both: an event that matches the wait AND its end is the match; a pin wait ends only when the finish comes first
  {
    static Log log;
    const Spec s = P("finish finish_name=pinfall 72000 until finish");
    const Spec p = P("pin 72000 until finish");
    Event e;
    uint64_t lost = 0, scan = 0;
    const uint64_t s_pf = log.Add(1, "match", "\"what\": \"finish\", \"finish_name\": \"pinfall\"");
    Check(log.Poll(scan, s.filter, &s.until, e, lost) == Log::Hit::kMatch && e.seq == s_pf, "a pinfall finish: the match, not the end");
    uint64_t scan_p = 0;
    Check(log.Poll(scan_p, p.filter, &p.until, e, lost) == Log::Hit::kEnd && e.seq == s_pf, "a pin wait: the finish came first: ended");
    const uint64_t s_pin = log.Add(2, "match", "\"what\": \"pin\", \"pinning_slot\": 1");
    log.Add(3, "match", "\"what\": \"finish\", \"finish_name\": \"pinfall\"");
    uint64_t scan_after = s_pf;
    Check(log.Poll(scan_after, p.filter, &p.until, e, lost) == Log::Hit::kMatch && e.seq == s_pin, "a pin before the finish: the match");
    const Spec o = P("pin 72000 until finish optional");
    uint64_t scan_o = 0;
    Check(o.optional && log.Poll(scan_o, o.filter, &o.until, e, lost) == Log::Hit::kEnd && e.seq == s_pf, "optional: the same end (the runner warns)");
  }
  // ---- read once with an until: a long wait over more than kRing non-matching events loses nothing it read; the end is found once
  {
    static Log log;
    log.Add(1, "match", "\"what\": \"clock_started\", \"clock\": 1");
    uint64_t scan = log.Newest();
    const Spec p = P("pin 72000 until finish");
    Event e;
    uint64_t lost = 9;
    bool early = false, any_lost = false;
    for (int tick = 0; tick < 4; ++tick) {
      for (int i = 0; i < 300; ++i) log.Add(2, "move", "\"slot\": 0, \"class\": \"finisher\"");
      early |= log.Poll(scan, p.filter, &p.until, e, lost) != Log::Hit::kNone;
      any_lost |= lost != 0;
    }
    Check(!early && !any_lost && scan == log.Newest(), "until: 1200 moves read tick by tick: nothing lost, scan at the newest");
    const uint64_t s_fin = log.Add(3, "match", "\"what\": \"finish\", \"finish_name\": \"submission\"");
    Check(log.Poll(scan, p.filter, &p.until, e, lost) == Log::Hit::kEnd && e.seq == s_fin && scan == s_fin && lost == 0, "then the end");
    Check(log.Poll(scan, p.filter, &p.until, e, lost) == Log::Hit::kNone && scan == s_fin, "from the end's seq it is not read again");
    for (uint64_t i = 0; i < Log::kRing + 5; ++i) log.Add(4, "move", "\"slot\": 1");
    Check(log.Poll(scan, p.filter, &p.until, e, lost) == Log::Hit::kNone && lost == 5, "until: unread events are counted lost: " + std::to_string(lost));
  }
  // ---- two threads: the bus thread adds while the route thread reads with its mark; every seq is seen once, in order
  {
    static Log log;   // static: kRing events of strings are too big for the stack
    const uint64_t kN = 20000;
    std::thread bus([&] {
      for (uint64_t i = 1; i <= kN; ++i) log.Add(i, "match", i % 2 ? "\"what\": \"count\", \"count\": 1" : "\"what\": \"pin\"");
    });
    const Filter any = P("match").filter;
    uint64_t mark = 0, seen = 0, lost_total = 0, prev = 0;
    bool ordered = true;
    for (uint64_t guard = 0; mark < kN && guard < 100 * kN; ++guard) {   // the guard: a broken log must fail, not hang
      Event e;
      uint64_t lost = 0;
      if (log.FindAfter(mark, any, e, lost)) {
        lost_total += lost;
        if (e.seq <= prev) {
          ordered = false;
          break;
        }
        prev = e.seq;
        mark = e.seq;
        ++seen;
      } else {
        lost_total += lost;
        mark += lost;  // as the route does: the lost ones are skipped once
      }
    }
    bus.join();
    Check(ordered, "two threads: seqs strictly increasing");
    Check(seen + lost_total == kN, "two threads: seen " + std::to_string(seen) + " + lost " + std::to_string(lost_total) + " = " + std::to_string(kN));
  }

  std::printf("event_wait: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
