// Session 73: one lvx as the generated code did it (old) and does it now (new), for the ARM64 assembly.
#include <rex/ppc/intrinsics.h>

extern "C" void lvx_old(uint8_t* dst, const uint8_t* src) {
  simde_mm_store_si128((simde__m128i*)dst, simde_mm_shuffle_epi8(simde_mm_load_si128((const simde__m128i*)src),
                                                                 simde_mm_load_si128((simde__m128i*)rex::ppc::VectorMaskL)));
}
extern "C" void lvx_new(uint8_t* dst, const uint8_t* src) {
  simde_mm_store_si128((simde__m128i*)dst, rex::ppc::ByteReverse128(simde_mm_load_si128((const simde__m128i*)src)));
}
