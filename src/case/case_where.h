// wwe2k14 - the case zip's `where` block (S101 lane 32, go-anywhere): turn the live game state into a goto address and the words around it.
//
// No game or SDK includes: the caller (src/bookmark.h FillCaseWhere) reads the state words and cvars it already has and hands them over in
// `Inputs`; src/case/where_test.cpp (tools/case/selftest.py section 12) feeds synthetic inputs to this exact header. Nothing here reads guest memory.
//
// Address rules (first that applies), each part marked exact or best-effort in `where.address_basis` (a basis that starts "exact" is a claim intake checks):
//   a scene was forced by the director (src/scene_forced.h)  menu -> scene:ID (exact);  after / mid / victory -> match:SPEC@after:ID / @scene:ID /
//                                                            @victory:ID (the basis of SPEC, below)
//   a recording is being replayed (wwe2k14_replay_play)      moment:NAME@frame:F (best-effort: the recording must be registered as a moment NAME)
//   the 30 Years jump is armed (wwe2k14_wm30 >= 0)           wm30:N (best-effort: the cvar says where the run was sent, not that it is still there)
//   in a match (sequence 9)                                  match:SPEC (+ @bell when the match clock runs). SPEC is the run's wwe2k14_match only when
//                                                            the builder really built it (wwe2k14_match_go != 0, not dry) AND it agrees with the live
//                                                            match description M (type, the given participant ids as a prefix, the arena unless absent /
//                                                            300): exact. Disagreeing with M (a stale cvar): SPEC is rebuilt from M, best-effort. M not
//                                                            readable: the cvar, best-effort. No cvar: rebuilt from M, best-effort (controllers assumed,
//                                                            rules / options not carried; more than 6 participants are left out, said in the basis)
//   on a main-menu list (sequence 75, menu word read)        menu:list:N (exact for the atlas's lists 1-60, else best-effort)
//   anything else                                            null, with a reason in manifest.unavailable["where.address"]
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "case_manifest.h"

namespace rdr_case::where {

constexpr int kMaxSpecParticipants = 6;  // src/scenario.h ParseBuildSpec / the builder's slots: more p= items are refused by tools/goto
constexpr int kAtlasListFirst = 1, kAtlasListLast = 60;  // docs/systems/game_atlas.json has list:1 .. list:60 (the selftest pins this)

struct Inputs {
  int64_t seq = -1, scene_word = -1, menu = -1, match_clock = -1;  // states.toml: seq, scene_word, menu, match_clock (-1 = unreadable)
  std::vector<int64_t> entrance_steps;                             // entrance_step_0..5
  std::string match_spec;                                          // the wwe2k14_match cvar (the numeric builder spec the run used), "" = none
  int64_t match_go = 0;                                            // wwe2k14_match_go (0 = the builder was never triggered)
  bool match_dry = false;                                          // wwe2k14_match_dry (built, not launched)
  int64_t wm30 = -1;                                               // the wwe2k14_wm30 cvar
  std::string replay_play;                                         // the wwe2k14_replay_play cvar (a recording being replayed)
  struct Live {                                                    // the live match description M (0x836F2D88), read like session_log.h's card
    bool have = false;
    int64_t type = -1, arena = -1, pace = -1;
    std::vector<int64_t> ids;                                      // participant character ids in slot order
  } live;
  std::optional<int64_t> ring_bank, ring_type;                     // the scene ring's current slot bank / context type (S-1 layout), when a manager exists
  std::string forced_verb;                                         // scene director marker (scene_forced.h): menu | mid | victory | after | ""
  int64_t forced_id = 0;
  uint64_t guest_frame = 0;
};

inline bool Token(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
    if (!ok) return false;
  }
  return true;
}

// The stem of a recording path ("C:\x\out\moments\bryan_entrance\bryan_entrance.rec" -> "bryan_entrance"), "" when it is not a token.
inline std::string Stem(const std::string& path) {
  size_t a = path.find_last_of("/\\");
  std::string f = a == std::string::npos ? path : path.substr(a + 1);
  const size_t dot = f.find_last_of('.');
  if (dot != std::string::npos && dot > 0) f = f.substr(0, dot);
  return Token(f) ? f : std::string();
}

// The match spec of the live match description, in the game's numeric builder form (tools/atlas/match.py's output). *note says what was left out.
inline std::string LiveSpec(const Inputs::Live& l, std::string* note = nullptr) {
  std::string s = "type=" + std::to_string(l.type);
  if (!l.ids.empty() && l.ids.size() <= size_t(kMaxSpecParticipants)) {
    s += ";p=";
    for (size_t i = 0; i < l.ids.size(); ++i) s += (i ? "," : "") + std::to_string(l.ids[i]) + ":" + (i == 0 ? "0" : "-1");
  } else if (!l.ids.empty() && note) {
    *note = "; the " + std::to_string(l.ids.size()) + " participants are left out (a spec takes at most " + std::to_string(kMaxSpecParticipants) + ")";
  }
  if (l.arena >= 0) s += ";arena=" + std::to_string(l.arena);
  if (l.pace >= 0 && l.pace <= 2) s += ";pace=" + std::to_string(l.pace);
  return s;
}

