// wwe2k14 - kernel export overrides.
//
// Generated code calls kernel imports through symbols like __imp__MmAllocatePhysicalMemoryEx, which
// rexruntime.lib provides as import thunks. Defining the symbol here wins at link time (ReXGlue wiki,
// "Function Overrides"), so these wrappers run instead and call ReXGlue's own implementation.

#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/perf/timeline.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xio.h>
#include <rex/system/xmemory.h>
#include <rex/system/xtypes.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "frame_stats.h"
#include "input_script.h"
#include "bookmark.h"  // Session 29: the "that, just now" key (frame-time ring always on, the key in harness runs)
#include "sampling_profiler.h"
#include "file_trace.h"  // --wwe2k14_file_trace (Session 8 task 7, default off)
#include "guest_peek.h"  // --wwe2k14_peek (Session 8 task 7 menu/UI, default off)
#include "frame_hash.h"  // --wwe2k14_det_hash (Session 8 task 8b, default off)
#include "flight_recorder.h"  // --wwe2k14_flight_recorder (Session 8 task 11, default off)
#include "load_turbo.h"  // --wwe2k14_load_turbo / _boot / _menu (Session 12, default off)
#include <rex/det.h>
#include "boot_skip.h"   // --wwe2k14_skip_boot_screens (Session 12, default off)
#include "mods.h"        // --wwe2k14_mods (Session 17 item 1A, default off)
#include "step_rate.h"   // --wwe2k14_step_hz (Session 19 item 10, default off)
#include "discord_status.h"  // --wwe2k14_discord (Session 33 item 3, default off)
#include "entrance_music.h"  // --wwe2k14_entrance_music (Session 33 item 5, default off)
#include "ultrawide.h"       // --wwe2k14_aspect (Session 33 item 6, default off)
#include "perf_report.h"  // --wwe2k14_perf_report (Session 20 item 9, default off)
#include "half2_hooks.h"  // SDK gpu_native_half2 (Session 48, Half 2: the game's D3D library draws; default off)
#include "scene_crowd.h"  // --wwe2k14_scene_crowd (Session 50 step 3: the crowd as a scene-level pass; default off)

namespace rex::kernel::xboxkrnl {
// ReXGlue's implementations, exported by rexruntime.dll but not declared in the public headers.
// Signatures must match exactly (checked against the mangled names in rexruntime.lib).
uint32_t MmAllocatePhysicalMemoryEx_entry(uint32_t flags, uint32_t region_size, uint32_t protect_bits,
                                          uint32_t min_addr_range, uint32_t max_addr_range,
                                          uint32_t alignment);
uint32_t NtCreateFile_entry(mapped_u32 handle_out, uint32_t desired_access,
                            ppc_ptr_t<rex::system::X_OBJECT_ATTRIBUTES> object_attrs,
                            ppc_ptr_t<rex::system::X_IO_STATUS_BLOCK> io_status_block,
                            mapped_u64 allocation_size_ptr, uint32_t file_attributes, uint32_t share_access,
                            uint32_t creation_disposition, uint32_t create_options);
uint32_t NtOpenFile_entry(mapped_u32 handle_out, uint32_t desired_access,
                          ppc_ptr_t<rex::system::X_OBJECT_ATTRIBUTES> object_attributes,
                          ppc_ptr_t<rex::system::X_IO_STATUS_BLOCK> io_status_block, uint32_t open_options);
void VdSwap_entry(mapped_void buffer_ptr, mapped_void fetch_ptr, mapped_void unk2, mapped_void unk3,
                  mapped_void unk4, mapped_u32 frontbuffer_ptr, mapped_u32 texture_format_ptr,
                  mapped_u32 color_space_ptr, mapped_u32 width, mapped_u32 height);
}  // namespace rex::kernel::xboxkrnl

REXCVAR_DEFINE_BOOL(wwe2k14_log_file_opens, true, "wwe2k14",
                    "Log every file the game opens or creates, with the result");

// Port of Xenia's ignore_offset_for_ranged_allocations (CLAUDE.md known-good setting for 2K14).
// ReXGlue v0.10.0 deliberately left it out (see the NOTE in src/kernel/xboxkrnl/xboxkrnl_memory.cpp).
REXCVAR_DEFINE_BOOL(wwe2k14_ignore_offset_for_ranged_allocations, true, "wwe2k14",
                    "Xenia's ignore_offset_for_ranged_allocations: don't apply the heap's physical "
                    "address offset when a physical allocation passes both min and max addresses");

