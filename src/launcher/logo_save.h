// wwe2k14 launcher - Create-a-Logo import (Session 25 item 1). No UI code here.
//
// The player picks PNGs; the launcher backs up the save, then starts the game with an input script that goes to
// CREATIONS -> LOGO, opens each free slot at 256x256 (or 128x128, Session 53), lets the logo importer (src\logo_import.h) put the picture on
// the canvas and presses ACCEPT: the game writes its own save (no hand-edited bytes, no checksum risk). Afterwards the
// launcher reads the save again and checks each slot. Format and route: docs\systems\save_logos.md.
//
// The save is only read here, except by RestoreBackup (the player's explicit choice). Backups live in
// %LOCALAPPDATA%\wwe2k14-launcher\backups\YYYYMMDD_HHMMSS\ (the last 10 are kept).

#pragma once

#include <array>
#include <chrono>
#include <cstring>
#include <ctime>

#include "launcher_core.h"
// Needs stb_image.h (stbi_zlib_decode_buffer) included before this header (launcher_main.cpp does, once, with the
// implementation; including it again here would compile the implementation twice).

namespace launcher {

inline constexpr int kLogoSlots = 20;
inline constexpr size_t kLogoSlotBytes = 0x50468;
inline constexpr size_t kLogoHeaderBytes = 8;
inline constexpr size_t kLogoBlobBytes = kLogoHeaderBytes + kLogoSlots * kLogoSlotBytes + 4;
inline constexpr const char* kTitleId = "545408B2";

struct LogoSlot {
  bool used = false;
  int w = 0, h = 0;
  std::string date;           // "2026-10-17 01:15" (UTC, from the save)
  std::vector<uint8_t> rgba;  // w x h RGBA (used slots only)
};

struct LogoSave {
  fs::path user_root;   // the game's user data folder
  fs::path title_dir;   // <user root>\<profile>\545408B2 (empty: no save yet)
  fs::path file;        // ...\00000001\00PaintTool.pt\SaveData.Dat (may not exist: no logo saved yet)
  bool save_exists = false;   // the main save (SaveData.dat) is there: the game has been played and saved once
  bool logos_file = false;    // 00PaintTool.pt exists
  std::string error;          // the logo file exists but can't be read
  std::array<LogoSlot, kLogoSlots> slots;
  int Free() const {
    int n = 0;
    for (const auto& s : slots) n += !s.used;
    return n;
  }
};

// <user root>\<profile>\545408B2 holding a main save (the first profile folder that has one).
inline fs::path FindTitleDir(const fs::path& user_root) {
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(user_root, ec)) {
    if (!e.is_directory()) continue;
    const fs::path t = e.path() / kTitleId;
    if (fs::is_regular_file(t / "00000001" / "SaveData.dat" / "SaveData.Dat", ec)) return t;
  }
  return {};
}

// The inflated blob of 00PaintTool.pt (4 "ZLIB" blocks), or an error.
inline bool ReadLogoBlob(const fs::path& file, std::vector<uint8_t>& blob, std::string& err) {
  std::ifstream f(file, std::ios::binary);
  std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (d.empty()) return err = "can't read the logo save", false;
  blob.clear();
  size_t off = 0;
  while (off + 16 <= d.size() && std::memcmp(&d[off], "ZLIB", 4) == 0) {
    const uint32_t csize = Le32(&d[off + 8]), usize = Le32(&d[off + 12]);
    if (off + 16 + csize > d.size() || usize > (64u << 20)) return err = "logo save: a block runs past the end", false;
    const size_t at = blob.size();
    blob.resize(at + usize);
    const int n = stbi_zlib_decode_buffer(reinterpret_cast<char*>(blob.data() + at), int(usize),
                                          reinterpret_cast<const char*>(&d[off + 16]), int(csize));
    if (n != int(usize)) return err = "logo save: a block does not decompress", false;
    off += 16 + csize;
  }
  if (blob.size() != kLogoBlobBytes)
    return err = "logo save: unexpected size " + std::to_string(blob.size()) + " (unknown version?)", false;
  return true;
}

