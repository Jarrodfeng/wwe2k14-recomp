// Session 73 (ARM CPU maths): rex::ppc::ByteReverse128 (rev64 + ext on ARM, a constant pshufb on x86) against the
// generated code's old lvx / stvx reversal, simde_mm_shuffle_epi8 with VectorMaskL's first row, on 100,000 random
// vectors. Built for x86 (clang) and ARM64 (NDK, QEMU) by run_byterev_test.ps1.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <random>

#include <rex/ppc/intrinsics.h>

int main() {
  std::mt19937_64 rng(73);
  int bad = 0;
  for (int i = 0; i < 100000; ++i) {
    alignas(16) uint64_t in[2] = {rng(), rng()};
    const simde__m128i v = simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(in));
    alignas(16) uint8_t a[16], b[16];
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(a),
                         simde_mm_shuffle_epi8(v, simde_mm_load_si128((simde__m128i*)rex::ppc::VectorMaskL)));
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(b), rex::ppc::ByteReverse128(v));
    if (std::memcmp(a, b, 16) != 0) ++bad;
  }
  std::printf("ByteReverse128 vs shuffle_epi8(VectorMaskL): %d of 100000 differ\n", bad);
  return bad ? 1 : 0;
}
