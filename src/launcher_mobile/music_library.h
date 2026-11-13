// wwe2k14 phone launcher - the song LIBRARY of the MODS > MUSIC group (lane 297, 2026-11-13).
//
// Jarrod: custom music on Android, "clean and functional", the visuals inspired by the PC launcher: a row per song with its album art,
// title / artist / duration, loudness and the gain the importer applied, and a play button over the thumbnail.
//
// The same folder and file as the in-game picker and USER PLAYLIST (docs\systems\music.md, Library design / picker):
//   <mods>/music/library/            the songs (the player's own copies; the original stays where it was)
//   <mods>/music/library/choices.txt "KEY<TAB>file name"; the key "*" is the song for every entrance until the game's own
//                                    wrestler key is confirmed (user_playlist_core.h; this file only ever writes that one key)
//   <mods>/music/.incoming/          where the system file picker drops what the player chose (a staging folder: a file is
//                                    imported from here and then gone, or refused and gone; never played from here)
//   <mods>/music/.cache/             audio_import's normalised copies, thumbnails and tag files (lane 283)
//
// No Windows, no SDL, no ImGui in this header: the phone launcher and the host test (tools\music\library_model_test.cpp) both build it.
// Everything that can take time (decoding, loudness, cover art, the copy) runs on ONE worker thread, in order, so a phone never
// decodes two songs at once; the UI thread only calls Refresh() / Entries() / TakeMessages(), which never block on a decode.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <functional>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../audio_import/audio_import.h"
#include "../user_playlist_core.h"

namespace mlauncher::musiclib {

namespace fs = std::filesystem;
namespace up = wwe2k14::user_playlist;

inline fs::path MusicDir(const fs::path& mods) { return mods / "music"; }
inline fs::path LibraryDir(const fs::path& mods) { return mods / "music" / "library"; }
inline fs::path IncomingDir(const fs::path& mods) { return mods / "music" / ".incoming"; }
inline fs::path CacheDir(const fs::path& mods) { return mods / "music" / ".cache"; }
// Lane 297b: each picker purpose has its own staging folder under .incoming ("library", "new", "theme"), so one purpose never imports
// (or deletes) what another one just copied.
inline fs::path StagingDir(const fs::path& mods, const std::string& purpose) { return IncomingDir(mods) / purpose; }
inline fs::path ChoicesFile(const fs::path& mods) { return LibraryDir(mods) / "choices.txt"; }

inline std::string U8(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}
inline std::string Lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// ---- what the rows say -------------------------------------------------------------------------------------------------------
inline std::string FormatTime(double sec) {
  const int t = int(sec + 0.5);
  char b[24];
  std::snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
  return b;
}
// "-17.5 LUFS" and, small, "gain +3.2 dB" (a "limited" tag when the peak limiter had to act).
inline std::string LoudnessText(const audio_import::Song& s) {
  char b[48];
  std::snprintf(b, sizeof b, "%.1f LUFS", s.out_lufs);
  return b;
}
inline std::string GainText(const audio_import::Song& s) {
  char b[64];
  std::snprintf(b, sizeof b, "gain %+.1f dB%s", s.gain_db, s.limited ? ", limited" : "");
  return b;
}
// The line under the title: "Artist  -  Album" with whatever the file had.
inline std::string ByLine(const audio_import::Song& s) {
  std::string by = s.artist;
  if (!s.album.empty()) by += (by.empty() ? "" : "  -  ") + s.album;
  return by;
}

// ---- the library ---------------------------------------------------------------------------------------------------------------
enum class State {
  kConverting,  // read / converted in the background right now (or waiting for its turn)
  kReady,       // meta.ok: normalised copy, tags and (maybe) art are there
  kRefused,     // meta.err says why: the file cannot be played
};

struct Entry {
  std::string file;  // file name in the library folder (also the name choices.txt uses)
  fs::path path;
  State state = State::kConverting;
  audio_import::Song meta;  // valid when state != kConverting
  bool current = false;     // the song the game plays (choices.txt "*", else the first song)
  // The preview player plays WAV only: a normalised copy always is; a song that plays as it is (silent / too short) only if it is a .wav.
  bool previewable() const {
    return state == State::kReady && (meta.normalised || Lower(U8(meta.audio.extension())) == ".wav");
  }
  bool has_art() const { return state == State::kReady && !meta.thumb.empty(); }
  std::string title() const { return state == State::kReady && !meta.title.empty() ? meta.title : U8(fs::u8path(file).stem()); }
};

struct Message {
  bool ok = true;
  std::string text;  // a sentence for the toast
};

class Library {
 public:
  explicit Library(fs::path mods) : sh_(std::make_shared<Shared>()) {
    sh_->mods = std::move(mods);
    sh_->cache = CacheDir(sh_->mods);
    // Half-written files of an earlier run (a killed copy / conversion): *.part older than 10 minutes, in the cache and the song folders.
    std::thread([sh = sh_] {
      try {
        audio_import::CleanStale(sh->cache);
        const auto limit = std::chrono::minutes(10);
        std::error_code ec;
        for (const fs::path& d : {LibraryDir(sh->mods), IncomingDir(sh->mods), StagingDir(sh->mods, "library"), StagingDir(sh->mods, "new"),
                                  StagingDir(sh->mods, "theme"), MusicDir(sh->mods), MusicDir(sh->mods) / "new"})
          for (const auto& e : fs::directory_iterator(d, ec)) {
            if (!e.is_regular_file(ec) || U8(e.path().filename()).size() < 6 || e.path().extension() != ".part") continue;
            if (fs::file_time_type::clock::now() - e.last_write_time(ec) > limit) fs::remove(e.path(), ec);
          }
      } catch (...) {
      }
    }).detach();
  }
  Library(const Library&) = delete;
  Library& operator=(const Library&) = delete;
  ~Library() {
    {
      std::lock_guard lock(sh_->m);
      sh_->quit = true;
    }
    sh_->cv.notify_all();  // the worker owns a share of the state, so it may outlive us safely
  }

