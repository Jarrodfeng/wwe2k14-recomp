// wwe2k14 - Windows compatibility for the Android port (Session 63, ANDROID_PLAN.md step 3.3).
//
// On Windows this is exactly #include <windows.h>: the PC build is unchanged. On Android it gives our headers the
// small part of Win32 they use in code that runs on the phone (types, timers, sleeping, memory queries, file opens,
// message boxes as log lines). Windows-only features (Media Foundation, dbghelp, the profilers, the launcher hand-off)
// stay behind #if defined(_WIN32) in their own headers instead of growing this file.
//
// Not emulated: structured exceptions. __try / __except become a plain block and a never-taken else, so a guest
// memory fault that the PC build would catch crashes the phone build instead (none of these paths run by default).

#pragma once

#if defined(_WIN32)

#include <windows.h>

#else  // Android (and any other POSIX target)

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string>

#include <rex/logging.h>

// -- types ------------------------------------------------------------------------------------------------------------
using BOOL = int;
using BYTE = uint8_t;
using WORD = uint16_t;
using DWORD = uint32_t;
using UINT = unsigned int;
using LONG = int32_t;
using ULONG = uint32_t;
using LONGLONG = int64_t;
using ULONGLONG = uint64_t;
using DWORD64 = uint64_t;
using DWORD_PTR = uintptr_t;
using ULONG_PTR = uintptr_t;
using SIZE_T = size_t;
using WCHAR = wchar_t;
using LPCWSTR = const wchar_t*;
using LPWSTR = wchar_t*;
using PCWSTR = const wchar_t*;
using LPVOID = void*;
using LPCVOID = const void*;
using HANDLE = void*;
using HMODULE = void*;
using HINSTANCE = void*;
using HWND = void*;
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(intptr_t(-1)))
#define WINAPI
#define CALLBACK

union LARGE_INTEGER {
  struct {
    DWORD LowPart;
    LONG HighPart;
  };
  LONGLONG QuadPart;
};

// -- time -------------------------------------------------------------------------------------------------------------
// The performance counter in nanoseconds (CLOCK_MONOTONIC): QueryPerformanceFrequency reports 1e9.
inline BOOL QueryPerformanceCounter(LARGE_INTEGER* out) {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  out->QuadPart = LONGLONG(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
  return TRUE;
}
inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* out) {
  out->QuadPart = 1000000000LL;
  return TRUE;
}
inline ULONGLONG GetTickCount64() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ULONGLONG(ts.tv_sec) * 1000ULL + ULONGLONG(ts.tv_nsec) / 1000000ULL;
}
inline DWORD GetTickCount() { return DWORD(GetTickCount64()); }
inline void Sleep(DWORD ms) {
  timespec ts{time_t(ms / 1000), long(ms % 1000) * 1000000L};
  while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
  }
}
inline BOOL SwitchToThread() { return sched_yield() == 0; }
#define YieldProcessor() __asm__ __volatile__("yield")
inline uint16_t _byteswap_ushort(uint16_t v) { return __builtin_bswap16(v); }
inline unsigned long _byteswap_ulong(unsigned long v) { return __builtin_bswap32(uint32_t(v)); }
inline uint64_t _byteswap_uint64(uint64_t v) { return __builtin_bswap64(v); }
using errno_t = int;
inline errno_t localtime_s(struct tm* out, const time_t* t) { return localtime_r(t, out) ? 0 : errno; }
inline errno_t gmtime_s(struct tm* out, const time_t* t) { return gmtime_r(t, out) ? 0 : errno; }

