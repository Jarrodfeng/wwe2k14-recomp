// S106 PSEED: where shader_list.txt is (the rule shader_list.h Import used inline, factored out cvar-free).
// entry: wwe2k14::shader_list::ListPath
// shader_list.h defines the cvar (REXCVAR_DEFINE_STRING: a non-inline symbol + a registrar), so a second unit cannot include it; the stream-free host (host_start.cpp)
// reads the cvar's value by name (rex::cvar::GetFlagByName) and calls this to get the same path the importer reads: the seed (R lines) and the importer share one file.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "platform/compat.h"  // GetModuleFileNameW

#include <filesystem>
#include <string>

namespace wwe2k14::shader_list {

// `setting` is the --wwe2k14_shader_list value: "off" = none (empty path), empty = shader_list.txt next to the exe, else that path. An empty result = no list.
inline std::filesystem::path ListPath(const std::string& setting) {
  if (setting == "off") return {};
  if (!setting.empty()) return std::filesystem::path(setting);
  wchar_t buf[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (!n || n >= MAX_PATH) return {};
  return std::filesystem::path(buf).parent_path() / "shader_list.txt";
}

}  // namespace wwe2k14::shader_list
