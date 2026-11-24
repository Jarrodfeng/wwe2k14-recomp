// S108 lane THR (docs\research\s108\THR_plan.md): the Superstar Threads editor's live colours, read where the game hands them to the GPU.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The game's rule [C, listings; our words]:
//   * The editor installs a draw callback on the edited character's body model [char+48] and its SUBM piece models (sub_8339F4C0):
//     model+324 = 0x8339F0E0, model+328 = E (the editor object; the only store of 0x8339F0E0 in the game).
//   * Its per-frame method (sub_8339F5D0, editor state [E+20] == 10) writes the switch model+604 (sub_825A0E00). While model+604 != 0 the model draw
//     (sub_82C04DF0 +BB4) takes each material's effect from model+392 (the second effect registry: EDIT/STS, edit.pac PACH 9) instead of +384.
//   * The draw then calls [model+324](model, [model+328]) (sub_82C04DF0 +4A00..+4A20). The callback sends, when [E+20] == 10 only, 16 colours
//     g_fChangeColor[i] = (E+92+3i, E+93+3i, E+94+3i, 255) / 255 (PS c16..c31 in ych_gear's table) and binds the descriptor's Mask1..Mask4 at samplers 7, 4, 14, 2.
// What this file does: the pure read of one draw (template over the guest word reader, so the unit drives it with a byte map), and the per-body store the
// draw-ask hook (WrestlerContributor::NoteDrawAsked, the game's draw, game thread) fills and the record side reads. Guest memory is read ONLY inside the hook, at
// the model address the game passed to its draw; E is re-read from [model+328] at every draw, never remembered.
//
// UNSETTLED [U] (the census counts it): a frame where model+604 is set but [E+20] != 10. The callback sends no colours then, and PS c16..c31 are written by
// other draws of the same frame (fog c20 / c21 sub_82C1C1A0, c27 / c29 / c30 sub_82C1B668, c31 sub_82C1C768, the pre-light method c16.. sub_82C1FBE0, the
// post passes c16..c31), so what the body's draw really sees in those frames is not known. We draw the colours of the last state-10 read and count the frame
// `unsettled` (no guess is presented as the game's value: the census names every such frame).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace rdr_scene::live::change_colour {

constexpr uint32_t kCallbackThreads = 0x8339F0E0u;   // sub_8339F0E0: the Threads editor's draw callback (installed by sub_8339F4C0 only)
// S108 THR2: sub_822E6CC8 = the Create a Superstar editor's logo callback (q addr: 2 code refs, both in sub_822E6DE8, which stores it at model+324 / +328). Its draw is A5c's
// (the logo table, char_effects.cpp): with the switch on it is NOT a change-colour shortfall (counted `caw_logo`, never reported), so the CAW editor raises no false alarm.
constexpr uint32_t kCallbackCawLogo = 0x822E6CC8u;
constexpr uint32_t kOffSwitch = 604, kOffCallback = 324, kOffCallbackArg = 328;   // model words
constexpr uint32_t kOffState = 20, kOffColours = 92;                               // editor object E
constexpr uint32_t kStateEditing = 10;
constexpr int kColours = 16, kColourBytes = 48;

// The sampler slot of each mask (Mask1..Mask4), as the callback binds them; -1 = not a mask slot.
inline int MaskIndexOfSlot(int slot) { return slot == 7 ? 0 : slot == 4 ? 1 : slot == 14 ? 2 : slot == 2 ? 3 : -1; }

enum class Read {
  kOff,              // model+604 == 0: the game draws the +384 row (no change colour)
  kFresh,            // switch on, the Threads callback, state 10: the 48 bytes were read this draw
  kUnsettled,        // switch on, the Threads callback, state != 10: no colours sent this draw (see UNSETTLED above)
  kForeignCallback,  // switch on, another callback (another editor's): not ours to read (a shortfall)
  kNoCallback,       // switch on, no callback: the GPU keeps whatever was written last (a shortfall)
  kUnreadable,       // a guest word could not be read (a shortfall)
  kCawLogo,          // switch on, the Create a Superstar logo callback (A5c draws it): not ours to recolour, not a shortfall
};

