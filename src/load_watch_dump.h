// wwe2k14 - what the loader watch's first-fire dump reads to settle H3 vs H1 / H2 (S105 A6W2; docs/research/s105/A6.md "Still needed").
// Pure logic, no SDK includes (unit test tools\tests\load_watch_test.cpp): every guest word goes through `read(addr, &word)` (the caller's
// table-checked SafeRead32: false = unreadable, never a fault); no guest call is made. Every line is LOG ONLY.
//
// H3 = the worker spins in sub_82C54BB0 on a full loader heap (the heap object [0x839400AC]). sub_82C2DF00 computes the heap statistics;
// BuildHeapStats reproduces its arithmetic over the same free list (listing generated/default/wwe2k14_recomp.111.cpp:17166, read for this
// lane): table = [0x8392FFE4] + 36 * index, entry +0 = start, +4 = end; a node at address p holds w0 = [p]: low 28 bits = the node's END in 16-byte
// units from the heap start, w8 = [p+8]: 0 -> counted in the "flag 0" totals (sub_82C2DF00's r5 / r6), non-zero -> the "flag 1" totals (r7 /
// r8, and r4 = the largest). Node length (16-byte units) = (w0 & 0xFFFFFFF) - (((p - start) >> 4) & 0xFFFFFFF); next node = ((w0 & 0xFFFFFFF) << 4) + start.
// Which flag value means "free" is NOT confirmed (unconfirmed: the names stay neutral); the dump prints both.
// The heap's table index: [[0x839400AC] + 76] (the word sub_82C54BB0 passes to sub_82C2DE28; [L]: the same index sub_82C2DF00 takes). The dump
// prints the object pointer, the index and the table entry's own words, so a wrong index is visible.
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <cstdio>
#include "load_watch.h"  // Sample

