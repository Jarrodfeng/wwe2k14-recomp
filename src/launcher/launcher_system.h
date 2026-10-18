// wwe2k14 launcher - system helpers (Session 25 item 2). No UI code here.
//
//   Hardware basics (GPU, monitor, CPU, RAM, Windows) for the suggested settings and the problem report: names and
//   numbers only, nothing personal (no user name, no paths outside the game's own folders).
//   CheckGameData: default.xex against the one disc this project supports (SHA-256 + the version in its header).
//   ZipWriter: a stored (uncompressed) .zip, for "Report a problem".
//   GameProcess: the game started by the launcher, so an abnormal exit can be noticed.

#pragma once

#include <bcrypt.h>
#include <dxgi.h>
#include <tlhelp32.h>

#include "launcher_core.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")

namespace launcher {

#ifndef WWE2K14_VERSION
#define WWE2K14_VERSION "unknown"
#endif
inline const char* Version() { return WWE2K14_VERSION; }
// Playtest builds turn the performance report on by default (SESSION25_PLAN item 2).
inline bool IsPlaytest() { return std::string(Version()).find("playtest") != std::string::npos; }

// ---------------------------------------------------------------------------------------------------------------
// Hardware

struct Hardware {
  std::string gpu;          // first hardware adapter's name
  uint32_t gpu_vendor = 0;  // 0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel
  uint64_t gpu_vram_mb = 0;
  bool rtx = false;         // an NVIDIA RTX card (DLSS / DLAA)
  int screen_w = 0, screen_h = 0, refresh_hz = 0;  // the primary monitor's current mode
  std::string cpu, windows;
  double ram_gb = 0;
};

inline std::string RegString(HKEY root, const wchar_t* key, const wchar_t* value) {
  wchar_t buf[256];
  DWORD size = sizeof(buf);
  if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
  return Narrow(buf);
}

// `window`: the monitor it is on is the one measured (a PC can have several, some virtual; Session 25: the primary was
// a 1280x720 virtual display while the RTX card drove a 3440x1440 165 Hz monitor).
inline Hardware DetectHardware(HWND window = nullptr) {
  Hardware h;
  IDXGIFactory1* f = nullptr;
  if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f)))) {
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
      DXGI_ADAPTER_DESC1 d{};
      a->GetDesc1(&d);
      a->Release();
      if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
      // Prefer a discrete card (the most dedicated memory) when there are several.
      if (h.gpu.empty() || d.DedicatedVideoMemory / (1024 * 1024) > h.gpu_vram_mb) {
        h.gpu = Narrow(d.Description), h.gpu_vendor = d.VendorId, h.gpu_vram_mb = d.DedicatedVideoMemory / (1024 * 1024);
      }
    }
    f->Release();
  }
  h.rtx = h.gpu_vendor == 0x10DE && h.gpu.find("RTX") != std::string::npos;
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  const wchar_t* device = nullptr;
  if (window && GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &mi)) device = mi.szDevice;
  if (EnumDisplaySettingsW(device, ENUM_CURRENT_SETTINGS, &dm))
    h.screen_w = int(dm.dmPelsWidth), h.screen_h = int(dm.dmPelsHeight), h.refresh_hz = int(dm.dmDisplayFrequency);
  h.cpu = RegString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
  while (!h.cpu.empty() && h.cpu.back() == ' ') h.cpu.pop_back();
  MEMORYSTATUSEX m{sizeof(m)};
  if (GlobalMemoryStatusEx(&m)) h.ram_gb = double(m.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
  const wchar_t* cv = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
  std::string product = RegString(HKEY_LOCAL_MACHINE, cv, L"ProductName");
  const std::string build = RegString(HKEY_LOCAL_MACHINE, cv, L"CurrentBuild");
  // Windows 11 still reports "Windows 10" in ProductName: build 22000 and later is Windows 11.
  if (!build.empty() && std::atoi(build.c_str()) >= 22000 && product.rfind("Windows 10", 0) == 0)
    product.replace(0, 10, "Windows 11");
  h.windows = product + " (build " + build + ", " + RegString(HKEY_LOCAL_MACHINE, cv, L"DisplayVersion") + ")";
  return h;
}

// Session 31: every hardware graphics adapter (for the Settings chooser), the discrete one with the most dedicated
// memory first (the default: a laptop's Intel / AMD integrated GPU is never picked by itself).
struct Gpu {
  std::string name;
  uint32_t vendor = 0;
  uint64_t vram_mb = 0;
};
inline std::vector<Gpu> ListGpus() {
  std::vector<Gpu> out;
  IDXGIFactory1* f = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f)))) return out;
  IDXGIAdapter1* a = nullptr;
  for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 d{};
    a->GetDesc1(&d);
    a->Release();
    if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
    Gpu g{Narrow(d.Description), d.VendorId, d.DedicatedVideoMemory / (1024 * 1024)};
    if (std::none_of(out.begin(), out.end(), [&](const Gpu& o) { return o.name == g.name; })) out.push_back(g);
  }
  f->Release();
  std::stable_sort(out.begin(), out.end(), [](const Gpu& x, const Gpu& y) { return x.vram_mb > y.vram_mb; });
  return out;
}

