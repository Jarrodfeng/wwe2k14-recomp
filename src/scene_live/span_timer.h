// Session 101 lane 122 (KR, docs\renderer\SKIP_GAME_WORK_MAP.md 1.3 items 1-2): what a recording span WROTE. lane 83's cost counters (src\cost_counters.h) give a span's time;
// this adds the records it appended to the drawing thread's command list, by tag, so "what does skipping an object save" has both halves: ms and records.
// No game or SDK includes: the memory is reached through a small accessor, so the unit test (tools\thin\) runs this exact header on fake guest memory.
//
// The list (read in the listing of sub_82BB1CF8 at 0x82BB1D04-0x82BB1DCC and in db sub_82C26E00, both [C]):
//   G      = word at 0x8392D9B8
//   thread = word at ([r13] + 1048); index = thread ? word at (thread + 32) : 0
//   buffer = word at (word at (G + 156) + 4 * index)            the buffer number of this thread
//   desc   = word at (G + 104) + 68 * buffer                    the 68-byte descriptor: +0 the buffer's base, +4 the cursor (in 16-byte units)
//   a record at cursor c starts at base + 16 * c: word 0 = the tag (opcode), word 2 = its payload in 16-byte units; the next record is at c + word 2 + 1.
// A span's records = the walk from the cursor at its start to the cursor at its end; the walk must land EXACTLY on the end cursor (else `walk_bad` counts it and the span's
// tag counts are dropped: a wrong stride, a list that was reset inside the span, a thread that changed).
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace wwe2k14::spans {

inline constexpr uint32_t kListGlobal = 0x8392D9B8u;
inline constexpr uint32_t kThreadWord = 1048, kThreadIndex = 32, kGDescriptors = 104, kGIndices = 156;
inline constexpr uint32_t kDescriptorStride = 68;  // [C] mulli r9,r7,68 in sub_82BB1CF8 (the plant `descriptor_stride_64` reads 64)
inline constexpr int kTags = 128;
inline constexpr int kClasses = 6;  // cost_counters.h Cls order: arena wrestlers ring flash crowd other

// A big-endian guest memory view. `Ld32(addr)` returns 0 for an address outside the view (a real view faults earlier; a fake returns 0).
template <class Mem>
struct Cursor {
  bool ok = false;
  uint32_t base = 0;    // the buffer's guest address
  uint32_t cursor = 0;  // 16-byte units
  uint32_t buffer = 0;
};

// The current thread's list cursor. `r13` is the guest thread-block register. `stride` is a parameter only so the unit test can plant the wrong one.
template <class Mem>
inline Cursor<Mem> ReadCursor(const Mem& mem, uint32_t r13, uint32_t stride = kDescriptorStride) {
  Cursor<Mem> c;
  const uint32_t g = mem.Ld32(kListGlobal);
  if (!g) return c;
  const uint32_t block = mem.Ld32(r13);
  const uint32_t thread = block ? mem.Ld32(block + kThreadWord) : 0;
  const uint32_t index = thread ? mem.Ld32(thread + kThreadIndex) : 0;
  const uint32_t indices = mem.Ld32(g + kGIndices), descriptors = mem.Ld32(g + kGDescriptors);
  if (!indices || !descriptors) return c;
  c.buffer = mem.Ld32(indices + 4 * index);
  const uint32_t desc = descriptors + stride * c.buffer;
  c.base = mem.Ld32(desc);
  c.cursor = mem.Ld32(desc + 4);
  c.ok = c.base != 0;
  return c;
}

// What one span wrote, by tag.
struct SpanDelta {
  uint32_t records = 0, units = 0;
  uint32_t by_tag[kTags] = {};
  bool walk_ok = false;
};

// Walks [from, to) of a buffer. Exact landing is the check: false = the cursors do not describe whole records.
template <class Mem>
inline SpanDelta Walk(const Mem& mem, uint32_t base, uint32_t from, uint32_t to) {
  SpanDelta d;
  if (to < from || to - from > (1u << 20)) return d;
  uint32_t at = from;
  while (at < to) {
    const uint32_t record = base + 16 * at;
    const uint32_t tag = mem.Ld32(record), payload = mem.Ld32(record + 8);
    if (payload > (1u << 16)) return d;
    ++d.records;
    ++d.by_tag[tag < uint32_t(kTags) ? tag : kTags - 1];
    at += payload + 1;
  }
  d.units = to - from;
  d.walk_ok = at == to;
  return d;
}

// The totals per class (relaxed atomics: the spans run on the game's threads; the line is read on the render thread).
struct Totals {
  std::atomic<uint64_t> spans[kClasses] = {}, records[kClasses] = {}, units[kClasses] = {}, walk_bad[kClasses] = {}, no_cursor[kClasses] = {};
  std::atomic<uint64_t> by_tag[kClasses][kTags] = {};
};
inline Totals g_totals;
inline std::atomic<bool> g_on{true};  // (the hook gates on cost::On() and the outermost span; this is the unit test's switch)

inline void Add(Totals& t, int cls, const SpanDelta& d) {
  if (!d.walk_ok) {
    t.walk_bad[cls].fetch_add(1, std::memory_order_relaxed);
    return;
  }
  t.spans[cls].fetch_add(1, std::memory_order_relaxed);
  t.records[cls].fetch_add(d.records, std::memory_order_relaxed);
  t.units[cls].fetch_add(d.units, std::memory_order_relaxed);
  for (int i = 0; i < kTags; ++i)
    if (d.by_tag[i]) t.by_tag[cls][i].fetch_add(d.by_tag[i], std::memory_order_relaxed);
}

// Around one span (the outermost one of a thread, like cost::RecordSpan): the cursor at the start, and at the end the walk. `Mem` is the guest view; `r13` the thread block.
template <class Mem>
class CursorSpan {
 public:
  CursorSpan(const Mem& mem, uint32_t r13, uint32_t stride = kDescriptorStride, bool enabled = true) : mem_(mem), r13_(r13), stride_(stride), on_(enabled && g_on.load(std::memory_order_relaxed)) {
    if (on_) start_ = ReadCursor(mem_, r13_, stride_);
  }
  bool on() const { return on_; }
  // The class is known only after the span (cost::ClassOf of the object), so the caller names it here.
  void Finish(int cls) {
    if (!on_ || done_) return;
    done_ = true;
    if (!start_.ok) {
      g_totals.no_cursor[cls].fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const Cursor<Mem> end = ReadCursor(mem_, r13_, stride_);
    if (!end.ok || end.buffer != start_.buffer || end.base != start_.base) {  // the thread moved to another buffer (or the list was reset) inside the span
      Add(g_totals, cls, SpanDelta{});
      return;
    }
    Add(g_totals, cls, Walk(mem_, start_.base, start_.cursor, end.cursor));
  }
  ~CursorSpan() = default;  // (no class, no count: the caller calls Finish)
  CursorSpan(const CursorSpan&) = delete;
  CursorSpan& operator=(const CursorSpan&) = delete;

 private:
  const Mem& mem_;
  uint32_t r13_, stride_;
  bool on_, done_ = false;
  Cursor<Mem> start_;
};

// "[rdr][span] records per span {arena a/b ...}": per class, records and 16-byte units a span wrote (averages), the top tags, and the spans whose walk did not land.
// Printed every 600 frames beside the cost line; empty while nothing was counted.
inline std::string Line(uint64_t frames_window) {
  std::string out;
  char text[256];
  for (int c = 0; c < kClasses; ++c) {
    static const char* const kName[kClasses] = {"arena", "wrestlers", "ring", "flash", "crowd", "other"};
    const uint64_t n = g_totals.spans[c].load(std::memory_order_relaxed), bad = g_totals.walk_bad[c].load(std::memory_order_relaxed), none = g_totals.no_cursor[c].load(std::memory_order_relaxed);
    if (!n && !bad && !none) continue;
    const uint64_t records = g_totals.records[c].load(std::memory_order_relaxed), units = g_totals.units[c].load(std::memory_order_relaxed);
    std::snprintf(text, sizeof text, "%s%s spans/f %.1f records/span %.1f units/span %.1f", out.empty() ? "" : "; ", kName[c], frames_window ? double(n) / double(frames_window) : 0.0,
                  n ? double(records) / double(n) : 0.0, n ? double(units) / double(n) : 0.0);
    out += text;
    // the three busiest tags
    int top[3] = {-1, -1, -1};
    for (int t = 0; t < kTags; ++t) {
      const uint64_t v = g_totals.by_tag[c][t].load(std::memory_order_relaxed);
      if (!v) continue;
      for (int k = 0; k < 3; ++k) {
        if (top[k] < 0 || v > g_totals.by_tag[c][top[k]].load(std::memory_order_relaxed)) {
          for (int j = 2; j > k; --j) top[j] = top[j - 1];
          top[k] = t;
          break;
        }
      }
    }
    out += " tags {";
    for (int k = 0; k < 3 && top[k] >= 0; ++k) {
      std::snprintf(text, sizeof text, "%sop%d %.1f", k ? " " : "", top[k], n ? double(g_totals.by_tag[c][top[k]].load(std::memory_order_relaxed)) / double(n) : 0.0);
      out += text;
    }
    out += "}";
    if (bad || none) {
      std::snprintf(text, sizeof text, " (walk bad %llu, no cursor %llu)", (unsigned long long)bad, (unsigned long long)none);
      out += text;
    }
  }
  return out.empty() ? out : "[rdr][span] " + out;
}

}  // namespace wwe2k14::spans
