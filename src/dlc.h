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
// Session 98 test switches (Superstar Importer run 2, default off): --wwe2k14_dlc_extra_dir (a second folder of packs,
// e.g. a test pack under out\) and --wwe2k14_dlc_gate_extra (open the game's pack-id gate to one more id).
// Session 98, the real feature: --wwe2k14_mods_superstars (src\superstars.h) joins <mods>\superstars\pack6 the same way and
// opens the gate for its id (only with this provider on, and only when the packs it was built on are offered).
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
#include <rex/system/xmemory.h>  // BaseHeap::Protect (--wwe2k14_dlc_gate_extra)
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
#include "superstars.h"  // Session 98: the Superstar Importer's pack (<mods>\superstars\pack6), one more pack when switched on

REXCVAR_DEFINE_BOOL(wwe2k14_dlc, false, "wwe2k14",
                    "Session 32: give the game the DLC packs in the DLC folder (one folder per pack with "
                    "info\\catalog.dlc), read-only and licensed. Off = ReXGlue's own content handling only");
REXCVAR_DEFINE_STRING(wwe2k14_dlc_dir, "", "wwe2k14",
                      "Session 32: the DLC folder (empty = 'dlc' next to the game data folder or disc image, e.g. "
                      "game_data\\dlc, else 'dlc' next to the exe)");
REXCVAR_DEFINE_STRING(wwe2k14_dlc_off, "", "wwe2k14",
                      "Session 32: pack ids switched off, comma separated (e.g. 3,5); the launcher's DLC switches");
REXCVAR_DEFINE_BOOL(wwe2k14_dlc_reverse_order, false, "wwe2k14",
                    "Session 98, TEST ONLY (Superstar Importer run 1): offer the DLC packs to the game in reverse "
                    "pack-id order (highest first) instead of ascending; logs '[wwe2k14][dlc] reverse order'");
REXCVAR_DEFINE_STRING(wwe2k14_dlc_extra_dir, "", "wwe2k14",
                      "Session 98, TEST ONLY (Superstar Importer run 2): a second folder scanned for packs (e.g. a test "
                      "pack built by tools\\importer\\build_mod_pack.py under out\\), offered with the DLC folder's "
                      "packs. Empty = none");
REXCVAR_DEFINE_INT32(wwe2k14_dlc_gate_extra, 0, "wwe2k14",
                     "Session 98, TEST ONLY (Superstar Importer run 2): let the game accept DLC pack id N (6..65535) by "
                     "writing (N << 16) | 1 over word 0 (pack id 1, which no real pack uses) of the game's pack-id gate "
                     "table at 0x82035E9C before the first catalog parse. 0 = the game's own table");
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
constexpr const char* kSuperstarsFolder = "[superstars] ";  // the Superstar Importer's pack in g_packs (folder name prefix)
inline int g_gate_id = 0;  // Session 98: the pack id the gate is opened for (the superstars pack's), 0 = none

