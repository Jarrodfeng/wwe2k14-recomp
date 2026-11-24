// S106 G4-4: the STL headers every plain game unit shares. ONE header for two jobs:
//  - CMakeLists.txt force-includes it (-include) into every plain unit of the game target (not main.cpp, which has pch_main.h), Windows and Android;
//  - tools/compile/game_syntax.py precompiles THIS file as the compile check's PCH, so the check parses exactly what the build parses
//    (a unit that forgot a non-STL header fails in both; a unit that forgot only an STL header passes in both).
// STL only: no Windows headers (the G12 macro-order class: a field named `small` after windows.h needs each unit to see them in its own order),
// no project headers (a force-included project header would make every unit depend on it). Editing this file rebuilds every plain unit and the check's PCH.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
