// wwe2k14 - flight recorder (Session 8 task 11). Default OFF: --wwe2k14_flight_recorder=true.
//
// While on, every guest thread keeps a lock-free ring of its most recent events (kFlightRingSize, newest
// overwrite oldest; one writer per ring = the thread itself, no locks):
//   kernel calls that matter for hangs (hooked here, at the bottom of this file): waits (NtWaitForSingleObjectEx,
//   KeWaitForSingleObject, KeWaitForMultipleObjects, NtWaitForMultipleObjectsEx), KeDelayExecutionThread, events
//   (KeSetEvent, NtSetEvent, KeResetEvent, NtClearEvent, NtCreateEvent), NtReleaseSemaphore, thread create /
//   resume / terminate, virtual and physical memory alloc / free, NtClose, NtWriteFile; plus NtReadFile
//   (file_trace.h), file opens and MmAllocatePhysicalMemoryEx (kernel_overrides.h); each call is one event
//   recorded when it returns (start time, duration, arguments, result, and the guest caller = ctx.lr, the return
//   address the generated code stores before every `bl`). A call still in progress (a thread blocked in a wait) is
//   kept in the ring's in-call slots, so a dump shows what a stuck thread is waiting on and for how long;
//   VdSwap frames (the main thread's ring) and input-script steps (input_script.h).
// The kernel calls need no SDK change: ReXGlue has no generic kernel-call trace point (REXKRNL_IMPORT_TRACE is
// per-export logging at the noisy trace level), so the imports are overridden the same way kernel_overrides.h does
// it. The one SDK change is the GPU records provider (rex/perf/flight.h, patches/rexglue-sdk/0006).
// RtlEnter/LeaveCriticalSection (~2,200 call sites) are not recorded; a thread stuck on one shows in the stack
// samples instead.
//
// Dumps go to logs/flight_NNN.txt (NNN = this run's wwe2k14_NNN.log; appended, one section per dump) on:
//   crash   unhandled exception (crash_handler.h calls FlightDumpOnCrash): includes the faulting thread's stack
//           unwound from the exception context
//   abort   abort() (crash_handler.h's SIGABRT handler)
//   hang    no VdSwap for --wwe2k14_flight_hang_seconds (default 10; 0 = off) once frames have started: every
//           thread's host stack is sampled --wwe2k14_flight_hang_samples times (default 3, 300 ms apart). One dump
//           per stall (re-armed when frames resume).
// Low-rate events (script steps, file opens, thread create/exit, tests) also go to one shared
// "milestones" ring (kFlightMilestones), since a busy thread's own ring wraps fast (main thread: ~16 frames).
// Each dump: a summary line per thread (what it is doing now), the milestones, then per thread newest first: blocked-in calls,
// host stack samples (identical samples merged; exe frames named from wwe2k14.map = the recompiled guest function
// sub_XXXXXXXX, DLL frames through DbgHelp when the PDBs are next to them), the guest stack (ctx.lr + the r1 back
// chain, guest addresses named by the containing sub_ from the map), and the ring's events. Then host threads
// without a ring (GPU Commands, audio...) with their stack samples, and, when the SDK build has it
// (rex/perf/flight.h, setting vulkan_flight_gpu_records), the newest GPU checkpoint records (draws / copies) from
// the Vulkan command processor's crash ring.
//
// Debug (default empty): --wwe2k14_flight_test=hang@FRAME[:SECONDS],crash@FRAME,abort@FRAME,dump@FRAME,bench@FRAME
//   hang: block the main thread inside the VdSwap hook (default 15 s); crash: null-pointer write on the main
//   thread; abort: std::abort(); dump: write a dump now (reason "requested"); bench: time the recording cost on
//   the main thread and log it with the event rate so far (FlightBench). FRAME = guest frame.
//
// Cost when off: each hooked kernel call does one setting read and a not-taken branch before calling ReXGlue's own
// implementation (the hooks always run, like kernel_overrides.h's). When on: one QueryPerformanceCounter and a
// ~100-byte ring write per call (measured with --wwe2k14_flight_test=bench@FRAME). The setting is read on every
// call, so `cvar wwe2k14_flight_recorder false/true` toggles recording live (in-run A/B), and crash / abort dumps
// work after such a toggle. Only with the setting on AT STARTUP: the hang watchdog, the forced tests and the GPU
// checkpoint records (vulkan_flight_gpu_records is set at startup; the dump says "(off ...)" otherwise).
// Hang dumps per run are capped (--wwe2k14_flight_max_hang_dumps, default 5).
// Thread state in a dump: a ring whose thread called ExTerminateThread says "exited"; a ring whose thread is no
// longer alive (returned without ExTerminateThread) says "gone". Rings are matched to live threads by thread id AND
// creation time (Windows reuses thread ids), and to guest threads by host and guest thread id.
// Dump timing: every ring is copied first, then "now" is taken, so no age is negative and the summary and the
// per-thread sections of one dump agree.
// Guest stack walks use the 360 back-chain convention (crash_handler.h GuestContextReport): r1 is exact for a
// thread blocked in a kernel call (the generated code passes ctx in memory across calls), may lag elsewhere.

#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xio.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#if __has_include(<rex/perf/flight.h>)
#include <rex/perf/flight.h>
#define WWE2K14_FLIGHT_GPU 1
#endif

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>
#pragma comment(lib, "dbghelp.lib")
#endif

#include "frame_stats.h"
#include "precise_sleep.h"  // --wwe2k14_precise_sleep / --wwe2k14_sleep_stats (Session 8 task B, default off)
// NOTE: those two settings work only through FlightKeDelayExecutionThread below (the REX_HOOK of
// __imp__KeDelayExecutionThread calls wwe2k14::sleep::Delay with the recorder on or off). Keep that call if the
// hook is ever changed or removed.

REXCVAR_DEFINE_BOOL(wwe2k14_flight_recorder, false, "wwe2k14",
                    "Flight recorder: per-thread rings of recent kernel calls, file opens, frames and script steps, "
                    "dumped to logs/flight_NNN.txt on crash, abort or hang");
REXCVAR_DEFINE_INT32(wwe2k14_flight_hang_seconds, 10, "wwe2k14",
                     "Flight recorder: hang dump when no frame is presented for this many seconds (0 = off)");
REXCVAR_DEFINE_INT32(wwe2k14_flight_hang_samples, 3, "wwe2k14",
                     "Flight recorder: stack samples per thread in a hang dump (300 ms apart)");
REXCVAR_DEFINE_INT32(wwe2k14_flight_dump_events, 32, "wwe2k14",
                     "Flight recorder: newest events written per thread in a dump (main thread: 4x)");
REXCVAR_DEFINE_INT32(wwe2k14_flight_gpu_dump_records, 48, "wwe2k14",
                     "Flight recorder: newest GPU checkpoint records in a dump (needs vulkan_flight_gpu_records)");
REXCVAR_DEFINE_INT32(wwe2k14_flight_max_hang_dumps, 5, "wwe2k14",
                     "Flight recorder: at most this many hang dumps per run (0 = no limit)");
REXCVAR_DEFINE_STRING(wwe2k14_flight_test, "", "wwe2k14",
                      "Debug: forced failures for the flight recorder, comma-separated: hang@FRAME[:SECONDS], "
                      "crash@FRAME, abort@FRAME, dump@FRAME, bench@FRAME (time the recording cost)");

namespace rex::kernel::xboxkrnl {
// ReXGlue's implementations (exported by rexruntime.dll, not declared in the public headers). Signatures copied
// from src/kernel/xboxkrnl/xboxkrnl_threading.cpp / _memory.cpp / _ob.cpp / _io.cpp of our SDK checkout.
uint32_t NtWaitForSingleObjectEx_entry(uint32_t object_handle, uint32_t wait_mode, uint32_t alertable,
                                       mapped_u64 timeout_ptr);
uint32_t KeWaitForSingleObject_entry(mapped_void object_ptr, uint32_t wait_reason, uint32_t processor_mode,
                                     uint32_t alertable, mapped_u64 timeout_ptr);
uint32_t KeWaitForMultipleObjects_entry(uint32_t count, mapped_u32 objects_ptr, uint32_t wait_type,
                                        uint32_t wait_reason, uint32_t processor_mode, uint32_t alertable,
                                        mapped_u64 timeout_ptr, mapped_void wait_block_array_ptr);
uint32_t NtWaitForMultipleObjectsEx_entry(uint32_t count, mapped_u32 handles, uint32_t wait_type, uint32_t wait_mode,
                                          uint32_t alertable, mapped_u64 timeout_ptr);
uint32_t KeDelayExecutionThread_entry(uint32_t processor_mode, uint32_t alertable, mapped_u64 interval_ptr);
uint32_t KeSetEvent_entry(ppc_ptr_t<rex::system::X_KEVENT> event_ptr, uint32_t increment, uint32_t wait);
uint32_t KeResetEvent_entry(ppc_ptr_t<rex::system::X_KEVENT> event_ptr);
uint32_t NtCreateEvent_entry(mapped_u32 handle_ptr, ppc_ptr_t<rex::system::X_OBJECT_ATTRIBUTES> obj_attributes_ptr,
                             uint32_t event_type, uint32_t initial_state);
uint32_t NtSetEvent_entry(uint32_t handle, mapped_u32 previous_state_ptr);
uint32_t NtClearEvent_entry(uint32_t handle);
uint32_t NtReleaseSemaphore_entry(uint32_t sem_handle, uint32_t release_count, mapped_u32 previous_count_ptr);
uint32_t ExCreateThread_entry(mapped_u32 handle_ptr, uint32_t stack_size, mapped_u32 thread_id_ptr,
                              uint32_t xapi_thread_startup, mapped_void start_address, mapped_void start_context,
                              uint32_t creation_flags);
uint32_t NtResumeThread_entry(uint32_t handle, mapped_u32 suspend_count_ptr);
uint32_t ExTerminateThread_entry(uint32_t exit_code);
uint32_t NtAllocateVirtualMemory_entry(mapped_u32 base_addr_ptr, mapped_u32 region_size_ptr, uint32_t alloc_type,
                                       uint32_t protect_bits, uint32_t debug_memory);
uint32_t NtFreeVirtualMemory_entry(mapped_u32 base_addr_ptr, mapped_u32 region_size_ptr, uint32_t free_type,
                                   uint32_t debug_memory);
void MmFreePhysicalMemory_entry(uint32_t type, uint32_t base_address);
uint32_t NtClose_entry(uint32_t handle);
uint32_t NtWriteFile_entry(uint32_t file_handle, uint32_t event_handle, uint32_t apc_routine, mapped_void apc_context,
                           ppc_ptr_t<rex::system::X_IO_STATUS_BLOCK> io_status_block, mapped_void buffer,
                           uint32_t buffer_length, mapped_u64 byte_offset_ptr);
}  // namespace rex::kernel::xboxkrnl

