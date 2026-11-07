#pragma once
// Session 95 (PACE): the retry plan for APerformanceHint_createSession (src\android_perf_hint.h). Until Session 95 one failed create
// switched the whole performance hint off for the run (r192: createSession failed on a 642L, Android 14, then nothing for the rest of the run).
// A create can fail for a moment (a thread that just ended, the hint service not ready), so: tries 1 and 2 use the live busy threads
// (found again at each scan), tries 3 and 4 use the GPU thread alone (the one thread the hint matters for), then the hint stops with
// the last errno logged. No Android headers: tools\android\cpu_semantics\dynres_test.cpp runs this with a faked create.
#include <cstdint>
#include <vector>

namespace wwe2k14::perf_hint {

class CreateRetry {
 public:
  static constexpr int kMaxTries = 4, kLiveTries = 2;

  bool Stopped() const { return failures_ >= kMaxTries; }
  int Failures() const { return failures_; }
  int LastErrno() const { return last_errno_; }
  bool GpuAloneNext() const { return failures_ >= kLiveTries; }

  // The thread list for the next create: the live busy threads, or (after kLiveTries failures) the GPU thread alone; the busiest
  // thread when the GPU thread was not seen. Empty = nothing to create with this scan.
  std::vector<int32_t> Pick(const std::vector<int32_t>& live, int32_t gpu_tid, int32_t busiest_tid, bool force_alone = false) const {
    if (!GpuAloneNext() && !force_alone) return live;  // force_alone: a GPU-alone session already worked, keep it
    const int32_t only = gpu_tid > 0 ? gpu_tid : busiest_tid;
    return only > 0 ? std::vector<int32_t>{only} : std::vector<int32_t>{};
  }

  void Failed(int err) {
    ++failures_;
    last_errno_ = err;
  }
  void Succeeded() { failures_ = 0; }

 private:
  int failures_ = 0, last_errno_ = 0;
};

}  // namespace wwe2k14::perf_hint
