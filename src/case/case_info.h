// What the build already knows, put into the case zip's manifest (Session 101, lane 19; docs\systems\case_zip.md). No game or SDK includes: the live
// path, the phone and the selftest all build it.
//   * the renderer's own log lines ("[rdr] device: ...", "[rdr] swapchain WxH", "[rdr] display timing: refresh X ms") are read as they are logged
//     (NoteRendererLine, called from live.cpp's log tee), so the device block comes from the presenter and never from a second query;
//   * the [rdr][names] counters (reader.h g_names / g_roles / g_prov) are sampled once per frame through a probe the app sets (SetNamesProbe)
//     and summed over the window's frames (FillNames);
//   * the recorder's rows for the window (InputsWindow);
//   * Complete(): every required field has a value or a reason in manifest.unavailable. A value is never made up and a zero never stands for
//     "not measured".
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "../renderer/log_formats.h"  // the renderer's own log formats (review fix 5)
#include "case_manifest.h"

namespace rdr_case::info {

// ---- the device, from the renderer's log lines ----
struct DeviceInfo {
  std::string gpu, driver;
  uint32_t surface_w = 0, surface_h = 0;       // the swapchain (the picture's size on the display)
  std::optional<double> refresh_hz;            // only where the presenter reads the display's refresh (display timing: phones)
  uint32_t render_w = 0, render_h = 0;
  bool sink_started = false;                   // SetRender ran: the renderer was started, so its log lines should have arrived (review fix 5)
  double render_scale = 1.0;
};
namespace detail {
struct DeviceState {
  std::mutex mu;
  DeviceInfo d;
};
inline DeviceState& Dev() {
  static DeviceState* s = new DeviceState;  // leaked: lines may arrive from any thread late in shutdown
  return *s;
}
inline bool StartsWith(const char* s, const char* prefix) { return std::strncmp(s, prefix, std::strlen(prefix)) == 0; }
}  // namespace detail

// One renderer log line ("[rdr] ..."). Lines this does not know are ignored. Any thread.
inline void NoteRendererLine(const char* line) {
  if (!line) return;
  detail::DeviceState& s = detail::Dev();
  if (detail::StartsWith(line, RDR_LOG_DEVICE_PREFIX)) {  // "[rdr] device: NAME, Vulkan 1.3.280, driver 0000ABCD, unified memory ..."
    const std::string l = line + std::strlen(RDR_LOG_DEVICE_PREFIX);
    const size_t vk = l.find(", Vulkan ");
    const size_t drv = l.find(", driver ");
    if (vk == std::string::npos) return;
    std::lock_guard lock(s.mu);
    s.d.gpu = l.substr(0, vk);
    std::string vulkan = l.substr(vk + 9, drv == std::string::npos ? std::string::npos : drv - (vk + 9));
    std::string driver = drv == std::string::npos ? "" : l.substr(drv + 9, l.find(',', drv + 9) - (drv + 9));
    s.d.driver = "Vulkan " + vulkan + (driver.empty() ? "" : ", driver 0x" + driver);
  } else if (detail::StartsWith(line, RDR_LOG_SWAPCHAIN_PREFIX)) {  // "[rdr] swapchain 1920x1080, 3 images, ..."
    unsigned w = 0, h = 0;
    if (std::sscanf(line + std::strlen(RDR_LOG_SWAPCHAIN_PREFIX), "%ux%u", &w, &h) == 2 && w && h) {
      std::lock_guard lock(s.mu);
      s.d.surface_w = w, s.d.surface_h = h;
    }
  } else if (detail::StartsWith(line, RDR_LOG_DISPLAY_TIMING_PREFIX)) {  // "refresh 16.580 ms" or "the refresh is now 8.300 ms (was ...)"
    const char* at = std::strstr(line, "refresh is now ");
    at = at ? at + 15 : std::strstr(line, "refresh ");
    if (!at) return;
    if (!std::strstr(line, "refresh is now ")) at += 8;
    const double ms = std::atof(at);
    std::lock_guard lock(s.mu);
    if (ms > 0.5) s.d.refresh_hz = 1000.0 / ms;
    else s.d.refresh_hz.reset();
  }
}
// The renderer's draw size (the game's 1280x720 targets drawn `scale` times larger). live.cpp, when it starts.
inline void SetRender(uint32_t w, uint32_t h, double scale) {
  detail::DeviceState& s = detail::Dev();
  std::lock_guard lock(s.mu);
  s.d.render_w = w, s.d.render_h = h, s.d.render_scale = scale, s.d.sink_started = true;
}
inline DeviceInfo Device() {
  detail::DeviceState& s = detail::Dev();
  std::lock_guard lock(s.mu);
  return s.d;
}
inline void ResetDevice() {
  detail::DeviceState& s = detail::Dev();
  std::lock_guard lock(s.mu);
  s.d = DeviceInfo{};
}
// Fills what the renderer reported into the manifest; a field it did not report stays as the app set it (or empty: Complete() then says why).
inline void FillDevice(Manifest& m) {
  const DeviceInfo d = Device();
  if (m.device.gpu.empty()) m.device.gpu = d.gpu;
  if (m.device.driver.empty()) m.device.driver = d.driver;
  if (!m.device.display_width) m.device.display_width = d.surface_w, m.device.display_height = d.surface_h;
  if (!m.device.refresh_hz) m.device.refresh_hz = d.refresh_hz;
  if (!m.device.render_width) m.device.render_width = d.render_w, m.device.render_height = d.render_h, m.device.render_scale = d.render_scale;
}

// ---- the [rdr][names] counts of the window ----
struct NamesSample {  // cumulative counters at one moment; a counter that went down was reset by the census: its new value is the whole delta
  uint64_t draws = 0, untagged = 0, unclassified = 0, textures_unnamed = 0, texture_binds = 0, surfaces_guessed = 0;
};
namespace detail {
struct NamesRow {
  uint64_t guest_frame = 0;
  NamesSample delta;
};
struct NamesState {
  std::mutex mu;
  std::function<NamesSample()> probe;
  NamesSample prev;
  bool have_prev = false;
  std::deque<NamesRow> rows;
};
inline NamesState& Names() {
  static NamesState* s = new NamesState;
  return *s;
}
inline uint64_t DeltaOf(uint64_t cur, uint64_t prev) { return cur >= prev ? cur - prev : cur; }
}  // namespace detail

inline void SetNamesProbe(std::function<NamesSample()> probe) {
  detail::NamesState& s = detail::Names();
  std::lock_guard lock(s.mu);
  s.probe = std::move(probe);
  s.have_prev = false;
  s.rows.clear();
}
inline bool HasNamesProbe() {
  detail::NamesState& s = detail::Names();
  std::lock_guard lock(s.mu);
  return static_cast<bool>(s.probe);
}
// Once per frame handed to the case ring (case_hook::OnFrame).
inline void SampleNames(uint64_t guest_frame) {
  detail::NamesState& s = detail::Names();
  std::function<NamesSample()> probe;
  {
    std::lock_guard lock(s.mu);
    probe = s.probe;
  }
  if (!probe) return;
  const NamesSample cur = probe();  // (outside our lock: the probe takes the reader's own)
  std::lock_guard lock(s.mu);
  detail::NamesRow row;
  row.guest_frame = guest_frame;
  if (!s.have_prev) {  // review fix 10: the first sample has nothing to subtract from: no row (a zero would read as "nothing happened")
    s.prev = cur;
    s.have_prev = true;
    return;
  }
  {
    row.delta.draws = detail::DeltaOf(cur.draws, s.prev.draws);
    row.delta.untagged = detail::DeltaOf(cur.untagged, s.prev.untagged);
    row.delta.unclassified = detail::DeltaOf(cur.unclassified, s.prev.unclassified);
    row.delta.textures_unnamed = detail::DeltaOf(cur.textures_unnamed, s.prev.textures_unnamed);
    row.delta.texture_binds = detail::DeltaOf(cur.texture_binds, s.prev.texture_binds);
    row.delta.surfaces_guessed = detail::DeltaOf(cur.surfaces_guessed, s.prev.surfaces_guessed);
  }
  s.prev = cur;
  s.have_prev = true;
  s.rows.push_back(row);
  while (s.rows.size() > 16384) s.rows.pop_front();  // (a window above ~4.5 min at 60 fps would lose its oldest rows; the default is 5 s)
}
// Sums the frames first..last (guest frames). Nothing sampled in the window: the counts stay null (Complete() says why).
inline void FillNames(Manifest& m, uint64_t first, uint64_t last) {
  detail::NamesState& s = detail::Names();
  std::lock_guard lock(s.mu);
  if (!s.probe) return;
  NamesSample sum;
  uint32_t rows = 0;
  for (const detail::NamesRow& r : s.rows) {
    if (r.guest_frame < first || r.guest_frame > last) continue;
    sum.draws += r.delta.draws, sum.untagged += r.delta.untagged, sum.unclassified += r.delta.unclassified;
    sum.textures_unnamed += r.delta.textures_unnamed, sum.texture_binds += r.delta.texture_binds, sum.surfaces_guessed += r.delta.surfaces_guessed;
    ++rows;
  }
  if (!rows) return;
  m.names.draws = sum.draws, m.names.untagged_draws = sum.untagged, m.names.unclassified_draws = sum.unclassified;
  m.names.textures_unnamed = sum.textures_unnamed, m.names.texture_binds = sum.texture_binds, m.names.surfaces_guessed = sum.surfaces_guessed;
  m.names.unnamed_draws = uint32_t(sum.untagged + sum.unclassified);
  m.names.unnamed_surfaces = uint32_t(sum.surfaces_guessed);
  m.names.unnamed_textures = uint32_t(sum.textures_unnamed);
}

// ---- inputs.rec: the replay recorder's file (src\replay.h format) cut to the window ----
// Keeps every line that is not a controller row (comments, name / seed / dlc / flag), the rows with first <= frame <= last, and, as context for a
// reader, the newest S / C row before `first` for each (tag, user, flags). The cut file is FOR READING (what the pads did in the window); it does NOT
// play back on its own: a pad key with no row before the window answers "not connected" until its first row, and the T totals (controller-call
// counts since the start) would SLIP. Frame-exact way-backs need the whole session recording (lane 31 asks for it; not shipped in the zip yet).
// A last line the recorder had not finished (no newline after the flush) is dropped.
// `rows` = controller rows (S / K / C) inside the window.
inline std::string InputsWindow(const std::string& text, uint64_t first, uint64_t last, uint32_t* rows = nullptr) {
  std::string head, before_out, body;
  std::map<std::tuple<char, std::string, std::string>, std::string> before;  // (tag, user, flags) -> newest row before the window
  uint32_t n = 0;
  size_t at = 0;
  while (at < text.size()) {
    size_t eol = text.find('\n', at);
    if (eol == std::string::npos) break;  // an unterminated last row: the recorder was mid-row at the flush
    std::string line = text.substr(at, eol - at);
    at = eol + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    const char tag = line[0];
    const bool row = line.size() > 2 && line[1] == ' ' && (tag == 'S' || tag == 'K' || tag == 'C' || tag == 'T' || tag == 'E') && line[2] >= '0' && line[2] <= '9';
    if (!row) {
      head += line + "\n";
      continue;
    }
    const uint64_t frame = std::strtoull(line.c_str() + 2, nullptr, 10);
    if (frame >= first && frame <= last) {
      body += line + "\n";
      if (tag == 'S' || tag == 'K' || tag == 'C') ++n;
    } else if (frame < first && (tag == 'S' || tag == 'C')) {
      // fields: tag F K U FL ...
      size_t p = 0;
      std::vector<std::string> f;
      while (p < line.size() && f.size() < 5) {
        size_t sp = line.find(' ', p);
        if (sp == std::string::npos) sp = line.size();
        f.push_back(line.substr(p, sp - p));
        p = sp + 1;
      }
      if (f.size() >= 5) before[{tag, f[3], f[4]}] = line;
    }
  }
  for (const auto& kv : before) before_out += kv.second + "\n";
  if (rows) *rows = n;
  return head + "# lane 19: window " + std::to_string(first) + ".." + std::to_string(last) + " cut from the run's recording; rows before it = each pad's last state\n" +
         before_out + body;
}

// ---- Complete(): a value or a reason for every required field ----
inline bool Missing(const std::string& s) { return s.empty() || s == "unknown"; }
inline void Complete(Manifest& m) {
  auto need = [&](const char* field, bool have, const char* reason) {
    if (!have && !m.IsUnavailable(field)) m.Unavailable(field, reason);
  };
  need("build.git", !Missing(m.build.git), "this build was not stamped with its git hash (built outside the CMake stamp target)");
  need("build.sdk_pin", !Missing(m.build.sdk_pin), "no SDK pin was found for this build (no tools\\sdk_pins.json match for the SDK DLLs, no SDK source)");
  need("build.kind", !Missing(m.build.kind), "the build kind (dev / release / apk) was not stamped");
  need("build.apk", m.build.apk.has_value(), "not an Android build (the APK number is android\\VERSION's versionCode)");
  const bool started = Device().sink_started;  // the renderer ran: a missing line is format drift, not "no renderer" (review fix 5)
  const char* parse = "PARSE FAILED: the renderer was started but its log line was not understood (src/renderer/log_formats.h changed?) ";
  need("device.gpu", !m.device.gpu.empty(), started ? parse : "the renderer did not log \"[rdr] device:\" (no owner-mode renderer ran)");
  need("device.driver", !m.device.driver.empty(), started ? parse : "the renderer did not log \"[rdr] device:\" (no owner-mode renderer ran)");
  need("device.display", m.device.display_width && m.device.display_height, "the renderer did not log \"[rdr] swapchain\" (a headless mirror has no swapchain, or the renderer did not start)");
  need("device.refresh_hz", m.device.refresh_hz.has_value(),
       "the presenter reads the display refresh only with VK_GOOGLE_display_timing (Android); the PC presenter does not measure it");
  need("device.render", m.device.render_width && m.device.render_height, "the live sink did not report its draw size");
  need("device.resolution_scale", m.device.resolution_scale.has_value(), "the app did not report the resolution_scale setting in effect");
  need("where.address", m.where.address.has_value(), "the caller did not fill where (no live game state was handed to the case writer): no goto address names this state");  // S101 lane32
  need("where.route", m.where.route.has_value(), "the game was not started by an input-script route (a run contract run); a player or hand-driven session");
  need("device.battery_pct", m.device.battery_pct.has_value(), "not read: a PC has no battery query here; the phone's CaseButton JNI seam does not pass it yet");
  need("device.thermal", m.device.thermal.has_value(), "not read: a PC has no thermal query here; the phone's CaseButton JNI seam does not pass it yet");
  need("settings.launcher_file", m.settings.launcher_file.has_value(), "no launcher settings file was applied to this run");
  need("way_back.builder", m.way_back.builder.has_value(), "the game was not entered through a match builder (--wwe2k14_launch) or the 30 Years jump");
  need("way_back.seed", m.way_back.seed.has_value(), "no fixed seed was set (replay mode off)");
  need("way_back.inputs_rows", m.way_back.inputs_rows.has_value(), "the replay recorder was not running (--wwe2k14_replay_record), so no controller rows exist");
  need("names", m.names.draws.has_value(), "no [rdr][names] probe was set, or no frame of the window was sampled");
}

}  // namespace rdr_case::info