namespace wwe2k14 {

enum FlightKind : uint16_t {
  kFlightNone = 0,
  kFlightSwap,
  kFlightScript,
  kFlightTest,
  kFlightFileOpen,   // a = 0 NtCreateFile / 1 NtOpenFile, b = disposition, c = access, d = handle, e = result
  kFlightRead,       // a = handle, b = length, c = offset (low 32; 0xFFFFFFFF = current), e = result
  kFlightWrite,      // same
  kFlightNtWait,     // a = handle, b = timeout ms (0xFFFFFFFF = infinite, 0xFFFFFFFE = absolute), c = alertable
  kFlightKeWait,     // a = object (guest address), b = timeout, c = alertable, d = wait reason
  kFlightKeWaitMulti,  // a = count, b = timeout, c = wait type (0 all, 1 any), d = first object
  kFlightNtWaitMulti,  // a = count, b = timeout, c = wait type, d = first handle
  kFlightDelay,      // a = interval us (0xFFFFFFFF = absolute/none), c = alertable
  kFlightKeSetEvent,   // a = event, e = previous state
  kFlightNtSetEvent,   // a = handle, e = result
  kFlightKeResetEvent,  // a = event
  kFlightNtClearEvent,  // a = handle
  kFlightNtCreateEvent,  // a = type, b = initial state, d = handle, e = result
  kFlightSemRelease,     // a = handle, b = count, e = result
  kFlightThreadCreate,   // a = start address, b = start context, c = flags, d = handle, e = result
  kFlightThreadResume,   // a = handle, e = result
  kFlightThreadExit,     // a = exit code
  kFlightVAlloc,         // a = base (after), b = size (after), c = type, d = protect, e = result
  kFlightVFree,          // a = base, b = size, c = type, e = result
  kFlightPAlloc,         // a = size, b = protect, c = min, d = max, e = result (address)
  kFlightPFree,          // a = address, b = type
  kFlightClose,          // a = handle, e = result
  kFlightTestHang,       // a = seconds
  kFlightKindCount
};

inline const char* FlightKindName(uint16_t kind) {
  static constexpr const char* kNames[kFlightKindCount] = {
      "?",  "frame", "script", "test", "file_open", "NtReadFile", "NtWriteFile", "NtWaitForSingleObjectEx",
      "KeWaitForSingleObject", "KeWaitForMultipleObjects", "NtWaitForMultipleObjectsEx", "KeDelayExecutionThread",
      "KeSetEvent", "NtSetEvent", "KeResetEvent", "NtClearEvent", "NtCreateEvent", "NtReleaseSemaphore",
      "ExCreateThread", "NtResumeThread", "ExTerminateThread", "NtAllocateVirtualMemory", "NtFreeVirtualMemory",
      "MmAllocatePhysicalMemoryEx", "MmFreePhysicalMemory", "NtClose", "flight test hang"};
  return kind < kFlightKindCount ? kNames[kind] : "?";
}

struct FlightEvent {
  std::atomic<uint64_t> seq{0};  // ring index + 1 once complete; 0 while being written
  uint16_t kind = 0;
  uint8_t text_len = 0;
  uint8_t pad = 0;
  uint32_t dur_us = 0;  // call duration (0 for point events)
  int64_t qpc = 0;      // call start (QueryPerformanceCounter)
  uint32_t frame = 0;   // guest frame at the start
  uint32_t lr = 0;      // guest caller (ctx.lr), 0 = unknown
  uint32_t a = 0, b = 0, c = 0, d = 0, e = 0;
  char text[44] = {};
};
static_assert(sizeof(FlightEvent) == 96, "FlightEvent layout");

struct FlightInCall {
  std::atomic<uint32_t> active{0};
  uint16_t kind = 0;
  int64_t qpc = 0;
  uint32_t frame = 0, lr = 0, a = 0, b = 0, c = 0, d = 0;
};

constexpr size_t kFlightRingSize = 512;  // power of two
constexpr int kFlightMaxDepth = 4;       // nested calls kept (an alertable wait can run an APC that calls in)
constexpr size_t kFlightMaxRings = 512;

struct FlightRing {
  uint32_t host_tid = 0;
  uint32_t guest_tid = 0;  // XThread::thread_id(), 0 = not a guest thread
  uint64_t created = 0;    // system time (FILETIME units) when the ring was made: later than the thread's creation
  char name[32] = {};      // the XThread's name when the ring was made (shown once the thread is gone)
  std::atomic<uint32_t> exited{0};  // 1 once the thread called ExTerminateThread
  uint32_t exit_code = 0, exit_frame = 0;
  std::atomic<uint64_t> head{0};
  std::atomic<int> depth{0};
  FlightInCall calls[kFlightMaxDepth];
  FlightEvent events[kFlightRingSize];
};

inline std::atomic<FlightRing*> g_flight_rings[kFlightMaxRings];
inline std::atomic<uint32_t> g_flight_ring_count{0};
inline thread_local FlightRing* t_flight_ring = nullptr;
inline thread_local bool t_flight_in_dump = false;
inline std::atomic<int64_t> g_flight_last_swap_qpc{0};
inline int64_t g_flight_start_qpc = 0;  // StartFlightRecorder (0 = not on at startup)
inline int g_flight_log_number = -1;
inline volatile uintptr_t g_flight_null = 0;  // the crash test writes through this

inline int64_t FlightNow() {
#ifdef _WIN32
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
#else
  return 0;
#endif
}
inline double FlightQpcSeconds(int64_t ticks) {
#ifdef _WIN32
  static const double frequency = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return double(f.QuadPart);
  }();
  return double(ticks) / frequency;
#else
  return 0;
#endif
}

inline bool FlightOn() { return REXCVAR_GET(wwe2k14_flight_recorder); }

inline FlightRing* FlightThisRing() {
  if (FlightRing* r = t_flight_ring) return r;
  const uint32_t index = g_flight_ring_count.fetch_add(1, std::memory_order_relaxed);
  if (index >= kFlightMaxRings) return nullptr;  // keeps failing for this thread: fine, rings are a debug aid
  auto* r = new FlightRing();
#ifdef _WIN32
  r->host_tid = GetCurrentThreadId();
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  r->created = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
#endif
  if (auto* thread = rex::system::XThread::GetCurrentThread()) {
    r->guest_tid = thread->thread_id();
    const std::string name = thread->name();
    std::snprintf(r->name, sizeof(r->name), "%s", name.c_str());
  }
  g_flight_rings[index].store(r, std::memory_order_release);
  t_flight_ring = r;
  return r;
}

inline uint32_t FlightGuestLr() {
  auto* ts = rex::runtime::ThreadState::Get();
  return ts && ts->context() ? uint32_t(ts->context()->lr) : 0;
}

// Low-rate events of every thread (script steps, file opens, thread create / exit, tests; not physical allocations,
// ~100 per load screen crowded out the rest) also go to one shared ring, so a dump shows their recent history even when a busy thread's own ring has wrapped
// (the main thread records ~30 events per frame: its 512-event ring holds ~16 frames). Several writers: each takes
// a slot with fetch_add; a slot being rewritten is skipped by the reader (seq check).
constexpr size_t kFlightMilestones = 1024;  // power of two
struct FlightMilestoneRing {
  std::atomic<uint64_t> head{0};
  FlightEvent events[kFlightMilestones];
  uint32_t tids[kFlightMilestones] = {};
};
inline FlightMilestoneRing g_flight_milestones;

inline bool FlightIsMilestone(uint16_t kind) {
  return kind == kFlightScript || kind == kFlightTest || kind == kFlightFileOpen || kind == kFlightThreadCreate ||
         kind == kFlightThreadExit || kind == kFlightTestHang;  // not PAlloc: ~100 per load screen, they crowded out the rest
}

