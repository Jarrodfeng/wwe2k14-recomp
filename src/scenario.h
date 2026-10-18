// wwe2k14 - scenario launcher, step 1: observe how the menus set up a match (Session 9, SESSION9_PLAN.md Goal 1).
// --wwe2k14_scenario_observe=true logs every call to the match-setup functions (docs/systems/match_setup.md) with its
// arguments and guest caller, and snapshots the match description M, the settings struct G and the participant
// slots A at the key points. Default off: every hook returns after one setting read. Log only: no hook changes a
// register or guest memory. Hooks: config/scenario_hooks.toml.
//
// Hooked (each at its first instruction, so r3.. are the arguments and ctx.lr is the caller's return address; the
// ThreadState lookup is the same one flight_recorder.h uses, because [[midasm_hook]] cannot pass lr):
//   sub_82B6D900(obj, v)          M+4 = v (mode). Generic 2-instruction setter, 145 call sites: logged only when obj
//                                 is the current M (pointer at 0x836F2D88), other calls are counted.
//   sub_822CA9C0(M, type, opt)    match type (M+8) and its rule bytes
//   sub_822C9748(M, arena, flag)  arena (M+532), flag (M+537); also called from inside sub_822CA9C0
//   sub_822C89F8(M, i, id, team, a5, a6, a7, a8)  participant i
//   sub_829263B0(...)             exhibition finalize (calls the setters, then apply); snapshot M on entry
//   sub_822CA740(M)               apply M to G and A; snapshot M on entry, G and A at its exit (0x822CA77C)
//   sub_827DD0B0()                launch match; snapshot M, G and A on entry
//   sub_82833440()                main-menu dispatcher (switch on the word at 0x83929434); first calls and changes
//   sub_8225E790(obj, kind, fac)  switch sequence (kind 3 = in-game, 1 = menus; factory function address)
// Snapshots go to logs/scenario_NNN_KK_<point>_<M|G|A>.bin (NNN = run log number, KK = order; raw big-endian guest
// bytes, local only like everything in logs/, never committed). Compare with tools/scenarios/diff_bin.py.
// Sizes: M = 724-byte header + 43 entries of 3,580 bytes (154,664); G = 18,600 bytes from 0x8376AAF4 (last field
// known: G+18576); A = 43 slots of 56 bytes at P+148 (P = word at 0x8376AAF0).
//
// Step 2, capture (design A, Jarrod's decision after step 1): --wwe2k14_scenario_capture=NAME saves the match the
// menus set up, at apply: M exactly as finalize left it (apply entry), plus G and A as apply left them (apply exit,
// for checking a launch against later, not restored). Files in --wwe2k14_scenario_dir (default ../../scenarios
// from the exe folder = out/scenarios, gitignored; local only by Jarrod's decision: they are copies of game memory):
//   NAME.toml   readable summary (match type, mode, arena, participants, the extra ids) + file sizes and FNV-1a 64
//               hashes + the setter calls seen since finalize (for reading only)
//   NAME.m.bin  M, 154,664 raw big-endian bytes (what a launch writes back)
//   NAME.g.bin  G, 18,600 bytes; NAME.a.bin  A, 2,408 bytes
// Never overwrites: if NAME is taken, the next free NAME_2, NAME_3 ... is used (logged). Every apply on the current M
// in the run is captured, so several matches set up in one run give NAME, NAME_2, ... Each file is written to
// .tmp and renamed. Read back / list: py tools\scenarios\list.py. Capture changes no register or guest memory.

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>
#include <rex/hook.h>

#include <windows.h>  // __try / GetExceptionCode for the snapshot trigger's fault-safe copy

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <mutex>
#include <string>
#include <vector>

#include "crowd_hype.h"  // called from the owner update hook below
#include "frame_stats.h"

REXCVAR_DEFINE_BOOL(wwe2k14_scenario_observe, false, "wwe2k14",
                    "Log the match-setup calls (mode / match type / arena / participant setters, exhibition "
                    "finalize, apply, launch, main-menu dispatcher, sequence switch) with their arguments and caller, "
                    "and snapshot the match description, settings struct and participant slots to "
                    "logs/scenario_NNN_*.bin. Log only. See src/scenario.h");
REXCVAR_DEFINE_STRING(wwe2k14_scenario_capture, "", "wwe2k14",
                      "Capture the matches set up in the menus as scenario NAME (letters, digits, _ and -, up to 64): "
                      "NAME.toml + NAME.m.bin / .g.bin / .a.bin in --wwe2k14_scenario_dir, written when the menus "
                      "apply the match. Never overwrites (NAME_2, NAME_3 ...). Empty = off. See src/scenario.h");
REXCVAR_DEFINE_STRING(wwe2k14_scenario_dir, "../../scenarios", "wwe2k14",
                      "Folder for scenario files; relative paths are from the exe folder (default = out/scenarios)");

namespace wwe2k14::scenario {

constexpr uint32_t kMatchDescPtr = 0x836F2D88;  // word: M
constexpr uint32_t kSettings = 0x8376AAF4;      // G (static)
constexpr uint32_t kGameDataPtr = 0x8376AAF0;   // word: P
constexpr uint32_t kMenuSelection = 0x83929434;  // word read by the main-menu dispatcher
constexpr uint32_t kMatchDescSize = 724 + 43 * 3580;
constexpr uint32_t kSettingsSize = 18600;
constexpr uint32_t kSlotsOffset = 148, kSlotsSize = 43 * 56;
// P block (NAME.pblock.bin, optional; Session 13): P+0x2934..P+0x2D3F, filled by the menus' finalize before apply
// (per participant, sub_829263B0 0x82926E50 / 0x8292703C -> sub_822C4E10 -> sub_827F5698; write watch, run 984) and
// left zero by a launch (menu runs 978 / 979: identical for two different rosters). Restoring it did NOT bring back
// a launched Royal Rumble's entrants #1 / #2 entrances (run 985), so --wwe2k14_launch_restore_pblock is off by
// default. Its first word points into the block (P+0x2AA0) and is rebased on restore.
constexpr uint32_t kPBlockOffset = 0x2934, kPBlockSize = 0x2D40 - 0x2934, kPBlockSelfTarget = 0x2AA0;
// Extras (NAME.x.bin, 32 bytes; step 3): state the menus set outside M that a launch restores.
//   +0  16 bytes  P+10528..P+10543: the menus' arena record in the P+144 object (+10388 flag, +10392 arena; setter
//                 sub_827F5290 = obj+10392, called at the arena confirm by sub_82849A18, watch run 480; read by
//                 sub_827F5298 -> sub_82AAD550, which maps arena 0-55 to an arena-specific resource id)
//   +16  4 bytes  SM+796: the SmartMenu's active controller index, handed to the next sequence through
//                 sub_8283F308 -> 0x83626D80 by the page callback (menus 0, a launch without it -1; runs 474-476)
//   +20 12 bytes  reserved (0)
constexpr uint32_t kExtrasSize = 32;
// Session 14 (Rumble entrants #1 / #2): the per-participant array [0x836F2D98] (first word = entries, 3376 bytes each,
// entrance record at +456+572 = +1028, its first id halfword at +1048). Apply fills entries 0..8 from the in-ring
// slots (sub_822CA008 loop -> sub_822CE2C0; watch run 1034); the entrance object copies them for Rumble-type matches
// (sub_82318460, G[0] 20..23).
constexpr uint32_t kParticipantArrayPtr = 0x836F2D98, kParticipantEntrySize = 3376, kEntranceRecordId = 1048;
constexpr uint32_t kArenaRecordOffset = 10528, kArenaRecordSize = 16;  // P-relative
constexpr uint32_t kSmartMenuWord = 0x8392945C, kSmPadOffset = 796;
constexpr uint32_t kMenuStateSize = 12800;  // finalize's r3 object (see wwe2k14_scn_finalize)
constexpr uint32_t kImageDataStart = 0x835C0000, kImageDataEnd = 0x83A60000;  // static data after the code (q info)
constexpr uint32_t kGameDataSnapSize = 0x20000;  // first 128 KB of P (P+0x129A8 is in use, rng.md)
constexpr uint32_t kMaxLinesPerHook = 256;

inline bool On() { return REXCVAR_GET(wwe2k14_scenario_observe); }
inline uint8_t* Guest(uint32_t address) { return REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(address); }
inline uint32_t U32(uint32_t address) { return rex::memory::load_and_swap<uint32_t>(Guest(address)); }
inline uint8_t U8(uint32_t address) { return *Guest(address); }
inline uint64_t Frame() { return g_guest_frame.load(std::memory_order_relaxed); }
inline uint32_t CallerLr() {
  auto* ts = rex::runtime::ThreadState::Get();
  return ts && ts->context() ? uint32_t(ts->context()->lr) : 0;
}
inline uint32_t CurrentM() { return U32(kMatchDescPtr); }

inline std::atomic<uint32_t> g_snap_seq{0};
inline std::atomic<uint32_t> g_mode_other{0};

// Raw copy of guest memory to logs/scenario_NNN_KK_<tag>.bin; returns the file name (or an error note).
inline std::string Snapshot(const char* tag, uint32_t address, uint32_t size) {
  if (!address) return "(null address)";
  const uint32_t seq = ++g_snap_seq;
  const int run = CurrentLogNumber();
  char name[96];
  std::snprintf(name, sizeof(name), "scenario_%03d_%02u_%s.bin", run >= 0 ? run : 0, seq, tag);
  const auto path = std::filesystem::current_path() / "logs" / name;
  std::FILE* f = std::fopen(path.string().c_str(), "wb");
  if (!f) return std::string(name) + " (open failed)";
  const size_t written = std::fwrite(Guest(address), 1, size, f);
  std::fclose(f);
  if (written != size) return std::string(name) + " (short write)";
  return name;
}

// One line per hook call, capped per hook (the count keeps going; the cap is logged once).
inline bool Admit(std::atomic<uint32_t>& count, const char* what) {
  const uint32_t n = ++count;
  if (n == kMaxLinesPerHook + 1) {
    REXLOG_INFO("[wwe2k14][scenario] {}: {} calls logged, further calls not logged", what, kMaxLinesPerHook);
  }
  return n <= kMaxLinesPerHook;
}

// Key fields of M as the match_setup.md table names them (offsets confirmed static only).
inline void LogMatchDesc(const char* where, uint32_t m) {
  if (!m) {
    REXLOG_INFO("[wwe2k14][scenario] {}: M is null", where);
    return;
  }
  REXLOG_INFO("[wwe2k14][scenario] {}: M={:08X} mode(M+4)={} type(M+8)={} arena(M+532)={} M+536={} flag(M+537)={} "
              "M+446={} rule bytes M+12..27 {:08X} {:08X} {:08X} {:08X}",
              where, m, U32(m + 4), U32(m + 8), U32(m + 532), U8(m + 536), U8(m + 537), U8(m + 446), U32(m + 12),
              U32(m + 16), U32(m + 20), U32(m + 24));
  for (uint32_t i = 0; i < 43; ++i) {
    const uint32_t e = m + 724 + 3580 * i;
    const uint32_t id = U32(e + 56);
    if (id >= 500) continue;  // the setter's empty-slot value is 500; entries left empty are skipped
    REXLOG_INFO("[wwe2k14][scenario] {}:   entry {:2}: id(+56)={} slot +0 {:08X} +4 {:08X} +8 {:08X} +12 {:08X} "
                "+16 {:08X} +20 {:08X} +24 {:08X} +28 {:08X}",
                where, i, id, U32(e), U32(e + 4), U32(e + 8), U32(e + 12), U32(e + 16), U32(e + 20), U32(e + 24),
                U32(e + 28));
  }
}

inline void LogSettings(const char* where) {
  const uint32_t g = kSettings;
  REXLOG_INFO("[wwe2k14][scenario] {}: G[0]={} G+64(arena)={} G+68={:08X} G+72={} G+74={} G+75={} G+18572={} "
              "G+18576={} menu_sel(0x83929434)={}",
              where, U8(g), U32(g + 64), U32(g + 68), U8(g + 72), U8(g + 74), U8(g + 75), U32(g + 18572),
              U32(g + 18576), U32(kMenuSelection));
}

inline uint32_t SlotsAddress() {
  const uint32_t p = U32(kGameDataPtr);
  return p ? p + kSlotsOffset : 0;
}

inline void SnapAll(const char* point, bool m, bool g, bool a) {
  char tag[48];
  if (m) {
    std::snprintf(tag, sizeof(tag), "%s_M", point);
    REXLOG_INFO("[wwe2k14][scenario] snapshot {}: {}", tag, Snapshot(tag, CurrentM(), kMatchDescSize));
  }
  if (g) {
    std::snprintf(tag, sizeof(tag), "%s_G", point);
    REXLOG_INFO("[wwe2k14][scenario] snapshot {}: {}", tag, Snapshot(tag, kSettings, kSettingsSize));
  }
  if (a) {
    std::snprintf(tag, sizeof(tag), "%s_A", point);
    REXLOG_INFO("[wwe2k14][scenario] snapshot {}: {}", tag, Snapshot(tag, SlotsAddress(), kSlotsSize));
  }
}

inline std::atomic<uint32_t> g_n_mode{0}, g_n_type{0}, g_n_arena{0}, g_n_part{0}, g_n_fin{0}, g_n_apply{0},
    g_n_apply_exit{0}, g_n_launch{0}, g_n_disp{0}, g_n_switch{0};
inline std::atomic<uint32_t> g_last_menu_sel{0xFFFFFFFFu};

// ---------------------------------------------------------------------------------------------------------------
// Capture (step 2)
// ---------------------------------------------------------------------------------------------------------------
constexpr size_t kMaxCalls = 256;

struct SetupCall {
  const char* fn;
  int32_t args[7];
  int nargs;
  uint32_t lr;
  uint64_t frame;
};

// All of this is touched from the thread running the menus' finalize / apply (one thread in run 470); the mutex
// makes it safe if another path ever applies a match from another thread.
inline std::mutex g_cap_mutex;
inline std::vector<SetupCall> g_calls;  // setter calls on the current M since the last finalize entry / capture
inline uint32_t g_finalize_lr = 0;
inline std::vector<uint8_t> g_pending_m;  // M at apply entry, written out at apply exit
inline std::vector<uint8_t> g_pending_x;  // extras at apply entry (see kExtrasSize)
inline bool g_pending = false;
inline uint32_t g_pending_lr = 0;
inline uint64_t g_pending_frame = 0;
inline bool g_bad_name_logged = false;
inline std::atomic<bool> g_launcher_applying{false};  // the launcher's own apply call is never captured

inline std::string CaptureName() { return REXCVAR_GET(wwe2k14_scenario_capture); }
inline bool Capturing() { return !CaptureName().empty(); }
inline bool ValidName(const std::string& n) {
  if (n.empty() || n.size() > 64) return false;
  for (char c : n) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
      return false;
    }
  }
  return true;
}

