// The build stamp as the case zip's manifest wants it (Session 101, lane 19; docs\systems\case_zip.md). The values come from src/build_stamp.cpp, the only
// file that includes <build>/stamp/wwe2k14_build_stamp.h (tools/build_stamp.cmake rewrites it on every build; CMakeLists.txt target wwe2k14_stamp).
// A build without it reports "unknown": the manifest then carries the reason and intake flags the zip as not from a stamped build.
// Nothing here is guessed at run time.
#pragma once
#include <string>
#include <string_view>

#include "case/case_manifest.h"

namespace wwe2k14::build_stamp {

struct Values {
  const char* git = "unknown";
  bool dirty = false;
  const char* sdk_pin = "unknown";
  int apk = 0;  // 0 = not an APK build
};
Values Get();  // build_stamp.cpp

inline void Fill(rdr_case::Manifest::Build& b) {
  const Values v = Get();
  b.git = v.git;
  b.dirty = v.dirty;
  b.sdk_pin = v.sdk_pin;
  if (v.apk) b.apk = v.apk;
  // The kind: release builds have no "+dev" in their version (CMakeLists.txt); the phone's builds are APKs.
  bool dev = false;
#ifdef WWE2K14_VERSION
  dev = std::string_view(WWE2K14_VERSION).find("+dev") != std::string_view::npos;
  b.version = WWE2K14_VERSION;
#endif
#if defined(__ANDROID__)
  b.kind = dev ? "apk-dev" : "apk";
#elif defined(WWE2K14_VERSION)
  b.kind = dev ? "dev" : "release";
#else
  b.kind = "unknown";
#endif
}

}  // namespace wwe2k14::build_stamp