inline void FlightWrite(FlightEvent& ev, uint64_t i, uint32_t* tid_slot, uint32_t tid, uint16_t kind, int64_t qpc,
                        uint32_t dur_us, uint32_t frame, uint32_t lr, uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                        uint32_t e, std::string_view text) {
  ev.seq.store(0, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  if (tid_slot) *tid_slot = tid;
  ev.kind = kind;
  ev.dur_us = dur_us;
  ev.qpc = qpc;
  ev.frame = frame;
  ev.lr = lr;
  ev.a = a;
  ev.b = b;
  ev.c = c;
  ev.d = d;
  ev.e = e;
  if (text.size() > sizeof(ev.text)) text = text.substr(text.size() - sizeof(ev.text));
  ev.text_len = uint8_t(text.size());
  if (!text.empty()) std::memcpy(ev.text, text.data(), text.size());
  ev.seq.store(i + 1, std::memory_order_release);
}

inline void FlightPush(FlightRing* r, uint16_t kind, int64_t qpc, uint32_t dur_us, uint32_t frame, uint32_t lr,
                       uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, std::string_view text = {}) {
  if (FlightIsMilestone(kind)) {
    const uint64_t j = g_flight_milestones.head.fetch_add(1, std::memory_order_relaxed);
    const size_t slot = j & (kFlightMilestones - 1);
    FlightWrite(g_flight_milestones.events[slot], j, &g_flight_milestones.tids[slot], r->host_tid, kind, qpc, dur_us,
                frame, lr, a, b, c, d, e, text);
  }
  const uint64_t i = r->head.load(std::memory_order_relaxed);
  FlightEvent& ev = r->events[i & (kFlightRingSize - 1)];
  ev.seq.store(0, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  ev.kind = kind;
  ev.dur_us = dur_us;
  ev.qpc = qpc;
  ev.frame = frame;
  ev.lr = lr;
  ev.a = a;
  ev.b = b;
  ev.c = c;
  ev.d = d;
  ev.e = e;
  // Keep the tail of long texts (file paths: the file name matters most).
  if (text.size() > sizeof(ev.text)) text = text.substr(text.size() - sizeof(ev.text));
  ev.text_len = uint8_t(text.size());
  if (!text.empty()) std::memcpy(ev.text, text.data(), text.size());
  ev.seq.store(i + 1, std::memory_order_release);
  r->head.store(i + 1, std::memory_order_release);
}

// A point event on the calling thread's ring (no-op when off).
inline void FlightNote(uint16_t kind, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0, uint32_t e = 0,
                       std::string_view text = {}) {
  if (!FlightOn()) return;
  FlightRing* r = FlightThisRing();
  if (!r) return;
  FlightPush(r, kind, FlightNow(), 0, uint32_t(g_guest_frame.load(std::memory_order_relaxed)), FlightGuestLr(), a, b,
             c, d, e, text);
}

// One kernel call: in progress (visible to dumps) from construction, one ring event at Done().
class FlightCall {
 public:
  FlightCall(uint16_t kind, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0) {
    if (!FlightOn()) return;
    ring_ = FlightThisRing();
    if (!ring_) return;
    kind_ = kind;
    a_ = a, b_ = b, c_ = c, d_ = d;
    qpc_ = FlightNow();
    frame_ = uint32_t(g_guest_frame.load(std::memory_order_relaxed));
    lr_ = FlightGuestLr();
    slot_ = ring_->depth.load(std::memory_order_relaxed);
    if (slot_ < kFlightMaxDepth) {
      FlightInCall& call = ring_->calls[slot_];
      call.active.store(0, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_release);
      call.kind = kind, call.qpc = qpc_, call.frame = frame_, call.lr = lr_;
      call.a = a, call.b = b, call.c = c, call.d = d;
      call.active.store(1, std::memory_order_release);
    }
    ring_->depth.store(slot_ + 1, std::memory_order_release);
  }
  FlightCall(const FlightCall&) = delete;
  FlightCall& operator=(const FlightCall&) = delete;
  ~FlightCall() { Pop(); }

  // Updates arguments known only after the call (e.g. the handle created), records the event, returns `result`.
  void Set(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { a_ = a, b_ = b, c_ = c, d_ = d; }
  void SetD(uint32_t d) { d_ = d; }
  uint32_t Done(uint32_t result, std::string_view text = {}) {
    if (!ring_) return result;
    const int64_t now = FlightNow();
    const double us = FlightQpcSeconds(now - qpc_) * 1e6;
    FlightPush(ring_, kind_, qpc_, us > 4e9 ? 0xFFFFFFFFu : uint32_t(us), frame_, lr_, a_, b_, c_, d_, result, text);
    Pop();
    return result;
  }

 private:
  void Pop() {
    if (!ring_) return;
    if (slot_ < kFlightMaxDepth) ring_->calls[slot_].active.store(0, std::memory_order_release);
    ring_->depth.store(slot_, std::memory_order_release);
    ring_ = nullptr;
  }
  FlightRing* ring_ = nullptr;
  int slot_ = 0;
  uint16_t kind_ = 0;
  int64_t qpc_ = 0;
  uint32_t frame_ = 0, lr_ = 0, a_ = 0, b_ = 0, c_ = 0, d_ = 0;
};

// Xbox timeouts: 100 ns units, negative = relative, positive = absolute. -> ms; 0xFFFFFFFF = none (infinite).
inline uint32_t FlightTimeoutMs(mapped_u64 timeout_ptr) {
  if (!timeout_ptr) return 0xFFFFFFFFu;
  const int64_t t = int64_t(uint64_t(*timeout_ptr));
  if (t > 0) return 0xFFFFFFFEu;
  const int64_t ms = -t / 10000;
  return ms >= 0xFFFFFFFDll ? 0xFFFFFFFDu : uint32_t(ms);
}

// ---------------------------------------------------------------------------------------------------------------
// Names: wwe2k14.map (exe symbols; sub_XXXXXXXX = the recompiled guest function at that address) and DbgHelp.
// Loaded at the first dump. Dumps are serialised (g_flight_dump_mutex), so no other locking.
// ---------------------------------------------------------------------------------------------------------------
#ifdef _WIN32
class FlightNames {
 public:
  void Load() {
    if (loaded_) return;
    loaded_ = true;
    exe_base_ = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    MODULEINFO info{};
    GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &info, sizeof(info));
    exe_size_ = info.SizeOfImage;
    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    std::ifstream in(std::filesystem::path(exe_path).replace_extension(".map"));
    const uint64_t preferred = 0x140000000;
    for (std::string line; std::getline(in, line);) {
      // " 0001:00012340       sub_82CE3868    0000000140013340     wwe2k14_recomp.161.cpp.obj"
      if (line.size() < 40 || line[0] != ' ' || line[5] != ':') continue;
      const size_t name_start = line.find_first_not_of(' ', 14);
      if (name_start == std::string::npos) continue;
      const size_t name_end = line.find(' ', name_start);
      if (name_end == std::string::npos) continue;
      const size_t addr_start = line.find_first_not_of(' ', name_end);
      if (addr_start == std::string::npos || addr_start + 16 > line.size()) continue;
      const uint64_t address = std::strtoull(line.substr(addr_start, 16).c_str(), nullptr, 16);
      if (address < preferred) continue;
      std::string name = line.substr(name_start, name_end - name_start);
      if (name.size() == 12 && name.compare(0, 4, "sub_") == 0) {
        const uint32_t guest = uint32_t(std::strtoul(name.c_str() + 4, nullptr, 16));
        if (guest) guest_.push_back(guest);
      }
      host_.push_back({address - preferred, std::move(name)});
    }
    std::sort(host_.begin(), host_.end(), [](auto& x, auto& y) { return x.first < y.first; });
    std::sort(guest_.begin(), guest_.end());
    guest_.erase(std::unique(guest_.begin(), guest_.end()), guest_.end());
    // DbgHelp is single-threaded: leave it to the sampling profiler when that runs.
    const bool profiler = rex::cvar::GetFlagByName("wwe2k14_profile_main") == "true" ||
                          rex::cvar::GetFlagByName("wwe2k14_profile_gpu_commands") == "true";
    if (!profiler) {
      SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
      dbghelp_ = SymInitializeW(GetCurrentProcess(), std::filesystem::path(exe_path).parent_path().c_str(), TRUE);
    }
  }
  size_t host_symbols() const { return host_.size(); }
  bool dbghelp() const { return dbghelp_; }

  std::string Host(uintptr_t rip) {
    if (rip >= exe_base_ && rip < exe_base_ + exe_size_) {
      const uint64_t rva = rip - exe_base_;
      auto it = std::upper_bound(host_.begin(), host_.end(), rva,
                                 [](uint64_t v, const std::pair<uint64_t, std::string>& s) { return v < s.first; });
      if (it != host_.begin()) {
        --it;
        return fmt::format("{}+0x{:X}", it->second, rva - it->first);
      }
      return fmt::format("wwe2k14.exe+0x{:X}", rva);
    }
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(rip), &module);
    char path[MAX_PATH] = "?";
    if (module) GetModuleFileNameA(module, path, MAX_PATH);
    const char* leaf = path;
    for (const char* p = path; *p; ++p)
      if (*p == '\\' || *p == '/') leaf = p + 1;
    if (dbghelp_) {
      alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] = {};
      auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
      symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
      symbol->MaxNameLen = 512;
      DWORD64 displacement = 0;
      if (SymFromAddr(GetCurrentProcess(), rip, &displacement, symbol))
        return fmt::format("{}!{}+0x{:X}", leaf, static_cast<const char*>(symbol->Name), displacement);
    }
    return fmt::format("{}+0x{:X}", leaf, rip - reinterpret_cast<uintptr_t>(module));
  }

  // "82C9F1F0 (sub_82C9F0A8+0x148)": the nearest recompiled function starting at or below the address (a guess
  // only in the sense that an address past a function's end would be attributed to it; return addresses aren't).
  std::string Guest(uint32_t address) const {
    if (!address) return "0";
    auto it = std::upper_bound(guest_.begin(), guest_.end(), address);
    if (it == guest_.begin()) return fmt::format("{:08X}", address);
    --it;
    return fmt::format("{:08X} (sub_{:08X}+0x{:X})", address, *it, address - *it);
  }

 private:
  bool loaded_ = false;
  bool dbghelp_ = false;
  uintptr_t exe_base_ = 0;
  size_t exe_size_ = 0;
  std::vector<std::pair<uint64_t, std::string>> host_;
  std::vector<uint32_t> guest_;
};
inline FlightNames g_flight_names;
inline std::mutex g_flight_dump_mutex;
inline int g_flight_dump_count = 0;

// ---------------------------------------------------------------------------------------------------------------
// Host stack sampling (same method as sampling_profiler.h: suspend, GetThreadContext, unwind, resume; nothing
// allocates while a thread is suspended).
// ---------------------------------------------------------------------------------------------------------------
constexpr size_t kFlightStackDepth = 40;
struct FlightStack {
  uint32_t depth = 0;
  uintptr_t rips[kFlightStackDepth];
};
struct FlightHostThread {
  DWORD id = 0;
  HANDLE handle = nullptr;
  uintptr_t stack_base = 0, stack_limit = 0;
  uint64_t created = 0;  // GetThreadTimes creation time (FILETIME units), 0 = unknown
  std::string name;
  std::vector<FlightStack> samples;
};

// The thread's stack range from its TEB: [limit, base) is committed, so reads inside it can't fault.
inline bool FlightStackRange(HANDLE thread, uintptr_t* base, uintptr_t* limit) {
  struct BasicInformation {
    LONG exit_status;
    PVOID teb;
    PVOID client_id[2];
    ULONG_PTR affinity;
    LONG priority, base_priority;
  } info{};
  using Query = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
  static const auto query =
      reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
  if (!query || query(thread, 0 /* ThreadBasicInformation */, &info, sizeof(info), nullptr) != 0 || !info.teb)
    return false;
  *base = reinterpret_cast<uintptr_t>(static_cast<NT_TIB*>(info.teb)->StackBase);
  *limit = reinterpret_cast<uintptr_t>(static_cast<NT_TIB*>(info.teb)->StackLimit);
  return *base > *limit;
}

