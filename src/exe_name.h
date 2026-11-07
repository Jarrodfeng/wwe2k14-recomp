// wwe2k14 - the running exe's file name, for crash lines and offsets (Session 95). wwe2k14.exe and wwe2k14_v3.exe have
// different layouts, so an offset is only useful with the right name beside it: wwe2k14_v3.exe+0x... resolves against
// wwe2k14_v3.map, never wwe2k14.map (tools\analysis\resolve_backtrace.py, q.py addr --host ... --map).

#pragma once

#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "platform/compat.h"  // <windows.h> on Windows
#endif

namespace wwe2k14 {

inline std::string ExeFileName() {
#ifdef _WIN32
  char path[MAX_PATH] = "wwe2k14.exe";
  GetModuleFileNameA(nullptr, path, MAX_PATH);
  const char* leaf = path;
  for (const char* p = path; *p; ++p)
    if (*p == '\\' || *p == '/') leaf = p + 1;
  return leaf;
#else
  return "wwe2k14";
#endif
}

}  // namespace wwe2k14
