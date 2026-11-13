#pragma once
// wwe2k14 - Session 101 lane 302: what the MACHINE was doing during a window, one log line every ~10 s next to [perf][mem].
//
//   [wwe2k14][perf][sys] t=30.0 s win=10.0 s sys_cpu=23 proc_cpu=11 cores=32 hard_faults_s=0 proc_faults_s=1450 free_mb=21030 commit_mb=19800 commit_limit_mb=52000 log_write_ms=0.3 log_call_ms=0.02 log_dropped=0 file_write_ms=1.2
//
// Why: run 7202 (2026-11-13 17:12) went 1 fps for 47 s because C: stalled under other agents' compiles; nothing in the log told "the PC was
// busy" from "our code stalled". Read this line first when a run is slow: sys_cpu high while proc_cpu is low = the PC (other processes);
// log_write_ms / file_write_ms in the hundreds = the disk; free_mb low / hard_faults_s high = memory pressure.
//
// Fields (a field the platform cannot read is left out, never 0):
//   win             seconds this window covers (the delta every rate and the maxima are over)
//   sys_cpu         whole-machine CPU busy %, 0-100 over ALL logical cores (Windows GetSystemTimes; Android /proc/stat)
//   proc_cpu        this process's CPU %, in the same units (100 = every logical core busy with us)
//   cores           logical cores
//   hard_faults_s   system-wide pages read from disk per second (NtQuerySystemInformation SystemPerformanceInformation.PageReadCount; Windows
//                   only; left out unless the struct passes its self-check against GlobalMemoryStatusEx, see Selftest)
//   proc_faults_s   this process's page faults per second, soft + hard (PROCESS_MEMORY_COUNTERS.PageFaultCount)
//   free_mb         available physical RAM (GlobalMemoryStatusEx ullAvailPhys; Android MemAvailable)
//   commit_mb / commit_limit_mb   system commit charge and limit (Windows)
//   log_write_ms    slowest single write the log's writer thread made in the window (rex::TakeLogWriteStats; the file sink's disk time)
//   log_call_ms     slowest log() call on any CALLING thread in the window (the queue push: this is what a game thread pays; ~0.00x)
//   log_dropped     info lines dropped on a full queue in the window (a stalled disk); warn / error are never dropped
//   file_write_ms   slowest file write noted by NoteFileWrite in the window (live pictures, the pipeline store save)
//
// Cost: one sample per window, taken by the [perf][mem] thread (perf_mem.h Emit). Header only; no rex dependency except the log counters.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <string>

#include "perf_file_write.h"

#if defined(_WIN32)
#include "platform/compat.h"  // <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif

