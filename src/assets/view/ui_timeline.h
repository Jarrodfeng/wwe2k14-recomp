// Session 98 (entrance timeline bar): the full-width bar at the bottom of the RUNTIME view while an entrance is active.
//   segments = the entrance's cues, laid out in proportion to their real lengths (cue 0 includes the intro hold), each named for what
//   happens in it (assets::entrance::Segments, from the root path and the arena's stage / ramp rectangles); a draggable playhead;
//   transport (previous / next segment, frame step, play / pause, loop, speeds), the camera hand-off, the time readout.
// Every action is an entrance_time command through TimelineCtx::send (queued by the UI; the main loop runs it outside the frame).
// ImGui only; the font helpers come from ui.cpp (the game's menu face).
#pragma once
#include <functional>
#include <string>

#include "imgui.h"
#include "ui.h"

namespace view {
namespace ui {

struct TimelineCtx {
  float d = 1;  // dpi scale
  ImVec2 disp;
  float right_margin = 0;  // the sidebar's width: the bar stops before it
  bool pending = false;    // actions are still queued (a key step must not re-base on a stale time)
  std::function<void(ImDrawList*, ImVec2, float, ImU32, const char*)> text;  // shadowed menu-font text
  std::function<ImVec2(float, const char*)> size;
  std::function<void(const std::string&)> send;
};

struct TimelineState {
  bool loop = false;
  bool scrubbing = false, resume = false;  // a playhead drag: paused while held, plays on again on release if it was playing
  bool prev_playing = false;
  double key_t = -1;  // where the last key step aimed (several key presses may arrive before the main loop applies the first)
  double last_sent = -1;
};

float TimelineHeight(float d);
void DrawTimeline(const EntranceView& ev, TimelineState& st, const TimelineCtx& cx);
// A key press (Win32 VK code). True = used. Call only while ev.active and no text field has the keyboard.
bool TimelineKey(unsigned vk, bool shift, const EntranceView& ev, TimelineState& st, const std::function<void(const std::string&)>& send);

}  // namespace ui
}  // namespace view
