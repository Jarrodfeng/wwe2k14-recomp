// S103 L33F (review F1): the one answer to "is this a +dev build?" for every translation unit. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// CMakeLists.txt defines WWE2K14_VERSION for single source files only (a target-wide define would rebuild the generated code on every version change): main.cpp and src/dev_build.cpp.
// Any other file that tests `#ifdef WWE2K14_VERSION` sees it undefined in EVERY build (live.cpp did: its "dev only" switches were honoured by player release builds). Call these instead;
// tools/lint/version_define_lint.py fails a game TU that reads the macro without the define, and its --release-check proves IsDevBuild() for release / +dev / no define.
#pragma once

namespace wwe2k14 {
// The version string CMakeLists.txt built (VERSION + suffix, "+dev" unless -DWWE2K14_RELEASE=ON); "" when the build had none (fail closed: not dev).
const char* VersionString();
// true only when the version carries "+dev". A build with no version string is NOT dev: a missing define must never switch dev-only behaviour on.
bool IsDevBuild();
}  // namespace wwe2k14