// -- waitable timers (precise_sleep.h, low_power.h) ------------------------------------------------------------------
// A handle that only knows its due time; WaitForSingleObject sleeps until it (CLOCK_MONOTONIC, absolute). Other handle
// kinds are not emulated.
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#define TIMER_ALL_ACCESS 0x1F0003
#define INFINITE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0u
#define WAIT_FAILED 0xFFFFFFFFu
namespace wwe2k14::compat {
struct WaitableTimer {
  uint32_t magic = 0x54494D52;  // 'TIMR'
  timespec due{};
};
}  // namespace wwe2k14::compat
inline HANDLE CreateWaitableTimerExW(void*, LPCWSTR, DWORD, DWORD) { return new wwe2k14::compat::WaitableTimer; }
// `due` in 100 ns units: negative = relative to now (the only form our code uses); positive (absolute) waits 0.
inline BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER* due, LONG, void*, void*, BOOL) {
  auto* t = static_cast<wwe2k14::compat::WaitableTimer*>(h);
  if (!t || t->magic != 0x54494D52) return FALSE;
  clock_gettime(CLOCK_MONOTONIC, &t->due);
  const int64_t rel_ns = due->QuadPart < 0 ? -due->QuadPart * 100 : 0;
  int64_t ns = int64_t(t->due.tv_nsec) + rel_ns;
  t->due.tv_sec += time_t(ns / 1000000000LL);
  t->due.tv_nsec = long(ns % 1000000000LL);
  return TRUE;
}
inline DWORD WaitForSingleObject(HANDLE h, DWORD) {
  auto* t = static_cast<wwe2k14::compat::WaitableTimer*>(h);
  if (!t || t->magic != 0x54494D52) return WAIT_FAILED;
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t->due, nullptr) == EINTR) {
  }
  return WAIT_OBJECT_0;
}
inline BOOL CloseHandle(HANDLE h) {
  auto* t = static_cast<wwe2k14::compat::WaitableTimer*>(h);
  if (t && t->magic == 0x54494D52) {
    t->magic = 0;
    delete t;
    return TRUE;
  }
  return FALSE;
}

// -- process / thread -------------------------------------------------------------------------------------------------
inline DWORD GetCurrentProcessId() { return DWORD(getpid()); }
inline DWORD GetCurrentThreadId() { return DWORD(gettid()); }
inline HANDLE GetCurrentThread() { return reinterpret_cast<HANDLE>(intptr_t(-2)); }
inline HANDLE GetCurrentProcess() { return reinterpret_cast<HANDLE>(intptr_t(-1)); }
inline DWORD GetLastError() { return DWORD(errno); }
[[noreturn]] inline void ExitProcess(UINT code) { std::_Exit(int(code)); }
// Thread names: the first 15 characters (the kernel's limit) on the calling thread only.
inline long SetThreadDescription(HANDLE thread, PCWSTR name) {
  if (thread != GetCurrentThread() || !name) return 0;
  std::string narrow = std::filesystem::path(name).string();
  if (narrow.size() > 15) narrow.resize(15);
  pthread_setname_np(pthread_self(), narrow.c_str());
  return 0;
}

// -- memory -----------------------------------------------------------------------------------------------------------
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOPY 0x08
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_GUARD 0x100
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_FREE 0x10000

struct MEMORY_BASIC_INFORMATION {
  void* BaseAddress;
  void* AllocationBase;
  DWORD AllocationProtect;
  SIZE_T RegionSize;
  DWORD State;
  DWORD Protect;
  DWORD Type;
};

namespace wwe2k14::compat {
inline DWORD ProtFromMapsFlags(const char* f) {
  const bool r = f[0] == 'r', w = f[1] == 'w', x = f[2] == 'x';
  if (!r && !w && !x) return PAGE_NOACCESS;
  if (x) return w ? PAGE_EXECUTE_READWRITE : (r ? PAGE_EXECUTE_READ : PAGE_EXECUTE);
  return w ? PAGE_READWRITE : PAGE_READONLY;
}
inline int ProtToPosix(DWORD p) {
  switch (p & 0xFF) {
    case PAGE_READONLY: return PROT_READ;
    case PAGE_READWRITE:
    case PAGE_WRITECOPY: return PROT_READ | PROT_WRITE;
    case PAGE_EXECUTE: return PROT_EXEC;
    case PAGE_EXECUTE_READ: return PROT_READ | PROT_EXEC;
    case PAGE_EXECUTE_READWRITE: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default: return PROT_NONE;
  }
}
}  // namespace wwe2k14::compat