inline LogoSave ReadLogoSave(const fs::path& user_root, bool pixels = true) {
  LogoSave s;
  s.user_root = user_root;
  s.title_dir = FindTitleDir(user_root);
  if (s.title_dir.empty()) return s;
  s.save_exists = true;
  s.file = s.title_dir / "00000001" / "00PaintTool.pt" / "SaveData.Dat";
  std::error_code ec;
  s.logos_file = fs::is_regular_file(s.file, ec);
  if (!s.logos_file) return s;  // no logo saved yet: all 20 free
  std::vector<uint8_t> b;
  if (!ReadLogoBlob(s.file, b, s.error)) return s;
  for (int i = 0; i < kLogoSlots; ++i) {
    const uint8_t* r = b.data() + kLogoHeaderBytes + size_t(i) * kLogoSlotBytes;
    LogoSlot& sl = s.slots[size_t(i)];
    sl.used = Be32(r) == 1;
    if (!sl.used) continue;
    sl.w = int(Be32(r + 0x40)), sl.h = int(Be32(r + 0x44));
    const uint8_t* dt = r + 0x5045C;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u", unsigned(dt[0] << 8 | dt[1]), dt[2], dt[3], dt[4], dt[5]);
    sl.date = buf;
    if (!pixels || sl.w <= 0 || sl.h <= 0 || sl.w > 256 || sl.h > 256) continue;
    sl.rgba.resize(size_t(sl.w) * sl.h * 4);
    const uint8_t* px = r + 0x48;  // ARGB, big-endian words
    for (size_t p = 0; p < size_t(sl.w) * sl.h; ++p)
      sl.rgba[p * 4] = px[p * 4 + 1], sl.rgba[p * 4 + 1] = px[p * 4 + 2], sl.rgba[p * 4 + 2] = px[p * 4 + 3],
                  sl.rgba[p * 4 + 3] = px[p * 4];
  }
  return s;
}

// The grid position of slot i (0-19): page 0/1, row 0/1, column 0-4. "Slot 1" in the UI = index 0.
inline int SlotRow(int i) { return (i % 10) / 5; }
inline int SlotX(int i) { return (i / 10) * 5 + i % 5; }  // the rows run across both pages: RIGHT from column 5 = page 2

// ---------------------------------------------------------------------------------------------------------------
// Backups: the whole <profile>\545408B2 folder (logos, wrestlers, arenas and their headers).

inline fs::path BackupRoot() { return LocalDataDir() / "backups"; }

struct Backup {
  fs::path dir;       // BackupRoot()\YYYYMMDD_HHMMSS
  std::string label;  // "2026-10-17 12:30:05  before logo import"
  std::string source; // the save folder it was copied from (note.txt line 2)
};

// The same folder, however it is written (slashes, case, "..").
inline bool SamePath(const fs::path& a, const fs::path& b) {
  std::error_code ec;
  return Lower(PathStr(fs::weakly_canonical(a, ec).make_preferred())) == Lower(PathStr(fs::weakly_canonical(b, ec).make_preferred()));
}

// The backups of the save folder `title_dir` (empty: all), newest first.
inline std::vector<Backup> ListBackups(const fs::path& title_dir = {}) {
  std::vector<Backup> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(BackupRoot(), ec)) {
    if (!e.is_directory() || !fs::exists(e.path() / kTitleId, ec)) continue;
    const std::string n = PathStr(e.path().filename());
    Backup b{e.path(), n.size() >= 15 ? n.substr(0, 4) + "-" + n.substr(4, 2) + "-" + n.substr(6, 2) + " " +
                                            n.substr(9, 2) + ":" + n.substr(11, 2) + ":" + n.substr(13, 2)
                                      : n};
    std::ifstream note(e.path() / "note.txt");
    std::string why;
    if (std::getline(note, why) && !why.empty()) b.label += "  " + why;
    std::getline(note, b.source);
    if (!title_dir.empty() && !SamePath(fs::u8path(b.source), title_dir)) continue;
    out.push_back(b);
  }
  std::sort(out.begin(), out.end(), [](const Backup& a, const Backup& b) { return a.dir > b.dir; });  // newest first
  return out;
}

inline std::string NowStamp(const char* fmt) {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &t);
  char b[32];
  std::strftime(b, sizeof(b), fmt, &tm);
  return b;
}