inline void Record(const char* fn, std::initializer_list<int32_t> args) {
  std::lock_guard<std::mutex> lock(g_cap_mutex);
  if (g_calls.size() >= kMaxCalls) return;
  SetupCall c{fn, {}, 0, CallerLr(), Frame()};
  for (int32_t v : args) {
    if (c.nargs < 7) c.args[c.nargs++] = v;
  }
  g_calls.push_back(c);
}

inline std::vector<uint8_t> CopyGuest(uint32_t address, uint32_t size) {
  std::vector<uint8_t> out(size);
  if (address) std::memcpy(out.data(), Guest(address), size);
  return out;
}

// The extras block (layout at kExtrasSize), read from guest memory now.
inline std::vector<uint8_t> CopyExtras() {
  std::vector<uint8_t> x(kExtrasSize, 0);
  const uint32_t p = U32(kGameDataPtr);
  if (p) std::memcpy(x.data(), Guest(p + kArenaRecordOffset), kArenaRecordSize);
  const uint32_t sm = U32(kSmartMenuWord);
  const uint32_t pad = sm ? U32(sm + kSmPadOffset) : 0xFFFFFFFFu;
  x[16] = uint8_t(pad >> 24);
  x[17] = uint8_t(pad >> 16);
  x[18] = uint8_t(pad >> 8);
  x[19] = uint8_t(pad);
  return x;
}

inline uint64_t Fnv1a64(const std::vector<uint8_t>& data) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (uint8_t b : data) {
    h ^= b;
    h *= 0x100000001B3ull;
  }
  return h;
}

inline std::filesystem::path ScenarioDir() {
  std::filesystem::path dir(REXCVAR_GET(wwe2k14_scenario_dir));
  if (dir.is_relative()) dir = std::filesystem::current_path() / dir;
  return dir.lexically_normal();
}

// Write to NAME.tmp, then rename: a crash mid-write never leaves a half file under the real name.
inline bool WriteAtomic(const std::filesystem::path& path, const void* data, size_t size, std::string& error) {
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
  if (!f) {
    error = "cannot open " + tmp.string();
    return false;
  }
  const size_t written = size ? std::fwrite(data, 1, size, f) : 0;
  const bool closed = std::fclose(f) == 0;
  if (written != size || !closed) {
    error = "short write to " + tmp.string();
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    return false;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    error = "cannot rename " + tmp.string() + ": " + ec.message();
    return false;
  }
  return true;
}

// Reads a big-endian word out of a host copy of guest memory.
inline uint32_t BeU32(const std::vector<uint8_t>& d, size_t off) {
  return off + 4 <= d.size() ? (uint32_t(d[off]) << 24) | (uint32_t(d[off + 1]) << 16) | (uint32_t(d[off + 2]) << 8) |
                                   uint32_t(d[off + 3])
                             : 0;
}

inline std::string HashHex(uint64_t h) { return fmt::format("{:016X}", h); }