// Does the builder spec `spec` describe the match `l`? type equal; each given p id equal to l.ids[i] (a prefix: a partial-seat spec is fine); the
// arena equal unless absent or 300 (random); a rumble= spec only has its type compared (its entrants are drawn / ordered by the game).
inline bool SpecAgrees(const std::string& spec, const Inputs::Live& l) {
  bool rumble = false;
  int64_t type = -2, arena = -1;
  std::vector<int64_t> ids;
  size_t a = 0;
  while (a <= spec.size()) {
    size_t b = spec.find(';', a);
    if (b == std::string::npos) b = spec.size();
    const std::string kv = spec.substr(a, b - a);
    a = b + 1;
    const size_t eq = kv.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
    if (k == "type") type = std::atoll(v.c_str());
    else if (k == "arena") arena = std::atoll(v.c_str());
    else if (k == "rumble") rumble = true;
    else if (k == "p") {
      size_t c = 0;
      while (c <= v.size()) {
        size_t d = v.find(',', c);
        if (d == std::string::npos) d = v.size();
        if (d > c) ids.push_back(std::atoll(v.c_str() + c));
        c = d + 1;
      }
    }
  }
  if (type != l.type) return false;
  if (!rumble) {
    if (ids.size() > l.ids.size()) return false;
    for (size_t i = 0; i < ids.size(); ++i)
      if (ids[i] != l.ids[i]) return false;
  }
  if (arena >= 0 && arena != 300 && l.arena >= 0 && arena != l.arena) return false;
  return true;
}

struct Result {
  std::optional<std::string> address;
  std::string basis;   // "exact: ..." / "best-effort: ..." parts joined by "; "
  std::string reason;  // when address is null
};

inline Result Address(const Inputs& in) {
  Result r;
  std::string spec, spec_basis;
  const bool in_match = in.seq == 9;
  const bool live_ok = in.live.have && in.live.type >= 0;
  if (in_match && !in.match_spec.empty()) {
    const bool built = in.match_go != 0 && !in.match_dry;
    if (live_ok && !SpecAgrees(in.match_spec, in.live)) {
      std::string note;
      spec = LiveSpec(in.live, &note);
      spec_basis = "best-effort: wwe2k14_match (" + in.match_spec + ") disagrees with the live match description (a stale setting), so the spec is rebuilt from it" + note;
    } else if (!live_ok) {
      spec = in.match_spec;
      spec_basis = "best-effort: the live match description was not readable, so wwe2k14_match could not be checked against it";
    } else if (!built) {
      spec = in.match_spec;
      spec_basis = "best-effort: wwe2k14_match agrees with the live match but the match builder did not build this match (wwe2k14_match_go " + std::to_string(in.match_go) +
                   (in.match_dry ? ", dry" : "") + ")";
    } else {
      spec = in.match_spec;
      spec_basis = "exact: the match spec is this run's wwe2k14_match (built by the match builder, agrees with the live match description)";
    }
  } else if (in_match && live_ok) {
    std::string note;
    spec = LiveSpec(in.live, &note);
    spec_basis = "best-effort: the match spec is rebuilt from the live match description (type, arena, pace, participant ids; controllers assumed, rules / options not carried)" + note;
  }
  const bool bell = in.match_clock > 0;
  const std::string id = std::to_string(in.forced_id);
  if (in.forced_verb == "menu") {
    r.address = "scene:" + id;
    r.basis = "exact: scene " + id + " was forced from the main menu by the scene director";
  } else if ((in.forced_verb == "step" || in.forced_verb == "fire") && in.wm30 >= 0 && in.wm30 <= 45) {
    // S101 lane 121: the 30 Years step / fire verbs (src/wm30_step.h, src/wm30_fire.h); the marker's id is the step / the bank
    r.address = "wm30:" + std::to_string(in.wm30) + (in.forced_verb == "step" ? "@step:" : "@fire:") + id;
    r.basis = "best-effort: the 30 Years jump was set to match " + std::to_string(in.wm30) + " and the " + in.forced_verb + " verb forced " + (in.forced_verb == "step" ? "step " : "bank ") + id +
              " (a guarded step needs /force to replay)";
  } else if (!in.forced_verb.empty()) {
    const char* suffix = in.forced_verb == "after" ? "@after:" : in.forced_verb == "mid" ? "@scene:" : in.forced_verb == "victory" ? "@victory:" : nullptr;
    if (!suffix) {
      r.reason = "an unknown forced-scene verb '" + in.forced_verb + "'";
    } else if (spec.empty()) {
      r.reason = "a scene was forced (" + in.forced_verb + " " + id + ") but no match spec is known (not in a match, or the match description was not readable)";
    } else {
      r.address = "match:" + spec + suffix + id;
      r.basis = spec_basis + "; the forced " + in.forced_verb + " bank " + id + " is exact (the scene director's marker)";
    }
  } else if (!in.replay_play.empty() && !Stem(in.replay_play).empty()) {
    r.address = "moment:" + Stem(in.replay_play) + "@frame:" + std::to_string(in.guest_frame);
    r.basis = "best-effort: a recording is being replayed; the address needs it registered as moment '" + Stem(in.replay_play) + "' (tools/goto/moments.py add)";
  } else if (in.wm30 >= 0 && in.wm30 <= 45) {
    r.address = "wm30:" + std::to_string(in.wm30);
    r.basis = "best-effort: the 30 Years jump was set to match " + std::to_string(in.wm30) + " (the setting says where the run was sent, not that it is still there)";
  } else if (!spec.empty()) {
    r.address = "match:" + spec + (bell ? "@bell" : "");
    r.basis = spec_basis + (bell ? "; @bell: the match clock was running" : "");
  } else if (in.seq == 75 && in.menu >= 0) {
    r.address = "menu:list:" + std::to_string(in.menu);
    if (in.menu >= kAtlasListFirst && in.menu <= kAtlasListLast)
      r.basis = "exact for the list: the main-menu list id (SmartMenu +364) is " + std::to_string(in.menu) + " (the cursor row is not carried)";
    else
      r.basis = "best-effort: the SmartMenu list id is " + std::to_string(in.menu) + ", which the atlas has no node for (it knows lists " + std::to_string(kAtlasListFirst) + "-" +
                std::to_string(kAtlasListLast) + ")";
  } else {
    r.reason = "the game was in sequence " + std::to_string(in.seq) + " with no match spec, no readable match description and no readable menu list: no address names this state";
  }
  return r;
}

