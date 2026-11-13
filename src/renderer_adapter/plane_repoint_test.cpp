// lane 195: clang++ -std=c++17 -I src/renderer_adapter src/renderer_adapter/plane_repoint_test.cpp   (add -DWWE2K14_PLANT_STICKY: must FAIL)
// A model of one image's bind: the flush-driven early return (ServeCached), else the page-watch rule (served while its pages are clean).
#include <cstdio>
#include <map>
#include <vector>
#include "plane_repoint.h"
#include "content_fit.h"
using namespace rdr_adapter::plane;
static int fails = 0;
static void Check(const char* n, bool ok) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", n); if (!ok) ++fails; }

struct Img {
  State s;
  uint32_t content_base = 0;  // which base the cached bytes came from (0 = none)
};
// One bind with header w; `pages_dirty` = a CPU write hit the watched pages. Returns true if the image was read (content refreshed from w's base).
static bool Bind(Img& m, const uint32_t w[6], bool pages_dirty, bool legacy = false) {
  Rebind(m.s, w, legacy);
  const bool serve = m.s.flush_driven ? ServeCached(m.s) : (m.s.content_written && !m.s.flushed && !pages_dirty);
  if (serve) return false;
  m.s.content_written = true;
  m.s.flushed = false;
  m.content_base = w[1] & 0xFFFFF000u;
  return true;
}
static void Flush(std::map<uint32_t, uint32_t>& by_base, std::vector<Img>& imgs, uint32_t phys, uint32_t size) {
  NameByFlush(by_base, phys, size, [&](uint32_t id) { imgs[id].s.flushed = true; imgs[id].s.flush_driven = true; return true; });
}

