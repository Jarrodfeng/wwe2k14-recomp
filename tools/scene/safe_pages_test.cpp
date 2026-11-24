// Offline unit for src\scene_live\safe_guest_pages.h: readable / unreadable page -> true / false, no memory touched (so no crash); the physical walk's mapping against src\guest_view.h.
// Build + run: pwsh -File tools\scene\build_safe_pages_test.ps1
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
#include "guest_view.h"
#include "scene_live/safe_guest_pages.h"
using wwe2k14::safe_pages::RangeReadable;
using wwe2k14::safe_pages::PhysReadable;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
int main() {
  auto ok = [](uint32_t a) { return a >= 0x10000 && a < 0x14000; };   // pages 0x10000..0x13FFF readable
  CHECK(RangeReadable(0x10000, 4, ok));
  CHECK(RangeReadable(0x10FFC, 4, ok));
  CHECK(RangeReadable(0x10FFD, 0x3003, ok));          // ends exactly at the end of the last readable page
  CHECK(!RangeReadable(0x10FFD, 0x3004, ok));         // spills one byte into 0x14000
  CHECK(!RangeReadable(0x13FFE, 4, ok));              // straddles into an unreadable page
  CHECK(!RangeReadable(0x0FFFC, 8, ok));              // starts before the readable range
  CHECK(!RangeReadable(0x20000, 4, ok));
  CHECK(RangeReadable(0x10000, 0, ok));               // n == 0 reads nothing: true, as the Windows SafeCopy
  CHECK(RangeReadable(0x20000, 0, ok));
  CHECK(!RangeReadable(0xFFFFFFFCu, 8, [](uint32_t) { return true; }));   // wraps past 4 GB
  CHECK(RangeReadable(0xFFFFFFFCu, 4, [](uint32_t) { return true; }));

  // Physical: a texture the game binds through the 0xE0000000 view. The GPU address is ToPhysical(cpu); the copy reads raw physical bytes at that address; the table that controls them is the physical heap at the same address.
  // The OLD rule checked the CPU view address's own heap (0xE0000000 + physical): one page off for this view.
  const uint32_t cpu = 0xFC545000u, gpu = wwe2k14::guest_view::ToPhysical(cpu);   // GPU 0x1C546000
  auto old_rule_page = [](uint32_t phys) { return wwe2k14::guest_view::ToPhysical(0xE0000000u + phys); };   // the physical heap address the old check reached for guest 0xE0000000 + phys
  CHECK(old_rule_page(gpu) != gpu);                    // the old rule asked about another page (this is what failed)
  auto only_gpu_page = [gpu](uint32_t p) { return p >= (gpu & ~0xFFFu) && p < (gpu & ~0xFFFu) + 0x2000; };   // exactly the texture's two pages are mapped
  CHECK(PhysReadable(gpu, 0x2000, only_gpu_page));
  CHECK(!PhysReadable(gpu, 0x2001, only_gpu_page));
  CHECK(!PhysReadable(old_rule_page(gpu) + 0x2000, 16, only_gpu_page));
  CHECK(PhysReadable(0xA0000000u | gpu, 16, only_gpu_page));   // the other views carry the low 29 bits only
  CHECK(PhysReadable(0xC0000000u | gpu, 16, only_gpu_page));
  CHECK(!PhysReadable(0x1FFFFFF0u, 0x20, [](uint32_t) { return true; }));   // leaves the 512 MB
  CHECK(PhysReadable(0x1FFFFFF0u, 0x10, [](uint32_t) { return true; }));
  CHECK(PhysReadable(0x1000, 0, [](uint32_t) { return false; }));

  // The per-frame cache: a readable page is asked once a frame; the next frame asks again; unreadable pages are never cached.
  {
    wwe2k14::safe_pages::PageCache<> c;
    uint64_t lk = 0, hit = 0, asks = 0;
    bool mapped = true;
    auto raw = [&](uint32_t) { ++asks; return mapped; };
    auto run = [&](uint64_t frame, uint32_t addr) { return RangeReadable(addr, 4, wwe2k14::safe_pages::Cached(c, frame, lk, hit, raw)); };
    CHECK(run(7, 0x30000) && asks == 1 && hit == 0);
    CHECK(run(7, 0x30010) && asks == 1 && hit == 1);        // same page, same frame: remembered
    mapped = false;
    CHECK(run(7, 0x30FF0));                                 // still remembered inside the frame
    CHECK(!run(8, 0x30000) && asks == 2);                   // next frame: asked again, now refused
    CHECK(!run(8, 0x30000) && asks == 3);                   // unreadable is not cached
    mapped = true;
    CHECK(run(8, 0x30000) && asks == 4 && run(8, 0x30000) && asks == 4);
    // PLANT: a cache that never clears must fail the frame-N+1 check above.
    wwe2k14::safe_pages::PageCache<false> bad;
    uint64_t lk2 = 0, hit2 = 0, asks2 = 0;
    bool m2 = true;
    auto raw2 = [&](uint32_t) { ++asks2; return m2; };
    RangeReadable(0x30000, 4, wwe2k14::safe_pages::Cached(bad, 7, lk2, hit2, raw2));
    m2 = false;
    const bool planted_passes = !RangeReadable(0x30000, 4, wwe2k14::safe_pages::Cached(bad, 8, lk2, hit2, raw2));
    CHECK(!planted_passes);                                 // the never-clearing cache still says readable in frame 8: the real check above would fail on it
  }
  // S108 GAVF review: the census counts are kept per thread and published once a frame (no shared atomic on the per-call path). Every count of frame N reaches the totals at the first check of a later frame, once.
  {
    struct Totals { uint64_t calls = 0, pages = 0, hits = 0, refused = 0, publishes = 0; };
    auto drive = [](auto& t, Totals& tot) {
      auto sink = [&tot](uint64_t c, uint64_t p, uint64_t h, uint64_t r) { tot.calls += c, tot.pages += p, tot.hits += h, tot.refused += r, ++tot.publishes; };
      for (int i = 0; i < 3; ++i) { t.Roll(7, sink); ++t.calls; t.pages += 2; ++t.hits; }   // frame 7: 3 calls
      t.Roll(7, sink);
      const bool held_in_frame = tot.publishes == 0;                                          // nothing published inside the frame
      t.Roll(8, sink); ++t.calls; ++t.pages; ++t.refused;                                      // frame 8 begins: frame 7 published
      t.Roll(8, sink);
      t.Roll(9, sink);                                                                         // frame 9 begins: frame 8 published
      t.Roll(10, sink);                                                                        // an empty frame publishes nothing
      return held_in_frame;
    };
    wwe2k14::safe_pages::FrameTally<> t;
    Totals tot;
    CHECK(drive(t, tot));
    CHECK(tot.calls == 4 && tot.pages == 7 && tot.hits == 3 && tot.refused == 1 && tot.publishes == 2);
    // PLANT: a tally that resets at the frame change without publishing loses every count: the totals check above fails on it.
    wwe2k14::safe_pages::FrameTally<false> bad;
    Totals tot2;
    drive(bad, tot2);
    CHECK(!(tot2.calls == 4 && tot2.pages == 7 && tot2.hits == 3 && tot2.refused == 1 && tot2.publishes == 2));
  }
  // BH3 (S103): the page-size rule, the E0 view crossing, the stall-watch pointer and the alpha check.
  {
    using namespace wwe2k14::safe_pages;
    CHECK(HostPageIs4k(4096));
    CHECK(!HostPageIs4k(16384) && !HostPageIs4k(65536) && !HostPageIs4k(0));   // a simulated 16 KB-page phone is not 4 KB
    CHECK(CrossesE0View(0xDFFFFFF0u, 0x20) && !CrossesE0View(0xDFFFFFF0u, 0x10));
    CHECK(!CrossesE0View(0xDFFFFFFCu, 4) && CrossesE0View(0xDFFFFFFCu, 8));
    CHECK(!CrossesE0View(0xE0000000u, 0x100) && !CrossesE0View(0x10000u, 0x100));
    CHECK(!PlausibleGuestPtr(0) && !PlausibleGuestPtr(0x8000) && !PlausibleGuestPtr(0xFFF0u));   // null / low
    CHECK(!PlausibleGuestPtr(0x7F000000u) && !PlausibleGuestPtr(0x7FFFFFFCu) && !PlausibleGuestPtr(0x7FFFFFF0u));   // MMIO
    CHECK(!PlausibleGuestPtr(0x40001002u));                                        // unaligned
    CHECK(!PlausibleGuestPtr(0xFFFFFFC0u, 104) && !PlausibleGuestPtr(0xFFFFFF00u, 252));   // +off wraps past 4 GB
    CHECK(PlausibleGuestPtr(0x40000000u) && PlausibleGuestPtr(0x40001000u, 104) && PlausibleGuestPtr(0x83940000u, 104) && PlausibleGuestPtr(0x10000u));
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    CHECK(!ModelAlphaPlausible(nan) && !ModelAlphaPlausible(inf) && !ModelAlphaPlausible(-inf) && !ModelAlphaPlausible(1e30f));
    CHECK(ModelAlphaPlausible(-0.0f) && ModelAlphaPlausible(1.7f) && ModelAlphaPlausible(0.5f) && ModelAlphaPlausible(1.0f));   // the range is the consumer's clamp
  }
  // S108 GAVF: the Windows reader's order is page check -> copy under SEH. A wild word (0x41700001, the bits of 15.000001f) must be refused by the CHECK with no host fault; the SEH only guards the check-then-copy race.
  {
    using namespace wwe2k14::safe_pages;
    RefusalLog log;
    uint32_t refused_logs = 0;
    for (int i = 0; i < 100; ++i) if (log.ShouldLog(0x41700001u)) ++refused_logs;
    CHECK(refused_logs == 1);                                   // the same address asked 100 times: one line
    for (uint32_t k = 1; k <= 20; ++k) if (log.ShouldLog(0x41700001u + 0x1000 * k)) ++refused_logs;
    CHECK(refused_logs == 8);                                   // stops after 8 distinct addresses
    CHECK(!log.ShouldLog(0x41700001u + 0x1000 * 30));
    RefusalLog lowlog;
    CHECK(!lowlog.ShouldLog(0) && !lowlog.ShouldLog(0xFFFC) && lowlog.n == 0 && lowlog.ShouldLog(0x10000));   // a null-ish field is not wild
#ifdef _WIN32
    // A host page we control stands for the guest page 0x20000. `table_says_readable_20000` = what the page table says. fault_filter_hits counts the host faults that really happened.
    uint8_t* host = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    uint8_t* dead = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS));   // every other guest address maps here: touching it faults
    CHECK(host != nullptr && dead != nullptr);
    host[0] = 0x5A;
    static int fault_filter_hits = 0;
    auto seh_copy = [](void* dst, const void* src, size_t n) -> bool {   // the shape of wrestlers::SafeCopy
      static void* (*volatile copy)(void*, const void*, size_t) = std::memcpy;
      __try { copy(dst, src, n); }
      __except ((++fault_filter_hits, GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)) { return false; }
      return true;
    };
    auto host_of = [&](uint32_t g) { return (g & ~0xFFFu) == 0x20000 ? host + (g & 0xFFF) : dead + (g & 0xFFF); };
    bool table_says_readable_20000 = true;
    auto table = [&](uint32_t a) { return (a & ~0xFFFu) == 0x20000 && table_says_readable_20000; };
    auto read_new = [&](uint32_t g, uint8_t* out) {   // NEW Windows order: check, then SEH copy
      PageCache<> c; uint64_t lk = 0, h = 0;
      return RangeReadable(g, 1, Cached(c, 1, lk, h, table)) && seh_copy(out, host_of(g), 1);
    };
    auto read_old = [&](uint32_t g, uint8_t* out) { return seh_copy(out, host_of(g), 1); };   // OLD Windows branch: GuestRangeReadable returned true unchecked
    uint8_t b = 0;
    fault_filter_hits = 0;
    CHECK(!read_new(0x41700001u, &b));
    CHECK(fault_filter_hits == 0);                              // refused by the check: no host fault taken
    CHECK(read_new(0x20000, &b) && b == 0x5A && fault_filter_hits == 0);   // a readable address still reads
    CHECK(!read_old(0x41700001u, &b) && fault_filter_hits == 1);   // the old branch: the host fault IS the check (what the SDK logs as an error)
    DWORD oldp = 0;   // the race: the table says readable, the page goes unreadable before the copy: the __except catches it
    CHECK(VirtualProtect(host, 0x1000, PAGE_NOACCESS, &oldp) != 0);
    fault_filter_hits = 0;
    CHECK(!read_new(0x20000, &b) && fault_filter_hits == 1);
    VirtualProtect(host, 0x1000, PAGE_READWRITE, &oldp);
    table_says_readable_20000 = false;
    fault_filter_hits = 0;
    CHECK(!read_new(0x20000, &b) && fault_filter_hits == 0);    // the table now says no: refused by the check
    VirtualFree(host, 0, MEM_RELEASE); VirtualFree(dead, 0, MEM_RELEASE);
