// wwe2k14 - DLC packs from the player's own folder (Session 32, plan item 2; docs\systems\dlc.md).
//
// The game asks for marketplace content once at boot (XamContentCreateEnumerator, content type 2), opens each item it
// gets as dlc0:, dlc1:, ... (XamContentCreateEx, OPEN_EXISTING, with a licence mask) and reads info/catalog.dlc from
// each. ReXGlue's own content manager only lists folders inside the user data folder and answers licence 0, so with
// --wwe2k14_dlc=true this header answers those three calls for the packs in the DLC folder itself:
//   - the enumeration gets one item per ready pack that is not switched off (--wwe2k14_dlc_off=3,5: pack ids), after
//     whatever ReXGlue lists;
//   - opening one of them gives the root name a read-only view of the pack's folder, with every licence bit set (the
//     packs are the player's own files);
//   - closing it removes the root name. Each pack's device stays registered for the whole run, so a file the game
//     still has open from a pack can never point into a removed device.
// Every other content call (saves, ReXGlue's own content) goes to ReXGlue unchanged, and with the setting off (or no
// packs) nothing here changes what the game sees. The DLC folder (empty --wwe2k14_dlc_dir) is `dlc` next to the game
// data folder or disc image, the same rule as the mods folder. A pack is an extracted folder or an Xbox 360 package
// (STFS, read in place by ReXGlue's StfsContainerDevice once dlc_catalog.h has checked it). Damaged, incomplete and
// duplicate packs, and packages for other games, are listed in the log and skipped. Nothing here writes to the DLC
// folder. (The default folder rule is
// DefaultDir() in dlc_catalog.h: next to the game data, else next to the exe; the launcher uses the same.)
#pragma once

#include <rex/cvar.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "dlc_catalog.h"
#include "gfx_settings.h"
#include "replay.h"  // g_dlc_offered: a recording keeps the packs it was made with

REXCVAR_DEFINE_BOOL(wwe2k14_dlc, false, "wwe2k14",
                    "Session 32: give the game the DLC packs in the DLC folder (one folder per pack with "
                    "info\\catalog.dlc), read-only and licensed. Off = ReXGlue's own content handling only");
REXCVAR_DEFINE_STRING(wwe2k14_dlc_dir, "", "wwe2k14",
                      "Session 32: the DLC folder (empty = 'dlc' next to the game data folder or disc image, e.g. "
                      "game_data\\dlc, else 'dlc' next to the exe)");
REXCVAR_DEFINE_STRING(wwe2k14_dlc_off, "", "wwe2k14",
                      "Session 32: pack ids switched off, comma separated (e.g. 3,5); the launcher's DLC switches");
REXCVAR_DEFINE_BOOL(wwe2k14_log_content_calls, true, "wwe2k14",
                    "Session 32: log the game's content calls (enumerate / open / close: saves and DLC) with the "
                    "answers");

namespace rex::kernel::xam {
// ReXGlue's implementations (exported by rexruntime.dll, not in the public headers; signatures checked against the
// mangled names in rexruntime.lib, Session 32).
uint32_t XamContentCreateEnumerator_entry(uint32_t user_index, uint32_t device_id, uint32_t content_type,
                                          uint32_t content_flags, uint32_t items_per_enumerate,
                                          mapped_u32 buffer_size_ptr, mapped_u32 handle_out);
uint32_t XamContentCreateEx_entry(uint32_t user_index, mapped_string root_name, mapped_void content_data_ptr,
                                  uint32_t flags, mapped_u32 disposition_ptr, mapped_u32 license_mask_ptr,
                                  uint32_t cache_size, uint64_t content_size, mapped_void overlapped_ptr);
uint32_t XamContentClose_entry(mapped_string root_name, mapped_void overlapped_ptr);
}  // namespace rex::kernel::xam