struct Sample {
  Read read = Read::kOff;
  uint32_t callback = 0;             // [model+324] when read (the shortfall line names it)
  uint32_t editor = 0;               // E (re-read this draw), 0 = none
  uint32_t state = 0;                // [E+20]
  uint8_t colours[kColourBytes] = {};  // valid for kFresh
};

// Planted faults for the offline proof; never set in the game.
enum class Fault { kNone, kIgnoreSwitch /* recolour without the switch */, kIgnoreState /* a non-10 state reads E+92 as fresh */, kAnyCallback /* any callback read as the Threads editor's */ };
inline Fault& PlantForTest() {   // the unit's planted rule, applied by the hook's call (never set in the game)
  static Fault f = Fault::kNone;
  return f;
}

// The pure read of one draw. `rw(addr, uint32_t&) -> bool` reads one big-endian guest word (the hook's page-checked ReadWord).
template <class ReadWordFn>
inline Sample ReadDraw(ReadWordFn rw, uint32_t model, Fault fault = PlantForTest()) {
  Sample s;
  uint32_t sw = 0, cb = 0, e = 0;
  if (!rw(model + kOffSwitch, sw)) return s.read = Read::kUnreadable, s;
  if (sw == 0 && fault != Fault::kIgnoreSwitch) return s.read = Read::kOff, s;
  if (!rw(model + kOffCallback, cb) || !rw(model + kOffCallbackArg, e)) return s.read = Read::kUnreadable, s;
  s.callback = cb;
  if (cb == 0) return s.read = Read::kNoCallback, s;
  if (cb == kCallbackCawLogo && fault != Fault::kAnyCallback) return s.read = Read::kCawLogo, s;
  if (cb != kCallbackThreads && fault != Fault::kAnyCallback) return s.read = Read::kForeignCallback, s;
  if (e < 0x10000u) return s.read = Read::kUnreadable, s;   // the callback itself returns at once for a null argument
  s.editor = e;
  if (!rw(e + kOffState, s.state)) return s.read = Read::kUnreadable, s;
  if (s.state != kStateEditing && fault != Fault::kIgnoreState) return s.read = Read::kUnsettled, s;
  for (int i = 0; i < kColourBytes / 4; ++i) {
    uint32_t w = 0;
    if (!rw(e + kOffColours + 4u * uint32_t(i), w)) return s.read = Read::kUnreadable, s;
    for (int b = 0; b < 4; ++b) s.colours[4 * i + b] = uint8_t(w >> (24 - 8 * b));   // big-endian word -> bytes in memory order
  }
  return s.read = Read::kFresh, s;
}

// The 16 constants the callback sends: (R, G, B, 1) with each byte / 255 (`f31 = 1/255`; the fourth lane stores 1.0).
inline void ToConstants(const uint8_t colours[kColourBytes], float out[kColours * 4]) {
  for (int i = 0; i < kColours; ++i) {
    out[4 * i + 0] = float(colours[3 * i + 0]) * (1.0f / 255.0f);
    out[4 * i + 1] = float(colours[3 * i + 1]) * (1.0f / 255.0f);
    out[4 * i + 2] = float(colours[3 * i + 2]) * (1.0f / 255.0f);
    out[4 * i + 3] = 1.0f;
  }
}

struct Census {
  uint64_t off = 0, fresh = 0, unsettled = 0, foreign = 0, no_callback = 0, unreadable = 0, forgotten = 0, aged = 0;
  uint64_t caw_logo = 0;       // S108 THR2: switch on with the CAW logo callback (A5c's draw)
  uint64_t rows = 0;           // S108 THR2: section-67 rows the record side wrote (Collect, one per recoloured body per record)
  uint64_t piece_apart = 0;    // S108 THR2: a piece of a recoloured body drawn as its own object (detached / own rows): drawn plain (change_colour_piece)
};