// Called at apply exit with M and the extras from apply entry; G and A are read now. Caller holds g_cap_mutex.
inline void WriteScenarioLocked(const std::vector<uint8_t>& m, const std::vector<uint8_t>& x) {
  const std::string base = CaptureName();
  if (!ValidName(base)) {
    if (!g_bad_name_logged) {
      g_bad_name_logged = true;
      REXLOG_WARN("[wwe2k14][scenario] capture: name '{}' not used: letters, digits, _ and - only, 1-64 characters",
                  base);
    }
    return;
  }
  const std::filesystem::path dir = ScenarioDir();
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::string name;
  for (int k = 1; k < 1000; ++k) {
    const std::string candidate = k == 1 ? base : fmt::format("{}_{}", base, k);
    if (!std::filesystem::exists(dir / (candidate + ".toml")) &&
        !std::filesystem::exists(dir / (candidate + ".m.bin"))) {
      name = candidate;
      break;
    }
  }
  if (name.empty()) {
    REXLOG_WARN("[wwe2k14][scenario] capture: no free name for '{}' in {}", base, dir.string());
    return;
  }
  const std::vector<uint8_t> g = CopyGuest(kSettings, kSettingsSize);
  const std::vector<uint8_t> a = CopyGuest(SlotsAddress(), kSlotsSize);

  // Readable summary, taken from the bytes being saved (offsets: docs/systems/match_setup.md).
  std::string t;
  t += "# WWE 2K14 recomp scenario, captured by --wwe2k14_scenario_capture (src/scenario.h).\n";
  t += "# LOCAL ONLY (Jarrod's decision, Session 9): the .bin files are copies of game memory; never commit them.\n";
  t += "# NAME.m.bin is the match description M as the menus left it at apply (raw big-endian bytes): a launch writes\n";
  t += "# it back, then calls apply and launch. NAME.x.bin (extras: the menus' arena record in P and their active\n";
  t += "# controller index, layout in src/scenario.h) is restored too. NAME.g.bin / NAME.a.bin (settings struct G and\n";
  t += "# participant slots A at apply exit) are for checking a launch, not restored. [summary] and [[call]] are for\n";
  t += "# reading only.\n";
  t += "format = 2\n";
  t += fmt::format("name = \"{}\"\n", name);
  t += fmt::format("captured_run = {}\n", CurrentLogNumber());
  t += fmt::format("captured_frame = {}\n", g_pending_frame);
  t += fmt::format("apply_caller = \"0x{:08X}\"\n", g_pending_lr);
  t += fmt::format("finalize_caller = \"0x{:08X}\"   # 0 = no finalize seen before this apply\n", g_finalize_lr);
  t += "\n[summary]\n";
  t += fmt::format("match_type = {}   # M+8\n", int32_t(BeU32(m, 8)));
  t += fmt::format("mode = {}   # M+4\n", int32_t(BeU32(m, 4)));
  t += fmt::format("arena = {}   # M+532 (BG id)\n", int32_t(BeU32(m, 532)));
  t += fmt::format("extra_ids = [{}, {}, {}]   # M+544/548/552, slots 40-42 (roles unknown; 500 = empty)\n",
                   int32_t(BeU32(m, 544)), int32_t(BeU32(m, 548)), int32_t(BeU32(m, 552)));
  t += fmt::format("menu_arena_record = [{}, {}]   # extras: P+10532 flag, P+10536 arena\n", int32_t(BeU32(x, 4)),
                   int32_t(BeU32(x, 8)));
  t += fmt::format("menu_pad = {}   # extras: SmartMenu+796, the active controller index (-1 = none)\n",
                   int32_t(BeU32(x, 16)));
  for (uint32_t i = 0; i < 43; ++i) {
    const size_t e = 724 + size_t(3580) * i;
    const uint32_t id = BeU32(m, e + 56);
    if (id >= 500) continue;
    t += "\n[[summary.participant]]\n";
    t += fmt::format("index = {}\n", i);
    t += fmt::format("id = {}   # entry+56\n", id);
    t += fmt::format("slot_id = {}   # entry+16 (id x 100 + n; meaning of n unconfirmed)\n", BeU32(m, e + 16));
  }
  t += "\n[files]\n";
  t += fmt::format("m = {{ file = \"{}.m.bin\", size = {}, fnv1a64 = \"{}\" }}\n", name, m.size(), HashHex(Fnv1a64(m)));
  t += fmt::format("g = {{ file = \"{}.g.bin\", size = {}, fnv1a64 = \"{}\" }}\n", name, g.size(), HashHex(Fnv1a64(g)));
  t += fmt::format("a = {{ file = \"{}.a.bin\", size = {}, fnv1a64 = \"{}\" }}\n", name, a.size(), HashHex(Fnv1a64(a)));
  t += fmt::format("x = {{ file = \"{}.x.bin\", size = {}, fnv1a64 = \"{}\" }}\n", name, x.size(), HashHex(Fnv1a64(x)));
  for (const SetupCall& c : g_calls) {
    t += "\n[[call]]\n";
    t += fmt::format("fn = \"{}\"\n", c.fn);
    std::string args;
    for (int k = 0; k < c.nargs; ++k) args += fmt::format("{}{}", k ? ", " : "", c.args[k]);
    t += fmt::format("args = [{}]\n", args);
    t += fmt::format("from = \"0x{:08X}\"\n", c.lr);
    t += fmt::format("frame = {}\n", c.frame);
  }

  // The P block finalize fills before apply (see kPBlockOffset): optional, restored by a launch when present.
  std::vector<uint8_t> pblock;
  if (const uint32_t pp = U32(kGameDataPtr); pp) pblock = CopyGuest(pp + kPBlockOffset, kPBlockSize);
  std::string error;
  // The .toml goes last: list.py treats a scenario as present when its .toml exists.
  const bool ok = WriteAtomic(dir / (name + ".m.bin"), m.data(), m.size(), error) &&
                  WriteAtomic(dir / (name + ".g.bin"), g.data(), g.size(), error) &&
                  WriteAtomic(dir / (name + ".a.bin"), a.data(), a.size(), error) &&
                  WriteAtomic(dir / (name + ".x.bin"), x.data(), x.size(), error) &&
                  (pblock.empty() || WriteAtomic(dir / (name + ".pblock.bin"), pblock.data(), pblock.size(), error)) &&
                  WriteAtomic(dir / (name + ".toml"), t.data(), t.size(), error);
  if (!ok) {
    REXLOG_WARN("[wwe2k14][scenario] capture '{}' FAILED: {}", name, error);
    return;
  }
  REXLOG_INFO("[wwe2k14][scenario] captured '{}'{}: type={} mode={} arena={} ids {} / {} ({} setter calls) -> {} "
              "(m fnv1a64 {}) frame={}",
              name, name == base ? "" : fmt::format(" ('{}' was taken)", base), int32_t(BeU32(m, 8)),
              int32_t(BeU32(m, 4)), int32_t(BeU32(m, 532)), BeU32(m, 724 + 56), BeU32(m, 724 + 3580 + 56),
              g_calls.size(), (dir / (name + ".toml")).string(), HashHex(Fnv1a64(m)), Frame());
}

}  // namespace wwe2k14::scenario

// Global, non-inline: the generated code declares these extern (config/scenario_hooks.toml).

// sub_82B6D900 entry: r3 = object, r4 = value stored at object+4.
void wwe2k14_scn_mode(PPCRegister& r3, PPCRegister& r4) {
  using namespace wwe2k14::scenario;
  if (!On() && !Capturing()) return;
  if (r3.u32 != CurrentM()) {
    ++g_mode_other;
    return;
  }
  if (Capturing()) Record("mode_set", {int32_t(r4.u32)});
  if (!On() || !Admit(g_n_mode, "mode setter")) return;
  REXLOG_INFO("[wwe2k14][scenario] call mode_set(M={:08X}, mode={}) from {:08X} frame={} (other objects so far: {})",
              r3.u32, int32_t(r4.u32), CallerLr(), Frame(), g_mode_other.load());
}