// Walks only while Rsp stays inside [stack_limit, stack_base) (committed stack) and Rip is a canonical user-mode
// address; the caller also runs it under SEH (FlightUnwindSafe), so a bad unwind drops that one stack.
inline uint32_t FlightUnwind(CONTEXT context, uintptr_t stack_base, uintptr_t stack_limit, uintptr_t* out,
                             size_t max) {
  constexpr uint64_t kUserTop = 0x00007FFFFFFFFFFFull;
  uint32_t n = 0;
  if (!context.Rip || context.Rip > kUserTop) return 0;
  out[n++] = uintptr_t(context.Rip);
  while (n < max) {
    if (context.Rsp < stack_limit || context.Rsp + 8 > stack_base) break;
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
    if (!function) {
      context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);  // leaf function: return address on top
      context.Rsp += 8;
    } else {
      PVOID handler_data = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data, &establisher,
                       nullptr);
    }
    if (!context.Rip || context.Rip > kUserTop) break;
    out[n++] = uintptr_t(context.Rip);
  }
  return n;
}

// FlightUnwind through a volatile pointer inside __try (clang's synchronous SEH catches faults in calls, as
// SafeCopy in frame_hash.h): a fault while unwinding gives an empty stack instead of a fault inside the dump.
inline uint32_t FlightUnwindSafe(const CONTEXT* context, uintptr_t stack_base, uintptr_t stack_limit, uintptr_t* out,
                                 size_t max) {
  static uint32_t (*volatile unwind)(CONTEXT, uintptr_t, uintptr_t, uintptr_t*, size_t) = FlightUnwind;
  if (!stack_base || stack_base <= stack_limit) return 0;
  uint32_t n = 0;
  __try {
    n = unwind(*context, stack_base, stack_limit, out, max);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    n = 0;
  }
  return n;
}

inline std::string FlightThreadDescription(DWORD id) {
  using GetDescription = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static const auto get =
      reinterpret_cast<GetDescription>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  std::string out;
  if (!get) return out;
  HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
  if (!thread) return out;
  PWSTR description = nullptr;
  if (SUCCEEDED(get(thread, &description)) && description) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, description, -1, nullptr, 0, nullptr, nullptr);
    if (size > 1) {
      out.resize(size_t(size));
      WideCharToMultiByte(CP_UTF8, 0, description, -1, out.data(), size, nullptr, nullptr);
      out.resize(size_t(size) - 1);
    }
    LocalFree(description);
  }
  CloseHandle(thread);
  return out;
}

// Every thread of the process except the caller, `samples` stacks each, `gap_ms` apart.
inline std::vector<FlightHostThread> FlightSampleThreads(int samples, int gap_ms) {
  std::vector<FlightHostThread> threads;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return threads;
  THREADENTRY32 entry{sizeof(entry)};
  for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == GetCurrentThreadId()) continue;
    FlightHostThread t;
    t.id = entry.th32ThreadID;
    t.handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, t.id);
    if (!t.handle) continue;
    if (!FlightStackRange(t.handle, &t.stack_base, &t.stack_limit)) t.stack_base = t.stack_limit = 0;
    FILETIME creation, exit_time, kernel_time, user_time;
    if (GetThreadTimes(t.handle, &creation, &exit_time, &kernel_time, &user_time))
      t.created = (uint64_t(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
    t.name = FlightThreadDescription(t.id);
    t.samples.resize(size_t(std::max(1, samples)));
    threads.push_back(std::move(t));
  }
  CloseHandle(snapshot);
  for (int k = 0; k < std::max(1, samples); ++k) {
    if (k) Sleep(DWORD(gap_ms));
    for (FlightHostThread& t : threads) {
      FlightStack& s = t.samples[size_t(k)];
      if (SuspendThread(t.handle) == DWORD(-1)) continue;
      CONTEXT context{};
      context.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(t.handle, &context) && t.stack_base)
        s.depth = FlightUnwindSafe(&context, t.stack_base, t.stack_limit, s.rips, kFlightStackDepth);
      ResumeThread(t.handle);
    }
  }
  for (FlightHostThread& t : threads) CloseHandle(t.handle), t.handle = nullptr;
  return threads;
}

inline void FlightAppendStacks(std::string& out, const std::vector<FlightStack>& samples, const char* indent) {
  std::vector<bool> shown(samples.size(), false);
  for (size_t i = 0; i < samples.size(); ++i) {
    if (shown[i] || !samples[i].depth) continue;
    size_t same = 0;
    for (size_t j = i; j < samples.size(); ++j) {
      if (samples[j].depth == samples[i].depth &&
          std::equal(samples[i].rips, samples[i].rips + samples[i].depth, samples[j].rips)) {
        shown[j] = true;
        ++same;
      }
    }
    out += fmt::format("{}host stack (sample {}; same in {}/{} samples):\n", indent, i + 1, same, samples.size());
    for (uint32_t f = 0; f < samples[i].depth; ++f)
      out += fmt::format("{}  #{:02} {}\n", indent, f, g_flight_names.Host(samples[i].rips[f]));
  }
}
#endif  // _WIN32

// ---------------------------------------------------------------------------------------------------------------
// Dump
// ---------------------------------------------------------------------------------------------------------------
inline std::string FlightFormatTimeout(uint32_t ms) {
  if (ms == 0xFFFFFFFFu) return "inf";
  if (ms == 0xFFFFFFFEu) return "absolute";
  return fmt::format("{}ms", ms);
}

inline std::string FlightDescribe(uint16_t kind, uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                                  std::string_view text) {
  switch (kind) {
    case kFlightSwap: return fmt::format("frame {} presented (VdSwap)", a);
    case kFlightScript: return fmt::format("input script line {}: {}", a, text);
    case kFlightTest: return fmt::format("flight test: {}", text);
    case kFlightFileOpen:
      return fmt::format("{} '{}' disp={} access=0x{:X} handle=0x{:X}", a ? "NtOpenFile" : "NtCreateFile", text, b, c,
                         d);
    case kFlightRead:
    case kFlightWrite: return fmt::format("{} h=0x{:X} len=0x{:X} off=0x{:X}", FlightKindName(kind), a, b, c);
    case kFlightNtWait:
      return fmt::format("NtWaitForSingleObjectEx h=0x{:X} timeout={} alertable={}", a, FlightFormatTimeout(b), c);
    case kFlightKeWait:
      return fmt::format("KeWaitForSingleObject obj={:08X} timeout={} alertable={} reason={}", a,
                         FlightFormatTimeout(b), c, d);
    case kFlightKeWaitMulti:
    case kFlightNtWaitMulti:
      return fmt::format("{} n={} {} first={:08X} timeout={}", FlightKindName(kind), a, c ? "any" : "all", d,
                         FlightFormatTimeout(b));
    case kFlightDelay:
      return fmt::format("KeDelayExecutionThread {} alertable={}",
                         a == 0xFFFFFFFFu ? std::string("?") : fmt::format("{}us", a), c);
    case kFlightKeSetEvent:
    case kFlightKeResetEvent: return fmt::format("{} ev={:08X}", FlightKindName(kind), a);
    case kFlightNtSetEvent:
    case kFlightNtClearEvent:
    case kFlightThreadResume:
    case kFlightClose: return fmt::format("{} h=0x{:X}", FlightKindName(kind), a);
    case kFlightNtCreateEvent:
      return fmt::format("NtCreateEvent type={} initial={} handle=0x{:X}", a, b, d);
    case kFlightSemRelease: return fmt::format("NtReleaseSemaphore h=0x{:X} count={}", a, b);
    case kFlightThreadCreate:
      return fmt::format("ExCreateThread start={:08X} ctx={:08X} flags=0x{:X} handle=0x{:X}", a, b, c, d);
    case kFlightThreadExit: return fmt::format("ExTerminateThread code={}", a);
    case kFlightVAlloc:
      return fmt::format("NtAllocateVirtualMemory base={:08X} size=0x{:X} type=0x{:X} protect=0x{:X}", a, b, c, d);
    case kFlightVFree: return fmt::format("NtFreeVirtualMemory base={:08X} size=0x{:X} type=0x{:X}", a, b, c);
    case kFlightPAlloc:
      return fmt::format("MmAllocatePhysicalMemoryEx size=0x{:X} protect=0x{:X} range={:08X}-{:08X}", a, b, c, d);
    case kFlightPFree: return fmt::format("MmFreePhysicalMemory addr={:08X} type={}", a, b);
    case kFlightTestHang: return fmt::format("flight test hang {} s (main thread blocked on purpose)", a);
    default: return FlightKindName(kind);
  }
}

inline bool FlightHasResult(uint16_t kind) {
  return kind >= kFlightFileOpen && kind != kFlightKeResetEvent && kind != kFlightNtClearEvent &&
         kind != kFlightThreadExit && kind != kFlightPFree && kind != kFlightTestHang;
}

struct FlightEventCopy {
  uint16_t kind;
  uint32_t dur_us, frame, lr, a, b, c, d, e;
  int64_t qpc;
  std::string text;
  uint32_t tid = 0;  // milestones only
};

// Newest first, up to `max`; skips slots being rewritten while we read. tids: per-slot thread ids (milestones).
inline std::vector<FlightEventCopy> FlightReadEvents(const FlightEvent* events, size_t size, uint64_t head, size_t max,
                                                     const uint32_t* tids = nullptr) {
  std::vector<FlightEventCopy> out;
  const uint64_t count = std::min<uint64_t>({head, uint64_t(size), uint64_t(max)});
  for (uint64_t k = 0; k < count; ++k) {
    const uint64_t i = head - 1 - k;
    const FlightEvent& ev = events[i & (size - 1)];
    const uint64_t s1 = ev.seq.load(std::memory_order_acquire);
    if (s1 != i + 1) continue;
    FlightEventCopy c{ev.kind, ev.dur_us, ev.frame, ev.lr, ev.a, ev.b, ev.c, ev.d, ev.e, ev.qpc,
                      std::string(ev.text, std::min<size_t>(ev.text_len, sizeof(ev.text))),
                      tids ? tids[i & (size - 1)] : 0u};
    std::atomic_thread_fence(std::memory_order_acquire);
    if (ev.seq.load(std::memory_order_relaxed) != s1) continue;
    out.push_back(std::move(c));
  }
  return out;
}
inline std::vector<FlightEventCopy> FlightReadRing(const FlightRing& r, size_t max) {
  return FlightReadEvents(r.events, kFlightRingSize, r.head.load(std::memory_order_acquire), max);
}

struct FlightGuestThread {
  uint32_t host_tid = 0, guest_tid = 0;
  std::string name;
  std::string stack;  // formatted
};