// The mapping that holds `address`, from /proc/self/maps (slow: only for paths that are off by default). An unmapped
// address reports MEM_FREE.
inline SIZE_T VirtualQuery(LPCVOID address, MEMORY_BASIC_INFORMATION* info, SIZE_T length) {
  if (!info || length < sizeof(MEMORY_BASIC_INFORMATION)) return 0;
  std::memset(info, 0, sizeof(*info));
  const uintptr_t a = reinterpret_cast<uintptr_t>(address);
  FILE* maps = std::fopen("/proc/self/maps", "r");
  if (!maps) return 0;
  char line[512];
  uintptr_t prev_end = 0;
  bool found = false;
  while (std::fgets(line, sizeof(line), maps)) {
    unsigned long long lo = 0, hi = 0;
    char flags[5] = {};
    if (std::sscanf(line, "%llx-%llx %4s", &lo, &hi, flags) != 3) continue;
    if (a < lo) {  // in the gap before this mapping
      info->BaseAddress = reinterpret_cast<void*>(prev_end);
      info->RegionSize = SIZE_T(lo - prev_end);
      info->State = MEM_FREE;
      info->Protect = PAGE_NOACCESS;
      found = true;
      break;
    }
    if (a < hi) {
      info->BaseAddress = reinterpret_cast<void*>(uintptr_t(lo));
      info->AllocationBase = info->BaseAddress;
      info->RegionSize = SIZE_T(hi - lo);
      info->State = MEM_COMMIT;
      info->Protect = wwe2k14::compat::ProtFromMapsFlags(flags);
      info->AllocationProtect = info->Protect;
      found = true;
      break;
    }
    prev_end = uintptr_t(hi);
  }
  std::fclose(maps);
  if (!found) {
    info->BaseAddress = reinterpret_cast<void*>(prev_end);
    info->State = MEM_FREE;
    info->Protect = PAGE_NOACCESS;
  }
  return sizeof(MEMORY_BASIC_INFORMATION);
}

// mprotect on the host pages that cover [address, address + size). The old protection is read from /proc/self/maps.
inline BOOL VirtualProtect(LPVOID address, SIZE_T size, DWORD new_protect, DWORD* old_protect) {
  const uintptr_t page = uintptr_t(sysconf(_SC_PAGESIZE));
  const uintptr_t begin = reinterpret_cast<uintptr_t>(address) & ~(page - 1);
  const uintptr_t end = (reinterpret_cast<uintptr_t>(address) + size + page - 1) & ~(page - 1);
  if (old_protect) {
    MEMORY_BASIC_INFORMATION mbi;
    *old_protect = VirtualQuery(address, &mbi, sizeof(mbi)) ? mbi.Protect : PAGE_NOACCESS;
  }
  return mprotect(reinterpret_cast<void*>(begin), end - begin, wwe2k14::compat::ProtToPosix(new_protect)) == 0;
}

// -- structured exceptions (not emulated: see the file comment) -------------------------------------------------------
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_SEARCH 0
#define __try if (true)
#define __except(filter) else
inline DWORD GetExceptionCode() { return 0; }

// -- files ------------------------------------------------------------------------------------------------------------
#define _SH_DENYNO 0x40
#define _SH_DENYWR 0x20
inline FILE* _wfopen(const wchar_t* path, const wchar_t* mode) {
  return std::fopen(std::filesystem::path(path).string().c_str(), std::filesystem::path(mode).string().c_str());
}
inline FILE* _wfsopen(const wchar_t* path, const wchar_t* mode, int) { return _wfopen(path, mode); }
inline FILE* _fsopen(const char* path, const char* mode, int) { return std::fopen(path, mode); }
inline int _fseeki64(FILE* f, int64_t offset, int origin) { return fseeko(f, off_t(offset), origin); }
inline int64_t _ftelli64(FILE* f) { return int64_t(ftello(f)); }

inline FILE* _wfopen(const char* path, const wchar_t* mode) {  // fs::path::c_str() is char here
  return std::fopen(path, std::filesystem::path(mode).string().c_str());
}

// -- COM-style results ------------------------------------------------------------------------------------------------
using HRESULT = int32_t;
#define S_OK HRESULT(0)
#define E_FAIL HRESULT(0x80004005)
#define SUCCEEDED(hr) (HRESULT(hr) >= 0)
#define FAILED(hr) (HRESULT(hr) < 0)

