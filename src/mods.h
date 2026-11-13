// wwe2k14 - mods folder (Session 17 item 1A, default off).
//
// --wwe2k14_mods=true mounts a host folder as the guest device mods: (default: `mods` next to the game data, i.e.
// game_data\mods in the dev layout; --wwe2k14_mods_dir=PATH to change it). When the game opens a titantron,
// GAME:\movies\titantron\NNN.bik2 (docs\systems\video_tron.md: one loose Bink 2 file per entrance, opened by the movie
// thread), and the mods folder has titantron\NNN.bik2, the open is redirected there: the game's own Bink decoder plays
// the replacement. The original files are never touched. Every override is logged:
//   [wwe2k14][mods] titantron 343 -> mods:\titantron\343.bik2
// The redirect swaps the guest ANSI string of the open's object attributes for the duration of the kernel call (a
// guest-heap copy of the new path) and restores it afterwards, so the caller's buffers are left as they were.
//
// MP4 / MOV / MKV trons (item 1B) go through src\tron_shim.h instead: ModsFindTron() reports them.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xio.h>
#include <rex/system/xmemory.h>
#include <rex/system/xtypes.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>


REXCVAR_DEFINE_BOOL(wwe2k14_mods, false, "wwe2k14",
                    "Mods folder: a titantron in <mods>\\titantron\\NNN.bik2 replaces movies\\titantron\\NNN.bik2 "
                    "(NNN.mp4 / .mov / .mkv need the tron shim). Session 17, default off");
REXCVAR_DEFINE_STRING(wwe2k14_mods_dir, "", "wwe2k14",
                      "Mods folder (empty = 'mods' next to the game data folder or disc image, e.g. "
                      "game_data\\mods)");

REXCVAR_DEFINE_BOOL(wwe2k14_mods_lighting, false, "wwe2k14",
                    "Session 98: lighting mods: <mods>\\lighting\\arena_NN.json replaces an arena's light colours / "
                    "directions, entrance_NNN.json an entrance's (src\\lighting_mod.h, docs\\systems\\lighting_mod.md). "
                    "Independent of --wwe2k14_mods (the trons / music switch): works with or without it. Default off");

REXCVAR_DEFINE_BOOL(wwe2k14_mods_superstars, false, "wwe2k14",
                    "Session 98: imported superstars (the Superstar Importer's pack in <mods>\\superstars\\pack6) join the "
                    "game as one more DLC pack (src\\superstars.h, src\\dlc.h; needs --wwe2k14_dlc). Independent of "
                    "--wwe2k14_mods. Off = vanilla. Default off (the launchers pass it: USE MY SUPERSTARS)");

