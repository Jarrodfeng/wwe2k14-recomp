// S102 lane U1 / S103: the message a player sees when the new renderer cannot start, shared by both starts (the stream path's StartLive in reader.h and the stream-free scene host's StartForGame in
// renderer_host/host_start.cpp): the same sentence, the same box on the PC, the same exit request on a phone. Before this the stream-free build showed a black screen (S103 ANS gap 1).
// Included by exactly one translation unit per build (reader.h in a stream build, host_start.cpp in a stream-free one): it DEFINES the two switches.
#pragma once
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "../android_lifecycle.h"   // WriteExitRequest
#include "../platform/compat.h"     // MessageBoxW / ExitProcess
#include "start_failure_text.h"

REXCVAR_DECLARE(bool, wwe2k14_rdr_owner);

namespace wwe2k14::rdr_reader {

REXCVAR_DEFINE_BOOL(wwe2k14_rdr_start_failure_dialog, true, "wwe2k14",
                    "S102 lane U1: when the new renderer cannot start (graphics driver refused the card, window or swapchain), say so in a plain message (a box on the PC; a line in the phone's start-up file and the launcher is asked to come back) instead of a black window for ever. Tests turn the box off");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_test_fail_start, false, "wwe2k14",
                    "S102 lane U1 (+dev builds only): make the new renderer's start fail on purpose, to see the message and the report's verdict line");


// S102 U1 (R9 finding 1): the message. On the PC a box, then the process ends (the game would otherwise run on behind a black window); on a phone the start-up file
// carries the reason and the launcher is asked to come back (the same file the hang guard's exit uses). A test run (no dialog) only logs.
inline void ShowStartFailure(const std::string& reason, const std::string& card) {
  const std::string text = rdr_adapter::start_failure::Text(reason, card);
  REXLOG_ERROR("[rdr][verdict] message for the player: {}", text);
  // Only when the new renderer OWNS the window: otherwise the SDK's presenter still draws and a failed mirror is not a black window (review U1 item 4a).
  if (!REXCVAR_GET(wwe2k14_rdr_start_failure_dialog) || !REXCVAR_GET(wwe2k14_rdr_owner)) return;
#if defined(_WIN32)
  std::wstring wide;  // UTF-8 (a driver's or a card's name may not be ASCII)
  if (const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0); n > 0) {
    wide.resize(size_t(n));
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), wide.data(), n);
  }
  MessageBoxW(nullptr, wide.c_str(), L"WWE 2K14", MB_ICONERROR | MB_OK | MB_TOPMOST | MB_SETFOREGROUND);
  rex::FlushLogging();  // the log line above must be on disk before the process ends
  ExitProcess(1);  // the launcher's kGameReportedErrorExitCode: the game showed its own message
#elif defined(__ANDROID__)
  rex::FlushLogging();
  wwe2k14::android_lifecycle::WriteExitRequest("the new renderer could not start");
#endif
}

}  // namespace wwe2k14::rdr_reader
