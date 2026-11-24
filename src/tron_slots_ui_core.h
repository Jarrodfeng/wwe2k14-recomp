// wwe2k14 - S110 TRON-B: the decisions behind the custom-tron screens (docs\research\s110\TRONSLOT.md sites 13-15). Portable, std only
// (on top of src\tron_slots_core.h): the launcher's TRONS tab, the importer form (PC and phone) and the viewer's commands all call THIS, so
// the list a player sees, the number a new tron gets and the text of a problem are one rule. No empty slot is ever listed.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "tron_slots_core.h"

namespace wwe2k14::tron_slots_ui {

namespace core = wwe2k14::tron_slots_core;
namespace fs = std::filesystem;

// ---------------------------------------------------------------- the list a screen shows
// A broken entry and the one line that says how to mend it. Never drawn as a slot.
struct Problem {
  enum Kind { kVideoMissing, kNameMissing, kText } kind = kText;
  int id = 0;  // the tron number (0 for kText)
  std::string text;
};
struct Model {
  std::vector<core::Slot> filled;  // by number: ONLY slots with a line and a video
  std::vector<Problem> problems;
};

inline std::string ExtOf(const fs::path& f) {  // ".mp4", ".alpha.mp4", ...
  const std::string n = f.filename().string();
  const size_t dot = n.find('.');
  return dot == std::string::npos ? std::string() : n.substr(dot);
}

inline Model BuildModel(const fs::path& dir, const core::Library& lib) {
  Model m;
  m.filled = lib.listed;
  for (const core::Slot& s : lib.line_only)
    m.problems.push_back({Problem::kVideoMissing, s.id,
                          s.name + " (number " + std::to_string(s.id) + ") has no video. Add the video again."});
  for (int n : lib.file_only) {
    const std::string file = std::to_string(n) + ExtOf(core::FileFor(dir, n));
    m.problems.push_back({Problem::kNameMissing, n, "The video " + file + " has no name. Give it a name."});
  }
  for (const std::string& t : lib.problems) m.problems.push_back({Problem::kText, 0, t});
  return m;
}

// A .bik2 the game plays on the big screen must be the 320x320 sheet (src\tron_sheet_core.h); an MP4 is laid out by the game's shim.
// Returns "" when fine.
inline std::string SheetWarning(const std::string& ext, int w, int h) {
  if (ext != ".bik2" || (w == 320 && h == 320)) return "";
  return "This .bik2 is " + std::to_string(w) + "x" + std::to_string(h) +
         ". A custom tron needs a 320x320 picture sheet, so it may look wrong. An MP4 works at any size.";
}

// ---------------------------------------------------------------- the decisions
// `copy(id)` puts the chosen video at <dir>\<id>.<ext> (the launcher's AssignTron / the viewer's AssignTron) and returns "" or why not.
using CopyFn = std::function<std::string(int)>;
// (review) Every writer below takes the folder the screen was given; an empty one (the viewer's TronDir() with no mods folder) is refused, or
// dir / "custom.txt" would be the WORKING folder's file.
inline const char* const kNoFolder = "No mods folder.";

// ADD: the lowest free number (never one that has a live line, never a video with no line; a retired one only when no never-used number is
// left: core NextFreeId), the video copied there, then the name saved.
// An empty name becomes "Custom tron N". Returns "" or why not; `*out_id` gets the number.
inline std::string AddSlot(const fs::path& dir, const std::string& name, const CopyFn& copy, int* out_id = nullptr) {
  if (dir.empty()) return kNoFolder;
  core::Library lib = core::ReadSlots(dir);
  const int id = core::NextFreeId(lib.Lines(), lib.file_only);
  if (id < 0) return "All 116 custom tron numbers are used. Remove one first.";
  std::string err = copy(id);
  if (!err.empty()) return err;
  std::vector<core::Slot> lines;
  for (const core::Slot& x : lib.Lines())
    if (x.id != id) lines.push_back(x);  // (only a RETIRED line can share the number: NextFreeId gives one only when none is never-used)
  core::Slot s;
  s.id = id;
  s.name = core::CleanName(name);
  if (s.name.empty()) s.name = "Custom tron " + std::to_string(id);
  lines.push_back(s);
  if (!core::WriteSlots(dir, lines)) {
    for (const char* e : core::kTronExts) {
      std::error_code ec;
      fs::remove(dir / (std::to_string(id) + e), ec);  // no half-added tron: a video with no line is a problem row
    }
    return "Could not save the name list (custom.txt).";
  }
  if (out_id) *out_id = id;
  return "";
}

// RENAME, also the fix for a video that has no name (file_only): the line is added. A number with neither a line nor a video is refused.
inline std::string RenameSlot(const fs::path& dir, int id, const std::string& name) {
  if (dir.empty()) return kNoFolder;
  if (!core::IsSlotId(id)) return "That is not a custom tron number.";
  const std::string clean = core::CleanName(name);
  if (clean.empty()) return "Type a name first.";
  core::Library lib = core::ReadSlots(dir);
  for (const core::Slot& s : lib.retired)
    if (s.id == id) return "That custom tron was removed.";
  std::vector<core::Slot> lines = lib.Lines();
  bool found = false;
  for (core::Slot& s : lines)
    if (s.id == id) s.name = clean, found = true;
  if (!found) {
    if (core::FileFor(dir, id).empty()) return "That custom tron does not exist.";
    core::Slot s;
    s.id = id, s.name = clean;
    lines.push_back(s);
  }
  return core::WriteSlots(dir, lines) ? "" : "Could not save the name list (custom.txt).";
}

// The fix for a name with no video: the video is copied again to the same number (the line stays, so the number keeps its meaning).
inline std::string RefillSlot(const fs::path& dir, int id, const CopyFn& copy) {
  if (dir.empty()) return kNoFolder;
  core::Library lib = core::ReadSlots(dir);
  for (const core::Slot& s : lib.line_only)
    if (s.id == id) return copy(id);
  return "That custom tron is not waiting for a video.";
}

// REMOVE, what goes: every video of the number (each kTronExts extension, not only the one that plays) and its banner files (the launcher's
// RemoveBanner list). Only the files that exist; all inside `dir`. The launcher deletes them (RemoveSlot), the viewer recycles each.
inline std::vector<fs::path> RemovedFiles(const fs::path& dir, int id) {
  std::vector<fs::path> out;
  if (dir.empty() || !core::IsSlotId(id)) return out;
  std::error_code ec;
  const std::string n = std::to_string(id);
  for (const char* e : core::kTronExts)
    if (fs::is_regular_file(dir / (n + e), ec)) out.push_back(dir / (n + e));
  for (const char* e : {".txt", ".mp4", ".mov", ".mkv"})
    if (fs::is_regular_file(dir / (n + ".banner" + e), ec)) out.push_back(dir / (n + ".banner" + e));
  return out;
}
// The line becomes RETIRED (tron_slots_core.h: "#retired<TAB>N<TAB>name", the old name kept): never listed, the number not handed out again
// while a never-used one is left. A number with no line (a video that had no name) is retired too. Retired already: nothing to do.
inline std::string RetireLine(const fs::path& dir, int id) {
  if (dir.empty()) return kNoFolder;
  if (!core::IsSlotId(id)) return "That is not a custom tron number.";
  core::Library lib = core::ReadSlots(dir);
  std::vector<core::Slot> lines = lib.Lines();
  bool found = false;
  for (core::Slot& s : lines)
    if (s.id == id) s.retired = true, s.present = false, s.file.clear(), found = true;
  if (!found) {
    core::Slot s;
    s.id = id, s.name = "Custom tron " + std::to_string(id), s.retired = true;
    lines.push_back(s);
  }
  return core::WriteSlots(dir, lines) ? "" : "Could not save the name list (custom.txt).";
}
// REMOVE: the files above go, then the line is retired. A saved superstar or entrance that holds the number shows an idle big screen; the
// number goes to a new video only when all others are used. Returns "" or why not.
inline std::string RemoveSlot(const fs::path& dir, int id) {
  if (dir.empty()) return kNoFolder;
  if (!core::IsSlotId(id)) return "That is not a custom tron number.";
  std::error_code ec;
  for (const fs::path& f : RemovedFiles(dir, id)) fs::remove(f, ec);
  return RetireLine(dir, id);
}
// RESTORE (explicit only): a retired number gets a video again under its old name. Refused for any number that is not retired.
inline std::string RestoreSlot(const fs::path& dir, int id, const CopyFn& copy) {
  if (dir.empty()) return kNoFolder;
  core::Library lib = core::ReadSlots(dir);
  std::vector<core::Slot> lines = lib.Lines();
  bool found = false;
  for (core::Slot& s : lines)
    if (s.id == id && s.retired) s.retired = false, found = true;
  if (!found) return "That custom tron was not removed.";
  if (std::string err = copy(id); !err.empty()) return err;
  if (core::WriteSlots(dir, lines)) return "";
  std::error_code ec;
  for (const char* e : core::kTronExts) fs::remove(dir / (std::to_string(id) + e), ec);  // (no video under a still-retired line)
  return "Could not save the name list (custom.txt).";
}

// ---------------------------------------------------------------- the importer's TRON choice
// One superstar's tron: his entrance wrestler's (the default), another wrestler's, or a custom slot. Saved as tron_from / tron_slot.
struct Choice {
  enum Kind { kEntrance, kWrestler, kSlot } kind = kEntrance;
  int value = 0;  // the wrestler id / the slot number
};
inline Choice ChoiceOf(int tron_from, int tron_slot) {
  if (tron_slot > 0) return {Choice::kSlot, tron_slot};
  if (tron_from > 0) return {Choice::kWrestler, tron_from};
  return {Choice::kEntrance, 0};
}
inline bool IsFilled(const std::vector<core::Slot>& filled, int id) {
  for (const core::Slot& s : filled)
    if (s.id == id) return true;
  return false;
}
inline void Apply(const Choice& c, int& tron_from, int& tron_slot) {
  tron_from = c.kind == Choice::kWrestler ? c.value : 0;
  tron_slot = c.kind == Choice::kSlot ? c.value : 0;
}
// The text of the closed combo. `wrestler_name(id)` names a wrestler. A saved slot that is not filled any more reads "MISSING: name or number"
// (the value is kept until the player picks another).
inline std::string ChoiceLabel(const Choice& c, const std::vector<core::Slot>& filled, const core::Library* lib,
                               const std::function<std::string(int)>& wrestler_name) {
  switch (c.kind) {
    case Choice::kEntrance: return "ENTRANCE WRESTLER'S";
    case Choice::kWrestler: return "ANOTHER WRESTLER'S: " + wrestler_name(c.value);
    case Choice::kSlot: {
      for (const core::Slot& s : filled)
        if (s.id == c.value) return s.name;
      if (lib) {
        for (const core::Slot& s : lib->line_only)
          if (s.id == c.value) return "MISSING: " + s.name;
        for (const core::Slot& s : lib->retired)  // a removed tron: its old name
          if (s.id == c.value) return "MISSING: " + s.name;
      }
      return "MISSING: " + std::to_string(c.value);
    }
  }
  return "";
}
inline bool IsRetired(const core::Library& lib, int id) {
  for (const core::Slot& s : lib.retired)
    if (s.id == id) return true;
  return false;
}

}  // namespace wwe2k14::tron_slots_ui
