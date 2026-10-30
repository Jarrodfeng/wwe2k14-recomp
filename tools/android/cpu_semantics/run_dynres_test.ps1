# Session 73: src\dynamic_res.h's controller on made-up frame times (dynres_test.cpp), built on the PC with clang
# against shims of rex/cvar.h and rex/logging.h. Seconds; no game run.
#   pwsh -File tools\android\cpu_semantics\run_dynres_test.ps1
$ErrorActionPreference = 'Stop'
$R = Split-Path (Split-Path (Split-Path $PSScriptRoot))
. "$R\tools\env.ps1"
$d = Join-Path $R 'out\android\cpu_semantics\dynres'
New-Item -ItemType Directory -Force "$d\shim\rex" | Out-Null
Set-Content -Encoding ascii "$d\shim\rex\cvar.h" @'
#pragma once
// Session 73 test shim: just enough of rex/cvar.h for src/dynamic_res.h and src/frame_word.h.
#include <map>
#include <string>
namespace rex::cvar {
inline std::map<std::string, std::string>& Flags() { static std::map<std::string, std::string> m; return m; }
inline std::string GetFlagByName(const std::string& n) { auto it = Flags().find(n); return it == Flags().end() ? "" : it->second; }
inline void SetFlagByName(const std::string& n, const std::string& v) { Flags()[n] = v; }
}
struct ShimRange { ShimRange range(int, int) const { return *this; } };
#define REXCVAR_DEFINE_BOOL(name, def, cat, desc) inline bool cvar_##name = def
#define REXCVAR_DEFINE_INT32(name, def, cat, desc) inline int cvar_##name = def; inline ShimRange shim_##name = ShimRange{}
#define REXCVAR_DEFINE_DOUBLE(name, def, cat, desc) inline double cvar_##name = def
#define REXCVAR_GET(name) cvar_##name
'@
Set-Content -Encoding ascii "$d\shim\rex\logging.h" @'
#pragma once
#include <fmt/format.h>
#include <cstdio>
#define REXLOG_INFO(...) std::printf("  log: %s\n", fmt::format(__VA_ARGS__).c_str())
'@
$fmt = Join-Path $R 'thirdparty\rexglue-sdk-src\thirdparty\fmt\include'
& clang++ -std=c++20 -O1 -DFMT_HEADER_ONLY -I "$d\shim" -I $fmt (Join-Path $PSScriptRoot 'dynres_test.cpp') -o "$d\dynres_test.exe"
if ($LASTEXITCODE) { throw 'build failed' }
& "$d\dynres_test.exe"
exit $LASTEXITCODE
