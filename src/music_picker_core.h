// wwe2k14 - USER PLAYLIST music picker: the state machine (lane 291, 2026-11-13). No game, SDK, ImGui or Windows
// includes: the host test (tools\music\music_picker_test.cpp) builds this header alone. The view/logic boundary is
// src\music_picker_view.h; the guest glue (the dialog hook, the overlay host, the pad gate, choices.txt) is
// src\music_picker.h.
//
// The picker is the 360 Guide's part of "USER PLAYLIST": the player picks a song of the Mods > Music library, the choice
// is saved under the wrestler's key (choices.txt), and the game plays "user music" as it did on the console.
//
// Rules (Jarrod, 2026-11-13):
// - Moving the cursor NEVER plays anything. A preview starts only on kTogglePlay (the row's PLAY button) and runs until it is
//   stopped, another row is played, the song ends, or the picker closes. Moving to another row leaves it playing.
// - Choosing a song (kSelect / kTapRow) stops the preview, saves nothing itself (the caller gets Result{Chosen, ...} and
//   writes choices.txt) and closes. Cancel stops the preview and closes with no change.
// - A file that vanished since the scan is dropped from the list with a message; the picker stays open.
// - An empty library shows the message and only Cancel works.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "music_picker_view.h"
#include "user_playlist_core.h"

namespace wwe2k14::music_picker {

// What previews a row. The real one drives the user-playlist machine (lane 288's shared player replaces it).
struct PreviewIf {
  virtual ~PreviewIf() = default;
  virtual bool Play(const std::filesystem::path& file) = 0;  // by path (the lists can shift); false = the song cannot be played
  virtual void Stop() = 0;
  virtual bool IsPlaying() = 0;         // false once the song ended or was stopped from outside
};

// The open / close bookkeeping of one picker (lane 292 re-review SF1). `open` = the pad is the picker's; `gen` = which open posted work belongs to.
// The UI thread can fail to run its queue for seconds (a window move loop, a stall, a minimised non-owner presenter, Android in the background):
// the game thread then lets go (Release) while the posted open and the dialog are still pending. A posted open runs only for the generation
// that posted it AND while it is still open; a frame that runs on a released picker closes the dialog and drops the view.
struct OpenLifecycle {
  std::atomic<bool> open{false};
  std::atomic<uint32_t> gen{0};
  std::atomic<uint32_t> pump_frames{0};  // frames the dialog ran since Begin (0 for 2 s = it never came)
  uint32_t Begin() {                     // game thread: a picker is opening
    pump_frames.store(0);
    const uint32_t g = gen.fetch_add(1) + 1;
    open.store(true, std::memory_order_release);
    return g;
  }
  bool PostedMayRun(uint32_t posted_gen) const { return gen.load() == posted_gen && open.load(); }
  void Release() {  // closed, force-released, or shut down: every posted piece of this open is void
    open.store(false, std::memory_order_release);
    gen.fetch_add(1);
  }
  bool Stale() const { return !open.load(std::memory_order_acquire); }  // a dialog frame that sees this must close the dialog
};

enum class Outcome : uint8_t { None, Chosen, Cancelled };
struct Result {
  Outcome outcome = Outcome::None;
  std::filesystem::path file;  // Chosen: the song's full path
};

class Picker {
 public:
  using ExistsFn = std::function<bool(const std::filesystem::path&)>;
  Picker(PreviewIf& preview, ExistsFn exists) : preview_(preview), exists_(std::move(exists)) {}

  // `songs` = full paths in list order, `current` = the file name of the saved choice ("" = none).
  void Open(std::vector<std::filesystem::path> songs, const std::string& current, std::string title, std::string subtitle,
            std::string empty_message) {
    paths_ = std::move(songs);
    empty_message_ = std::move(empty_message);
    model_ = PickerModel{};
    model_.title = std::move(title);
    model_.subtitle = std::move(subtitle);
    for (const auto& p : paths_) model_.songs.push_back({user_playlist::U8(p.stem()), {}, 0});
    model_.selected = -1;
    for (size_t i = 0; i < paths_.size(); ++i) {
      if (!current.empty() && user_playlist::Lower(user_playlist::U8(paths_[i].filename())) == user_playlist::Lower(current)) {
        model_.selected = int(i);
        model_.cursor = int(i);
        break;
      }
    }
    open_ = true;
    Refresh();
    Bump();
  }
  bool is_open() const { return open_; }
  const PickerModel& model() const { return model_; }
  const std::vector<std::filesystem::path>& paths() const { return paths_; }

  // The tags the glue read (title / artist / art id) for row `i`, any time while open.
  void SetTags(size_t i, const std::string& title, const std::string& artist, uint32_t art_id) {
    if (i >= model_.songs.size()) return;
    if (!title.empty()) model_.songs[i].name = title;
    model_.songs[i].artist = artist;
    model_.songs[i].art_id = art_id;
    Bump();
  }