// -- the app's own folder: "next to the exe" and the known folders ----------------------------------------------------
// The Java activity names the app's files folder in WWE2K14_FILES_DIR (the SDK's SDL_main makes it the working
// folder). Files the PC build looks for next to wwe2k14.exe (shader_list.txt, settings) and the PC's Documents /
// Pictures / AppData folders all map to it, so GetModuleFileNameW reports a virtual "wwe2k14" file in it.
namespace wwe2k14::compat {
inline std::filesystem::path AppFolder() {
  if (const char* d = std::getenv("WWE2K14_FILES_DIR"); d && *d) return d;
  std::error_code ec;
  return std::filesystem::current_path(ec);
}
}  // namespace wwe2k14::compat
inline DWORD GetModuleFileNameW(HMODULE, wchar_t* buf, DWORD size) {
  const std::wstring p = (wwe2k14::compat::AppFolder() / "wwe2k14").wstring();
  if (!buf || !size) return 0;
  const size_t n = std::min<size_t>(p.size(), size - 1);
  std::wmemcpy(buf, p.data(), n);
  buf[n] = 0;
  return DWORD(n);
}
struct KNOWNFOLDERID {
  int id;
};
inline constexpr KNOWNFOLDERID FOLDERID_RoamingAppData{1}, FOLDERID_LocalAppData{2}, FOLDERID_Documents{3},
    FOLDERID_Pictures{4}, FOLDERID_Videos{5};
using PWSTR = wchar_t*;
inline HRESULT SHGetKnownFolderPath(const KNOWNFOLDERID&, DWORD, HANDLE, PWSTR* out) {
  const std::wstring p = wwe2k14::compat::AppFolder().wstring();
  *out = static_cast<wchar_t*>(std::malloc((p.size() + 1) * sizeof(wchar_t)));
  if (!*out) return E_FAIL;
  std::wmemcpy(*out, p.c_str(), p.size() + 1);
  return S_OK;
}
inline void CoTaskMemFree(void* p) { std::free(p); }

// -- dynamic libraries (narrow names; a Windows DLL name simply fails to load) ----------------------------------------
inline HMODULE LoadLibraryW(LPCWSTR name) { return dlopen(std::filesystem::path(name).string().c_str(), RTLD_NOW); }
inline void* GetProcAddress(HMODULE m, const char* name) { return m ? dlsym(m, name) : nullptr; }
inline BOOL FreeLibrary(HMODULE m) { return m && dlclose(m) == 0; }

// -- virtual-key codes (keyboard_typing.h; the values of winuser.h) ---------------------------------------------------
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_NUMPAD0 0x60
#define VK_MULTIPLY 0x6A
#define VK_ADD 0x6B
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_DIVIDE 0x6F
#define VK_F1 0x70
#define VK_OEM_1 0xBA
#define VK_OEM_PLUS 0xBB
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2 0xBF
#define VK_OEM_3 0xC0
#define VK_OEM_4 0xDB
#define VK_OEM_5 0xDC
#define VK_OEM_6 0xDD
#define VK_OEM_7 0xDE
#define VK_OEM_8 0xDF
#define VK_OEM_102 0xE2

// -- GUIDs: only as tags (the picture container choice in input_script.h / clip_capture.h) ----------------------------
struct GUID {
  uint32_t Data1;
  uint16_t Data2, Data3;
  uint8_t Data4[8];
};
inline bool operator==(const GUID& a, const GUID& b) { return std::memcmp(&a, &b, sizeof(GUID)) == 0; }
inline constexpr GUID GUID_ContainerFormatPng{0x1b7cfaf4, 0x713f, 0x473c, {0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf}};
inline constexpr GUID GUID_ContainerFormatJpeg{0x19e4a5aa, 0x5662, 0x4fc5, {0xa0, 0xc0, 0x17, 0x58, 0x02, 0x8e, 0x10, 0x57}};

// -- message boxes: logged (there is no desktop to show them on) ------------------------------------------------------
#define MB_OK 0x0
#define MB_OKCANCEL 0x1
#define MB_YESNO 0x4
#define MB_ICONERROR 0x10
#define MB_ICONQUESTION 0x20
#define MB_ICONWARNING 0x30
#define MB_ICONINFORMATION 0x40
#define MB_TOPMOST 0x40000
#define MB_SETFOREGROUND 0x10000
#define IDOK 1
#define IDCANCEL 2
#define IDYES 6
#define IDNO 7
inline int MessageBoxW(HWND, LPCWSTR text, LPCWSTR caption, UINT) {
  REXLOG_WARN("[wwe2k14] message box '{}': {}", caption ? std::filesystem::path(caption).string() : "",
              text ? std::filesystem::path(text).string() : "");
  return IDOK;
}

#endif  // _WIN32
