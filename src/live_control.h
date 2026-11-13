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
//   !scene ARGS    S101 lane 32: the scene director (src/scene_director.h): menu ID | mid IDS | victory ID[/LAYOUT] | off
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

// S101 goto: the 30 Years jump's live arming (src/wm30_jump.h sets this; null when that header is not built in).
// Takes the text after "!wm30" ("N", "N CODE" or "off"), returns one line of answer.
inline std::string (*g_wm30_live)(const std::string& args) = nullptr;

// S101 lane 32: the scene director (src/scene_director.h sets this; null when that header is not built in).
// Takes the text after "!scene" ("menu ID", "mid ID,ID", "victory ID[/LAYOUT]" or "off"), returns one line of answer
// ("fired: ..." / "armed: ..." / "disarmed" / "error: ...").
inline std::string (*g_scene_live)(const std::string& args) = nullptr;

// S101 lane 118 (N2 / L-6): the Create a Superstar editor driver's reader (src/caw_live.h sets this; null when that header is not built in).
// Takes the text after "!caw" ("on", "off", "since GF [MAX]", "state"), returns one line of answer ("on: ..." / "off" / "[{...},...]" / "error: ...").
inline std::string (*g_caw_live)(const std::string& args) = nullptr;

// S101 lane 161: the situation director (src/situation_director.h sets this; null when that header is not built in).
// Takes the text after "!director" ("passive 1 on", "passive all off", "momentum 2 100", "stock 0 3", "passive_cpu on"), returns one line of answer
// ("ok: ..." / "failed: ..." / "refused: ..." / "busy: ..." / "error: ...").
inline std::string (*g_director_live)(const std::string& args) = nullptr;

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
  bool wm30_cmd = false;  // S101 goto
  std::string wm30_args;
  bool scene_cmd = false;  // S101 lane 32: !scene (the scene director)
  std::string scene_args;
  bool caw_cmd = false;  // S101 lane 118: !caw (the editor driver's reader)
  std::string caw_args;
  bool director_cmd = false;  // S101 lane 161: !director (the situation director's verbs)
  std::string director_args;
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
      else if (directive == "wm30") {  // S101 goto: !wm30 N [CODE] | !wm30 off - arm the 30 Years jump (src/wm30_jump.h)
        wm30_cmd = true;
        std::getline(words, wm30_args);
      } else if (directive == "scene") {  // S101 lane 32: !scene menu ID | mid IDS | victory ID | off (src/scene_director.h)
        scene_cmd = true;
        std::getline(words, scene_args);
      } else if (directive == "caw") {  // S101 lane 118: !caw on | off | state | since GF [MAX] (src/caw_text.h, src/caw_live.h)
        caw_cmd = true;
        std::getline(words, caw_args);
      } else if (directive == "director") {  // S101 lane 161: !director passive|momentum|stock ... (src/situation_director.h)
        director_cmd = true;
        std::getline(words, director_args);
      } else return fmt::format("status: error\nerror: unknown directive !{}\n.\n", directive);
      continue;
    }
    script += line;
    script += '\n';
  }
  timeout_s = std::clamp(timeout_s, 1, 24 * 3600);
  const int run = std::max(CurrentLogNumber(), 0);
  std::string wm30_note;  // S101 goto: armed before the batch (if any) runs, so one request can arm and press
  // S101 goto: a refused arm is the request's status and nothing else in the request runs (a pick after a refused arm would
  // play the wrong match). An accepted arm with no script and no other directive answers at once; with another directive
  // (status, stop, bookmark, text) the request goes on and that answer carries the wm30: line (AddNote below).
  std::string scene_note;  // S101 lane 32
  auto add_note = [&wm30_note, &scene_note](std::string answer) {
    if (wm30_note.empty() && scene_note.empty()) return answer;
    const size_t nl = answer.find('\n');
    if (nl == std::string::npos) return answer;
    std::string extra;
    if (!wm30_note.empty()) extra += fmt::format("wm30: {}\n", wm30_note);
    if (!scene_note.empty()) extra += fmt::format("scene: {}\n", scene_note);
    return answer.substr(0, nl + 1) + extra + answer.substr(nl + 1);
  };
  if (wm30_cmd) {
    // S101 lane 111 (L-3): a playing recording already holds its wm30 arm (E line); an accepted arm is queued as `E F wm30 ...`.
    // `!wm30 off` stays allowed while playing (resolve._disarm after a SLIP). `!wm30 fire ...` is served by the owner tick and is NOT recorded [U].
    const bool wm30_off = replay::verbs::Trim(wm30_args) == "off";
    wm30_note = replay::verbs::Playing() && !wm30_off ? "error: a recording is playing (its own wm30 arm replays from the recording)"
                : g_wm30_live ? g_wm30_live(wm30_args) : "error: the 30 Years jump is not built in";
    const bool refused = wm30_note.rfind("error", 0) == 0;
    if (!refused && replay::verbs::Trim(wm30_args).rfind("fire", 0) != 0) replay::verbs::Queue(replay::verbs::Wm30Text(wm30_args));
    const bool only_arm = script.find_first_not_of(" \t\n") == std::string::npos && !status_only && !stop && !take_bookmark &&
                          !text_only;
    if (refused || only_arm) {
      return fmt::format("status: {}\nrun: {:03}\nwm30: {}\n.\n", refused ? "error" : "ok", run, wm30_note);
    }
  }
  if (scene_cmd) {  // S101 lane 32: same shape as the wm30 arm: a refusal is the request's status and nothing else runs
    // S101 lane 111 (L-3): same for !scene (`E F scene ...`, served at the owner tick on playback).
    // Recorded where the owner tick serves it (scene_director.h Tick -> verbs::RecordServed), not here; `!scene off` stays allowed while playing.
    scene_note = replay::verbs::Playing() && replay::verbs::Trim(scene_args) != "off"
                     ? "error: a recording is playing (its own scene verbs replay from the recording)"
                 : g_scene_live ? g_scene_live(scene_args) : "error: the scene director is not built in";
    const bool refused = scene_note.rfind("error", 0) == 0;
    if (!refused && replay::verbs::Trim(scene_args) == "off") replay::verbs::Queue(replay::verbs::SceneText(scene_args));  // other scene verbs: recorded at the serve site
    const bool only_arm = script.find_first_not_of(" \t\n") == std::string::npos && !status_only && !stop && !take_bookmark &&
                          !text_only;
    if (refused || only_arm) {
      return fmt::format("status: {}\nrun: {:03}\n{}scene: {}\n.\n", refused ? "error" : "ok", run,
                         wm30_note.empty() ? "" : fmt::format("wm30: {}\n", wm30_note), scene_note);
    }
  }
  if (caw_cmd) {  // S101 lane 118: an answer of its own (one request, one line; nothing else in the request runs)
    const std::string caw_note = g_caw_live ? g_caw_live(caw_args) : "error: the editor reader is not built in";
    return fmt::format("status: {}\nrun: {:03}\ncaw: {}\n.\n", caw_note.rfind("error", 0) == 0 ? "error" : "ok", run, caw_note);
  }
  if (director_cmd) {  // S101 lane 161: an answer of its own (one request, one line; not recorded into a replay: a verb of the director is a test aid, not a recording event)
    const std::string note = g_director_live ? g_director_live(director_args) : "error: the situation director is not built in";
    return fmt::format("status: {}\nrun: {:03}\ndirector: {}\n.\n", note.rfind("ok", 0) == 0 ? "ok" : "error", run, note);
  }
  if (take_bookmark) {  // Session 29: the same bundle as the F11 key ("that, just now" said in chat)
    const uint64_t log_from = LogSize();
    const int k = bookmark::Take();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (bookmark::Written() < k && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return add_note(fmt::format("status: ok\nrun: {:03}\nbookmark: {}\nlog:\n{}.\n", run, bookmark::LastLine(),
                                LogSince(log_from)));
  }
  if (stop) {
    const bool stopped = g_input_script.StopLive();
    if (script.find_first_not_of(" \t\n") == std::string::npos) {
      return add_note(fmt::format("status: ok\nrun: {:03}\nframe: {}\nstopped: {}\n.\n", run, g_input_script.Frame(),
                                  stopped ? "yes" : "nothing was running"));
    }
  }
  if (text_only) {  // Session 30
    std::string recent;
    for (const auto& [f, t] : screen_text::Recent(40)) recent += fmt::format("  {} (frame {})\n", t, f);
    return add_note(fmt::format("status: ok\nrun: {:03}\nframe: {}\ntext: {}\nrecent:\n{}.\n", run, g_input_script.Frame(),
                                screen_text::Joined(2000), recent));
  }
  if (status_only) {
    // Session 29: what the game is doing now, in one call (states from states.toml; fps from the bookmark ring).
    const bookmark::FrameStats one = bookmark::Recent(1.0, 1), five = bookmark::Recent(5.0, 1);
    std::string scene;
    for (const char* name : {"seq", "scene_word", "menu", "cursor", "match_clock"}) {
      const std::string v = bookmark::StateValue(name);
      if (!v.empty()) scene += fmt::format("{}{}={}", scene.empty() ? "" : " ", name, v);
    }
    return add_note(fmt::format("status: ok\nrun: {:03}\nframe: {}\nbusy: {}\nfps: {:.1f} (last 1 s), {:.1f} (last 5 s, worst "
                                "frame {:.1f} ms, {} >= 50 ms)\nstates: {}\ntext: {}\nbookmarks: {}\n.\n",
                                run, g_input_script.Frame(), g_input_script.Busy() ? "yes" : "no", one.fps, five.fps,
                                five.worst_ms, five.over50, scene.empty() ? "(no states.toml)" : scene,
                                screen_text::Joined(300), bookmark::LastLine()));
  }
  using clock = std::chrono::steady_clock;
  const auto deadline = clock::now() + std::chrono::seconds(timeout_s);
  while (g_input_script.Busy() && clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  const uint64_t log_from = LogSize();
  std::string error;
  const uint64_t batch = g_input_script.StartLive(script, error);
  if (!batch) {
    const bool busy = error.rfind("busy", 0) == 0;
    return add_note(fmt::format("status: {}\nrun: {:03}\nerror: {}\nlog:\n{}.\n", busy ? "busy" : "error", run, error,
                                LogSince(log_from)));
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
  if (!wm30_note.empty()) answer += fmt::format("wm30: {}\n", wm30_note);  // S101 goto
  if (!scene_note.empty()) answer += fmt::format("scene: {}\n", scene_note);  // S101 lane 32
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