// Copies title_dir to a new dated backup and keeps the newest 10. Returns "" or the reason it failed.
inline std::string MakeBackup(const fs::path& title_dir, const std::string& why, fs::path* made = nullptr) {
  std::error_code ec;
  if (title_dir.empty() || !fs::is_directory(title_dir, ec)) return "no save to back up";
  fs::path dir = BackupRoot() / NowStamp("%Y%m%d_%H%M%S");
  for (int n = 2; fs::exists(dir, ec); ++n) dir = BackupRoot() / (NowStamp("%Y%m%d_%H%M%S") + "_" + std::to_string(n));
  fs::create_directories(dir, ec);
  fs::copy(title_dir, dir / kTitleId, fs::copy_options::recursive, ec);
  if (ec) {
    fs::remove_all(dir, ec);
    return "backup failed: " + ec.message();
  }
  std::ofstream(dir / "note.txt") << why << "\n" << PathStr(title_dir) << "\n";
  if (made) *made = dir;
  auto mine = ListBackups(title_dir);  // keep this save's newest 10
  for (size_t i = 10; i < mine.size(); ++i) fs::remove_all(mine[i].dir, ec);
  return "";
}

// True when every file under a equals the one at the same place under b (and both have the same files).
inline bool SameTree(const fs::path& a, const fs::path& b) {
  std::error_code ec;
  std::set<std::string> fa, fb;
  for (auto it = fs::recursive_directory_iterator(a, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
    if (it->is_regular_file()) fa.insert(PathStr(fs::relative(it->path(), a, ec)));
  for (auto it = fs::recursive_directory_iterator(b, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
    if (it->is_regular_file()) fb.insert(PathStr(fs::relative(it->path(), b, ec)));
  if (fa != fb) return false;
  for (const auto& r : fa) {
    std::ifstream x(a / fs::u8path(r), std::ios::binary), y(b / fs::u8path(r), std::ios::binary);
    std::string sx((std::istreambuf_iterator<char>(x)), {}), sy((std::istreambuf_iterator<char>(y)), {});
    if (sx != sy) return false;
  }
  return true;
}

// Puts a backup back: first a backup of the current save ("before restore"), then the backup's files replace
// title_dir's, then a byte-for-byte check. The game must be closed.
inline std::string RestoreBackup(const Backup& b, const fs::path& title_dir) {
  std::error_code ec;
  if (title_dir.empty()) return "no save folder to restore into";
  const fs::path src = b.dir / kTitleId;
  if (!fs::is_directory(src, ec)) return "the backup is missing its files";
  // Copy the chosen backup out first: the "before restore" backup below may push the oldest one out of the 10 kept.
  const fs::path tmp = title_dir.parent_path() / (std::string(kTitleId) + ".restoring");
  fs::remove_all(tmp, ec);
  fs::copy(src, tmp, fs::copy_options::recursive, ec);
  if (ec) {
    fs::remove_all(tmp, ec);
    return "not restored: copy failed (" + ec.message() + ")";
  }
  if (fs::is_directory(title_dir, ec)) {
    if (std::string e = MakeBackup(title_dir, "before restore"); !e.empty()) {
      fs::remove_all(tmp, ec);
      return "not restored (" + e + ")";
    }
  }
  const fs::path old = title_dir.parent_path() / (std::string(kTitleId) + ".old");
  fs::remove_all(old, ec);
  fs::rename(title_dir, old, ec);
  if (ec) {
    fs::remove_all(tmp, ec);
    return "not restored: the save folder is in use (is the game running?)";
  }
  fs::rename(tmp, title_dir, ec);
  if (ec) {
    fs::rename(old, title_dir, ec);
    return "not restored: rename failed";
  }
  fs::remove_all(old, ec);
  if (!fs::is_directory(src, ec)) return "";  // the restored backup was the oldest and got rotated out (copied first)
  return SameTree(src, title_dir) ? "" : "restored, but the files differ from the backup (check the save)";
}

// ---------------------------------------------------------------------------------------------------------------
// The import route (docs\systems\save_logos.md; tools\input_scripts\short\logo_import_check.txt is the same route as a
// fixed test). Free slots are filled in order of their grid column, so the cursor only moves RIGHT (the rows run
// across both pages) plus one DOWN to change row (the grid's rows wrap).

struct LogoJob {
  int slot = 0;      // 0-19
  fs::path png;
  int size = 256;    // Session 53: the canvas, 256 or 128 (the game's SELECT CANVAS SIZE choice)
};

inline std::vector<LogoJob> PlanLogoJobs(const LogoSave& save, const std::vector<fs::path>& pngs, std::string& err,
                                         int size = 256) {
  std::vector<int> free;
  // Session 27: no Custom Logo file yet = the game creates one with its default logo in slot 1 (run 1921).
  std::error_code ec;
  const bool fresh = !fs::exists(save.file, ec);
  for (int i = 0; i < kLogoSlots; ++i)
    if (!save.slots[size_t(i)].used && !(fresh && i == 0)) free.push_back(i);
  if (pngs.size() > free.size()) {
    err = "only " + std::to_string(free.size()) + " free logo slot(s)";
    return {};
  }
  std::vector<int> pick(free.begin(), free.begin() + std::ptrdiff_t(pngs.size()));
  std::sort(pick.begin(), pick.end(), [](int a, int b) {
    return SlotX(a) != SlotX(b) ? SlotX(a) < SlotX(b) : SlotRow(a) < SlotRow(b);
  });
  std::vector<LogoJob> jobs;
  for (size_t i = 0; i < pick.size(); ++i) jobs.push_back({pick[i], pngs[i], size == 128 ? 128 : 256});
  return jobs;
}

// Session 27 item 2 (docs\systems\menus.md): the route reads where it is from game memory. Every press that moves is
// confirmed by the cursor it should produce; every press that changes something (A, ACCEPT) is preceded by a check of
// the screen and cursor. Any mismatch = a script failure: the game quits WITHOUT saving (on_fail quit) and
// --wwe2k14_input_result names the stage (the last `log` line). The chains are written inline (a launched script has
// no states.toml beside it); the same states are named in tools\input_scripts\states.toml.
inline constexpr const char* kSeqId = "0x839400F8>0>48";             // 75 main menu, 78 Create-a-Logo
inline constexpr const char* kSmMenu = "0x8392945C>364";             // SmartMenu list: 1 main, 3 CREATIONS
inline constexpr const char* kSmTarget = "0x8392945C>368";           // highlighted item: 3 CREATIONS, 1522 LOGO
inline constexpr const char* kLogoScreen = "0x839400F8>256>76>1200";  // 1 grid, 0 size menu, 3 editor, 4 Tools
inline constexpr const char* kLogoGrid = "0x839400F8>256>76>1448";    // grid cursor 0-19
inline constexpr const char* kLogoSize = "0x839400F8>256>76>1428";    // 0 = 128x128, 1 = 256x256
inline constexpr const char* kLogoTool = "0x839400F8>256>76>1292";    // 0 pen, 3 eraser, 6 fill, 9 ACCEPT
inline constexpr const char* kLogoCanvas = "0x839400F8>256>76>1252";  // the editor's canvas size in pixels
inline constexpr const char* kLogoPhase = "0x839400F8>256>76>3204";   // 124 grid ready, 13 "create logo save?", 8 editor

inline int SlotAt(int x, int row) { return (x / 5) * 10 + row * 5 + x % 5; }

// legacy = Session 25's counted presses (no memory checks), kept as a fallback (Settings -> Advanced).
// logo_save_exists = false: a save without a Custom Logo file yet; the game first asks to create it (YES = an empty
// logo save with the game's default logo in slot 1; snapshots run 1921).
inline std::string LogoImportScript(const std::vector<LogoJob>& jobs, bool legacy = false, bool logo_save_exists = true) {
  std::ostringstream o;
  o << "# Written by the WWE 2K14 launcher (logo import, src\\launcher\\logo_save.h). Presses through the menus; the\n"
       "# real controller is ignored until it ends. Do not edit: it is rewritten for every import.\n"
       "on_fail quit\n"
       "log logo_import: start\n";
  if (legacy) {
    // Title -> main menu -> CREATIONS -> LOGO (Jarrod's recording, logo_probe.txt).
    o << "wait_file SaveData.Dat 3600\nwait_frames 170\npress START\nwait_frames 70\npress A\n"
         "wait_file MNUIHD.PAC 1800\nwait_frames 150\npress DOWN\npress DOWN\npress DOWN\npress A\nwait_frames 80\n"
         "press DOWN 60\nwait_frames 30\npress A\nwait_file CASTHUMB.PAC 900\nwait_frames 400\n"
         "log logo_import: grid\n";
    int cur = 0;  // the grid cursor starts on slot 1
    for (const auto& j : jobs) {
      if (SlotRow(j.slot) != SlotRow(cur)) o << "press DOWN\nwait_frames 30\n";
      for (int n = SlotX(cur); n < SlotX(j.slot); ++n) o << "press RIGHT\nwait_frames 20\n";
      o << "wait_frames 10\n"
           "log logo_import: slot " << j.slot + 1 << "\n"
           "press A\nwait_frames 60\n"                   // SELECT CANVAS SIZE (cursor on 128x128)
        << (j.size == 128 ? "" : "press DOWN\nwait_frames 20\n")  // 256x256
        << "press A\nwait_frames 90\n"                   // the editor
           "cvar wwe2k14_logo_import_file " << PathStr(j.png) << "\n"
           "cvar wwe2k14_logo_import true\nwait_frames 20\n"
           "press LT 10\nwait_frames 30\n"               // the Tools panel; the left stick moves in it
           "press L_DOWN 8 20\npress L_DOWN 8 20\npress L_DOWN 8 20\nwait_frames 10\n"  // pen, eraser, fill, ACCEPT
           "press A\n"
           "wait_file savedata.dat 1800\n"               // the game writes the logo save
           "log logo_import: saved slot " << j.slot + 1 << "\n"
           "cvar wwe2k14_logo_import false\nwait_frames 400\n";  // back on the grid, cursor on this slot
      cur = j.slot;
    }
    o << "log logo_import: done\nwait_frames 60\nquit\n";
    return o.str();
  }
  // Title -> main menu (the title screens' presses as recorded; they change nothing).
  o << "wait_file SaveData.Dat 3600\nwait_frames 170\npress START\nwait_frames 70\npress A\n"
       "wait_file MNUIHD.PAC 1800\n"
       "log logo_import: waiting for the main menu\n"
    << "wait_state " << kSeqId << " 75 1200\n"
    << "wait_state " << kSmMenu << " 1 600\n"
    << "wait_frames 60\n"
       "log logo_import: main menu -> CREATIONS\n"
    << "press_until_state DOWN " << kSmTarget << " 3 6 40\n"
    << "assert_state " << kSmMenu << " 1\n"
    << "assert_state " << kSmTarget << " 3\n"
    << "press A\n"
    << "wait_state " << kSmMenu << " 3 300\n"
    << "wait_frames 20\n"
       "log logo_import: CREATIONS -> LOGO\n"
    << "press_until_state DOWN " << kSmTarget << " 1522 12 40\n"
    << "assert_state " << kSmMenu << " 3\n"
    << "assert_state " << kSmTarget << " 1522\n"
    << "press A\n"
    << "wait_state " << kSeqId << " 78 900\n";
  if (!logo_save_exists) {
    // A save without a Custom Logo file: the game asks to create one (cursor on NO). Only on that prompt: LEFT = YES,
    // A; anything else (or NO) fails the grid check below and the game closes without saving.
    o << "log logo_import: create the Custom Logo save\n"
      << "wait_state " << kLogoPhase << " 13 900\n"
      << "wait_frames 120\n"
      << "press LEFT\nwait_frames 30\n"
      << "assert_state " << kLogoPhase << " 13\n"
      << "press A\n";
  }
  o << "wait_state " << kLogoPhase << " 124 1800\n"
    << "wait_state " << kLogoScreen << " 1 900\n"
    << "wait_file CASTHUMB.PAC 900 optional\n"
    << (logo_save_exists ? "wait_file SaveData.Dat 1800 optional\n" : "")  // the grid reads the logo save first
    << "wait_frames 280\n"
       "log logo_import: the logo grid\n"
    << "assert_state " << kLogoGrid << " 0\n";  // a fresh grid starts on slot 1
  int cur = 0;
  for (const auto& j : jobs) {
    o << "log logo_import: to slot " << j.slot + 1 << "\n";
    if (SlotRow(j.slot) != SlotRow(cur)) {
      const int next = SlotAt(SlotX(cur), SlotRow(j.slot));
      o << "press_until_state DOWN " << kLogoGrid << " " << next << " 2 40\n";
      cur = next;
    }
    for (int x = SlotX(cur) + 1; x <= SlotX(j.slot); ++x)
      o << "press_until_state RIGHT " << kLogoGrid << " " << SlotAt(x, SlotRow(j.slot)) << " 2 40\n";
    cur = j.slot;
    o << "assert_state " << kLogoScreen << " 1\n"
      << "assert_state " << kLogoGrid << " " << j.slot << "\n"
      << "log logo_import: slot " << j.slot + 1 << " -> SELECT CANVAS SIZE\n"
      << "press A\n"
      << "wait_state " << kLogoScreen << " 0 120\n"   // a used slot would not open the size menu: stop here
      << "wait_frames 20\n"
      << "assert_state " << kLogoSize << " 0\n";      // the menu starts on 128x128
    if (j.size != 128) o << "press_until_state DOWN " << kLogoSize << " 1 2 30\n";
    o << "assert_state " << kLogoSize << " " << (j.size == 128 ? 0 : 1) << "\n"
      << "assert_state " << kLogoScreen << " 0\n"
      << "log logo_import: slot " << j.slot + 1 << " -> " << j.size << "x" << j.size << " editor\n"
      << "press A\n"
      << "wait_state " << kLogoScreen << " 3 300\n"
      << "wait_state " << kLogoCanvas << " " << j.size << " 120\n"
      << "wait_frames 30\n"
      << "cvar wwe2k14_logo_import_file " << PathStr(j.png) << "\n"
      << "cvar wwe2k14_logo_import true\nwait_frames 20\n"
      << "log logo_import: slot " << j.slot + 1 << " -> Tools -> ACCEPT\n"
      << "press LT 10\n"
      << "wait_state " << kLogoScreen << " 4 120\n"
      << "assert_state " << kLogoTool << " 0\n"
      << "press_until_state L_DOWN " << kLogoTool << " 3 2 40 8\n"   // eraser
      << "press_until_state L_DOWN " << kLogoTool << " 6 2 40 8\n"   // fill
      << "press_until_state L_DOWN " << kLogoTool << " 9 2 40 8\n"   // ACCEPT
      << "wait_frames 10\n"
      << "assert_state " << kLogoScreen << " 4\n"
      << "assert_state " << kLogoTool << " 9\n"
      << "assert_state " << kLogoGrid << " " << j.slot << "\n"
      << "press A\n"
      << "wait_file savedata.dat 1800\n"                // the game writes the logo save
      << "log logo_import: saved slot " << j.slot + 1 << "\n"
      << "cvar wwe2k14_logo_import false\n"
      << "wait_state " << kLogoScreen << " 1 900\n"     // back on the grid, cursor on this slot
      << "wait_frames 200\n"
      << "assert_state " << kLogoGrid << " " << j.slot << "\n";
  }
  o << "log logo_import: done\nwait_frames 60\nquit\n";
  return o.str();
}
// After the game closed: did every job's slot become a used logo, at its size (the save's own size field)?
struct LogoResult {
  int ok = 0, failed = 0, low_res = 0;
  std::string detail;
};

inline LogoResult CheckLogoJobs(const std::vector<LogoJob>& jobs, const LogoSave& before, const LogoSave& after) {
  LogoResult r;
  for (const auto& j : jobs) {
    const auto& a = after.slots[size_t(j.slot)];
    const bool was = before.slots[size_t(j.slot)].used;
    std::string line = "slot " + std::to_string(j.slot + 1) + " (" + PathStr(j.png.filename()) + "): ";
    if (!was && a.used && a.w == j.size) ++r.ok, line += "saved " + std::to_string(j.size) + "x" + std::to_string(j.size);
    else if (!was && a.used) ++r.ok, ++r.low_res, line += "saved, but " + std::to_string(a.w) + "x" + std::to_string(a.h);
    else ++r.failed, line += "NOT saved";
    r.detail += line + "\n";
  }
  return r;
}

}  // namespace launcher
