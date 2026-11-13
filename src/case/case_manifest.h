// The case zip's manifest, version 1 (Session 101, docs\renderer\CASE_ZIP.md). One zip = one moment, complete.
// No game or SDK includes: the live path, the phone and the tools all build it. Unknown values stay unset and are written as null.
#pragma once
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace rdr_case {

inline constexpr int kCaseVersion = 2;  // S101 lane32: 2 = the where block carries address_basis / scene (new keys)
inline constexpr const char* kCaseFormat = "wwe2k14-case";

// Entry names inside the zip.
inline constexpr const char* kManifestName = "manifest.json";
inline constexpr const char* kTimingName = "timing.csv";
inline constexpr const char* kLogName = "log.txt";
inline constexpr const char* kInputsName = "inputs.rec";
inline constexpr const char* kStatesName = "states.txt";
inline constexpr const char* kPictureName = "picture.png";
inline constexpr const char* kCaptureDir = "capture/";
inline constexpr const char* kTimingHeader = "guest_frame,step_ms,render_ms,present_ms,shown_ms,captured";

enum class Part { Ours, Game };
inline const char* PartName(Part p) { return p == Part::Ours ? "ours" : "game"; }

enum class Trigger { Key, Chord, Tap, AutoHitch, RunExit, RunShot, RunFailure };
inline const char* TriggerName(Trigger t) {
  switch (t) {
    case Trigger::Key: return "key";
    case Trigger::Chord: return "chord";
    case Trigger::Tap: return "tap";
    case Trigger::AutoHitch: return "auto_hitch";
    case Trigger::RunExit: return "run_exit";
    case Trigger::RunShot: return "run_shot";
    case Trigger::RunFailure: return "run_failure";
  }
  return "key";
}

struct FileEntry {
  std::string path;
  Part part = Part::Ours;
  uint64_t bytes = 0;
  uint64_t xxh3 = 0;
};

struct Manifest {
  std::string id, created, note;
  int utc_offset_min = 0;
  Trigger trigger = Trigger::Key;
  bool ours_only = false;  // part_filter: "ours" = a tester-safe zip with no GAME parts

  struct Build { std::string git, sdk_pin, kind; bool dirty = false; std::optional<int> apk; std::string version; /* S101 lane 19: VERSION + "+dev" */ } build;
  struct Device {
    std::string platform, gpu, driver;
    uint32_t display_width = 0, display_height = 0;
    std::optional<double> refresh_hz;
    uint32_t render_width = 0, render_height = 0;
    double render_scale = 1.0;
    std::optional<int> resolution_scale;  // S101 lane 19: the launcher RESOLUTION multiple (cvar resolution_scale) as in effect at the trigger
    std::optional<double> battery_pct;
    std::optional<std::string> thermal;
  } device;
  struct Settings {
    std::vector<std::pair<std::string, std::string>> effective;  // S101 lane 19: every setting off its default, with its value (host paths redacted)
    std::vector<std::string> non_default;
    std::optional<std::string> launcher_file;  // S101 lane 19: the launcher settings file in effect (its file name only), null = none applied
  } settings;
  struct Where {
    std::optional<std::string> address;  // go-anywhere scheme (RENDERER_ARC.md 4A); null until the resolver exists
    std::string words;
    std::optional<uint32_t> menu_id;
    std::optional<std::string> node;
    std::optional<std::string> route;  // S101 lane 19: the input-script route a run contract run started the game with (file name only)
    std::optional<std::string> match_type, arena, camera_kind;
    std::vector<std::string> wrestlers, attires;
    std::optional<uint64_t> clock_frames;
    uint64_t guest_frame = 0;
    // S101 lane32: this state was reached by forcing a scene (src/scene_director.h); verb menu | mid | victory, the scene id, the guest frame it was forced at.
    struct Forced { std::string verb; uint32_t id = 0; uint64_t frame = 0; };
    std::optional<Forced> forced;
    // S101 lane32 (where, filled by src/case/case_where.h FillWhere): how exact the address is, and what the scene machine says.
    std::optional<std::string> address_basis;  // "exact: ..." / "best-effort: ..." parts, what each address part came from
    struct Scene {
      std::optional<int64_t> scene_word, bank, ctx_type;  // the in-game scene word (states.toml), the scene ring's read context bank id / type (S-1 layout)
      std::vector<int64_t> entrance_steps;               // entrance_step_0..5 (states.toml), -1 = unreadable
    } scene;
  } where;
  struct WayBack {
    std::optional<std::string> builder;
    uint64_t first_frame = 0, moment_frame = 0;
    std::optional<uint64_t> seed;          // S101 lane 19: null = no fixed seed was set (reason in `unavailable`)
    bool replay_mode = false;
    std::optional<uint32_t> inputs_rows;   // S101 lane 19: controller rows in inputs.rec for the window; null = none recorded (reason in `unavailable`)
  } way_back;
  struct Window {
    double seconds = 0;
    uint32_t frames = 0, capture_frames = 0;
    uint64_t first_guest_frame = 0, capture_bytes = 0;
    uint32_t unresolved = 0;        // S101 lane 11: draw / content refs the ring did not have (a draw is marked no-geometry, a content ref is dropped); 0 = the zip replays whole
    uint64_t baseline_dropped = 0;  // S101 lane 11: baseline textures the ring let go of for its cap (never evicted now: always 0)
  } window;
  struct Names {
    std::optional<uint32_t> unnamed_draws, unnamed_surfaces, unnamed_textures;
    // S101 lane 19: the [rdr][names] counts summed over the window's frames (the same counters the census logs).
    std::optional<uint64_t> draws, untagged_draws, unclassified_draws, surfaces_guessed, textures_unnamed, texture_binds;
  } names;
  // S101 lane 19: a field the build could not fill, with the reason ("device.refresh_hz" -> "..."). Every field the manifest lists as required
  // has a value or an entry here (intake checks it); a value is never made up and a zero never stands for "not measured".
  std::vector<std::pair<std::string, std::string>> unavailable;
  void Unavailable(const std::string& field, const std::string& reason) {
    for (auto& kv : unavailable)
      if (kv.first == field) { kv.second = reason; return; }
    unavailable.emplace_back(field, reason);
  }
  bool IsUnavailable(const std::string& field) const {
    for (const auto& kv : unavailable)
      if (kv.first == field) return true;
    return false;
  }
  std::vector<FileEntry> files;
};

// ---- JSON writer (small, enough for this manifest) ----------------------------------------------------------------------------------
namespace detail {
inline std::string Esc(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if (c == '\t') o += "\\t";
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
    else o += char(c);
  }
  return o + "\"";
}
inline std::string Opt(const std::optional<std::string>& v) { return v ? Esc(*v) : "null"; }
template <class T> std::string OptNum(const std::optional<T>& v) { return v ? std::to_string(*v) : "null"; }
inline std::string Num(double v) { char b[32]; std::snprintf(b, sizeof(b), "%.3f", v); return b; }
inline std::string OptD(const std::optional<double>& v) { return v ? Num(*v) : "null"; }
inline std::string List(const std::vector<std::string>& v) {
  std::string o = "[";
  for (size_t i = 0; i < v.size(); ++i) o += (i ? ", " : "") + Esc(v[i]);
  return o + "]";
}
inline std::string Hex64(uint64_t v) { char b[24]; std::snprintf(b, sizeof(b), "%016llx", (unsigned long long)v); return b; }
}  // namespace detail

