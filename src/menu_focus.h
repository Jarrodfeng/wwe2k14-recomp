#pragma once
// wwe2k14 - Session 108 lane MENU: the menu cursor as a route state (docs/research/s108/MENU_plan.md).
//
// Every menu we have read keeps its cursor in the game's shared input object (the "mixin", base vtable 0x8200F5E8,
// dispatch sub_823BC8C0; src/focus_widget.h hooks that dispatch every frame). This header is the pure part: what the
// hook records per mixin, which recorded mixin is "the focused one", and the value of each built-in route state
// (menu_ready, menu_page, menu_row, menu_rows, menu_col, menu_cols, menu_count). Std only, so a unit can drive it on
// planted words (tools/input_scripts/menu_focus_unit.cpp); the words are read through a callable `rd(guest address)`.
//
// The pick rule (plan section 4.1), on the mixins that dispatched in the newest recorded frame still live (this or the
// previous guest frame):
//   1. none live -> no focus (menu_ready 0, the rest unreadable); one -> that one;
//   2. several -> those with a cursor (counts > 0, an owner family, or cursor words that ever changed); one left -> it;
//   3. still several (or none with a cursor: then all of them) -> the one whose cursor words (row, column, owner
//      cursor) changed most recently (the hook stamps change_frame); a tie on that stamp is NOT resolved: no focus,
//      the count of tied mixins is reported (ambiguous), menu_count > 1 with menu_page unreadable shows it to the route
//      (menu_ready stays 1: the tied mixins passed the input gates, so a press is taken).
// Grounds: the generic move sub_823BC968 moves every dispatched mixin on the same pad mask, so two live cursors on one
// pad would visibly move together; a container page keeps counts 0 and never changes its words, so the stamp drops it.
// Owner families keep the cursor outside the mixin: the SmartMenu (mixin at SM+48, vtable word 0x82037940, row SM+352)
// and the UI list widget (vtable 0x820660A8, row list+88, count list+612, its mixin standalone at [list+924]).

#include <array>
#include <cstddef>
#include <cstdint>

namespace wwe2k14::menu_focus {

enum Field : uint32_t { kNone = 0, kReady, kPage, kRow, kRows, kCol, kCols, kCount };

struct HostName {
  const char* name;
  Field field;
  bool is_signed;
  const char* meaning;
};
// The built-in route states. A states.toml entry with one of these names is refused (src/game_state.h).
inline constexpr HostName kHostNames[] = {
    {"menu_ready", kReady, false, "1 when a menu mixin dispatched (passed the input gates) in the last guest frame, else 0; 1 on a tie too"},
    {"menu_page", kPage, false, "the focused mixin's vtable word (names the page class)"},
    {"menu_row", kRow, true, "the focused menu's row cursor (SmartMenu SM+352, UI list [list+88], else mixin +20)"},
    {"menu_rows", kRows, true, "the focused menu's row count (mixin +24; UI list [list+612]; unreadable on the SmartMenu)"},
    {"menu_col", kCol, true, "the focused mixin's column (+36; 0 for an owner family)"},
    {"menu_cols", kCols, true, "the focused mixin's column count (+40; 1 for an owner family)"},
    {"menu_count", kCount, false, "how many mixins dispatched in the newest live frame (1 = unambiguous; diagnosis)"},
};

inline constexpr uint32_t kSmartMenuVtable = 0x82037940u;  // [mixin] when the mixin is the SmartMenu's (at SM+48)
inline constexpr uint32_t kListVtable = 0x820660A8u;       // [list] of a UI list widget
inline constexpr uint32_t kHistGap = 4;                    // frames a mixin may skip before its history restarts

enum class Owner : uint8_t { kNone, kSmartMenu, kUiList };

struct MixinRec {
  uint32_t m = 0, vtable = 0, pads = 0, moved = 0;
  int32_t row = 0, rows = 0, col = 0, cols = 0;
  uint64_t frame = 0;         // last dispatch
  uint64_t first_frame = 0;   // first dispatch of this run of dispatches
  uint64_t change_frame = 0;  // last frame a cursor word (row, column, owner cursor) differed from the frame before
  Owner owner_kind = Owner::kNone;
  uint32_t owner = 0;
  int32_t owner_cursor = 0;
};

struct ListNote {
  uint32_t list = 0, mixin = 0;
  uint64_t frame = 0;
};

struct Pick {
  const MixinRec* rec = nullptr;
  uint32_t newest = 0;     // mixins live in the newest recorded frame (menu_count)
  uint32_t tied = 0;       // > 1: that many mixins tie on the change stamp: ambiguous, rec stays null
};

inline bool HasCursor(const MixinRec& r) {
  return r.rows > 0 || r.cols > 0 || r.owner_kind != Owner::kNone || r.change_frame > r.first_frame;
}

class Tracker {
 public:
  static constexpr size_t kRecs = 32, kHists = 64, kLists = 16;

