// Offline unit for src\scene_live\safe_guest_pages.h: readable / unreadable page -> true / false, no memory touched (so no crash); the physical walk's mapping against src\guest_view.h.
// Build + run: pwsh -File tools\scene\build_safe_pages_test.ps1
#include <cstdio>
#include <limits>
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
  std::printf(fails ? "FAILED\n" : "OK\n");
  return fails;
}
