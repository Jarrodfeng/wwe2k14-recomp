// Host test of the USER PLAYLIST music picker's state machine (lane 291). Build and run:
//   clang++ -std=c++20 -I. tools\music\music_picker_test.cpp -o %TEMP%\music_picker_test.exe && %TEMP%\music_picker_test.exe
// (from the repo root). Checks src\music_picker_core.h with a fake preview and a fake file system, the choices.txt helpers
// and the stop rule of src\user_playlist_core.h. Prints "FAIL ..." lines and exits 1 on any failure.
#include <cstdio>
#include <set>

#include "src/music_picker_core.h"

using namespace wwe2k14::music_picker;
namespace up = wwe2k14::user_playlist;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    if (!(c)) {                                                   \
      std::printf("FAIL line %d: %s\n", __LINE__, #c);            \
      ++g_fail;                                                   \
    }                                                             \
  } while (0)

struct FakePreview : PreviewIf {
  int plays = 0, stops = 0;
  int index = -1;
  bool playing = false, refuse = false;
  const std::vector<fs::path>* all = nullptr;  // the env's songs: index = the position of the file asked for
  fs::path last;
  bool Play(const fs::path& f) override {
    ++plays;
    if (refuse) return false;
    last = f;
    index = -1;
    for (size_t i = 0; all && i < all->size(); ++i)
      if ((*all)[i] == f) index = int(i);
    playing = true;
    return true;
  }
  void Stop() override { ++stops, playing = false; }
  bool IsPlaying() override { return playing; }
};

struct Env {
  FakePreview prev;
  std::set<std::string> present;
  Picker picker{prev, [this](const fs::path& p) { return present.count(p.filename().string()) > 0; }};
  std::vector<fs::path> songs;
  Env(std::vector<std::string> names) {
    for (auto& n : names) songs.push_back(fs::path("lib") / n), present.insert(n);
    prev.all = &songs;
  }
  void Open(const std::string& current = "") { picker.Open(songs, current, "MUSIC LIBRARY", "Choose", "Library is empty."); }
};
static PickerInput In(InputKind k, int row = -1, int scroll = 0) { return PickerInput{k, row, scroll}; }