// Guest threads (ReXGlue's XThread objects): name, host thread id and the guest stack from the saved context.
inline std::vector<FlightGuestThread> FlightGuestThreads() {
  std::vector<FlightGuestThread> out;
  auto* kernel = REX_KERNEL_STATE();
  if (!kernel || !kernel->object_table()) return out;
  auto* memory = REX_KERNEL_MEMORY();
  for (auto& thread : kernel->object_table()->GetObjectsByType<rex::system::XThread>()) {
    if (!thread || !thread->is_guest_thread()) continue;
    FlightGuestThread g;
    g.guest_tid = thread->thread_id();
    g.name = thread->name();
    if (auto* host = thread->thread()) g.host_tid = host->system_id();
    auto* ts = thread->thread_state();
    const PPCContext* c = ts ? ts->context() : nullptr;
    if (c && thread->pcr_ptr()) {
      auto* pcr = memory->TranslateVirtual<rex::system::X_KPCR*>(thread->pcr_ptr());
      const uint32_t stack_high = pcr->stack_base_ptr, stack_low = pcr->stack_end_ptr;
      g.stack = fmt::format("lr={}  r1={:08X} (stack {:08X}-{:08X})", g_flight_names.Guest(uint32_t(c->lr)),
                            c->r1.u32, stack_low, stack_high);
      uint32_t sp = c->r1.u32;
      for (int depth = 0; depth < 24; ++depth) {
        if (sp < stack_low || sp + 4 > stack_high) break;
        const uint32_t caller_sp = rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(sp));
        if (caller_sp <= sp || caller_sp > stack_high || caller_sp - 8 < stack_low) break;
        g.stack += fmt::format("\n      <- {}", g_flight_names.Guest(rex::memory::load_and_swap<uint32_t>(
                                                    memory->TranslateVirtual(caller_sp - 8))));
        sp = caller_sp;
      }
    }
    out.push_back(std::move(g));
  }
  return out;
}

// The log number is taken at startup when the recorder is on then, else at the first dump (a live toggle).
inline std::string FlightLogsPath() {
  if (g_flight_log_number < 0) g_flight_log_number = CurrentLogNumber();
  char name[40];
  std::snprintf(name, sizeof(name), "flight_%03d.txt", g_flight_log_number >= 0 ? g_flight_log_number : 0);
  return (std::filesystem::current_path() / "logs" / name).string();
}

// reason: "hang", "crash", "abort", "requested". detail: one line. crash_context: the faulting thread's context
// (crash dumps), else null. samples: host stack samples per thread (0 = none).
inline void FlightDump(const char* reason, const std::string& detail, const void* crash_context, int samples) {
#ifdef _WIN32
  if (t_flight_in_dump) return;  // a fault inside the dump itself
  t_flight_in_dump = true;
  std::lock_guard<std::mutex> lock(g_flight_dump_mutex);
  const int64_t start = FlightNow();
  const uint64_t frame = g_guest_frame.load();
  const int number = ++g_flight_dump_count;
  g_flight_names.Load();
  // Sample first (the state of the moment), name afterwards.
  std::vector<FlightHostThread> host = FlightSampleThreads(samples, 300);
  // The guest thread list takes ReXGlue's object table lock, which a stuck thread might hold: read it on a helper
  // thread and give up after 2 s (the helper is left to finish on its own; its result is then dropped).
  std::vector<FlightGuestThread> guests;
  bool guests_timed_out = false;
  {
    auto task = std::make_shared<std::packaged_task<std::vector<FlightGuestThread>()>>(FlightGuestThreads);
    std::future<std::vector<FlightGuestThread>> future = task->get_future();
    std::thread([task] { (*task)(); }).detach();
    if (future.wait_for(std::chrono::seconds(2)) == std::future_status::ready) guests = future.get();
    else guests_timed_out = true;
  }
  FlightLogsPath();  // sets g_flight_log_number now if the recorder was turned on after startup
  FlightStack crash_stack;
  if (crash_context) {
    const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    crash_stack.depth = FlightUnwindSafe(static_cast<const CONTEXT*>(crash_context),
                                         reinterpret_cast<uintptr_t>(tib->StackBase),
                                         reinterpret_cast<uintptr_t>(tib->StackLimit), crash_stack.rips,
                                         kFlightStackDepth);
  }

  // Copy every ring first (calls in progress and the newest events), THEN take "now": every copied start time is
  // earlier than it, so no age is negative and the summary and the per-thread sections show the same copy.
  const size_t per_thread = size_t(std::clamp(REXCVAR_GET(wwe2k14_flight_dump_events), 1, int(kFlightRingSize)));
  const uint32_t main_tid = g_swap_thread_id.load();
  const uint32_t self_tid = GetCurrentThreadId();
  struct InCallCopy {
    uint16_t kind;
    int64_t qpc;
    uint32_t frame, lr, a, b, c, d;
  };
  struct RingView {
    FlightRing* ring = nullptr;
    std::vector<InCallCopy> calls;
    std::vector<FlightEventCopy> events;  // newest first
    uint64_t head = 0;
    bool exited = false;                       // called ExTerminateThread
    FlightHostThread* host = nullptr;          // the live host thread (same id, created before the ring), or null
    const FlightGuestThread* guest = nullptr;  // the live guest thread (same host and guest id), or null
    bool gone = false;                         // no live thread is this ring's thread
    double blocked_s = -1.0;                   // oldest call in progress, -1 = none
    double last_s = 1e9;                       // newest event, 1e9 = none
  };
  std::vector<RingView> rings;
  const uint32_t ring_count = std::min<uint32_t>(g_flight_ring_count.load(), kFlightMaxRings);
  for (uint32_t i = 0; i < ring_count; ++i) {
    FlightRing* r = g_flight_rings[i].load(std::memory_order_acquire);
    if (!r) continue;
    RingView v;
    v.ring = r;
    for (int k = 0; k < kFlightMaxDepth; ++k) {
      const FlightInCall& c = r->calls[k];
      if (!c.active.load(std::memory_order_acquire)) continue;
      const InCallCopy copy{c.kind, c.qpc, c.frame, c.lr, c.a, c.b, c.c, c.d};
      std::atomic_thread_fence(std::memory_order_acquire);
      if (!c.active.load(std::memory_order_relaxed)) continue;  // the call returned while we copied
      v.calls.push_back(copy);
    }
    v.head = r->head.load(std::memory_order_acquire);
    v.events = FlightReadRing(*r, r->host_tid == main_tid ? std::min(per_thread * 4, kFlightRingSize) : per_thread);
    v.exited = r->exited.load(std::memory_order_acquire) != 0;
    rings.push_back(std::move(v));
  }
  const auto milestones = FlightReadEvents(g_flight_milestones.events, kFlightMilestones,
                                           g_flight_milestones.head.load(std::memory_order_acquire),
                                           std::min(per_thread * 4, kFlightMilestones), g_flight_milestones.tids);
  const uint64_t milestone_total = g_flight_milestones.head.load();

  const int64_t now = FlightNow();
  auto age = [&](int64_t qpc) { return std::max(0.0, FlightQpcSeconds(now - qpc)); };

  // Match rings to live threads. Windows reuses thread ids, so a host thread must also have been created before the
  // ring (0.1 s slack for clock granularity); a guest thread must match the ring's guest id too.
  constexpr uint64_t kCreatedSlack = 1000000;  // 0.1 s in FILETIME units
  for (RingView& v : rings) {
    const FlightRing& r = *v.ring;
    if (!v.exited) {
      for (FlightHostThread& t : host)
        if (t.id == r.host_tid && (!t.created || !r.created || t.created <= r.created + kCreatedSlack)) v.host = &t;
      // The dumping thread isn't sampled; any other ring without a live host thread belongs to a thread that ended.
      v.gone = !v.host && !host.empty() && r.host_tid != self_tid;
      if (!v.gone)
        for (const FlightGuestThread& g : guests)
          if (g.host_tid == r.host_tid && (!r.guest_tid || g.guest_tid == r.guest_tid)) v.guest = &g;
    }
    for (const InCallCopy& c : v.calls) v.blocked_s = std::max(v.blocked_s, age(c.qpc));
    if (!v.events.empty()) v.last_s = age(v.events.front().qpc);
  }

  const int64_t last_swap = g_flight_last_swap_qpc.load();
  std::string out = fmt::format(
      "==== wwe2k14 flight recorder dump #{}: {} ====\n{}\nrun log wwe2k14_{:03}.log, guest frame {}, last frame "
      "presented {:.3f} s before the dump started, dumping thread {}, stack sampling + ring copy took {:.3f} s\nnames: "
      "{} map symbols, DbgHelp {}; events newest first (in the order they completed), times are seconds before the "
      "rings were copied (after sampling), (+N) = call duration, 'from' = guest caller (ctx.lr)\n",
      number, reason, detail, g_flight_log_number, frame, last_swap ? FlightQpcSeconds(start - last_swap) : -1.0,
      self_tid, FlightQpcSeconds(now - start), g_flight_names.host_symbols(), g_flight_names.dbghelp() ? "on" : "off");

  // Main thread first, then live threads blocked longest, then by latest activity; exited / gone threads last.
  auto dead = [](const RingView& v) { return v.exited || v.gone; };
  std::sort(rings.begin(), rings.end(), [&](const RingView& x, const RingView& y) {
    const bool xm = x.ring->host_tid == main_tid && !dead(x), ym = y.ring->host_tid == main_tid && !dead(y);
    if (xm != ym) return xm;
    if (dead(x) != dead(y)) return !dead(x);
    if (x.blocked_s != y.blocked_s) return x.blocked_s > y.blocked_s;
    return x.last_s < y.last_s;
  });
  auto ring_name = [&](const RingView& v) -> std::string {
    if (v.host && !v.host->name.empty()) return v.host->name;
    if (v.guest && !v.guest->name.empty()) return v.guest->name;
    if (v.ring->host_tid == self_tid && !dead(v)) {
      std::string own = FlightThreadDescription(self_tid);
      if (!own.empty()) return own;
    }
    // An exited thread's XThread object can outlive it: same host AND guest id = the same thread (name only).
    for (const FlightGuestThread& g : guests)
      if (v.ring->guest_tid && g.guest_tid == v.ring->guest_tid && g.host_tid == v.ring->host_tid && !g.name.empty())
        return g.name;
    return v.ring->name;  // the XThread's name when the ring was made
  };
  auto active_calls = [&](const RingView& v) {
    std::string s;
    for (const InCallCopy& c : v.calls)
      s += fmt::format("{}{} for {:.3f} s (since frame {}) from {}", s.empty() ? "" : "; then ",
                       FlightDescribe(c.kind, c.a, c.b, c.c, c.d, {}), age(c.qpc), c.frame,
                       g_flight_names.Guest(c.lr));
    return s;
  };
  auto last_event = [&](const RingView& v) {
    return v.last_s < 1e9 ? fmt::format("last event {:.3f} s ago", v.last_s) : std::string("no events");
  };

  out += "\n-- summary (one line per guest thread with a ring) --\n";
  for (const RingView& v : rings) {
    const FlightRing& r = *v.ring;
    const std::string calls = active_calls(v);
    std::string state;
    if (v.exited)
      state = fmt::format("exited at frame {} (ExTerminateThread code {}); {}", r.exit_frame, r.exit_code, last_event(v));
    else if (v.gone)
      state = fmt::format("gone (no live thread with this id and creation time; ended without ExTerminateThread); {}{}",
                          last_event(v), calls.empty() ? "" : "; unfinished call: " + calls);
    else if (!calls.empty())
      state = "IN " + calls;
    else
      state = v.last_s < 1e9 ? "running; " + last_event(v) : last_event(v);
    out += fmt::format("  tid {:>6} guest {:>3} {:<28} {}{}\n", r.host_tid, r.guest_tid, ring_name(v).substr(0, 28),
                       r.host_tid == main_tid && !dead(v) ? "[main] " : "", state);
  }

  if (crash_context) {
    out += fmt::format("\n-- faulting thread {} ({}): host stack from the exception context --\n", self_tid,
                       FlightThreadDescription(self_tid));
    for (uint32_t f = 0; f < crash_stack.depth; ++f)
      out += fmt::format("    #{:02} {}\n", f, g_flight_names.Host(crash_stack.rips[f]));
  }

  auto event_line = [&](const FlightEventCopy& e, bool with_tid) {
    std::string line = fmt::format("    -{:9.6f}s f{:<6} {}{}", age(e.qpc), e.frame,
                                   with_tid ? fmt::format("tid {:>6}  ", e.tid) : std::string(),
                                   FlightDescribe(e.kind, e.a, e.b, e.c, e.d, e.text));
    if (FlightHasResult(e.kind)) line += fmt::format(" -> 0x{:X}", e.e);
    if (e.dur_us) line += fmt::format(" (+{:.3f} ms)", e.dur_us / 1000.0);
    if (e.lr && e.kind != kFlightSwap) line += " from " + g_flight_names.Guest(e.lr);
    return line + "\n";
  };
  out += fmt::format("\n-- milestones, all threads (script steps, file opens, thread create/exit, tests): "
                     "last {} of {} --\n",
                     milestones.size(), milestone_total);
  for (const FlightEventCopy& e : milestones) out += event_line(e, true);

  for (const RingView& v : rings) {
    const FlightRing& r = *v.ring;
    out += fmt::format("\n-- thread {} \"{}\" guest thread {}{} --\n", r.host_tid, ring_name(v), r.guest_tid,
                       v.exited                                ? " [exited]"
                       : v.gone                                ? " [gone]"
                       : r.host_tid == main_tid ? " [main: the VdSwap caller]"
                                                               : "");
    const std::string calls = active_calls(v);
    if (!calls.empty()) out += (dead(v) ? "  UNFINISHED CALL: " : "  IN CALL: ") + calls + "\n";
    if (v.host) FlightAppendStacks(out, v.host->samples, "  ");
    if (v.guest && !v.guest->stack.empty()) out += "  guest stack: " + v.guest->stack + "\n";
    out += fmt::format("  last {} events (ring has {} so far):\n", v.events.size(), v.head);
    for (const FlightEventCopy& e : v.events) out += event_line(e, false);
  }

  // Guest threads that never recorded anything, then the host-only threads.
  out += guests_timed_out ? "\n-- guest thread list: TIMED OUT after 2 s (object table lock busy?); no guest stacks "
                            "in this dump --\n"
                          : "\n-- guest threads without events --\n";
  for (const FlightGuestThread& g : guests) {
    // Skip threads shown with a ring, including exited ones whose XThread object is still alive (same host and
    // guest id as the ring).
    if (std::any_of(rings.begin(), rings.end(), [&](const RingView& v) {
          return v.guest == &g || (dead(v) && v.ring->guest_tid && v.ring->guest_tid == g.guest_tid &&
                                   v.ring->host_tid == g.host_tid);
        }))
      continue;
    out += fmt::format("  thread {} \"{}\" guest thread {}\n", g.host_tid, g.name, g.guest_tid);
    for (FlightHostThread& h : host)
      if (h.id == g.host_tid) FlightAppendStacks(out, h.samples, "    ");
    if (!g.stack.empty()) out += "    guest stack: " + g.stack + "\n";
  }
  out += "\n-- host threads (not guest threads) --\n";
  for (FlightHostThread& h : host) {
    const bool guest = std::any_of(guests.begin(), guests.end(), [&](const FlightGuestThread& g) { return g.host_tid == h.id; });
    const bool has_ring = std::any_of(rings.begin(), rings.end(), [&](const RingView& v) { return v.host == &h; });
    if (guest || has_ring) continue;
    out += fmt::format("  thread {} \"{}\"\n", h.id, h.name);
    FlightAppendStacks(out, h.samples, "    ");
  }

#if defined(WWE2K14_FLIGHT_GPU)
  out += "\n-- GPU checkpoint records (Vulkan command processor crash ring, newest first) --\n";
  out += rex::perf::flight::GpuRecords(uint32_t(std::max(0, REXCVAR_GET(wwe2k14_flight_gpu_dump_records))));
#else
  out += "\n-- GPU checkpoint records: not in this SDK build (rex/perf/flight.h) --\n";
#endif
  out += fmt::format("==== end of dump #{} ({:.3f} s to write) ====\n\n", number, FlightQpcSeconds(FlightNow() - start));

  const std::string path = FlightLogsPath();
  if (std::FILE* f = std::fopen(path.c_str(), "ab")) {
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
  }
  REXLOG_WARN("[wwe2k14][flight] dump #{} ({}) written to {}: {}", number, reason, path, detail);
  rex::FlushLogging();
  t_flight_in_dump = false;
#else
  (void)reason, (void)detail, (void)crash_context, (void)samples;
#endif
}

