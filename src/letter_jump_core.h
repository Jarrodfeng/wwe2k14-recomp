// S110 LETTER (docs\research\s110\LETTERJUMP.md, the owner's idea docs\IDEAS.md:352): the pure half of the select screen's letter jump.
// std only, no game memory: the initial of a name, and which row a trigger press moves the cursor to. The game side is src\letter_jump.h;
// the unit is tools\letter_jump\letter_jump_unit.cpp (run by letter_jump_selftest.py, with planted twins).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wwe2k14::letter_jump {

// The bucket of a name that starts with a digit or a symbol; it sorts before 'A' ('#' is 0x23).
constexpr char kSymbolBucket = '#';

// One row of the list the panel shows. `index` is the row's index in the panel's list (what the game's "cursor to row N" takes), `letter` is
// Initial() of its name (0 = no letter: the row is never a target), `selectable` = the panel could rest on it (the game's own walk would accept it).
struct Row {
  int index = 0;
  char letter = 0;
  bool selectable = true;
};

namespace detail {

// A Latin-1 letter (0xC0..0xFF) to its base letter; 0 = not a letter there.
inline char Latin1Base(uint32_t c) {
  if (c == 0xFF) return 'Y';  // y with diaeresis
  if (c >= 0xE0 && c != 0xF7) c -= 0x20;  // the lower-case half folds onto the upper (0xDF, sharp s, is handled before)
  switch (c) {
    case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: case 0xC6: return 'A';
    case 0xC7: return 'C';
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: return 'E';
    case 0xCC: case 0xCD: case 0xCE: case 0xCF: return 'I';
    case 0xD0: return 'D';
    case 0xD1: return 'N';
    case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD8: return 'O';
    case 0xD9: case 0xDA: case 0xDB: case 0xDC: return 'U';
    case 0xDD: return 'Y';
    default: return 0;
  }
}

// The code point at s[i] and its width. A well-formed UTF-8 sequence (lead 0xC2..0xF4 and its continuation bytes; the text tables are UTF-8,
// docs\formats\string_pac.md) is read as its code point, so a lead byte outside Latin-1 (0xC5 of S caron, 0xE2 of a curly quote) is never taken for a
// Latin-1 letter; any other byte is Latin-1.
inline uint32_t CodeAt(const std::string& s, size_t i, size_t& width) {
  const uint8_t c = uint8_t(s[i]);
  width = 1;
  const size_t n = (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 1;
  if (n > 1 && i + n <= s.size()) {
    uint32_t cp = c & (0xFFu >> (n + 1));
    size_t k = 1;
    for (; k < n && (uint8_t(s[i + k]) & 0xC0) == 0x80; ++k) cp = (cp << 6) | (uint8_t(s[i + k]) & 0x3F);
    if (k == n) {
      width = n;
      return cp;
    }
  }
  return c;
}

inline char Upper(char c) { return (c >= 'a' && c <= 'z') ? char(c - 32) : c; }

}  // namespace detail

// The initial of a display name: case-folded; a leading "THE " dropped (the game's own rule for its stock ranks: "THE ROCK" sits under R);
// Latin-1 accents to the base letter; a digit or symbol -> kSymbolBucket; an empty (or blank) name -> 0 (no letter: the row is skipped).
inline char Initial(const std::string& name) {
  size_t i = 0;
  while (i < name.size() && name[i] == ' ') ++i;
  if (i >= name.size()) return 0;
  if (name.size() - i > 4 && detail::Upper(name[i]) == 'T' && detail::Upper(name[i + 1]) == 'H' && detail::Upper(name[i + 2]) == 'E' && name[i + 3] == ' ') {
    size_t j = i + 4;
    while (j < name.size() && name[j] == ' ') ++j;
    if (j < name.size()) i = j;  // "THE " alone (nothing after it) stays under T
  }
  size_t w = 1;
  const uint32_t c = detail::CodeAt(name, i, w);
  if (c < 0x80) {
    const char u = detail::Upper(char(c));
    return (u >= 'A' && u <= 'Z') ? u : kSymbolBucket;
  }
  if (c == 0xDF) return 'S';  // sharp s (a code point beyond Latin-1 has no Latin1Base letter: the # bucket)
  const char b = detail::Latin1Base(c);
  return b ? b : kSymbolBucket;
}

// Where a trigger press puts the cursor. `cur` = the list index of the row the cursor is on. dir +1 = next letter, -1 = previous.
// The list is not globally alphabetical (stock rows are, then the DLC rows in pack order, then the imports), so the rule is defined on the letters
// PRESENT among the selectable rows and on "the first such row in LIST ORDER" (the order of `rows`): every present letter has a row to land on.
//   +1: the first row of the next letter present after the current row's letter (wrapping from the last letter to the first).
//   -1: from the middle of a letter's run (the cursor is on that letter but not on its first row) to the FIRST row of the CURRENT letter; from
//       its first row (or from a row whose letter has no selectable row) to the first row of the previous letter present (wrapping).
// -1 (the return value) = nothing to do: fewer than two letters are present among the selectable rows (one row, or every row the same letter).
inline int NextRow(const std::vector<Row>& rows, int cur, int dir) {
  bool present[256] = {};
  int distinct = 0;
  char cl = 0;  // the current row's letter, 0 = unknown
  for (const Row& r : rows) {
    if (r.index == cur) cl = r.letter;
    if (!r.selectable || r.letter == 0) continue;
    if (!present[uint8_t(r.letter)]) {
      present[uint8_t(r.letter)] = true;
      ++distinct;
    }
  }
  if (distinct < 2) return -1;
  auto first_of = [&](char l) {
    for (const Row& r : rows)
      if (r.selectable && r.letter == l) return r.index;
    return -1;
  };
  char target = 0;
  if (dir > 0) {
    for (int c = int(uint8_t(cl)) + 1; c < 256 && !target; ++c)
      if (present[c]) target = char(c);
    for (int c = 0; c < 256 && !target; ++c)  // wrap
      if (present[c]) target = char(c);
  } else {
    if (cl && present[uint8_t(cl)]) {
      const int f = first_of(cl);
      if (f != cur) return f;  // the middle of a run: back to the start of the letter first
    }
    for (int c = int(uint8_t(cl)) - 1; c >= 0 && !target; --c)
      if (present[c]) target = char(c);
    for (int c = 255; c >= 0 && !target; --c)  // wrap
      if (present[c]) target = char(c);
  }
  if (!target) return -1;
  const int n = first_of(target);
  return n == cur ? -1 : n;
}

// S110 LETTERUI: the letters the jump can land on, ascending and distinct (the selectable rows' letters): what the letter strip shows, so the strip and the
// jump never disagree. NextRow's `present` set, as a list.
inline std::vector<char> PresentLetters(const std::vector<Row>& rows) {
  bool present[256] = {};
  for (const Row& r : rows)
    if (r.selectable && r.letter != 0) present[uint8_t(r.letter)] = true;
  std::vector<char> out;
  for (int c = 0; c < 256; ++c)
    if (present[c]) out.push_back(char(c));
  return out;
}

}  // namespace wwe2k14::letter_jump