  // The same for the row whose file is `file` (the tags are read off the UI thread and come back later: the list may have shifted).
  void SetTagsByPath(const std::filesystem::path& file, const std::string& title, const std::string& artist, uint32_t art_id) {
    for (size_t i = 0; i < paths_.size(); ++i)
      if (paths_[i] == file) return SetTags(i, title, artist, art_id);
  }

  // One input. Returns Chosen / Cancelled when this input closed the picker (the picker is closed then).
  Result Handle(const PickerInput& in, int page = 6) {
    if (!open_) return {};
    const int n = int(paths_.size());
    auto move = [&](int to) {
      to = n ? std::clamp(to, 0, n - 1) : 0;
      if (to != model_.cursor) model_.cursor = to, Bump();
    };
    switch (in.kind) {
      case InputKind::kUp: move(model_.cursor - 1); break;
      case InputKind::kDown: move(model_.cursor + 1); break;
      case InputKind::kPageUp: move(model_.cursor - page); break;
      case InputKind::kPageDown: move(model_.cursor + page); break;
      case InputKind::kScroll: move(model_.cursor + in.scroll_rows); break;
      case InputKind::kCancel: return Close(Outcome::Cancelled, {});
      case InputKind::kSelect:
      case InputKind::kTapRow: {
        if (!n) break;
        const int row = in.kind == InputKind::kTapRow ? in.row : model_.cursor;
        if (row < 0 || row >= n) break;
        if (!exists_(paths_[row])) {
          Drop(row);
          break;
        }
        const auto file = paths_[row];
        return Close(Outcome::Chosen, file);
      }
      case InputKind::kTogglePlay: {
        if (!n) break;
        const int row = in.row < 0 ? model_.cursor : in.row;
        if (row < 0 || row >= n) break;
        if (model_.playing == row) {
          preview_.Stop();
          model_.playing = -1;
          model_.message.clear();
          Bump();
          break;
        }
        if (!exists_(paths_[row])) {
          Drop(row);
          break;
        }
        model_.message.clear();
        if (preview_.Play(paths_[size_t(row)])) {
          model_.playing = row;
        } else {
          preview_.Stop();
          model_.playing = -1;
          model_.message = "That song could not be played.";
        }
        Bump();
        break;
      }
    }
    return {};
  }

  // Once per UI frame: a preview that ended by itself clears the playing row.
  void Tick() {
    if (open_ && model_.playing >= 0 && !preview_.IsPlaying()) {
      model_.playing = -1;
      Bump();
    }
  }

  // The host closes the picker from outside (the game left the editor, shutdown): stops the preview, no result.
  void ForceClose() {
    if (open_) Close(Outcome::Cancelled, {});
  }

 private:
  void Bump() { ++model_.revision; }
  void Refresh() {
    model_.empty = paths_.empty();
    if (model_.empty) {
      model_.message = empty_message_;
      model_.cursor = 0;
    }
  }
  Result Close(Outcome o, std::filesystem::path file) {
    if (model_.playing >= 0) preview_.Stop();
    model_.playing = -1;
    open_ = false;
    Bump();
    return {o, std::move(file)};
  }
  // A song that vanished: out of the list, with the message.
  void Drop(int row) {
    const std::string name = model_.songs[row].name;
    if (model_.playing == row) {
      preview_.Stop();
      model_.playing = -1;
    } else if (model_.playing > row) {
      --model_.playing;
    }
    paths_.erase(paths_.begin() + row);
    model_.songs.erase(model_.songs.begin() + row);
    if (model_.selected == row) model_.selected = -1;
    else if (model_.selected > row) --model_.selected;
    model_.cursor = paths_.empty() ? 0 : std::clamp(model_.cursor, 0, int(paths_.size()) - 1);
    Refresh();
    if (!model_.empty) model_.message = "\"" + name + "\" is no longer in the library.";
    Bump();
  }

