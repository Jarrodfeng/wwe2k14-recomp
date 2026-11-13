// lane 266: the ms each periodic (every-N-frames) job took, printed with every census window as one `[rdr][periodic]` line so a bench
// sees a stall's owner without a second run. Pure; reader.h feeds it.
#pragma once

#include <cstdint>
#include <string>

#include <fmt/format.h>

namespace rdr_adapter::periodic {

struct Job {
  double sum_ms = 0, max_ms = 0;
  uint32_t runs = 0;
  void Add(double ms) {
    sum_ms += ms;
    if (ms > max_ms) max_ms = ms;
    ++runs;
  }
};

struct Times {
  Job forget;  // LiveForgetGeometry, every present now (budgeted)
  Job memo;    // ForgetStaleMemo (every 600th, 300 out of phase)
  Job census;  // the census block of this window (names, scene census, case lines, the reader time line)
  Job cost;    // the cost / span lines (every 600th)
  uint64_t forget_erased = 0, forget_looked = 0, forget_requeued = 0, forget_time_capped = 0, forget_count_capped = 0;
  uint64_t queue_depth = 0;  // geometry keys waiting in the age queue at the end of the window

  std::string Line(uint64_t from, uint64_t to) const {
    auto job = [](const char* name, const Job& j) { return fmt::format("{} {:.2f} (worst {:.2f}, {} runs)", name, j.sum_ms, j.max_ms, j.runs); };
    return fmt::format(
        "[rdr][periodic] frames {}-{}: ms the periodic jobs took on the game's swap thread (sum over the window; worst single run): {}; {}; {}; {}; "
        "forget: {} keys erased, {} queue items looked at, {} used again and queued again, stopped by the time cap {}x, by the count cap {}x, {} keys waiting",
        from, to, job("geometry forget", forget), job("memo sweep", memo), job("census block", census), job("cost lines", cost), forget_erased,
        forget_looked, forget_requeued, forget_time_capped, forget_count_capped, queue_depth);
  }
  void Reset() { *this = Times{}; }
};

}  // namespace rdr_adapter::periodic
