// S110 SEATREL (docs/research/s110/MODELKEEP.md part 3): which pac file the game's model was BUILT from, noted where the game built it.
// The old rule (pac_answer.h, S110 PACANS) kept a table keyed by the asked NAME and updated it from EVERY `/EMD/` lookup, including the menus' probe lookups
// (an existence test with no read): in match 2 it said CH398 while the game drew the model it kept from match 1 (CH397's), so our model met the wrong skeleton.
// The rule here:
//   NoteRead   an `/EMD/` answer seen INSIDE a real read (the pac request sub_82C37E48's window, src/pac_answer_hook.h): stored per READER. A probe lookup
//              outside the window feeds nothing.
//   NoteBuild  at the end of the game's model build (sub_82720658, hook wwe2k14_model_link): the reader's note becomes the slot descriptor's. A descriptor
//              the game kept (no build) keeps its old note, which is right: it is still the old model.
//   FileOfDesc a body's file comes from its descriptor's note, and only while the descriptor's key still equals the noted key. No note / another key = the
//              caller's id rule (key / 100).
// `desc` is the descriptor's guest address ([[0x837635A4] + 8] + 192 i): one number for one slot, the same on both sides. Header-only, std only (the hook
// and the unit tests both include it). Threads: NoteRead / NoteBuild run on the game's threads, FileOfDesc on the scene thread: a mutex, the atomic
// count read first.
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

#include "pac_answer.h"

namespace rdr_scene::live::model_file_note {

using pac_answer::Answer;

namespace detail {
struct Note {
  uint32_t key = 0, file_id = 0;
  std::string node_path;
};
constexpr size_t kMaxReaders = 256;   // readers are a small pool of guest objects; a bound so a stray address stream can never grow the table
struct State {
  std::mutex mu;
  std::map<uint32_t, Note> readers;   // reader address -> the last `/EMD/` answer of a read it made
  std::map<uint32_t, Note> descs;     // descriptor address -> the note of the read that built it
  std::atomic<size_t> count{0};       // descs.size(), the fast path
  void (*log)(const std::string&) = nullptr;
};
inline State& S() {
  static State s;
  return s;
}
inline std::string PathOf(std::string_view node_path48) {
  size_t n = 0;
  while (n < node_path48.size() && node_path48[n] != '\0') ++n;
  return std::string(node_path48.substr(0, n));
}
inline uint32_t& CurrentSlot() {
  static thread_local uint32_t reader = 0;
  return reader;
}
}  // namespace detail

// The game side supplies the log sink (once); nullptr = silent.
inline void SetLog(void (*fn)(const std::string&)) { detail::S().log = fn; }

// The reader whose real read this thread is inside (0 = none: a lookup now is a probe). Set around sub_82C37E48 by ReaderScope.
inline uint32_t CurrentReader() { return detail::CurrentSlot(); }
struct ReaderScope {
  uint32_t prev;
  explicit ReaderScope(uint32_t reader) : prev(detail::CurrentSlot()) { detail::CurrentSlot() = reader; }
  ~ReaderScope() { detail::CurrentSlot() = prev; }
  ReaderScope(const ReaderScope&) = delete;
  ReaderScope& operator=(const ReaderScope&) = delete;
};

// An `/EMD/` lookup made inside reader `reader`'s read: the asked name and the matched node's 48 path bytes. True when it was a model answer (stored).
inline bool NoteRead(uint32_t reader, std::string_view asked, std::string_view node_path48) {
  uint32_t key = 0, fid = 0;
  std::string name;
  if (!reader || !pac_answer::ParseAsked(asked, key, name) || !pac_answer::ParseNodeFileId(node_path48, fid)) return false;
  auto& s = detail::S();
  std::lock_guard<std::mutex> l(s.mu);
  if (s.readers.size() >= detail::kMaxReaders && s.readers.find(reader) == s.readers.end()) s.readers.clear();
  s.readers[reader] = detail::Note{key, fid, detail::PathOf(node_path48)};
  return true;
}

// The game built descriptor `desc` (model key `key`) from the read of `source_reader`: its note becomes the descriptor's. A note whose asked key is not the
// descriptor's key (a reader that asked for something else) is not taken, and the descriptor's old note is dropped: the build replaced the model.
// True when the descriptor's note changed (one log line per change).
inline bool NoteBuild(uint32_t desc, uint32_t key, uint32_t source_reader) {
  if (!desc) return false;
  auto& s = detail::S();
  std::string line;
  void (*log)(const std::string&) = nullptr;
  bool changed = false;
  {
    std::lock_guard<std::mutex> l(s.mu);
    const auto r = s.readers.find(source_reader);
    const auto d = s.descs.find(desc);
    if (r != s.readers.end() && r->second.key == key) {
      // S110 LIBSIZE: every library file is CH497, so the id alone cannot tell two characters apart: the node path (the KEY folder) must be equal too
      const bool same = d != s.descs.end() && d->second.key == key && d->second.file_id == r->second.file_id && d->second.node_path == r->second.node_path;
      if (!same) {
        changed = true;
        line = "[wwe2k14][pac_answer] descriptor " + std::to_string(desc) + " key " + std::to_string(key) + " built from " + r->second.node_path + " (file key " +
               std::to_string(r->second.file_id * 100 + key % 100) + ")";
        s.descs[desc] = r->second;
      }
    } else if (d != s.descs.end()) {
      changed = true;
      line = "[wwe2k14][pac_answer] descriptor " + std::to_string(desc) + " key " + std::to_string(key) + " built with no model-file answer (the id rule)";
      s.descs.erase(d);
    }
    s.count.store(s.descs.size(), std::memory_order_release);
    log = s.log;
  }
  if (changed && log) log(line);
  return changed;
}

// The file the game's model in descriptor `desc` was built from, while `key` is still the key it was built for. The usual case (no note) is one atomic load.
inline bool FileOfDesc(uint32_t desc, uint32_t key, Answer& out) {
  auto& s = detail::S();
  if (s.count.load(std::memory_order_acquire) == 0) return false;
  std::lock_guard<std::mutex> l(s.mu);
  const auto d = s.descs.find(desc);
  if (d == s.descs.end() || d->second.key != key) return false;
  out.file_id = d->second.file_id;
  out.file_key = d->second.file_id * 100 + key % 100;
  out.lib_rel = pac_answer::LibRel(d->second.node_path);
  return true;
}

// The id of the file that holds `key`'s model: the descriptor's note, else key / 100.
inline uint32_t FileIdOf(uint32_t desc, uint32_t key) {
  Answer a;
  return FileOfDesc(desc, key, a) ? a.file_id : key / 100;
}

inline void Clear() {   // tests
  auto& s = detail::S();
  std::lock_guard<std::mutex> l(s.mu);
  s.readers.clear();
  s.descs.clear();
  s.count.store(0, std::memory_order_release);
}

}  // namespace rdr_scene::live::model_file_note