  PreviewIf& preview_;
  ExistsFn exists_;
  std::vector<std::filesystem::path> paths_;
  std::string empty_message_;
  PickerModel model_;
  bool open_ = false;
};

// ---- ID3v2 tags (the title, the artist and the cover of an mp3) ----
// Only v2.3 / v2.4 (what taggers write today), text frames TIT2 / TPE1 and the first APIC. Anything unreadable = empty fields.
struct Tags {
  std::string title, artist;
  std::vector<uint8_t> art;  // the encoded image (JPEG or PNG) as stored
};

namespace id3 {
inline uint32_t Syncsafe(const uint8_t* p) { return uint32_t(p[0] & 0x7F) << 21 | uint32_t(p[1] & 0x7F) << 14 | uint32_t(p[2] & 0x7F) << 7 | (p[3] & 0x7F); }
inline uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

inline void AppendUtf8(std::string& s, uint32_t cp) {
  if (cp < 0x80) s += char(cp);
  else if (cp < 0x800) s += char(0xC0 | cp >> 6), s += char(0x80 | (cp & 0x3F));
  else if (cp < 0x10000) s += char(0xE0 | cp >> 12), s += char(0x80 | ((cp >> 6) & 0x3F)), s += char(0x80 | (cp & 0x3F));
  else s += char(0xF0 | cp >> 18), s += char(0x80 | ((cp >> 12) & 0x3F)), s += char(0x80 | ((cp >> 6) & 0x3F)), s += char(0x80 | (cp & 0x3F));
}

// A text field of `n` bytes in the ID3 encoding `enc` (0 latin1, 1 UTF-16 with BOM, 2 UTF-16BE, 3 UTF-8), as UTF-8.
inline std::string Text(const uint8_t* p, size_t n, int enc) {
  std::string out;
  if (enc == 0 || enc == 3) {
    for (size_t i = 0; i < n && p[i]; ++i) {
      if (enc == 3) out += char(p[i]);
      else AppendUtf8(out, p[i]);
    }
    return out;
  }
  bool be = enc == 2;
  size_t i = 0;
  if (enc == 1 && n >= 2) {
    if (p[0] == 0xFE && p[1] == 0xFF) be = true, i = 2;
    else if (p[0] == 0xFF && p[1] == 0xFE) be = false, i = 2;
  }
  for (; i + 1 < n; i += 2) {
    uint32_t u = be ? uint32_t(p[i]) << 8 | p[i + 1] : uint32_t(p[i + 1]) << 8 | p[i];
    if (!u) break;
    if (u >= 0xD800 && u < 0xDC00 && i + 3 < n) {
      uint32_t lo = be ? uint32_t(p[i + 2]) << 8 | p[i + 3] : uint32_t(p[i + 3]) << 8 | p[i + 2];
      if (lo >= 0xDC00 && lo < 0xE000) u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00), i += 2;
    }
    AppendUtf8(out, u);
  }
  return out;
}

// Offset past a null-terminated string in `enc` starting at `i`; n if unterminated.
inline size_t SkipString(const uint8_t* p, size_t n, size_t i, int enc) {
  if (enc == 0 || enc == 3) {
    while (i < n && p[i]) ++i;
    return i < n ? i + 1 : n;
  }
  while (i + 1 < n && (p[i] || p[i + 1])) i += 2;
  return i + 1 < n ? i + 2 : n;
}
}  // namespace id3

// `data` = the first bytes of the file (the tag is at the start). The art is kept only up to `max_art` bytes.
inline Tags ParseId3(const uint8_t* data, size_t size, size_t max_art = 2u << 20) {
  Tags t;
  if (size < 10 || data[0] != 'I' || data[1] != 'D' || data[2] != '3') return t;
  const int ver = data[3];
  if (ver != 3 && ver != 4) return t;
  const uint8_t flags = data[5];
  size_t pos = 10;
  const size_t end = std::min(size, size_t(10) + id3::Syncsafe(data + 6));
  if (flags & 0x40) {  // extended header: size field first
    if (pos + 4 > end) return t;
    pos += ver == 4 ? id3::Syncsafe(data + pos) : id3::Be32(data + pos) + 4;
  }
  while (pos + 10 <= end) {
    const uint8_t* h = data + pos;
    if (!h[0]) break;  // padding
    const uint32_t fsize = ver == 4 ? id3::Syncsafe(h + 4) : id3::Be32(h + 4);
    const uint16_t fflags = uint16_t(h[8] << 8 | h[9]);
    pos += 10;
    if (fsize > end - pos) break;
    const uint8_t* b = data + pos;
    pos += fsize;
    const bool plain = !(ver == 4 ? (fflags & 0x000F) : (fflags & 0xC000));  // compressed / encrypted / unsynchronised: skipped
    if (!plain || fsize < 2) continue;
    const std::string id(reinterpret_cast<const char*>(h), 4);
    const int enc = b[0] <= 3 ? b[0] : 0;
    if (id == "TIT2" && t.title.empty()) t.title = id3::Text(b + 1, fsize - 1, enc);
    else if (id == "TPE1" && t.artist.empty()) t.artist = id3::Text(b + 1, fsize - 1, enc);
    else if (id == "APIC" && t.art.empty()) {
      size_t i = 1;
      while (i < fsize && b[i]) ++i;  // MIME type (latin1, null-terminated)
      ++i;
      ++i;  // picture type
      i = id3::SkipString(b, fsize, i, enc);  // description
      if (i < fsize && fsize - i <= max_art) t.art.assign(b + i, b + fsize);
    }
  }
  return t;
}

}  // namespace wwe2k14::music_picker
