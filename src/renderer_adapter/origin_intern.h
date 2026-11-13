// Draw origins (Session 101, arc A1a; docs\renderer\NAMES.md "Draw origin"): the host table behind the 32-bit tag a draw record carries.
// The game's draw recorders (op 15 sub_82BB10C0, op 19 sub_82C1B350, op 68 sub_82BCBDD8) copy a 32-byte record header into the command
// list and never write its word at +12; the replay handlers never read it (listings read S101, docs\research\s101\A0-1.md). Our recorder
// hooks put a tag there for the copy only (the caller's word is restored after), and the replay dispatch hook reads it back. The tag is an
// id into this table plus 8 check bits: a record written by code we do not hook carries whatever was in its +12, and the check bits turn
// that into "unnamed" instead of a wrong name (a garbage word passes with odds ~ table size / 2^24 / 256).
// No game or SDK includes: draw_origin.h (the hooks) and reader.h (the frame) share it.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace rdr_adapter::origin {

struct Key {
  uint32_t call_site = 0;  // the recorder's caller (lr - 4): the submit function
  uint32_t pass_site = 0;  // for a model part: the call of sub_82BDED40 (lr - 4), i.e. the scene pass; 0 = not inside one
  uint32_t object = 0;     // sub_82BDED40's r3 (the model object); 0 = none
  uint32_t part = 0;       // sub_82BDED40's r4 (the part index)
  uint32_t op = 0;         // the record's opcode (15, 19, 68)
  uint32_t widget = 0;     // S101 lane18: 1-based id into Widgets() (the UI element + screen this 2D draw belongs to); 0 = none
  uint32_t text = 0;       // S101 U1: 1 = recorded inside a text draw (sub_82BD06B8) by a text-object function: a glyph quad (class text)
  uint32_t run = 0;        // S101 U1: 1-based id into Runs() (the whole string of that text draw; labels on only); 0 = none
  bool operator==(const Key& o) const {
    return call_site == o.call_site && pass_site == o.pass_site && object == o.object && part == o.part && op == o.op && widget == o.widget &&
           text == o.text && run == o.run;
  }
};

struct KeyHash {
  size_t operator()(const Key& k) const {
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (uint32_t v : {k.call_site, k.pass_site, k.object, k.part, k.op, k.widget, k.text, k.run}) h = (h ^ v) * 0x100000001B3ull;
    return size_t(h ^ (h >> 29));
  }
};

inline constexpr uint32_t kMaxIds = 0xFFFFFF;  // 24-bit ids; id 0 = none

inline uint32_t CheckBits(uint32_t id) { return ((id * 0x2545F491u) >> 24) ^ 0xA7u; }
inline uint32_t MakeTag(uint32_t id) { return id ? (id & kMaxIds) | (CheckBits(id & kMaxIds) << 24) : 0; }

// On only while the reader records or draws live (reader.h sets it once a frame from Recording()): with nothing reading the tags, the
// recorder hooks write none (S101 review 1: players must not pay for names nobody reads). The first frame after a start is unnamed.
inline std::atomic<bool> g_on{false};

class Table {
 public:
  // Writer side (game threads recording draws). Never fails: past kMaxIds the tag is 0 (unnamed) and counted.
  // A thread-local front cache answers steady frames with no lock (S101 review 4).
  uint32_t Intern(const Key& k) {
    thread_local std::unordered_map<Key, uint32_t, KeyHash> front;
    if (auto it = front.find(k); it != front.end()) return it->second;
    const uint32_t tag = InternLocked(k);
    if (tag && front.size() < 65536) front.emplace(k, tag);
    return tag;
  }
  uint32_t InternLocked(const Key& k) {
    std::lock_guard lock(mutex_);
    if (auto it = ids_.find(k); it != ids_.end()) return MakeTag(it->second);
    if (keys_.size() >= kMaxIds) { ++overflow_; return 0; }
    keys_.push_back(k);
    const uint32_t id = uint32_t(keys_.size());  // 1-based
    ids_.emplace(k, id);
    return MakeTag(id);
  }
  // Reader side. False: not one of our tags (garbage at +12, or 0).
  bool Resolve(uint32_t tag, Key& out) const {
    const uint32_t id = tag & kMaxIds;
    if (!id || (tag >> 24) != CheckBits(id)) return false;
    thread_local std::unordered_map<uint32_t, Key> front;  // ids are never reused: a cached answer stays right
    if (auto it = front.find(tag); it != front.end()) {
      out = it->second;
      return true;
    }
    {
      std::lock_guard lock(mutex_);
      if (id > keys_.size()) return false;
      out = keys_[id - 1];
    }
    if (front.size() < 65536) front.emplace(tag, out);
    return true;
  }
  size_t size() const { std::lock_guard lock(mutex_); return keys_.size(); }
  uint64_t overflow() const { std::lock_guard lock(mutex_); return overflow_; }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<Key, uint32_t, KeyHash> ids_;
  std::vector<Key> keys_;
  uint64_t overflow_ = 0;
};

