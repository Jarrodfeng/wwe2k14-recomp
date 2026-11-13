// wwe2k14 - the Create-an-Entrance MUSIC preview's decision logic (lane 281, 2026-11-13). Pure C++: no game, no
// platform; tools\audio\music_preview_unit.cpp tests it, src\music_preview.h wires it to the game (see there and
// docs\systems\music.md "Preview" for the game side).
//
// The game's preview object M has one setter, SetSong(M, id) (sub_8334C138). We decide per call:
//   - M is locked (the game ignores the call)                   -> nothing changes, the game gets the call as it is
//   - id has a file of ours (new song 6001+, or a replaced theme) -> ours plays, the game is told NONE (9999) so its own
//                                                                    preview stops (what NONE does), nothing else
//   - any other id (stock song, USER PLAYLIST 9998, NONE 9999)   -> ours stops if it plays, the game does its own
// and Stop / Shutdown / destruction of M stop ours; a frame check stops ours when M is gone or a match starts.
#pragma once

#include <cstdint>

namespace wwe2k14::music_preview {

constexpr int kNone = 9999;  // the MUSIC list's NONE record (docs\systems\music.md)

// Fault switches for the offline test only (the shipped code never sets one).
struct Faults {
  bool no_stop_on_switch = false;  // a switch to another song / a stock song leaves the old one playing
  bool no_stop_on_close = false;   // closing the menu (Stop / Shutdown / destruction) leaves the song playing
  bool game_keeps_id = false;      // the game is not told NONE: both its preview and ours play
  bool play_when_locked = false;   // a call the game ignores (M locked) still starts ours
  bool play_stock = false;         // a stock song is also taken over
};

struct Step {
  bool stop = false;      // stop our player first
  bool play = false;      // then start `play_id`
  int play_id = -1;
  int game_id = 0;        // the id the game's SetSong must get (the call's own id unless we took over)
};

class Preview {
 public:
  Faults faults;

  // `has_file(id)` -> true when a file of ours exists for the music id and the mods rules allow it.
  template <class HasFile>
  Step OnSet(int id, bool locked, HasFile&& has_file) {
    Step s;
    s.game_id = id;
    if (locked && !faults.play_when_locked) return s;  // the game ignores the call: so do we
    const bool ours = (id != kNone && has_file(id)) || (faults.play_stock && id != kNone);
    if (ours) {
      s.play = true;
      s.play_id = id;
      s.stop = playing_ >= 0;  // a switch: the old one ends (the player also replaces it, this keeps the log honest)
      if (faults.no_stop_on_switch) s.stop = false;
      if (!faults.game_keeps_id) s.game_id = kNone;
      playing_ = id;
      return s;
    }
    if (playing_ >= 0 && !faults.no_stop_on_switch) {
      s.stop = true;
      playing_ = -1;
    }
    return s;
  }

  // Stop / Shutdown / destruction of the preview object, or the frame check finding it gone.
  Step OnStop() {
    Step s;
    if (playing_ >= 0 && !faults.no_stop_on_close) {
      s.stop = true;
      playing_ = -1;
    }
    return s;
  }

  int playing() const { return playing_; }
  // The entrance path or another owner took the player: forget (no stop).
  void Forget() { playing_ = -1; }

 private:
  int playing_ = -1;
};

}  // namespace wwe2k14::music_preview