  const fs::path& mods() const { return sh_->mods; }

  // The folder again (cheap): songs with no answer yet are queued for the worker. Call every second or two and after an event.
  void Refresh() {
    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(LibraryDir(sh_->mods), ec)) {
      if (!e.is_regular_file(ec)) continue;
      const std::string name = U8(e.path().filename());
      if (name.empty() || name[0] == '.' || !audio_import::IsSupportedExt(e.path())) continue;
      files.push_back(e.path());
    }
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
      const std::string la = Lower(U8(a.filename())), lb = Lower(U8(b.filename()));
      return la != lb ? la < lb : a < b;
    });
    std::vector<Entry> v;
    std::vector<fs::path> todo;
    {
      std::lock_guard lock(sh_->m);
      for (const fs::path& f : files) {
        Entry e;
        e.file = U8(f.filename()), e.path = f;
        const std::string key = KeyOf(f);
        auto it = sh_->meta.find(key);
        if (it != sh_->meta.end()) {
          e.meta = it->second;
          e.state = e.meta.ok ? State::kReady : State::kRefused;
        } else if (!sh_->queued.count(key)) {
          // The cache first (stat + small files): a song that is already converted is listed at once, with no card and no work.
          audio_import::Song s = audio_import::LibraryCached(f, sh_->cache);
          if (s.ok || !s.err.empty()) {
            sh_->meta[key] = s;
            e.meta = std::move(s);
            e.state = e.meta.ok ? State::kReady : State::kRefused;
          } else {
            todo.push_back(f);  // real work: not converted yet
          }
        }
        v.push_back(std::move(e));
      }
      for (const fs::path& f : todo) {
        sh_->queued.insert(KeyOf(f));
        EnqueueLocked({Job::kRead, f, false, {}});
      }
      sh_->entries = std::move(v);
      MarkCurrentLocked();
    }
    if (!themes_scanned_) {  // once: wrestlers' own theme files <mods>/music/<id>.<ext> with no current cache copy convert too
      themes_scanned_ = true;
      std::vector<fs::path> th;
      for (const auto& e : fs::directory_iterator(MusicDir(sh_->mods), ec)) {
        const std::string stem = U8(e.path().stem());
        if (!e.is_regular_file(ec) || stem.empty() || stem.find_first_not_of("0123456789") != std::string::npos) continue;
        if (!audio_import::IsSupportedExt(e.path())) continue;
        const audio_import::Imported l = audio_import::Lookup(e.path(), sh_->cache);
        if (!l.ok && l.err.empty()) th.push_back(e.path());
      }
      if (!th.empty()) Check(std::move(th), /*keep=*/true);  // lane 297b: a start-up scan never deletes the player's file
    }
    if (!todo.empty()) Kick();
  }
  bool themes_scanned_ = false;

  // A snapshot for drawing (a copy: the worker may be changing the original).
  std::vector<Entry> Entries() const {
    std::lock_guard lock(sh_->m);
    return sh_->entries;
  }

  // Songs the player chose: `files` (full paths; the system picker's staging folder). Each is imported in turn on the worker thread;
  // a file that cannot be turned into audio is refused with the importer's sentence and NOTHING is added; `remove_source` deletes the
  // staged file afterwards (added or refused). Returns at once.
  // dest = the folder the copy goes to (empty = the library; the NEW SONGS list passes <mods>/music/new).
  void Import(std::vector<fs::path> files, bool remove_source, fs::path dest = {}) {
    {
      std::lock_guard lock(sh_->m);
      for (auto& f : files) EnqueueLocked({Job::kImport, std::move(f), remove_source, dest});
    }
    Kick();
  }
  // Files the picker already put where they belong (a wrestler's own theme <mods>/music/<id>.ext): decode + measure them now, so a
  // file that is not audio is refused with the importer's sentence and DELETED (it is the app's copy; the original is untouched).
  // keep (lane 297b): a refusal only REPORTS ("cannot be played: why"; index.txt's refusal row already stops the game using the file) and the
  // file stays: used by the start-up scan, where a hand-copied file or a decoder stall on a version-bump re-check must never cost a theme.
  void Check(std::vector<fs::path> files, bool keep = false) {
    {
      std::lock_guard lock(sh_->m);
      for (auto& f : files) {
        Job j{Job::kCheck, std::move(f), false, {}};
        j.keep = keep;
        EnqueueLocked(std::move(j));
      }
    }
    Kick();
  }
  // Everything in the staging folder (what the picker just copied there).
  // Work that touches files (copying a song to a wrestler's theme ...) runs here too, never on the UI thread; n returns the sentence for
  // the toast. fter (optional) is converted with the card afterwards (a cache hit for a song that is already converted).
  void Task(std::function<Message()> fn, fs::path after = {}) {
    {
      std::lock_guard lock(sh_->m);
      Job j{Job::kTask, {}, false, {}};
      j.fn = std::move(fn), j.after = std::move(after);
      EnqueueLocked(std::move(j));
    }
    Kick();
  }
  void ImportIncoming(fs::path dest = {}) {
    std::vector<fs::path> files;
    std::error_code ec;
    // (lane 297b: the library list and the NEW SONGS list stage in their own folders)
    for (const auto& e : fs::directory_iterator(StagingDir(sh_->mods, dest.empty() ? "library" : "new"), ec))
      if (e.is_regular_file(ec) && U8(e.path().filename()).rfind(".", 0) != 0 && audio_import::IsSupportedExt(e.path()))
        files.push_back(e.path());  // (a half-copied *.part, a stray file: never imported)
    std::sort(files.begin(), files.end());
    if (!files.empty()) Import(std::move(files), true, std::move(dest));
  }
  // Lane 297b: a wrestler's theme picked into StagingDir(mods,"theme") as <stem><ext>. The earlier theme stays until the new file has
  // converted: only then are the old <stem>.* files removed and the staged one moved to <mods>/music/<stem><ext>. A refusal (or a write
  // error) deletes the staged file only and says so; the earlier theme is untouched.
  void InstallTheme(fs::path staged, std::string stem) {
    std::shared_ptr<Shared> sh = sh_;
    const fs::path dest_dir = MusicDir(sh->mods), final_path = dest_dir / fs::u8path(stem + U8(staged.extension()));
    Task(
        [sh, staged, stem, dest_dir, final_path]() -> Message {
          std::error_code ec;
          const std::string shown = U8(staged.filename());
          if (!fs::is_regular_file(staged, ec)) return {false, "Not given: " + shown + " was not found"};
          const audio_import::Imported imp = Convert(*sh, staged);
          if (imp.cancelled) {
            fs::remove(staged, ec);
            return {false, "Cancelled: your earlier theme is kept"};
          }
          if (!imp.ok) {
            fs::remove(staged, ec);
            return {false, "Not given: " + shown + ": " + imp.err + " (your earlier theme is kept)"};
          }
          for (const auto& e : fs::directory_iterator(dest_dir, ec))
            if (e.is_regular_file(ec) && U8(e.path().stem()) == stem && audio_import::IsSupportedExt(e.path()) && e.path() != final_path)
              fs::remove(e.path(), ec);
          fs::rename(staged, final_path, ec);
          if (ec) {
            fs::remove(staged, ec);
            return {false, "Not given: " + shown + ": " + ec.message()};
          }
          return {true, "Theme set: " + (imp.title.empty() ? shown : imp.title)};
        },
        final_path);  // converted again afterwards under its final path (a cache hit: same bytes)
  }

  // Songs being imported or read right now, waiting ones included (the UI says "converting...").
  int Pending() const { return sh_->pending.load(); }
  int Importing() const { return sh_->importing.load(); }

  // ---- the progress bar (Jarrod: a song is converted from the moment it is added, with a bar, not quietly in the background) ----
  // One batch = everything queued since the worker was last idle ("song 2 of 5"). rac is the CURRENT song's fraction 0..1 as the
  // importer reports it through Report() (-1 = it has not said: the bar shows the batch only).
  struct Progress {
    bool active = false;
    int done = 0, total = 0;
    std::string name, stage;
    float frac = -1;
    float overall() const { return total > 0 ? std::min(1.f, (float(done) + (frac >= 0 ? frac : 0.f)) / float(total)) : 0.f; }
  };
  Progress Snapshot() const {
    std::lock_guard lock(sh_->m);
    Progress p;
    p.active = !sh_->jobs.empty() || sh_->busy;
    p.done = sh_->done, p.total = sh_->total, p.name = sh_->cur_name, p.stage = sh_->stage, p.frac = sh_->frac;
    return p;
  }
  bool Busy() const { return Snapshot().active; }
  // The importer's progress callback (lane 283) calls this: the stage ("decoding", "measuring", ...) and the fraction of the song. It
  // returns false when the player pressed CANCEL: the importer then stops that song at its next step.
  bool Report(const std::string& stage, float frac) {
    std::lock_guard lock(sh_->m);
    sh_->stage = stage, sh_->frac = frac;
    return !sh_->cancel_current;
  }
  // CANCEL: songs not started yet are dropped (their staged files deleted, nothing added); the song in progress is stopped by the
  // importer through Report() (until it does, it finishes). One message says what was left out.
  void Cancel() {
    std::lock_guard lock(sh_->m);
    sh_->cancel_current = true;
    int dropped = 0;
    std::error_code ec;
    for (const Job& j : sh_->jobs) {
      if (j.kind == Job::kImport && j.remove_source) fs::remove(j.path, ec);
      if (j.kind == Job::kRead) {  // an unread song of the library: listed as not converted (it is not wanted now)
        audio_import::Song s;
        s.err = "not converted (cancelled): remove it or ADD it again";
        sh_->meta[KeyOf(j.path)] = s;
        sh_->queued.erase(KeyOf(j.path));
      }
      ++dropped;
    }
    sh_->jobs.clear();
    sh_->total = sh_->done + (sh_->busy ? 1 : 0);
    if (dropped) sh_->messages.push_back({false, "Cancelled: " + std::to_string(dropped) + " song(s) not added"});
  }
  // What happened since the last call (one per imported / refused file).
  std::vector<Message> TakeMessages() {
    std::lock_guard lock(sh_->m);
    std::vector<Message> out;
    out.swap(sh_->messages);
    return out;
  }
  // For tests: blocks until the worker has nothing left.
  void WaitIdle() const {
    std::unique_lock lock(sh_->m);
    sh_->cv_idle.wait(lock, [this] { return sh_->jobs.empty() && !sh_->busy; });
  }

  // ---- choices.txt (the in-game picker's file; we only ever write the "*" key) --------------------------------------------------
  // The file name chosen for every entrance, "" when there is no line (the game then plays the first song).
  static std::string ChosenFile(const fs::path& mods) {
    const auto c = up::ParseChoices(ReadText(ChoicesFile(mods)));
    for (auto it = c.rbegin(); it != c.rend(); ++it)
      if (it->first == up::kAnyKey) return it->second;
    return {};
  }
  // "" = done, else a sentence. The file must be a song of the library.
  std::string SetCurrent(const std::string& file) {
    if (!SafeName(file)) return "bad file name";
    std::error_code ec;
    if (!fs::is_regular_file(LibraryDir(sh_->mods) / fs::u8path(file), ec)) return "that song is not in the library any more";
    const std::string err = WriteText(ChoicesFile(sh_->mods), up::UpsertChoice(ReadText(ChoicesFile(sh_->mods)), up::kAnyKey, file));
    if (err.empty()) {
      std::lock_guard lock(sh_->m);
      MarkCurrentLocked();
    }
    return err;
  }
  // Deletes the player's copy (the original stays where it was) and every choices.txt line that named it; "" = done.
  std::string Remove(const std::string& file) {
    if (!SafeName(file)) return "bad file name";
    std::error_code ec;
    const fs::path p = LibraryDir(sh_->mods) / fs::u8path(file);
    if (!fs::remove(p, ec)) return ec ? "remove failed: " + ec.message() : "file not found";
    auto c = up::ParseChoices(ReadText(ChoicesFile(sh_->mods)));
    const size_t before = c.size();
    c.erase(std::remove_if(c.begin(), c.end(), [&](const auto& kv) { return Lower(kv.second) == Lower(file); }), c.end());
    if (c.size() != before) WriteText(ChoicesFile(sh_->mods), up::FormatChoices(c));
    Refresh();
    return "";
  }

  // ---- small file helpers (also the test's) -------------------------------------------------------------------------------------
  static bool SafeName(const std::string& n) { return !n.empty() && n.find_first_of("\\/") == std::string::npos && n[0] != '.'; }
  static std::string ReadText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }
  static std::string WriteText(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = fs::path(p).concat(".part");
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      out << text;
      if (!out) return "could not write " + U8(p.filename());
    }
    fs::rename(tmp, p, ec);
    return ec ? "could not write " + U8(p.filename()) + ": " + ec.message() : "";
  }

 private:
  struct Job {
    enum Kind { kRead, kImport, kCheck, kTask } kind;
    fs::path path;
    bool remove_source;
    fs::path dest;
    std::function<Message()> fn;  // kTask: runs on the worker; fter = a file to convert afterwards (its song, with the card)
    fs::path after;
    bool keep = false;  // kCheck: never delete the file on a refusal (the start-up scan)
  };
  struct Shared {
    fs::path mods, cache;
    mutable std::mutex m;
    std::condition_variable cv, cv_idle;
    std::deque<Job> jobs;
    std::map<std::string, audio_import::Song> meta;  // key = name|size|mtime
    std::set<std::string> queued;
    std::vector<Entry> entries;
    std::vector<Message> messages;
    bool quit = false, busy = false, started = false, cancel_current = false;
    int total = 0, done = 0;
    std::string cur_name, stage;
    float frac = -1;
    std::atomic<int> pending{0}, importing{0};
  };

  static std::string KeyOf(const fs::path& f) {
    std::error_code ec;
    return U8(f) + "|" + std::to_string((unsigned long long)fs::file_size(f, ec)) + "|" + std::to_string((long long)fs::last_write_time(f, ec).time_since_epoch().count());
  }

  // `m` held.
  void MarkCurrentLocked() {
    // (a song the importer refused is skipped by the game's playlist, so it is never the current one)
    std::vector<fs::path> paths;
    std::vector<size_t> idx;
    for (size_t k = 0; k < sh_->entries.size(); ++k)
      if (sh_->entries[k].state != State::kRefused) paths.push_back(sh_->entries[k].path), idx.push_back(k);
    const size_t i = up::ChooseSong(up::ParseChoices(ReadText(ChoicesFile(sh_->mods))), "", paths);
    for (Entry& e : sh_->entries) e.current = false;
    if (i != size_t(-1)) sh_->entries[idx[i]].current = true;
  }

  // m held.
  void EnqueueLocked(Job j) {
    if (sh_->jobs.empty() && !sh_->busy) sh_->total = sh_->done = 0;  // a new batch
    sh_->jobs.push_back(std::move(j));
    ++sh_->total;
  }

  void Kick() {
    std::shared_ptr<Shared> sh = sh_;
    std::lock_guard lock(sh->m);
    sh->pending = int(sh->jobs.size()) + (sh->busy ? 1 : 0);
    if (!sh->started) {
      sh->started = true;
      std::thread([sh] { Worker(sh); }).detach();
    }
    sh->cv.notify_one();
  }

  static void Push(Shared& sh, bool ok, std::string text) {
    std::lock_guard lock(sh.m);
    sh.messages.push_back({ok, std::move(text)});
  }

  static void Worker(std::shared_ptr<Shared> sh) {
    for (;;) {
      Job job;
      {
        std::unique_lock lock(sh->m);
        sh->busy = false;
        sh->cv_idle.notify_all();
        sh->cv.wait(lock, [&] { return sh->quit || !sh->jobs.empty(); });
        if (sh->jobs.empty()) return;  // quit with nothing left
        job = std::move(sh->jobs.front());
        sh->jobs.pop_front();
        sh->busy = true;
        sh->pending = int(sh->jobs.size()) + 1;
        sh->cur_name = U8(job.path.filename()), sh->frac = -1, sh->cancel_current = false;
        sh->stage = job.kind == Job::kRead ? "reading" : "converting";
        if (job.kind != Job::kRead) ++sh->importing;
      }
      try {
      if (job.kind == Job::kRead) {
        // A song already in the library: the cache is asked first (stat + small files); a song with no current copy (a new file, a cache
        // version bump, dropped in by hand) is converted NOW with the progress card, and its index.txt row is written by Import.
        audio_import::Song s = audio_import::LibraryCached(job.path, sh->cache);
        if (!s.ok && s.err.empty()) {
          const audio_import::Imported imp = Convert(*sh, job.path);
          if (imp.cancelled) {  // CANCEL (the batch was dropped; this one was running)
            s = audio_import::Song();
            s.err = "not converted (cancelled): remove it or ADD it again";
          } else {
            s = audio_import::LibraryCached(job.path, sh->cache);
            if (!s.ok && s.err.empty()) s.err = imp.err.empty() ? "this song could not be converted" : imp.err;
          }
        }
        std::lock_guard lock(sh->m);
        sh->meta[KeyOf(job.path)] = std::move(s);
        sh->queued.erase(KeyOf(job.path));
      } else if (job.kind == Job::kTask) {
        const Message m = job.fn ? job.fn() : Message{false, "nothing to do"};
        Push(*sh, m.ok, m.text);
        if (m.ok && !job.after.empty()) CheckOne(*sh, Job{Job::kCheck, job.after, false, {}}, true);
        --sh->importing;
      } else if (job.kind == Job::kCheck) {
        CheckOne(*sh, job);
        --sh->importing;
      } else {
        ImportOne(*sh, job);
        --sh->importing;
      }
      } catch (const std::exception& ex) {  // a name or file that throws must not end the worker (and with it the launcher)
        if (job.kind != Job::kRead) --sh->importing;
        std::lock_guard lock(sh->m);
        if (job.kind == Job::kRead) {
          audio_import::Song bad;
          bad.err = std::string("this song could not be read: ") + ex.what();
          sh->meta[KeyOf(job.path)] = bad;  // (KeyOf may itself throw on a bad name: then the catch is the last word)
          sh->queued.erase(KeyOf(job.path));
        }
        sh->messages.push_back({false, std::string("Song skipped: ") + ex.what()});
      }
      sh->pending = int(sh->jobs.size());  // (the one that just ended is no longer pending)
      {
        std::lock_guard lock(sh->m);
        ++sh->done, sh->cur_name.clear(), sh->frac = -1;
      }
    }
  }

  // Import with the progress callback: stage + the song's overall fraction go to the card; CANCEL makes the importer stop (it then writes
  // nothing, indexes nothing and leaves no .part). Every successful Import writes <cache>/index.txt, which the game reads.
  static audio_import::Imported Convert(Shared& sh, const fs::path& path) {
    return audio_import::Import(path, sh.cache, audio_import::kTargetLufs, [&sh](const audio_import::Progress& p) {
      std::lock_guard lock(sh.m);
      sh.stage = audio_import::StageName(p.stage), sh.frac = p.overall;
      return !sh.cancel_current;
    });
  }

  static void CheckOne(Shared& sh, const Job& job, bool quiet = false) {
    std::error_code ec;
    const std::string shown = U8(job.path.filename());
    const audio_import::Imported imp = Convert(sh, job.path);
    if (imp.cancelled) return;  // the file stays as it is
    if (!imp.ok && !imp.io_failed && job.keep) {  // the start-up scan: report, keep the file
      return Push(sh, false, shown + ": cannot be played: " + imp.err);
    }
    if (!imp.ok && !imp.io_failed) {  // refused (not audio): our copy goes; a write error (full disk) is not the song's fault, the file stays
      fs::remove(job.path, ec);
      return Push(sh, false, "Not added: " + shown + ": " + imp.err);
    }
    if (!imp.ok) return Push(sh, false, shown + ": could not be converted: " + imp.err + " (the file is kept)");
    if (!quiet) Push(sh, true, "Added: " + (imp.title.empty() ? shown : imp.title));
  }

  static void ImportOne(Shared& sh, const Job& job) {
    std::error_code ec;
    const std::string shown = U8(job.path.filename());
    auto drop_source = [&] {
      if (job.remove_source) fs::remove(job.path, ec);
    };
    if (!fs::is_regular_file(job.path, ec)) return Push(sh, false, "Not added: " + shown + " was not found");
    // 1. decode + measure + normalise FIRST: a file that cannot become audio is refused here, with the importer's own sentence,
    //    and nothing is copied into the library.
    const audio_import::Imported imp = Convert(sh, job.path);
    if (imp.cancelled) return drop_source();  // (Cancel() has said what was left out)
    if (!imp.ok) {
      drop_source();
      return Push(sh, false, "Not added: " + shown + ": " + imp.err);
    }
    // 2. the library already has this very song (same bytes, whatever its name): say so, add nothing
    const fs::path lib = job.dest.empty() ? LibraryDir(sh.mods) : job.dest;
    fs::create_directories(lib, ec);
    for (const auto& e : fs::directory_iterator(lib, ec)) {
      if (!e.is_regular_file(ec) || !audio_import::IsSupportedExt(e.path())) continue;
      if (fs::file_size(e.path(), ec) != fs::file_size(job.path, ec)) continue;
      if (audio_import::ContentKey(e.path(), audio_import::kTargetLufs) == imp.key) {
        drop_source();
        return Push(sh, false, shown + " is already in your library as " + U8(e.path().filename()));
      }
    }
    // 3. a free file name in the library (a different song with the same name keeps both: "Name (2).mp3")
    std::string name = U8(job.path.filename());
    for (char& c : name)
      if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    while (!name.empty() && name[0] == '.') name.erase(name.begin());
    if (name.empty()) name = "song" + std::string(U8(job.path.extension()));
    {
      const fs::path stem = fs::u8path(name).stem(), ext = fs::u8path(name).extension();
      for (int n = 2; fs::exists(lib / fs::u8path(name), ec) && n < 1000; ++n)
        name = U8(stem) + " (" + std::to_string(n) + ")" + U8(ext);
    }
    // 4. the copy, whole, to a .part file first (a half-copied song is never listed), then renamed
    const fs::path dst = lib / fs::u8path(name), tmp = lib / fs::u8path(name + ".part");
    fs::copy_file(job.path, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      fs::remove(tmp, ec);
      drop_source();
      return Push(sh, false, "Not added: " + shown + ": the copy failed (" + ec.message() + ")");
    }
    fs::rename(tmp, dst, ec);
    if (ec) {
      fs::remove(tmp, ec);
      drop_source();
      return Push(sh, false, "Not added: " + shown + ": " + ec.message());
    }
    drop_source();
    // The copy has its own path, so it gets its own index.txt row: Import finds the same bytes in the cache (a hit, no decoding)
    // and writes the row the game looks the library song up by.
    audio_import::Import(dst, sh.cache);
    audio_import::Song s = audio_import::LibraryCached(dst, sh.cache);
    {
      std::lock_guard lock(sh.m);
      sh.meta[KeyOf(dst)] = s;
      sh.queued.erase(KeyOf(dst));
    }
    Push(sh, true, "Added: " + (s.title.empty() ? name : s.title));
  }

  std::shared_ptr<Shared> sh_;
};

