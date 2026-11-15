// S102 lane G2c: the pick rule of wwe2k14_display_720p_mode=auto:N (display_mode.h), kept free of Windows so tools\g2c\pick_test.cpp runs it with planted mode lists
// and planted twins. docs\renderer\OUTPUT_PASS.md "Lane G2c".
//
// From the monitor's mode list: the modes with the PANEL's aspect exactly (square pixels: w*PH == h*PW), at the CURRENT refresh exactly, whose height is at or above the
// asked height and BELOW the panel's own; of those the smallest height. None = no change (the caller logs the list and stays at the panel mode).
#pragma once
#include <cstddef>
#include <cstdint>

namespace wwe2k14::display_pick {

struct Mode {
  uint32_t w = 0, h = 0, hz = 0;
};

inline bool Pick(const Mode* list, size_t count, uint32_t panel_w, uint32_t panel_h, uint32_t current_hz, uint32_t asked_h, Mode& out) {
  if (!panel_w || !panel_h || !asked_h) return false;
  bool found = false;
  for (size_t i = 0; i < count; ++i) {
    const Mode& m = list[i];
    if (!m.w || !m.h || m.hz != current_hz) continue;
    if (uint64_t(m.w) * panel_h != uint64_t(m.h) * panel_w) continue;  // square pixels: the panel's aspect exactly
    if (m.h < asked_h) continue;
    if (m.h >= panel_h) continue;  // the panel's own mode (or bigger): nothing to gain
    if (!found || m.h < out.h) {
      out = m;
      found = true;
    }
  }
  return found;
}

// "auto:912" -> 912; anything else (including "1280x720") -> 0.
inline uint32_t ParseAuto(const char* text) {
  if (!text || text[0] != 'a' || text[1] != 'u' || text[2] != 't' || text[3] != 'o' || text[4] != ':') return 0;
  uint64_t v = 0;
  size_t i = 5;
  if (text[i] < '0' || text[i] > '9') return 0;
  for (; text[i] >= '0' && text[i] <= '9'; ++i) {
    v = v * 10 + uint64_t(text[i] - '0');
    if (v > 100000) return 0;
  }
  return text[i] == 0 ? uint32_t(v) : 0;
}

}  // namespace wwe2k14::display_pick