inline Table& Global() {
  static Table* t = new Table;  // never destroyed: game threads may still record while static destructors run at exit (S101 review 6)
  return *t;
}

// S101 lane18 (docs/renderer/NAMES.md "2D draws"): the UI widget a 2D draw belongs to, and the screen the game was on.
// Registry (written by the set-text hook, game thread): text element -> {its vtable word, parent layout node, string word, text id}. Widget
// infos are interned like keys, each with its text built ONCE at intern time (review 1: FrameString keys by pointer, so the text must live
// at a stable address, never in a per-draw buffer).
struct WidgetInfo {
  uint32_t element = 0;    // the text element (set-text r3); 0 = no element known (screen only)
  uint32_t node = 0;       // the element's parent layout node ([element+16], menu_ui.md; [U] its meaning beyond "the node it is filed under")
  uint32_t text_id = 0;    // the string id the element was last given (set-text r4); 0 = none known
  uint32_t seq = 0xFFFFFFFFu;   // sequence id ([[0x839400F8]+0]+48), -1 = unreadable
  uint32_t menu = 0xFFFFFFFFu;  // SmartMenu menu id ([0x8392945C]+364) while seq == 75, else -1
  char label[40] = {};          // the text the open text-object draw shows (sub_82BD06B8's r4, UTF-16BE, filtered to printable ASCII); empty = none
  bool operator==(const WidgetInfo& o) const {
    return element == o.element && node == o.node && text_id == o.text_id && seq == o.seq && menu == o.menu && std::memcmp(label, o.label, sizeof(label)) == 0;
  }
};
struct WidgetHash {
  size_t operator()(const WidgetInfo& w) const {
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (uint32_t v : {w.element, w.node, w.text_id, w.seq, w.menu}) h = (h ^ v) * 0x100000001B3ull;
    for (char c : w.label) h = (h ^ uint8_t(c)) * 0x100000001B3ull;
    return size_t(h ^ (h >> 29));
  }
};
inline std::string FormatWidget(const WidgetInfo& w) {
  char buf[240];
  int len = std::snprintf(buf, sizeof(buf), "screen seq=%d menu=%d", int(w.seq), int(w.menu));
  if (w.element) len += std::snprintf(buf + len, sizeof(buf) - size_t(len), "; element 0x%08X node 0x%08X text %u", w.element, w.node, w.text_id);
  if (w.label[0]) std::snprintf(buf + len, sizeof(buf) - size_t(len), "; label \"%s\"", w.label);
  return buf;
}
class WidgetTable {
 public:
  struct Element {
    uint32_t vtable = 0, node = 0, str = 0, text_id = 0;
  };
  static constexpr uint32_t kMaxWidgets = 1u << 20;
  // Game thread, at every set-text (after the game stored the string word at element+1136).
  void NoteText(uint32_t element, uint32_t vtable, uint32_t node, uint32_t str, uint32_t text_id) {
    std::lock_guard lock(mutex_);
    if (elements_.size() >= 16384) elements_.clear();  // bounded: a cleared entry only means "no text id" until its next set-text
    elements_[element] = {vtable, node, str, text_id};
  }
  // The registry entry for `element` if its LIVE vtable word, parent node and string word all still match what set-text saw (review 3: a freed
  // element's address reused by a new element must not take the dead one's text).
  bool FindElement(uint32_t element, uint32_t vtable_now, uint32_t node_now, uint32_t str_now, Element& out) const {
    std::lock_guard lock(mutex_);
    auto it = elements_.find(element);
    if (it == elements_.end() || it->second.vtable != vtable_now || it->second.node != node_now || it->second.str != str_now) return false;
    out = it->second;
    return true;
  }
  // 1-based id. A thread-local cache answers repeats with no lock (review 2). When the table is full the draw falls back to the
  // screen-only entry (review 4); 0 only if even that cannot be made.
  uint32_t Intern(const WidgetInfo& w) {
    thread_local std::unordered_map<WidgetInfo, uint32_t, WidgetHash> front;
    if (auto it = front.find(w); it != front.end()) return it->second;
    uint32_t id = InternLocked(w);
    if (!id && w.label[0]) {  // label cap or table full (review 4): keep the element, drop the label
      WidgetInfo nolabel = w;
      nolabel.label[0] = 0;
      id = InternLocked(nolabel);
    }
    if (!id && (w.element || w.label[0])) {  // review 5: then the screen alone (element AND label cleared)
      WidgetInfo screen = w;
      screen.element = screen.node = screen.text_id = 0;
      screen.label[0] = 0;
      id = InternLocked(screen);
    }
    if (id && front.size() < 65536) front.emplace(w, id);
    return id;
  }
  // The info and its text (a stable pointer, valid for the process lifetime).
  const char* Get(uint32_t id, WidgetInfo& out) const {
    std::lock_guard lock(mutex_);
    if (!id || id > infos_.size()) return nullptr;
    out = infos_[id - 1];
    return texts_[id - 1].c_str();
  }
  size_t size() const { std::lock_guard lock(mutex_); return infos_.size(); }
  uint64_t overflow() const { std::lock_guard lock(mutex_); return overflow_; }
  uint64_t label_overflow() const { std::lock_guard lock(mutex_); return label_overflow_; }
  size_t labels() const { std::lock_guard lock(mutex_); return label_count_; }
  static constexpr size_t kMaxLabelWidgets = 4096;  // review 4: dynamic strings (a clock) must not grow the table for the whole session

