// S111 BLACKFRAME (docs\research\s111\BLACKFRAME.md): WHY the host showed black, per run of black frames. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// Decide (host_core.h) already says per frame what F is made of (Plan::source) and why (Plan::reason; Plan::cause = the branch that ASKED for a hold before it fell to black, Plan::mid_match).
// The [host] frames census only counts them per window (run 328, S110: "black_no_3d 59 / 86" and nothing of the reason). This keeps the per-frame verdict as RUNS: one line per change of
// reason, written when the run ends (a picture, another reason, or the exit):
//   [host] black frames A-B (n N, held H): REASON[ <- CAUSE][, mid_match][, fault_black] | inputs: ours O flat L no_scene S loading K never_fed [b ...] | why: TEXT
// A-B = the first / last guest frame of the run (the frame_tick guest frame: the frame column of frames_NNN.csv); n = frames that made F black; held = frames inside the run that presented
// F unchanged (still black) or presented nothing. ours / flat / no_scene and `why` are the run's first frame's (Facts, the host's why text); loading = the run's frames with a loading
// miss; never_fed = every never-fed record input bit of the run ("-" = none). After kMaxLines lines a run is only counted; SummaryLine (at exit) gives the totals.
// Read by tools\bench\run_read.py (BLACK FRAMES). No game / REX / Vulkan header: tools\renderer\host_core_unit.cpp builds it (BlackFrameArm).
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "host_core.h"

namespace wwe2k14::scene_host {

// What one handled frame put on the screen (SceneHost::Handle's present switch).
enum class ShownKind : uint8_t {
  Picture = 0,   // a 3D picture, drawn or held (Draw3D / Hold3D)
  Black,         // F cleared black (Source::Black)
  FaultBlack,    // a HoldF whose picture a draw fault had spoiled: F cleared black instead (scene_host.cpp direct_fault_black_)
  Unchanged,     // F presented as it was (HoldF), or nothing presented
};

class BlackSpans {
 public:
  static constexpr uint32_t kMaxLines = 400;   // run lines printed per process; later runs are only counted (SummaryLine)
  static constexpr size_t kWhyMax = 160;       // the why text kept per run
  bool Open() const { return open_; }
  // One handled frame. The caller calls it only when `shown` is Black / FaultBlack or Open() (a frame that is not black with no run open costs one test). Returns the line of the run
  // this frame ended ("" = none).
  std::string Note(uint64_t frame, ShownKind shown, const Plan& p, const Facts& f, const std::string& why) {
    if (shown == ShownKind::Unchanged) {
      if (open_) ++held_, last_ = frame;
      return {};
    }
    if (shown == ShownKind::Picture) return Close();
    const bool fault = shown == ShownKind::FaultBlack;
    std::string line;
    if (open_ && (p.reason != reason_ || p.cause != cause_ || p.mid_match != mid_ || fault != fault_)) line = Close();
    if (!open_) {
      open_ = true, first_ = frame, n_ = 0, held_ = 0, loading_ = 0, never_fed_ = 0;
      reason_ = p.reason, cause_ = p.cause, mid_ = p.mid_match, fault_ = fault;
      ours_ = f.ours, flat_ = f.flat, no_scene_ = f.game_draws_no_scene;
      why_ = why.size() > kWhyMax ? why.substr(0, kWhyMax) : why;
    }
    ++n_, last_ = frame;
    never_fed_ |= f.never_fed;
    if (f.loading) ++loading_;
    return line;
  }
  // Ends the open run: its line ("" when none is open, or past kMaxLines: then only counted).
  std::string Close() {
    if (!open_) return {};
    open_ = false;
    ++runs_, black_all_ += n_, held_all_ += held_;
    if (printed_ >= kMaxLines) return {};
    ++printed_;
    return Line();
  }
  // The totals (call after the last Close): "" when no frame was black.
  std::string SummaryLine() const {
    if (!runs_) return {};
    char b[200];
    std::snprintf(b, sizeof b, "[host] black spans %llu (frames black %llu, held %llu), lines printed %llu", (unsigned long long)runs_, (unsigned long long)black_all_,
                  (unsigned long long)held_all_, (unsigned long long)printed_);
    return b;
  }

 private:
  std::string Line() const {
    std::string what = ReasonName(reason_);
    if (cause_ != Reason::Ours && cause_ != reason_) what += std::string(" <- ") + ReasonName(cause_);
    if (mid_) what += ", mid_match";
    if (fault_) what += ", fault_black";
    std::string bits;
    for (unsigned b = 0; b < 32; ++b)
      if ((never_fed_ >> b) & 1u) bits += (bits.empty() ? "" : " ") + std::to_string(b);
    char head[160];
    std::snprintf(head, sizeof head, "[host] black frames %llu-%llu (n %llu, held %llu): ", (unsigned long long)first_, (unsigned long long)last_, (unsigned long long)n_, (unsigned long long)held_);
    char in[160];
    std::snprintf(in, sizeof in, " | inputs: ours %d flat %d no_scene %d loading %llu never_fed ", ours_ ? 1 : 0, flat_ ? 1 : 0, no_scene_ ? 1 : 0, (unsigned long long)loading_);
    return std::string(head) + what + in + (bits.empty() ? std::string("-") : "[" + bits + "]") + " | why: " + (why_.empty() ? std::string("-") : why_);
  }
  bool open_ = false, mid_ = false, fault_ = false, ours_ = false, flat_ = false, no_scene_ = false;
  Reason reason_ = Reason::Ours, cause_ = Reason::Ours;
  uint64_t first_ = 0, last_ = 0, n_ = 0, held_ = 0, loading_ = 0;
  uint32_t never_fed_ = 0;
  std::string why_;
  uint64_t runs_ = 0, black_all_ = 0, held_all_ = 0, printed_ = 0;
};

}  // namespace wwe2k14::scene_host
