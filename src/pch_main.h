// wwe2k14 - precompiled header for main.cpp (Session 30, #9 faster incremental builds).
//
// main.cpp compiles all our headers as one translation unit (~37 s per build before Session 30). This header holds
// what every build parses again but never changes between our edits: the generated declarations (wwe2k14_init.h,
// 2.7 MB of guest function declarations), the SDK, the C++ library, Windows, fmt, imgui. CMake builds it once
// (target_precompile_headers in CMakeLists.txt, with PCH_INSTANTIATE_TEMPLATES: templates used by these headers are
// instantiated in the PCH): main.cpp 37 s -> 24 s. Our own headers (src\*.h) must NOT go here: a change to one would
// rebuild the PCH too. The list = every <...> include of src\*.h (collected 2026-10-18); a system header missing here
// is simply parsed in main.cpp as before. Only NOMINMAX / WIN32_LEAN_AND_MEAN are defined before these in our
// headers, and the build defines both globally, so the PCH sees the same headers main.cpp saw.

#pragma once

#include "generated/default/wwe2k14_init.h"
#include "platform/compat.h"  // Session 63: <windows.h> on Windows (src/platform/compat.h)
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <charconv>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#if defined(_WIN32)  // Session 63 (Android port): Windows / x86 only
#include <dbghelp.h>
#endif
#include <deque>
#if defined(_WIN32)  // Session 63 (Android port): Windows / x86 only
#include <dxgi.h>
#endif
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <functional>
#include <future>
#include <imgui.h>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#if defined(_WIN32)  // Session 63 (Android port): Windows / x86 only
#include <mfapi.h>
#endif
#if defined(_WIN32)  // Session 63 (Android port): Windows / x86 only
#include <mferror.h>
#endif
#if defined(_WIN32)  // Session 63 (Android port): Windows / x86 only
#include <mfidl.h>
#endif
#if defined(_WIN32)  // Session 63 (Android port): Windows / x86 only
#include <mfreadwrite.h>
#endif
#include <mutex>
#include <new>
#if defined(_WIN32)  // Session 63 (Android port): Windows / x86 only
#include <pdh.h>
#endif
#if defined(_WIN32)  // Session 63 (Android port): Windows / x86 only
#include <pdhmsg.h>
#endif
#if defined(_WIN32)  // Session 63 (Android port): Windows / x86 only
#include <psapi.h>
#endif
#include <regex>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/det.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>
#include <rex/hook.h>
#include <rex/image_info.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/perf/audio_tap.h>
#include <rex/perf/dlaa.h>
#include <rex/perf/flight.h>
#include <rex/perf/timeline.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xio.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#include <rex/thread/mutex.h>
#include <rex/types.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/presenter.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/window.h>
#include <set>
#if defined(_WIN32)  // Session 63 (Android port): Windows / x86 only
#include <share.h>
#endif
#if defined(_WIN32)  // Session 63 (Android port): Windows / x86 only
#include <shlobj.h>
#endif
#include <span>
#include <sstream>
#include <string_view>
#include <string>
#include <system_error>
#include <thread>
#if defined(_WIN32)  // Session 63 (Android port): Windows / x86 only
#include <tlhelp32.h>
#endif
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#if defined(_WIN32)  // Session 63 (Android port): Windows / x86 only
#include <wincodec.h>
#endif
#if defined(_WIN32)  // Session 63 (Android port): Windows / x86 only
#include <xmmintrin.h>
#endif
