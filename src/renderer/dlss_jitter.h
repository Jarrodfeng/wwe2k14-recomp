// Settings parity S10b: the sub-pixel jitter DLSS needs, one sequence for both sides: the reader moves the scene's draws by it (reader.h
// ApplyDlssJitter) and the binder tells DLSS the same values (binder.h). Halton (2, 3), centred, in render pixels, by guest frame.
// Phases (NVIDIA's guidance, 8 x the area ratio): 8 for DLAA, 32 for 2x per axis (performance), 72 for 3x (ultra performance).
#pragma once
#include <cstdint>

namespace rdr {

inline float Halton(uint32_t index, uint32_t base) {
  float f = 1.0f, r = 0.0f;
  while (index > 0) {
    f /= float(base);
    r += f * float(index % base);
    index /= base;
  }
  return r;
}

inline void DlssJitter(uint64_t guest_frame, uint32_t phases, float out[2]) {
  const uint32_t i = uint32_t(guest_frame % (phases ? phases : 8)) + 1;  // Halton from 1 (0 is the corner)
  out[0] = Halton(i, 2) - 0.5f;
  out[1] = Halton(i, 3) - 0.5f;
}

}  // namespace rdr