inline std::string ToJson(const Manifest& m) {
  using namespace detail;
  std::string o = "{\n";
  o += " \"format\": " + Esc(kCaseFormat) + ", \"version\": " + std::to_string(kCaseVersion) + ",\n";
  o += " \"id\": " + Esc(m.id) + ",\n";
  o += " \"created\": " + Esc(m.created) + ", \"utc_offset_min\": " + std::to_string(m.utc_offset_min) + ",\n";
  o += " \"trigger\": " + Esc(TriggerName(m.trigger)) + ",\n";
  o += " \"note\": " + Esc(m.note) + ",\n";
  o += std::string(" \"part_filter\": ") + (m.ours_only ? "\"ours\"" : "\"all\"") + ",\n";
  o += " \"build\": {\"git\": " + Esc(m.build.git) + ", \"dirty\": " + (m.build.dirty ? "true" : "false") + ", \"sdk_pin\": " +
       Esc(m.build.sdk_pin) + ", \"apk\": " + OptNum(m.build.apk) + ", \"kind\": " + Esc(m.build.kind) + ", \"version\": " + Esc(m.build.version) + "},\n";
  const auto& d = m.device;
  o += " \"device\": {\"platform\": " + Esc(d.platform) + ", \"gpu\": " + Esc(d.gpu) + ", \"driver\": " + Esc(d.driver) +
       ", \"display\": {\"width\": " + std::to_string(d.display_width) + ", \"height\": " + std::to_string(d.display_height) +
       ", \"refresh_hz\": " + OptD(d.refresh_hz) + "}, \"render\": {\"width\": " + std::to_string(d.render_width) + ", \"height\": " +
       std::to_string(d.render_height) + ", \"scale\": " + Num(d.render_scale) + ", \"resolution_scale\": " + OptNum(d.resolution_scale) + "}, \"battery_pct\": " + OptD(d.battery_pct) +
       ", \"thermal\": " + Opt(d.thermal) + "},\n";
  o += " \"settings\": {\"effective\": {";
  for (size_t i = 0; i < m.settings.effective.size(); ++i)
    o += (i ? ", " : "") + Esc(m.settings.effective[i].first) + ": " + Esc(m.settings.effective[i].second);
  o += "}, \"non_default\": " + List(m.settings.non_default) + ", \"launcher_file\": " + Opt(m.settings.launcher_file) + "},\n";
  const auto& w = m.where;
  o += " \"where\": {\"address\": " + Opt(w.address) + ", \"words\": " + Esc(w.words) + ", \"screen\": {\"menu_id\": " + OptNum(w.menu_id) +
       ", \"node\": " + Opt(w.node) + "}, \"route\": " + Opt(w.route) + ", \"match\": {\"type\": " + Opt(w.match_type) + ", \"arena\": " + Opt(w.arena) + ", \"wrestlers\": " +
       List(w.wrestlers) + ", \"attires\": " + List(w.attires) + ", \"clock_frames\": " + OptNum(w.clock_frames) + "}, \"camera\": {\"kind\": " +
       Opt(w.camera_kind) + "}, \"guest_frame\": " + std::to_string(w.guest_frame) +
       ", \"address_basis\": " + Opt(w.address_basis) + ", \"scene\": {\"scene_word\": " + OptNum(w.scene.scene_word) + ", \"bank\": " + OptNum(w.scene.bank) +
       ", \"ctx_type\": " + OptNum(w.scene.ctx_type) + ", \"entrance_steps\": [" + [&] { std::string s; for (size_t i = 0; i < w.scene.entrance_steps.size(); ++i) s += (i ? ", " : "") + std::to_string(w.scene.entrance_steps[i]); return s; }() + "]}" +
       (w.forced ? ", \"forced\": {\"verb\": " + Esc(w.forced->verb) + ", \"id\": " + std::to_string(w.forced->id) + ", \"frame\": " +
                       std::to_string(w.forced->frame) + "}"  // S101 lane32: only when a scene was forced (the key is absent otherwise)
                 : std::string()) + "},\n";
  const auto& b = m.way_back;
  o += " \"way_back\": {\"builder\": " + Opt(b.builder) + ", \"inputs\": " + Esc(kInputsName) + ", \"first_frame\": " + std::to_string(b.first_frame) +
       ", \"moment_frame\": " + std::to_string(b.moment_frame) + ", \"seed\": " + OptNum(b.seed) + ", \"replay_mode\": " +
       (b.replay_mode ? "true" : "false") + ", \"inputs_rows\": " + OptNum(b.inputs_rows) + "},\n";
  const auto& wn = m.window;
  o += " \"window\": {\"seconds\": " + Num(wn.seconds) + ", \"frames\": " + std::to_string(wn.frames) + ", \"first_guest_frame\": " +
       std::to_string(wn.first_guest_frame) + ", \"capture_frames\": " + std::to_string(wn.capture_frames) + ", \"capture_bytes\": " +
       std::to_string(wn.capture_bytes) + ", \"unresolved\": " + std::to_string(wn.unresolved) + ", \"baseline_dropped\": " +
       std::to_string(wn.baseline_dropped) + "},\n";
  o += " \"names\": {\"unnamed_draws\": " + OptNum(m.names.unnamed_draws) + ", \"unnamed_surfaces\": " + OptNum(m.names.unnamed_surfaces) +
       ", \"unnamed_textures\": " + OptNum(m.names.unnamed_textures) + ", \"draws\": " + OptNum(m.names.draws) + ", \"untagged_draws\": " +
       OptNum(m.names.untagged_draws) + ", \"unclassified_draws\": " + OptNum(m.names.unclassified_draws) + ", \"surfaces_guessed\": " +
       OptNum(m.names.surfaces_guessed) + ", \"textures_unnamed\": " + OptNum(m.names.textures_unnamed) + ", \"texture_binds\": " +
       OptNum(m.names.texture_binds) + "},\n";
  o += " \"unavailable\": {";
  for (size_t i = 0; i < m.unavailable.size(); ++i) o += (i ? ", " : "") + Esc(m.unavailable[i].first) + ": " + Esc(m.unavailable[i].second);
  o += "},\n";
  o += " \"files\": [";
  for (size_t i = 0; i < m.files.size(); ++i) {
    const auto& f = m.files[i];
    o += std::string(i ? ",\n  " : "\n  ") + "{\"path\": " + Esc(f.path) + ", \"part\": " + Esc(PartName(f.part)) + ", \"bytes\": " +
         std::to_string(f.bytes) + ", \"xxh3\": " + Esc(Hex64(f.xxh3)) + "}";
  }
  o += "\n ]\n}\n";
  return o;
}

}  // namespace rdr_case