inline std::string HardwareText(const Hardware& h) {
  char b[512];
  std::snprintf(b, sizeof(b),
                "GPU: %s (%llu MB)\nMonitor: %dx%d at %d Hz\nCPU: %s\nRAM: %.0f GB\nWindows: %s\n", h.gpu.c_str(),
                static_cast<unsigned long long>(h.gpu_vram_mb), h.screen_w, h.screen_h, h.refresh_hz, h.cpu.c_str(),
                h.ram_gb, h.windows.c_str());
  return b;
}

// Suggested settings from the hardware (first run). Resolution: 0 720p, 1 1440p, 2 4K.
inline int SuggestResolution(const Hardware& h) {
  if (h.screen_h >= 2160) return 2;
  if (h.screen_h >= 1080) return 1;  // 1080p monitors: 1440p, downscaled (sharper than 720p)
  return 0;
}
// Frame rate: 0 classic, 1 60 everywhere, 2 high refresh (a monitor above 60 Hz).
inline int SuggestFrameRate(const Hardware& h) { return h.refresh_hz > 75 ? 2 : 1; }

// ---------------------------------------------------------------------------------------------------------------
// Game data check: the one supported disc (SESSION25_PLAN: default.xex 0.0.0.2, no title update).

inline constexpr const char* kExpectedXexSha256 = "544EF8B505AEBCECE3459865A4E27E1A7CFAD568406514F27E1115B378D3A8FB";
inline constexpr uint32_t kExpectedTitleId = 0x545408B2;

// SHA-256 of a file in the game files (a folder or a disc image, Session 26); empty if it can't be read.
inline std::string Sha256GameFile(GameFiles& files, const std::string& rel) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
  std::string out;
  if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
    const bool read = files.Stream(rel, [&](const uint8_t* p, size_t n) {
      BCryptHashData(hash, const_cast<PUCHAR>(p), ULONG(n), 0);
    });
    unsigned char d[32];
    if (BCryptFinishHash(hash, d, sizeof(d), 0) == 0 && read) {
      char hex[65];
      for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02X", d[i]);
      out = hex;
    }
    BCryptDestroyHash(hash);
  }
  BCryptCloseAlgorithmProvider(alg, 0);
  return out;
}

struct GameDataCheck {
  bool ok = false;           // the expected default.xex
  bool found = false;        // a default.xex is there at all
  std::string message;       // plain English, for the player
  std::string version;       // from the XEX header, e.g. "0.0.0.2"
  uint32_t title_id = 0;
};

// `root`: a folder or (Session 26) a disc image file.
inline GameDataCheck CheckGameData(const fs::path& root) {
  GameDataCheck c;
  GameFiles& files = GameFilesAt(root);
  files.Reopen();  // a check reads the files as they are now
  if (root.empty()) {
    c.message = "Pick your game files: the folder with default.xex in it, or a disc image (.iso) of your disc.";
    return c;
  }
  if (files.is_image() && !files.image_error().empty()) {
    c.message = "Can't use this file: " + files.image_error() + ".";
    return c;
  }
  if (!files.Exists("default.xex")) {
    c.message = files.is_image() ? "This disc image has no default.xex: it isn't a game disc."
                                 : "No default.xex in this folder. Pick the folder you copied from your disc (it has "
                                   "default.xex, a pac folder and a movies folder), or a disc image (.iso).";
    return c;
  }
  c.found = true;
  // XEX2 header: optional header 0x00040006 (execution info) = media id, version, base version, title id.
  const std::vector<uint8_t> head = files.ReadHead("default.xex", 0x10000);
  if (head.size() >= 24 && std::memcmp(head.data(), "XEX2", 4) == 0) {
    const uint32_t count = Be32(&head[20]);
    for (uint32_t i = 0; i < count && 24 + i * 8 + 8 <= head.size(); ++i) {
      if (Be32(&head[24 + i * 8]) != 0x00040006) continue;
      const uint32_t off = Be32(&head[24 + i * 8 + 4]);
      if (off + 16 > head.size()) break;
      const uint32_t v = Be32(&head[off + 4]);
      c.title_id = Be32(&head[off + 12]);
      c.version = std::to_string(v >> 28) + "." + std::to_string((v >> 24) & 0xF) + "." +
                  std::to_string((v >> 8) & 0xFFFF) + "." + std::to_string(v & 0xFF);
    }
  }
  const std::string sha = Sha256GameFile(files, "default.xex");
  if (sha == kExpectedXexSha256) {
    c.ok = true;
    c.message = std::string(files.is_image() ? "Your disc image matches" : "Your game files match") +
                " the supported disc (WWE 2K14, version 0.0.0.2).";
    // Session 26: the game ignores a title update next to default.xex (xex_apply_patch, src\wwe2k14_app.h).
    if (files.Exists("default.xexp"))
      c.message += " A title update (default.xexp) is there too: the game ignores it and plays the disc version.";
    return c;
  }
  char tid[16];
  std::snprintf(tid, sizeof(tid), "%08X", c.title_id);
  if (c.version.empty()) {
    c.message = "default.xex is not a readable Xbox 360 game file. Copy it again from your disc.";
  } else if (c.title_id != kExpectedTitleId) {
    c.message = "This default.xex is a different game (title id " + std::string(tid) +
                "). WWE 2K14 is 545408B2.";
  } else {
    c.message = "Found WWE 2K14 version " + c.version + ", but this build needs the disc version 0.0.0.2 with no "
                "title update (SHA-256 " + std::string(kExpectedXexSha256).substr(0, 16) + "...). ";
    c.message += c.version == "0.0.0.2" ? "Same version number, different file: it may be modified or damaged. "
                                          "Copy it again from your disc."
                                        : "A title update or another edition changes default.xex; use the files "
                                          "straight from the disc.";
  }
  return c;
}