inline void Install(rex::filesystem::VirtualFileSystem* file_system, const fs::path& game_data_root) {
  try {
    superstars::Load();  // (no-op unless --wwe2k14_mods_superstars)
  } catch (const std::exception& e) {  // (review: never stop the boot over our own file)
    superstars::g_found = false;
    REXLOG_WARN("[wwe2k14][superstars] could not read the superstars files: {}; the pack is left out", e.what());
  }
  // Session 98 (Jarrod: the importer must work without any DLC): the superstars pack does not need USE MY DLC or a DLC folder. A
  // pack built on real packs is still left out below when they are not offered.
  // Session 101 (review N4): until the pack is taken in (PatchGate), the verdict says the game never got that far
  if (superstars::g_found) superstars::WriteStatus("out the game stopped before it checked your DLC (send a report if this stays)");
  const bool real_dlc = REXCVAR_GET(wwe2k14_dlc);
  if (!real_dlc && !superstars::g_found) return;
  const std::string dir_flag = REXCVAR_GET(wwe2k14_dlc_dir);
  std::error_code ec;
  g_dir = dir_flag.empty() ? DefaultDir(game_data_root, gfxs::ExeFolder())
                           : fs::path(std::u8string(dir_flag.begin(), dir_flag.end()));  // flags are UTF-8
  g_dir = fs::absolute(g_dir, ec);
  const std::string extra_flag = REXCVAR_GET(wwe2k14_dlc_extra_dir);
  const bool have_dir = fs::is_directory(g_dir, ec);
  if (!have_dir && extra_flag.empty() && !superstars::g_found) {
    REXLOG_INFO("[wwe2k14][dlc] no DLC folder at {}: the game sees no packs", PathUtf8(g_dir));
    return;
  }
  try {
    if (real_dlc && have_dir) g_packs = Scan(g_dir);
    // Session 101 (lane 199): an id the game's own gate refuses (0x82035E9C: ids 1..5 only) never reaches it - logged and dropped
    // here, so a copy of the Superstar Importer's pack (id 6) put in the DLC folder cannot shadow the real one as a "duplicate".
    std::erase_if(g_packs, [](const Pack& p) {
      if (p.pack_id < 0 || GateAccepts(p.pack_id, p.revision) || (p.status != Status::kReady && p.status != Status::kDuplicate))
        return false;
      REXLOG_INFO("[wwe2k14][dlc] pack {} '{}' (revision {}): the game accepts DLC pack ids 1-5 only: skipped (a Superstar Importer "
                  "pack belongs in the mods folder, not the DLC folder)", p.pack_id, p.folder, p.revision);
      return true;
    });
    // Test only (S98 importer run 2): a second folder's packs join the list (sorted by pack id again; a pack id seen
    // in both folders is a duplicate and skipped, as within one folder). Its packs are named "[extra] <folder>".
    if (!extra_flag.empty()) {
      const fs::path extra = fs::absolute(fs::path(std::u8string(extra_flag.begin(), extra_flag.end())), ec);
      std::vector<Pack> more = Scan(extra);
      REXLOG_INFO("[wwe2k14][dlc] extra folder {} (--wwe2k14_dlc_extra_dir, test only): {} pack(s)", PathUtf8(extra),
                  more.size());
      for (Pack& p : more) {
        p.folder = "[extra] " + p.folder;
        g_packs.push_back(std::move(p));
      }
      if (g_packs.size() > kMaxPacks) g_packs.resize(kMaxPacks);
      SortAndMarkDuplicates(g_packs);
    }
    // Session 98: the Superstar Importer's pack joins the same way (its folder named "[superstars] ."); checked below.
    if (superstars::g_found) {
      std::vector<Pack> ours = Scan(superstars::g_pack);
      for (Pack& p : ours) {
        p.folder = kSuperstarsFolder + p.folder;
        g_packs.push_back(std::move(p));
      }
      if (g_packs.size() > kMaxPacks) g_packs.resize(kMaxPacks);
      SortAndMarkDuplicates(g_packs);
    }
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
    // Session 98: the superstars pack: its id must be the one superstars.txt names, and every pack it was built on must be
    // offered (they have lower ids, so they are decided before it in this pack-id order).
    const bool ours = p.folder.rfind(kSuperstarsFolder, 0) == 0;
    // Session 101 (lane 199): whether the superstars pack fits the real packs the game gets - the ONE rule the importer and the
    // launchers use too (src\importer\pack_fit.h; they rebuild before PLAY). The real packs have lower ids, so they are decided
    // before ours in this pack-id order.
    std::string misfit;
    if (ours && p.status == Status::kReady) {
      std::vector<const Pack*> real;
      for (const Offered& o : g_offered)
        if (o.pack->folder.rfind(kSuperstarsFolder, 0) != 0) real.push_back(o.pack);
      misfit = superstars_fit::Problem(superstars::g_built, real);
    }
    if (p.status != Status::kReady) {
      what = std::string(StatusName(p.status)) + " (" + p.reason + "): skipped";
      if (ours) superstars::WriteStatus("out the pack is " + std::string(StatusName(p.status)) + " (" + p.reason + "): build it again");  // (Session 101)
    } else if (ours && p.pack_id != superstars::g_pack_id) {
      what = "pack id is not superstars.txt's " + std::to_string(superstars::g_pack_id) + ": skipped (rebuild it in the importer)";
      superstars::WriteStatus("out the pack does not match its superstars.txt (build it again in the importer)");
    } else if (ours && p.pack_id < 6) {
      what = "a superstars pack needs an id above the real packs (6..): skipped";
      superstars::WriteStatus("out the pack has a wrong id (build it again in the importer)");
    } else if (ours && !misfit.empty()) {
      what = misfit + ": skipped (rebuild it: the launcher does at PLAY, or the importer's BUILD INTO GAME)";
      superstars::WriteStatus("out " + misfit);
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
        if (ours) superstars::WriteStatus("out the pack could not be opened: send a report");  // (Session 101)
      } else {
        what = "offered to the game";
        g_offered.push_back(std::move(o));
        if (ours) {
          superstars::g_active = true;
          g_gate_id = p.pack_id;
          what += " (imported superstars)";
          // ("in N" only once PatchGate has opened the gate for it: review N4)
        }
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

// Test only (S98 importer run 2, --wwe2k14_dlc_gate_extra=N): the catalog parser sub_8281F150 accepts a pack only if
// its id matches a word of the gate table at 0x82035E9C ((pack id << 16) | minimum revision; 5 words, ids 1-5: listing
// 0x8281F32C-0x8281F36C, `lis r11,-32253` + `addi r11,r11,24220`, the loop `cmplwi r9,20`). The generated code loads
// the table from guest memory on every parse (REX_LOAD_U32 at loc_8281F340), so a word changed before the first parse
// is what the game checks. The page is read-only (run 072: a same-value write faulted): its protection is changed
// through ReXGlue's heap (BaseHeap::Protect also sets the host page), the word written, the protection put back.
// Word 0 (pack id 1, minimum revision 1) belongs to no real pack (packs 2-5, docs\systems\dlc.md). Called from the
// enumeration hook: the game enumerates (frame ~1100) before it opens and parses any catalog.
constexpr uint32_t kGateTable = 0x82035E9C;
constexpr uint32_t kGateWord0 = 0x00010001;

inline void PatchGate() {
  static std::atomic<bool> done{false};
  // Session 98: the test switch, else the superstars pack's id when it was offered (src\superstars.h)
  const int32_t test_id = REXCVAR_GET(wwe2k14_dlc_gate_extra);
  const int32_t id = test_id ? test_id : g_gate_id;
  const char* why = test_id ? "test only, --wwe2k14_dlc_gate_extra" : "imported superstars, --wwe2k14_mods_superstars";
  if (id == 0 || done.exchange(true)) return;
  // (review) the superstars pack only counts as in the game when the gate really opens for it: else its music remap stays off
  const bool stars_gate = g_gate_id && (!test_id || test_id == g_gate_id);
  superstars::g_active = false;  // (true again below, once the patch succeeds for the superstars pack's id)
  auto refused = [&] {
    if (!g_gate_id) return;
    REXLOG_WARN("[wwe2k14][superstars] the pack-id gate stays closed for pack {}: the game refuses it", g_gate_id);
    superstars::WriteStatus("out the game refused the pack (its pack-id gate could not be opened: send a report)");  // (Session 101)
  };
  if (g_gate_id && !stars_gate)
    REXLOG_WARN("[wwe2k14][superstars] --wwe2k14_dlc_gate_extra={} wins over the superstars pack {}: it is refused", test_id, g_gate_id);
  auto* memory = REX_KERNEL_MEMORY();
  uint8_t* host = memory->TranslateVirtual<uint8_t*>(kGateTable);
  auto word = [&](int i) {
    const uint8_t* p = host + 4 * i;
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  };
  if (id < 6 || id > 0xFFFF) {
    REXLOG_WARN("[wwe2k14][dlc] --wwe2k14_dlc_gate_extra={}: pack ids 6..65535 only; gate table left alone", id);
    refused();
    return;
  }
  if (word(0) != kGateWord0) {
    REXLOG_WARN("[wwe2k14][dlc] gate table {:08X} word 0 is {:08X}, not {:08X}: not this game image? left alone",
                kGateTable, word(0), kGateWord0);
    refused();
    return;
  }
  auto* heap = memory->LookupHeap(kGateTable);
  uint32_t old = 0;
  if (!heap || !heap->Protect(kGateTable, 4, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, &old)) {
    REXLOG_WARN("[wwe2k14][dlc] gate table {:08X}: could not make its page writable; pack {} stays refused", kGateTable,
                id);
    refused();
    return;
  }
  const uint32_t v = (uint32_t(id) << 16) | 1u;
  host[0] = uint8_t(v >> 24), host[1] = uint8_t(v >> 16), host[2] = uint8_t(v >> 8), host[3] = uint8_t(v);
  const bool restored = heap->Protect(kGateTable, 4, old);
  if (stars_gate) {
    superstars::g_active = true;  // the superstars pack is in the game (its music remap may run)
    superstars::WriteStatus("in " + std::to_string(superstars::g_music.size()));  // (Session 101, review N4: only now)
  }
  REXLOG_INFO("[wwe2k14][dlc] gate table {:08X} ({}): {:08X} {:08X} {:08X} {:08X} "
              "{:08X}; word 0 {:08X} -> {:08X} (pack {} accepted from revision 1), protection 0x{:X} {} frame={}",
              kGateTable, why, word(0), word(1), word(2), word(3), word(4), kGateWord0, v, id, old,
              restored ? "put back" : "NOT put back", g_guest_frame.load());
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
  if (g_on && content_type == kMarketplaceContent) PatchGate();  // test only (S98): before any catalog is parsed
  const uint32_t r = rex::kernel::xam::XamContentCreateEnumerator_entry(
      user_index, device_id, content_type, content_flags, items_per_enumerate, buffer_size_ptr, handle_out);
  size_t sdk_items = 0, ours = 0;
  if (r == 0 && handle_out) {
    auto e = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XStaticEnumerator<XCONTENT_DATA>>(*handle_out);
    if (e) {
      sdk_items = e->item_count();
      if (g_on && content_type == kMarketplaceContent && (device_id == 0 || device_id == kHddDeviceId)) {
        std::lock_guard lock(g_mutex);
        // Test only (S98 importer run 1): offer the packs highest pack id first, to tell "newest pack = last
        // enumerated" from "= highest pack id" by which cumulative *_dlcPP files the game opens.
        const bool reverse = REXCVAR_GET(wwe2k14_dlc_reverse_order);
        for (size_t k = 0; k < g_offered.size(); ++k) {
          const Offered& o = g_offered[reverse ? g_offered.size() - 1 - k : k];
          if (reverse && REXCVAR_GET(wwe2k14_log_content_calls))
            REXLOG_INFO("[wwe2k14][dlc] reverse order: item {} = pack {}", sdk_items + k, o.pack->pack_id);
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
