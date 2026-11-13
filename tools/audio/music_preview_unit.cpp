// Lane 281: offline test of the Create-an-Entrance MUSIC preview's decision logic (src\music_preview_core.h).
// Answers: does the preview follow the game's own lifecycle (cursor on a song of ours -> ours plays and the game's preview
// is silenced; move to another -> the old stops, the new plays; a stock song -> ours stops, the game plays; menu close /
// Stop / Shutdown / destruction -> everything stops; a locked preview object is ignored) and is there ever more than one
// song sounding.
//   clang++ -std=c++20 tools\audio\music_preview_unit.cpp -o out\tools\music_preview_unit.exe
//   music_preview_unit.exe            -> exit 0, "music_preview_unit: PASS"
//   music_preview_unit.exe PLANT      -> exit 1 for each of: no_stop_on_switch no_stop_on_close game_keeps_id
//                                        play_when_locked play_stock
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

#include "../../src/music_preview_core.h"

using namespace wwe2k14::music_preview;

static int g_fail = 0;
#define CHECK(c, ...)                                  \
  do {                                                 \
    if (!(c)) {                                        \
      ++g_fail;                                        \
      std::printf("FAIL line %d: %s  ", __LINE__, #c); \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
    }                                                  \
  } while (0)

// A model of the two things that can sound: our player and the game's own preview (fed the id the hook hands it).
struct World {
  Preview p;
  int ours = -1;     // the song our player holds
  int game = -1;     // the song the game's preview holds (-1 silent; 9998 = the Xbox library)
  bool locked = false;
  std::set<int> files{6001, 6002, 343};  // ids we have a file for (two new songs, one replaced wrestler's theme)

  int sounding() const { return (ours >= 0) + (game >= 0); }

  Step Set(int id) {
    Step s = p.OnSet(id, locked, [&](int i) { return files.count(i) != 0; });
    if (s.stop) ours = -1;
    if (s.play) ours = s.play_id;
    // the game's SetSong: ignored when locked; 9999 silences its preview; anything else previews that id
    if (!locked) game = (s.game_id == kNone) ? -1 : s.game_id;
    return s;
  }
  void Close() {
    Step s = p.OnStop();
    if (s.stop) ours = -1;
    game = -1;  // the game's own Stop / Shutdown / destructor end its preview
  }
};

int main(int argc, char** argv) {
  Faults f;
  if (argc > 1) {
    const std::string a = argv[1];
    if (a == "no_stop_on_switch") f.no_stop_on_switch = true;
    else if (a == "no_stop_on_close") f.no_stop_on_close = true;
    else if (a == "game_keeps_id") f.game_keeps_id = true;
    else if (a == "play_when_locked") f.play_when_locked = true;
    else if (a == "play_stock") f.play_stock = true;
    else {
      std::printf("unknown plant %s\n", a.c_str());
      return 2;
    }
  }

  {  // cursor walk: stock -> A -> B -> stock -> NONE -> replaced wrestler -> USER PLAYLIST -> close
    World w;
    w.p.faults = f;
    Step s = w.Set(100);  // a stock song
    CHECK(!s.play && !s.stop && s.game_id == 100 && w.ours == -1 && w.game == 100, "stock: game plays, ours silent");
    s = w.Set(6001);  // a new song
    CHECK(s.play && s.play_id == 6001 && s.game_id == kNone, "A: ours plays, the game is told NONE");
    CHECK(w.ours == 6001 && w.game == -1, "A: only ours sounds (ours %d game %d)", w.ours, w.game);
    s = w.Set(6002);  // move to another new song
    CHECK(s.stop && s.play && s.play_id == 6002, "A -> B: the old one stops, the new one plays");
    CHECK(w.ours == 6002 && w.sounding() == 1, "A -> B: one song (ours %d game %d)", w.ours, w.game);
    s = w.Set(101);  // move to a stock song
    CHECK(s.stop && !s.play && s.game_id == 101, "B -> stock: ours stops, the game gets the call");
    CHECK(w.ours == -1 && w.game == 101 && w.sounding() == 1, "B -> stock: only the game's (ours %d game %d)", w.ours, w.game);
    s = w.Set(6001);
    s = w.Set(kNone);  // the NONE row
    CHECK(s.stop && w.ours == -1 && w.game == -1, "-> NONE: silence (ours %d game %d)", w.ours, w.game);
    s = w.Set(343);  // a replaced wrestler's theme
    CHECK(s.play && s.play_id == 343 && w.ours == 343 && w.game == -1, "replaced theme plays ours");
    s = w.Set(9998);  // USER PLAYLIST: the Xbox library, the game's
    CHECK(s.stop && s.game_id == 9998 && w.ours == -1 && w.game == 9998, "-> USER PLAYLIST: ours stops, the game's path");
    s = w.Set(6002);
    w.Close();  // the menu closes
    CHECK(w.ours == -1 && w.game == -1, "close: nothing sounds (ours %d game %d)", w.ours, w.game);
    CHECK(w.p.playing() == -1, "close: state cleared");
    w.Close();  // a second close is harmless
    CHECK(w.ours == -1, "double close");
  }

  {  // the same song set twice restarts it (the game restarts its own preview on every SetSong)
    World w;
    w.p.faults = f;
    w.Set(6001);
    Step s = w.Set(6001);
    CHECK(s.play && s.play_id == 6001 && w.ours == 6001 && w.sounding() == 1, "repeat: restarts, still one song");
  }

  {  // a locked preview object: the game ignores SetSong, so do we (no start, no stop)
    World w;
    w.p.faults = f;
    w.Set(6001);
    w.locked = true;
    Step s = w.Set(6002);
    CHECK(!s.play && !s.stop && s.game_id == 6002, "locked: the call passes through untouched");
    CHECK(w.ours == 6001, "locked: ours unchanged (%d)", w.ours);
    World v;
    v.p.faults = f;
    v.locked = true;
    s = v.Set(6001);
    CHECK(!s.play && v.ours == -1, "locked from the start: ours does not play");
  }

  {  // mods off / no file: every id is the game's own
    World w;
    w.p.faults = f;
    w.files.clear();
    Step s = w.Set(6001);
    CHECK(!s.play && s.game_id == 6001 && w.ours == -1 && w.game == 6001, "no file: the game's path");
  }

  {  // a stock song never gets taken over
    World w;
    w.p.faults = f;
    Step s = w.Set(250);
    CHECK(!s.play && w.ours == -1 && w.game == 250, "stock song: not ours");
  }

  if (g_fail) {
    std::printf("music_preview_unit: FAIL (%d)\n", g_fail);
    return 1;
  }
  std::printf("music_preview_unit: PASS\n");
  return 0;
}
