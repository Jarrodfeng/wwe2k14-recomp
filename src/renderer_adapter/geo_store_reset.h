// S103 IXF: a new geometry store (StartCapture / StartLive) must not inherit the memo of the old one.
//
// reader.h's geometry memo has a shortcut: an entry whose used_frame equals the current record frame "was already checked this frame" and
// hands out its stored ref without a lookup. g_record_frame only advances while recording, so the frame a capture stops on and the frame
// the next capture starts on were the SAME record frame: every draw whose entry was last used in the old capture's last frame took the
// shortcut and got the OLD capture's geometry.bin offsets (8 sealed captures, docs\research\s103\IX_index_overrun.md).
// The fix: starting a store advances the record frame and empties the memo (every ref in it names the store just cleared).
//
// Pure: src\renderer_adapter\geo_store_reset_test.cpp is the offline proof (with the plant: the reset left out).
#pragma once

#include <cstdint>

namespace rdr_adapter::geostore {

// The reader's "already used this record frame" shortcut.
inline bool UsedThisFrame(uint64_t used_frame, uint64_t record_frame) { return used_frame == record_frame; }

// Called where a store is cleared whole and a new one starts. memo = g_geo_memo (any map with clear()).
template <class Memo>
inline void BeginNewStore(uint64_t& record_frame, Memo& memo) {
  ++record_frame;
  memo.clear();
}

}  // namespace rdr_adapter::geostore