  // The hook's record: mixin m dispatched in guest frame `frame` (call after the move, the words are final).
  template <class Rd>
  void Note(uint32_t m, uint64_t frame, Rd rd) {
    size_t keep = 0;  // keep this frame's and the previous frame's entries; drop older ones and this mixin's old one
    for (size_t i = 0; i < count_; ++i)
      if (recs_[i].frame + 1 >= frame && recs_[i].m != m) recs_[keep++] = recs_[i];
    count_ = keep;
    if (count_ == recs_.size()) return;
    MixinRec e;
    e.m = m, e.frame = frame, e.vtable = rd(m), e.pads = rd(m + 8);
    e.row = int32_t(rd(m + 20)), e.rows = int32_t(rd(m + 24)), e.col = int32_t(rd(m + 36)),
    e.cols = int32_t(rd(m + 40)), e.moved = rd(m + 80);
    ResolveOwner(e, frame, rd);
    if (e.owner_kind != Owner::kNone) e.owner_cursor = int32_t(rd(OwnerRowAddr(e)));
    Stamp(e, frame);
    recs_[count_++] = e;
  }

  void NoteList(uint32_t list, uint32_t mixin, uint64_t frame) {
    if (!list) return;
    ListNote* slot = &lists_[0];
    for (ListNote& n : lists_) {
      if (n.list == list) {
        slot = &n;
        break;
      }
      if (n.frame < slot->frame) slot = &n;  // else the oldest note
    }
    *slot = {list, mixin, frame};
  }

  // A UI list noted this or the previous frame for this mixin (its list keeps the cursor).
  template <class Rd>
  void ResolveOwner(MixinRec& e, uint64_t now, Rd rd) const {
    e.owner_kind = Owner::kNone, e.owner = 0;
    if (rd(e.m) == kSmartMenuVtable) {
      e.owner_kind = Owner::kSmartMenu, e.owner = e.m - 48;
      return;
    }
    for (const ListNote& n : lists_)
      if (n.list && n.mixin == e.m && n.frame + 1 >= now && rd(n.list) == kListVtable) {
        e.owner_kind = Owner::kUiList, e.owner = n.list;
        return;
      }
  }

  static uint32_t OwnerRowAddr(const MixinRec& e) { return e.owner + (e.owner_kind == Owner::kSmartMenu ? 352 : 88); }

