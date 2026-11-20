// S105 T3 (docs\research\s105\T3_attire.md): the launcher's participant token, pure (no guest memory, no game types).
//   --wwe2k14_launch_participants=ID[:PAD[:ATTIRE]],...
//   ATTIRE = N     attire 0..9 (CAW ids < 100: 0..3) (the setter's a5), then the colour set setter with -1
//          = N/S   attire N, colour set S (-1..3)
//          = tS    attire 0, colour set S
//          = s     the superstar's saved defaults (the select screen's own copy on a scratch record)
// Split on EVERY ':' (not the first): "397::1" is id 397, pad -1 (empty PAD = CPU), attire 1.
// 1- and 2-part tokens keep the meaning they had before this header (strtol on the id; "397:" = pad 0).
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace wwe2k14::launch {

struct ParticipantToken {
  long id = 0;
  long pad = -1;            // -1 = CPU
  bool has_attire = false;  // a third part was given and understood
  bool saved = false;       // `s`: the superstar's saved defaults
  bool has_set = false;     // the colour set setter is called after the participant setter
  int attire = 0;           // 0..9
  int set = -1;             // -1 = none
};

// The attire limit: a CAW (id < 100) has 4 attires (sub_822C3000's CAW branch, set = attire, 0..3), a stock superstar 0..9.
inline int MaxAttire(long id) { return id >= 0 && id < 100 ? 3 : 9; }

namespace detail {
inline long Lenient(std::string_view s) {  // as the old strtol: leading digits, junk / empty = 0
  const std::string t(s);
  return std::strtol(t.c_str(), nullptr, 10);
}
inline bool WholeInt(std::string_view s, int lo, int hi, int& out) {
  if (s.empty()) return false;
  size_t i = 0;
  bool neg = false;
  if (s[0] == '-') neg = true, i = 1;
  if (i >= s.size() || s.size() - i > 3) return false;
  long v = 0;
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + (s[i] - '0');
  }
  if (neg) v = -v;
  if (v < lo || v > hi) return false;
  out = int(v);
  return true;
}
}  // namespace detail

// Returns false only when the token cannot be used at all (empty, or more than three parts). A bad ATTIRE part is not
// fatal: the id and pad still apply, has_attire stays false and `why` says what was skipped.
inline bool ParseParticipantToken(std::string_view token, ParticipantToken& out, std::string& why) {
  out = ParticipantToken{};
  why.clear();
  if (token.empty()) {
    why = "empty token";
    return false;
  }
  std::vector<std::string_view> parts;
  size_t pos = 0;
  while (true) {
    const size_t c = token.find(':', pos);
    if (c == std::string_view::npos) {
      parts.push_back(token.substr(pos));
      break;
    }
    parts.push_back(token.substr(pos, c - pos));
    pos = c + 1;
  }
  if (parts.size() > 3) {
    why = "more than three ':' parts";
    return false;
  }
  out.id = detail::Lenient(parts[0]);
  if (parts.size() >= 2) {
    // a 2-part token keeps the old reading ("397:" = pad 0); with an attire part an empty PAD is CPU (-1)
    out.pad = (parts.size() == 3 && parts[1].empty()) ? -1 : detail::Lenient(parts[1]);
  }
  if (parts.size() < 3) return true;

  const std::string_view a = parts[2];
  int attire = 0, set = -1;
  if (a == "s") {
    out.has_attire = out.saved = out.has_set = true;  // attire / set / alt come from the game's copy at run time
    return true;
  }
  if (a.size() >= 2 && a[0] == 't') {
    if (!detail::WholeInt(a.substr(1), -1, 3, set)) {
      why = "attire part skipped: colour set out of range (-1..3)";
      return true;
    }
  } else {
    const size_t slash = a.find('/');
    const int amax = MaxAttire(out.id);
    if (!detail::WholeInt(a.substr(0, slash), 0, amax, attire)) {
      why = out.id >= 0 && out.id < 100 ? "attire part skipped: attire not 0..3 for a CAW" : "attire part skipped: attire not 0..9";
      return true;
    }
    if (slash != std::string_view::npos && !detail::WholeInt(a.substr(slash + 1), -1, 3, set)) {
      why = "attire part skipped: colour set out of range (-1..3)";
      return true;
    }
  }
  out.has_attire = out.has_set = true;
  out.attire = attire;
  out.set = set;
  return true;
}

// The colour set's own limits and the game's empty-set predicate (sub_827DAD00, 0x827DADD8-0x827DADF0): a stock id (>= 100)
// has sets 0..2, a CAW (< 100) 0..3; a stock set whose base attire word [record + 860 + 132 set] is 0xFFFFFFFF is empty.
// `word` is that word read big-endian (ignored for a CAW). Returns the set to use (-1 = none) and says why in `why`.
inline int ResolveSet(long id, int set, uint32_t word, std::string& why) {
  why.clear();
  if (set < 0) return -1;
  if (id >= 100) {
    if (set > 2) {
      why = "set out of range for a stock superstar (0..2)";
      return -1;
    }
    if (word == 0xFFFFFFFFu) {
      why = "that colour set is empty";
      return -1;
    }
    return set;
  }
  if (set > 3) {
    why = "set out of range for a CAW (0..3)";
    return -1;
  }
  return set;
}

// The whole per-participant decision of FireLaunch (src/scenario.h), pure: which attire (r7), whether sub_822C4FF8 (alternate
// look key) and sub_822C6808 (colour set) are called, and with what. `saved` = the select screen's copy for an `s` token (null
// = it could not be made); `word_of_set(set)` reads the game's base attire word [record + 860 + 132 set] and is called only for
// a stock id's explicit set 0..2 (the record is slot+104+456 when slot+68 != 0, else sub_827DA658(id): 0x822C6840-0x822C6854).
struct SavedDefaults {
  int attire = 0;
  int set = -1;
  uint32_t alt = 512;
};
struct ParticipantPlan {
  int attire = 0;
  bool alt_call = false;  // sub_822C4FF8(m, slot, alt, attire, 1)
  uint32_t alt = 512;
  bool set_call = false;  // sub_822C6808(m, slot, set)
  int set = -1;
  std::string set_why;    // non-empty = the set was refused
};
template <class WordFn>
inline ParticipantPlan PlanParticipant(const ParticipantToken& tok, const SavedDefaults* saved, WordFn word_of_set) {
  ParticipantPlan p;
  p.attire = tok.attire;
  int set_want = tok.set;
  if (tok.has_attire && tok.saved && saved) {
    p.attire = saved->attire;
    set_want = saved->set;
    p.alt = saved->alt;
    if (p.attire < 0 || p.attire > MaxAttire(tok.id)) p.attire = 0;
  }
  p.alt_call = tok.has_attire && tok.saved && p.alt != 512;
  if (tok.has_attire && tok.has_set) {
    uint32_t word = 0;
    if (set_want >= 0 && set_want <= 2 && tok.id >= 100) word = word_of_set(set_want);
    p.set = ResolveSet(tok.id, set_want, word, p.set_why);
    p.set_call = true;
  }
  return p;
}

}  // namespace wwe2k14::launch