inline std::string SceneWordName(int64_t w) { return w == 4 ? "match load / entrances" : w == 1 ? "in the ring" : "?"; }

// Fills manifest.where (address, basis, screen, match, scene, words) and the reasons for what stays empty.
inline void Fill(Manifest& m, const Inputs& in) {
  auto& w = m.where;
  const Result r = Address(in);
  if (r.address) {
    w.address = r.address;
    w.address_basis = r.basis;
  } else {
    m.Unavailable("where.address", r.reason);
  }
  if (in.seq == 75 && in.menu >= 0) {
    w.menu_id = uint32_t(in.menu);
    if (in.menu >= kAtlasListFirst && in.menu <= kAtlasListLast) w.node = "list:" + std::to_string(in.menu);
  }
  if (in.seq == 9 && in.live.have) {
    if (in.live.type >= 0) w.match_type = std::to_string(in.live.type);
    if (in.live.arena >= 0) w.arena = std::to_string(in.live.arena);
    for (int64_t id : in.live.ids) w.wrestlers.push_back(std::to_string(id));
  }
  if (in.seq == 9 && in.match_clock >= 0) w.clock_frames = uint64_t(in.match_clock);
  w.scene.scene_word = in.scene_word >= 0 ? std::optional<int64_t>(in.scene_word) : std::nullopt;
  w.scene.bank = in.ring_bank;
  w.scene.ctx_type = in.ring_type;
  w.scene.entrance_steps = in.entrance_steps;
  std::string words;
  if (in.seq == 9) {
    words = "in a match";
    if (in.live.have && in.live.type >= 0) words += " (type " + std::to_string(in.live.type) + ", arena " + std::to_string(in.live.arena) + ")";
    if (in.scene_word >= 0) words += ", scene word " + std::to_string(in.scene_word) + " (" + SceneWordName(in.scene_word) + ")";
    if (in.ring_bank) words += ", scene ring bank " + std::to_string(*in.ring_bank) + " (best-effort)";
  } else if (in.seq == 75) {
    words = in.menu >= 0 ? "main menu list " + std::to_string(in.menu) : std::string("in the menus");
  } else if (in.seq >= 0) {
    words = "sequence " + std::to_string(in.seq);
  }
  if (!in.forced_verb.empty()) words += "; FORCED scene (" + in.forced_verb + " " + std::to_string(in.forced_id) + ")";
  if (!words.empty()) w.words = words;
  // What this block cannot say, with the reason (a null is never silent).
  if (!w.camera_kind) m.Unavailable("where.camera.kind", "no camera-kind state word is exposed to the case writer (CineCam / game camera state is not read)");
  if (w.attires.empty()) m.Unavailable("where.match.attires", "the match description's attire field is not mapped; the builder spec carries none");
  if (!in.ring_bank) m.Unavailable("where.scene.bank", "no scene manager existed (R = 0), or its current context was not readable: no scene bank was playing");
}

}  // namespace rdr_case::where
