// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Stream-free build only (the cvar exists only there).
// S111 RBSWITCH: wwe2k14_rdr_readbacks is START-ONLY. It is read ONCE (the first call of On(), the first present / lock of the run, long before any
// input-script `cvar` step or inspector edit can act) and held for the run, so a read-back deferred by R1 / R7 is always delivered by the path that
// deferred it (a mid-run switch-off used to skip thumb_off::OnPresent while the game's save waited on the deferred object forever).
// EVERY reader of the switch calls readbacks_start::On(); a REXCVAR_GET of it anywhere else is a lint failure (tools\readback\rbc_check.py).
#pragma once
#include <atomic>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, wwe2k14_rdr_readbacks);

namespace wwe2k14::readbacks_start {

inline std::atomic<int> g_start{-1};   // -1 = not read yet, 0 off, 1 on

inline bool On() {
  int v = g_start.load(std::memory_order_relaxed);
  if (v < 0) {
    const int want = REXCVAR_GET(wwe2k14_rdr_readbacks) ? 1 : 0;
    g_start.compare_exchange_strong(v, want, std::memory_order_relaxed);   // a racing first reader: the winner's value stays
    v = g_start.load(std::memory_order_relaxed);
  }
  return v == 1;
}

}  // namespace wwe2k14::readbacks_start
