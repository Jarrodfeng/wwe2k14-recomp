// wwe2k14 - lane 303: what the Create-an-Entrance editor asked its preview object for, as two atomics a script can read (no game, SDK or OS includes).
// The hook on SetSong (src\music_preview.h, sub_8334C138) calls Note(id) for EVERY call, before any decision: id = the music id of the row the editor
// just highlighted (a stock id, a new song 6001+, 9998 USER PLAYLIST, 9999 NONE). src\input_script.h `press_until_preview` navigates the music list by
// it, because the editor's list cursor is not a SmartMenu cursor (state `cursor` is unreadable there: run 7207). Costs two atomic stores per call.
#pragma once
#include <atomic>
#include <cstdint>

namespace wwe2k14::music_seen {
inline std::atomic<int32_t> last_id{-1};
inline std::atomic<uint32_t> calls{0};
inline void Note(int32_t id) {
  last_id.store(id, std::memory_order_relaxed);
  calls.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace wwe2k14::music_seen