namespace wwe2k14::dlc {

using rex::X_HRESULT;  // the X_ERROR_* / X_HRESULT_FROM_WIN32 macros name these unqualified
using rex::X_RESULT;
using rex::system::xam::XCONTENT_DATA;

constexpr uint32_t kMarketplaceContent = 2;  // XContentType::kMarketplaceContent
inline uint32_t TypeOf(const XCONTENT_DATA& d) { return static_cast<uint32_t>(rex::system::XContentType(d.content_type)); }
constexpr uint32_t kHddDeviceId = 1;         // DummyDeviceId::HDD

struct Offered {
  const Pack* pack = nullptr;
  std::string file_name;    // the XCONTENT_DATA file name we give it (unique per pack id)
  std::string device_path;  // its read-only device
};

inline std::mutex g_mutex;
inline bool g_on = false;
inline fs::path g_dir;
inline std::vector<Pack> g_packs;                // every pack found, any status
inline std::vector<Offered> g_offered;           // the ones the game gets
inline std::map<std::string, size_t> g_roots;    // lower-case root name the game opened -> g_offered index
inline std::atomic<uint32_t> g_opens{0};

inline std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

inline std::string Mb(uint64_t bytes) {
  char b[32];
  std::snprintf(b, sizeof(b), "%.1f MB", double(bytes) / (1024.0 * 1024.0));
  return b;
}

// Before the presets (OnPreSetup, release builds): the launcher's DLC settings, for a game started without it. A
// setting the command line gave wins.
inline void ApplyLauncherSettings(const fs::path& settings_file) {
  const std::string json = gfxs::ReadFile(settings_file);
  if (json.empty()) return;
  auto unset = [](const char* n) { return rex::cvar::GetFlagSource(n) == rex::cvar::Source::kDefault; };
  bool on = false;
  std::string dir, off, applied;
  if (unset("wwe2k14_dlc") && gfxs::GetBool(json, "dlc", on)) {
    rex::cvar::SetFlagByName("wwe2k14_dlc", on ? "true" : "false");
    applied += std::string(" dlc=") + (on ? "true" : "false");
  }
  if (unset("wwe2k14_dlc_dir") && gfxs::GetString(json, "dlc_dir", dir) && !dir.empty()) {
    rex::cvar::SetFlagByName("wwe2k14_dlc_dir", dir);
    applied += " dlc_dir=" + dir;
  }
  if (unset("wwe2k14_dlc_off") && gfxs::GetString(json, "dlc_off", off) && !off.empty()) {
    rex::cvar::SetFlagByName("wwe2k14_dlc_off", off);
    applied += " dlc_off=" + off;
  }
  if (!applied.empty()) REXLOG_INFO("[wwe2k14][dlc] from the settings file:{}", applied);
}

// After setup (OnPostSetup): find the packs and register a read-only device for each one the game gets.
inline void Install(rex::filesystem::VirtualFileSystem* file_system, const fs::path& game_data_root) {
  if (!REXCVAR_GET(wwe2k14_dlc)) return;
  const std::string dir_flag = REXCVAR_GET(wwe2k14_dlc_dir);
  std::error_code ec;
  g_dir = dir_flag.empty() ? DefaultDir(game_data_root, gfxs::ExeFolder())
                           : fs::path(std::u8string(dir_flag.begin(), dir_flag.end()));  // flags are UTF-8
  g_dir = fs::absolute(g_dir, ec);
  if (!fs::is_directory(g_dir, ec)) {
    REXLOG_INFO("[wwe2k14][dlc] no DLC folder at {}: the game sees no packs", PathUtf8(g_dir));
    return;
  }
  try {
    g_packs = Scan(g_dir);
  } catch (const std::exception& e) {  // review: a folder name the path conversion cannot take, and the like
    REXLOG_WARN("[wwe2k14][dlc] could not read the DLC folder {}: {}; the game sees no packs", PathUtf8(g_dir), e.what());
    g_packs.clear();
    return;
  }
  const std::vector<int> off = ParseIdList(REXCVAR_GET(wwe2k14_dlc_off));
  std::lock_guard lock(g_mutex);
  for (const Pack& p : g_packs) {
    const bool switched_off = std::find(off.begin(), off.end(), p.pack_id) != off.end();
    std::string what;
    if (p.status != Status::kReady) {
      what = std::string(StatusName(p.status)) + " (" + p.reason + "): skipped";
    } else if (switched_off) {
      what = "switched off";
    } else {
      Offered o;
      o.pack = &p;
      o.file_name = "WWE2K14DLC" + std::to_string(p.pack_id);
      // The trailing '\' matters (review, Session 32): the VFS picks the first device whose mount path is a plain
      // prefix of the path, so without it pack 1's device would also answer pack 10's paths (ReXGlue's own content
      // devices end in '\' too).
      o.device_path = "\\Device\\WWE2K14DLC" + std::to_string(p.pack_id) + "\\";
      // A folder: a read-only host path device. A package: ReXGlue's STFS device reads it in place (Scan has already
      // walked it the same way, bounds-checked: dlc_catalog.h StfsPackage); its files are read-only.
      std::unique_ptr<rex::filesystem::Device> device;
      if (p.package) device = std::make_unique<rex::filesystem::StfsContainerDevice>(o.device_path, p.root);
      else device = std::make_unique<rex::filesystem::HostPathDevice>(o.device_path, p.root, /*read_only=*/true);
      if (!device->Initialize() || !file_system->RegisterDevice(std::move(device))) {
        what = "could not be mounted: skipped";
      } else {
        what = "offered to the game";
        g_offered.push_back(std::move(o));
      }
    }
    REXLOG_INFO("[wwe2k14][dlc] pack {} '{}' ({}): {}, {} file(s) listed, {} -> {}", p.pack_id, p.folder,
                p.DisplayName(), StatusName(p.status), p.files_listed, Mb(p.bytes), what);
  }
  g_on = !g_offered.empty();
  for (const Offered& o : g_offered)  // for replays: a recording keeps them (src/replay.h)
    replay::g_dlc_offered += (replay::g_dlc_offered.empty() ? "" : ",") + std::to_string(o.pack->pack_id);
  REXLOG_INFO("[wwe2k14][dlc] DLC folder {}: {} pack(s) found, {} offered to the game", PathUtf8(g_dir),
              g_packs.size(), g_offered.size());
}

// The offered pack this content item names, or -1.
inline int FindOffered(const XCONTENT_DATA& d) {
  if (TypeOf(d) != kMarketplaceContent) return -1;
  const std::string name = d.file_name();
  for (size_t i = 0; i < g_offered.size(); ++i)
    if (g_offered[i].file_name == name) return int(i);
  return -1;
}

inline uint32_t CreateEnumeratorOverride(uint32_t user_index, uint32_t device_id, uint32_t content_type,
                                         uint32_t content_flags, uint32_t items_per_enumerate,
                                         mapped_u32 buffer_size_ptr, mapped_u32 handle_out) {
  const uint32_t r = rex::kernel::xam::XamContentCreateEnumerator_entry(
      user_index, device_id, content_type, content_flags, items_per_enumerate, buffer_size_ptr, handle_out);
  size_t sdk_items = 0, ours = 0;
  if (r == 0 && handle_out) {
    auto e = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XStaticEnumerator<XCONTENT_DATA>>(*handle_out);
    if (e) {
      sdk_items = e->item_count();
      if (g_on && content_type == kMarketplaceContent && (device_id == 0 || device_id == kHddDeviceId)) {
        std::lock_guard lock(g_mutex);
        for (const Offered& o : g_offered) {
          XCONTENT_DATA* item = e->AppendItem();
          std::memset(item, 0, sizeof(*item));
          item->device_id = kHddDeviceId;
          item->content_type = rex::system::XContentType(kMarketplaceContent);
          const std::string name = o.pack->DisplayName();
          std::u16string wide;
          for (unsigned char c : name) wide += char16_t(c < 0x80 ? c : '?');  // ASCII only: the game logs it at most
          item->set_display_name(wide);
          item->set_file_name(o.file_name);
          ++ours;
        }
      }
    }
  }
  if (REXCVAR_GET(wwe2k14_log_content_calls))
    REXLOG_INFO("[wwe2k14][content] enumerate user={} device={} type={} flags=0x{:X} items={} -> 0x{:X}: {} from "
                "ReXGlue, {} DLC pack(s) frame={}",
                user_index, device_id, content_type, content_flags, items_per_enumerate, r, sdk_items, ours,
                g_guest_frame.load());
  return r;
}

inline uint32_t CreateExOverride(uint32_t user_index, mapped_string root_name, mapped_void content_data_ptr,
                                 uint32_t flags, mapped_u32 disposition_ptr, mapped_u32 license_mask_ptr,
                                 uint32_t cache_size, uint64_t content_size, mapped_void overlapped_ptr) {
  const std::string root(root_name.value());
  const auto* data = content_data_ptr ? content_data_ptr.as<const XCONTENT_DATA*>() : nullptr;
  int idx = -1;
  if (g_on && data) {
    std::lock_guard lock(g_mutex);
    idx = FindOffered(*data);
  }
  if (idx < 0) {
    const uint32_t r = rex::kernel::xam::XamContentCreateEx_entry(user_index, root_name, content_data_ptr, flags,
                                                                  disposition_ptr, license_mask_ptr, cache_size,
                                                                  content_size, overlapped_ptr);
    if (REXCVAR_GET(wwe2k14_log_content_calls))
      REXLOG_INFO("[wwe2k14][content] open '{}' user={} type={} file='{}' flags=0x{:X} -> 0x{:X}{} frame={}", root,
                  user_index, data ? TypeOf(*data) : 0, data ? data->file_name() : "", flags, r,
                  overlapped_ptr ? " (overlapped)" : "", g_guest_frame.load());
    return r;
  }
  // One of our packs: read-only, so only the open modes (3 OPEN_EXISTING, 4 OPEN_ALWAYS) succeed.
  uint32_t result = X_ERROR_SUCCESS, disposition = 0, licence = 0;
  const uint32_t mode = flags & 0xF;
  {
    std::lock_guard lock(g_mutex);
    const Offered& o = g_offered[size_t(idx)];
    const std::string key = Lower(root);
    if (mode != 3 && mode != 4) {
      result = X_ERROR_ACCESS_DENIED;
    } else if (g_roots.count(key)) {
      result = X_ERROR_ALREADY_EXISTS;  // as ReXGlue answers a root name that is still open
    } else if (!REX_KERNEL_FS()->RegisterSymbolicLink(root + ":", o.device_path)) {
      result = X_ERROR_ACCESS_DENIED;
    } else {
      g_roots[key] = size_t(idx);
      disposition = 2;  // opened
      licence = 0xFFFFFFFFu;
      ++g_opens;
    }
    REXLOG_INFO("[wwe2k14][dlc] open '{}:' = pack {} '{}' flags=0x{:X} -> 0x{:X} licence 0x{:08X} frame={}", root,
                o.pack->pack_id, o.pack->folder, flags, result, licence, g_guest_frame.load());
  }
  if (license_mask_ptr && result == X_ERROR_SUCCESS) *license_mask_ptr = licence;
  if (disposition_ptr) *disposition_ptr = disposition;
  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediateEx(overlapped_ptr.guest_address(), result,
                                                      X_HRESULT_FROM_WIN32(result), disposition);
    return X_ERROR_IO_PENDING;
  }
  return result;
}