#endif
  }
  {   // S108 RDS: HullOk. A hull of at most two pages touches exactly the pages its first and last field touch; a wider copy (the twin) newly fails at a mapped page end.
    using wwe2k14::safe_pages::HullOk;
    CHECK(HullOk(0x10F00, 0x11000));                         // inside one page, ending exactly at the page end
    CHECK(HullOk(0x10FFC, 0x11008));                         // two neighbouring pages
    CHECK(HullOk(0x10004, 0x11FFC + 4));                     // first byte of page 0x10, last byte of page 0x11: two pages
    CHECK(!HullOk(0x10FFC, 0x12004));                        // three pages: the middle page no field reads: refused
    CHECK(!HullOk(0x10000, 0x10000));                        // empty
    CHECK(!HullOk(0xFFFFFFF0u, 0x100000004ull));             // past 4 GB
    // a struct whose last field ends at the end of the last readable page (0x13FFF): the field reads and the exact hull are readable, the hull one byte wider is not
    auto fields_ok = RangeReadable(0x13F00, 4, ok) && RangeReadable(0x13FFC, 4, ok);
    const uint32_t first = 0x13F00; const uint64_t end = 0x14000;
    CHECK(fields_ok && HullOk(first, end) && RangeReadable(first, end - first, ok));
    CHECK(HullOk<true>(first, end + 1) && !RangeReadable(first, end + 1 - first, ok));   // the planted twin (one byte past the hull): refused where every field read passed
    // every pair of field ranges inside a <= 2-page hull: the hull's pages == the union of the two fields' pages (exhaustive over offsets at 4-byte steps across two pages)
    bool same = true;
    for (uint32_t a = 0x10000; a < 0x12000 && same; a += 4)
      for (uint32_t b2 = a; b2 < 0x12000 && same; b2 += 4) {
        if (!HullOk(a, uint64_t(b2) + 4)) continue;
        for (uint32_t bad = 0x10; bad <= 0x11; ++bad) {   // one page unreadable at a time
          auto rd = [bad](uint32_t x) { return (x >> 12) != bad; };
          const bool hull = RangeReadable(a, b2 + 4 - a, rd), each = RangeReadable(a, 4, rd) && RangeReadable(b2, 4, rd);
          if (hull != each) same = false;
        }
      }
    CHECK(same);
  }
  {   // S108 RDS phase 2: LineCopy. Fake guest 0x20000..0x23000, page 0x22 unreadable. For every field (1..16 bytes, every offset across the readable page end 0x22000 and a line edge) in a fresh pass:
      // the verdict and bytes equal the field read's, and every page the line copy ASKS is a page the field read asks (a line never reaches a page no field read touches). The twin (the window starts AT
      // the field) asks page 0x22 for a field in the last 128 bytes of page 0x21 and must be caught. Cost: 32 words of one line = one read. A refused line (the race) is the field read.
    using wwe2k14::safe_pages::LineCopy;
    static uint8_t img[0x3000];
    for (uint32_t i = 0; i < sizeof img; ++i) img[i] = uint8_t(i * 7 + 3);
    static uint64_t asked_mask = 0;   // bit (page - 0x20) for each page asked (the fake records a range's pages before refusing)
    static uint32_t reads = 0;
    static bool refuse_lines = false;
    auto rd = [](uint32_t a, uint32_t n, void* d) {
      ++reads;
      bool ok = a >= 0x20000 && uint64_t(a) + n <= 0x23000 && !(refuse_lines && n == 128);
      for (uint64_t p = a >> 12; p <= (uint64_t(a) + n - 1) >> 12; ++p) {
        if (p >= 0x20 && p < 0x60) asked_mask |= 1ull << (p - 0x20);
        if (p == 0x22) ok = false;
      }
      if (ok) std::memcpy(d, img + (a - 0x20000), n);
      return ok;
    };
    auto field_pages = [](uint32_t a, uint32_t n) { uint64_t m = 0; for (uint64_t p = a >> 12; p <= (uint64_t(a) + n - 1) >> 12; ++p) if (p >= 0x20 && p < 0x60) m |= 1ull << (p - 0x20); return m; };
    bool same = true, subset = true, twin_caught = false;
    const uint32_t sizes[] = {1, 2, 4, 8, 16};
    for (uint32_t a = 0x21E80; a < 0x22010 && same && subset; ++a)
      for (uint32_t n : sizes) {
        uint8_t want[16] = {}, got[16] = {};
        asked_mask = 0;
        const bool fok = rd(a, n, want);
        LineCopy<> lc;
        asked_mask = 0;
        const bool lok = lc.Get(a, n, got, rd);
        if (lok != fok || (lok && std::memcmp(want, got, n) != 0)) same = false;
        if (asked_mask & ~field_pages(a, n)) subset = false;
        LineCopy<1, true> tw;
        asked_mask = 0;
        tw.Get(a, n, got, rd);
        if (asked_mask & ~field_pages(a, n)) twin_caught = true;
      }
    CHECK(same);          // every verdict and value is the field read's
    CHECK(subset);        // no page asked that the field read does not ask
    CHECK(twin_caught);   // the unaligned twin asks the page past the mapped end
    {
      LineCopy<> lc;
      reads = 0;
      uint8_t w[4];
      bool all = true;
      for (uint32_t i = 0; i < 32; ++i) all = all && lc.Get(0x20080 + 4 * i, 4, w, rd) && w[0] == img[0x80 + 4 * i];
      CHECK(all && reads == 1 && lc.lines_read == 1 && lc.served == 32);   // one line read serves the 32 words of its line
      refuse_lines = true;
      LineCopy<> lr;
      reads = 0;
      CHECK(lr.Get(0x20104, 4, w, rd) && w[0] == img[0x104] && reads == 2 && lr.direct == 1);   // the line refused: the field read decides
      refuse_lines = false;
    }
  }
  {   // S108 RDS phase 2: RowsHullOk / ReadRows. Exhaustive over row layouts near page edges: when RowsHullOk, the hull's pages == the union of the rows' pages. The twin (a gap of exactly one page let
      // through) must find a layout where the hull asks a page no row touches.
    using wwe2k14::safe_pages::RowsHullOk;
    auto pages_of = [](uint64_t a, uint64_t n, std::vector<uint32_t>& out) { for (uint64_t p = a >> 12; p <= (a + n - 1) >> 12; ++p) out.push_back(uint32_t(p)); };
    bool equal = true, twin = false;
    const uint32_t widths[] = {0x10, 0x100, 0x280, 0x1000};
    for (uint32_t w : widths)
      for (uint32_t gap = 0; gap <= 0x1100; gap += 0x40)
        for (uint32_t start = 0x10000; start < 0x11000; start += 0x80)
          for (uint32_t rows = 1; rows <= 3; ++rows) {
            const uint32_t pitch = w + gap;
            std::vector<uint32_t> hull, each;
            pages_of(start, uint64_t(rows - 1) * pitch + w, hull);
            for (uint32_t r = 0; r < rows; ++r) pages_of(uint64_t(start) + uint64_t(r) * pitch, w, each);
            std::sort(each.begin(), each.end());
            each.erase(std::unique(each.begin(), each.end()), each.end());
            if (RowsHullOk(pitch, w, rows) && hull != each) equal = false;
            if (RowsHullOk<true>(pitch, w, rows) && hull != each) twin = true;
          }
    CHECK(equal);
    CHECK(twin);
    // ReadRows over a fake with one unreadable page: the verdict equals the per-row reads', one hull check when RowsHullOk, the per-row path otherwise
    static uint32_t bad_page = 0;
    static uint32_t hull_checks = 0, row_reads = 0;
    auto readable = [](uint32_t a, uint64_t n) { for (uint64_t p = a >> 12; p <= (uint64_t(a) + n - 1) >> 12; ++p) if (p == bad_page) return false; return true; };
    bool verdicts = true, one_check = true;
    uint8_t buf[0x4000];
    for (bad_page = 0x10; bad_page <= 0x14; ++bad_page)
      for (uint32_t gap : {0u, 0x80u, 0xFC0u, 0x1000u, 0x1040u}) {
        const uint32_t w = 0x100, pitch = w + gap, rows = 4, start = 0x10F00;
        bool each = true;
        for (uint32_t r = 0; r < rows; ++r) each = each && readable(start + r * pitch, w);
        hull_checks = row_reads = 0;
        const bool got = wwe2k14::safe_pages::ReadRows(
            start, pitch, rows, w, buf, [](uint32_t, uint64_t) { return true; }, [&](uint32_t a, uint64_t n) { ++hull_checks; return readable(a, n); },
            [](uint32_t, uint8_t*, uint32_t) { return true; }, [&](uint32_t a, uint8_t*, uint32_t n) { ++row_reads; return readable(a, n); });
        if (got != each) verdicts = false;
        if (RowsHullOk(pitch, w, rows) && (hull_checks != 1 || row_reads != 0)) one_check = false;
        if (!RowsHullOk(pitch, w, rows) && hull_checks != 0) one_check = false;
      }
    CHECK(verdicts);
    CHECK(one_check);
  }
  std::printf(fails ? "FAILED\n" : "OK\n");
  return fails;
}
