// S110 SEAT-C: what the launchers and the Superstar Importer screen SAY about the library (docs\research\s110\SEAT1.md section 5): one place for
// the sentences, so the PC launcher, the phone launcher and the importer screen read the same, and a unit (tools\importer) can check them.
// Header only, no ImGui, no disk: the state comes in as arguments (importer_core.h reports it; this file only words it).
//
// The words the player sees: "seated" / "not seated" / "library" (never card / frame / seat none).
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

#include "importer_core.h"

namespace importer::screen {

inline std::string Num(int n) { return std::to_string(n); }

// How many of the list hold a seat (slot >= 0). A character past the seats is stored with slot -1.
inline int SeatedCount(const std::vector<Superstar>& v) {
  int n = 0;
  for (const Superstar& s : v) n += s.slot >= 0 ? 1 : 0;
  return n;
}

// "5 imported, 5 of 102 seated"
inline std::string CountLine(int imported, int seated, int seats) { return Num(imported) + " imported, " + Num(seated) + " of " + Num(seats) + " seated"; }
inline std::string CountLine(const std::vector<Superstar>& v, int seats) { return CountLine(int(v.size()), SeatedCount(v), seats); }

// One plain line for a character who has no seat (never an error). `seated` = how many are seated now, `seats` = SeatCount.
// (SEAT-C review: no pronoun: imports are women too; the line also stands for several at once)
inline std::string NotSeatedWhy(int seated, int seats) {
  if (seated >= seats) return "All " + Num(seats) + " seats are taken. Kept in your library, and can be seated when a seat is free.";
  return "Not seated yet: BUILD INTO GAME gives a free seat.";
}

// The order the list shows: seated characters first (by seat), then the ones not seated (by name). Indexes into `v`.
inline std::vector<size_t> DisplayOrder(const std::vector<Superstar>& v) {
  auto low = [](std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  std::vector<size_t> o(v.size());
  for (size_t i = 0; i < o.size(); ++i) o[i] = i;
  std::stable_sort(o.begin(), o.end(), [&](size_t a, size_t b) {
    const bool sa = v[a].slot >= 0, sb = v[b].slot >= 0;
    if (sa != sb) return sa;
    if (sa) return v[a].slot < v[b].slot;
    return low(v[a].name) < low(v[b].name);
  });
  return o;
}

// The one line about the old pack's conversion, from what MigrationState() reports ("" = nothing to say).
inline std::string MigrationLine(Migration m) {
  if (m == Migration::Pending) return "Your imported superstars are converted to the new library when the pack is built. Nothing is lost.";
  // (SEAT-C review: legacy.old\ goes at the first BUILD after a game boot that took the library in (migrate::RemoveHolds), not at the boot itself)
  if (m == Migration::Kept)
    return "Your imported superstars were converted to the new library. Nothing was lost; a copy of the old pack is kept until the game has "
           "loaded the new library once (the next build then removes it).";
  return {};
}

// A problem sentence of PackState::problems for the list: first letter up, no full stop doubled. Nothing in the folder is changed.
inline std::string ProblemLine(std::string p) {
  if (!p.empty()) p[0] = char(std::toupper(static_cast<unsigned char>(p[0])));
  if (!p.empty() && p.back() != '.') p += ".";
  return p;
}

namespace detail {
inline int LeadingNumber(const std::string& s, size_t& end) {
  size_t i = 0;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
  end = i;
  return i ? std::atoi(s.substr(0, i).c_str()) : -1;
}
inline std::string Stars(int n) { return Num(n) + " imported superstar" + (n == 1 ? " was" : "s were"); }
inline std::string Needs(int n) { return n == 1 ? "needs" : "need"; }
}  // namespace detail

// The game's own verdict at its last boot (<mods>\superstars\game_status.txt, the first line) as plain sentences for the player. "" = nothing to say.
//   "in N"                                                      -> ""
//   "in N, M imported superstar(s) need a rebuild: open the importer"
//   "in N, K card(s) left out (see the game's log)"
//   "out N imported superstar(s) need converting: start the game from the launcher once"
//   "out an old pack was found: ..." / "out the pack is not built yet ..." / any other "out <why>"
inline std::string GameStatusSentence(const std::string& raw) {
  using detail::LeadingNumber;
  using detail::Stars;
  size_t e = 0;
  if (raw.rfind("in ", 0) == 0) {
    // "in N" then clauses ", <clause>", each worded on its own, in order (S110 SEAT2 review: the hand-over's clause comes last, after dlc.h's)
    std::string said;
    for (size_t at = raw.find(", "); at != std::string::npos;) {
      const size_t next = raw.find(", ", at + 2);
      const std::string c = raw.substr(at + 2, next == std::string::npos ? std::string::npos : next - at - 2);
      at = next;
      const int n = LeadingNumber(c, e);
      std::string s;
      if (n >= 0 && c.find("need a rebuild") != std::string::npos)
        s = "In your last game " + Stars(n) + " left out and " + detail::Needs(n) + " a rebuild: open the Superstar Importer and BUILD INTO GAME.";
      else if (n >= 0 && c.find("left out") != std::string::npos)
        s = "In your last game " + Num(n) + " superstar" + (n == 1 ? " was" : "s were") +
            " left out (the game's log says why). Build again in the Superstar Importer; send a report if it happens again.";
      else if (n >= 0 && c.find("may show another superstar's saved edits") != std::string::npos)  // (src\slot_handover_core.h kUnseenClause)
        s = "In your last game " + Num(n) + " imported superstar" + (n == 1 ? "" : "s") +
            " may have shown the saved move set, entrance or Threads of another superstar: that save had not been loaded with your library before. "
            "Check the move set and entrance of each imported superstar before saving.";
      if (!s.empty()) said += (said.empty() ? "" : " ") + s;
    }
    return said;
  }
  if (raw.rfind("out ", 0) != 0) return {};
  const std::string why = raw.substr(4);
  const int n = LeadingNumber(why, e);
  if (n >= 0 && why.find("need converting") != std::string::npos)
    return "In your last game " + Stars(n) + " left out: the old pack still needs converting to the new library. Start the game from the launcher "
           "(PLAY) once: the launcher converts the old pack.";
  if (why.rfind("an old pack was found", 0) == 0)
    return "In your last game your imported superstars were left out: the old pack still needs converting to the new library. Start the game from "
           "the launcher (PLAY) once: the launcher converts the old pack.";
  if (why.rfind("the pack is not built yet", 0) == 0)
    return "Your superstars were left out of your last game: they are not built yet. PLAY from the launcher, or BUILD INTO GAME in the Superstar Importer, builds them.";
  std::string w = why;
  while (!w.empty() && (w.back() == '.' || w.back() == ' ')) w.pop_back();
  return "In your last game your superstars were left out: " + w + ". Send a report if it happens again.";
}

}  // namespace importer::screen
