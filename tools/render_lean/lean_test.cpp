// S102 RT1: the pure pieces of render_lean.h, with planted twins. Build + run: py -I tools\render_lean\lean_unit.py. Exit 0 = every case behaved; a `plant` argument runs the faulty twin, which must exit 1.
//   lean_test            the real rules: all cases pass
//   lean_test plant1     the texture-set cache does not evict a destroyed image's sets: a recreated image at the same handle reads the OLD set (caught)
//   lean_test plant3     the staging pool hands a buffer back while its copy is in flight (caught by the GPU model)
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "../../src/renderer/render_lean.h"

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}
constexpr size_t N = 4;
using Key = rdr::lean::SetKey<N>;
Key MakeKey(uint64_t v0, uint64_t v1, uint64_t s) {
  Key k;
  std::memset(&k, 0, sizeof(k));
  k.view[0] = v0, k.view[1] = v1, k.sampler[0] = s, k.sampler[1] = s;
  return k;
}
}  // namespace

int main(int argc, char** argv) {
  const std::string plant = argc > 1 ? argv[1] : "";
  // ---- Q5: the set cache ----
  {
    rdr::lean::SetCache<N, uint64_t> cache(8);
    bool collide = false;
    const Key a = MakeKey(0x100, 0x200, 7), b = MakeKey(0x100, 0x300, 7);
    Check(cache.Find(a, a.Hash(), 1, collide) == nullptr, "an empty cache holds nothing");
    Check(cache.Insert(a, a.Hash(), 1111, 1) && cache.Insert(b, b.Hash(), 2222, 1), "two sets inserted");
    auto* hit = cache.Find(a, a.Hash(), 2, collide);
    Check(hit && hit->set == 1111 && hit->last_use == 2, "the same key (view and sampler handles) is a hit and refreshes last_use");
    Check(cache.Find(MakeKey(0x100, 0x200, 8), MakeKey(0x100, 0x200, 8).Hash(), 2, collide) == nullptr, "another sampler is a miss");
    // a recreated image: the view handle 0x200 is destroyed and a new image gets the SAME handle (drivers reuse them). The cache must have dropped every set naming it.
    std::vector<uint64_t> freed;
    std::vector<uint64_t> dying{0x200};
    const size_t n = cache.EvictViews(dying, freed, plant == "plant1");
    const bool stale = cache.Find(a, a.Hash(), 3, collide) != nullptr;   // the recreated image's key is equal to the old one
    Check(n == 1 && freed.size() == 1 && freed[0] == 1111 && !stale, "a destroyed image's views drop exactly the sets that name them, so a recreated image at the same handle builds a fresh set");
    Check(cache.Find(b, b.Hash(), 3, collide) != nullptr, "a set that names other views stays");
    // capacity + sweep
    rdr::lean::SetCache<N, uint64_t> small(2);
    const Key k1 = MakeKey(1, 1, 1), k2 = MakeKey(2, 2, 2), k3 = MakeKey(3, 3, 3);
    small.Insert(k1, k1.Hash(), 1, 10), small.Insert(k2, k2.Hash(), 2, 100);
    Check(!small.Insert(k3, k3.Hash(), 3, 101), "a full cache refuses an insert (the renderer falls back to the frame's own pool)");
    std::vector<uint64_t> gone;
    Check(small.Sweep(700, 600, gone) == 1 && gone[0] == 1, "the sweep takes only sets idle for more than the age");
    // a colliding hash with a different key is not a hit
    rdr::lean::SetCache<N, uint64_t> col(4);
    col.Insert(k1, 42, 9, 1);
    bool c = false;
    Check(col.Find(k2, 42, 2, c) == nullptr && c, "the same hash with another key is a collision, never a hit");
  }
  // ---- Q9: the staging pool against a GPU model ----
  {
    // The GPU model: a copy recorded in frame f reads its buffer until frame f finishes (fence serial >= f). Buffers return to the pool when the render thread drains their slot (frame f finished).
    rdr::lean::StagingPool<int> pool(1ull << 30);
    const uint32_t cls = rdr::lean::StagingClass(5000);
    Check(cls == 2 && rdr::lean::StagingClassBytes(cls) == 5120 && rdr::lean::StagingClassBytes(rdr::lean::StagingClass(9000)) == 10240 && rdr::lean::StagingClassBytes(rdr::lean::StagingClass(4097)) <= 4097 + 4097 / 4, "a 5000 byte upload is the 5 KiB class; a pooled buffer is never over 25 % larger than its upload");
    Check(rdr::lean::StagingClass(16ull << 20) != 0 && rdr::lean::StagingClass((16ull << 20) + 1) == 0, "uploads over 16 MiB are not pooled");
    std::set<int> reading;       // buffers whose copy has not finished
    int next_buf = 100;
    int finished = 0;            // frames <= finished are done on the GPU
    struct InFlight { int frame, buf; };
    std::vector<InFlight> flight;
    bool reused_in_flight = false;
    for (int frame = 1; frame <= 40; ++frame) {
      int buf = 0;
      if (!pool.Take(cls, buf)) buf = next_buf++;
      if (reading.count(buf)) reused_in_flight = true;   // the host would overwrite memory the GPU still reads
      reading.insert(buf);
      flight.push_back({frame, buf});
      if (frame >= 3) finished = frame - 2;   // the GPU is two frames behind
      for (auto it = flight.begin(); it != flight.end();) {
        if (it->frame <= finished) {
          reading.erase(it->buf);
          if (plant != "plant3") pool.Give(cls, it->buf);   // the drain: after the fence
          it = flight.erase(it);
        } else {
          if (plant == "plant3") pool.Give(cls, it->buf);   // PLANT 3: handed back at retire time, copy not yet run
          ++it;
        }
      }
    }
    Check(!reused_in_flight && next_buf - 100 <= 3, "no buffer is taken while its copy is in flight, and 40 uploads needed only the in-flight number of buffers");
    rdr::lean::StagingPool<int> tiny(8192);
    Check(tiny.Give(cls, 1) && !tiny.Give(cls, 2), "the pool holds at most its byte budget (the second buffer is destroyed by the caller)");
  }
  if (plant.empty()) std::printf(g_fail ? "FAILED\n" : "all cases pass\n");
  else std::printf(g_fail ? "the plant was CAUGHT (exit 1)\n" : "the plant was NOT caught\n");
  return g_fail ? 1 : 0;
}