// S108 THR2: one refusal to report through rdr::DrawShortfall. The hook side (game thread) and the record side only QUEUE it here (they are linked into units with no
// report sink); the render side drains the queue once per frame (rf_wrestlers.cpp ApplyWrestlers -> ReportChangeColourShortfalls). Once per (kind, object) a run.
struct Shortfall {
  const char* kind = "";   // "change_colour" (a body the game recolours that we cannot) / "change_colour_piece"
  std::string object;      // what a person needs to find it (the model address and the reason)
  std::string detail;
};

// One body's change-colour state, as the draw left it. Keyed by the body model address the game passed to its draw (= the wrestler skeleton key).
struct Body {
  bool on = false;            // the game draws this body with the change-colour row (fresh or unsettled)
  bool have_colours = false;  // a state-10 read has happened for this body since it was (re)made
  bool unsettled = false;     // this frame's draw had no colours sent (see UNSETTLED)
  uint8_t colours[kColourBytes] = {};
  uint64_t frame = 0;         // the guest frame of the last draw read
};

// The per-body store. Written by the draw hook (under the caller's lock or its own), read by the record side. Entries are dropped when the game destroys the
// model (ForgetModel, wired to the wrestlers' model-release hook) or when no draw refreshed them for `kAgeFrames` frames (a body the game stopped drawing);
// nothing here is ever used to read guest memory.
class Store {
 public:
  static constexpr uint64_t kAgeFrames = 60;
  // One draw's read. Returns the body's state after the draw.
  Body Note(uint32_t model, uint64_t frame, const Sample& s) {
    std::lock_guard<std::mutex> l(mu_);
    switch (s.read) {
      case Read::kOff: ++c_.off; break;
      case Read::kFresh: ++c_.fresh; break;
      case Read::kUnsettled: ++c_.unsettled; break;
      case Read::kForeignCallback: ++c_.foreign; break;
      case Read::kNoCallback: ++c_.no_callback; break;
      case Read::kUnreadable: ++c_.unreadable; break;
      case Read::kCawLogo: ++c_.caw_logo; break;
    }
    if (s.read == Read::kForeignCallback || s.read == Read::kNoCallback || s.read == Read::kUnreadable) {   // S108 THR2: the game recolours this body and we cannot: said once per (model, reason)
      char obj[96], det[160];
      std::snprintf(obj, sizeof obj, "model 0x%08X: %s", model, s.read == Read::kForeignCallback ? "foreign callback" : s.read == Read::kNoCallback ? "no callback" : "unreadable");
      std::snprintf(det, sizeof det, "switch model+604 set, callback 0x%08X, editor 0x%08X: drawn with its plain row", s.callback, s.editor);
      QueueLocked("change_colour", obj, det);
    }
    if (s.read == Read::kOff || s.read == Read::kForeignCallback || s.read == Read::kNoCallback || s.read == Read::kUnreadable || s.read == Read::kCawLogo) {
      // Not ours to draw recoloured this frame: the body draws its plain row. A switch-off body forgets its colours (a later switch-on starts afresh).
      auto it = bodies_.find(model);
      if (it == bodies_.end()) return Body{};
      if (s.read == Read::kOff) { bodies_.erase(it); return Body{}; }
      it->second.on = false, it->second.unsettled = false, it->second.frame = frame;
      return it->second;
    }
    Body& b = bodies_[model];
    b.frame = frame;
    if (s.read == Read::kFresh) {
      std::memcpy(b.colours, s.colours, kColourBytes);
      b.have_colours = true, b.on = true, b.unsettled = false;
    } else {   // kUnsettled: the colours of the last state-10 read, counted; with none yet the body draws plain
      b.on = b.have_colours, b.unsettled = true;
    }
    return b;
  }
  // The record side: the body's state, or nothing (off / never drawn recoloured / aged out at `now`).
  bool Get(uint32_t model, uint64_t now, Body& out) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = bodies_.find(model);
    if (it == bodies_.end()) return false;
    if (now > it->second.frame + kAgeFrames) {
      bodies_.erase(it);
      ++c_.aged;
      return false;
    }
    if (!it->second.on) return false;
    out = it->second;
    return true;
  }
  // The game destroyed this model (the wrestlers' model-release hook): its address may be reused by another object, so the entry goes at once.
  void ForgetModel(uint32_t model) {
    std::lock_guard<std::mutex> l(mu_);
    if (bodies_.erase(model)) ++c_.forgotten;
  }
  void Clear() {
    std::lock_guard<std::mutex> l(mu_);
    bodies_.clear();
  }
  // S108 THR2: the record side wrote this body's section-67 row (census).
  void NoteRow() {
    std::lock_guard<std::mutex> l(mu_);
    ++c_.rows;
  }
  // S108 THR2: a piece of a recoloured owner is drawn as its OWN object (a detached / own-rows piece, EmitItems): the draw side has no colours for it, so it is drawn plain
  // (the game recolours it: sub_8339F4C0 installs the callback on the SUBM piece models). Counted and reported once per piece model (kind change_colour_piece). A piece the
  // owner's merged model draws (kObjDrawnByOwner) takes the owner's change-colour bind and is not a shortfall.
  void NotePieceApart(uint32_t piece, uint32_t owner) {
    std::lock_guard<std::mutex> l(mu_);
    ++c_.piece_apart;
    char obj[64], det[128];
    std::snprintf(obj, sizeof obj, "piece model 0x%08X", piece);
    std::snprintf(det, sizeof det, "owner 0x%08X is recoloured; the piece is drawn as its own object with its plain row", owner);
    QueueLocked("change_colour_piece", obj, det);
  }
  // S108 THR2: the queued refusals (each (kind, object) once a run), handed to the caller and cleared.
  void TakeShortfalls(std::vector<Shortfall>& out) {
    std::lock_guard<std::mutex> l(mu_);
    out.swap(pending_);
    pending_.clear();
  }
  // Is a body on now (the record side's question for a piece's owner, under the age rule of Get).
  bool IsOn(uint32_t model, uint64_t now) {
    Body b;
    return Get(model, now, b);
  }
  size_t Size() {
    std::lock_guard<std::mutex> l(mu_);
    return bodies_.size();
  }
  Census Counts() {
    std::lock_guard<std::mutex> l(mu_);
    return c_;
  }
  std::string CensusLine() {
    const Census c = Counts();
    char b[480];
    std::snprintf(b, sizeof b, "[scene][change_colour] draws: fresh %llu, unsettled %llu (switch on, editor state != 10: colours held), off %llu, caw_logo %llu; shortfall: foreign callback %llu, no callback %llu, unreadable %llu, piece_apart %llu; bodies %zu, rows %llu, forgotten %llu, aged %llu",
                  (unsigned long long)c.fresh, (unsigned long long)c.unsettled, (unsigned long long)c.off, (unsigned long long)c.caw_logo, (unsigned long long)c.foreign, (unsigned long long)c.no_callback,
                  (unsigned long long)c.unreadable, (unsigned long long)c.piece_apart, Size(), (unsigned long long)c.rows, (unsigned long long)c.forgotten, (unsigned long long)c.aged);
    return b;
  }
  void ResetForTest() {
    std::lock_guard<std::mutex> l(mu_);
    bodies_.clear(), c_ = Census{}, pending_.clear(), reported_.clear();
  }

 private:
  static constexpr size_t kMaxKeys = 4096, kMaxPending = 64;
  void QueueLocked(const char* kind, const std::string& object, const std::string& detail) {
    if (reported_.size() >= kMaxKeys || pending_.size() >= kMaxPending) return;   // (the draw_shortfall sink keeps its own bound; this only caps memory)
    if (!reported_.insert(std::string(kind) + '\n' + object).second) return;
    pending_.push_back(Shortfall{kind, object, detail});
  }
  std::mutex mu_;
  std::map<uint32_t, Body> bodies_;
  Census c_;
  std::set<std::string> reported_;
  std::vector<Shortfall> pending_;
};

inline Store& Bodies() {
  static Store s;
  return s;
}

}  // namespace rdr_scene::live::change_colour