namespace wwe2k14::perf_sys {

// (NoteFileWrite / FileWriteWorstUs: perf_file_write.h, dependency free so renderer code can call it.)

// Raw counters at one instant; Sample is derived from two of them.
struct Raw {
  std::chrono::steady_clock::time_point at{};
  uint64_t sys_busy = 0, sys_total = 0;    // 100 ns ticks (Windows) / jiffies (Android) summed over all cores
  uint64_t proc_ticks = 0;                 // same unit
  uint64_t proc_faults = 0;
  uint64_t page_reads = 0;                 // system-wide, pages; ~0 = unreadable
  double free_mb = -1, commit_mb = -1, commit_limit_mb = -1;
  int cores = 0;
  bool ok = false;
};

struct Sample {
  double win_s = 0;
  double sys_cpu = -1, proc_cpu = -1;
  int cores = -1;
  double hard_faults_s = -1, proc_faults_s = -1;
  double free_mb = -1, commit_mb = -1, commit_limit_mb = -1;
  double log_write_ms = -1, log_call_ms = -1;
  long long log_dropped = -1;
  double file_write_ms = -1;
};

#if defined(_WIN32)

namespace detail {
// SYSTEM_PERFORMANCE_INFORMATION (class 2) up to PageReadCount. Layout as in ntdll's public symbols / Process Hacker's phnt; the
// self-check in ReadRaw (AvailablePages vs GlobalMemoryStatusEx) drops the fields when it does not hold on this Windows build.
struct SysPerfInfo {
  LARGE_INTEGER IdleProcessTime, IoReadTransferCount, IoWriteTransferCount, IoOtherTransferCount;
  ULONG IoReadOperationCount, IoWriteOperationCount, IoOtherOperationCount;
  ULONG AvailablePages, CommittedPages, CommitLimit, PeakCommitment;
  ULONG PageFaultCount, CopyOnWriteCount, TransitionCount, CacheTransitionCount, DemandZeroCount;
  ULONG PageReadCount, PageReadIoCount;
  ULONG Padding[64];  // the real struct is longer; room so the call never overruns
};
inline bool QuerySysPerf(SysPerfInfo& out) {
  using Fn = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
  static Fn fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation")));
  if (!fn) return false;
  ULONG got = 0;
  return fn(2 /* SystemPerformanceInformation */, &out, sizeof(out), &got) >= 0 && got >= offsetof(SysPerfInfo, PageReadIoCount) + sizeof(ULONG);
}
inline uint64_t Ft(const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; }
}  // namespace detail

inline Raw ReadRaw() {
  Raw r;
  r.at = std::chrono::steady_clock::now();
  FILETIME idle{}, kernel{}, user{}, c{}, e{}, pk{}, pu{};
  if (!GetSystemTimes(&idle, &kernel, &user)) return r;
  const uint64_t i = detail::Ft(idle), k = detail::Ft(kernel), u = detail::Ft(user);  // kernel includes idle
  r.sys_total = k + u;
  r.sys_busy = (k + u) - i;
  if (GetProcessTimes(GetCurrentProcess(), &c, &e, &pk, &pu)) r.proc_ticks = detail::Ft(pk) + detail::Ft(pu);
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  r.cores = int(si.dwNumberOfProcessors);
  PROCESS_MEMORY_COUNTERS pmc{};
  pmc.cb = sizeof(pmc);
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) r.proc_faults = pmc.PageFaultCount;
  MEMORYSTATUSEX ms{};
  ms.dwLength = sizeof(ms);
  const bool have_ms = GlobalMemoryStatusEx(&ms) != 0;
  if (have_ms) r.free_mb = double(ms.ullAvailPhys) / (1024.0 * 1024.0);
  detail::SysPerfInfo sp{};
  r.page_reads = ~0ull;
  if (detail::QuerySysPerf(sp)) {
    const double page_mb = 4096.0 / (1024.0 * 1024.0);
    if (have_ms) {  // the struct is only trusted when its available pages match the documented API within 10 %
      const double avail = double(sp.AvailablePages) * page_mb;
      const double ref = r.free_mb;
      if (ref > 0 && avail > ref * 0.9 && avail < ref * 1.1) {
        r.page_reads = sp.PageReadCount;
        r.commit_mb = double(sp.CommittedPages) * page_mb;
        r.commit_limit_mb = double(sp.CommitLimit) * page_mb;
      }
    }
  }
  r.ok = true;
  return r;
}

#else  // Android / Linux: /proc

inline Raw ReadRaw() {
  Raw r;
  r.at = std::chrono::steady_clock::now();
  if (FILE* f = std::fopen("/proc/stat", "r")) {
    unsigned long long user = 0, nice = 0, sys = 0, idle = 0, iowait = 0, irq = 0, soft = 0, steal = 0;
    if (std::fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &user, &nice, &sys, &idle, &iowait, &irq, &soft, &steal) >= 4) {
      r.sys_total = user + nice + sys + idle + iowait + irq + soft + steal;
      r.sys_busy = r.sys_total - idle - iowait;
      r.ok = true;
    }
    std::fclose(f);
  }
  if (FILE* f = std::fopen("/proc/self/stat", "r")) {
    char buf[1024] = {};
    if (std::fgets(buf, sizeof(buf), f)) {
      const char* p = std::strrchr(buf, ')');  // fields after "(comm)": state is field 3; utime / stime are fields 14 / 15; minflt / majflt 10 / 12
      unsigned long long minflt = 0, majflt = 0, utime = 0, stime = 0;
      if (p && std::sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %llu %*u %llu %*u %llu %llu", &minflt, &majflt, &utime, &stime) == 4) {
        r.proc_ticks = utime + stime;
        r.proc_faults = minflt + majflt;
      }
    }
    std::fclose(f);
  }
  if (FILE* f = std::fopen("/proc/meminfo", "r")) {
    char line[128];
    while (std::fgets(line, sizeof(line), f)) {
      long long kb = 0;
      if (std::sscanf(line, "MemAvailable: %lld kB", &kb) == 1) r.free_mb = double(kb) / 1024.0;
    }
    std::fclose(f);
  }
  r.cores = int(sysconf(_SC_NPROCESSORS_ONLN));
  r.page_reads = ~0ull;
  r.ok = true;  // (/proc/stat may be denied: Derive then leaves the CPU fields out and still prints the rest)
  return r;
}

