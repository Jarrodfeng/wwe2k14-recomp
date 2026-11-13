// wwe2k14 - the `!caw` live verb (S101 lane 118, N2 / L-6 of docs/research/s101/N2_EDITOR_DRIVER.md).
//   !caw on            start reading the editor's drawn state (src/caw_text.h) from every frame the live sink parses; turns the run-string labels on
//                      (origin::g_labels_wanted, the way --wwe2k14_rdr_ui_text_ours does) for as long as the verb is on. Needs the new renderer's live sink
//                      (owner mode): without it no frame is ever parsed and the answer says so. The ring starts empty.
//   !caw state         the newest state (a one-element array)
//   !caw since GF [N]  the states after guest frame GF (up to N, default 64), oldest first, as ONE line: a JSON array
//   !caw off           stop; the labels go back to what they were
// Switches itself off after 30 s with no on / state / since (a driver that died must not leave the labels on).
// Costs nothing while unused: the frame path tests one atomic bool (caw::g_on) and the labels stay off.
#pragma once
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <string>

#include "caw_text.h"
#include "live_control.h"  // g_caw_live
#include "renderer_adapter/origin_intern.h"  // origin::g_labels_wanted

namespace wwe2k14::caw {

inline std::mutex g_switch_mu;        // on / off / auto-off are one at a time
inline bool g_labels_prior = false;   // what g_labels_wanted was when the verb went on (the ui_text hook may hold it on)

inline void TurnOff() {
  std::lock_guard<std::mutex> sw(g_switch_mu);
  if (g_on.exchange(false)) rdr_adapter::origin::g_labels_wanted.store(g_labels_prior, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_mu);
  g_ring.clear(), g_dropped = 0, g_frames = 0;
}

inline std::string LiveCommand(const std::string& args) {
  std::istringstream words(args);
  std::string verb;
  words >> verb;
  if (verb.empty()) verb = "state";
  if (verb == "on") {
    std::lock_guard<std::mutex> sw(g_switch_mu);
    {
      std::lock_guard<std::mutex> lock(g_mu);
      if (!g_on.load()) g_ring.clear(), g_dropped = 0, g_frames = 0;   // a new session never starts on the last one's states
    }
    g_last_read_ms.store(NowMs());
    if (!g_on.exchange(true)) g_labels_prior = rdr_adapter::origin::g_labels_wanted.exchange(true, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_mu);
    return "on: reading the editor's drawn state from every live frame; frames read " + std::to_string(g_frames) + ", ring " + std::to_string(g_ring.size());
  }
  if (verb == "off") {
    TurnOff();
    return "off";
  }
  if (!g_on.load()) return "error: !caw is off (send !caw on first; the labels are not on, so no frame carries run strings)";
  g_last_read_ms.store(NowMs());
  if (verb == "state") {
    uint64_t newest = 0;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      if (g_ring.empty()) return "error: no frame read yet (the live sink parses frames only in owner mode; wait for one)";
      newest = g_ring.back().guest;
    }
    return Since(newest - 1, 1);
  }
  if (verb == "since") {
    unsigned long long gf = 0, max_states = 64;
    if (!(words >> gf)) return "error: usage !caw since GF [N]";
    words >> max_states;
    return Since(gf, size_t(std::min<unsigned long long>(std::max<unsigned long long>(max_states, 1), 256)));
  }
  return "error: usage !caw on | off | state | since GF [N]";
}

inline const bool g_registered = (wwe2k14::live::g_caw_live = &LiveCommand, g_auto_off = &TurnOff, true);

}  // namespace wwe2k14::caw
