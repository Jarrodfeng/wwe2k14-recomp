// S102 lane H1: name every thread WE create, so the Surface's thread log (tools\perf\thread_busy.ps1, the [perf] sampler, a debugger) says which of
// our threads burn CPU instead of "ucrtbase+offset". Call NameThread(L"...") as the FIRST statement of a thread's body; it names the calling thread.
// Windows: SetThreadDescription (UTF-16, no length limit that matters). Android / Linux: pthread_setname_np, the first 15 characters (the kernel's
// limit), so keep names short and distinct in their first 15 characters ("rdr_..." / "ours_..."). A name costs nothing and changes nothing.
// tools\lint\thread_names.py fails any std::thread / CreateThread in src\ that has no NameThread( in its body (planted twin in its selftest).
// Self-contained (no windows.h, no other project header), so the standalone renderer tools can include it.
#pragma once
#include <cstdint>
#include <cwchar>
#include <string>
#if defined(_WIN32)
// NOT <windows.h>: it defines the macros near / far / small / interface, which break other headers of ours that this one is included before.
// kernel32's own symbol, declared under another name (the pseudo-handle -2 is GetCurrentThread()).
extern "C" long __stdcall wwe2k14_h1_SetThreadDescription(void* thread, const wchar_t* name) __asm__("SetThreadDescription");
#else
#include <pthread.h>
#endif

namespace wwe2k14 {

inline void NameThread(const wchar_t* name) {
  if (!name || !*name) return;
#if defined(_WIN32)
  wwe2k14_h1_SetThreadDescription(reinterpret_cast<void*>(static_cast<intptr_t>(-2)), name);
#else
  std::string narrow;
  for (const wchar_t* p = name; *p && narrow.size() < 15; ++p) narrow.push_back(*p < 128 ? char(*p) : '_');
  pthread_setname_np(pthread_self(), narrow.c_str());
#endif
}

}  // namespace wwe2k14
