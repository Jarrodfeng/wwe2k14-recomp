// S103 landing 2, piece P7 H1: the game-side start of the scene host (host_start.cpp). Light on purpose: the app includes this, not scene_host.h.
// StartForGame / StopForGame exist only in a WWE2K14_KEEP_STREAM=OFF build (a call from a stream build fails to link) and are called ONLY from the app's OFF branch (tools\renderer\host_core_unit.py lints the call sites).
#pragma once

#include <functional>
#include <string>

#include "../keep_stream.h"

namespace wwe2k14::scene_host {
#if !WWE2K14_STREAM()
// S103: set by the app before StartForGame: the blackbox note + ShowStartFailure (kept out of host_start.cpp: those headers DEFINE cvars, one definition per exe).
inline std::function<void(const std::string& reason, const std::string& card)> g_start_failure_sink;
void StartFailedNoWindow();   // the app's no-game-window case: the same message / exit as a failed start
bool StartForGame();   // the end of OnPreLaunchModule (window known, contributors registered, store folder set, guest not running): the keeper sink, the host thread, the frame_tick poster, the quit flush, the HUD hide
void StopForGame();    // shutdown: the poster off, the host thread joined, the pipeline store saved
// S103 AIM: set by the app (before the guest runs) when the SDK made the OFF overlay drawer; called on the game's present thread from the poster (PostTick), once per present: the app asks the UI thread to paint the ImGui dialogs (overlay_off.h RequestPaint).
inline std::function<void()> g_on_present;
#endif
}  // namespace wwe2k14::scene_host