 private:
  uint32_t InternLocked(const WidgetInfo& w) {
    std::lock_guard lock(mutex_);
    if (auto it = ids_.find(w); it != ids_.end()) return it->second;
    if (infos_.size() >= kMaxWidgets) { ++overflow_; return 0; }
    if (w.label[0]) {
      if (label_count_ >= kMaxLabelWidgets) { ++label_overflow_; return 0; }
      ++label_count_;
    }
    infos_.push_back(w);
    texts_.push_back(FormatWidget(w));  // a deque: existing strings never move
    const uint32_t id = uint32_t(infos_.size());
    ids_.emplace(w, id);
    return id;
  }
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, Element> elements_;
  std::unordered_map<WidgetInfo, uint32_t, WidgetHash> ids_;
  std::vector<WidgetInfo> infos_;
  std::deque<std::string> texts_;
  uint64_t overflow_ = 0, label_overflow_ = 0;
  size_t label_count_ = 0;
};
// ---- labels: the text a text draw shows (review of a83e727d) ------------------------------------------------------------------------
// sub_82BD06B8 (the text object's draw, r3 = object, r4 = the string) calls wcslen (sub_82C7C5C8) on r4: the string is UTF-16 big-endian. The
// hook in draw_origin.h filters its code units with FilterLabel and keeps the result in t_label for the duration of that call; a 2D scope opened
// inside it (sub_82BC3518 / sub_82BC8180) takes it. The label therefore exists only inside a real text draw on this thread.
constexpr size_t kLabelUnitsRun = 512;  // S101 U1: code units read for the whole run (= rdr_adapter::kRunUnits, names.h)
constexpr size_t kLabelUnits = 40;  // code units read at most (the label is at most 39 chars + NUL)
// Pure filter: `units` = n code units (host order). False (no label) when the string starts with NUL, or has any unit that is not 0x20-0x7E, or is a
// quote (the widget text quotes it). A string of 39 or more units is cut to 36 chars + "..." (marked truncated, never silently).
inline bool FilterLabel(const uint16_t* units, size_t n, char (&out)[40]) {
  size_t len = 0;
  for (; len < n; ++len) {
    const uint16_t u = units[len];
    if (u == 0) break;
    if (u < 0x20 || u > 0x7E || u == '"') return false;
  }
  if (len == 0) return false;
  if (len >= sizeof(out) - 1) {
    for (size_t i = 0; i < sizeof(out) - 4; ++i) out[i] = char(units[i]);
    out[sizeof(out) - 4] = out[sizeof(out) - 3] = out[sizeof(out) - 2] = '.';
    out[sizeof(out) - 1] = 0;
    return true;
  }
  for (size_t i = 0; i < len; ++i) out[i] = char(units[i]);
  out[len] = 0;
  return true;
}
struct LabelText {
  char text[40] = {};
  bool valid = false;
};
inline thread_local LabelText t_label;
// RAII: the label of the text draw whose call is open on this thread; nested calls restore the outer one.
class LabelScope {
 public:
  explicit LabelScope(const uint16_t* units, size_t n) : saved_(t_label) {
    t_label.valid = FilterLabel(units, n, t_label.text);
  }
  LabelScope() : saved_(t_label) { t_label.valid = false; }
  ~LabelScope() { t_label = saved_; }
  LabelScope(const LabelScope&) = delete;
  LabelScope& operator=(const LabelScope&) = delete;

