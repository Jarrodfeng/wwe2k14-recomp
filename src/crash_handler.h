// wwe2k14 - crash harness: on abort() or an unhandled exception, log what happened plus a backtrace of
// exe-relative offsets, then flush the log so the last lines aren't lost (CLAUDE.md: every crash gets a
// log and a backtrace). Resolve offsets with out/build/win-amd64-release/wwe2k14.map.

#pragma once

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#include <csignal>
#include <cstdint>
#include <string>

#include "guest_watch.h"
#include "flight_recorder.h"  // --wwe2k14_flight_recorder (Session 8 task 11, default off)

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace wwe2k14 {

#ifdef _WIN32
inline std::string ExeRelativeBacktrace(uint32_t skip) {
  void* frames[48];
  USHORT count = RtlCaptureStackBackTrace(skip, 48, frames, nullptr);
  auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  std::string out;
  for (USHORT i = 0; i < count; ++i) {
    auto pc = reinterpret_cast<uintptr_t>(frames[i]);
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(frames[i]), &module);
    char name[MAX_PATH] = "?";
    if (module) GetModuleFileNameA(module, name, MAX_PATH);
    const char* leaf = name;
    for (const char* p = name; *p; ++p)
      if (*p == '\\' || *p == '/') leaf = p + 1;
    auto mod_base = reinterpret_cast<uintptr_t>(module);
    out += fmt::format("\n    #{:02} {}+0x{:X}{}", i, leaf, pc - (module ? mod_base : 0),
                       reinterpret_cast<uintptr_t>(module) == base ? "  (look up in wwe2k14.map)" : "");
  }
  return out;
}

// The crashing guest thread's PPC registers and a walk of its stack, so a crash in game code shows the
// game's own state (e.g. which object pointer was null), not just host frames.
// Caveats: generated code may keep a register in a host register between calls, so ctx values can lag
// inside the faulting function (r1 may still be the caller's). The stack walk assumes the 360 convention
// that a function saves its return address at -8 from its caller's stack pointer (the __savegprlr
// helpers); stops at the first frame outside the thread's stack.
inline std::string GuestContextReport() {
  auto* thread = rex::system::XThread::GetCurrentThread();
  if (!thread || !thread->thread_state() || !thread->thread_state()->context()) return " (not a guest thread)";
  const PPCContext& c = *thread->thread_state()->context();
  const uint32_t r[32] = {c.r0.u32,  c.r1.u32,  c.r2.u32,  c.r3.u32,  c.r4.u32,  c.r5.u32,  c.r6.u32,
                          c.r7.u32,  c.r8.u32,  c.r9.u32,  c.r10.u32, c.r11.u32, c.r12.u32, c.r13.u32,
                          c.r14.u32, c.r15.u32, c.r16.u32, c.r17.u32, c.r18.u32, c.r19.u32, c.r20.u32,
                          c.r21.u32, c.r22.u32, c.r23.u32, c.r24.u32, c.r25.u32, c.r26.u32, c.r27.u32,
                          c.r28.u32, c.r29.u32, c.r30.u32, c.r31.u32};
  std::string out = fmt::format("\n    guest thread id {} registers (may lag inside the faulting function):",
                                thread->thread_id());
  for (int i = 0; i < 32; i += 4) {
    out += fmt::format("\n      r{:<2}={:08X}  r{:<2}={:08X}  r{:<2}={:08X}  r{:<2}={:08X}", i, r[i], i + 1,
                       r[i + 1], i + 2, r[i + 2], i + 3, r[i + 3]);
  }
  out += fmt::format("\n      lr={:08X}  ctr={:08X}  last_indirect_target={:08X}", uint32_t(c.lr), c.ctr.u32,
                     c.last_indirect_target);

  auto* memory = REX_KERNEL_MEMORY();
  auto* pcr = memory->TranslateVirtual<rex::system::X_KPCR*>(thread->pcr_ptr());
  const uint32_t stack_high = pcr->stack_base_ptr, stack_low = pcr->stack_end_ptr;
  out += fmt::format("\n    guest stack 0x{:08X}-0x{:08X}, return addresses:", stack_low, stack_high);
  uint32_t sp = r[1];
  int depth = 0;
  for (; depth < 40; ++depth) {
    if (sp < stack_low || sp + 4 > stack_high) break;
    const uint32_t caller_sp = rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(sp));
    if (caller_sp <= sp || caller_sp > stack_high || caller_sp - 8 < stack_low) break;
    out += fmt::format(" {:08X}",
                       rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(caller_sp - 8)));
    sp = caller_sp;
  }
  // Walk ended early: show the raw words at r1 (back chain first), e.g. to spot a corrupted frame.
  if (depth < 2 && r[1] >= stack_low && r[1] + 64 <= stack_high) {
    out += fmt::format("\n    walk stopped at depth {}; 16 words at r1=0x{:08X}:", depth, r[1]);
    for (uint32_t i = 0; i < 16; ++i) {
      out += fmt::format(" {:08X}", rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(r[1] + i * 4)));
    }
  }
  return out;
}

inline void OnAbortSignal(int) {
  REXLOG_CRITICAL("[wwe2k14][crash] abort() called. Backtrace:{}", ExeRelativeBacktrace(1));
  rex::FlushLogging();
  FlightDumpOnAbort();  // no-op unless --wwe2k14_flight_recorder
}

inline LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info) {
  auto* rec = info->ExceptionRecord;
  auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  auto addr = reinterpret_cast<uintptr_t>(rec->ExceptionAddress);
  REXLOG_CRITICAL("[wwe2k14][crash] unhandled exception 0x{:08X} at wwe2k14.exe+0x{:X} (if inside the exe). "
                  "Backtrace:{}",
                  static_cast<uint32_t>(rec->ExceptionCode), addr - base, ExeRelativeBacktrace(1));
  rex::FlushLogging();
  GuestWatchDrainLog();  // any watched-word zeroing not yet logged
  // Separate line and flush: if reading guest state faults, the backtrace above is already saved.
  REXLOG_CRITICAL("[wwe2k14][crash] guest context:{}", GuestContextReport());
  rex::FlushLogging();
  FlightDumpOnCrash(info);  // logs/flight_NNN.txt; no-op unless --wwe2k14_flight_recorder
  return EXCEPTION_CONTINUE_SEARCH;  // let Windows record it in the event log as before
}

inline void InstallCrashHandlers() {
  std::signal(SIGABRT, OnAbortSignal);
  SetUnhandledExceptionFilter(OnUnhandledException);
}
#else
inline void InstallCrashHandlers() {}
#endif

}  // namespace wwe2k14