// Session 15 (A3): the game sizes its memory pools by asking for a block, then 1 MB less, ... until one fits (~480
// tries per pool, 5-8 pools per load; run 1089). Most tries ask for more than their whole min..max range (e.g. 0x1BA00000
// in a 0x680000 range), which ReXGlue rejects before any heap search, but each rejection logs two [error] lines
// ("BaseHeap::Alloc page count too big for requested range" + "PhysicalHeap::Alloc unable to alloc ... parent heap"):
// 9,740 of a run's 11,889 lines, ~6 ms per pool. With this on, a ranged request whose page-rounded size exceeds the
// range plus every rounding the SDK can add (alignment + page size + 64 KB) returns 0 at once: the same result, no
// SDK call. _verify: call the SDK anyway and log any request it would have satisfied (never expected).
REXCVAR_DEFINE_BOOL(wwe2k14_phys_alloc_fast_fail, false, "wwe2k14",
                    "Return 0 at once for a ranged MmAllocatePhysicalMemoryEx that cannot fit its range (the game's "
                    "pool-size probing; skips ReXGlue's two error lines per try). Session 15, default off");
REXCVAR_DEFINE_BOOL(wwe2k14_phys_alloc_fast_fail_verify, false, "wwe2k14",
                    "wwe2k14_phys_alloc_fast_fail: still call ReXGlue for each skipped request and log a mismatch if "
                    "it succeeds (the allocation is then kept and returned)");