 private:
  LabelText saved_;
};
// The scope predicate: a 2D scope takes the open label only when labels are on (capture / case ring active, not the live mirror alone) and it
// is a text-object draw function.
inline const char* LabelFor(bool labels_on, bool text_object_fn) { return (labels_on && text_object_fn && t_label.valid) ? t_label.text : nullptr; }
inline std::atomic<bool> g_labels{false};  // set by the reader with the capture (reader.h), not by the live mirror
inline std::atomic<bool> g_labels_wanted{false};  // S101 lane 89 (UT): a LIVE consumer of the run strings (the ui_text hook, --wwe2k14_rdr_ui_text_ours) asks for labels without a capture ring; set once by live.cpp, read by reader.h with g_cap.active / g_prov_on

// ---- S101 U1: text runs (docs\renderer\UI_PROMOTION_MAP.md sections 1.3, 8) --------------------------------------------------------------
// The glyph quads of one text draw (sub_82BD06B8, r3 = the text object, r4 = the UTF-16BE string) are told from plain rectangles by the run that
// drew them: TextRunScope marks the call (always while origins are on, so the class is right with labels off), and while labels are on
// (capture / case ring) it also interns the WHOLE string (not the 40-char label). The string lives in Runs(), keyed by content, so a clock that
// changes every second is bounded by kMaxRuns (past it a draw keeps its class and loses its string; overflow is counted).
inline void AppendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) out.push_back(char(cp));
  else if (cp < 0x800) { out.push_back(char(0xC0 | (cp >> 6))); out.push_back(char(0x80 | (cp & 0x3F))); }
  else if (cp < 0x10000) { out.push_back(char(0xE0 | (cp >> 12))); out.push_back(char(0x80 | ((cp >> 6) & 0x3F))); out.push_back(char(0x80 | (cp & 0x3F))); }
  else { out.push_back(char(0xF0 | (cp >> 18))); out.push_back(char(0x80 | ((cp >> 12) & 0x3F))); out.push_back(char(0x80 | ((cp >> 6) & 0x3F))); out.push_back(char(0x80 | (cp & 0x3F))); }
}
// units = n code units (host order); stops at the first NUL. A valid surrogate pair is one code point; a lone surrogate is encoded as itself.
// `count` (optional) = the code points, counted in the same pass.
inline std::string UnitsToUtf8(const uint16_t* units, size_t n, uint32_t* count = nullptr) {
  std::string s;
  uint32_t c = 0;
  for (size_t i = 0; i < n && units[i]; ++i, ++c) {
    uint32_t u = units[i];
    if (u >= 0xD800 && u < 0xDC00 && i + 1 < n && units[i + 1] >= 0xDC00 && units[i + 1] < 0xE000) {
      u = 0x10000 + ((u - 0xD800) << 10) + (units[i + 1] - 0xDC00);
      ++i;
    }
    AppendUtf8(s, u);
  }
  if (count) *count = c;
  return s;
}
class TextRuns {
 public:
  static constexpr size_t kMaxRuns = 8192;
  struct Run {
    std::string utf8;
    uint32_t units = 0;
    bool truncated = false;
  };
  // 1-based id; 0 = an empty string, or the table is full (counted). The labels-on path runs this for every text draw of a captured frame: a repeat is
  // answered from a per-thread cache keyed by two independent 64-bit hashes of the units + their count (looked up BEFORE any UTF-8 is built), a new string
  // is converted once (the code points counted in the same pass). Collision odds are ~2^-128 per pair of strings.
  uint32_t Intern(const uint16_t* units, size_t n, bool truncated) {
    size_t len = 0;
    uint64_t h1 = 0xCBF29CE484222325ull, h2 = 0x9E3779B97F4A7C15ull;
    for (; len < n && units[len]; ++len) {
      h1 = (h1 ^ units[len]) * 0x100000001B3ull;
      h2 = (h2 + units[len] + 1) * 0xFF51AFD7ED558CCDull, h2 ^= h2 >> 31;
    }
    if (!len) return 0;
    const HashKey key{h1, h2, uint32_t(len) | (truncated ? 0x80000000u : 0u)};
    thread_local std::unordered_map<HashKey, uint32_t, HashKeyHash> front;
    if (auto it = front.find(key); it != front.end()) return it->second;
    uint32_t count = 0;
    std::string s = UnitsToUtf8(units, len, &count);
    uint32_t id = 0;
    {
      std::lock_guard lock(mutex_);
      if (auto it = ids_.find(key); it != ids_.end()) {
        id = it->second;
      } else if (runs_.size() - base_ >= kMaxRuns) {
        ++overflow_;
      } else {
        runs_.push_back({std::move(s), count, truncated});  // a deque: existing strings never move
        id = uint32_t(runs_.size());
        ids_.emplace(key, id);
      }
    }
    if (id && front.size() < 65536) front.emplace(key, id);
    return id;
  }
  const Run* Get(uint32_t id) const {
    std::lock_guard lock(mutex_);
    return (id && id <= runs_.size()) ? &runs_[id - 1] : nullptr;
  }
  // A capture starts: the cap and the overflow count begin again. Strings already handed out stay (their ids sit in the draw-origin keys and the
  // per-thread caches; an id is never reused), they just stop counting against the new capture's room.
  void Clear() {
    std::lock_guard lock(mutex_);
    base_ = runs_.size();
    overflow_ = 0;
  }
  size_t size() const { std::lock_guard lock(mutex_); return runs_.size() - base_; }
  uint64_t overflow() const { std::lock_guard lock(mutex_); return overflow_; }