// sub_822CA9C0 entry: r3 = M, r4 = match type id, r5 = option.
void wwe2k14_scn_type(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {
  using namespace wwe2k14::scenario;
  if (!On() && !Capturing()) return;
  if (Capturing() && r3.u32 == CurrentM()) Record("type_set", {int32_t(r4.u32), int32_t(r5.u32)});
  if (!On() || !Admit(g_n_type, "type setter")) return;
  REXLOG_INFO("[wwe2k14][scenario] call type_set(M={:08X}{}, type={}, opt={}) from {:08X} frame={}", r3.u32,
              r3.u32 == CurrentM() ? "" : " NOT current M", int32_t(r4.u32), int32_t(r5.u32), CallerLr(), Frame());
}

// sub_822C9748 entry: r3 = M, r4 = arena id (300 = the type's default), r5 = flag (M+537).
void wwe2k14_scn_arena(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {
  using namespace wwe2k14::scenario;
  if (!On() && !Capturing()) return;
  if (Capturing() && r3.u32 == CurrentM()) Record("arena_set", {int32_t(r4.u32), int32_t(r5.u32)});
  if (!On() || !Admit(g_n_arena, "arena setter")) return;
  REXLOG_INFO("[wwe2k14][scenario] call arena_set(M={:08X}{}, arena={}, flag={}) from {:08X} frame={}", r3.u32,
              r3.u32 == CurrentM() ? "" : " NOT current M", int32_t(r4.u32), int32_t(r5.u32), CallerLr(), Frame());
}

// sub_822C89F8 entry: r3 = M, r4 = index, r5 = wrestler id (500 = clear), r6 = team, r7..r10 = a5..a8.
void wwe2k14_scn_part(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6, PPCRegister& r7,
                      PPCRegister& r8, PPCRegister& r9, PPCRegister& r10) {
  using namespace wwe2k14::scenario;
  if (!On() && !Capturing()) return;
  if (Capturing() && r3.u32 == CurrentM()) {
    Record("participant_set", {int32_t(r4.u32), int32_t(r5.u32), int32_t(r6.u32), int32_t(r7.u32), int32_t(r8.u32),
                               int32_t(r9.u32), int32_t(r10.u32)});
  }
  if (!On() || !Admit(g_n_part, "participant setter")) return;
  REXLOG_INFO("[wwe2k14][scenario] call participant_set(M={:08X}{}, i={}, id={}, team={}, a5={}, a6={}, a7={}, "
              "a8={}) from {:08X} frame={}",
              r3.u32, r3.u32 == CurrentM() ? "" : " NOT current M", int32_t(r4.u32), int32_t(r5.u32),
              int32_t(r6.u32), int32_t(r7.u32), int32_t(r8.u32), int32_t(r9.u32), int32_t(r10.u32), CallerLr(),
              Frame());
}

// sub_829263B0 entry (exhibition finalize; argument meanings unknown, logged raw).
void wwe2k14_scn_finalize(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) {
  using namespace wwe2k14::scenario;
  if (Capturing()) {
    std::lock_guard<std::mutex> lock(g_cap_mutex);
    g_calls.clear();
    g_finalize_lr = CallerLr();
  }
  if (!On() || !Admit(g_n_fin, "finalize")) return;
  REXLOG_INFO("[wwe2k14][scenario] call finalize(r3={:08X}, r4={:08X}, r5={:08X}, r6={:08X}) from {:08X} frame={}",
              r3.u32, r4.u32, r5.u32, r6.u32, CallerLr(), Frame());
  LogMatchDesc("finalize entry", CurrentM());
  SnapAll("finalize_entry", true, false, false);
  // r3 = the menu state object finalize reads the choices from (index at +11000, 1,100-byte records, fields read up
  // to +12520 in the listing): snapshot 12,800 bytes of it (layout G = flat offsets in diff_bin.py).
  REXLOG_INFO("[wwe2k14][scenario] snapshot finalize_entry_S: {} (S+11000={})",
              Snapshot("finalize_entry_S", r3.u32, kMenuStateSize), int32_t(U32(r3.u32 + 11000)));
}

// sub_822CA740 entry: r3 = M.
void wwe2k14_scn_apply(PPCRegister& r3) {
  using namespace wwe2k14::scenario;
  if (Capturing() && !g_launcher_applying.load()) {
    std::lock_guard<std::mutex> lock(g_cap_mutex);
    if (r3.u32 && r3.u32 == CurrentM()) {
      g_pending_m = CopyGuest(r3.u32, kMatchDescSize);
      g_pending_x = CopyExtras();
      g_pending = true;
      g_pending_lr = CallerLr();
      g_pending_frame = Frame();
    } else {
      REXLOG_INFO("[wwe2k14][scenario] capture: apply on {:08X}, not the current M {:08X}: not captured frame={}",
                  r3.u32, CurrentM(), Frame());
    }
  }
  if (!On() || !Admit(g_n_apply, "apply")) return;
  REXLOG_INFO("[wwe2k14][scenario] call apply(M={:08X}{}) from {:08X} frame={}", r3.u32,
              r3.u32 == CurrentM() ? "" : " NOT current M", CallerLr(), Frame());
  LogMatchDesc("apply entry", r3.u32);
  SnapAll("apply_entry", true, true, true);
}

// sub_822CA740 at 0x822CA77C (`addi r1,r1,96`, after its last call): G and A as apply left them.
void wwe2k14_scn_apply_exit() {
  using namespace wwe2k14::scenario;
  if (Capturing()) {
    std::lock_guard<std::mutex> lock(g_cap_mutex);
    if (g_pending) {
      WriteScenarioLocked(g_pending_m, g_pending_x);
      g_pending = false;
      g_pending_m.clear();
      g_pending_x.clear();
      g_calls.clear();
      g_finalize_lr = 0;
    }
  }
  if (!On() || !Admit(g_n_apply_exit, "apply exit")) return;
  REXLOG_INFO("[wwe2k14][scenario] apply done frame={}", Frame());
  LogSettings("apply exit");
  SnapAll("apply_exit", false, true, true);
}

// sub_827DD0B0 entry (launch match, no arguments).
void wwe2k14_scn_launch() {
  using namespace wwe2k14::scenario;
  if (!On() || !Admit(g_n_launch, "launch")) return;
  REXLOG_INFO("[wwe2k14][scenario] call launch() from {:08X} frame={}", CallerLr(), Frame());
  LogSettings("launch entry");
  LogMatchDesc("launch entry", CurrentM());
  SnapAll("launch_entry", true, true, true);
  // Step 3 diagnosis (entrances skipped after a launcher launch although M, G and A match): the image's whole
  // static data range after the code (q info: code ends 0x835C34F4, image ends 0x83A60000) and the first 128 KB of
  // the game data object P, so menu-driven and launcher runs can be compared word by word.
  REXLOG_INFO("[wwe2k14][scenario] snapshot launch_entry_D: {} (0x{:08X}..0x{:08X})",
              Snapshot("launch_entry_D", kImageDataStart, kImageDataEnd - kImageDataStart), kImageDataStart,
              kImageDataEnd);
  REXLOG_INFO("[wwe2k14][scenario] snapshot launch_entry_P: {} (P={:08X})",
              Snapshot("launch_entry_P", U32(kGameDataPtr), kGameDataSnapSize), U32(kGameDataPtr));
}

// sub_82833440 entry (main-menu dispatcher): the selection word it switches on. First calls, then changes only.
void wwe2k14_scn_dispatch() {
  using namespace wwe2k14::scenario;
  if (!On()) return;
  const uint32_t sel = U32(kMenuSelection);
  const uint32_t last = g_last_menu_sel.exchange(sel);
  const uint32_t n = ++g_n_disp;
  if (n > 8 && sel == last) return;
  REXLOG_INFO("[wwe2k14][scenario] call menu_dispatch #{} selection={} (was {}) from {:08X} frame={}", n,
              int32_t(sel), int32_t(last), CallerLr(), Frame());
}

// sub_8225E790 entry: r3 = object, r4 = sequence kind, r5 = factory function.
void wwe2k14_scn_switch(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {
  using namespace wwe2k14::scenario;
  if (!On() || !Admit(g_n_switch, "sequence switch")) return;
  REXLOG_INFO("[wwe2k14][scenario] call switch_sequence(obj={:08X}, kind={}, factory={:08X}) from {:08X} frame={}",
              r3.u32, int32_t(r4.u32), r5.u32, CallerLr(), Frame());
}

// ---------------------------------------------------------------------------------------------------------------
// Step 3, launch: --wwe2k14_launch=NAME (design A). Runs at the start of the per-frame owner update sub_82C302A0
// (main thread; coverage saw it in every scenario window), between the sequences' own updates:
//   1. load NAME.m.bin / .g.bin / .a.bin / .x.bin and check them against NAME.toml (sizes, FNV-1a 64); any problem
//      = no launch
//   2. wait until the root main menu has been idle for --wwe2k14_launch_idle_frames guest frames: sequence 75,
//      SmartMenu (0x8392945C) present, SM+364 == 1 (main menu), exit action 0x83929434 == 0 (states.toml entries)
//   3. restore the menus' arena record (extras +0 -> P+10528, set before finalize in a menu run), write the saved
//      bytes over M (*0x836F2D88), then the same steps finalize runs from its apply call on (listing of
//      sub_829263B0 at 0x82927120-0x8292718C, offline): apply sub_822CA740(M); sub_822CBD28() (builds the
//      wrestler list at P+73488.. from the slots: without it the in-game sequence skips the entrances, runs 472/478,
//      watch run 480); sub_827D7770(G+472). The online-only branches (sub_82B5AA48) and the mode-dependent tail that
//      reads the menu's state object are not run (no menu object exists here). Then compare G and A with the capture
//      (logged: words that differ), restore the active controller index (extras +16 -> SM+796) and call
//      smart_menu_request_exit sub_8283CFF0(SM, 1) = what the menus' screen handlers call on the final confirm. The
//      game's own per-frame code then does the rest, as in a menu-driven run: sub_8283AA18 sees the exit action,
//      SM+11404 gets set, the page callback sub_82840490 hands SM+796 on (0x83626D80), deletes the SmartMenu and
//      runs the dispatcher, case 1 = launch sub_827DD0B0.
//   4. log when the in-game sequence (9) starts, or warn if it has not after kLaunchWarnFrames.
// The guest calls run on the live context: every register is saved before and restored after each call (the
// owner update's own body has not started yet, so it sees its entry registers). lr is 0 during the calls, so the
// observe hooks show the caller as 00000000 (= this launcher). One launch per run.

REXCVAR_DEFINE_STRING(wwe2k14_launch, "", "wwe2k14",
                      "Launch scenario NAME (from --wwe2k14_scenario_dir) once the main menu is idle: restores the "
                      "captured match description, applies it and leaves the main menu the game's own way "
                      "(exit action 1). Empty = off. See src/scenario.h");
REXCVAR_DEFINE_INT64(wwe2k14_launch_idle_frames, 60, "wwe2k14",
                     "Guest frames the root main menu must stay idle before --wwe2k14_launch fires");
REXCVAR_DEFINE_INT64(wwe2k14_launch_arena, -1, "wwe2k14",
                     "Launch the scenario in arena N instead (the arena setter's value = pac/bg/BGnn.PAC, "
                     "db/ids/arenas.json), set through the game's arena setter sub_822C9748; -1 = the captured arena");
REXCVAR_DEFINE_STRING(wwe2k14_launch_participants, "", "wwe2k14",
                      "Replace the scenario's participants: comma-separated ID[:PAD] for slots 0, 1, ... (character "
                      "ids, db/ids/characters.json; PAD = controller 0-3, default -1 = CPU), set through the game's "
                      "participant setter sub_822C89F8 like the demo launcher does. Empty = the captured ones");
// Session 27 item 8: Quick Match without a captured setup.
REXCVAR_DEFINE_INT64(wwe2k14_quick_type, 0, "wwe2k14",
                     "With --wwe2k14_launch=@quick: the match type id for the game's type setter (0 1v1, 3 tag, 23 Royal "
                     "Rumble, 37 six-man, 39 Elimination Chamber, 49 Hell in a Cell; docs/systems/match_setup.md)");
REXCVAR_DEFINE_STRING(wwe2k14_quick_compare, "", "wwe2k14",
                      "Check only: with --wwe2k14_launch=@quick, log how the built match description (M) and, after apply, "
                      "the settings (G) and slots (A) differ from scenario NAME's capture");
REXCVAR_DEFINE_INT64(wwe2k14_quick_pace, 0, "wwe2k14",
                     "With --wwe2k14_launch=@quick: match pace 0 Normal, 1 Quick, 2 Epic");
REXCVAR_DEFINE_STRING(wwe2k14_quick_rules, "", "wwe2k14",
                      "Session 30: with --wwe2k14_launch=@quick: rule bytes of the match description M to set after the "
                      "type setter, before apply: OFF=BYTE,... (OFF 12..75, M byte offsets; apply copies M+12..75 to "
                      "G+0..63). Empty = the menus' values for the types checked (60 steel cage: M+21=3; 81 Inferno: "
                      "M+71=3); 'none' = the type's bare defaults");
REXCVAR_DEFINE_BOOL(wwe2k14_launch_restore_ga, true, "wwe2k14",
                    "After the launcher's apply, write the captured settings struct G and participant slots A back "
                    "when they differ (e.g. a Royal Rumble's entry order, drawn during apply). false = keep what "
                    "this apply produced");
REXCVAR_DEFINE_BOOL(wwe2k14_launch_sync_slot_copy, false, "wwe2k14",
                    "Launcher (Session 13): after apply, copy the participant slots A (P+148, 43 x 56 bytes) to the "
                    "second copy right after them (P+2556), as the menus leave it (runs 978 / 979: both copies equal "
                    "A at launch; a launch leaves the second one stale). Default off: it did not fix the Rumble "
                    "entrants #1 / #2 (runs 981 / 985); menu-equivalent state, Jarrod's call to make it default");
REXCVAR_DEFINE_BOOL(wwe2k14_launch_restore_pblock, false, "wwe2k14",
                    "Launcher (Session 13): restore the P block the menus' finalize fills (NAME.pblock.bin, "
                    "P+0x2934..+0x2D3F) when the scenario has one. Default off: restoring it did not fix the Rumble "
                    "entrants #1 / #2 (run 985)");
REXCVAR_DEFINE_BOOL(wwe2k14_launch_refill_participants, true, "wwe2k14",
                    "Launcher (Session 14): when the captured participant slots are written back (a Royal Rumble's "
                    "entry order), rebuild the per-participant array [0x836F2D98] from them the way apply does, so "
                    "entrants #1 / #2 get their entrances. Launches only; false = Session 13 behaviour");
REXCVAR_DEFINE_BOOL(wwe2k14_launch_skip_entrances, false, "wwe2k14",
                    "Test tool (Session 13): launch straight into the match without entrances by skipping the wrestler "
                    "list build (sub_822CBD28) after apply, as in run 472 (match loaded, no EVT / ENTRANCE / titantron "
                    "files). Side effects of the empty P wrestler list (P+73488..) are unconfirmed; for match-only "
                    "tests");
REXCVAR_DEFINE_STRING(wwe2k14_launch_poke, "", "wwe2k14",
                      "Debug (step 3 diagnosis): guest words to write after the launcher's apply and before its exit "
                      "request, comma-separated ADDR=VALUE; ADDR = 0x... or P+OFFSET (P = the game data object, word "
                      "at 0x8376AAF0) or S+OFFSET (the SmartMenu, word at 0x8392945C); numbers decimal or 0x-hex; "
                      "big-endian 32-bit words. Empty = none");

REX_EXTERN(__imp__sub_82C302A0);  // per-frame owner update (the original body)
REX_EXTERN(sub_822CA740);         // apply M to G and the participant slots
REX_EXTERN(sub_8283CFF0);         // smart_menu_request_exit(SM, action)
REX_EXTERN(sub_822CBD28);         // finalize's step after apply: wrestler list in P from the slots (no arguments)
REX_EXTERN(sub_827D7770);         // finalize's next step: reset of the block at G+472
REX_EXTERN(sub_822C9748);         // arena setter (M, arena, flag)
REX_EXTERN(sub_822C89F8);         // participant setter (M, i, id, controller, a5, a6, a7, a8)
REX_EXTERN(sub_822C0898);         // per-character data (id) -> pointer; the demo launcher reads +108 for a6
REX_EXTERN(sub_82B6D900);         // Session 27: mode setter (M, mode): M+4
REX_EXTERN(sub_822CA9C0);         // Session 27: match type setter (M, type, options; 0 = the type's defaults)
REX_EXTERN(sub_822C6668);         // Session 27: pace setter (M, pace): M+528

namespace wwe2k14::scenario {

constexpr uint32_t kSequenceManagerPtr = 0x839400F8;  // states.toml "seq": [[0x839400F8]+0]+48
constexpr uint32_t kSmartMenuPtr = 0x8392945C;         // states.toml "smart_menu"
constexpr uint32_t kSeqMainMenu = 75, kSeqInGame = 9, kMenuMain = 1;
constexpr uint64_t kLaunchWarnFrames = 900;

enum class LaunchState { kOff, kWaiting, kRequested, kDone, kFailed };
// Main thread only (the owner update), so no locking.
inline LaunchState g_launch_state = LaunchState::kOff;
inline std::string g_launch_name;
inline std::vector<uint8_t> g_launch_m, g_launch_g, g_launch_a, g_launch_x;
inline std::vector<uint8_t> g_launch_pblock;  // optional (empty = none)
inline uint64_t g_launch_last_frame = 0, g_launch_idle = 0, g_launch_fired_frame = 0;
inline uint32_t g_launch_last_seq = 0;

inline uint32_t SequenceId() {
  const uint32_t mgr = U32(kSequenceManagerPtr);
  const uint32_t cur = mgr ? U32(mgr) : 0;
  return cur ? U32(cur + 48) : 0;
}

inline bool ReadAll(const std::filesystem::path& path, std::vector<uint8_t>& out) {
  std::FILE* f = std::fopen(path.string().c_str(), "rb");
  if (!f) return false;
  out.clear();
  uint8_t buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
  std::fclose(f);
  return true;
}

// The fnv1a64 value of `KEY = { ... }` in a scenario .toml as WriteScenarioLocked writes it ("" if not found).
inline std::string TomlFileHash(const std::string& toml, const std::string& key) {
  const std::string start = "\n" + key + " = {";
  const size_t at = toml.find(start);
  if (at == std::string::npos) return "";
  const size_t end = toml.find('\n', at + 1);
  const size_t h = toml.find("fnv1a64 = \"", at);
  if (h == std::string::npos || (end != std::string::npos && h > end)) return "";
  return toml.substr(h + 11, 16);
}

inline bool LoadScenario(const std::string& name) {
  if (!ValidName(name)) {
    REXLOG_WARN("[wwe2k14][scenario] launch: name '{}' not used: letters, digits, _ and - only, 1-64 characters",
                name);
    return false;
  }
  const std::filesystem::path dir = ScenarioDir();
  std::vector<uint8_t> toml_bytes;
  if (!ReadAll(dir / (name + ".toml"), toml_bytes)) {
    REXLOG_WARN("[wwe2k14][scenario] launch '{}': {} not found (list them with tools/scenarios/list.py)", name,
                (dir / (name + ".toml")).string());
    return false;
  }
  const std::string toml(toml_bytes.begin(), toml_bytes.end());
  struct Part {
    const char* key;
    uint32_t size;
    std::vector<uint8_t>* out;
  } parts[] = {{"m", kMatchDescSize, &g_launch_m},
               {"g", kSettingsSize, &g_launch_g},
               {"a", kSlotsSize, &g_launch_a},
               {"x", kExtrasSize, &g_launch_x}};
  for (const Part& p : parts) {
    const std::filesystem::path file = dir / (name + "." + p.key + ".bin");
    if (!ReadAll(file, *p.out)) {
      REXLOG_WARN("[wwe2k14][scenario] launch '{}': {} not found{}", name, file.string(),
                  std::string(p.key) == "x" ? " (captured before format 2, which added the extras: capture it again)"
                                            : "");
      return false;
    }
    if (p.out->size() != p.size) {
      REXLOG_WARN("[wwe2k14][scenario] launch '{}': {} is {} bytes, expected {}", name, file.string(),
                  p.out->size(), p.size);
      return false;
    }
    const std::string want = TomlFileHash(toml, p.key), got = HashHex(Fnv1a64(*p.out));
    if (want != got) {
      REXLOG_WARN("[wwe2k14][scenario] launch '{}': {} hash {} does not match the .toml ({})", name, file.string(),
                  got, want.empty() ? "missing" : want);
      return false;
    }
  }
  g_launch_pblock.clear();
  if (const std::filesystem::path pb = dir / (name + ".pblock.bin"); std::filesystem::exists(pb)) {
    if (!ReadAll(pb, g_launch_pblock) || g_launch_pblock.size() != kPBlockSize) {
      REXLOG_WARN("[wwe2k14][scenario] launch '{}': {} unreadable or not {} bytes; not used", name, pb.string(),
                  kPBlockSize);
      g_launch_pblock.clear();
    }
  }
  const auto& m = g_launch_m;
  REXLOG_INFO("[wwe2k14][scenario] launch '{}': loaded from {} (type={} mode={} arena={} ids {} / {}); waiting for "
              "the idle main menu ({} frames) frame={}",
              name, dir.string(), int32_t(BeU32(m, 8)), int32_t(BeU32(m, 4)), int32_t(BeU32(m, 532)),
              BeU32(m, 724 + 56), BeU32(m, 724 + 3580 + 56), REXCVAR_GET(wwe2k14_launch_idle_frames), Frame());
  return true;
}

// Words of guest memory at `address` that differ from `ref`; the first few offsets go to `first`.
inline size_t DiffWords(uint32_t address, const std::vector<uint8_t>& ref, std::string& first) {
  if (!address) {
    first = " (null address)";
    return ref.size() / 4;
  }
  const uint8_t* g = Guest(address);
  size_t n = 0;
  for (size_t off = 0; off + 4 <= ref.size(); off += 4) {
    if (std::memcmp(g + off, ref.data() + off, 4) == 0) continue;
    if (++n <= 8) first += fmt::format(" +{}", off);
  }
  return n;
}

// --wwe2k14_launch_poke: "ADDR=VALUE,..." (see the setting). Bad tokens are logged and skipped.
inline void ApplyLaunchPokes() {
  const std::string spec = REXCVAR_GET(wwe2k14_launch_poke);
  if (spec.empty()) return;
  size_t pos = 0;
  while (pos <= spec.size()) {
    size_t comma = spec.find(',', pos);
    if (comma == std::string::npos) comma = spec.size();
    const std::string token = spec.substr(pos, comma - pos);
    pos = comma + 1;
    if (token.empty()) continue;
    const size_t eq = token.find('=');
    if (eq == std::string::npos) {
      REXLOG_WARN("[wwe2k14][scenario] launch poke '{}': no '=', skipped", token);
      continue;
    }
    std::string where = token.substr(0, eq);
    const std::string value_text = token.substr(eq + 1);
    uint64_t address = 0;
    char rel = 0;  // 'P' = game data object, 'S' = SmartMenu (word at 0x8392945C), 0 = absolute
    if (where.size() > 2 && where[1] == '+' && (where[0] == 'P' || where[0] == 'S')) {
      rel = where[0];
      where = where.substr(2);
    }
    char* end = nullptr;
    address = std::strtoull(where.c_str(), &end, 0);
    const bool addr_ok = end && *end == '\0' && !where.empty();
    const uint64_t value = std::strtoull(value_text.c_str(), &end, 0);
    const bool value_ok = end && *end == '\0' && !value_text.empty() && value <= 0xFFFFFFFFull;
    const uint32_t rel_base = rel == 'P' ? U32(kGameDataPtr) : rel == 'S' ? U32(kSmartMenuPtr) : 0;
    address += rel_base;
    if (!addr_ok || !value_ok || (rel && !rel_base) || address > 0xFFFFFFFCull || (address & 3)) {
      REXLOG_WARN("[wwe2k14][scenario] launch poke '{}': bad address or value, skipped", token);
      continue;
    }
    const uint32_t old = U32(uint32_t(address));
    rex::memory::store_and_swap<uint32_t>(Guest(uint32_t(address)), uint32_t(value));
    REXLOG_INFO("[wwe2k14][scenario] launch poke {:08X}{}: {:08X} -> {:08X}", uint32_t(address),
                rel ? fmt::format(" ({}+{})", rel, address - rel_base) : std::string(), old, uint32_t(value));
  }
}

// Session 14 (M5, the launched Royal Rumble's entrants #1 / #2): apply fills the per-participant array
// [0x836F2D98] from the in-ring slots near the end of sub_822CA008 (0x822CA560..0x822CA6FC). In a launched Rumble
// apply's own entry-order draw has no menu roster to draw from, so the in-ring slots are empty at that loop and the
// array stays empty (run 1035: entry 0 / 1 entrance id 0 / 0); the launcher then writes the captured slots back, but
// the array is already built. The entrance object copies each slot's entrance record from that array for Rumble-type
// matches (sub_82318460), so entrants #1 / #2 get entrance blocks with id 0 and the game skips them (runs 1022-1033).
// This runs the same loop again on the restored slots, calling the game's own functions with the same arguments
// (listing of sub_822CA008; slot list {0..5, 40, 41, 42} from sub_827D7180).
REX_EXTERN(sub_827D7368);  // slot index -> slot object (P+144)
REX_EXTERN(sub_822FF368);  // slot object -> character word (0xC800 = empty)
REX_EXTERN(sub_822C0A68);  // character word -> character id
REX_EXTERN(sub_822CE2C0);  // array, index, id, custom data or 0: fill the entry
REX_EXTERN(sub_822CE098);  // array, index, id, 0: second character (slot +48)
REX_EXTERN(sub_822CDCC0);  // array, index, 231 / 232, value
REX_EXTERN(sub_822C7880);  // M, index
REX_EXTERN(sub_822CE040);  // array, index
REX_EXTERN(sub_822CDD88);  // array, index, k, M data
inline void RefillParticipantArray(PPCContext& ctx, uint8_t* base, uint32_t m) {
  const uint32_t arr = U32(kParticipantArrayPtr);
  if (!arr || !m) return;
  const PPCContext saved = ctx;
  auto call = [&](void (*fn)(PPCContext&, uint8_t*), std::initializer_list<uint64_t> args) -> uint32_t {
    ctx = saved;
    ctx.lr = 0;
    uint64_t* regs[] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64};
    size_t i = 0;
    for (uint64_t a : args) *regs[i++] = a;
    fn(ctx, base);
    const uint32_t r3 = ctx.r3.u32;
    ctx = saved;
    return r3;
  };
  uint32_t f31_bits = U32(0x8218B8CC);  // lfs f31,-18228(0x82190000) at 0x822CA574
  float f31;
  std::memcpy(&f31, &f31_bits, 4);
  constexpr uint32_t kList[9] = {0, 1, 2, 3, 4, 5, 40, 41, 42};
  std::string filled;
  for (uint32_t idx = 0; idx < 9; ++idx) {
    const uint32_t slot = kList[idx];
    const uint32_t sobj = call(sub_827D7368, {slot});
    const uint32_t chr = call(sub_822FF368, {sobj});
    if (chr == 0xC800) continue;
    const uint32_t e = m + slot * 3580;
    const uint32_t custom = (idx < 6 && U32(e + 792) != 0) ? e + 828 : 0;
    const uint32_t id = call(sub_822C0A68, {chr});
    call(sub_822CE2C0, {arr, idx, id, custom});
    if (!custom) {
      const uint32_t second = U32(call(sub_827D7368, {slot}) + 48);
      if (second != 0xC800) call(sub_822CE098, {arr, idx, call(sub_822C0A68, {second}), 0});
    }
    call(sub_822CDCC0, {arr, idx, 231, U32(e + 816) & 0xFFFF});
    call(sub_822CDCC0, {arr, idx, 232, U32(e + 816) & 0xFFFF});
    call(sub_822C7880, {m, idx});
    if (idx < 6) {
      const uint32_t fb = U32(e + 3752);
      float fv;
      std::memcpy(&fv, &fb, 4);
      if (fv > f31) call(sub_822CE040, {arr, idx});
    }
    for (uint32_t k = 0; k < 4; ++k) {
      if (U32(e + 4204 + 4 * k) != 0) call(sub_822CDD88, {arr, idx, k, e + 3756 + 112 * k});
    }
    filled += fmt::format(" {}:{}", idx, id);
  }
  const uint32_t entries = U32(arr);
  REXLOG_INFO("[wwe2k14][scenario] launch '{}': participant array refilled from the restored slots (index:id{}); "
              "entry 0 / 1 entrance id {} / {} frame={}",
              g_launch_name, filled, entries ? U32(entries + kEntranceRecordId) >> 16 : 0,
              entries ? U32(entries + kParticipantEntrySize + kEntranceRecordId) >> 16 : 0, Frame());
}

inline void FireLaunch(PPCContext& ctx, uint8_t* base, uint32_t sm) {
  const uint32_t m = CurrentM();
  if (!m) {
    REXLOG_WARN("[wwe2k14][scenario] launch '{}': the match description pointer (0x836F2D88) is null; not launching",
                g_launch_name);
    g_launch_state = LaunchState::kFailed;
    return;
  }
  const uint32_t p = U32(kGameDataPtr);
  if (!p) {
    REXLOG_WARN("[wwe2k14][scenario] launch '{}': the game data object pointer (0x8376AAF0) is null; not launching",
                g_launch_name);
    g_launch_state = LaunchState::kFailed;
    return;
  }
  // The menus' arena record is set at the arena confirm, before finalize: restore it first.
  const uint32_t old_flag = U32(p + kArenaRecordOffset + 4), old_arena = U32(p + kArenaRecordOffset + 8);
  const bool build = g_launch_m.empty();  // Session 27: --wwe2k14_launch=@quick (no captured files)
  const PPCContext saved = ctx;
  if (!build) {
    std::memcpy(Guest(p + kArenaRecordOffset), g_launch_x.data(), kArenaRecordSize);
    std::memcpy(Guest(m), g_launch_m.data(), g_launch_m.size());
  } else {
    // Session 27 item 8 (Quick Match from our id tables): the game's own match description as the main menu left it,
    // set the way the attract demo's launcher sub_82800158 does (mode, type with no options = the type's defaults,
    // arena), plus the pace. Nothing in the release is a copy of game memory: only our numbers go in.
    auto e = [&](uint32_t entry) { return U32(m + 724 + 3580 * entry + 56); };
    REXLOG_INFO("[wwe2k14][scenario] quick: M at {:08X} before: mode {} type {} pace {} arena {}; ids {} {} {} {} {} {}; "
                "extras {} {} {} frame={}",
                m, int32_t(U32(m + 4)), int32_t(U32(m + 8)), int32_t(U32(m + 528)), int32_t(U32(m + 532)), e(0), e(1),
                e(2), e(3), e(4), e(5), e(40), e(41), e(42), Frame());
    auto call = [&](void (*fn)(PPCContext&, uint8_t*), std::initializer_list<uint64_t> args) {
      ctx = saved;
      ctx.lr = 0;
      uint64_t* regs[] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64};
      size_t i = 0;
      for (uint64_t a : args) *regs[i++] = a;
      fn(ctx, base);
      ctx = saved;
    };
    call(sub_82B6D900, {m, 0});                                              // mode 0 = exhibition
    call(sub_822CA9C0, {m, uint64_t(std::clamp<int64_t>(REXCVAR_GET(wwe2k14_quick_type), 0, 118)), 0});  // 119 types
    call(sub_822C6668, {m, uint64_t(std::clamp<int64_t>(REXCVAR_GET(wwe2k14_quick_pace), 0, 2))});  // pace
    // Session 30: rule bytes the menus write but the type's defaults leave out. Found by bisecting menu captures
    // against Quick Matches (runs 2298-2330, docs/systems/match_setup.md): M+21 bit 0 = the steel cage, M+71 bit 0 =
    // the Inferno fire ring (the type setter without the menus' options pointer leaves 0 / 2: no cage, no fire).
    const int64_t qtype = std::clamp<int64_t>(REXCVAR_GET(wwe2k14_quick_type), 0, 118);
    std::string rules = REXCVAR_GET(wwe2k14_quick_rules);
    // (Session 31: the other steel cages, 61-63, have M+21 = 3 from their own type defaults: runs 2387-2389, 2405.)
    if (rules.empty()) rules = qtype == 60 ? "21=3" : qtype == 81 ? "71=3" : "";
    if (rules == "none") rules.clear();
    std::string applied;
    size_t rpos = 0;
    while (rpos < rules.size()) {
      size_t comma = rules.find(',', rpos);
      if (comma == std::string::npos) comma = rules.size();
      const std::string token = rules.substr(rpos, comma - rpos);
      rpos = comma + 1;
      const size_t eq = token.find('=');
      const long off = eq == std::string::npos ? -1 : std::strtol(token.substr(0, eq).c_str(), nullptr, 0);
      const long val = eq == std::string::npos ? -1 : std::strtol(token.substr(eq + 1).c_str(), nullptr, 0);
      if (off < 12 || off > 75 || val < 0 || val > 255) {
        REXLOG_WARN("[wwe2k14][scenario] quick rules: '{}' skipped (OFF 12..75 = BYTE 0..255)", token);
        continue;
      }
      applied += fmt::format(" M+{}: {} -> {}", off, U8(m + uint32_t(off)), val);
      *Guest(m + uint32_t(off)) = uint8_t(val);
    }
    if (!applied.empty()) REXLOG_INFO("[wwe2k14][scenario] quick rules (type {}):{}", qtype, applied);
  }
  // Overrides (step 5): through the game's own setters, the way finalize / the demo launcher call them, so the
  // derived data is recomputed. A changed match no longer equals the captured G and A: no write-back then.
  bool overridden = build;
  uint32_t parts_set = 0;
  int64_t first_pad = -1;
  const std::string parts = REXCVAR_GET(wwe2k14_launch_participants);
  if (!parts.empty()) {
    uint32_t slot = 0;
    size_t pos = 0;
    while (pos <= parts.size() && slot < 40) {
      size_t comma = parts.find(',', pos);
      if (comma == std::string::npos) comma = parts.size();
      const std::string token = parts.substr(pos, comma - pos);
      pos = comma + 1;
      if (token.empty()) continue;
      const size_t colon = token.find(':');
      char* end = nullptr;
      const long id = std::strtol(token.substr(0, colon).c_str(), &end, 10);
      long pad = -1;
      if (colon != std::string::npos) pad = std::strtol(token.substr(colon + 1).c_str(), nullptr, 10);
      if (id < 0 || id >= 500 || pad < -1 || pad > 3) {
        REXLOG_WARN("[wwe2k14][scenario] launch participant '{}' skipped (id 0-499, pad -1..3)", token);
        ++slot;
        continue;
      }
      ctx.lr = 0;
      ctx.r3.u64 = uint64_t(id);
      sub_822C0898(ctx, base);  // per-character data
      const uint32_t chr = ctx.r3.u32;
      ctx = saved;
      const uint32_t a6 = chr ? U8(chr + 108) : 0;  // as the demo launcher sub_82800158 passes it
      ctx.lr = 0;
      ctx.r3.u64 = m;
      ctx.r4.u64 = slot;
      ctx.r5.u64 = uint64_t(id);
      ctx.r6.s64 = pad;
      ctx.r7.u64 = 0;   // a5 (0 in every capture)
      ctx.r8.u64 = a6;
      ctx.r9.u64 = 1;   // a7, a8 (1 in every capture)
      ctx.r10.u64 = 1;
      sub_822C89F8(ctx, base);
      ctx = saved;
      REXLOG_INFO("[wwe2k14][scenario] launch '{}': participant {} = id {} ({}), a6 {} (character byte +108) frame={}",
                  g_launch_name, slot, id, pad < 0 ? "CPU" : fmt::format("pad {}", pad), a6, Frame());
      if (pad >= 0 && first_pad < 0) first_pad = pad;
      ++slot;
      overridden = true;
    }
    parts_set = slot;
  }
  if (build) {
    // Session 27: the three extra people, as in every capture (255 referee, 150 Michael Cole, 153 Jerry Lawler): the
    // idle main menu leaves 512 (empty) in slots 40-42, and apply fills them for some types only (runs 1949 / 1954:
    // type 0 got them, types 37 / 39 had no referee).
    constexpr uint32_t kExtras[3][2] = {{40, 255}, {41, 150}, {42, 153}};
    for (const auto& x : kExtras) {
      if (U32(m + 724 + 3580 * x[0] + 56) < 500) continue;
      ctx = saved;
      ctx.lr = 0;
      ctx.r3.u64 = x[1];
      sub_822C0898(ctx, base);
      const uint32_t chr = ctx.r3.u32;
      ctx = saved;
      ctx.lr = 0;
      ctx.r3.u64 = m, ctx.r4.u64 = x[0], ctx.r5.u64 = x[1], ctx.r6.s64 = -1;
      ctx.r7.u64 = 0, ctx.r8.u64 = chr ? U8(chr + 108) : 0, ctx.r9.u64 = 1, ctx.r10.u64 = 1;
      sub_822C89F8(ctx, base);
      ctx = saved;
    }
    // Session 27: the slots after ours emptied (id 500 = an empty slot in sub_822C6BD8), so a menu's earlier match can't
    // leave extra wrestlers in this one.
    for (uint32_t slot = parts_set; slot < 40; ++slot) {
      if (U32(m + 724 + 3580 * slot + 56) >= 500) continue;
      ctx = saved;
      ctx.lr = 0;
      ctx.r3.u64 = m, ctx.r4.u64 = slot, ctx.r5.u64 = 500, ctx.r6.s64 = -1;
      ctx.r7.u64 = 0, ctx.r8.u64 = 0, ctx.r9.u64 = 1, ctx.r10.u64 = 1;
      sub_822C89F8(ctx, base);
      ctx = saved;
    }
  }
  const int64_t arena = REXCVAR_GET(wwe2k14_launch_arena);
  if (arena >= 0) {
    ctx.lr = 0;
    ctx.r3.u64 = m;
    ctx.r4.u64 = uint64_t(arena);
    ctx.r5.u64 = U8(m + 537);  // the captured flag
    sub_822C9748(ctx, base);
    ctx = saved;
    rex::memory::store_and_swap<uint32_t>(Guest(p + kArenaRecordOffset + 4), 1);
    rex::memory::store_and_swap<uint32_t>(Guest(p + kArenaRecordOffset + 8), uint32_t(arena));
    REXLOG_INFO("[wwe2k14][scenario] launch '{}': arena {} -> {} (M+532 now {}) frame={}", g_launch_name,
                build ? -1 : int32_t(BeU32(g_launch_m, 532)), arena, U32(m + 532), Frame());
    overridden = true;
  }
  g_launcher_applying = true;
  ctx.lr = 0;
  ctx.r3.u64 = m;
  sub_822CA740(ctx, base);  // apply
  ctx = saved;
  g_launcher_applying = false;
  // Session 31: a built match's sides as apply left them (slot +13 = team, +16 = id x 100 + 2): which slots a
  // handicap or tag type puts together, for the launcher's per-type layouts.
  if (const uint32_t slots = SlotsAddress(); build && slots) {
    std::string sides;
    for (uint32_t i = 0; i < parts_set; ++i)
      sides += fmt::format(" {}:{}/t{}", i, U32(slots + 56 * i + 16) / 100, U8(slots + 56 * i + 13));
    REXLOG_INFO("[wwe2k14][scenario] quick sides (type {}): slot:id/team{} frame={}",
                REXCVAR_GET(wwe2k14_quick_type), sides, Frame());
  }
  // G and A were captured at apply exit in the menu run, so compare now, before finalize's later steps.
  std::string g_first, a_first;
  const size_t g_diff = DiffWords(kSettings, g_launch_g, g_first);
  const size_t a_diff = DiffWords(SlotsAddress(), g_launch_a, a_first);
  // Session 27 item 8 (check only): a built match against a capture of the same match (G and A at apply's exit).
  if (const std::string cmp = REXCVAR_GET(wwe2k14_quick_compare); build && !cmp.empty() && ValidName(cmp)) {
    std::vector<uint8_t> cg, ca, cm;
    const auto dir = ScenarioDir();
    if (ReadAll(dir / (cmp + ".g.bin"), cg) && ReadAll(dir / (cmp + ".a.bin"), ca) && ReadAll(dir / (cmp + ".m.bin"), cm) &&
        cg.size() == kSettingsSize && ca.size() == kSlotsSize && cm.size() == kMatchDescSize) {
      std::string gf, af, mf;
      const size_t gd = DiffWords(kSettings, cg, gf), ad = DiffWords(SlotsAddress(), ca, af), md = DiffWords(m, cm, mf);
      REXLOG_INFO("[wwe2k14][scenario] quick vs capture '{}': M {} words differ{}{}; G {} words differ{}{}; A {} words "
                  "differ{}{} frame={}", cmp, md, md ? " at" : "", mf, gd, gd ? " at" : "", gf, ad, ad ? " at" : "", af,
                  Frame());
      std::string gv;
      for (size_t off = 0; off + 4 <= cg.size() && gv.size() < 600; off += 4)
        if (U32(kSettings + uint32_t(off)) != BeU32(cg, off))
          gv += fmt::format(" +{} {:08X}/{:08X}", off, U32(kSettings + uint32_t(off)), BeU32(cg, off));
      REXLOG_INFO("[wwe2k14][scenario] quick vs capture '{}': G words built/captured:{}", cmp, gv);
    } else {
      REXLOG_WARN("[wwe2k14][scenario] quick vs capture '{}': capture files not found in {}", cmp, dir.string());
    }
  }
  REXLOG_INFO("[wwe2k14][scenario] launch '{}': arena record {} / {} -> {} / {}; M restored at {:08X} and applied; "
              "vs the capture: G {} words differ{}{}, A {} words differ{}{} frame={}",
              g_launch_name, int32_t(old_flag), int32_t(old_arena), int32_t(U32(p + kArenaRecordOffset + 4)),
              int32_t(U32(p + kArenaRecordOffset + 8)), m, g_diff, g_diff ? " at" : "", g_first, a_diff,
              a_diff ? " at" : "", a_first, Frame());
  {
    const uint32_t arr = U32(kParticipantArrayPtr), entries = arr ? U32(arr) : 0;
    REXLOG_INFO("[wwe2k14][scenario] launch '{}': after apply, participant array entry 0 / 1 entrance id {} / {} "
                "frame={}",
                g_launch_name, entries ? U32(entries + kEntranceRecordId) >> 16 : 0,
                entries ? U32(entries + kParticipantEntrySize + kEntranceRecordId) >> 16 : 0, Frame());
  }
  // Apply also builds state that M does not hold: for a Royal Rumble it draws the entry order of the 40 entrants
  // (from the menu state object's roster at +11024, screen order) into G+84.. and the slots (capture s4_3, run 486:
  // 82 words of G+76..+472 and 97 of A change during apply; M holds 6 leftover entries). To launch exactly the
  // captured match, the captured G and A (apply exit) are written back when they differ. Off = keep what this
  // apply produced (to study the game's own draw).
  if ((g_diff || a_diff) && overridden) {
    REXLOG_INFO("[wwe2k14][scenario] launch '{}': arena / participants overridden: G and A are kept as this apply "
                "made them (no write-back) frame={}", g_launch_name, Frame());
  } else if ((g_diff || a_diff) && REXCVAR_GET(wwe2k14_launch_restore_ga)) {
    std::memcpy(Guest(kSettings), g_launch_g.data(), g_launch_g.size());
    if (const uint32_t slots = SlotsAddress()) std::memcpy(Guest(slots), g_launch_a.data(), g_launch_a.size());
    REXLOG_INFO("[wwe2k14][scenario] launch '{}': G and A written back as captured ({} + {} words) frame={}",
                g_launch_name, g_diff, a_diff, Frame());
    // Session 14: the slots changed (a Royal Rumble's drawn entry order): rebuild the participant array from them.
    if (a_diff && REXCVAR_GET(wwe2k14_launch_refill_participants)) RefillParticipantArray(ctx, base, m);
  }
  // The menus leave a second copy of the slots right after them (P+148+2408 = P+2556) equal to A; a launch leaves it
  // stale (Session 13, runs 978-980). Syncing it did not fix the Rumble's skipped first entrances (run 981): off by
  // default (--wwe2k14_launch_sync_slot_copy).
  if (const uint32_t slots = SlotsAddress(); slots && REXCVAR_GET(wwe2k14_launch_sync_slot_copy)) {
    std::string first;
    const size_t stale = DiffWords(slots + kSlotsSize, std::vector<uint8_t>(Guest(slots), Guest(slots) + kSlotsSize),
                                   first);
    std::memmove(Guest(slots + kSlotsSize), Guest(slots), kSlotsSize);
    REXLOG_INFO("[wwe2k14][scenario] launch '{}': second slot copy P+{} synced from the slots ({} words differed)",
                g_launch_name, kSlotsOffset + kSlotsSize, stale);
  }
  if (!g_launch_pblock.empty() && REXCVAR_GET(wwe2k14_launch_restore_pblock)) {
    std::memcpy(Guest(p + kPBlockOffset), g_launch_pblock.data(), g_launch_pblock.size());
    if (U32(p + kPBlockOffset) != 0) {
      rex::memory::store_and_swap<uint32_t>(Guest(p + kPBlockOffset), p + kPBlockSelfTarget);
    }
    REXLOG_INFO("[wwe2k14][scenario] launch '{}': P block P+0x{:X} ({} bytes) restored from {}.pblock.bin",
                g_launch_name, kPBlockOffset, kPBlockSize, g_launch_name);
  }
  // Finalize's next steps after apply (offline): the wrestler list, then the G+472 block reset. Without the list the
  // in-game sequence skips the entrances (run 472): --wwe2k14_launch_skip_entrances.
  if (REXCVAR_GET(wwe2k14_launch_skip_entrances)) {
    REXLOG_INFO("[wwe2k14][scenario] launch '{}': wrestler list not built (--wwe2k14_launch_skip_entrances): no "
                "entrances", g_launch_name);
  } else {
    ctx.lr = 0;
    sub_822CBD28(ctx, base);
    ctx = saved;
  }
  ctx.lr = 0;
  ctx.r3.u64 = kSettings + 472;
  sub_827D7770(ctx, base);
  ctx = saved;
  // Session 27: a built match has no captured controller: the first participant given a pad, else pad 1 (index 0).
  const uint32_t pad = build ? uint32_t(first_pad >= 0 ? first_pad : 0) : BeU32(g_launch_x, 16);
  const uint32_t old_pad = U32(sm + kSmPadOffset);
  rex::memory::store_and_swap<uint32_t>(Guest(sm + kSmPadOffset), pad);
  REXLOG_INFO("[wwe2k14][scenario] launch '{}': wrestler list built (P+73496 = {:08X}, P+73576 = {:08X}); active "
              "controller {} -> {} frame={}",
              g_launch_name, U32(p + 73496), U32(p + 73576), int32_t(old_pad), int32_t(pad), Frame());
  ApplyLaunchPokes();
  ctx.lr = 0;
  ctx.r3.u64 = sm;
  ctx.r4.u64 = 1;  // exit action 1 = launch exhibition match
  sub_8283CFF0(ctx, base);
  ctx = saved;
  g_launch_fired_frame = Frame();
  g_launch_last_seq = SequenceId();
  g_launch_state = LaunchState::kRequested;
  REXLOG_INFO("[wwe2k14][scenario] launch '{}': exit action requested (0x83929434 = {}) frame={}", g_launch_name,
              U32(kMenuSelection), Frame());
}

inline void LaunchTick(PPCContext& ctx, uint8_t* base) {
  if (g_launch_state == LaunchState::kDone || g_launch_state == LaunchState::kFailed) return;
  if (g_launch_state == LaunchState::kOff) {
    const std::string name = REXCVAR_GET(wwe2k14_launch);
    if (name.empty()) return;
    g_launch_name = name;
    if (name == "@quick") {  // Session 27 item 8: build the match from our numbers (FireLaunch, build = true)
      g_launch_m.clear(), g_launch_g.clear(), g_launch_a.clear(), g_launch_x.clear(), g_launch_pblock.clear();
      if (REXCVAR_GET(wwe2k14_launch_participants).empty()) {
        REXLOG_WARN("[wwe2k14][scenario] quick: --wwe2k14_launch_participants is empty; not launching");
        g_launch_state = LaunchState::kFailed;
        return;
      }
      REXLOG_INFO("[wwe2k14][scenario] quick: type {} pace {} arena {} participants {}; waiting for the idle main menu",
                  REXCVAR_GET(wwe2k14_quick_type), REXCVAR_GET(wwe2k14_quick_pace), REXCVAR_GET(wwe2k14_launch_arena),
                  REXCVAR_GET(wwe2k14_launch_participants));
      g_launch_state = LaunchState::kWaiting;
      return;
    }
    g_launch_state = LoadScenario(name) ? LaunchState::kWaiting : LaunchState::kFailed;
    return;
  }
  const uint64_t frame = Frame();
  if (frame == g_launch_last_frame) return;  // once per guest frame
  g_launch_last_frame = frame;
  const uint32_t seq = SequenceId();
  if (g_launch_state == LaunchState::kWaiting) {
    const uint32_t sm = U32(kSmartMenuPtr);
    const bool idle = seq == kSeqMainMenu && sm && U32(sm + 364) == kMenuMain && U32(kMenuSelection) == 0;
    g_launch_idle = idle ? g_launch_idle + 1 : 0;
    if (g_launch_idle < uint64_t(std::max<int64_t>(1, REXCVAR_GET(wwe2k14_launch_idle_frames)))) return;
    FireLaunch(ctx, base, sm);
    return;
  }
  // kRequested: report how the game's own exit goes.
  if (seq != g_launch_last_seq) {
    REXLOG_INFO("[wwe2k14][scenario] launch '{}': sequence {} -> {} ({} frames after the request) frame={}",
                g_launch_name, g_launch_last_seq, seq, frame - g_launch_fired_frame, frame);
    g_launch_last_seq = seq;
  }
  if (seq == kSeqInGame) {
    REXLOG_INFO("[wwe2k14][scenario] launch '{}': in-game sequence started frame={}", g_launch_name, frame);
    g_launch_state = LaunchState::kDone;
  } else if (frame - g_launch_fired_frame == kLaunchWarnFrames) {
    REXLOG_WARN("[wwe2k14][scenario] launch '{}': no in-game sequence {} frames after the request (sequence {}, "
                "exit action {}, SmartMenu {:08X})",
                g_launch_name, kLaunchWarnFrames, seq, U32(kMenuSelection), U32(kSmartMenuPtr));
  }
}

}  // namespace wwe2k14::scenario

// ---------------------------------------------------------------------------------------------------------------
// Debug: on-demand memory snapshots (Session 9, finding where the match pace is stored). With
// --wwe2k14_snapshot_trigger=PATH set, every 30 guest frames the main thread looks for that file; when it exists, its
// first line is a NAME (letters, digits, _ and -), the file is deleted, and two snapshots are written:
//   logs/snap_NNN_NAME_D.bin  the image's static data 0x835C0000..0x83A60000 (4.8 MB)
//   logs/snap_NNN_NAME_H.bin  all of physical memory through its 64 KB-page view 0xA0000000..0xBFFFFFFF (512 MB)
// Raw big-endian guest bytes, local only (game memory: never commit). Copied in 64 KB chunks with a fault-safe copy
// (the same __try pattern as guest_watch.h); a chunk that faults is written as zeros and counted. The game stops for
// the time the 517 MB take to write (about a second). Default empty = off (one string read every 30 frames).
// ---------------------------------------------------------------------------------------------------------------
REXCVAR_DEFINE_STRING(wwe2k14_snapshot_trigger, "", "wwe2k14",
                      "Debug: path of a trigger file; when it appears, its first line names a snapshot of the static "
                      "data and all 512 MB of physical memory (logs/snap_NNN_NAME_D.bin / _H.bin), then it is deleted. "
                      "Empty = off. See src/scenario.h");
REXCVAR_DEFINE_STRING(wwe2k14_snapshot_now, "", "wwe2k14",
                      "Debug (Session 27.5): set to a NAME (e.g. `cvar wwe2k14_snapshot_now belt_a` in an input "
                      "script) to write the same two snapshots at the next per-frame update; cleared after. Empty = off");

namespace wwe2k14::scenario {

// No C++ objects with destructors in this function (required for __try).
inline bool SnapSafeCopy(void* dst, const void* src, size_t n) {
  static void* (*volatile copy)(void*, const void*, size_t) = std::memcpy;
  __try {
    copy(dst, src, n);
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_GUARD_PAGE
                  ? EXCEPTION_EXECUTE_HANDLER
                  : EXCEPTION_CONTINUE_SEARCH) {
    return false;
  }
  return true;
}

// Writes guest [start, end) to path in 64 KB chunks; returns the number of chunks that faulted (written as zeros),
// or -1 if the file could not be written.
inline int64_t SnapRange(const std::filesystem::path& path, uint32_t start, uint64_t end) {
  std::FILE* f = std::fopen(path.string().c_str(), "wb");
  if (!f) return -1;
  constexpr uint32_t kChunk = 64 * 1024;
  std::vector<uint8_t> buf(kChunk);
  int64_t faulted = 0;
  bool ok = true;
  for (uint64_t a = start; a < end && ok; a += kChunk) {
    const uint32_t n = uint32_t(std::min<uint64_t>(kChunk, end - a));
    const uint8_t* host = REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(uint32_t(a));
    if (!host || !SnapSafeCopy(buf.data(), host, n)) {
      std::memset(buf.data(), 0, n);
      ++faulted;
    }
    ok = std::fwrite(buf.data(), 1, n, f) == n;
  }
  ok = (std::fclose(f) == 0) && ok;
  return ok ? faulted : -1;
}

inline uint64_t g_snap_last_check = 0;

inline void SnapshotTick() {
  std::string name;
  const uint64_t frame = Frame();
  // Session 27.5: wwe2k14_snapshot_now (a script's `cvar wwe2k14_snapshot_now NAME`) takes it at this frame.
  // Read under the cvar registry lock (the script thread assigns it with SetFlagByName).
  if (std::string now = rex::cvar::Query<std::string>("wwe2k14_snapshot_now"); !now.empty()) {
    rex::cvar::SetFlagByName("wwe2k14_snapshot_now", "");
    name = now;
  } else {
    const std::string trigger = REXCVAR_GET(wwe2k14_snapshot_trigger);
    if (trigger.empty()) return;
    if (frame - g_snap_last_check < 30) return;
    g_snap_last_check = frame;
    std::error_code ec;
    if (!std::filesystem::exists(trigger, ec)) return;
    if (std::FILE* f = std::fopen(trigger.c_str(), "rb")) {
      char line[128] = {};
      if (std::fgets(line, sizeof(line), f)) name = line;
      std::fclose(f);
    }
    std::filesystem::remove(trigger, ec);
  }
  while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' ')) name.pop_back();
  if (!ValidName(name)) {
    REXLOG_WARN("[wwe2k14][snapshot] trigger name '{}' not used (letters, digits, _ and -, 1-64)", name);
    return;
  }
  const int run = CurrentLogNumber();
  const auto logs = std::filesystem::current_path() / "logs";
  const auto pd = logs / fmt::format("snap_{:03}_{}_D.bin", run >= 0 ? run : 0, name);
  const auto ph = logs / fmt::format("snap_{:03}_{}_H.bin", run >= 0 ? run : 0, name);
  const auto t0 = std::chrono::steady_clock::now();
  const int64_t fd = SnapRange(pd, kImageDataStart, kImageDataEnd);
  const int64_t fh = SnapRange(ph, 0xA0000000u, 0xC0000000ull);
  // Session 28: the 64 KB-page heap's low part (0x70000000..0x78000000: menu layout data, nodes and sprite names),
  // 128 MB, local only like the rest.
  {
    const auto pv = logs / fmt::format("snap_{:03}_{}_V.bin", run >= 0 ? run : 0, name);
    const int64_t fv = SnapRange(pv, 0x70000000u, 0x78000000ull);
    REXLOG_INFO("[wwe2k14][snapshot] '{}': {} ({} faulted chunks)", name, pv.filename().string(), fv);
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  REXLOG_INFO("[wwe2k14][snapshot] '{}' frame={}: {} ({} faulted chunks), {} ({} faulted chunks), {:.0f} ms", name,
              frame, pd.filename().string(), fd, ph.filename().string(), fh, ms);
}

}  // namespace wwe2k14::scenario

// Override of the per-frame owner update (weak alias of __imp__sub_82C302A0): the snapshot trigger and the
// launcher's tick, then the original body with its entry registers. With both settings empty this is two string
// reads (the trigger one every 30 frames) and a call.
REX_HOOK_RAW(sub_82C302A0) {
  wwe2k14::crowd::Tick();  // --wwe2k14_crowd_hype (Session 14, default off: one setting read)
  wwe2k14::scenario::SnapshotTick();
  wwe2k14::scenario::LaunchTick(ctx, base);
  __imp__sub_82C302A0(ctx, base);
}