int main() {
  const uint32_t A[6] = {0, 0x1A000000, 0x00FF00FF, 0, 0, 0}, A_fmt[6] = {7, 0x1A000123, 0x00FF00FF, 0, 0, 0};
  const uint32_t B[6] = {0, 0x1B400000, 0x00FF00FF, 0, 0, 0}, A_size[6] = {0, 0x1A000000, 0x01FF01FF, 0, 0, 0};
  std::vector<Img> imgs(2);
  std::map<uint32_t, uint32_t> by_base = {{0x1A000000, 0}, {0x1B400000, 0}};  // the one image is registered at both bases (re-pointed)
  Img& m = imgs[0];

  Check("first bind: read", Bind(m, A, false));
  Flush(by_base, imgs, 0x1A000000, 0x10000);
  Check("flush names it: flush-driven, flushed", m.s.flush_driven && m.s.flushed);
  Check("bound after the flush: read", Bind(m, A, false));
  Check("bound again, no flush: served", !Bind(m, A, false));
  Check("another format / endian bits and low address bits, same base page and size: same plane, served", !Bind(m, A_fmt, false));

  // (a) a move, then a flush naming the new base -> read again
  Check("(a) re-pointed to other memory: read at once (old bytes not served)", Bind(m, B, false) && m.content_base == 0x1B400000);
  Check("(a) the move left the flush-driven rule", !m.s.flush_driven);
  Flush(by_base, imgs, 0x1B400000, 0x10000);
  Check("(a) the flush naming the new base: read again", Bind(m, B, false));
  Check("(a) then served until the next flush", !Bind(m, B, false));

  // (b) a bind at the new base before its first flush: one read, then correct after the flush
  Img b;
  imgs[0] = b;
  Bind(imgs[0], A, false);
  Flush(by_base, imgs, 0x1A000000, 0x10000);
  Bind(imgs[0], A, false);
  const bool r1 = Bind(imgs[0], B, false), r2 = Bind(imgs[0], B, false);
  Check("(b) bind at B before its first flush: exactly one read, then watch-served", r1 && !r2);
  Flush(by_base, imgs, 0x1B400000, 0x10000);
  Check("(b) after the flush: read, content is B's", Bind(imgs[0], B, false) && imgs[0].content_base == 0x1B400000);

  // (c) bases alternating A/B every frame: a read each move (the cost: one read per header rewrite, about 3 per load in the review's count)
  int reads = 0, moves = 0;
  for (int f = 0; f < 10; ++f) {
    const uint32_t* w = (f & 1) ? B : A;
    if (f) ++moves;
    if (Bind(imgs[0], w, false)) ++reads;
    Check("(c) alternating: the bytes are the bound base's", imgs[0].content_base == (w[1] & 0xFFFFF000u));
  }
  Check("(c) alternating A/B: one read per move (reads == moves + 1 at the first frame)", reads == moves + 1);

  // (d) a move to a base no flush names -> NOT frozen: the page watch covers it
  Img d;
  imgs[0] = d;
  Bind(imgs[0], A, false);
  Flush(by_base, imgs, 0x1A000000, 0x10000);
  Bind(imgs[0], A, false);
  Bind(imgs[0], B, false);  // B is never flushed in this case
  Check("(d) unflushed base, pages clean: served", !Bind(imgs[0], B, false));
  Check("(d) unflushed base, a CPU write hits its pages: read again (not frozen)", Bind(imgs[0], B, true));
  Check("(d) read leaves the content current", imgs[0].content_base == 0x1B400000);

  // size-only move
  Img sz;
  imgs[0] = sz;
  Bind(imgs[0], A, false);
  Flush(by_base, imgs, 0x1A000000, 0x10000);
  Bind(imgs[0], A, false);
  Check("size-only move: read (not served stale)", Bind(imgs[0], A_size, false));

  // stale census: lane 172's condition (cached, unflushed, then moved)
  Img c;
  Bind(c, A, false);
  c.s.flush_driven = true;
  c.s.content_written = true;
  c.s.flushed = false;
  const Result rr = Rebind(c.s, B);
  Check("census: moved and stale reported for a cached, unflushed plane", rr.moved && rr.stale);
  Img c2;
  Bind(c2, A, false);
  c2.s.flushed = true;
  const Result rr2 = Rebind(c2.s, B);
  Check("census: moved but not stale when a flush is pending", rr2.moved && !rr2.stale);
  Img c3;
  const Result rr3 = Rebind(c3.s, A);
  Check("census: first bind is neither", !rr3.moved && !rr3.stale);

  // legacy cvar: the old sticky behaviour comes back
  Img l;
  Bind(l, A, false);
  Flush(by_base, imgs, 0x1A000000, 0x10000);
  imgs[0] = l;
  imgs[0].s.flush_driven = true;
  Bind(imgs[0], A, false, true);
  Check("legacy: re-pointed plane served stale (old behaviour)", !Bind(imgs[0], B, false, true));

  // SubresourceFits (binder ApplyContents guard): an older-shape content against the refreshed record
  using rdr_adapter::SubresourceFits;
  Check("fits: mip 0 of a 512x256, 1 mip", SubresourceFits(512, 256, 1, 1, 0, 0, 512, 256));
  Check("fits: mip 2 of a 512x256 with 4 mips is 128x64", SubresourceFits(512, 256, 4, 1, 2, 0, 128, 64));
  Check("fits: the 1x1 tail clamps", SubresourceFits(4, 2, 4, 1, 3, 0, 1, 1));
  Check("old 512x256 content, record now 256x128: skipped", !SubresourceFits(256, 128, 1, 1, 0, 0, 512, 256));
  Check("old content with 4 mips, record now 1 mip: mip 1 skipped", !SubresourceFits(512, 256, 1, 1, 1, 0, 256, 128));
  Check("layer beyond the record: skipped", !SubresourceFits(64, 64, 1, 1, 0, 1, 64, 64));
  Check("height differs only: skipped", !SubresourceFits(512, 256, 1, 1, 0, 0, 512, 128));
  std::printf("%s\n", fails ? "FAILED" : "all passed");
  return fails;
}
