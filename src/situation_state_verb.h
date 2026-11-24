// wwe2k14 - the situation director's `state SLOT NAME` verb (S110 OMGROUTE, docs/research/s110/OMGROUTE.md section 3 and "Sites the build will touch").
//
//   state SLOT NAME   DEV BUILDS ONLY. Queues the named game state on wrestler SLOT through the game's own state queue, the door the Lua's `yChgState` uses
//                     (yaseChgState sub_826D9080 +0xA0 / +0xA4: `lwz r3,3148(r11)` then `bl sub_8266B550`; the name there has its leading "State" dropped).
//                     NAME is looked up in the wrestler's OWN state registry, no list of names lives here: descriptor = [[char+3148]+40], [desc] = the count,
//                     [desc+4] = the records, 8 bytes each {+0 constructor, +4 name pointer} (sub_8266B550 +0x10..+0x60; the name search sub_826AC388 reads
//                     [desc+4] and [desc]). A name the registry does not hold is refused and nothing is called.
//
// NOT a stand-alone header: situation_director_core.h includes it INSIDE namespace wwe2k14::situation, after SlotChar / Hex / Fn are declared, so it
// opens no namespace of its own and needs nothing the core has not already included. Pure: no game include (the unit test compiles it with the core).
#pragma once

constexpr uint32_t kStateMachineOff = 3148;  // char+3148 = the state machine (sub_826D9080 0x826D9120 `lwz r3,3148(r11)`); sub_8266B550's r3
constexpr uint32_t kStateDescOff = 40;       // [sm+40] = the registry descriptor (sub_8266B550 0x8266B560 `lwz r3,40(r3)`, 0x8266B56C `lwz r11,40(r30)`)
constexpr uint32_t kStateRecsOff = 4;        // [desc+4] = the records (0x8266B594 `lwz r10,4(r11)`); [desc+0] = the count (0x8266B5A0)
constexpr uint32_t kStateRecStride = 8;      // 8-byte records (0x8266B5AC `rlwinm r11,r29,3,0,28`)
constexpr uint32_t kStateRecNameOff = 4;     // record+4 = the name pointer (sub_826AC270 compares the string it finds there; record+0 is the constructor `lwzx r31,r11,r10`)
constexpr uint32_t kStateCurOff = 12;        // [desc+12] = the current-state object, [that+12] = its index (reviewer, OMGROUTE: nothing is queued when it equals the found index)
constexpr uint32_t kStateCurIdxOff = 12;
constexpr uint32_t kStateCountMax = 4096;    // a sanity bound on the count read from guest memory (a larger one is not a registry)
constexpr uint32_t kStateNameMax = 63;       // names are read up to 63 characters (then the terminator)
constexpr uint32_t kActPtrOff = 28;          // [[char+28]] = the u16 act (OMGROUTE G8: sub_825FBD58 +0x64..+0x84)

// The C string at guest address `p`, or "" when it is longer than kStateNameMax (then it can match no name this verb accepts).
template <class E>
std::string StateName(E& e, uint32_t p) {
  std::string s;
  for (uint32_t i = 0; i <= kStateNameMax; ++i) {
    const uint8_t c = e.U8(p + i);
    if (c == 0) return s;
    s += char(c);
  }
  return "";
}

template <class E>
std::string QueueState(E& e, int slot, const std::string& name) {
  const std::string tag = "state " + std::to_string(slot) + " " + name;
  auto refuse = [&](const std::string& why) {
    e.Log("director: " + tag + ": refused (" + why + ") clock " + e.Clock());
    return "refused: " + tag + " (" + why + ")";
  };
  uint32_t ch = 0;
  const std::string r = SlotChar(e, slot, ch);
  if (!r.empty()) return refuse(r);
  const uint32_t sm = e.U32(ch + kStateMachineOff);
  if (!e.Ok(sm)) return refuse("the wrestler has no state machine, [char+3148] is empty");
  const uint32_t desc = e.U32(sm + kStateDescOff);
  if (!e.Ok(desc)) return refuse("the state machine has no registry, [sm+40] is empty");
  const uint32_t n = e.U32(desc);
  const uint32_t recs = e.U32(desc + kStateRecsOff);
  if (n == 0 || n > kStateCountMax || !e.Ok(recs)) return refuse("the state registry is empty or unreadable");
  int found = -1;
  uint32_t namep = 0;
  // The game's own lookup (sub_826AC270 via sub_826AC388) is a binary search with EXCLUSIVE bounds: records 0 and n-1 are never found by it, and a name it
  // cannot find queues the last record (index 839 is out of range, the table is fixed and sorted). So only records 1..n-2 are reachable by name: a name that
  // exists only at 0 or n-1 is refused like an unknown one, never queued.
  for (uint32_t i = 1; i + 1 < n; ++i) {
    const uint32_t p = e.U32(recs + kStateRecStride * i + kStateRecNameOff);
    if (!e.Ok(p)) continue;
    const std::string s = StateName(e, p);
    if (s == name) {
      found = int(i);
      namep = p;
      break;
    }
  }
  if (found < 0) return refuse("no such state among records 1..n-2 of the wrestler's registry (n = " + std::to_string(n) + "; the game's search never finds record 0 or the last)");
  const uint32_t actp = e.U32(ch + kActPtrOff);
  const uint32_t act = e.Ok(actp) ? (uint32_t(e.U8(actp)) << 8 | e.U8(actp + 1)) : 0;  // big-endian u16 at [char+28]
  const uint32_t cur = e.U32(desc + kStateCurOff);  // [[desc+12]+12] = the index of the state the machine is in now; the game queues nothing for that index
  if (e.Ok(cur) && e.U32(cur + kStateCurIdxOff) == uint32_t(found)) {
    e.Log("director: " + tag + ": already in it (record " + std::to_string(found) + "), nothing queued, act 0x" + Hex(act) + " clock " + e.Clock());
    return "ok: " + tag + " (already in it, nothing queued)";
  }
  e.Call3(Fn::QueueState, sm, namep, 0);  // the return value is not a success flag (not read)
  e.Log("director: " + tag + ": queued (record " + std::to_string(found) + ", name 0x" + Hex(namep) + ", act was 0x" + Hex(act) +
        "; the game only QUEUES the state, it takes effect on a later tick) clock " + e.Clock());
  return "ok: " + tag + " (queued; takes effect on a later tick)";
}
