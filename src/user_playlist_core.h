// wwe2k14 - USER PLAYLIST core (lane 282, 2026-11-13): the song list and the play / pause / stop state machine behind
// the game's XMP "play the user's music" calls. No game, SDK or Windows includes: the host test
// (tools\music\user_playlist_test.cpp) builds this header alone. The glue (the guest hooks, the Media Foundation
// player) is src\user_playlist.h; the finding behind it is docs\systems\music.md "USER PLAYLIST / XMP".
//
// The rules (what a 360 owner's Dashboard playlist does, as far as the game can see it):
// - Play starts the first playable song; a song that is gone (removed since the scan) or that the player refuses is
//   skipped; no playable song = Play fails and nothing starts (the caller then does what it did before: the SDK's
//   "unimplemented" answer).
// - A song that ends starts the next one; the end of the list = Idle (no repeat: a playlist of one song stops after
//   the song, as the Dashboard's does; the game asks the status and sees the music is over).
// - Pause only from Playing, Continue only from Paused; Stop from anywhere = Idle. Play while playing starts again.
// - End-of-song events carry the token of the song start they belong to: an event of a song that was stopped or
//   replaced since is ignored (the player thread reports late).
#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace wwe2k14::user_playlist {

// The XMP states the game's status call returns (SDK XmpApp::State; game mapping sub_82AF6B90: 0 -> 1, 1 -> 8, 2 -> 72).
enum class State : uint32_t { Idle = 0, Playing = 1, Paused = 2 };

inline constexpr const char* kExts[] = {".mp3", ".m4a", ".wav", ".flac", ".wma", ".aac"};

// Paths and names as UTF-8 (review B2, lane 292): std::filesystem::path::string() converts through the ANSI code page and THROWS on a name it
// cannot hold (a CJK or emoji file name on an English Windows), which inside a guest hook is a crash. Every name that is kept, shown, saved
// in choices.txt or compared goes through U8(); a name read back is turned into a path with FromU8().
inline std::string U8(const std::filesystem::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}
inline std::filesystem::path FromU8(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

inline std::string Lower(std::string s) {
  for (auto& c : s) c = char((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
  return s;
}

inline bool IsSongName(const std::string& name) {
  const std::string l = Lower(name);
  for (const char* e : kExts) {
    const std::string ext = e;
    if (l.size() > ext.size() && l.compare(l.size() - ext.size(), ext.size(), ext) == 0) return true;
  }
  return false;
}

// The song file names among `names` (the entries of one folder), in file-name order, case-insensitive. Hidden dot
// files and non-songs are dropped.
inline std::vector<std::string> PickSongs(const std::vector<std::string>& names) {
  std::vector<std::string> out;
  for (const auto& n : names) {
    if (!n.empty() && n[0] != '.' && IsSongName(n)) out.push_back(n);
  }
  std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
    const std::string la = Lower(a), lb = Lower(b);
    return la != lb ? la < lb : a < b;
  });
  return out;
}

// Which folder is the playlist: <mods>\music\library\ when it holds a song, else <mods>\music\new\ (the new
// entrance songs). 0 = playlist, 1 = new, -1 = none.
inline int ChooseFolder(const std::vector<std::string>& playlist_names, const std::vector<std::string>& new_names) {
  if (!PickSongs(playlist_names).empty()) return 0;
  if (!PickSongs(new_names).empty()) return 1;
  return -1;
}

// ---- the per-wrestler choice (sidecar <library>\choices.txt: "KEY<TAB>file name" per line, '#' comments) ----
// KEY = whatever identifies the created entrance / wrestler (UNCONFIRMED which game value; the glue passes it in).
inline std::vector<std::pair<std::string, std::string>> ParseChoices(const std::string& text) {
  std::vector<std::pair<std::string, std::string>> out;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t e = text.find('\n', pos);
    if (e == std::string::npos) e = text.size();
    std::string line = text.substr(pos, e - pos);
    pos = e + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const size_t t = line.find('\t');
    if (t == std::string::npos || t == 0 || t + 1 >= line.size()) continue;  // malformed line: ignored
    out.emplace_back(line.substr(0, t), line.substr(t + 1));
  }
  return out;
}
inline std::string FormatChoices(const std::vector<std::pair<std::string, std::string>>& c) {
  std::string s = "# WWE 2K14 recomp: USER PLAYLIST song per wrestler. Key <TAB> file name in this folder.\n";
  for (const auto& [k, f] : c) s += k + "\t" + f + "\n";
  return s;
}
// The key under which a choice is saved until the game's wrestler / entrance key is confirmed (lane 291): it applies to
// every wrestler whose own key has no line. A line for a real key always wins over it.
inline constexpr const char* kAnyKey = "*";

