// Session 101 (RENDERER_ARC.md 4 tool 1, arc rule "every game run saves its black box"): the input script asks for a case zip when a route
// quits, without including the case hook itself (bookmark.h owns its cvars; input_script.h is included by bookmark.h). bookmark.h's Install
// fills the pointer; the call takes the case and waits until the zip is written, so the process does not exit mid-write.
#pragma once
#include <atomic>

namespace case_script {

// failure: the route stopped on a failed step (trigger run_failure), else run_exit.
using Fn = void (*)(bool failure, const char* note);
inline std::atomic<Fn> g_take{nullptr};

inline void TakeAndWait(bool failure, const char* note) {
  if (Fn f = g_take.load(std::memory_order_relaxed)) f(failure, note);
}

}  // namespace case_script