#endif

// Pure: the window between two readings. `log_*` come from rex::TakeLogWriteStats (the caller passes them; negative = unknown).
inline Sample Derive(const Raw& a, const Raw& b, double log_write_ms, double log_call_ms, long long log_dropped, double file_write_ms) {
  Sample s;
  s.win_s = std::chrono::duration<double>(b.at - a.at).count();
  if (!a.ok || !b.ok || s.win_s <= 0) return s;
  const double dtot = double(b.sys_total) - double(a.sys_total);
  if (dtot > 0 && b.sys_total > 0) {
    s.sys_cpu = 100.0 * (double(b.sys_busy) - double(a.sys_busy)) / dtot;
    // sys_total is summed over all cores, so proc ticks over the same total are already "percent of the whole machine".
    if (b.proc_ticks >= a.proc_ticks) s.proc_cpu = 100.0 * double(b.proc_ticks - a.proc_ticks) / dtot;
  }
  s.cores = b.cores > 0 ? b.cores : -1;
  if (b.proc_faults >= a.proc_faults) s.proc_faults_s = double(b.proc_faults - a.proc_faults) / s.win_s;
  if (a.page_reads != ~0ull && b.page_reads != ~0ull && b.page_reads >= a.page_reads) s.hard_faults_s = double(b.page_reads - a.page_reads) / s.win_s;
  s.free_mb = b.free_mb, s.commit_mb = b.commit_mb, s.commit_limit_mb = b.commit_limit_mb;
  s.log_write_ms = log_write_ms, s.log_call_ms = log_call_ms, s.log_dropped = log_dropped, s.file_write_ms = file_write_ms;
  return s;
}

// The line text; a missing field is left out. Pure.
inline std::string Format(const Sample& s, double t, bool exit_line) {
  std::string out;
  char buf[96];
  std::snprintf(buf, sizeof(buf), "[wwe2k14][perf][sys]%s t=%.1f s win=%.1f s", exit_line ? " exit" : "", t, s.win_s);
  out = buf;
  const auto f0 = [&](const char* name, double v) {
    if (v >= 0) {
      std::snprintf(buf, sizeof(buf), " %s=%.0f", name, v);
      out += buf;
    }
  };
  const auto f2 = [&](const char* name, double v) {
    if (v >= 0) {
      std::snprintf(buf, sizeof(buf), " %s=%.2f", name, v);
      out += buf;
    }
  };
  f0("sys_cpu", s.sys_cpu);
  f0("proc_cpu", s.proc_cpu);
  if (s.cores > 0) out += " cores=" + std::to_string(s.cores);
  f0("hard_faults_s", s.hard_faults_s);
  f0("proc_faults_s", s.proc_faults_s);
  f0("free_mb", s.free_mb);
  f0("commit_mb", s.commit_mb);
  f0("commit_limit_mb", s.commit_limit_mb);
  f2("log_write_ms", s.log_write_ms);
  f2("log_call_ms", s.log_call_ms);
  if (s.log_dropped >= 0) out += " log_dropped=" + std::to_string(s.log_dropped);
  f2("file_write_ms", s.file_write_ms);
  return out;
}

// Previous reading, kept by the (single) sampling thread.
inline Raw& Last() {
  static Raw r;
  return r;
}

// Called by perf_mem.h's Emit on its one thread. The first call only primes the counters (no line: a window needs two readings).
// `log_write_us` / `log_call_us` / `log_dropped_total` are rex::LogWriteStats fields (worst since the last take; total dropped).
inline bool Take(Sample& out, uint64_t log_write_us, uint64_t log_call_us, uint64_t log_dropped_total) {
  static uint64_t dropped_before = 0;
  Raw now = ReadRaw();
  Raw& last = Last();
  const bool primed = last.ok;
  const uint64_t file_us = FileWriteWorstUs().exchange(0, std::memory_order_relaxed);
  if (primed) {
    out = Derive(last, now, double(log_write_us) / 1000.0, double(log_call_us) / 1000.0, (long long)(log_dropped_total - dropped_before),
                 double(file_us) / 1000.0);
  }
  dropped_before = log_dropped_total;
  last = now;
  return primed && out.win_s > 0;
}

}  // namespace wwe2k14::perf_sys