// A choice for `key` (last line wins): the index in `songs` of the chosen file, size_t(-1) when there is no line or the
// file is gone from the list.
inline size_t FindChoice(const std::vector<std::pair<std::string, std::string>>& choices, const std::string& key,
                         const std::vector<std::filesystem::path>& songs) {
  for (auto it = choices.rbegin(); it != choices.rend(); ++it) {
    if (it->first != key) continue;
    for (size_t i = 0; i < songs.size(); ++i) {
      if (Lower(U8(songs[i].filename())) == Lower(it->second)) return i;
    }
    break;  // the chosen song is gone
  }
  return size_t(-1);
}
// Like ChooseSong but only an EXPLICIT choice (a line for `key`, else for kAnyKey, whose file is in the list): size_t(-1) otherwise. What the picker
// shows as "current": the default first song is not a choice.
inline size_t ExplicitChoice(const std::vector<std::pair<std::string, std::string>>& choices, const std::string& key,
                             const std::vector<std::filesystem::path>& songs) {
  if (!key.empty()) {
    const size_t i = FindChoice(choices, key, songs);
    if (i != size_t(-1)) return i;
  }
  return FindChoice(choices, kAnyKey, songs);
}
// The text of choices.txt with `key` set to `file` (a new line at the end; older lines of the key are dropped so the file
// does not grow by one line per pick).
inline std::string UpsertChoice(const std::string& text, const std::string& key, const std::string& file) {
  auto c = ParseChoices(text);
  c.erase(std::remove_if(c.begin(), c.end(), [&](const auto& p) { return p.first == key; }), c.end());
  c.emplace_back(key, file);
  return FormatChoices(c);
}

// ---- the wrestler key (lane 309) ----
// KEY = the character id of the wrestler whose custom entrance is being edited, as decimal text ("298" = AJ Lee: the roster's
// theme id is its character id, docs\systems\music.md). "" = unknown / unreadable (the caller then uses the kAnyKey line).
inline std::string KeyForCharacter(int64_t id) {
  return id >= 0 && id <= 4095 ? std::to_string(id) : std::string{};
}
// The picker's save: the wrestler's own line, and (until the ENTRANCE side can name its wrestler, docs\systems\music.md lane 309) the
// kAnyKey line too, so an entrance whose wrestler cannot be read still plays the latest pick like before. With no real key only the
// kAnyKey line is written.
inline std::string UpsertPick(const std::string& text, const std::string& key, const std::string& file) {
  if (key.empty() || key == kAnyKey) return UpsertChoice(text, kAnyKey, file);
  return UpsertChoice(UpsertChoice(text, key, file), kAnyKey, file);
}
// What the picker shows as the current choice: with a real key only that wrestler's own line (the kAnyKey line belongs to nobody in
// particular and would show another wrestler's pick); without a key the explicit choice as before.
inline size_t CurrentChoice(const std::vector<std::pair<std::string, std::string>>& choices, const std::string& key,
                            const std::vector<std::filesystem::path>& songs) {
  if (!key.empty() && key != kAnyKey) return FindChoice(choices, key, songs);
  return ExplicitChoice(choices, key, songs);
}
// The index in `songs` (full paths) of the song for `key`: its chosen file when still in the list, else the kAnyKey choice,
// else the FIRST song (the simple default, no shuffle). npos = no songs.
inline size_t ChooseSong(const std::vector<std::pair<std::string, std::string>>& choices, const std::string& key,
                         const std::vector<std::filesystem::path>& songs) {
  if (songs.empty()) return size_t(-1);
  if (!key.empty()) {
    const size_t i = FindChoice(choices, key, songs);
    if (i != size_t(-1)) return i;
  }
  const size_t any = FindChoice(choices, kAnyKey, songs);
  return any != size_t(-1) ? any : 0;
}

// ---- when the frame check stops the music (review S1, lane 291) ----
// The playlist plays at an entrance (in-game sequence 9) AND in the Create-an-Entrance editor's preview (sequence 25).
// It stops when the sequence it started in is left, or when the match clock runs. `play_seq` = the sequence at Play
// (-1 = it could not be read then: the entrance's 9), `seq` = now (-1 = unreadable now: counts as left, as before).
enum class StopWhy { None, SequenceLeft, MatchStarted };
inline StopWhy ShouldStop(int32_t play_seq, int32_t seq, uint32_t clock) {
  if (play_seq < 0) play_seq = 9;
  if (seq != play_seq) return StopWhy::SequenceLeft;
  if (clock > 0) return StopWhy::MatchStarted;
  return StopWhy::None;
}

// What the machine drives. The real one is the Media Foundation worker (src\user_playlist.h); the host test has a fake.
// Calls must not block on the machine (the real one only posts to its thread).
struct PlayerIf {
  virtual ~PlayerIf() = default;
  virtual bool Start(const std::filesystem::path& file, uint32_t token, float volume) = 0;  // false: refuses at once
  virtual void Stop() = 0;
  virtual void Pause(bool paused) = 0;
  virtual void SetVolume(float volume) = 0;
};

// Starts `file` through `player` after `map` has turned it into what is actually played (lane 283: music_slots::Playable = the converted
// copy; an empty path = refused / cannot be opened unconverted -> false, the machine moves on). The machine's list keeps the SOURCE paths.
template <class MapFn>
inline bool StartVia(PlayerIf& player, MapFn&& map, const std::filesystem::path& file, uint32_t token, float volume) {
  const std::filesystem::path play = map(file);
  if (play.empty()) return false;
  return player.Start(play, token, volume);
}