  Pick Choose(uint64_t now) const {
    Pick p;
    uint64_t newest = 0;
    bool any = false;
    for (size_t i = 0; i < count_; ++i)
      if (recs_[i].frame + 1 >= now && (!any || recs_[i].frame > newest)) newest = recs_[i].frame, any = true;
    if (!any) return p;
    const MixinRec* cand[kRecs];
    size_t n = 0;
    for (size_t i = 0; i < count_; ++i)
      if (recs_[i].frame == newest) cand[n++] = &recs_[i];
    p.newest = uint32_t(n);
    if (n == 1) {
      p.rec = cand[0];
      return p;
    }
    const MixinRec* cur[kRecs];
    size_t c = 0;
    for (size_t i = 0; i < n; ++i)
      if (HasCursor(*cand[i])) cur[c++] = cand[i];
    const MixinRec** set = c ? cur : cand;
    const size_t sn = c ? c : n;
    if (sn == 1) {
      p.rec = set[0];
      return p;
    }
    uint64_t best = 0;
    for (size_t i = 0; i < sn; ++i) best = set[i]->change_frame > best ? set[i]->change_frame : best;
    const MixinRec* hit = nullptr;
    uint32_t ties = 0;
    for (size_t i = 0; i < sn; ++i)
      if (set[i]->change_frame == best) hit = set[i], ++ties;
    if (ties == 1) {
      p.rec = hit;
      return p;
    }
    p.tied = ties;
    return p;
  }

  // The value of a built-in state now. False = unreadable (no focus, ambiguous, or the field does not exist there).
  template <class Rd>
  bool Read(Field field, uint64_t now, Rd rd, uint32_t* out) const {
    const Pick p = Choose(now);
    if (field == kCount) return *out = p.newest, true;
    // ready = a mixin passed the input gates in the newest live frame (the dispatch runs only past them, sub_823BD358
    // +418): a press now is taken. A tie is ready too (the plan's menu_ready); only the cursor states are unreadable.
    if (field == kReady) return *out = p.newest > 0 ? 1u : 0u, true;
    if (!p.rec) return false;
    MixinRec e = *p.rec;
    ResolveOwner(e, now, rd);  // live: a list noted after the dispatch still names its owner
    const uint32_t m = e.m;
    switch (field) {
      case kPage: *out = rd(m); return true;
      case kRow:
        *out = e.owner_kind == Owner::kNone ? rd(m + 20) : rd(OwnerRowAddr(e));
        return true;
      case kRows:
        if (e.owner_kind == Owner::kSmartMenu) return false;
        *out = e.owner_kind == Owner::kUiList ? rd(e.owner + 612) : rd(m + 24);
        return true;
      case kCol: *out = e.owner_kind == Owner::kNone ? rd(m + 36) : 0u; return true;
      case kCols: *out = e.owner_kind == Owner::kNone ? rd(m + 40) : 1u; return true;
      default: return false;
    }
  }

  size_t size() const { return count_; }
  const MixinRec& at(size_t i) const { return recs_[i]; }

 private:
  struct Hist {
    uint32_t m = 0, vtable = 0;
    int32_t row = 0, col = 0, owner_cursor = 0;
    uint64_t first_frame = 0, change_frame = 0, last_frame = 0;
    bool used = false;
  };

  void Stamp(MixinRec& e, uint64_t frame) {
    Hist* slot = nullptr;
    Hist* oldest = &hists_[0];
    for (Hist& h : hists_) {
      if (h.used && h.m == e.m) slot = &h;
      if (!h.used || h.last_frame < oldest->last_frame) oldest = &h;
    }
    const bool fresh = !slot || slot->vtable != e.vtable || slot->last_frame + kHistGap < frame;
    if (!slot) slot = oldest;
    if (fresh) {
      *slot = {e.m, e.vtable, e.row, e.col, e.owner_cursor, frame, frame, frame, true};
    } else {
      if (slot->row != e.row || slot->col != e.col || slot->owner_cursor != e.owner_cursor) slot->change_frame = frame;
      slot->row = e.row, slot->col = e.col, slot->owner_cursor = e.owner_cursor, slot->last_frame = frame;
    }
    e.first_frame = slot->first_frame, e.change_frame = slot->change_frame;
  }

  std::array<MixinRec, kRecs> recs_{};
  size_t count_ = 0;
  std::array<ListNote, kLists> lists_{};
  std::array<Hist, kHists> hists_{};
};

}  // namespace wwe2k14::menu_focus