 private:
  struct HashKey {
    uint64_t h1, h2;
    uint32_t len;  // code units (before the NUL) | 0x80000000 when truncated
    bool operator==(const HashKey& o) const { return h1 == o.h1 && h2 == o.h2 && len == o.len; }
  };
  struct HashKeyHash {
    size_t operator()(const HashKey& k) const { return size_t(k.h1 ^ (k.h2 << 1) ^ k.len); }
  };
  mutable std::mutex mutex_;
  std::deque<Run> runs_;
  std::unordered_map<HashKey, uint32_t, HashKeyHash> ids_;
  size_t base_ = 0;
  uint64_t overflow_ = 0;
};
inline TextRuns& Runs() {
  static TextRuns* t = new TextRuns;  // never destroyed (same reason as Global())
  return *t;
}
struct RunState {
  bool open = false;   // inside a text draw's call on this thread
  uint32_t run = 0;    // Runs() id of its whole string; 0 = none (labels off, empty, or the table is full)
};
inline thread_local RunState t_run;
// RAII: the text draw whose call is open on this thread; nested calls restore the outer one. `read_string` = labels on: intern the string.
class TextRunScope {
 public:
  TextRunScope(bool read_string, const uint16_t* units, size_t n, bool truncated) : saved_(t_run) {
    t_run.open = true;
    t_run.run = (read_string && units && n) ? Runs().Intern(units, n, truncated) : 0;
  }
  ~TextRunScope() { t_run = saved_; }
  TextRunScope(const TextRunScope&) = delete;
  TextRunScope& operator=(const TextRunScope&) = delete;

 private:
  RunState saved_;
};
// What a 2D scope of a text-object function takes from the open text draw (text flag + run id); {0, 0} for any other function or no open draw.
struct RunTake {
  uint32_t text = 0, run = 0;
};
inline RunTake RunFor(bool text_object_fn) { return (text_object_fn && t_run.open) ? RunTake{1, t_run.run} : RunTake{}; }

// Slot-15 census (Check C): how often the text element's draw method ran, and how often its element was one set-text had registered.
inline std::atomic<uint64_t> g_slot15_hits{0}, g_slot15_registered{0};
inline WidgetTable& Widgets() {
  static WidgetTable* t = new WidgetTable;  // never destroyed (same reason as Global())
  return *t;
}
// The widget id for a 2D draw: the element whose draw method is open on this thread (`open`), else the screen alone.
// draw_origin.h WidgetFor and the test share this.
struct OpenElement {
  uint32_t element = 0, node = 0, text_id = 0;
};
// `label` (may be null): the string a text-object draw shows, already filtered to printable ASCII (draw_origin.h ReadLabel).
inline uint32_t WidgetIdFor(const OpenElement& open, uint32_t seq, uint32_t menu, const char* label = nullptr) {
  WidgetInfo w;
  w.seq = seq, w.menu = menu;
  if (label) std::snprintf(w.label, sizeof(w.label), "%s", label);
  if (open.element) w.element = open.element, w.node = open.node, w.text_id = open.text_id;
  return Widgets().Intern(w);
}

// Set by the record handler hooks (op 15 / 19 / 68, draw_origin.h) for the record being replayed (render thread); read by the reader when
// that record's draw arrives inside the handler.
inline thread_local uint32_t t_replay_tag = 0;
inline thread_local uint32_t t_replay_op = 0;

}  // namespace rdr_adapter::origin