namespace wwe2k14 {

inline std::atomic<uint32_t> g_phys_alloc_fast_failed{0};
inline std::atomic<uint32_t> g_phys_alloc_fast_fail_mismatch{0};
inline std::atomic<uint32_t> g_phys_alloc_calls{0};
inline std::atomic<uint32_t> g_phys_alloc_failures{0};
inline std::atomic<uint32_t> g_phys_alloc_offset_ignored{0};
inline std::atomic<uint32_t> g_phys_alloc_failure_streak{0};  // consecutive failures since the last success
inline std::atomic<bool> g_phys_heap_map_dumped{false};

inline uint32_t MmAllocatePhysicalMemoryExOverride(uint32_t flags, uint32_t region_size,
                                                   uint32_t protect_bits, uint32_t min_addr,
                                                   uint32_t max_addr, uint32_t alignment) {
  ++g_phys_alloc_calls;
  g_frame_mem_probes.fetch_add(1, std::memory_order_relaxed);  // Session 73 (frame_stats.h)
  uint32_t call_min = min_addr, call_max = max_addr;
  // Same condition as Xenia (xboxkrnl_memory.cc): only ranged allocations with both bounds set.
  if (min_addr && max_addr && REXCVAR_GET(wwe2k14_ignore_offset_for_ranged_allocations)) {
    uint32_t page_size = (protect_bits & rex::X_MEM_LARGE_PAGES)  ? 64 * 1024
                         : (protect_bits & rex::X_MEM_16MB_PAGES) ? 16 * 1024 * 1024
                                                                  : 4 * 1024;
    auto* heap = static_cast<rex::memory::PhysicalHeap*>(
        REX_KERNEL_MEMORY()->LookupHeapByType(true, page_size));
    uint32_t offset = heap ? heap->GetPhysicalAddress(heap->heap_base()) : 0;
    if (offset) {
      // ReXGlue subtracts `offset` from both bounds; pre-adding it is equivalent to Xenia's offset = 0.
      call_min = min_addr > 0xFFFFFFFFu - offset ? 0xFFFFFFFFu : min_addr + offset;
      call_max = max_addr > 0xFFFFFFFFu - offset ? 0xFFFFFFFFu : max_addr + offset;
      if (++g_phys_alloc_offset_ignored == 1) {
        REXLOG_INFO("[wwe2k14][mem] ignore_offset_for_ranged_allocations applied for the first time: "
                    "size=0x{:X} min=0x{:X} max=0x{:X} offset=0x{:X}", region_size, min_addr, max_addr, offset);
      }
    }
  }
  FlightCall flight(kFlightPAlloc, region_size, protect_bits, min_addr, max_addr);  // no-op unless the recorder is on
  // Session 15 (A3): a ranged request that cannot fit its range fails at once (see wwe2k14_phys_alloc_fast_fail). The
  // SDK rounds the size and alignment up to the page size, aligns the range's ends to the alignment (the top end up)
  // and counts 4 KB pages in the parent heap, so the most it can offer is span + alignment + one page; the 64 KB is
  // extra margin.
  bool fast_failed = false;
  if (min_addr && max_addr && call_max >= call_min && REXCVAR_GET(wwe2k14_phys_alloc_fast_fail)) {
    const uint64_t ps = (protect_bits & rex::X_MEM_LARGE_PAGES)  ? 64 * 1024
                        : (protect_bits & rex::X_MEM_16MB_PAGES) ? 16 * 1024 * 1024
                                                                 : 4 * 1024;
    const uint64_t size = (uint64_t(region_size) + ps - 1) / ps * ps;
    const uint64_t align = (uint64_t(alignment) + ps - 1) / ps * ps;
    fast_failed = size > uint64_t(call_max) - call_min + align + ps + 0x10000;
  }
  uint32_t result = 0;
  if (fast_failed) {
    ++g_phys_alloc_fast_failed;
    if (REXCVAR_GET(wwe2k14_phys_alloc_fast_fail_verify)) {
      result = rex::kernel::xboxkrnl::MmAllocatePhysicalMemoryEx_entry(flags, region_size, protect_bits, call_min,
                                                                       call_max, alignment);
      if (result) {
        REXLOG_WARN("[wwe2k14][mem] fast_fail MISMATCH #{}: the SDK satisfied size=0x{:X} min=0x{:X} max=0x{:X} "
                    "align=0x{:X} protect=0x{:X} at 0x{:08X} (kept)",
                    ++g_phys_alloc_fast_fail_mismatch, region_size, call_min, call_max, alignment, protect_bits,
                    result);
      }
    }
    flight.Done(result);
  } else {
    result = flight.Done(rex::kernel::xboxkrnl::MmAllocatePhysicalMemoryEx_entry(
        flags, region_size, protect_bits, call_min, call_max, alignment));
  }
  if (!result) {
    uint32_t n = ++g_phys_alloc_failures;
    ++g_phys_alloc_failure_streak;
    if (n <= 40 || n % 500 == 0) {
      REXLOG_WARN("[wwe2k14][mem] MmAllocatePhysicalMemoryEx failed #{} (of {} calls): size=0x{:X} "
                  "protect=0x{:X} min=0x{:X} max=0x{:X} align=0x{:X} flags=0x{:X} offset_ignored={} "
                  "fast_failed={} (fast {} in all)",
                  n, g_phys_alloc_calls.load(), region_size, protect_bits, min_addr, max_addr,
                  alignment, flags, call_min != min_addr, fast_failed, g_phys_alloc_fast_failed.load());
    }
    // Once per run: show what occupies guest memory when a big (>=16 MB) request first fails.
    if (region_size >= 0x1000000 && !g_phys_heap_map_dumped.exchange(true)) {
      REXLOG_WARN("[wwe2k14][mem] first failed request >= 16 MB; dumping guest memory map:");
      REX_KERNEL_MEMORY()->DumpMap();
    }
  } else if (uint32_t streak = g_phys_alloc_failure_streak.exchange(0)) {
    REXLOG_WARN("[wwe2k14][mem] MmAllocatePhysicalMemoryEx succeeded after {} failures: size=0x{:X} "
                "({} MB) at 0x{:08X} protect=0x{:X}",
                streak, region_size, region_size >> 20, result, protect_bits);
  }
  return result;
}

// --- File opens (CLAUDE.md harness: structured log of file opens) ---

// The path as the game passed it (relative to root_directory when that's a real directory handle).
inline std::string GuestObjectPath(ppc_ptr_t<rex::system::X_OBJECT_ATTRIBUTES> attrs) {
  if (!attrs) return "<no attributes>";
  const uint32_t name_ptr = attrs->name_ptr;
  if (!name_ptr) return "<no name>";
  auto* name = REX_KERNEL_MEMORY()->TranslateVirtual<rex::system::X_ANSI_STRING*>(name_ptr);
  const uint32_t length = name->length, pointer = name->pointer;
  if (!pointer || !length) return "";
  return std::string(REX_KERNEL_MEMORY()->TranslateVirtual<const char*>(pointer), length);
}

inline void LogFileOpen(const char* call, ppc_ptr_t<rex::system::X_OBJECT_ATTRIBUTES> attrs,
                        uint32_t disposition, uint32_t access, uint32_t result, mapped_u32 handle_out) {
  const std::string path = GuestObjectPath(attrs);
  g_frame_file_opens.fetch_add(1, std::memory_order_relaxed);  // Session 73 (frame_stats.h)
  if (result == 0) g_input_script.OnFileOpened(path);  // wait_file sync points / recording comments
  if (result == 0) bookmark::NoteFileOpen(g_guest_frame);  // Session 29: no automatic bookmark right after a load
  if (result == 0) g_load_turbo.OnFileOpened(path);    // no-op unless --wwe2k14_load_turbo*
  if (result == 0 && g_readback_file_hook) g_readback_file_hook(path, g_guest_frame.load());  // --wwe2k14_readback_loads
  BootSkipOnFileOpen(g_guest_frame.load());            // --wwe2k14_skip_boot_screens bookkeeping
  FileTraceOnOpen(path, result, handle_out ? uint32_t(*handle_out) : 0);  // no-op unless --wwe2k14_file_trace
  FlightOnFileOpen(call[2] == 'O', path, disposition, access, result == 0 && handle_out ? uint32_t(*handle_out) : 0,
                   result);  // no-op unless --wwe2k14_flight_recorder
  if (!REXCVAR_GET(wwe2k14_log_file_opens)) return;
  static constexpr const char* kDispositions[] = {"supersede", "open", "create", "open_if", "overwrite",
                                                  "overwrite_if"};
  const uint32_t root = attrs ? uint32_t(attrs->root_directory) : 0;
  const std::string root_note =
      (root && root != 0xFFFFFFFD) ? fmt::format(" (relative to handle 0x{:X})", root) : "";
  REXLOG_INFO("[wwe2k14][file] {} '{}'{} {} access=0x{:X} -> 0x{:08X}{} frame={}", call, path,
              root_note, disposition < 6 ? kDispositions[disposition] : "?", access, result,
              result == 0 && handle_out ? fmt::format(" handle=0x{:X}", uint32_t(*handle_out)) : "",
              g_guest_frame.load());
}

inline uint32_t NtCreateFileOverride(mapped_u32 handle_out, uint32_t desired_access,
                                     ppc_ptr_t<rex::system::X_OBJECT_ATTRIBUTES> object_attrs,
                                     ppc_ptr_t<rex::system::X_IO_STATUS_BLOCK> io_status_block,
                                     mapped_u64 allocation_size_ptr, uint32_t file_attributes,
                                     uint32_t share_access, uint32_t creation_disposition,
                                     uint32_t create_options) {
  mods::OpenRedirect redirect(object_attrs);  // no-op unless --wwe2k14_mods and a replacement exists
  uint32_t result = rex::kernel::xboxkrnl::NtCreateFile_entry(
      handle_out, desired_access, object_attrs, io_status_block, allocation_size_ptr, file_attributes,
      share_access, creation_disposition, create_options);
  LogFileOpen("NtCreateFile", object_attrs, creation_disposition, desired_access, result, handle_out);
  return result;
}

inline uint32_t NtOpenFileOverride(mapped_u32 handle_out, uint32_t desired_access,
                                   ppc_ptr_t<rex::system::X_OBJECT_ATTRIBUTES> object_attributes,
                                   ppc_ptr_t<rex::system::X_IO_STATUS_BLOCK> io_status_block,
                                   uint32_t open_options) {
  mods::OpenRedirect redirect(object_attributes);  // no-op unless --wwe2k14_mods and a replacement exists
  uint32_t result = rex::kernel::xboxkrnl::NtOpenFile_entry(handle_out, desired_access, object_attributes,
                                                            io_status_block, open_options);
  LogFileOpen("NtOpenFile", object_attributes, 1 /* open */, desired_access, result, handle_out);
  return result;
}

// --- Frame timing: VdSwap marks the end of every guest frame (frame_stats.h) ---

// Session 16 (item 6, src/cinecam.h freeze cam): called every VdSwap on the guest thread after the input script
// step (null = nothing; set at install time only when its setting is on).
inline void (*g_vdswap_hook)(uint64_t frame) = nullptr;

inline void VdSwapOverride(mapped_void buffer_ptr, mapped_void fetch_ptr, mapped_void unk2, mapped_void unk3,
                           mapped_void unk4, mapped_u32 frontbuffer_ptr, mapped_u32 texture_format_ptr,
                           mapped_u32 color_space_ptr, mapped_u32 width, mapped_u32 height) {
  // Session 15 (determinism): with det_frames (SDK patch 0010), every frame ends at a barrier: the other guest
  // threads and the command processor settle first, so the frame's state (and the per-frame hashes below) does not
  // depend on host timing.
  if (rex::det::SerialEnabled() && rex::det::SerialIsGuestThread()) {
    // det_serial: at least one vblank per frame (none if one already came since the previous frame).
    static uint64_t last_frame_vblank = 0;
    const uint64_t target = last_frame_vblank + 1;
    if (rex::det::Vblanks() < target) rex::det::SerialBlock(nullptr, target);
    last_frame_vblank = rex::det::Vblanks();
  } else if (rex::det::Enabled()) {
    rex::det::FrameBarrier();
  }
  g_frame_stats.OnSwap();
  FlightOnSwap(g_guest_frame);  // no-op unless --wwe2k14_flight_recorder (its forced-hang/crash tests run here)
  rex::perf::timeline::Record("vdswap", uint32_t(g_guest_frame.load()));
  g_input_script.OnFrame(g_guest_frame);
  bookmark::OnFrame(g_guest_frame);  // Session 29: frame-time ring for the bookmark key and the live !status fps
  replay::g_replay.OnFrame(g_guest_frame);  // no-op unless --wwe2k14_replay_* (Session 16)
  if (g_vdswap_hook) g_vdswap_hook(g_guest_frame);  // --wwe2k14_cinecam_freeze (Session 16)
  GuestPeekOnFrame(g_guest_frame);  // no-op unless --wwe2k14_peek
  g_load_turbo.OnFrame(g_guest_frame);  // no-op unless --wwe2k14_load_turbo*
  BootSkipOnFrame(g_guest_frame);       // no-op unless --wwe2k14_skip_boot_screens
  FrameHashOnFrame(g_guest_frame);  // no-op unless --wwe2k14_det_hash
  step_rate::OnFrame(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(0), g_guest_frame);  // --wwe2k14_step_hz
  discord::OnFrame(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(0), g_guest_frame);  // no-op unless --wwe2k14_discord
  if (g_photo_frame_hook) g_photo_frame_hook(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(0));  // --wwe2k14_photo_mode
  if (g_wm30_frame_hook) g_wm30_frame_hook(g_guest_frame);  // --wwe2k14_wm30 (Session 34)
  if (g_readback_frame_hook) g_readback_frame_hook(g_guest_frame);  // --wwe2k14_readback_sequences (Session 57)
  if (g_pipeline_wait_frame_hook) g_pipeline_wait_frame_hook(g_guest_frame);  // --wwe2k14_load_pipeline_wait (S57)
  if (g_session_frame_hook) g_session_frame_hook(g_guest_frame);  // --wwe2k14_session_log (Session 58)
  entrance_music::OnFrame(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(0));  // no-op unless a custom theme plays
  ultrawide::OnFrame(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(0), g_guest_frame);  // no-op at 16:9
  perf_report::OnFrame();  // no-op unless --wwe2k14_perf_report (after step_rate: its scene flags)
  ProfilerOnSwap();
  rex::kernel::xboxkrnl::VdSwap_entry(buffer_ptr, fetch_ptr, unk2, unk3, unk4, frontbuffer_ptr,
                                      texture_format_ptr, color_space_ptr, width, height);
}

}  // namespace wwe2k14

REX_HOOK(__imp__MmAllocatePhysicalMemoryEx, wwe2k14::MmAllocatePhysicalMemoryExOverride)
REX_HOOK(__imp__NtCreateFile, wwe2k14::NtCreateFileOverride)
REX_HOOK(__imp__NtOpenFile, wwe2k14::NtOpenFileOverride)
REX_HOOK(__imp__VdSwap, wwe2k14::VdSwapOverride)
