// S103 L33F (review F1): see dev_build.h. CMakeLists.txt gives THIS file the WWE2K14_VERSION define (set_property SOURCE src/dev_build.cpp), so every translation unit asks here.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#include "dev_build.h"

#include <string_view>

namespace wwe2k14 {

const char* VersionString() {
#ifdef WWE2K14_VERSION
  return WWE2K14_VERSION;
#else
  return "";
#endif
}

bool IsDevBuild() { return std::string_view(VersionString()).find("+dev") != std::string_view::npos; }

}  // namespace wwe2k14
