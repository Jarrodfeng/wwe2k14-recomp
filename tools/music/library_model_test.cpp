// Host test of the phone launcher's song library model (lane 297): src\launcher_mobile\music_library.h with the real song importer
// (src\audio_import). No game, no SDL, no ImGui, nothing on the screen.
//   pwsh -File tools\music\library_model_test.ps1            (makes the synthetic samples, builds with and without the planted bug)
//   library_model_test.exe SAMPLES_DIR WORK_DIR              (SAMPLES_DIR from: py tools\audio\import_samples.py DIR)
// Covers: the library scan (empty folder, order, "converting..." before the read, ready after), import (tags + art present / absent,
// loudness near the target), refused files (the importer's sentence, nothing copied, the staged file gone), a duplicate and a name clash,
// choices.txt (round trip, the in-game picker's own key lines kept, the first song as the default, removing the chosen song) and the
// preview rule (only the play button starts a preview; focus / hover never does).
// Built a second time with -DWWE_TEST_MUTATE_FOCUS_PLAYS (the known bug: focus starts playback) the preview checks must FAIL.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "../../src/launcher_mobile/music_library.h"

namespace fs = std::filesystem;
using namespace mlauncher::musiclib;

static int g_pass = 0, g_fail = 0;
#define CHECK(c)                                                                  \
  do {                                                                            \
    if (c) ++g_pass;                                                              \
    else ++g_fail, std::printf("  FAIL line %d: %s\n", __LINE__, #c);              \
  } while (0)
#define SECTION(n) std::printf("%s\n", n)

static void Copy(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::create_directories(to.parent_path(), ec);
  fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
}
static bool Has(const std::vector<Message>& m, bool ok, const char* sub) {
  for (const Message& x : m)
    if (x.ok == ok && x.text.find(sub) != std::string::npos) return true;
  return false;
}
static const Entry* Find(const std::vector<Entry>& v, const std::string& file) {
  for (const Entry& e : v)
    if (e.file == file) return &e;
  return nullptr;
}

int main(int argc, char** argv) {
  if (argc < 3) return std::printf("usage: library_model_test SAMPLES_DIR WORK_DIR\n"), 2;
  const fs::path samples = argv[1], work = argv[2];
  std::error_code ec;
  fs::remove_all(work, ec);
  fs::create_directories(work, ec);
  const fs::path mods = work / "mods";

  // ---------------------------------------------------------------- preview rule (pure, no files)
  SECTION("preview rule");
  {
    PreviewState p;
    CHECK(p.OnRowFocus("a.mp3").what == PreviewState::Do::kNothing);  // THE rule: focus / hover never plays
    CHECK(p.playing().empty());
    CHECK(p.OnRowFocus("a.mp3").what == PreviewState::Do::kNothing);
    auto a = p.OnPlayButton("a.mp3");
    CHECK(a.what == PreviewState::Do::kStart && a.file == "a.mp3" && p.playing() == "a.mp3");
    CHECK(p.OnRowFocus("b.mp3").what == PreviewState::Do::kNothing && p.playing() == "a.mp3");  // focus on another row leaves it alone
    auto b = p.OnPlayButton("b.mp3");  // another song's button switches
    CHECK(b.what == PreviewState::Do::kStart && b.file == "b.mp3" && p.playing() == "b.mp3");
    CHECK(p.OnPlayButton("b.mp3").what == PreviewState::Do::kStop && p.playing().empty());  // the same button stops
    p.OnPlayButton("a.mp3");
    CHECK(p.OnRemoved("zzz.mp3").what == PreviewState::Do::kNothing && p.playing() == "a.mp3");
    CHECK(p.OnRemoved("a.mp3").what == PreviewState::Do::kStop && p.playing().empty());
    p.OnPlayButton("a.mp3");
    CHECK(p.OnLeave().what == PreviewState::Do::kStop && p.playing().empty());  // leaving the page stops it
    CHECK(p.OnLeave().what == PreviewState::Do::kNothing);
    p.OnPlayButton("a.mp3");
    p.OnEnded();  // the song finished by itself
    CHECK(p.playing().empty() && p.OnPlayButton("a.mp3").what == PreviewState::Do::kStart);
  }
  CHECK(FormatTime(0) == "0:00" && FormatTime(59.6) == "1:00" && FormatTime(225.2) == "3:45");

  // ---------------------------------------------------------------- empty library, import, tags + art
  SECTION("empty library, import, tags and art");
  Library lib(mods);
  lib.Refresh();
  CHECK(lib.Entries().empty());
  CHECK(Library::ChosenFile(mods).empty());
  CHECK(lib.Pending() == 0);

  const fs::path inc = StagingDir(mods, "library");
  Copy(samples / "song_art.mp3", inc / "song_art.mp3");
  Copy(samples / "song.wav", inc / "song.wav");
  lib.ImportIncoming();
  {  // the progress bar's numbers: one batch of 2 ("song 1 of 2"), overall 0..1, idle afterwards
    const auto pr = lib.Snapshot();
    CHECK(pr.active && pr.total == 2 && pr.done <= 1 && lib.Busy());
    CHECK(pr.overall() >= 0.f && pr.overall() <= 1.f);
  }
  lib.WaitIdle();
  {
    const auto pr = lib.Snapshot();
    CHECK(!pr.active && pr.done == pr.total && pr.total == 2 && pr.overall() == 1.f && !lib.Busy());
    const auto msgs = lib.TakeMessages();
    CHECK(msgs.size() == 2);
    CHECK(Has(msgs, true, "Added: Sample Song"));  // the tag title of song_art.mp3
    CHECK(Has(msgs, true, "Added: song"));      // no tags: the file name without its extension
    lib.Refresh();
    const auto v = lib.Entries();
    CHECK(v.size() == 2);
    const Entry* art = Find(v, "song_art.mp3");
    const Entry* plain = Find(v, "song.wav");
    CHECK(art && plain);
    if (art && plain) {
      CHECK(art->state == State::kReady && plain->state == State::kReady);
      CHECK(art->has_art() && fs::is_regular_file(art->meta.thumb));  // art present: a 256x256 PNG in the cache
      CHECK(!plain->has_art() && plain->meta.thumb.empty());          // art absent: the view draws the generic record
      CHECK(art->meta.title == "Sample Song" && art->meta.artist == "Sample Artist");
      CHECK(art->title() == "Sample Song" && plain->title() == "song");
      CHECK(std::fabs(art->meta.seconds - 12.0) < 0.5);
      CHECK(std::fabs(art->meta.out_lufs - audio_import::kTargetLufs) < 0.6);  // normalised to the game's themes
      CHECK(art->meta.gain_db > 5.0);                                          // the quiet synthetic song was lifted
      CHECK(LoudnessText(art->meta).find("LUFS") != std::string::npos && GainText(art->meta).find("gain +") == 0);
      CHECK(ByLine(art->meta) == "Sample Artist");
      CHECK(fs::is_regular_file(art->meta.audio));  // the player plays only this
    }
    // lane 283: the game finds the converted songs through <cache>/index.txt, keyed by the LIBRARY copy's path
    CHECK(fs::is_regular_file(CacheDir(mods) / "index.txt"));
    CHECK(audio_import::Lookup(LibraryDir(mods) / "song_art.mp3", CacheDir(mods)).ok && audio_import::Lookup(LibraryDir(mods) / "song.wav", CacheDir(mods)).ok);
    // the staged files are gone, the originals (the samples) untouched
    CHECK(!fs::exists(inc / "song.wav") && !fs::exists(inc / "song_art.mp3"));
    CHECK(fs::exists(samples / "song.wav"));
  }

  // ---------------------------------------------------------------- refused files add nothing
  SECTION("refused files");
  {
    const char* bad[] = {"garbage.mp3", "empty.ogg", "notes.m4a"};
    for (const char* b : bad) Copy(samples / b, inc / b);
    lib.ImportIncoming();
    lib.WaitIdle();
    const auto msgs = lib.TakeMessages();
    CHECK(msgs.size() == 3);
    int refused = 0;
    for (const Message& m : msgs) refused += !m.ok && m.text.rfind("Not added: ", 0) == 0;
    CHECK(refused == 3);
    CHECK(Has(msgs, false, "garbage.mp3") && Has(msgs, false, "could not be decoded"));  // the importer's own sentence
    lib.Refresh();
    CHECK(lib.Entries().size() == 2);  // nothing added
    for (const char* b : bad) CHECK(!fs::exists(LibraryDir(mods) / b) && !fs::exists(inc / b));
    CHECK(!fs::exists(LibraryDir(mods) / "garbage.mp3.part"));
  }

  // ---------------------------------------------------------------- CANCEL
  SECTION("cancel");
  {
    lib.Refresh();
    const size_t before = lib.Entries().size();
    const char* more[] = {"song.flac", "song_art.flac", "song.m4a", "song.opus"};
    for (const char* f : more) Copy(samples / f, inc / f);
    lib.ImportIncoming();
    lib.Report("decoding", 0.5f);  // (the importer's callback: the bar's stage + fraction)
    CHECK(lib.Snapshot().stage.size() > 0);
    lib.Cancel();
    CHECK(!lib.Report("decoding", 0.6f));  // the importer is told to stop
    lib.WaitIdle();
    lib.Refresh();
    CHECK(lib.Entries().size() <= before + 1);  // at most the song that was already being converted
    int left = 0;
    for (const auto& e : fs::directory_iterator(inc, ec)) left += e.is_regular_file(ec);
    CHECK(left == 0);  // the dropped songs' staged files are gone too
    bool said = false;
    for (const Message& m : lib.TakeMessages()) said |= m.text.rfind("Cancelled", 0) == 0;
    CHECK(said);
    CHECK(!lib.Snapshot().active);
  }

  // ---------------------------------------------------------------- duplicates and name clashes
  SECTION("duplicate and name clash");
  {
    Copy(samples / "song_art.mp3", inc / "other_name.mp3");  // the same bytes under another name
    lib.ImportIncoming();
    lib.WaitIdle();
    auto msgs = lib.TakeMessages();
    CHECK(msgs.size() == 1 && !msgs[0].ok && msgs[0].text.find("already in your library") != std::string::npos);
    lib.Refresh();
    CHECK(lib.Entries().size() == 2);
    // two different songs called dup.mp3 keep both
    Copy(samples / "song.mp3", inc / "dup.mp3");
    lib.ImportIncoming();
    lib.WaitIdle();
    Copy(samples / "song.ogg", inc / "dup.mp3");  // (ogg bytes under an .mp3 name: the importer reads the content or refuses it)
    lib.ImportIncoming();
    lib.WaitIdle();
    msgs = lib.TakeMessages();
    lib.Refresh();
    const auto v = lib.Entries();
    CHECK(Find(v, "dup.mp3") != nullptr);
    // the second file is either a second song "dup (2).mp3" or was refused with a sentence; never silently dropped / overwritten
    CHECK(Find(v, "dup (2).mp3") != nullptr || Has(msgs, false, "dup.mp3"));
  }

  // ---------------------------------------------------------------- a new Library over the folder: converting..., then ready
  SECTION("scan of an existing folder");
  {
    // (a copy of the folder; the cache is gone, so songs are read again on the worker thread)
    const fs::path mods2 = work / "mods2";
    fs::create_directories(LibraryDir(mods2), ec);
    for (const auto& e : fs::directory_iterator(LibraryDir(mods), ec))
      if (e.is_regular_file(ec) && e.path().extension() != ".txt") Copy(e.path(), LibraryDir(mods2) / e.path().filename());
    Copy(samples / "garbage.mp3", LibraryDir(mods2) / "bad_one.mp3");  // an unreadable song that slipped in by hand
    std::ofstream(LibraryDir(mods2) / ".hidden.mp3") << "x";
    std::ofstream(LibraryDir(mods2) / "readme.txt") << "x";
    Library l2(mods2);
    l2.Refresh();
    const auto first = l2.Entries();
    CHECK(!first.empty());
    bool all_converting = true;
    for (const Entry& e : first) all_converting &= e.state == State::kConverting;
    CHECK(all_converting);                  // "converting..." until the worker has answered
    CHECK(l2.Pending() > 0);
    CHECK(Find(first, ".hidden.mp3") == nullptr && Find(first, "readme.txt") == nullptr);
    l2.WaitIdle();
    l2.Refresh();
    const auto done = l2.Entries();
    CHECK(done.size() == first.size());
    const Entry* bad = Find(done, "bad_one.mp3");
    CHECK(bad && bad->state == State::kRefused && !bad->meta.err.empty());  // listed, marked, cannot be played
    const Entry* good = Find(done, "song_art.mp3");
    CHECK(good && good->state == State::kReady && good->has_art());
    CHECK(l2.Pending() == 0);
    for (const Entry& e : done) CHECK(!(e.current && e.state == State::kRefused));  // the refused song is never the current one
    { int cur = 0; for (const Entry& e : done) cur += e.current; CHECK(cur == 1); }
    // order: by file name, case-insensitive
    for (size_t i = 1; i < done.size(); ++i) CHECK(Lower(done[i - 1].file) <= Lower(done[i].file));
  }

  // ---------------------------------------------------------------- choices.txt
  SECTION("choices.txt");
  {
    lib.Refresh();
    auto v = lib.Entries();
    CHECK(v.size() >= 2);
    // no line: the first song is the current one (what the game plays)
    CHECK(v[0].current);
    int cur = 0;
    for (const Entry& e : v) cur += e.current;
    CHECK(cur == 1);
    // the in-game picker has written a line for a real wrestler key: ours must keep it
    const fs::path cf = ChoicesFile(mods);
    CHECK(Library::WriteText(cf, "# written by the picker\nK123\tsong.wav\n").empty());
    CHECK(lib.SetCurrent("song_art.mp3").empty());
    CHECK(Library::ChosenFile(mods) == "song_art.mp3");
    const std::string text = Library::ReadText(cf);
    CHECK(text.find("K123\tsong.wav") != std::string::npos);  // the picker's line survived
    CHECK(text.find("*\tsong_art.mp3") != std::string::npos);
    auto parsed = wwe2k14::user_playlist::ParseChoices(text);  // round trip through the picker's own parser
    CHECK(parsed.size() == 2);
    lib.Refresh();
    v = lib.Entries();
    CHECK(Find(v, "song_art.mp3") && Find(v, "song_art.mp3")->current && !Find(v, "song.wav")->current);
    // one "*" line, not one per pick
    CHECK(lib.SetCurrent("song.wav").empty());
    CHECK(lib.SetCurrent("song_art.mp3").empty());
    const std::string t2 = Library::ReadText(cf);
    size_t stars = 0;
    for (size_t p = 0; (p = t2.find("\n*\t", p)) != std::string::npos; ++p) ++stars;
    CHECK(stars == 1);
    // a song that is not in the library cannot be chosen; a path is not a name
    CHECK(!lib.SetCurrent("nope.mp3").empty());
    CHECK(!lib.SetCurrent("../x.mp3").empty());
    CHECK(!lib.Remove("../x.mp3").empty() && !lib.Remove("nope.mp3").empty());
    // removing the chosen song: the file and every line naming it go; the game falls back to the first song
    CHECK(lib.Remove("song_art.mp3").empty());
    CHECK(!fs::exists(LibraryDir(mods) / "song_art.mp3"));
    CHECK(Library::ChosenFile(mods).empty());
    CHECK(Library::ReadText(cf).find("song_art.mp3") == std::string::npos);
    CHECK(Library::ReadText(cf).find("K123\tsong.wav") != std::string::npos);
    v = lib.Entries();
    CHECK(!v.empty() && v[0].current);
    // the original of a removed song was never ours to delete
    CHECK(fs::exists(samples / "song_art.mp3"));
  }

  // ---------------------------------------------------------------- a silent song is not refused (lane 283: it plays as it is)
  SECTION("silent song");
  {
    const fs::path mods3 = work / "mods3";
    Library l3(mods3);
    Copy(samples / "silent.wav", StagingDir(mods3, "library") / "silent.wav");
    l3.ImportIncoming();
    l3.WaitIdle();
    CHECK(Has(l3.TakeMessages(), true, "Added"));
    l3.Refresh();
    const auto v = l3.Entries();
    CHECK(v.size() == 1 && v[0].state == State::kReady && !v[0].meta.normalised);
    // the game finds converted songs through <cache>/index.txt: the import wrote it, and a lookup by path answers
    CHECK(fs::is_regular_file(CacheDir(mods3) / "index.txt"));
    CHECK(audio_import::Lookup(LibraryDir(mods3) / "silent.wav", CacheDir(mods3)).ok);
  }

  // ---------------------------------------------------------------- review round: start-up pass, stray files, tasks, themes
  SECTION("start-up pass / stray files / task / themes");
  {
    // everything already converted: a NEW Library over the same folder lists it at once, nothing queued, no card
    Library l4(mods);
    l4.Refresh();
    const auto v = l4.Entries();
    bool all_ready = !v.empty();
    for (const Entry& e : v) all_ready &= e.state != State::kConverting;
    CHECK(all_ready && l4.Pending() == 0 && !l4.Busy());
    // a half-copied *.part and a stray file in the staging folder are never imported (and not counted)
    std::ofstream(StagingDir(mods, "library") / "half.mp3.part") << "x";
    std::ofstream(StagingDir(mods, "library") / "readme.txt") << "x";
    l4.ImportIncoming();
    CHECK(!l4.Busy() && l4.TakeMessages().empty());
    // a task runs on the worker, and its song is converted afterwards (a theme file in place)
    Copy(samples / "song.flac", MusicDir(mods) / "343.flac");
    bool ran = false;
    l4.Task([&ran] { ran = true; return Message{true, "task done"}; }, MusicDir(mods) / "343.flac");
    l4.WaitIdle();
    const auto msgs = l4.TakeMessages();
    CHECK(ran && Has(msgs, true, "task done"));
    CHECK(audio_import::Lookup(MusicDir(mods) / "343.flac", CacheDir(mods)).ok);
    // a theme file dropped in by hand with no cache copy converts at the first Refresh of a new Library
    const fs::path mods5 = work / "mods5";
    Copy(samples / "song.ogg", MusicDir(mods5) / "100.ogg");
    Library l5(mods5);
    l5.Refresh();
    CHECK(l5.Busy() || audio_import::Lookup(MusicDir(mods5) / "100.ogg", CacheDir(mods5)).ok);
    l5.WaitIdle();
    CHECK(audio_import::Lookup(MusicDir(mods5) / "100.ogg", CacheDir(mods5)).ok);
  }

  // ---------------------------------------------------------------- lane 297b review fixes
  SECTION("297b: start-up scan keeps a refused theme / theme replaced only after it converts / staging per purpose");
  {
    // SF2: a theme file the importer refuses (not audio) must SURVIVE the start-up scan (the old code deleted it); the message says so
    const fs::path mods6 = work / "mods6";
    Copy(samples / "garbage.mp3", MusicDir(mods6) / "343.mp3");
    Library l6(mods6);
    l6.Refresh();
    l6.WaitIdle();
    CHECK(fs::exists(MusicDir(mods6) / "343.mp3"));
    CHECK(Has(l6.TakeMessages(), false, "cannot be played"));
    CHECK(!audio_import::Lookup(MusicDir(mods6) / "343.mp3", CacheDir(mods6)).ok);  // (index.txt's refusal row keeps the game off it)
    // ... while a pick the player JUST made and the importer refuses is still turned away and deleted (Check without keep)
    Copy(samples / "garbage.mp3", MusicDir(mods6) / "344.mp3");
    l6.Check({MusicDir(mods6) / "344.mp3"});
    l6.WaitIdle();
    CHECK(!fs::exists(MusicDir(mods6) / "344.mp3") && Has(l6.TakeMessages(), false, "Not added"));

    // a theme PICK: the earlier theme goes only after the new one converts
    const fs::path mods7 = work / "mods7";
    Copy(samples / "song.flac", MusicDir(mods7) / "200.flac");
    Library l7(mods7);
    Copy(samples / "garbage.mp3", StagingDir(mods7, "theme") / "200.mp3");
    l7.InstallTheme(StagingDir(mods7, "theme") / "200.mp3", "200");
    l7.WaitIdle();
    CHECK(Has(l7.TakeMessages(), false, "earlier theme is kept"));
    CHECK(fs::exists(MusicDir(mods7) / "200.flac") && !fs::exists(StagingDir(mods7, "theme") / "200.mp3") && !fs::exists(MusicDir(mods7) / "200.mp3"));
    Copy(samples / "song.ogg", StagingDir(mods7, "theme") / "200.ogg");
    l7.InstallTheme(StagingDir(mods7, "theme") / "200.ogg", "200");
    l7.WaitIdle();
    CHECK(Has(l7.TakeMessages(), true, "Theme set"));
    CHECK(!fs::exists(MusicDir(mods7) / "200.flac") && fs::exists(MusicDir(mods7) / "200.ogg") && !fs::exists(StagingDir(mods7, "theme") / "200.ogg"));
    CHECK(audio_import::Lookup(MusicDir(mods7) / "200.ogg", CacheDir(mods7)).ok);

    // "@library" and "@new" no longer share a staging folder: the library import does not take what the NEW SONGS pick staged
    Copy(samples / "song.wav", StagingDir(mods7, "new") / "new_one.wav");
    l7.ImportIncoming();
    l7.WaitIdle();
    CHECK(fs::exists(StagingDir(mods7, "new") / "new_one.wav") && !fs::exists(LibraryDir(mods7) / "new_one.wav"));
    l7.ImportIncoming(MusicDir(mods7) / "new");
    l7.WaitIdle();
    CHECK(fs::exists(MusicDir(mods7) / "new" / "new_one.wav") && !fs::exists(StagingDir(mods7, "new") / "new_one.wav"));
  }

  std::printf("\nlibrary_model_test: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