// ---------------------------------------------------------------------------------------------------------------
// A .zip with stored (uncompressed) entries: enough for a few log files.

class ZipWriter {
 public:
  explicit ZipWriter(const fs::path& p) : f_(p, std::ios::binary | std::ios::trunc) {}
  bool ok() const { return bool(f_); }
  void Add(const std::string& name, const std::string& data) {
    Entry e{name, Crc(data), uint32_t(data.size()), uint32_t(f_.tellp())};
    Header(0x04034b50, e, false);
    f_.write(data.data(), std::streamsize(data.size()));
    entries_.push_back(e);
  }
  bool Close() {
    const uint32_t dir = uint32_t(f_.tellp());
    for (const auto& e : entries_) Header(0x02014b50, e, true);
    const uint32_t size = uint32_t(f_.tellp()) - dir;
    W32(0x06054b50), W16(0), W16(0), W16(uint16_t(entries_.size())), W16(uint16_t(entries_.size()));
    W32(size), W32(dir), W16(0);
    f_.close();
    return !f_.fail();
  }

 private:
  struct Entry {
    std::string name;
    uint32_t crc, size, offset;
  };
  static uint32_t Crc(const std::string& s) {
    static uint32_t t[256];
    static bool init = [] {
      for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[i] = c;
      }
      return true;
    }();
    (void)init;
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char ch : s) c = t[(c ^ ch) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
  }
  void W16(uint16_t v) { f_.put(char(v & 0xFF)), f_.put(char(v >> 8)); }
  void W32(uint32_t v) { W16(uint16_t(v & 0xFFFF)), W16(uint16_t(v >> 16)); }
  void Header(uint32_t sig, const Entry& e, bool central) {
    W32(sig);
    if (central) W16(20);  // made by
    W16(20), W16(0x0800), W16(0), W16(0), W16(0x21);  // needed, flags (UTF-8 names), stored, time, date (1980-01-01)
    W32(e.crc), W32(e.size), W32(e.size), W16(uint16_t(e.name.size())), W16(0);
    if (central) W16(0), W16(0), W16(0), W32(0), W32(e.offset);
    f_.write(e.name.data(), std::streamsize(e.name.size()));
  }
  std::ofstream f_;
  std::vector<Entry> entries_;
};

// ---------------------------------------------------------------------------------------------------------------
// The game process

// Starts the command and keeps the process handle (the caller closes it). Returns "" or the reason.
inline std::string StartProcess(const Command& c, HANDLE* proc) {
  if (!c.error.empty()) return c.error;
  std::wstring line = L"\"" + c.exe + L"\" " + c.args;
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, c.console ? CREATE_NEW_CONSOLE : 0, nullptr,
                      c.workdir.empty() ? nullptr : c.workdir.c_str(), &si, &pi))
    return "could not start (error " + std::to_string(GetLastError()) + ")";
  CloseHandle(pi.hThread);
  if (proc) *proc = pi.hProcess;
  else CloseHandle(pi.hProcess);
  return "";
}

// Any wwe2k14.exe running (the launcher's or one the player started).
inline bool GameRunning() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W e{sizeof(e)};
  bool found = false;
  for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e))
    found = _wcsicmp(e.szExeFile, L"wwe2k14.exe") == 0;
  CloseHandle(snap);
  return found;
}

// The game's exit code after an error it reported itself: ReXGlue returns EXIT_FAILURE when the start fails (after
// its message box: game files not found, a refused disc image, a XEX that doesn't load). Not a crash.
constexpr DWORD kGameReportedErrorExitCode = 1;

// Plain-English name for an exit code the game can end with.
inline std::string ExitCodeText(DWORD code) {
  char b[64];
  std::snprintf(b, sizeof(b), "0x%08lX", static_cast<unsigned long>(code));
  std::string s = b;
  if (code == kGameReportedErrorExitCode) s += " (an error the game reported itself)";
  else if (code == 0xC0000264) s += " (the known audio start-up crash: Try again usually fixes it)";
  else if (code == 0xC0000005) s += " (access violation)";
  else if (code == 0xC0000409) s += " (stack buffer overrun / fast fail)";
  else if (code == 0xE06D7363) s += " (C++ exception)";
  return s;
}

}  // namespace launcher
