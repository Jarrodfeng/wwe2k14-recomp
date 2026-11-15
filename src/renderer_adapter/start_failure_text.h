// S102 lane U1 (R9 finding 1): the plain sentence a player sees when the new renderer could not start. Before this a refused device, window or swapchain was
// one ERROR line in the log and a black window for ever while the game, the music and the input went on. Std only (the unit test
// tools\bc\start_failure_check.cpp includes this file alone).
#pragma once
#include <string>

namespace rdr_adapter::start_failure {

// `reason` is the renderer's own sentence (device.cpp / swapchain.cpp: "This graphics hardware is below the minimum. Missing: ..."), `card` the name of the card the
// game asked for in the launcher (empty = automatic). The text names what failed and the three things a player can do; it never blames the game files.
inline std::string Text(const std::string& reason, const std::string& card) {
  std::string t = "WWE 2K14 could not start its graphics.\n\n";
  t += reason.empty() ? std::string("The graphics driver refused to start the game's renderer.") : reason;
  t += "\n\n";
  if (!card.empty()) t += "The launcher asked for the graphics card \"" + card + "\". Pick AUTOMATIC in the launcher's graphics card setting, or another card, and press PLAY again.\n\n";
  else t += "In the launcher, the graphics card setting lists the cards this PC has: pick the dedicated one and press PLAY again.\n\n";
  t += "Also worth trying: update the graphics driver.\nIf it still fails, press REPORT A PROBLEM in the launcher (the report carries this message) and send it.";
  return t;
}

}  // namespace rdr_adapter::start_failure
