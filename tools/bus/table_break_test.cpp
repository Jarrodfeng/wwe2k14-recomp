// S110 TABLEEVENT: the unit of src/table_break_core.h (pure: no SDK, no game). Built and run by tools\bus\table_break_selftest.py.
#include <cstdint>
#include <cstdio>
#include <string>

#include "table_break_core.h"

using namespace wwe2k14::table_break;

static int fails = 0;
static void Check(bool ok, const std::string& what) {
  if (!ok) {
    ++fails;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

int main() {
  // the break: a table's latch [w+1464] 0 -> 1 across one motion start
  Check(IsBreak(5, 0, 1), "kind 5, 0 -> 1 is a break");
  Check(IsBreak(9, 0, 1), "kind 9, 0 -> 1 is a break");
  Check(!IsBreak(5, 1, 1), "an already broken table: no second event (run 250's act 1 after 15381)");
  Check(!IsBreak(5, 0, 0), "a hit that leaves the table whole (run 250's 15370 / 15372)");
  Check(!IsBreak(3, 0, 1), "not a table kind");
  Check(!IsBreak(5, -1, 1), "an unreadable latch before");
  Check(!IsBreak(5, 0, -1), "an unreadable latch after");
  Check(!IsBreak(5, 1, 0), "the reset direction");
  // the hit path: run 250's 15381 chain, and the 15001 set-up chain of the same run (log line 1955)
  const uint32_t hit[] = {0x823CA570u, 0x823CA76Cu, 0x827B5E58u, 0x827B7CD4u};
  Check(ViaHit(hit, 4), "run 250's break chain is the hit path");
  const uint32_t setup[] = {0x823CA570u, 0x823CA88Cu, 0x8258EBA0u, 0x8258F11Cu};
  Check(!ViaHit(setup, 4), "another caller");
  const uint32_t deep[] = {1u, 2u, 3u, 4u, 0x827B5E58u};
  Check(!ViaHit(deep, 5), "only the first kCallerDepth return addresses count");
  Check(!ViaHit(nullptr, 0), "an empty walk");
  // S110 TABLELATCH: the event's hook is the latch writer, one frame below sub_825E1960: the hit path's return address is index 3
  const uint32_t writer_multi[] = {0x823C9368u, 0x823CA570u, 0x823CA76Cu, 0x827B5E58u, 0x827B7CD4u};
  Check(ViaHit(writer_multi, 5), "the writer's walk on the multi-track break chain is the hit path (index 3)");
  const uint32_t writer_single[] = {0x823C94DCu, 0x823CA5A8u, 0x823CA76Cu, 0x827B5E58u};  // sub_823CA4B0 calls sub_823C9390 at 0x823CA5A4
  Check(ViaHit(writer_single, 4), "the writer's walk on the single-track chain is the hit path (index 3)");
  // sub_82684948 +4BC: slot +88(1) BEFORE the table's 15378 / 15379 start: the write is the break, the start after it is not
  Check(IsBreak(5, 0, 1), "the dive path's latch write (0 -> 1) is the break");
  Check(!IsBreak(5, 1, 1), "the start that follows it finds the latch already 1: no second event");
  // message bit 4 (sub_82741AF8) after a 15381 start: slot +88(1) on a latch already 1 stores nothing
  Check(!IsBreak(9, 1, 1), "the message's write after the start: no second event");
  // a reset (r4 = 0) and then a new break: two events for two breaks of one table
  Check(!IsBreak(5, 1, 0) && IsBreak(5, 0, 1), "after a reset a new break is a new event");
  // the fields
  Break b{0xA2C9C9D0u, 15381, 15001, 0, 5, 0, true, 1, 324};
  const std::string f = Fields(b);
  Check(f == "\"what\": \"table\", \"obj\": \"0xA2C9C9D0\", \"act\": 15381, \"previous\": 15001, \"track\": 0, \"weapon_kind\": 5, "
             "\"on_fire\": 0, \"via\": \"hit\", \"body_slot\": 1, \"body_id\": 324",
        "fields on the hit path: " + f);
  b.via_hit = false;
  const std::string g = Fields(b);
  Check(g.find("body_slot") == std::string::npos && g.find("body_id") == std::string::npos && g.find("\"via\": \"other\"") != std::string::npos,
        "no body field off the hit path: " + g);
  // finish type 2's name by the rule bits
  Check(TableRuleBits(0, 0) == 0 && TableRuleBits(1, 0) == 1 && TableRuleBits(0, 7) == 2 && TableRuleBits(2, 1) == 3, "rule bits");
  Check(std::string(Kind1Name(0)) == "count-out", "no table rule: count-out");
  Check(std::string(Kind1Name(1)) == "table" && std::string(Kind1Name(2)) == "table" && std::string(Kind1Name(3)) == "table",
        "a table rule: table");
  // S110 TABLEEVENT review: the referee enters its 10-count (mode 5, sub_825B5780, kind 1) only when the count-out rule byte G+26 is set
  // (sub_825B3CF0 +414..+4C0). With a table rule AND G+26 both set, both recorders can write kind 1 in one match: the name stays count-out.
  // 4 = kCountOutRule (G+26 != 0).
  Check(std::string(Kind1Name(1 | 4)) == "count-out" && std::string(Kind1Name(2 | 4)) == "count-out" &&
            std::string(Kind1Name(3 | 4)) == "count-out",
        "a table rule with the count-out rule on: count-out (the referee's 10-count can record kind 1 there too)");
  Check(std::string(Kind1Name(4)) == "count-out", "the count-out rule alone: count-out");
  std::printf("table_break unit: %d failures\n", fails);
  return fails ? 1 : 0;
}