// ---- the preview rule (Jarrod: never play on hover / focus) ---------------------------------------------------------------------
// The play button over a thumbnail is the ONLY thing that starts a preview. Moving the pointer, a keyboard / gamepad focus on the
// row, a scroll or a tap on the rest of the row never does. Tapping the song that is playing stops it; tapping another switches.
class PreviewState {
 public:
  enum class Do { kNothing, kStart, kStop };
  struct Act {
    Do what = Do::kNothing;
    std::string file;
  };

  Act OnPlayButton(const std::string& file) {
    if (!playing_.empty() && playing_ == file) return Stop();
    playing_ = file;
    return {Do::kStart, file};
  }
  // Hover, keyboard / pad focus, a tap on the row's body: nothing starts. (WWE_TEST_MUTATE_FOCUS_PLAYS makes this the known bug
  // the host test must catch: focus starting playback.)
  Act OnRowFocus(const std::string& file) {
#ifdef WWE_TEST_MUTATE_FOCUS_PLAYS
    if (playing_ != file) {
      playing_ = file;
      return {Do::kStart, file};
    }
#else
    (void)file;
#endif
    return {};
  }
  // The page was left, the app went to the background, the song was removed: whatever plays stops.
  Act OnLeave() { return playing_.empty() ? Act{} : Stop(); }
  Act OnRemoved(const std::string& file) { return !playing_.empty() && playing_ == file ? Stop() : Act{}; }
  // The player says it is not playing any more (the song finished, or it could not start).
  void OnEnded() { playing_.clear(); }
  const std::string& playing() const { return playing_; }

 private:
  Act Stop() {
    playing_.clear();
    return {Do::kStop, {}};
  }
  std::string playing_;
};

}  // namespace mlauncher::musiclib
