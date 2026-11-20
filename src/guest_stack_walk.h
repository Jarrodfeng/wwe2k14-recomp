// wwe2k14 - guarded guest stack walk (S105 A6W2). PC run 064: the loader watch's FlightDump crashed the game ("Unhandled guest access
// violation: read of guest 0x704AFEA0" on the flight_task helper): FlightGuestThreads followed a saved r1 / back-chain into an unmapped page
// with raw TranslateVirtual + load_and_swap. Pure logic (no SDK includes): `read(addr, &word)` is the caller's guarded aligned read
// (table-checked, returns false for anything unreadable); every read here goes through it; a bad pointer ends THIS walk with a marker.
// Unit: tools\tests\load_watch_test.cpp.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace wwe2k14 {

struct GuestWalkResult {
  std::vector<uint32_t> callers;  // return addresses, innermost first
  const char* end = "";           // why the walk ended: "" = clean (bottom / limit), else a marker
};

// PowerPC back-chain: [sp] = caller's sp, [caller_sp - 8] = the return address saved there. Bounded by max_depth and the thread's stack range.
template <class Read>
inline GuestWalkResult WalkGuestStack(uint32_t sp, uint32_t stack_low, uint32_t stack_high, int max_depth, Read&& read) {
  GuestWalkResult r;
  for (int depth = 0; depth < max_depth; ++depth) {
    if (sp < stack_low || sp + 4 > stack_high) break;
    uint32_t caller_sp = 0, ret = 0;
    if (!read(sp, &caller_sp)) { r.end = "stack word unreadable"; break; }
    if (caller_sp <= sp || caller_sp > stack_high || caller_sp - 8 < stack_low) break;
    if (!read(caller_sp - 8, &ret)) { r.end = "return slot unreadable"; break; }
    r.callers.push_back(ret);
    sp = caller_sp;
  }
  return r;
}

// The text FlightGuestThreads appends to a thread's stack line: one "\n      <- ADDR" per caller (name(addr) formats it), then the marker if the
// walk stopped on an unreadable word. The unit drives this (the production call site is FlightGuestThreads).
template <class Read, class Name>
inline std::string FormatGuestWalk(uint32_t sp, uint32_t stack_low, uint32_t stack_high, int max_depth, Read&& read, Name&& name) {
  const GuestWalkResult walk = WalkGuestStack(sp, stack_low, stack_high, max_depth, read);
  std::string s;
  for (uint32_t ret : walk.callers) s += "\n      <- " + name(ret);
  if (*walk.end) s += std::string("\n      <- (") + walk.end + ")";
  return s;
}

}  // namespace wwe2k14
