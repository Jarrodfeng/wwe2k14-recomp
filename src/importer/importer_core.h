// Session 98: the Superstar Importer's core - builds ONE mod DLC pack ("pack 6") holding every imported character, from the
// player's own game files + DLC packs + the community character pacs he imports (CLAUDE.md rule 1: built on his device,
// never committed or shipped; mods are additive: everything goes under <mods>\superstars\, nothing is written anywhere
// else). Portable C++ (PC + Android), runs inside the viewer process. Implementation: importer_core.cpp; formats:
// docs\formats\catalog_dlc.md, chetc_roster_record.md, roster_images.md, string_pac.md; the game side: src\superstars.h.
//
// <mods>\superstars\
//   pack.json          our manifest (LoadPack / BuildPack): the characters as the UI set them up
//   renders\<slot>_A.png, _B.png, _C.png   the UI's portraits (SSFA / SSFB / SSFC), kept for the next build and LoadPack
//   sources\           community pacs copied in by ExtractSource (the zip members)
//   pack6\             the built pack (info\catalog.dlc + arcs, pac\..., movies\titantron\...): what the game mounts
//   superstars.txt     what the game reads at boot (pack id, the packs it was built on, slot -> music id)
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../launcher/disc_image.h"  // launcher::GameFiles

namespace importer {

// A community character pac (or one inside a zip), as found.
struct SourceInfo {
  bool ok = false;
  std::string err;          // why not usable (ok = false)
  int claimed_id = -1;      // the character id its EMD names claim (Mr Kennedy: 102, a Triple H replacement)
  int attires = 0;          // attires with an in-ring model (EMD 000<id><a>2)
  int bip_bones = 0;        // distinct "Bip..." bone names in attire 0's body model (0 = motions will not drive it)
  std::vector<std::string> warnings;
  std::string entry_name;      // InspectSources: the member's path inside the zip ("" for a plain pac)
  std::string suggested_name;  // from the file name, cleaned ("Mr_Kennedy_09.pac" -> "Mr Kennedy")
};
SourceInfo InspectSource(const std::string& pac_or_zip_path);  // a pac, or the FIRST character pac inside a zip
// Every character pac inside a zip (nested folders too; char_mods.h's zip limits and path checks), or the one pac. Members
// that are not character pacs come back with ok = false and the reason.
std::vector<SourceInfo> InspectSources(const std::string& zip_or_pac_path);
// Copies one member of a zip (entry_name from InspectSources; "" or a plain pac path = the file itself) into
// <mods>\superstars\sources\ and returns that copy's path (use it as Superstar.source_pac); "" + err on failure.
std::string ExtractSource(const std::string& zip_path, const std::string& entry_name, const std::string& mods_dir,
                          std::string* err = nullptr);

struct Superstar {
  int slot = -1;               // the character id he gets (397..499, from FreeSlots)
  std::string name, short_name;  // UTF-8; written as text 60000 + slot (name), 62000 + slot (short), 64000 + slot (long = name)
  int base_id = 100;           // the wrestler he is built on: roster record (moveset, ratings, class), default The Rock
  int entrance_from = -1;      // whose entrance / music / tron he uses; -1 = the base's
  bool copy_ratings = true;    // true = the base's 14 ratings; false = every rating 75 (an average default)
  std::string source_pac;      // the community pac (a .pac file; ExtractSource for zip members)
  std::vector<uint8_t> render_rgba[3];  // SSFA, SSFB, SSFC as the UI made them (RGBA8); empty = copy the base's renders
  int render_w[3] = {0, 0, 0}, render_h[3] = {0, 0, 0};
};
inline constexpr int kMaxStars = 103;  // the mod pack's slot cap (397..499)

struct PackState {
  std::vector<Superstar> stars;
  bool enabled = true;       // kept in pack.json for the UI only: the game's one switch is the launcher's USE MY SUPERSTARS
  bool needs_build = false;  // the list changed since the last BuildPack (SavePack keeps the flag; BuildPack clears it)
};

// Reads <mods>\superstars\pack.json (+ the portraits in renders\). A missing manifest = an empty state, true. needs_build is
// also set when there are characters but no built pack.
bool LoadPack(const std::string& mods_dir, PackState& out, std::string& err);
// Writes pack.json + renders\ WITHOUT building (the list as the UI has it: REMOVE, an edit not built yet).
bool SavePack(const std::string& mods_dir, const PackState& st, std::string& err);
// The DLC the game gets, as everywhere below (Session 101): dlc_dir = the player's DLC folder, "" when USE MY DLC is off; dlc_off = the
// launcher's switched-off pack ids ("3,5"). The same view the launchers give the game (--wwe2k14_dlc_dir / --wwe2k14_dlc_off).
//
// "" when the built pack fits that DLC (or nothing is built yet); else why the game would leave it out (src\importer\pack_fit.h
// Problem: a pack it was built on is not offered, another top roster pack, a different copy of it), as the middle of a sentence.
std::string PackProblem(const std::string& mods_dir, const std::string& dlc_dir, const std::string& dlc_off = "");
// PackProblem() is not empty.
bool PackOutOfDate(const std::string& mods_dir, const std::string& dlc_dir, const std::string& dlc_off = "");

// Session 101 (lane 199): what a launcher must do before PLAY with USE MY SUPERSTARS on.
struct PlayCheck {
  int stars = 0;          // characters in pack.json
  bool build = false;     // build now: the list changed / never built / the pack no longer fits the DLC (`why` says which)
  std::string why;        // one sentence for the player ("" = nothing to do)
};
PlayCheck CheckBeforePlay(const std::string& mods_dir, const std::string& dlc_dir, const std::string& dlc_off = "");
// The game's own verdict at its last boot (<mods>\superstars\game_status.txt, written by src\dlc.h): "" when it took the pack in (or
// never said), else one sentence for the player.
std::string LastGameProblem(const std::string& mods_dir);

// Writes <mods>\superstars\pack6\ (the whole pack, rebuilt from scratch into pack6.building, then swapped in), pack.json,
// renders\ and superstars.txt; needs_build is written false. Built on the packs the game gets (dlc_dir / dlc_off above; none = the
// disc's own files; else the highest roster pack, folder or Xbox 360 package). A character whose slot is no longer free (a pack
// added since) or used twice is moved to a free slot (logged; pack.json follows). progress(0..1, what) is called along the way. Up
// to 103 characters (slots 397..499); each gets a type-1 catalog entry.
bool BuildPack(const std::string& mods_dir, const launcher::GameFiles& game, const std::string& dlc_dir, const PackState& st,
               std::function<void(float, const char*)> progress, std::string& err, const std::string& dlc_off = "");
// 397..499 minus kReservedSlots and the ids any pack in the DLC folder uses (catalog entries' characters, chNNN_dlcPP pacs; switched
// off or not). Our own pack's slots are NOT removed (the UI knows its own characters; a rebuild reuses them).
std::vector<int> FreeSlots(const launcher::GameFiles& game, const std::string& dlc_dir);

// For tests / the UI's log: what the last BuildPack did, one line per step.
const std::vector<std::string>& LastBuildLog();
// Session 101 (review S5b): the last successful BuildPack's slot moves, one sentence each for the player ("X moved from slot A to B
// (saves that used him may show someone else)."); empty = none.
std::vector<std::string> LastBuildMoves();

constexpr int kFirstSlot = 397, kLastSlot = 499;
// Session 101: ids in 397..499 a real 2K14 pack uses, never given out (whether or not this player has the pack): 399 = pack 4's
// "SuperStar Pack" bundle entry (catalog char id; docs\systems\dlc.md item 0, Jarrod's four packs).
constexpr int kReservedSlots[] = {399};
// The wrestlers a character may be based on / take his entrance from (roster ids; BuildPack refuses others): for the pickers.
constexpr int kMinBase = 100, kMaxBase = kFirstSlot - 1;

}  // namespace importer
