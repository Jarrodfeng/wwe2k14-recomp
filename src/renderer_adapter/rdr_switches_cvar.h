// S102 lane S0-4: the cvar side of rdr_switches.h (the pure table). Included by frame_stats.h (the per-frame refresh; input_script.h and
// inspector.h reach NoteWriteCvar through it).
#pragma once
#include <rex/cvar.h>
#include <rex/logging.h>

#include "rdr_switches.h"

namespace rdr_sw {

inline bool CvarRead(const char* name, std::string& text, bool probe) {
  if (probe && !rex::cvar::GetFlagInfo(name)) return false;  // the lookup only in the first refresh: the table knows which settings exist
  text = rex::cvar::GetFlagByName(name);
  return true;
}
inline void CvarLog(const char* line) { REXLOG_INFO("{}", line); }

// Once per guest frame, on the swap thread. Gated on the registry's change counter: no write since the last frame = no registry read at all.
inline int RefreshFromCvars(uint64_t frame) { return Refresh(frame, &CvarRead, &CvarLog, rex::cvar::ChangeCount()); }
inline void NoteWriteCvar(const std::string& name, const std::string& value, uint64_t frame) { NoteWrite(name.c_str(), value.c_str(), frame, &CvarLog); }

}  // namespace rdr_sw