class Machine {
 public:
  using ExistsFn = std::function<bool(const std::filesystem::path&)>;
  Machine(PlayerIf& player, ExistsFn exists) : player_(player), exists_(std::move(exists)) {}

  // The scan result (full paths, in play order). A playing song is not interrupted by a new list; the next advance
  // uses the new list from its start if the current file is not in it, else after the current file.
  void SetSongs(std::vector<std::filesystem::path> songs) {
    std::lock_guard lock(m_);
    songs_ = std::move(songs);
    if (state_ != State::Idle) {
      auto it = std::find(songs_.begin(), songs_.end(), current_);
      index_ = it == songs_.end() ? size_t(-1) : size_t(it - songs_.begin());
    }
  }
  std::vector<std::filesystem::path> Songs() const {
    std::lock_guard lock(m_);
    return songs_;
  }
  size_t SongCount() const {
    std::lock_guard lock(m_);
    return songs_.size();
  }

  // 0 = started, 1 = nothing playable (state stays Idle).
  // `first` = the song to start with (index into the list); `single` = play only that one song, then Idle (an
  // entrance plays the wrestler's chosen song, not the folder). A first song that cannot play falls on to the next ones
  // unless single (then 1: the caller does what it did before).
  uint32_t Play(size_t first = 0, bool single = false) {
    std::lock_guard lock(m_);
    return PlayLocked(first, single);
  }
  // The same by FILE (review S6: the picker's list and the machine's list can shift between two calls, an index cannot be trusted):
  // 1 = the file is not in the current list.
  uint32_t PlayPath(const std::filesystem::path& file, bool single) {
    std::lock_guard lock(m_);
    const auto it = std::find(songs_.begin(), songs_.end(), file);
    if (it == songs_.end()) return 1;
    return PlayLocked(size_t(it - songs_.begin()), single);
  }
  uint32_t PlayLocked(size_t first, bool single) {
    if (state_ != State::Idle) player_.Stop();
    state_ = State::Idle;
    single_ = single;
    if (single) {
      if (first >= songs_.size() || !exists_(songs_[first])) return 1;
      ++token_;
      if (!player_.Start(songs_[first], token_, volume_)) return 1;
      index_ = first, current_ = songs_[first], state_ = State::Playing;
      return 0;
    }
    return StartFrom(first) ? 0 : 1;
  }
  void Stop() {
    std::lock_guard lock(m_);
    if (state_ == State::Idle) return;
    ++token_;
    player_.Stop();
    state_ = State::Idle;
  }
  bool Pause() {
    std::lock_guard lock(m_);
    if (state_ != State::Playing) return false;
    state_ = State::Paused;
    player_.Pause(true);
    return true;
  }
  bool Continue() {
    std::lock_guard lock(m_);
    if (state_ != State::Paused) return false;
    state_ = State::Playing;
    player_.Pause(false);
    return true;
  }
  // The player says the song of `token` ended (ok) or could not be played (!ok): go on with the next one.
  void OnSongEnded(uint32_t token, bool ok = true) {
    std::lock_guard lock(m_);
    if (token != token_ || state_ == State::Idle) return;  // a song that was stopped or replaced
    (void)ok;
    if (single_) {
      state_ = State::Idle;
      return;
    }
    const size_t next = index_ == size_t(-1) ? 0 : index_ + 1;
    if (!StartFrom(next)) state_ = State::Idle;
  }
  void SetVolume(float v) {
    std::lock_guard lock(m_);
    volume_ = std::clamp(v, 0.0f, 1.0f);
    if (state_ != State::Idle) player_.SetVolume(volume_);
  }
  State state() const {
    std::lock_guard lock(m_);
    return state_;
  }
  uint32_t token() const {
    std::lock_guard lock(m_);
    return token_;
  }
  float volume() const {
    std::lock_guard lock(m_);
    return volume_;
  }
  std::filesystem::path current() const {
    std::lock_guard lock(m_);
    return state_ == State::Idle ? std::filesystem::path{} : current_;
  }

 private:
  // Starts the first playable song at or after `from`. Called under the lock. False (Idle) when none is left.
  bool StartFrom(size_t from) {
    for (size_t i = from; i < songs_.size(); ++i) {
      if (!exists_(songs_[i])) continue;  // removed since the scan
      ++token_;
      if (!player_.Start(songs_[i], token_, volume_)) continue;
      index_ = i;
      current_ = songs_[i];
      state_ = State::Playing;
      return true;
    }
    state_ = State::Idle;
    return false;
  }

  PlayerIf& player_;
  ExistsFn exists_;
  mutable std::mutex m_;
  std::vector<std::filesystem::path> songs_;
  size_t index_ = size_t(-1);
  std::filesystem::path current_;
  State state_ = State::Idle;
  bool single_ = false;
  uint32_t token_ = 0;
  float volume_ = 1.0f;
};

}  // namespace wwe2k14::user_playlist
