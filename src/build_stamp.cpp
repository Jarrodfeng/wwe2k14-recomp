// The only file that includes the generated build stamp (Session 101, lane 19 review fix 7): a new commit or a dirty flip recompiles just this file,
// not main.cpp. <build>/stamp/wwe2k14_build_stamp.h is written by tools/build_stamp.cmake on every build, only when its text changed. A build without
// it (a syntax check, an old worktree) reports "unknown" and the manifest says why.
#include "build_stamp.h"

#if defined(__has_include)
#if __has_include("wwe2k14_build_stamp.h")
#include "wwe2k14_build_stamp.h"
#endif
#endif

namespace wwe2k14::build_stamp {

Values Get() {
  Values v;
#ifdef WWE2K14_BUILD_GIT
  v.git = WWE2K14_BUILD_GIT;
  v.dirty = WWE2K14_BUILD_DIRTY != 0;
#endif
#ifdef WWE2K14_BUILD_SDK_PIN
  v.sdk_pin = WWE2K14_BUILD_SDK_PIN;
#endif
#ifdef WWE2K14_BUILD_APK
  v.apk = WWE2K14_BUILD_APK;
#endif
  return v;
}

}  // namespace wwe2k14::build_stamp
