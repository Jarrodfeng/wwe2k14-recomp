// S111 EQUIV piece B: the test DLL's precompiled header (CMake target_precompile_headers; tools\equiv\build_equiv.ps1 -Fake builds it with
// clang -include-pch): the asset headers take ~2.8 s to parse, the tests themselves well under one. Copyright (c) the wwe2k14-recomp authors.
// BSD-3-Clause.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <stb_image.h>

#include "assets/arena_levels.h"
#include "assets/arena_props_table.h"
