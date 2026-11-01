// Session 73: support for the generated differential fuzz of the PPC instruction tests (make_fuzz.py): a seeded
// random source biased towards edge values, bit-exact printing, the guest memory (4 GB + the physical offset, all
// read / write so random addresses land somewhere defined) and main().
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "ppc_config.h"
#include <rex/ppc.h>

namespace fz {

struct Rng {
  uint64_t s;
  uint64_t Next() {  // splitmix64
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
};

// Session 77 (XDET_PLAN.md P5, gaps 13 / 16): band 1 rounds draw only from these boundary lists (make_fuzz.py: rounds
// are in three bands: 0 random as in Session 73, 1 boundary values, 2 random with a rounding mode per round).
// f64: the integer conversion edges (fctid / fctidz at exactly 2^63, fctiw at 2^31, halves for the rounding modes,
// 2^52 for fcfid) and the float-precision edges (frsp just under FLT_MIN).
static const uint64_t kF64Boundary[] = {
    0x43E0000000000000ull, 0xC3E0000000000000ull, 0x43DFFFFFFFFFFFFFull, 0xC3E0000000000001ull, 0x43F0000000000000ull,
    0x41E0000000000000ull, 0x41DFFFFFFFC00000ull, 0x41DFFFFFFFE00000ull, 0xC1E0000000000000ull, 0xC1E0000000100000ull,
    0x41F0000000000000ull, 0x3FE0000000000000ull, 0x3FF8000000000000ull, 0x4004000000000000ull, 0xBFE0000000000000ull,
    0xBFF8000000000000ull, 0x4330000000000000ull, 0x4330000000000001ull, 0x3810000000000000ull, 0x380FFFFFF0000000ull,
    0x380FFFFFE0000000ull, 0x0010000000000000ull, 0x000FFFFFFFFFFFFFull, 0x7FF0000000000000ull};
// f32 lanes: operands whose products / sums land just under FLT_MIN and round up to it (x86 judges tininess after
// rounding, ARM before: with flush-to-zero one flushes, the other not), FLT_MIN neighbours, the int32 conversion edges.
static const uint32_t kV32Boundary[] = {
    0x00800000u, 0x00800001u, 0x007FFFFFu, 0x00000001u, 0x80800000u, 0x3F7FFFFEu, 0x3F7FFFFFu, 0x3F800000u,
    0x3F800001u, 0x3F000000u, 0x3F000001u, 0xBF7FFFFEu, 0x01000000u, 0x00FFFFFFu, 0x3EFFFFFFu, 0x4F000000u,
    0x4EFFFFFFu, 0xCF000000u, 0xCF000001u, 0x4F800000u, 0x7F7FFFFFu, 0x7F800000u};
template <class T, size_t N>
inline T Pick(Rng& r, const T (&list)[N]) { return list[r.Next() % N]; }

inline uint64_t Rand64(Rng& r, bool fp_bits, int band = 0) {
  static const uint64_t kEdge[] = {0, 1, 0xFFFFFFFFFFFFFFFFull, 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull,
                                   0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x100000000ull, 0xFFFFFFFF80000000ull, 31, 32,
                                   63, 64, 0x1F, 0x3F, 0x40, 0x10001000};
  if (band == 1) return fp_bits ? Pick(r, kF64Boundary) : Pick(r, kEdge);
  const uint64_t x = r.Next();
  switch (x & 7) {
    case 0: return kEdge[(x >> 8) % (sizeof(kEdge) / sizeof(kEdge[0]))];
    case 1: return (x >> 8) & 0xFF;
    case 2: return uint64_t(int64_t(int32_t(x >> 16)));
    default: return r.Next();
  }
}

inline double RandF64(Rng& r, int band = 0) {
  if (band == 1) {
    const uint64_t b = Pick(r, kF64Boundary);
    double d;
    std::memcpy(&d, &b, 8);
    return d;
  }
  static const uint64_t kEdge[] = {0x0000000000000000ull, 0x8000000000000000ull, 0x7FF0000000000000ull,
                                   0xFFF0000000000000ull, 0x7FF8000000000000ull, 0xFFF8000000000000ull,
                                   0x7FF0000000000001ull, 0x0000000000000001ull, 0x800FFFFFFFFFFFFFull,
                                   0x3FE0000000000000ull, 0xBFE0000000000000ull, 0x4004000000000000ull,
                                   0x41DFFFFFFFC00000ull, 0x41E0000000000000ull, 0xC1E0000000200000ull,
                                   0x47EFFFFFE0000000ull, 0x3810000000000000ull, 0x36A0000000000000ull};
  const uint64_t x = r.Next();
  uint64_t bits;
  switch (x & 7) {
    case 0: case 1: bits = kEdge[(x >> 8) % (sizeof(kEdge) / sizeof(kEdge[0]))]; break;
    case 2: { double d = double(int32_t(x >> 16)) / double(1 + ((x >> 8) & 0xFF)); std::memcpy(&bits, &d, 8); break; }
    case 3: { double d = double(int32_t(x >> 16)) + 0.5; std::memcpy(&bits, &d, 8); break; }
    default: bits = r.Next(); break;
  }
  double d;
  std::memcpy(&d, &bits, 8);
  return d;
}

inline uint32_t RandV32(Rng& r, int band = 0) {
  if (band == 1) return Pick(r, kV32Boundary);
  static const uint32_t kEdge[] = {0, 0x80000000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00000u, 0x7F800001u,
                                   0x00000001u, 0x807FFFFFu, 0x3F000000u, 0xBF000000u, 0x40200000u, 0x4EFFFFFFu,
                                   0x4F000000u, 0xCF000001u, 0x4F800000u, 0x7F7FFFFFu, 0x00800000u, 0xFFFFFFFFu,
                                   0x7FFFFFFFu, 0x00008000u, 0xFFFF8000u};
  const uint64_t x = r.Next();
  switch (x & 7) {
    case 0: case 1: case 2: return kEdge[(x >> 8) % (sizeof(kEdge) / sizeof(kEdge[0]))];
    case 3: { float f = float(int32_t(x >> 16)) / float(1 + ((x >> 8) & 0xFF)); uint32_t b; std::memcpy(&b, &f, 4); return b; }
    default: return uint32_t(r.Next());
  }
}

inline void Print(FILE* f, uint64_t v) { std::fprintf(f, " %016llx", (unsigned long long)v); }
inline void Print(FILE* f, uint32_t v) { std::fprintf(f, " %08x", v); }
inline void Print(FILE* f, double v) { uint64_t b; std::memcpy(&b, &v, 8); Print(f, b); }
inline void Print(FILE* f, float v) { uint32_t b; std::memcpy(&b, &v, 4); Print(f, b); }
template <class T>
inline void Print(FILE* f, const T& v) {
  unsigned char b[sizeof(T)];
  std::memcpy(b, &v, sizeof(T));
  std::fprintf(f, " ");
  for (size_t i = 0; i < sizeof(T); ++i) std::fprintf(f, "%02x", b[i]);
}

struct Case {
  const char* name;
  void (*run)(uint8_t* memory, Rng& rng, FILE* f);
};
extern const Case kCases[];
extern const int kCaseCount;

inline uint8_t* AllocGuest() {
  const size_t size = (size_t(1) << 32) + 0x10000;
#if defined(_WIN32)
  return static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
  void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  return p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
#endif
}

}  // namespace fz

#ifdef FZ_MAIN
int main() {
  uint8_t* memory = fz::AllocGuest();
  if (!memory) { std::fprintf(stderr, "guest memory allocation failed\n"); return 1; }
  for (int i = 0; i < fz::kCaseCount; ++i) {
    fz::Rng rng{0x5EED0000ull + uint64_t(i)};
    fz::kCases[i].run(memory, rng, stdout);
  }
  return 0;
}
#endif
