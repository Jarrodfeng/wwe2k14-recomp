// wwe2k14 - S110 TABLEEVENT: the pure part of the bus event `break what=table` (docs\research\s110\TABLEEVENT.md). No SDK, no guest reads.
// Unit: tools\bus\table_break_test.cpp (py -I tools\bus\table_break_selftest.py). The hook that feeds it: src\table_break.h.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace wwe2k14::table_break {

// sub_827B5D50 (the body-hit path's set-motion helper) calls object_set_motion at +104; this is that call's return address. Run 250's
// break stack at sub_825E1960: 0x823C9368 (lr) <- 0x823CA570 <- 0x823CA76C <- 0x827B5E58. The event's hook is the latch writer
// sub_825DDEC0, called from inside sub_825E1960, so its walk finds 0x823C9368 (sub_823C9210's slot +40 call, multi track) or 0x823C94DC
// (sub_823C9390's, single track), then 0x823CA570 (sub_823CA4B0's slot +32 call of sub_823C9210) or 0x823CA5A8 (its direct call of
// sub_823C9390 at 0x823CA5A4), then 0x823CA76C, 0x827B5E58: the 4th return address (S110 TABLELATCH). Index 3 exactly, no slack, by the
// fixed chain sub_827B5D50 -> object_set_motion -> sub_823CA4B0 -> track function -> sub_825E1960 -> writer; deeper is another chain.
inline constexpr uint32_t kRetHitSetMotion = 0x827B5E58;
inline constexpr int kCallerDepth = 4;  // the walk's depth: the hit path's return address sits at index 3 from the writer

inline bool IsTableKind(int64_t kind) { return kind == 5 || kind == 9; }

// The break: one write of a table's latch [w+1464] (slot +88 sub_825DDEC0 / slot +84 sub_825DDFA0, the broken-model swap) across which
// it went 0 -> 1. -1 = unreadable, never a break.
inline bool IsBreak(int64_t kind, int64_t latch_before, int64_t latch_after) {
  return IsTableKind(kind) && latch_before == 0 && latch_after == 1;
}

// Did this start come through sub_827B5D50? (its return address among the first kCallerDepth guest return addresses)
inline bool ViaHit(const uint32_t* callers, size_t n) {
  for (size_t i = 0; i < n && i < size_t(kCallerDepth); ++i) {
    if (callers[i] == kRetHitSetMotion) return true;
  }
  return false;
}

struct Break {
  uint32_t obj = 0;
  int64_t act = -1, previous = -1, track = -1, kind = -1, on_fire = -1;
  bool via_hit = false;
  int64_t body_slot = -1, body_id = -1;  // only on the hit path: the slot / character id of the hit command's [cmd+36]
};

// The event's fields (a JSON fragment for events::Emit). body_* only when via = hit.
inline std::string Fields(const Break& b) {
  char buf[384];
  int n = std::snprintf(buf, sizeof buf,
                        "\"what\": \"table\", \"obj\": \"0x%08X\", \"act\": %lld, \"previous\": %lld, \"track\": %lld, \"weapon_kind\": %lld, "
                        "\"on_fire\": %lld, \"via\": \"%s\"",
                        unsigned(b.obj), static_cast<long long>(b.act), static_cast<long long>(b.previous),
                        static_cast<long long>(b.track), static_cast<long long>(b.kind), static_cast<long long>(b.on_fire),
                        b.via_hit ? "hit" : "other");
  std::string s(buf, n > 0 ? size_t(n) : 0);
  if (b.via_hit) {
    n = std::snprintf(buf, sizeof buf, ", \"body_slot\": %lld, \"body_id\": %lld", static_cast<long long>(b.body_slot),
                      static_cast<long long>(b.body_id));
    s.append(buf, n > 0 ? size_t(n) : 0);
  }
  return s;
}

// Finish type 2 is decision kind 1 (table 0x8201EB58). The table-hit decision sub_827B5B98 records kind 1 only under rule byte G+39 or
// G+18 (G = 0x8376AAF4); the referee's 10-count (mode 5, sub_825B5780) records it too, and the referee enters mode 5 only when the
// count-out rule byte G+26 is set (sub_825B3CF0 +414..+4C0; G+26 == 2 also makes it a 5-count, sub_825B5780 +1C4). bit 0 = G+18 set,
// bit 1 = G+39 set, bit 2 (kCountOutRule) = G+26 set.
// The name is "table" only when a table rule is set AND the count-out rule is not: then the 10-count cannot record kind 1. With both set
// both recorders can, and the bus cannot tell which one did (the structural answer would mark the decision where it is recorded): it
// stays "count-out" and the finish line carries the bits.
inline constexpr int kCountOutRule = 4;
inline int TableRuleBits(uint8_t g18, uint8_t g39) { return (g18 ? 1 : 0) | (g39 ? 2 : 0); }
inline const char* Kind1Name(int rule_bits) { return (rule_bits & 3) && !(rule_bits & kCountOutRule) ? "table" : "count-out"; }

}  // namespace wwe2k14::table_break