namespace wwe2k14::mods {

// Session 98 (review): a path for a log line, UTF-8 (path::string() throws std::system_error on a folder name the ANSI code page
// cannot hold, and these run at boot outside any try).
inline std::string U8(const std::filesystem::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}
// The other way: UTF-8 text (slots.txt, flags) -> a path; an empty path for text that is not valid UTF-8. Never throws.
inline std::filesystem::path FromU8(const std::string& s) {
  try {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
  } catch (...) {
    return {};
  }
}
inline bool g_superstars_on = false;  // --wwe2k14_mods_superstars: src\superstars.h reads g_dir\superstars\ (Session 98)
inline std::filesystem::path g_dir;  // host folder (set by Install when the mods, lighting or superstars setting is on)
inline bool g_on = false;            // the mods: device is mounted (trons, music, logos); NOT set by lighting alone
inline bool g_lighting_on = false;   // --wwe2k14_mods_lighting: src\lighting_mod.h reads g_dir\lighting\ (Session 98)
inline std::atomic<uint32_t> g_overrides{0};

// Extensions in priority order: the game's own format first.
inline constexpr const char* kTronExts[] = {".bik2", ".mp4", ".mov", ".mkv", ".alpha.mp4", ".alpha.mov", ".alpha.mkv"};

// "NNN.alpha.mp4" (stacked alpha: colour on top, alpha as grey below; Session 18 item 2).
inline bool IsStackedAlpha(const std::filesystem::path& p) {
  std::string n = p.filename().string();
  for (auto& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
  return n.find(".alpha.") != std::string::npos;
}

// "GAME:\movies\titantron\343.bik2" (any case, '\' or '/') -> 343; -1 for anything else.
inline int ParseTitantronPath(const std::string& path) {
  static constexpr char kPrefix[] = "game:\\movies\\titantron\\";
  const size_t n = sizeof(kPrefix) - 1;
  if (path.size() < n + 6) return -1;
  for (size_t i = 0; i < n; ++i) {
    char c = char(std::tolower(static_cast<unsigned char>(path[i])));
    if (c == '/') c = '\\';
    if (c != kPrefix[i]) return -1;
  }
  size_t i = n;
  int id = 0, digits = 0;
  while (i < path.size() && std::isdigit(static_cast<unsigned char>(path[i])) && digits < 4) {
    id = id * 10 + (path[i] - '0');
    ++i, ++digits;
  }
  if (!digits || i + 5 != path.size()) return -1;
  std::string ext = path.substr(i);
  for (auto& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
  return ext == ".bik2" ? id : -1;
}

// The replacement for tron `id`, or an empty path. Checked on every open (the launcher can change the folder while
// the game runs).
inline std::filesystem::path FindTron(int id) {
  if (!g_on || id < 0) return {};
  std::error_code ec;
  for (const char* ext : kTronExts) {
    auto p = g_dir / "titantron" / (std::to_string(id) + ext);
    if (std::filesystem::is_regular_file(p, ec)) return p;
  }
  return {};
}

inline void Install(rex::filesystem::VirtualFileSystem* file_system, const std::filesystem::path& game_data_root) {
  const bool lighting = REXCVAR_GET(wwe2k14_mods_lighting);
  const bool superstars = REXCVAR_GET(wwe2k14_mods_superstars);
  if (!REXCVAR_GET(wwe2k14_mods) && !lighting && !superstars) return;
  // The default is next to the game files: game_data\mods for the folder game_data\extracted, and <folder>\mods for
  // a disc image <folder>\X.iso (Session 26).
  const std::string dir_flag = REXCVAR_GET(wwe2k14_mods_dir);
  // Session 101 (lane 199): flags are UTF-8 (ReXGlue converts the wide command line: windowed_app_main_sdl.cpp); path(std::string)
  // would read them in the ANSI code page on Windows, so a mods folder under "C:\Users\João\..." was never found (no trons, no
  // music, no imported superstars) and a mangled twin was created beside it. dlc.h reads its folder flags the same way.
  try {
    g_dir = dir_flag.empty() ? game_data_root.parent_path() / "mods"
                             : std::filesystem::path(std::u8string(dir_flag.begin(), dir_flag.end()));
  } catch (const std::exception&) {  // (review N3: a flag that is not valid UTF-8 must not stop the boot: the old reading)
    g_dir = std::filesystem::path(dir_flag);
  }
  std::error_code ec;
  std::filesystem::create_directories(g_dir / "titantron", ec);
  g_dir = std::filesystem::absolute(g_dir, ec);
  if (lighting) {
    // Session 98: lighting reads plain host files from <mods>\lighting\ on every use; it needs no guest mount, so it works
    // on its own (the launcher's USE MY LIGHTING) and leaves trons / music vanilla while --wwe2k14_mods is off.
    std::filesystem::create_directories(g_dir / "lighting", ec);
    g_lighting_on = true;
    REXLOG_INFO("[wwe2k14][mods] lighting mods on: reading {}", U8(g_dir / "lighting"));
  }
  // Session 98: imported superstars: plain host files too (src\dlc.h mounts the pack folder itself as a DLC pack).
  g_superstars_on = superstars;
  if (!REXCVAR_GET(wwe2k14_mods)) return;  // lighting only: no mods: device, g_on stays false (trons / music vanilla)
  constexpr std::string_view kMountPath = "\\Device\\WWE2K14Mods";
  auto device = std::make_unique<rex::filesystem::HostPathDevice>(kMountPath, g_dir, true);
  if (!device->Initialize() || !file_system->RegisterDevice(std::move(device)) ||
      !file_system->RegisterSymbolicLink("mods:", kMountPath)) {
    REXLOG_WARN("[wwe2k14][mods] failed to mount mods: at {}", U8(g_dir));
    return;
  }
  g_on = true;
  size_t count = 0;
  for (const auto& e : std::filesystem::directory_iterator(g_dir / "titantron", ec)) count += e.is_regular_file();
  REXLOG_INFO("[wwe2k14][mods] mods: mounted at {} ({} file(s) in titantron\\)", U8(g_dir), count);
}

// RAII: redirects one NtCreateFile / NtOpenFile of a titantron to mods:\titantron\NNN.bik2 when that file exists.
class OpenRedirect {
 public:
  explicit OpenRedirect(ppc_ptr_t<rex::system::X_OBJECT_ATTRIBUTES> attrs) {
    if (!g_on || !attrs || !attrs->name_ptr) return;
    auto* mem = REX_KERNEL_MEMORY();
    name_ = mem->TranslateVirtual<rex::system::X_ANSI_STRING*>(attrs->name_ptr);
    const uint32_t len = name_->length, ptr = name_->pointer;
    if (!ptr || !len) return;
    const std::string path(mem->TranslateVirtual<const char*>(ptr), len);
    const int id = ParseTitantronPath(path);
    if (id < 0) return;
    const auto host = FindTron(id);
    if (host.empty()) return;
    const std::string ext = host.extension().string();
    if (ext != ".bik2") return;  // other formats: the tron shim answers the Bink calls (src\tron_shim.h)
    const std::string guest = "mods:\\titantron\\" + std::to_string(id) + ext;
    buf_ = mem->SystemHeapAlloc(uint32_t(guest.size() + 1));
    if (!buf_) return;
    std::memcpy(mem->TranslateVirtual<char*>(buf_), guest.c_str(), guest.size() + 1);
    old_len_ = name_->length, old_max_ = name_->maximum_length, old_ptr_ = name_->pointer;
    name_->length = uint16_t(guest.size());
    name_->maximum_length = uint16_t(guest.size() + 1);
    name_->pointer = buf_;
    REXLOG_INFO("[wwe2k14][mods] titantron {} -> {} (override #{})", id, guest, ++g_overrides);
  }
  ~OpenRedirect() {
    if (!buf_) return;
    name_->length = old_len_, name_->maximum_length = old_max_, name_->pointer = old_ptr_;
    REX_KERNEL_MEMORY()->SystemHeapFree(buf_);
  }
  OpenRedirect(const OpenRedirect&) = delete;
  OpenRedirect& operator=(const OpenRedirect&) = delete;

 private:
  rex::system::X_ANSI_STRING* name_ = nullptr;
  uint32_t buf_ = 0;
  uint16_t old_len_ = 0, old_max_ = 0;
  uint32_t old_ptr_ = 0;
};

}  // namespace wwe2k14::mods