// crash_handler.h: an unhandled exception (info = EXCEPTION_POINTERS*).
inline void FlightDumpOnCrash(const void* exception_pointers) {
#ifdef _WIN32
  if (!FlightOn()) return;
  auto* info = static_cast<const EXCEPTION_POINTERS*>(exception_pointers);
  const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  const auto address = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
  std::string detail = fmt::format("unhandled exception 0x{:08X} at {:X} (wwe2k14.exe+0x{:X}) on thread {}",
                                   uint32_t(info->ExceptionRecord->ExceptionCode), address, address - base,
                                   GetCurrentThreadId());
  if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
    detail += fmt::format(", {} of address 0x{:X}", info->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                          info->ExceptionRecord->ExceptionInformation[1]);
  FlightDump("crash", detail, info->ContextRecord, 1);
#else
  (void)exception_pointers;
#endif
}

inline void FlightDumpOnAbort() {
  if (!FlightOn()) return;
#ifdef _WIN32
  FlightDump("abort", fmt::format("abort() called on thread {}", GetCurrentThreadId()), nullptr, 1);
#endif
}

// ---------------------------------------------------------------------------------------------------------------
// Watchdog, test triggers, frame hook
// ---------------------------------------------------------------------------------------------------------------
struct FlightTestTrigger {
  enum Kind { kHang, kCrash, kAbort, kDump, kBench } kind;
  uint64_t frame;
  uint32_t seconds;
  bool fired;
};
inline std::vector<FlightTestTrigger> g_flight_tests;

inline void FlightParseTests(const std::string& list) {
  size_t start = 0;
  while (start < list.size()) {
    size_t end = list.find(',', start);
    if (end == std::string::npos) end = list.size();
    const std::string item = list.substr(start, end - start);
    start = end + 1;
    const size_t at = item.find('@');
    if (at == std::string::npos) continue;
    const std::string what = item.substr(0, at);
    FlightTestTrigger t{FlightTestTrigger::kDump, std::strtoull(item.c_str() + at + 1, nullptr, 10), 15, false};
    if (const size_t colon = item.find(':', at); colon != std::string::npos)
      t.seconds = uint32_t(std::strtoul(item.c_str() + colon + 1, nullptr, 10));
    if (what == "hang") t.kind = FlightTestTrigger::kHang;
    else if (what == "crash") t.kind = FlightTestTrigger::kCrash;
    else if (what == "abort") t.kind = FlightTestTrigger::kAbort;
    else if (what == "dump") t.kind = FlightTestTrigger::kDump;
    else if (what == "bench") t.kind = FlightTestTrigger::kBench;
    else {
      REXLOG_WARN("[wwe2k14][flight] unknown test '{}' ignored", item);
      continue;
    }
    g_flight_tests.push_back(t);
    REXLOG_INFO("[wwe2k14][flight] test armed: {} at frame {}{}", what, t.frame,
                t.kind == FlightTestTrigger::kHang ? fmt::format(" for {} s", t.seconds) : "");
  }
}

