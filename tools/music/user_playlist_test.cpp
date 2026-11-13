// user_playlist_test: host test of src\user_playlist_core.h (lane 282, 2026-11-13; 0 game runs, no audio, no game files).
//   build: clang++ -std=c++20 -I. tools\music\user_playlist_test.cpp -o out\user_playlist_test.exe   (from the repo root)
//   run:   out\user_playlist_test.exe          exit 0 = every case held, 1 = a case failed
// Cases (the planted bad ones first): empty folder, unsupported files, hidden files, a removed song, a song the player
// refuses, every song refused, a song removed while playing, a stale end-of-song event, pause / continue / stop from the
// wrong state, Play while playing, the end of the list, the playlist-over-new folder choice.
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "src/user_playlist_core.h"

using namespace wwe2k14::user_playlist;

static int g_fail = 0;
#define CHECK(c)                                                    \
  do {                                                              \
    if (!(c)) {                                                     \
      std::printf("FAIL line %d: %s\n", __LINE__, #c);              \
      ++g_fail;                                                     \
    }                                                               \
  } while (0)

struct FakePlayer : PlayerIf {
  std::set<std::string> refuse;  // file names Start() refuses
  std::vector<std::string> log;
  uint32_t last_token = 0;
  float last_volume = -1;
  bool paused = false;
  bool Start(const std::filesystem::path& f, uint32_t token, float volume) override {
    if (refuse.count(f.filename().string())) {
      log.push_back("refuse " + f.filename().string());
      return false;
    }
    log.push_back("start " + f.filename().string());
    last_token = token, last_volume = volume, paused = false;
    return true;
  }
  void Stop() override { log.push_back("stop"); }
  void Pause(bool p) override { paused = p, log.push_back(p ? "pause" : "continue"); }
  void SetVolume(float v) override { last_volume = v, log.push_back("volume"); }
};

static std::vector<std::filesystem::path> Paths(const std::vector<std::string>& n) {
  std::vector<std::filesystem::path> out;
  for (const auto& s : n) out.push_back(std::filesystem::path("mods/music/playlist") / s);
  return out;
}

int main() {
  // --- names
  CHECK(PickSongs({}).empty());                                                      // empty folder
  CHECK(PickSongs({"notes.txt", "cover.jpg", "a.mp3.part", ".hidden.mp3", "mp3"}).empty());  // nothing playable
  CHECK((PickSongs({"b.MP3", "A.flac", "c.ogg", "a.wav", "slots.txt"}) ==
         std::vector<std::string>{"A.flac", "a.wav", "b.MP3"}));                     // case-insensitive order, ogg refused
  CHECK(ChooseFolder({}, {}) == -1);
  CHECK(ChooseFolder({"x.txt"}, {"k.mp3"}) == 1);                                    // playlist folder without songs: new
  CHECK(ChooseFolder({"p.m4a"}, {"k.mp3"}) == 0);                                    // playlist folder wins
  CHECK(ChooseFolder({}, {"k.mp3"}) == 1);

  // --- empty list: Play fails, nothing started
  {
    FakePlayer pl;
    Machine m(pl, [](const std::filesystem::path&) { return true; });
    CHECK(m.Play() == 1);
    CHECK(m.state() == State::Idle && pl.log.empty());
    m.Stop();
    CHECK(m.Pause() == false && m.Continue() == false);                              // wrong state
    m.OnSongEnded(0);                                                                // nothing playing: ignored
    CHECK(m.state() == State::Idle);
  }

  // --- a removed song is skipped; the end of the list = Idle
  {
    FakePlayer pl;
    std::set<std::string> gone = {"a.mp3"};
    Machine m(pl, [&](const std::filesystem::path& p) { return !gone.count(p.filename().string()); });
    m.SetVolume(0.5f);
    m.SetSongs(Paths({"a.mp3", "b.mp3", "c.mp3"}));
    CHECK(m.Play() == 0);
    CHECK(m.state() == State::Playing && m.current().filename() == "b.mp3");
    CHECK(pl.last_volume == 0.5f);
    m.OnSongEnded(pl.last_token);
    CHECK(m.current().filename() == "c.mp3");
    m.OnSongEnded(pl.last_token);                                                    // end of the list
    CHECK(m.state() == State::Idle && m.current().empty());
    CHECK(m.Play() == 0 && m.current().filename() == "b.mp3");                       // plays again from the start
  }

  // --- the player refuses a song; all refused
  {
    FakePlayer pl;
    pl.refuse = {"a.mp3"};
    Machine m(pl, [](const std::filesystem::path&) { return true; });
    m.SetSongs(Paths({"a.mp3", "b.mp3"}));
    CHECK(m.Play() == 0 && m.current().filename() == "b.mp3");
    pl.refuse = {"a.mp3", "b.mp3"};
    m.Stop();
    CHECK(m.Play() == 1 && m.state() == State::Idle);
  }

  // --- a song that fails to start in the middle of the list: skipped on advance, the list goes on
  {
    FakePlayer pl;
    pl.refuse = {"b.mp3"};
    Machine m(pl, [](const std::filesystem::path&) { return true; });
    m.SetSongs(Paths({"a.mp3", "b.mp3", "c.mp3"}));
    CHECK(m.Play() == 0);
    m.OnSongEnded(pl.last_token);
    CHECK(m.current().filename() == "c.mp3");
  }

  // --- stale end events
  {
    FakePlayer pl;
    Machine m(pl, [](const std::filesystem::path&) { return true; });
    m.SetSongs(Paths({"a.mp3", "b.mp3"}));
    CHECK(m.Play() == 0);
    const uint32_t t1 = pl.last_token;
    CHECK(m.Play() == 0);                                                            // Play while playing: starts again
    CHECK(pl.last_token != t1 && m.current().filename() == "a.mp3");
    m.OnSongEnded(t1);                                                               // the first start's late event
    CHECK(m.current().filename() == "a.mp3");
    m.Stop();
    m.OnSongEnded(pl.last_token);                                                    // after Stop: ignored
    CHECK(m.state() == State::Idle);
  }

  // --- pause / continue / stop
  {
    FakePlayer pl;
    Machine m(pl, [](const std::filesystem::path&) { return true; });
    m.SetSongs(Paths({"a.mp3"}));
    CHECK(m.Pause() == false);
    CHECK(m.Play() == 0);
    CHECK(m.Continue() == false);                                                    // not paused
    CHECK(m.Pause() && m.state() == State::Paused && pl.paused);
    CHECK(m.Pause() == false);                                                       // already paused
    m.OnSongEnded(pl.last_token);                                                    // a paused single song ending: list over
    CHECK(m.state() == State::Idle);
    CHECK(m.Play() == 0 && m.Pause() && m.Continue() && m.state() == State::Playing && !pl.paused);
    m.Stop();
    CHECK(m.state() == State::Idle && pl.log.back() == "stop");
    m.Stop();                                                                        // twice: no second stop
    int stops = 0;
    for (const auto& l : pl.log) stops += l == "stop";
    CHECK(stops == 1);
  }

  // --- the playing song is removed from the list while it plays
  {
    FakePlayer pl;
    Machine m(pl, [](const std::filesystem::path&) { return true; });
    m.SetSongs(Paths({"a.mp3", "b.mp3"}));
    CHECK(m.Play() == 0);
    m.SetSongs(Paths({"b.mp3", "c.mp3"}));                                           // a.mp3 gone from the scan
    CHECK(m.current().filename() == "a.mp3");                                        // it keeps playing
    m.OnSongEnded(pl.last_token);
    CHECK(m.current().filename() == "b.mp3");                                        // then the new list from its start
    m.SetVolume(2.0f);
    CHECK(m.volume() == 1.0f);                                                       // clamped
    m.SetVolume(-1.0f);
    CHECK(m.volume() == 0.0f);
  }

  // --- single-song play and the per-wrestler choice
  {
    FakePlayer pl;
    Machine m(pl, [](const std::filesystem::path&) { return true; });
    const auto songs = Paths({"a.mp3", "b.mp3", "c.mp3"});
    m.SetSongs(songs);
    const auto choices = ParseChoices("# c\n343\tB.MP3\n\nbadline\n\t x\n343\tc.mp3\n100\tgone.mp3\n7\t\n");
    CHECK(choices.size() == 3);                                                      // malformed lines dropped
    CHECK(ChooseSong(choices, "343", songs) == 2);                                   // the last line for a key wins
    CHECK(ChooseSong(choices, "100", songs) == 0);                                   // chosen song gone: the first
    CHECK(ChooseSong(choices, "999", songs) == 0 && ChooseSong(choices, "", songs) == 0);  // no choice: the first
    CHECK(ChooseSong(choices, "343", {}) == size_t(-1));                             // empty library
    CHECK(ParseChoices(FormatChoices(choices)) == choices);                          // round trip
    CHECK(m.Play(1, true) == 0 && m.current().filename() == "b.mp3");
    m.OnSongEnded(pl.last_token);                                                    // single: ends, no next song
    CHECK(m.state() == State::Idle);
    CHECK(m.Play(5, true) == 1 && m.state() == State::Idle);                         // index past the end
    pl.refuse = {"a.mp3"};
    CHECK(m.Play(0, true) == 1);                                                     // single: no fall-through to b
  }

  // --- lane 283 (review B1): the list keeps SOURCE paths; a player that substitutes the converted copy only where it starts a song
  // (SwitchPlayer::Start -> music_slots::Playable) leaves current() / PlayPath() / the picker's names on the source file names, and a
  // refused (empty) mapping makes Start() return false so the machine moves on.
  {
    struct MappingPlayer : PlayerIf {
      FakePlayer inner;
      std::set<std::string> refused;
      bool Start(const std::filesystem::path& f, uint32_t t, float v) override {
        // the real SwitchPlayer::Start is exactly this call (src\user_playlist_core.h StartVia) with music_slots::Playable as the map
        return StartVia(inner, [&](const std::filesystem::path& g) {
          if (refused.count(g.filename().string())) return std::filesystem::path();  // Playable() gave an empty path
          return g.parent_path() / ("0123abcd_tm175_v2_" + g.stem().string() + ".wav");
        }, f, t, v);
      }
      void Stop() override { inner.Stop(); }
      void Pause(bool p) override { inner.Pause(p); }
      void SetVolume(float v) override { inner.SetVolume(v); }
    } pl;
    Machine m(pl, [](const std::filesystem::path&) { return true; });
    m.SetSongs(Paths({"Kingdom.mp3", "Bad.mp3", "Next.mp3"}));
    pl.refused = {"Bad.mp3"};
    CHECK(m.Play(0, false) == 0);
    CHECK(m.current().filename().string() == "Kingdom.mp3");                           // the machine still names the source file
    CHECK(pl.inner.log.back() == "start 0123abcd_tm175_v2_Kingdom.wav");                // the player got the converted copy
    CHECK(m.PlayPath(Paths({"Kingdom.mp3"})[0], true) == 0);                           // the picker's by-path play uses the source path
    m.OnSongEnded(pl.inner.last_token);
    CHECK(m.PlayPath(Paths({"Next.mp3"})[0], true) == 0 && m.current().filename().string() == "Next.mp3");
    CHECK(m.PlayPath(Paths({"Bad.mp3"})[0], true) != 0);                               // refused unconverted/unopenable: not played
  }

  std::printf("%s (%d failure(s))\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}