inline uint32_t CloseOverride(mapped_string root_name, mapped_void overlapped_ptr) {
  const std::string root(root_name.value());
  bool ours = false;
  if (g_on) {
    std::lock_guard lock(g_mutex);
    auto it = g_roots.find(Lower(root));
    if (it != g_roots.end()) {
      ours = true;
      REX_KERNEL_FS()->UnregisterSymbolicLink(root + ":");
      REXLOG_INFO("[wwe2k14][dlc] close '{}:' (pack {}) frame={}", root, g_offered[it->second].pack->pack_id,
                  g_guest_frame.load());
      g_roots.erase(it);
    }
  }
  if (!ours) {
    const uint32_t r = rex::kernel::xam::XamContentClose_entry(root_name, overlapped_ptr);
    if (REXCVAR_GET(wwe2k14_log_content_calls))
      REXLOG_INFO("[wwe2k14][content] close '{}' -> 0x{:X} frame={}", root, r, g_guest_frame.load());
    return r;
  }
  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(), X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

}  // namespace wwe2k14::dlc

REX_HOOK(__imp__XamContentCreateEnumerator, wwe2k14::dlc::CreateEnumeratorOverride)
REX_HOOK(__imp__XamContentCreateEx, wwe2k14::dlc::CreateExOverride)
REX_HOOK(__imp__XamContentClose, wwe2k14::dlc::CloseOverride)
