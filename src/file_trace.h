// wwe2k14 - optional file-open / file-read backtraces (Session 8 task 7, file loading system map).
//
// --wwe2k14_file_trace=true logs, for every successful NtCreateFile/NtOpenFile whose path contains
// --wwe2k14_file_trace_filter (case-insensitive; default ".PAC"), a host backtrace as exe offsets
// (resolve with py tools\q\q.py addr --host wwe2k14.exe+0x... or tools\analysis\resolve_backtrace.py),
// at most --wwe2k14_file_trace_per_path times per distinct path. The first --wwe2k14_file_trace_reads
// NtReadFile calls on each traced handle are logged too (offset, length, guest buffer, event, APC routine
// and a backtrace). Lines start with "[wwe2k14][ftrace]".
//
// Default OFF: with the flag off the hooks only test one cvar and pass straight through.

#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xio.h>
#include <rex/system/xmemory.h>
#include <rex/system/xtypes.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "frame_stats.h"
#include "flight_recorder.h"  // NtReadFile events for --wwe2k14_flight_recorder (Session 8 task 11)

namespace rex::kernel::xboxkrnl {
// ReXGlue's implementation (xboxkrnl_io.cpp), exported by rexruntime.dll but not declared in the headers.
uint32_t NtReadFile_entry(uint32_t file_handle, uint32_t event_handle, mapped_void apc_routine_ptr,
                          mapped_void apc_context, ppc_ptr_t<rex::system::X_IO_STATUS_BLOCK> io_status_block,
                          mapped_void buffer, uint32_t buffer_length, mapped_u64 byte_offset_ptr);
}  // namespace rex::kernel::xboxkrnl

REXCVAR_DEFINE_BOOL(wwe2k14_file_trace, false, "wwe2k14",
                    "Log host backtraces of matching file opens and their first reads ([wwe2k14][ftrace])");
REXCVAR_DEFINE_STRING(wwe2k14_file_trace_filter, ".PAC", "wwe2k14",
                      "Substring (case-insensitive) a file path must contain to be traced");
REXCVAR_DEFINE_INT32(wwe2k14_file_trace_per_path, 2, "wwe2k14",
                     "Maximum traced opens per distinct path");
REXCVAR_DEFINE_INT32(wwe2k14_file_trace_reads, 4, "wwe2k14",
                     "Reads logged per traced handle (after each traced open)");
REXCVAR_DEFINE_STRING(wwe2k14_file_trace_scan, "", "wwe2k14",
                      "Path substring: on the first game read of a matching traced file, scan guest memory "
                      "once for the read's offset / sector / length (empty = never)");

namespace wwe2k14 {

struct FileTraceState {
  std::mutex mutex;
  std::unordered_map<std::string, int> opens_per_path;
  struct Handle {
    std::string path;
    int reads_left = 0;
  };
  std::unordered_map<uint32_t, Handle> handles;
};

inline FileTraceState& GetFileTraceState() {
  static FileTraceState state;
  return state;
}

inline std::string FileTraceUpper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::toupper(c)); });
  return s;
}

// Host backtrace as "+0xOFFSET" exe offsets (frames outside the exe are shown as "?").
inline std::string FileTraceBacktrace() {
#ifdef _WIN32
  void* frames[28] = {};
  const USHORT count = RtlCaptureStackBackTrace(2, 28, frames, nullptr);
  const uintptr_t base = uintptr_t(GetModuleHandleW(nullptr));
  std::string out;
  for (USHORT i = 0; i < count; ++i) {
    const uintptr_t a = uintptr_t(frames[i]);
    if (!out.empty()) out += ' ';
    if (a >= base && a - base < 0x10000000) {
      out += fmt::format("+0x{:X}", a - base);
    } else {
      out += '?';
    }
  }
  return out;
#else
  return "";
#endif
}

inline void FileTraceOnOpen(const std::string& path, uint32_t result, uint32_t handle) {
  if (!REXCVAR_GET(wwe2k14_file_trace)) return;
  if (result != 0 || !handle) return;
  const std::string upper = FileTraceUpper(path);
  const std::string filter = FileTraceUpper(REXCVAR_GET(wwe2k14_file_trace_filter));
  if (!filter.empty() && upper.find(filter) == std::string::npos) return;
  auto& st = GetFileTraceState();
  {
    std::lock_guard<std::mutex> lock(st.mutex);
    int& n = st.opens_per_path[upper];
    if (n >= REXCVAR_GET(wwe2k14_file_trace_per_path)) {
      st.handles.erase(handle);
      return;
    }
    ++n;
    st.handles[handle] = FileTraceState::Handle{path, REXCVAR_GET(wwe2k14_file_trace_reads)};
  }
  REXLOG_INFO("[wwe2k14][ftrace] open '{}' handle=0x{:X} frame={} tid={} bt: {}", path, handle,
              g_guest_frame.load(), uint32_t(GetCurrentThreadId()), FileTraceBacktrace());
}

