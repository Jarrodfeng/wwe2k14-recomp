// wwe2k14 - live control channel (Session 29, test harness; default off).
//
// --wwe2k14_live=NAME opens the local named pipe \\.\pipe\wwe2k14_live_NAME (local clients only). Each connection
// carries one request and gets one answer, then the pipe is free for the next client (tools/live/live.py, the MCP
// wrapper tools/live/mcp_server.py).
//
// Request: UTF-8 text lines, ended by a line holding only "." (or by the client closing its end). Lines starting with
// '!' are directives, the rest are input-script lines (every command of src/input_script.h):
//   !timeout S     seconds to wait for the batch to end (default 120; the batch keeps running after a timeout)
//   !status        answer with the run number, guest frame and whether a script runs (no batch)
//   !stop          end the script that runs now (a long wait, a stuck batch); the batch counts as failed
//   !bookmark      the F11 bundle (src/bookmark.h) now, e.g. when Jarrod says "that, just now" in chat
//   !text          Session 30: the text on screen now and the 40 newest distinct strings (src/screen_text.h)
// The batch waits (up to the timeout) for a script that still runs (e.g. the boot route) to end, then runs as its own
// little script (InputScript::StartLive): on_fail continue, so a failed wait ends the batch, not the game.
//
// Answer: "key: value" lines, then the batch's own lines of the run log, then a line holding only ".":
//   status: ok | failed | timeout | busy | error
//   batch: N            run: NNN            frames: A..B (C)          error: TEXT (busy / error only)
//   log:
//   <every log line written while the batch ran; the client filters them>
// Screenshots: the batch's `screenshot LABEL` lines; the answer is sent once every screenshot is written, and the
// "screenshot saved: PATH" lines in the log part name the files.

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "platform/compat.h"  // <windows.h> on Windows (Session 63)
#endif

#include "bookmark.h"  // recent fps, states, the last bookmark (for !status)
#include "frame_stats.h"
#include "input_script.h"

REXCVAR_DEFINE_STRING(wwe2k14_live, "", "wwe2k14",
                      "Session 29: live control channel: listen on the named pipe \\\\.\\pipe\\wwe2k14_live_NAME for "
                      "batches of input-script lines (src/live_control.h, tools/live/live.py); empty = off");