// bench@FRAME: the recording cost, timed on the calling (main, guest) thread inside the VdSwap hook, so it pays the
// same path as a real hooked call (setting read, QueryPerformanceCounter x2, ThreadState lookup for ctx.lr, ring
// write). Stalls that one frame for ~20-50 ms and fills the main thread's ring with NtClose events (it refills in
// ~16 frames). Results go to the run log.
inline void FlightBench(uint64_t frame) {
  constexpr int kIterations = 200000;
  volatile uint32_t sink = 0;
  int64_t t0 = FlightNow();
  for (int i = 0; i < kIterations; ++i) {
    std::atomic_signal_fence(std::memory_order_seq_cst);  // compiler barrier: re-read the setting every iteration
    if (FlightOn()) sink = sink + 1;                      // the off path's cost: one setting read and a branch
  }
  const double check_ns = FlightQpcSeconds(FlightNow() - t0) * 1e9 / kIterations;
  t0 = FlightNow();
  for (int i = 0; i < kIterations; ++i) {
    FlightCall call(kFlightClose, uint32_t(i));
    sink = call.Done(0);
  }
  const double call_ns = FlightQpcSeconds(FlightNow() - t0) * 1e9 / kIterations;
  t0 = FlightNow();
  for (int i = 0; i < kIterations; ++i) FlightNote(kFlightPFree, uint32_t(i));
  const double note_ns = FlightQpcSeconds(FlightNow() - t0) * 1e9 / kIterations;
  // Event rate of the whole process so far (every ring's head), for the cost per second.
  uint64_t events = 0;
  const uint32_t ring_count = std::min<uint32_t>(g_flight_ring_count.load(), kFlightMaxRings);
  for (uint32_t i = 0; i < ring_count; ++i)
    if (FlightRing* r = g_flight_rings[i].load(std::memory_order_acquire)) events += r->head.load();
  events -= uint64_t(2) * kIterations;  // this bench's own events
  const double elapsed = g_flight_start_qpc ? FlightQpcSeconds(t0 - g_flight_start_qpc) : 0.0;
  REXLOG_WARN("[wwe2k14][flight] bench at frame {} ({} iterations each, main thread): setting check {:.2f} ns, "
              "kernel call record (FlightCall + Done) {:.1f} ns, point event (FlightNote) {:.1f} ns; {} events "
              "recorded by all threads in the {:.1f} s before the bench = {:.0f} events/s = {:.3f} ms of recording "
              "per second (at the FlightCall cost)",
              frame, kIterations, check_ns, call_ns, note_ns, events, elapsed, elapsed > 0 ? events / elapsed : 0.0,
              elapsed > 0 ? events / elapsed * call_ns * 1e-6 : 0.0);
}

inline void FlightRunTests(uint64_t frame) {
  for (FlightTestTrigger& t : g_flight_tests) {
    if (t.fired || frame < t.frame) continue;
    t.fired = true;
    switch (t.kind) {
      case FlightTestTrigger::kHang: {
        REXLOG_WARN("[wwe2k14][flight] test: blocking the main thread for {} s at frame {}", t.seconds, frame);
        FlightNote(kFlightTest, 0, 0, 0, 0, 0, "hang");
        FlightCall call(kFlightTestHang, t.seconds);
#ifdef _WIN32
        Sleep(t.seconds * 1000);
#endif
        call.Done(0);
        REXLOG_WARN("[wwe2k14][flight] test: main thread released (frame {})", frame);
        break;
      }
      case FlightTestTrigger::kCrash:
        REXLOG_WARN("[wwe2k14][flight] test: null-pointer write on the main thread at frame {}", frame);
        FlightNote(kFlightTest, 0, 0, 0, 0, 0, "crash");
        rex::FlushLogging();
        *reinterpret_cast<volatile uint32_t*>(g_flight_null + 0x10) = 0x0BADF11D;
        break;
      case FlightTestTrigger::kAbort:
        REXLOG_WARN("[wwe2k14][flight] test: abort() at frame {}", frame);
        FlightNote(kFlightTest, 0, 0, 0, 0, 0, "abort");
        rex::FlushLogging();
        std::abort();
      case FlightTestTrigger::kDump:
        FlightNote(kFlightTest, 0, 0, 0, 0, 0, "dump");
        FlightDump("requested", fmt::format("--wwe2k14_flight_test dump at frame {}", frame), nullptr,
                   std::max(1, REXCVAR_GET(wwe2k14_flight_hang_samples)));
        break;
      case FlightTestTrigger::kBench:
        FlightNote(kFlightTest, 0, 0, 0, 0, 0, "bench");
        FlightBench(frame);
        break;
    }
  }
}

inline void FlightWatchdog() {
  uint64_t last_frame = 0;
  int64_t last_change = FlightNow();
  bool dumped = false;
  int hang_dumps = 0;
  for (;;) {
#ifdef _WIN32
    Sleep(250);
#endif
    const uint64_t frame = g_guest_frame.load();
    const int64_t now = FlightNow();
    if (frame != last_frame) {
      last_frame = frame;
      last_change = now;
      dumped = false;
      continue;
    }
    const int limit = REXCVAR_GET(wwe2k14_flight_hang_seconds);
    if (!FlightOn() || limit <= 0 || frame == 0 || dumped) continue;
    const double stalled = FlightQpcSeconds(now - last_change);
    if (stalled < limit) continue;
    dumped = true;
    const int max_dumps = REXCVAR_GET(wwe2k14_flight_max_hang_dumps);
    if (max_dumps > 0 && hang_dumps >= max_dumps) {
      REXLOG_WARN("[wwe2k14][flight] no frame for {:.1f} s at frame {}: hang dump skipped (limit of {} per run reached)",
                  stalled, frame, max_dumps);
      continue;
    }
    ++hang_dumps;
    FlightDump("hang", fmt::format("no frame presented for {:.1f} s (limit {} s); last frame {}", stalled, limit, frame),
               nullptr, std::clamp(REXCVAR_GET(wwe2k14_flight_hang_samples), 1, 20));
  }
}

// OnPostSetup. Watchdog, tests and GPU records only when on at startup; the log number is taken now (the newest
// wwe2k14_NNN.log), or at the first dump after a live toggle (FlightLogsPath).
inline void StartFlightRecorder() {
  if (!FlightOn()) return;
  g_flight_start_qpc = FlightNow();
  g_flight_log_number = CurrentLogNumber();
  // Our SDK patch's GPU checkpoint ring access (rex/perf/flight.h): its provider answers only with this on.
  if (rex::cvar::GetFlagInfo("vulkan_flight_gpu_records") &&
      rex::cvar::GetFlagSource("vulkan_flight_gpu_records") == rex::cvar::Source::kDefault) {
    rex::cvar::SetFlagByName("vulkan_flight_gpu_records", "true");
  }
  FlightParseTests(REXCVAR_GET(wwe2k14_flight_test));
  REXLOG_INFO("[wwe2k14][flight] recorder on: {} events per thread ring, hang watchdog {} s, dumps -> {}",
              kFlightRingSize, REXCVAR_GET(wwe2k14_flight_hang_seconds), FlightLogsPath());
  std::thread(FlightWatchdog).detach();
}

// VdSwap hook, main thread, every frame (before the frame is handed to ReXGlue).
inline void FlightOnSwap(uint64_t frame) {
  if (!FlightOn()) return;
  g_flight_last_swap_qpc.store(FlightNow(), std::memory_order_relaxed);
  FlightNote(kFlightSwap, uint32_t(frame));
  if (!g_flight_tests.empty()) FlightRunTests(frame);
}

// input_script.h, when a step starts. op = int(InputScript::Op) (names as of Session 8; newer ops print a number).
inline void FlightOnScriptStep(int line, int op, std::string_view text) {
  if (!FlightOn()) return;
  static constexpr const char* kOps[] = {"wait_frames", "wait_file", "set/press", "screenshot", "log", "on_fail",
                                         "quit", "watch_zero", "cvar", "mark", "fail_if_file", "clear_fail_if",
                                         "press_until_file", "wait_state", "assert_state", "log_state", "watch/snap"};
  const std::string what = op >= 0 && op < int(std::size(kOps)) ? fmt::format("{} {}", kOps[op], text)
                                                                 : fmt::format("op{} {}", op, text);
  FlightNote(kFlightScript, uint32_t(line), uint32_t(op), 0, 0, 0, what);
}

inline void FlightOnFileOpen(bool open_call, const std::string& path, uint32_t disposition, uint32_t access,
                             uint32_t handle, uint32_t result) {
  if (!FlightOn()) return;
  FlightNote(kFlightFileOpen, open_call ? 1 : 0, disposition, access, handle, result, path);
}

// ---------------------------------------------------------------------------------------------------------------
// Kernel import overrides (REX_HOOK below). Each calls ReXGlue's own implementation unchanged.
// ---------------------------------------------------------------------------------------------------------------
namespace xk = rex::kernel::xboxkrnl;