// One-shot guest memory scan (--wwe2k14_file_trace_scan=SUBSTRING, default empty = never). Searches every
// committed, readable page of the guest virtual range for the big-endian 32-bit values offset, offset/0x800
// and length, and logs up to 24 guest addresses per value plus the 8 words after each offset/sector hit.
inline void FileTraceScanOnce(const std::string& path, uint64_t offset, uint32_t length) {
#ifdef _WIN32
  const std::string want = FileTraceUpper(REXCVAR_GET(wwe2k14_file_trace_scan));
  if (want.empty() || FileTraceUpper(path).find(want) == std::string::npos) return;
  static std::mutex once_mutex;
  static std::unordered_map<std::string, bool> done;
  {
    std::lock_guard<std::mutex> lock(once_mutex);
    if (done[FileTraceUpper(path)]) return;
    done[FileTraceUpper(path)] = true;
  }
  auto* memory = REX_KERNEL_MEMORY();
  uint8_t* const base = memory->virtual_membase();
  // "sector-8": plist360.arc stores EPAC entry offsets in 0x800 sectors after an 8-sector (0x4000) header.
  const uint32_t values[4] = {uint32_t(offset), uint32_t(offset / 0x800), length, uint32_t(offset / 0x800) - 8};
  const char* names[4] = {"offset", "sector", "length", "sector-8"};
  auto be = [](uint32_t v) {
    return uint32_t((v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24));
  };
  std::string hits[4];
  int counts[4] = {0, 0, 0, 0};
  uint64_t scanned = 0;
  for (uint64_t g = 0; g < 0x100000000ull;) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(base + g, &mbi, sizeof(mbi))) break;
    const uint64_t region_start = uint64_t(reinterpret_cast<uint8_t*>(mbi.BaseAddress) - base);
    const uint64_t region_end = region_start + mbi.RegionSize;
    const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                          (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                                          PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY));
    if (readable) {
      const uint64_t end = std::min<uint64_t>(region_end, 0x100000000ull);
      const uint32_t* p = reinterpret_cast<const uint32_t*>(base + region_start);
      const uint64_t words = (end - region_start) / 4;
      scanned += end - region_start;
      for (uint64_t i = 0; i < words; ++i) {
        const uint32_t w = p[i];
        for (int k = 0; k < 4; ++k) {
          if (w != be(values[k]) || !values[k]) continue;
          const uint64_t guest = region_start + i * 4;
          if (counts[k]++ >= 24) continue;
          hits[k] += fmt::format(" 0x{:08X}", uint32_t(guest));
          if (k != 2 && counts[k] <= 8 && i + 8 < words) {
            hits[k] += "[";
            for (int j = -2; j < 6; ++j) {
              if (i + j >= words) break;
              hits[k] += fmt::format("{}{:08X}", j == -2 ? "" : ".", be(p[i + j]));
            }
            hits[k] += "]";
          }
        }
      }
    }
    g = region_end > g ? region_end : g + 0x1000;
  }
  for (int k = 0; k < 4; ++k) {
    REXLOG_INFO("[wwe2k14][ftrace] scan '{}' {}=0x{:X}: {} hits (scanned {} MB; guest addr = host - membase){}",
                path, names[k], values[k], counts[k], scanned >> 20, hits[k]);
  }
#endif
}

inline uint32_t NtReadFileTraceOverride(uint32_t file_handle, uint32_t event_handle, mapped_void apc_routine_ptr,
                                        mapped_void apc_context,
                                        ppc_ptr_t<rex::system::X_IO_STATUS_BLOCK> io_status_block,
                                        mapped_void buffer, uint32_t buffer_length, mapped_u64 byte_offset_ptr) {
  if (REXCVAR_GET(wwe2k14_file_trace)) {
    std::string path;
    bool log = false;
    {
      auto& st = GetFileTraceState();
      std::lock_guard<std::mutex> lock(st.mutex);
      auto it = st.handles.find(file_handle);
      if (it != st.handles.end() && it->second.reads_left > 0) {
        --it->second.reads_left;
        path = it->second.path;
        log = true;
      }
    }
    if (log) {
      const uint64_t offset = byte_offset_ptr ? static_cast<uint64_t>(*byte_offset_ptr) : ~0ull;
      REXLOG_INFO("[wwe2k14][ftrace] read '{}' handle=0x{:X} offset=0x{:X} len=0x{:X} buf=0x{:08X} event=0x{:X} "
                  "apc=0x{:08X} apc_ctx=0x{:08X} frame={} tid={} bt: {}",
                  path, file_handle, offset, buffer_length, buffer.guest_address(), event_handle,
                  apc_routine_ptr.guest_address(), apc_context.guest_address(), g_guest_frame.load(),
                  uint32_t(GetCurrentThreadId()), FileTraceBacktrace());
      // Game reads (ReadFileEx: APC routine set) of a path matching the scan filter: once per path, search
      // guest memory for the read's offset / sector (offset / 0x800) / length to find where the archive
      // index the game took them from lives.
      if (apc_routine_ptr.guest_address() && offset != ~0ull) FileTraceScanOnce(path, offset, buffer_length);
    }
  }
  if (!FlightOn())
    return rex::kernel::xboxkrnl::NtReadFile_entry(file_handle, event_handle, apc_routine_ptr, apc_context,
                                                   io_status_block, buffer, buffer_length, byte_offset_ptr);
  FlightCall flight(kFlightRead, file_handle, buffer_length,  // --wwe2k14_flight_recorder (task 11)
                    byte_offset_ptr ? uint32_t(uint64_t(*byte_offset_ptr)) : 0xFFFFFFFFu);
  return flight.Done(rex::kernel::xboxkrnl::NtReadFile_entry(file_handle, event_handle, apc_routine_ptr, apc_context,
                                                             io_status_block, buffer, buffer_length, byte_offset_ptr));
}

}  // namespace wwe2k14

REX_HOOK(__imp__NtReadFile, wwe2k14::NtReadFileTraceOverride)