namespace wwe2k14::load_probe {

struct HeapStats {
  bool ok = false;               // the table entry and every node word read
  const char* stop = "";         // why the walk ended early ("" = reached the end)
  uint32_t start = 0, end = 0;   // the table entry's +0 / +4
  uint32_t nodes = 0;
  uint64_t flag0_blocks = 0, flag0_bytes = 0;                    // w8 == 0
  uint64_t flag1_blocks = 0, flag1_bytes = 0, flag1_largest = 0;  // w8 != 0
};

template <class Read>
inline HeapStats BuildHeapStats(Read&& read, uint32_t table, uint32_t index, uint32_t max_nodes = 20000) {
  HeapStats h;
  uint32_t count = 0;
  if (!read(0x8392FFDCu, &count) || index >= count) { h.stop = "index out of range"; return h; }
  const uint32_t entry = table + 36u * index;
  if (!read(entry, &h.start) || !read(entry + 4, &h.end)) { h.stop = "heap table unreadable"; return h; }
  uint32_t p = h.start;
  while (p < h.end) {
    if (h.nodes >= max_nodes) { h.stop = "truncated"; return h; }
    uint32_t w0 = 0, w8 = 0;
    if (!read(p, &w0) || !read(p + 8, &w8)) { h.stop = "node unreadable"; return h; }
    const uint32_t len = (w0 & 0xFFFFFFFu) - (((p - h.start) >> 4) & 0xFFFFFFFu);
    ++h.nodes;
    if (w8 == 0) ++h.flag0_blocks, h.flag0_bytes += uint64_t(len) << 4;
    else {
      ++h.flag1_blocks, h.flag1_bytes += uint64_t(len) << 4;
      if (uint64_t(len) << 4 > h.flag1_largest) h.flag1_largest = uint64_t(len) << 4;
    }
    const uint32_t next = ((w0 & 0xFFFFFFFu) << 4) + h.start;
    if (next <= p) { h.stop = "node chain does not advance"; return h; }
    p = next;
  }
  h.ok = true;
  return h;
}

// The loader queue's list head: [[0x839400A8]+4] (0 = empty). [0x839400A8] itself is the queue OBJECT, never 0 after boot (run 064's false fire:
// the watch counted it as "something queued" and fired on every idle 5 s). `plausible` = the caller's pointer plausibility check.
// REVIEW (A6W2 finding 1): [[0x839400A8]+4] is not "the queued requests": it is a POOL of request slots, never empty after the first load (the hand-off
// sub_82C55530 re-appends each finished request; the enqueue sub_82C55268 -> sub_82C555E0 reuses the first slot whose +104 is 0 / 11 / 12). So this
// walks the list the way the dequeue sub_82C54FA8 does (listing 161.cpp:17572: head = [q+4]; loop { node = [cur+0]; item = [node+8]; busy if
// [item+104] in {1, 13, 15} or [item+108] == 3; cur = node; until [node+0] == 0 }, every word through `read`, capped at 256 nodes) and returns the FIRST BUSY
// ITEM (0 = nothing waiting to be dequeued). That is the dequeue's own test, so "queue non-empty" means what the game means by it.
template <class Read, class Plausible>
inline uint32_t SampleQueueHead(Read&& read, Plausible&& plausible) {
  uint32_t qobj = 0, cur = 0;
  if (!read(0x839400A8u, &qobj) || !plausible(qobj) || !read(qobj + 4, &cur) || cur == 0) return 0;
  for (int i = 0; i < 256; ++i) {
    uint32_t node = 0, item = 0, st = 0, w108 = 0;
    if (!read(cur, &node) || node == 0) return 0;
    if (!read(node + 8, &item) || !plausible(item)) return 0;
    if (!read(item + 104, &st) || !read(item + 108, &w108)) return 0;
    if (st == 1 || st == 13 || st == 15 || w108 == 3) return item;
    cur = node;
  }
  return 0;
}

inline std::string W(bool ok, uint32_t v) {
  char b[16];
  if (!ok) return "????????";
  std::snprintf(b, sizeof b, "%08X", v);
  return b;
}

// The worker's CPU share over the stall window: the host thread's CPU time at the start of the window (re-armed whenever the window's start
// changes) against now. Pure; the thread-time source (GetThreadTimes / /proc/self/task/<tid>/stat) is the caller's.
struct CpuWindow {
  int64_t base_since_us = -1, base_cpu_us = 0, base_wall_us = 0;
  void Note(int64_t since_us, int64_t now_us, int64_t cpu_us) {
    if (cpu_us < 0) return;
    if (since_us != base_since_us) base_since_us = since_us, base_cpu_us = cpu_us, base_wall_us = now_us;
  }
  // -1 = unknown (no source, or no wall time).
  double Percent(int64_t now_us, int64_t cpu_us) const {
    if (cpu_us < 0 || base_since_us < 0 || now_us <= base_wall_us) return -1.0;
    return 100.0 * double(cpu_us - base_cpu_us) / double(now_us - base_wall_us);
  }
};

inline std::string CpuText(double pct) {
  char b[48];
  if (pct < 0) return "worker cpu unknown over the window";
  std::snprintf(b, sizeof b, "worker cpu %.0f %% over the window", pct);
  return b;
}

// The lines, in production order. `req` = the current request (0 = none), `c` = [0x8394024C] (0 if unreadable).
template <class Read>
inline std::vector<std::string> BuildDumpLines(Read&& read, uint32_t req, uint32_t c) {
  std::vector<std::string> out;
  uint32_t v = 0;
  auto rd = [&](uint32_t a) { return read(a, &v) ? W(true, v) : W(false, 0); };
  std::string l = "[wwe2k14][load] WATCH h3 fields: request 0x" + W(true, req) + " +36=" + (req ? rd(req + 36) : std::string("-")) +
                  " +116=" + (req ? rd(req + 116) : std::string("-")) + " +104=" + (req ? rd(req + 104) : std::string("-")) +
                  " +96=" + (req ? rd(req + 96) : std::string("-")) + " +44=" + (req ? rd(req + 44) : std::string("-"));
  l += " | read-end object 0x" + W(true, c) + " +36=" + (c ? rd(c + 36) : std::string("-")) + " +40=" + (c ? rd(c + 40) : std::string("-"));
  out.push_back(l);
  {
    uint32_t qobj = 0;
    const bool qok = read(0x839400A8u, &qobj) && qobj != 0;
    out.push_back("[wwe2k14][load] WATCH h2 fields: queue object [0x839400A8]=" + W(qok, qobj) + " list head [[0x839400A8]+4]=" +
                  (qok ? rd(qobj + 4) : std::string("-")) + " pause gate [0x8362A0B0]=" + rd(0x8362A0B0u));
  }
  uint32_t obj = 0, idx = 0, table = 0;
  const bool have_obj = read(0x839400ACu, &obj) && obj != 0 && read(obj + 76, &idx) && read(0x8392FFE4u, &table);
  if (!have_obj) {
    out.push_back("[wwe2k14][load] WATCH loader heap: object [0x839400AC] or its index (+76) or the heap table [0x8392FFE4] unreadable");
    return out;
  }
  const HeapStats h = BuildHeapStats(read, table, idx);
  char b[512];
  std::snprintf(b, sizeof b,
                "[wwe2k14][load] WATCH loader heap: object 0x%08X index %u (obj+76) table 0x%08X range 0x%08X-0x%08X (%llu B), %u nodes walked%s%s; "
                "w8==0 (flag 0): %llu blocks %llu B; w8!=0 (flag 1): %llu blocks %llu B, largest %llu B (which flag is free: unconfirmed)",
                obj, idx, table, h.start, h.end, (unsigned long long)(h.end - h.start), h.nodes, *h.stop ? ", stopped: " : "", h.stop,
                (unsigned long long)h.flag0_blocks, (unsigned long long)h.flag0_bytes, (unsigned long long)h.flag1_blocks,
                (unsigned long long)h.flag1_bytes, (unsigned long long)h.flag1_largest);
  out.push_back(b);
  return out;
}

// The read-end thread is the guest thread named "PacComplete" / FileReadEndProcessThread (docs/research/s105/A6.md).
inline bool IsReadEndName(const std::string& n) {
  auto has = [&](const char* s) { return n.find(s) != std::string::npos; };
  return has("PacComplete") || has("ReadEnd") || has("FileRead");
}

// Thin pure entry points the watch thread's tick calls (and the unit drives: the tick itself needs the SDK).
template <class Read, class Plausible>
inline void FillQueueHead(Sample& s, Read&& read, Plausible&& plausible) { s.queue = SampleQueueHead(read, plausible); }
inline std::string CpuTextFor(const CpuWindow& w, int64_t now_us, int64_t cpu_us) { return CpuText(w.Percent(now_us, cpu_us)); }
template <class Read>
inline std::vector<std::string> FireLines(Read&& read, uint32_t req, uint32_t c) { return BuildDumpLines(read, req, c); }
// "read-end thread" / "worker" / "" for a guest thread (name, host tid) given the worker's host tid.
inline const char* WhichLoaderThread(const std::string& name, uint32_t host_tid, uint32_t worker_tid) {
  if (IsReadEndName(name)) return "read-end thread";
  if (worker_tid != 0 && host_tid == worker_tid) return "worker";
  return "";
}

}  // namespace wwe2k14::load_probe