int main() {
  // open: names without extension, the saved choice is selected and under the cursor
  {
    Env e({"a.mp3", "b.mp3", "c.flac"});
    e.Open("B.MP3");
    const auto& m = e.picker.model();
    CHECK(e.picker.is_open() && m.songs.size() == 3 && m.songs[1].name == "b");
    CHECK(m.selected == 1 && m.cursor == 1 && m.playing == -1 && !m.empty);
  }
  // moving the cursor never plays anything
  {
    Env e({"a.mp3", "b.mp3", "c.mp3"});
    e.Open();
    e.picker.Handle(In(InputKind::kDown));
    e.picker.Handle(In(InputKind::kDown));
    e.picker.Handle(In(InputKind::kDown));  // clamped at the end
    e.picker.Handle(In(InputKind::kPageUp));
    e.picker.Handle(In(InputKind::kScroll, -1, 2));
    CHECK(e.prev.plays == 0 && e.picker.model().playing == -1);
    CHECK(e.picker.model().cursor == 2);
    e.picker.Handle(In(InputKind::kUp));
    CHECK(e.picker.model().cursor == 1 && e.prev.plays == 0);
  }
  // play / stop / switch; the cursor moving on leaves the preview playing
  {
    Env e({"a.mp3", "b.mp3", "c.mp3"});
    e.Open();
    e.picker.Handle(In(InputKind::kTogglePlay));  // cursor row 0
    CHECK(e.prev.index == 0 && e.picker.model().playing == 0);
    e.picker.Handle(In(InputKind::kDown));
    e.picker.Handle(In(InputKind::kDown));
    CHECK(e.picker.model().playing == 0 && e.prev.playing && e.prev.stops == 0);
    e.picker.Handle(In(InputKind::kTogglePlay, 2));  // switch: another row's button
    CHECK(e.prev.index == 2 && e.picker.model().playing == 2 && e.prev.plays == 2);
    e.picker.Handle(In(InputKind::kTogglePlay, 2));  // the same row's button again: stop
    CHECK(e.picker.model().playing == -1 && !e.prev.playing && e.prev.stops == 1);
    const auto rev = e.picker.model().revision;
    e.picker.Handle(In(InputKind::kTogglePlay, 9));  // no such row
    CHECK(e.picker.model().revision == rev && e.prev.plays == 2);
  }
  // select while playing: the preview stops, the choice is returned, the picker closes, input is given back
  {
    Env e({"a.mp3", "b.mp3"});
    e.Open();
    e.picker.Handle(In(InputKind::kTogglePlay, 0));
    e.picker.Handle(In(InputKind::kDown));
    const Result r = e.picker.Handle(In(InputKind::kSelect));
    CHECK(r.outcome == Outcome::Chosen && r.file.filename() == "b.mp3");
    CHECK(!e.picker.is_open() && !e.prev.playing && e.picker.model().playing == -1);
    CHECK(e.picker.Handle(In(InputKind::kSelect)).outcome == Outcome::None);  // closed: ignores input
  }
  // a tap on a row chooses it at once (no play)
  {
    Env e({"a.mp3", "b.mp3", "c.mp3"});
    e.Open();
    const Result r = e.picker.Handle(In(InputKind::kTapRow, 2));
    CHECK(r.outcome == Outcome::Chosen && r.file.filename() == "c.mp3" && e.prev.plays == 0);
  }
  // cancel: preview stops, no choice
  {
    Env e({"a.mp3", "b.mp3"});
    e.Open("a.mp3");
    e.picker.Handle(In(InputKind::kTogglePlay, 1));
    const Result r = e.picker.Handle(In(InputKind::kCancel));
    CHECK(r.outcome == Outcome::Cancelled && r.file.empty() && !e.picker.is_open() && !e.prev.playing);
  }
  // cancel without a preview does not touch the player
  {
    Env e({"a.mp3"});
    e.Open();
    e.picker.Handle(In(InputKind::kCancel));
    CHECK(e.prev.stops == 0);
  }
  // empty library: the message, no list actions, cancel closes
  {
    Env e({});
    e.Open();
    const auto& m = e.picker.model();
    CHECK(m.empty && m.message == "Library is empty." && m.songs.empty());
    CHECK(e.picker.Handle(In(InputKind::kSelect)).outcome == Outcome::None && e.picker.is_open());
    CHECK(e.picker.Handle(In(InputKind::kTogglePlay)).outcome == Outcome::None && e.prev.plays == 0);
    CHECK(e.picker.Handle(In(InputKind::kDown)).outcome == Outcome::None && e.picker.model().cursor == 0);
    CHECK(e.picker.Handle(In(InputKind::kCancel)).outcome == Outcome::Cancelled);
  }
  // a removed file: select drops it, says so, stays open; indices of selected / playing shift
  {
    Env e({"a.mp3", "b.mp3", "c.mp3"});
    e.Open("c.mp3");
    e.picker.Handle(In(InputKind::kTogglePlay, 2));  // c plays
    e.present.erase("a.mp3");
    e.picker.Handle(In(InputKind::kTapRow, 0));      // a is gone
    const auto& m = e.picker.model();
    CHECK(e.picker.is_open() && m.songs.size() == 2 && m.songs[0].name == "b");
    CHECK(m.message.find("a") != std::string::npos && m.message.find("no longer") != std::string::npos);
    CHECK(m.selected == 1 && m.playing == 1 && e.prev.playing);
    // the playing song itself vanishes and is asked to play again: the preview stops
    e.present.erase("c.mp3");
    e.picker.Handle(In(InputKind::kTogglePlay, 0));  // b is fine: switches to b
    CHECK(e.picker.model().playing == 0);
    e.picker.Handle(In(InputKind::kTogglePlay, 1));  // c is gone
    CHECK(e.picker.model().songs.size() == 1 && e.picker.model().selected == -1);
    // everything gone: the empty message
    e.present.erase("b.mp3");
    e.picker.Handle(In(InputKind::kSelect));
    CHECK(e.picker.model().empty && e.picker.model().message == "Library is empty." && e.picker.is_open());
    CHECK(e.picker.Handle(In(InputKind::kCancel)).outcome == Outcome::Cancelled);
  }
  // the player refuses a song: a message, nothing playing
  {
    Env e({"a.mp3", "b.mp3"});
    e.Open();
    e.picker.Handle(In(InputKind::kTogglePlay, 0));
    e.prev.refuse = true;
    e.picker.Handle(In(InputKind::kTogglePlay, 1));
    CHECK(e.picker.model().playing == -1 && !e.prev.playing && !e.picker.model().message.empty());
    e.prev.refuse = false;
    e.picker.Handle(In(InputKind::kTogglePlay, 1));
    CHECK(e.picker.model().playing == 1 && e.picker.model().message.empty());
  }
  // a song that ends by itself clears the playing row
  {
    Env e({"a.mp3"});
    e.Open();
    e.picker.Handle(In(InputKind::kTogglePlay, 0));
    e.prev.playing = false;
    e.picker.Tick();
    CHECK(e.picker.model().playing == -1);
  }
  // ForceClose: stops the preview
  {
    Env e({"a.mp3"});
    e.Open();
    e.picker.Handle(In(InputKind::kTogglePlay, 0));
    e.picker.ForceClose();
    CHECK(!e.picker.is_open() && !e.prev.playing);
  }
  // choices.txt: upsert replaces the key's line; "*" is the fallback; a real key wins; a gone file falls back
  {
    std::string t;
    t = up::UpsertChoice(t, "*", "a.mp3");
    t = up::UpsertChoice(t, "wrestler7", "c.mp3");
    t = up::UpsertChoice(t, "*", "b.mp3");
    const auto c = up::ParseChoices(t);
    CHECK(c.size() == 2);
    const std::vector<fs::path> songs = {"lib/a.mp3", "lib/b.mp3", "lib/c.mp3"};
    CHECK(up::ChooseSong(c, "wrestler7", songs) == 2);
    CHECK(up::ChooseSong(c, "other", songs) == 1);
    CHECK(up::ChooseSong(c, "", songs) == 1);
    CHECK(up::ChooseSong({}, "x", songs) == 0);
    const std::vector<fs::path> no_c = {"lib/a.mp3", "lib/b.mp3"};
    CHECK(up::ChooseSong(c, "wrestler7", no_c) == 1);  // his song is gone: the "*" choice
  }
  // lane 309: per-wrestler keys. Two wrestlers pick different songs; each keeps its own; an unknown id and an unreadable key fall back to "*"
  {
    const std::vector<fs::path> songs = {"lib/a.mp3", "lib/b.mp3", "lib/c.mp3"};
    CHECK(up::KeyForCharacter(298) == "298" && up::KeyForCharacter(0) == "0" && up::KeyForCharacter(4095) == "4095");
    CHECK(up::KeyForCharacter(-1).empty() && up::KeyForCharacter(4096).empty() && up::KeyForCharacter(int64_t(int32_t(0xFFFFFFFFu))).empty());
    std::string t;
    t = up::UpsertPick(t, up::KeyForCharacter(298), "b.mp3");  // AJ Lee picks b
    t = up::UpsertPick(t, up::KeyForCharacter(343), "c.mp3");  // Brock picks c ("*" is now c: the latest pick)
    auto c = up::ParseChoices(t);
    CHECK(c.size() == 3);                                       // 298, 343, "*": one line per key, not per pick
    CHECK(up::ChooseSong(c, "298", songs) == 1);                // her own line, not the latest pick
    CHECK(up::ChooseSong(c, "343", songs) == 2);
    CHECK(up::ChooseSong(c, "400", songs) == 2);                // an id with no line: the "*" fallback (the latest pick)
    CHECK(up::ChooseSong(c, up::KeyForCharacter(99999), songs) == 2);  // an unknown id: key "" -> "*"
    CHECK(up::ChooseSong(c, "", songs) == 2);
    // what the picker shows as "current": the wrestler's own line only, never another wrestler's pick through "*"
    CHECK(up::CurrentChoice(c, "298", songs) == 1);
    CHECK(up::CurrentChoice(c, "400", songs) == size_t(-1));    // no line of his own: nothing is current
    CHECK(up::CurrentChoice(c, "", songs) == 2);                // no key: the "*" choice as before
    // a re-pick replaces his line (not a second one) and keeps the others
    t = up::UpsertPick(t, "298", "a.mp3");
    c = up::ParseChoices(t);
    CHECK(c.size() == 3 && up::ChooseSong(c, "298", songs) == 0 && up::ChooseSong(c, "343", songs) == 2);
    // no key (unreadable): only the "*" line is written, a real key's line is untouched
    t = up::UpsertPick(t, "", "b.mp3");
    c = up::ParseChoices(t);
    CHECK(c.size() == 3 && up::ChooseSong(c, "298", songs) == 0 && up::ChooseSong(c, "400", songs) == 1);
    t = up::UpsertPick(t, "*", "c.mp3");
    CHECK(up::ParseChoices(t).size() == 3);
    // his song was removed from the library: he falls back to "*", not to the first song
    const std::vector<fs::path> no_a = {"lib/b.mp3", "lib/c.mp3"};
    CHECK(up::ChooseSong(up::ParseChoices(t), "298", no_a) == 1);  // "*" = c.mp3
    // a legacy file with only the "*" line (written before lane 309) still works for everybody
    const auto legacy = up::ParseChoices("*\tb.mp3\n");
    CHECK(up::ChooseSong(legacy, "298", songs) == 1 && up::CurrentChoice(legacy, "", songs) == 1 && up::CurrentChoice(legacy, "298", songs) == size_t(-1));
  }
  // the frame stop rule: the editor (sequence 25) is not killed by the entrance rule; an entrance still is
  {
    using up::ShouldStop;
    using up::StopWhy;
    CHECK(ShouldStop(25, 25, 0) == StopWhy::None);
    CHECK(ShouldStop(25, 9, 0) == StopWhy::SequenceLeft);
    CHECK(ShouldStop(25, -1, 0) == StopWhy::SequenceLeft);
    CHECK(ShouldStop(9, 9, 0) == StopWhy::None);
    CHECK(ShouldStop(9, 9, 5) == StopWhy::MatchStarted);
    CHECK(ShouldStop(9, 25, 0) == StopWhy::SequenceLeft);
    CHECK(ShouldStop(-1, 9, 0) == StopWhy::None);  // unreadable at Play: the entrance's 9
  }
  // ID3v2.4 tag: UTF-8 title, UTF-16 artist, a cover
  {
    std::vector<uint8_t> tag = {'I', 'D', '3', 4, 0, 0, 0, 0, 0, 0};
    auto frame = [&](const char* id, std::vector<uint8_t> body) {
      for (int i = 0; i < 4; ++i) tag.push_back(uint8_t(id[i]));
      const uint32_t n = uint32_t(body.size());
      tag.push_back(uint8_t((n >> 21) & 0x7F)), tag.push_back(uint8_t((n >> 14) & 0x7F));
      tag.push_back(uint8_t((n >> 7) & 0x7F)), tag.push_back(uint8_t(n & 0x7F));
      tag.push_back(0), tag.push_back(0);
      tag.insert(tag.end(), body.begin(), body.end());
    };
    frame("TIT2", {3, 'H', 'e', 'l', 'l', 'o'});
    frame("TPE1", {1, 0xFF, 0xFE, 'A', 0, 'B', 0, 0xE9, 0});  // "ABé", little endian with BOM
    std::vector<uint8_t> apic = {0, 'i', 'm', 'a', 'g', 'e', '/', 'p', 'n', 'g', 0, 3, 'c', 0, 0x89, 'P', 'N', 'G'};
    frame("APIC", apic);
    const uint32_t body = uint32_t(tag.size() - 10);
    tag[6] = uint8_t((body >> 21) & 0x7F), tag[7] = uint8_t((body >> 14) & 0x7F);
    tag[8] = uint8_t((body >> 7) & 0x7F), tag[9] = uint8_t(body & 0x7F);
    for (int i = 0; i < 100; ++i) tag.push_back(0);  // audio / padding
    const Tags t = ParseId3(tag.data(), tag.size());
    CHECK(t.title == "Hello");
    CHECK(t.artist == "AB\xC3\xA9");
    CHECK(t.art.size() == 4 && t.art[0] == 0x89 && t.art[3] == 'G');
    CHECK(ParseId3(tag.data(), 12).title.empty());  // truncated tag: nothing, no crash
    const uint8_t junk[16] = {1, 2, 3};
    CHECK(ParseId3(junk, 16).title.empty());
  }
  // Names a Windows ANSI code page cannot hold (a CJK name, an emoji): shown, compared, saved and played back as UTF-8, never through
  // path::string() (which throws there; review B2). The CJK char is U+5531, the emoji U+1F3B5.
  {
    const std::u8string cjk = u8"\u5531\u6b4c.mp3", emo = u8"\U0001F3B5 beat.flac";
    const fs::path pc(cjk), pe(emo), plain("plain.mp3");
    const std::string nc(cjk.begin(), cjk.end()), ne(emo.begin(), emo.end());
    CHECK(up::U8(pc) == nc && up::U8(pe) == ne);
    CHECK(up::FromU8(nc) == pc && up::FromU8(ne) == pe);
    CHECK(up::IsSongName(nc) && up::IsSongName(ne));
    const auto picked = up::PickSongs({nc, ne, "plain.mp3", ".hidden.mp3"});
    CHECK(picked.size() == 3);
    const std::vector<fs::path> songs = {fs::path("lib") / pe, fs::path("lib") / pc, fs::path("lib") / plain};
    // choices.txt round trip with a non-ANSI name
    const std::string text = up::UpsertChoice("", "*", nc);
    const auto choices = up::ParseChoices(text);
    CHECK(choices.size() == 1 && choices[0].second == nc);
    CHECK(up::FindChoice(choices, "*", songs) == 1);
    CHECK(up::ExplicitChoice(choices, "", songs) == 1);
    CHECK(up::ExplicitChoice({}, "", songs) == size_t(-1));  // no line: the default first song is not "current"
    // the picker: the row names are UTF-8, the saved choice is found, a preview goes by PATH
    Env e({});
    e.songs = songs;
    FakePreview& pv = e.prev;
    pv.all = &e.songs;
    Picker p(pv, [](const fs::path&) { return true; });
    p.Open(songs, nc, "T", "S", "E");
    CHECK(p.model().songs[0].name == "\xF0\x9F\x8E\xB5 beat" && p.model().songs[1].name == nc.substr(0, nc.size() - 4));
    CHECK(p.model().selected == 1 && p.model().cursor == 1);
    p.Handle(In(InputKind::kTogglePlay, 0));
    CHECK(pv.index == 0 && pv.last == songs[0]);
    // tags arrive by path after a song vanished from the list: the right row gets them
    p.SetTagsByPath(songs[2], "Title", "Artist", 7);
    CHECK(p.model().songs[2].name == "Title" && p.model().songs[2].art_id == 7);
    CHECK(p.Handle(In(InputKind::kSelect)).file == songs[1]);
  }
  // Machine::PlayPath
  {
    struct P : up::PlayerIf {
      std::filesystem::path last;
      bool Start(const std::filesystem::path& f, uint32_t, float) override { last = f; return true; }
      void Stop() override {}
      void Pause(bool) override {}
      void SetVolume(float) override {}
    } pl;
    up::Machine m(pl, [](const fs::path&) { return true; });
    m.SetSongs({fs::path("lib/a.mp3"), fs::path("lib/b.mp3")});
    CHECK(m.PlayPath(fs::path("lib/b.mp3"), true) == 0 && pl.last == fs::path("lib/b.mp3"));
    m.SetSongs({fs::path("lib/b.mp3")});  // the list shifted
    CHECK(m.PlayPath(fs::path("lib/b.mp3"), true) == 0 && pl.last == fs::path("lib/b.mp3"));
    CHECK(m.PlayPath(fs::path("lib/gone.mp3"), true) == 1);
  }
  // SF1 (lane 292 re-review): the delayed-first-frame sequence. A UI thread that does not run its queue for > 2 s: the game thread lets go
  // (Release), the posted open and the dialog show up late, a new open begins. Nothing of the old open may start, and a frame of a released
  // picker must say "stale" (the dialog closes and drops the view instead of drawing nothing for ever).
  {
    OpenLifecycle life;
    const uint32_t g1 = life.Begin();
    CHECK(life.open.load() && life.PostedMayRun(g1) && !life.Stale());
    life.Release();  // the 2-s release in OnFrame
    CHECK(life.Stale() && !life.PostedMayRun(g1));  // the late posted open of g1 must not create a dialog / replace a view
    const uint32_t g2 = life.Begin();               // the player re-opens
    CHECK(!life.Stale() && life.PostedMayRun(g2) && !life.PostedMayRun(g1));
    CHECK(g2 != g1);
    life.Release();  // Shutdown / a close
    CHECK(life.Stale() && !life.PostedMayRun(g2));
    // pump_frames restarts with each Begin
    life.pump_frames.store(5);
    life.Begin();
    CHECK(life.pump_frames.load() == 0);
  }
  std::printf(g_fail ? "music_picker_test: %d FAILED\n" : "music_picker_test: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