inline uint32_t FlightNtWaitForSingleObjectEx(uint32_t handle, uint32_t wait_mode, uint32_t alertable,
                                              mapped_u64 timeout_ptr) {
  if (!FlightOn()) return xk::NtWaitForSingleObjectEx_entry(handle, wait_mode, alertable, timeout_ptr);
  FlightCall call(kFlightNtWait, handle, FlightTimeoutMs(timeout_ptr), alertable);
  return call.Done(xk::NtWaitForSingleObjectEx_entry(handle, wait_mode, alertable, timeout_ptr));
}
inline uint32_t FlightKeWaitForSingleObject(mapped_void object_ptr, uint32_t wait_reason, uint32_t processor_mode,
                                            uint32_t alertable, mapped_u64 timeout_ptr) {
  if (!FlightOn()) return xk::KeWaitForSingleObject_entry(object_ptr, wait_reason, processor_mode, alertable, timeout_ptr);
  FlightCall call(kFlightKeWait, object_ptr.guest_address(), FlightTimeoutMs(timeout_ptr), alertable, wait_reason);
  return call.Done(xk::KeWaitForSingleObject_entry(object_ptr, wait_reason, processor_mode, alertable, timeout_ptr));
}
inline uint32_t FlightKeWaitForMultipleObjects(uint32_t count, mapped_u32 objects_ptr, uint32_t wait_type,
                                               uint32_t wait_reason, uint32_t processor_mode, uint32_t alertable,
                                               mapped_u64 timeout_ptr, mapped_void wait_block_array_ptr) {
  if (!FlightOn())
    return xk::KeWaitForMultipleObjects_entry(count, objects_ptr, wait_type, wait_reason, processor_mode, alertable,
                                              timeout_ptr, wait_block_array_ptr);
  FlightCall call(kFlightKeWaitMulti, count, FlightTimeoutMs(timeout_ptr), wait_type,
                  objects_ptr && count ? uint32_t(*objects_ptr) : 0);
  return call.Done(xk::KeWaitForMultipleObjects_entry(count, objects_ptr, wait_type, wait_reason, processor_mode,
                                                      alertable, timeout_ptr, wait_block_array_ptr));
}
inline uint32_t FlightNtWaitForMultipleObjectsEx(uint32_t count, mapped_u32 handles, uint32_t wait_type,
                                                 uint32_t wait_mode, uint32_t alertable, mapped_u64 timeout_ptr) {
  if (!FlightOn())
    return xk::NtWaitForMultipleObjectsEx_entry(count, handles, wait_type, wait_mode, alertable, timeout_ptr);
  FlightCall call(kFlightNtWaitMulti, count, FlightTimeoutMs(timeout_ptr), wait_type,
                  handles && count ? uint32_t(*handles) : 0);
  return call.Done(xk::NtWaitForMultipleObjectsEx_entry(count, handles, wait_type, wait_mode, alertable, timeout_ptr));
}
inline uint32_t FlightKeDelayExecutionThread(uint32_t processor_mode, uint32_t alertable, mapped_u64 interval_ptr) {
  // Session 8 task B: wwe2k14::sleep::Delay (precise_sleep.h) = ReXGlue's KeDelayExecutionThread_entry unless
  // --wwe2k14_precise_sleep / --wwe2k14_sleep_stats (both default off).
  if (!FlightOn()) return wwe2k14::sleep::Delay(processor_mode, alertable, interval_ptr);
  uint32_t us = 0xFFFFFFFFu;
  if (interval_ptr) {
    const int64_t t = int64_t(uint64_t(*interval_ptr));
    if (t <= 0) us = uint32_t(std::min<int64_t>(-t / 10, 0xFFFFFFFEll));
  }
  FlightCall call(kFlightDelay, us, 0, alertable);
  return call.Done(wwe2k14::sleep::Delay(processor_mode, alertable, interval_ptr));
}
inline uint32_t FlightKeSetEvent(ppc_ptr_t<rex::system::X_KEVENT> event_ptr, uint32_t increment, uint32_t wait) {
  if (!FlightOn()) return xk::KeSetEvent_entry(event_ptr, increment, wait);
  FlightCall call(kFlightKeSetEvent, event_ptr.guest_address());
  return call.Done(xk::KeSetEvent_entry(event_ptr, increment, wait));
}
inline uint32_t FlightKeResetEvent(ppc_ptr_t<rex::system::X_KEVENT> event_ptr) {
  if (!FlightOn()) return xk::KeResetEvent_entry(event_ptr);
  FlightCall call(kFlightKeResetEvent, event_ptr.guest_address());
  return call.Done(xk::KeResetEvent_entry(event_ptr));
}
inline uint32_t FlightNtCreateEvent(mapped_u32 handle_ptr, ppc_ptr_t<rex::system::X_OBJECT_ATTRIBUTES> attrs,
                                    uint32_t event_type, uint32_t initial_state) {
  if (!FlightOn()) return xk::NtCreateEvent_entry(handle_ptr, attrs, event_type, initial_state);
  FlightCall call(kFlightNtCreateEvent, event_type, initial_state);
  const uint32_t result = xk::NtCreateEvent_entry(handle_ptr, attrs, event_type, initial_state);
  call.SetD(result == 0 && handle_ptr ? uint32_t(*handle_ptr) : 0);
  return call.Done(result);
}
inline uint32_t FlightNtSetEvent(uint32_t handle, mapped_u32 previous_state_ptr) {
  if (!FlightOn()) return xk::NtSetEvent_entry(handle, previous_state_ptr);
  FlightCall call(kFlightNtSetEvent, handle);
  return call.Done(xk::NtSetEvent_entry(handle, previous_state_ptr));
}
inline uint32_t FlightNtClearEvent(uint32_t handle) {
  if (!FlightOn()) return xk::NtClearEvent_entry(handle);
  FlightCall call(kFlightNtClearEvent, handle);
  return call.Done(xk::NtClearEvent_entry(handle));
}
inline uint32_t FlightNtReleaseSemaphore(uint32_t handle, uint32_t count, mapped_u32 previous_count_ptr) {
  if (!FlightOn()) return xk::NtReleaseSemaphore_entry(handle, count, previous_count_ptr);
  FlightCall call(kFlightSemRelease, handle, count);
  return call.Done(xk::NtReleaseSemaphore_entry(handle, count, previous_count_ptr));
}
inline uint32_t FlightExCreateThread(mapped_u32 handle_ptr, uint32_t stack_size, mapped_u32 thread_id_ptr,
                                     uint32_t xapi_thread_startup, mapped_void start_address,
                                     mapped_void start_context, uint32_t creation_flags) {
  if (!FlightOn())
    return xk::ExCreateThread_entry(handle_ptr, stack_size, thread_id_ptr, xapi_thread_startup, start_address,
                                    start_context, creation_flags);
  FlightCall call(kFlightThreadCreate, start_address.guest_address(), start_context.guest_address(), creation_flags);
  const uint32_t result = xk::ExCreateThread_entry(handle_ptr, stack_size, thread_id_ptr, xapi_thread_startup,
                                                   start_address, start_context, creation_flags);
  call.SetD(result == 0 && handle_ptr ? uint32_t(*handle_ptr) : 0);
  return call.Done(result);
}
inline uint32_t FlightNtResumeThread(uint32_t handle, mapped_u32 suspend_count_ptr) {
  if (!FlightOn()) return xk::NtResumeThread_entry(handle, suspend_count_ptr);
  FlightCall call(kFlightThreadResume, handle);
  return call.Done(xk::NtResumeThread_entry(handle, suspend_count_ptr));
}
inline uint32_t FlightExTerminateThread(uint32_t exit_code) {
  FlightNote(kFlightThreadExit, exit_code);  // the call doesn't return
  if (FlightRing* r = t_flight_ring) {       // a thread that recorded: dumps then say "exited", not "running"
    r->exit_code = exit_code;
    r->exit_frame = uint32_t(g_guest_frame.load(std::memory_order_relaxed));
    r->exited.store(1, std::memory_order_release);
  }
  return xk::ExTerminateThread_entry(exit_code);
}
inline uint32_t FlightNtAllocateVirtualMemory(mapped_u32 base_addr_ptr, mapped_u32 region_size_ptr, uint32_t alloc_type,
                                              uint32_t protect_bits, uint32_t debug_memory) {
  if (!FlightOn())
    return xk::NtAllocateVirtualMemory_entry(base_addr_ptr, region_size_ptr, alloc_type, protect_bits, debug_memory);
  FlightCall call(kFlightVAlloc, base_addr_ptr ? uint32_t(*base_addr_ptr) : 0,
                  region_size_ptr ? uint32_t(*region_size_ptr) : 0, alloc_type, protect_bits);
  const uint32_t result =
      xk::NtAllocateVirtualMemory_entry(base_addr_ptr, region_size_ptr, alloc_type, protect_bits, debug_memory);
  call.Set(base_addr_ptr ? uint32_t(*base_addr_ptr) : 0, region_size_ptr ? uint32_t(*region_size_ptr) : 0,
           alloc_type, protect_bits);
  return call.Done(result);
}
inline uint32_t FlightNtFreeVirtualMemory(mapped_u32 base_addr_ptr, mapped_u32 region_size_ptr, uint32_t free_type,
                                          uint32_t debug_memory) {
  if (!FlightOn()) return xk::NtFreeVirtualMemory_entry(base_addr_ptr, region_size_ptr, free_type, debug_memory);
  FlightCall call(kFlightVFree, base_addr_ptr ? uint32_t(*base_addr_ptr) : 0,
                  region_size_ptr ? uint32_t(*region_size_ptr) : 0, free_type);
  return call.Done(xk::NtFreeVirtualMemory_entry(base_addr_ptr, region_size_ptr, free_type, debug_memory));
}
inline void FlightMmFreePhysicalMemory(uint32_t type, uint32_t base_address) {
  FlightNote(kFlightPFree, base_address, type);
  xk::MmFreePhysicalMemory_entry(type, base_address);
}
inline uint32_t FlightNtClose(uint32_t handle) {
  if (!FlightOn()) return xk::NtClose_entry(handle);
  FlightCall call(kFlightClose, handle);
  return call.Done(xk::NtClose_entry(handle));
}
inline uint32_t FlightNtWriteFile(uint32_t file_handle, uint32_t event_handle, uint32_t apc_routine,
                                  mapped_void apc_context, ppc_ptr_t<rex::system::X_IO_STATUS_BLOCK> io_status_block,
                                  mapped_void buffer, uint32_t buffer_length, mapped_u64 byte_offset_ptr) {
  if (!FlightOn())
    return xk::NtWriteFile_entry(file_handle, event_handle, apc_routine, apc_context, io_status_block, buffer,
                                 buffer_length, byte_offset_ptr);
  FlightCall call(kFlightWrite, file_handle, buffer_length,
                  byte_offset_ptr ? uint32_t(uint64_t(*byte_offset_ptr)) : 0xFFFFFFFFu);
  return call.Done(xk::NtWriteFile_entry(file_handle, event_handle, apc_routine, apc_context, io_status_block, buffer,
                                         buffer_length, byte_offset_ptr));
}

}  // namespace wwe2k14

REX_HOOK(__imp__NtWaitForSingleObjectEx, wwe2k14::FlightNtWaitForSingleObjectEx)
REX_HOOK(__imp__KeWaitForSingleObject, wwe2k14::FlightKeWaitForSingleObject)
REX_HOOK(__imp__KeWaitForMultipleObjects, wwe2k14::FlightKeWaitForMultipleObjects)
REX_HOOK(__imp__NtWaitForMultipleObjectsEx, wwe2k14::FlightNtWaitForMultipleObjectsEx)
REX_HOOK(__imp__KeDelayExecutionThread, wwe2k14::FlightKeDelayExecutionThread)
REX_HOOK(__imp__KeSetEvent, wwe2k14::FlightKeSetEvent)
REX_HOOK(__imp__KeResetEvent, wwe2k14::FlightKeResetEvent)
REX_HOOK(__imp__NtCreateEvent, wwe2k14::FlightNtCreateEvent)
REX_HOOK(__imp__NtSetEvent, wwe2k14::FlightNtSetEvent)
REX_HOOK(__imp__NtClearEvent, wwe2k14::FlightNtClearEvent)
REX_HOOK(__imp__NtReleaseSemaphore, wwe2k14::FlightNtReleaseSemaphore)
REX_HOOK(__imp__ExCreateThread, wwe2k14::FlightExCreateThread)
REX_HOOK(__imp__NtResumeThread, wwe2k14::FlightNtResumeThread)
REX_HOOK(__imp__ExTerminateThread, wwe2k14::FlightExTerminateThread)
REX_HOOK(__imp__NtAllocateVirtualMemory, wwe2k14::FlightNtAllocateVirtualMemory)
REX_HOOK(__imp__NtFreeVirtualMemory, wwe2k14::FlightNtFreeVirtualMemory)
REX_HOOK(__imp__MmFreePhysicalMemory, wwe2k14::FlightMmFreePhysicalMemory)
REX_HOOK(__imp__NtClose, wwe2k14::FlightNtClose)
REX_HOOK(__imp__NtWriteFile, wwe2k14::FlightNtWriteFile)