namespace wwe2k14::live {

constexpr size_t kMaxRequest = 1 << 20;  // bytes
constexpr int kDefaultTimeoutS = 120;

inline std::filesystem::path LogPath() {
  return std::filesystem::current_path() / "logs" / fmt::format("wwe2k14_{:03}.log", std::max(CurrentLogNumber(), 0));
}

inline uint64_t LogSize() {
  std::error_code ec;
  const auto size = std::filesystem::file_size(LogPath(), ec);
  return ec ? 0 : uint64_t(size);
}

// The run log from byte `from` to its end (from the start if the log rotated and is now shorter).
inline std::string LogSince(uint64_t from) {
  std::ifstream in(LogPath(), std::ios::binary);
  if (!in) return "(run log not readable)\n";
  in.seekg(0, std::ios::end);
  const uint64_t size = uint64_t(in.tellg());
  if (from > size) from = 0;
  in.seekg(std::streamoff(from));
  std::string text(size_t(size - from), '\0');
  in.read(text.data(), std::streamsize(text.size()));
  text.resize(size_t(in.gcount()));
  return text;
}

#ifdef _WIN32
inline bool WriteAll(HANDLE pipe, const std::string& text) {
  size_t done = 0;
  while (done < text.size()) {
    DWORD wrote = 0;
    const DWORD chunk = DWORD(std::min<size_t>(text.size() - done, 1 << 16));
    if (!WriteFile(pipe, text.data() + done, chunk, &wrote, nullptr) || !wrote) return false;
    done += wrote;
  }
  return true;
}

// Reads one request: up to a line holding only "." or the client's end of the pipe.
inline std::string ReadRequest(HANDLE pipe) {
  std::string request;
  char buffer[4096];
  for (;;) {
    DWORD got = 0;
    if (!ReadFile(pipe, buffer, sizeof(buffer), &got, nullptr) || !got) break;
    request.append(buffer, got);
    std::string normalised = request;
    normalised.erase(std::remove(normalised.begin(), normalised.end(), '\r'), normalised.end());
    if (normalised == ".\n" || (normalised.size() >= 3 && normalised.ends_with("\n.\n"))) break;
    if (request.size() > kMaxRequest) break;
  }
  request.erase(std::remove(request.begin(), request.end(), '\r'), request.end());
  if (request.ends_with("\n.\n")) request.resize(request.size() - 2);
  else if (request == ".\n") request.clear();
  return request;
}

inline std::string Handle(const std::string& request) {
  int timeout_s = kDefaultTimeoutS;
  bool status_only = false, stop = false, take_bookmark = false, text_only = false;
  std::string script;
  std::istringstream lines(request);
  for (std::string line; std::getline(lines, line);) {
    const size_t first = line.find_first_not_of(" \t");
    if (first != std::string::npos && line[first] == '!') {
      std::istringstream words(line.substr(first + 1));
      std::string directive;
      words >> directive;
      if (directive == "timeout") words >> timeout_s;
      else if (directive == "status") status_only = true;
      else if (directive == "stop") stop = true;
      else if (directive == "bookmark") take_bookmark = true;
      else if (directive == "text") text_only = true;
      else return fmt::format("status: error\nerror: unknown directive !{}\n.\n", directive);
      continue;
    }
    script += line;
    script += '\n';
  }
  timeout_s = std::clamp(timeout_s, 1, 24 * 3600);
  const int run = std::max(CurrentLogNumber(), 0);
  if (take_bookmark) {  // Session 29: the same bundle as the F11 key ("that, just now" said in chat)
    const uint64_t log_from = LogSize();
    const int k = bookmark::Take();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (bookmark::Written() < k && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return fmt::format("status: ok\nrun: {:03}\nbookmark: {}\nlog:\n{}.\n", run, bookmark::LastLine(),
                       LogSince(log_from));
  }
  if (stop) {
    const bool stopped = g_input_script.StopLive();
    if (script.find_first_not_of(" \t\n") == std::string::npos) {
      return fmt::format("status: ok\nrun: {:03}\nframe: {}\nstopped: {}\n.\n", run, g_input_script.Frame(),
                         stopped ? "yes" : "nothing was running");
    }
  }
  if (text_only) {  // Session 30
    std::string recent;
    for (const auto& [f, t] : screen_text::Recent(40)) recent += fmt::format("  {} (frame {})\n", t, f);
    return fmt::format("status: ok\nrun: {:03}\nframe: {}\ntext: {}\nrecent:\n{}.\n", run, g_input_script.Frame(),
                       screen_text::Joined(2000), recent);
  }
  if (status_only) {
    // Session 29: what the game is doing now, in one call (states from states.toml; fps from the bookmark ring).
    const bookmark::FrameStats one = bookmark::Recent(1.0, 1), five = bookmark::Recent(5.0, 1);
    std::string scene;
    for (const char* name : {"seq", "scene_word", "menu", "cursor", "match_clock"}) {
      const std::string v = bookmark::StateValue(name);
      if (!v.empty()) scene += fmt::format("{}{}={}", scene.empty() ? "" : " ", name, v);
    }
    return fmt::format("status: ok\nrun: {:03}\nframe: {}\nbusy: {}\nfps: {:.1f} (last 1 s), {:.1f} (last 5 s, worst "
                       "frame {:.1f} ms, {} >= 50 ms)\nstates: {}\ntext: {}\nbookmarks: {}\n.\n",
                       run, g_input_script.Frame(), g_input_script.Busy() ? "yes" : "no", one.fps, five.fps,
                       five.worst_ms, five.over50, scene.empty() ? "(no states.toml)" : scene, screen_text::Joined(300),
                       bookmark::LastLine());
  }
  using clock = std::chrono::steady_clock;
  const auto deadline = clock::now() + std::chrono::seconds(timeout_s);
  while (g_input_script.Busy() && clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  const uint64_t log_from = LogSize();
  std::string error;
  const uint64_t batch = g_input_script.StartLive(script, error);
  if (!batch) {
    const bool busy = error.rfind("busy", 0) == 0;
    return fmt::format("status: {}\nrun: {:03}\nerror: {}\nlog:\n{}.\n", busy ? "busy" : "error", run, error,
                       LogSince(log_from));
  }
  // Session 30 (#1): the batch's lines as sent (before `include` expansion), so a live session can be saved as a route
  // (py tools\live\live.py save-route NAME reads them back from the run log); the result line follows below.
  {
    std::istringstream sent(script);
    for (std::string line; std::getline(sent, line);) {
      if (line.find_first_not_of(" \t") != std::string::npos) REXLOG_INFO("[wwe2k14][live] batch {}> {}", batch, line);
    }
  }
  InputScript::LiveResult result;
  bool timed_out = false;
  for (;;) {
    if (!g_input_script.LiveStatus(batch, result)) break;  // a newer batch took over (cannot happen: one client)
    if (!result.running) break;
    if (clock::now() >= deadline) {
      timed_out = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  // Screenshots are written on worker threads: answer once they are on disk (at most 10 s more).
  const auto shots_deadline = clock::now() + std::chrono::seconds(10);
  while (g_screenshots_in_flight.load() > 0 && clock::now() < shots_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const char* status = timed_out ? "timeout" : result.failed ? "failed" : "ok";
  REXLOG_INFO("[wwe2k14][live] batch {} result: {}", batch, status);  // Session 30: for save-route
  const uint64_t end = result.running ? result.frame : result.end_frame;
  std::string answer = fmt::format("status: {}\nbatch: {}\nrun: {:03}\nframes: {}..{} ({})\n", status, batch, run,
                                   result.start_frame, end, end - result.start_frame);
  if (timed_out) answer += fmt::format("running: line {} (the batch goes on; !stop ends it)\n", result.line);
  answer += "log:\n";
  answer += LogSince(log_from);
  if (!answer.ends_with('\n')) answer += '\n';
  answer += ".\n";
  return answer;
}

inline void Serve(std::wstring pipe_name) {
  for (;;) {
    HANDLE pipe = CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_DUPLEX,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                   1 << 16, 1 << 16, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      REXLOG_ERROR("[wwe2k14][live] CreateNamedPipe failed (error {}): live channel off", GetLastError());
      return;
    }
    const bool connected = ConnectNamedPipe(pipe, nullptr) ? true : GetLastError() == ERROR_PIPE_CONNECTED;
    if (connected) {
      const std::string request = ReadRequest(pipe);
      const std::string answer = Handle(request);
      WriteAll(pipe, answer);
      FlushFileBuffers(pipe);
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
  }
}
#endif

// From ReXApp::OnPostSetup, after the input script is set up. No-op unless --wwe2k14_live is set.
inline void Install() {
  const std::string name = REXCVAR_GET(wwe2k14_live);
  if (name.empty()) return;
#ifdef _WIN32
  const std::string full = "\\\\.\\pipe\\wwe2k14_live_" + name;
  std::thread([wide = std::wstring(full.begin(), full.end())] { Serve(wide); }).detach();
  REXLOG_INFO("[wwe2k14][live] listening on {} (src/live_control.h; client: py tools\\live\\live.py --name {})", full,
              name);
#else
  REXLOG_WARN("[wwe2k14][live] --wwe2k14_live is Windows only");
#endif
}

}  // namespace wwe2k14::live
