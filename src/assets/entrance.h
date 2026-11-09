// Session 98 (plan step 6): a wrestler's whole ENTRANCE, read from the player's own game files at run time (CLAUDE.md rules 1 and 2:
// nothing is kept or committed). Evidence: out\runtime\s98\entrance.md (the acts), fop.md (the cue end rule), leftovers.md section 4
// (the camera-attached light directions). Section = evt\Entrance0N.pac, the PACH of the wrestler's bank (entrance_light.h LoadSection):
//   leaf 0   act 25000  the body: one motion per cue, named SSSSTTTT with SSSS = the cue (0 / 5 / 10 / 15 / 20), 61 tracks, the `Bip` track
//                       is the root path in ARENA coordinates (x / z at scale 1.0 [C], y unresolved [U])
//   leaf 10  act 25010  the camera: per cue (and take TTTT = 0 / 1; cue 10 also 20 / 21) three tracks: `Camera01` (eye position + rotation),
//                       hash 0x70D146A5 (position only = the aim point), a flags-0x100 float track (the fov channel [U], not used)
//   leaf 11  act 25011  the light rig (entrance_light.h)
//   leaf 12+ act 25012+ rope pairs (not used: optional)
// Time: motion frames of 1/60 s. Body and camera of a cue have the same length [C]. The game starts each cue with blend 0.0 (a hard cut,
// entrance.md section 2) and ends it when the motion's clock reaches the cue length (fop.md, sub_8233DFC8); cue 0 additionally waits
// on the intro video: the viewer holds it for `intro_hold` seconds (a setting, default from run 070, unconfirmed).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "entrance_light.h"
#include "id_names.h"
#include "motion.h"

namespace assets {
namespace entrance {

constexpr uint32_t kHashCamera = 0x5EDFDF5F;  // yanm.node_hash("Camera01") [C]
constexpr uint32_t kHashAim = 0x70D146A5;     // the aim point track [C, entrance.md]

struct Cue {
  int sub = 0;                                // 0 / 5 / 10 / 15 / 20
  int len = 0;                                // motion frames (the body's last key)
  motion::Motion body;                        // act 25000 motion `sub`, track 0
  std::map<int, motion::Motion> cam;          // act 25010: take suffix (0, 1, 20, 21) -> motion
};

// Session 98 (props general): how a prop act's cue motion places the prop, from its `base` track (out\s98\props_general\notes.md rule 3):
//   kPropZero  base position zero in every key: an unused slot, not drawn [C: run 6820, bank 602's 2649 (zero in all cues) - the game drew
//              nothing in Cena's hands but the belt; 80 such motions in the census]
//   kPropHand  every key within kHandReach of the hand: the root in the HAND's frame (PropAttach; keys past kHandReach in a motion that
//              is mostly in reach are "parked" and hidden [U])
//   kPropWorld every key beyond kHandReach: the root in the ENTRANCE's motion space, like the body's `Bip` (PropWorld): carried props
//              whose base follows a performer's Bip path (82 motions), props standing on the stage / ramp (128: the Miz "AWESOME"
//              letters 2767 / 2771 at z 2818, Undertaker's druid torches 2253 in pairs along the ramp, Fandango's DLC 2591 on the
//              stage) and thrown ones (Cena's cap 2719 into the crowd) [C: the paths, notes.md; U: the game's own switch is not found -
//              no EV field separates the two kinds, so the base path decides]
enum PropMode { kPropZero = 0, kPropHand, kPropWorld };
constexpr double kHandReach = 100.0;  // [C: the largest in-hand belt offset in the files is 96.5 (bank 613); world props sit >= 103 from
                                      // the motion origin = the ring canvas centre, so a carried one is at least a hand's height away]
constexpr double kZeroBase = 0.5;     // "zero": bank 139's 2718 keys sit a hair off 0 (the census rounds them to 0); the smallest real hand
                                      // offset in the files is 3 (bank 674's 2773)

struct Show {
  bool ok = false;
  int wrestler = -1;
  std::string file;
  std::vector<Cue> cues;
  entrance_light::Rig rig;   // act 25011 (may be !ok)
  int ropes = 0;             // leaves 12+ present (not played)
  std::vector<std::string> notes;  // motions that could not be read (name: reason)
  // Session 98 (gen check 5): the entrance COMPANION. Run 6821 (Del Rio's bank 416 "Alberto Del Rio with Ricardo", Entrance03.pac):
  // leaf 6 (act 25006) holds a full body motion per cue (566 KB, like leaf 0) and the EV table names character 323 (Ricardo Rodriguez,
  // CH323.PAC loaded right after ENTRANCE03.PAC in the run log) in act 25005's X = 32302 (= id * 100 + 2). In the plain entrances
  // (Orton 161, Cena 139 / 602, Del Rio 655) leaf 6 is a 96-byte stub and acts 25001-25005 carry X = 19202. comp_id = that character
  // (-1 none). [U] what 19202 / act 25005 vs 25006 mean in general (one companion entrance read).
  int comp_id = -1;  // (= comps[0].char_id, -1 none)
  // Session 98 (props general): the COMPANIONS: every ACTOR record (Record::actor) other than leaf 0 whose leaf holds body motions
  // (a `Bip` track) [C: 131 such leaves in 347 banks - 416 leaf 6 Ricardo, 423 leaves 4 / 5 Cameron 354 / Naomi 355, 671 / 672 leaf 6
  // Stephanie 147, 674 leaf 6 Mr. Fuji 250, Entrance07 / 08 partners in leaves 1-5, 616 / 637 / 651 leaf 8]. Who = X / 100, unless X is
  // the bank's TEMPLATE id (the X its motion-less actor records carry: 19202 / 10402 / 13902 / 51200 / 12502) - then char_id = -1 (a
  // body with no named performer, e.g. Beth Phoenix's leaf 8 X 10402, not placed) [U who those are]. The game drops companions in 6-man
  // / tag matches (Jarrod, S98); the viewer plays a single entrance.
  struct Companion {
    int leaf = 0;
    uint32_t x = 0;
    int char_id = -1;
    std::map<int, motion::Motion> by_sub;  // cue sub -> body motion
  };
  std::vector<Companion> comps;
  // Session 98 (gen check 5): the PROP acts: every other leaf (7, 12+) with motions named per cue, act = 25000 + leaf. The title belt plays
  // its own motion here [C: bank 602's leaf 12 (cues 5 / 10 / 15) and leaf 13 (cue 20) hold 35-36 tracks named after the belt's nodes
  // (evtobj 352: AW_01.., bone01..); bank 416's leaf 7 cue 5 likewise; the other leaves' 1-3 tracks are small props (the microphone)].
  // The viewer matches a prop model to the act by node-name hashes (see viewer_main UpdateBelt).
  struct PropAct {
    int leaf = 0;
    uint32_t prop_id = 0;                  // gen check 6: the X of the act's EV record (ActX): the prop id, see PropAttach
    std::map<int, motion::Motion> by_sub;  // cue sub -> motion
    std::map<int, int> mode;               // cue sub -> PropMode of that motion (props general)
  };
  std::vector<PropAct> props;
  // Session 99 (announcer): the RING ANNOUNCER of the entrance = act 25008 (section leaf 8), his microphone = leaf 9 (record X 2038,
  // item 38 = "2038_wwe_mic", evtobj PACH 4; the game draws it in his RIGHT hand although "2" reads left elsewhere: sub_8233C758
  // special-cases 2038 [U how]) [C: the act numbers / X of every bank, census in out\s98\announcer\notes.md; the game's shots
  // 6824_sw_s1_ring / s5_ring show him in a suit in the ring, the microphone in his right hand].
  //   where:   the EV rows whose own act field is 25008 (ActRows): a spot + heading per cue. 291 of 368 banks stand him at (210, ring
  //            height 120, -105) heading 1.641 in every cue; others in the ring elsewhere or on the floor outside it [C: probe run 6824
  //            frames 20436 / 29342 (Del Rio / Kane in the ring): his bones at x 190-245, y 119.., z -88..-140 for that row]
  //   motion:  a cue whose row is NOT enabled (no motion of his in the bank, every plain entrance) plays evtmot.pac's shared act
  //            "25008Announcer_M_def" motion "SSSS0100" with SSSS = 5 * (row / 5) [C: sub_8230B680 fills the act-25008 rows that are not
  //            enabled with act 25008, sub 5 * (row / 5), track 100 for rows 0-14; probe run 6818: Orton's stage / ramp frames fit
  //            00050100 / 00100100 at the cue's own frame to 2 units]; rows 15-20 get act -1 there (the motion runs on) and
  //            sub_82320878 plays {25008, 20, 110} = 00200110 (or {25008, 15, 100}) by a state flag [C: 00200110 fits run 6818's
  //            ring-in / ring frames; U: when it starts - the viewer's AnnouncerLateAt]. The motions are local (Bip at x / z 0, his
  //            feet at y 0): placed at the row's spot, turned by its heading (PlaceAt).
  //            An enabled row = the bank's own leaf-8 motion of that cue, in the entrance's space like the entrant's (616 / 637 / 651
  //            cues 15 / 16 / 20, the announce banks 833-835).
  //   who:     leaf 8's X names him only when leaf 8 has motions and X is not the bank's template id (833-835: 18700 = Justin Roberts
  //            187); else the arena's announcer (the viewer: Justin Roberts 187, Howard Finkel 252 in the WrestleMania arenas -
  //            db\ids\characters.json: CH187 loads with every announcer scene, CH252 in its place in arena 44 [C, run 492]; the other
  //            WrestleMania arenas [U], see ArenaAnnouncer).
  //   absent:  a row with a zero spot that is not enabled (the Entrance08 scene banks 800-824 / 855) [U: the game's own test].
  struct Announcer {
    uint32_t x = 0;      // leaf 8's X
    int char_id = -1;    // the performer leaf 8 names, -1 = the arena's announcer
    uint32_t mic_id = 0; // leaf 9's X (a 2xxx / 3xxx prop id), 0 = none
    struct CueMo {
      motion::Motion mo;       // own = in the entrance's motion space; else local (PlaceAt with pos / heading)
      bool own = false;
      bool late = false;       // a row from 15 on: the motion sub_82320878 starts (00200110), after the earlier one runs on
      double pos[3] = {0, 0, 0}, heading = 0;
      std::string src;         // "leaf 8" | "evtmot 00050100" ..
    };
    std::map<int, CueMo> by_sub;
    std::map<std::string, motion::Motion> shared;  // evtmot's act-25008 motions by name (local; the viewer's A/B "announcer_late")
    std::map<int, motion::Motion> mic_own;  // cue -> the bank's own leaf-9 motion (833-835)
    motion::Motion mic;                     // evtmot's "25009Announcer_M_def" motion (one `base` track: the mic in his hand)
    bool has_mic = false;
  };
  Announcer ann;
};

namespace detail {
inline void SplitName(const std::string& nm, int& sub, int& tr) {
  sub = nm.size() >= 4 ? std::atoi(nm.substr(0, 4).c_str()) : -1;
  tr = nm.size() >= 8 ? std::atoi(nm.substr(4, 4).c_str()) : 0;
}
}  // namespace detail

// Session 98 (gen check 5): the X word of every act of bank `bank`'s EV table (the directory entry named `bank`: '0FOP' / 'EV', records
// at +0x44 of 32 bytes, +8 index (act = 25000 + index - 1), +12 X; big endian; tools\assets\fop.py, out\runtime\s98\fop.md). Empty when
// the table is missing or malformed (untrusted bytes: every read is bounds-checked).
inline bool EvTable(launcher::GameFiles& files, int bank, Bytes& t) {
  for (const std::string& rel : dlc_files::EntranceFiles(files)) {  // Session 98: + the DLC packs' Entrance_dlcPP (dlc_files.h)
    Bytes b;
    std::string e;
    if (!entrance_light::detail::ReadPac(files, rel, b, e)) continue;
    for (size_t o = 0x80C; o + 16 <= 0x4000 && o + 16 <= b.size(); o += 16) {
      const std::string nm = entrance_light::detail::Name8(b.data() + o);
      if (nm.empty() || nm.find_first_not_of("0123456789") != std::string::npos) break;
      if (std::atoi(nm.c_str()) != bank) continue;
      const size_t pos = 0x4000 + size_t(b[o + 8] | (b[o + 9] << 8) | (b[o + 10] << 16) | (uint32_t(b[o + 11]) << 24)) * 0x800;
      if (pos + 16 > b.size()) return false;
      t = pac::Decode(b.data() + pos, b.size() - pos);
      return t.size() >= 0x48 && std::memcmp(t.data(), "0FOP", 4) == 0;
    }
  }
  return false;
}
// The EV table's name (+0x22, e.g. "John_Cena Current", "John_Cena_Champ"); "" when missing.
// Session 98 (props general): a name may run 31 characters, through +0x40 = the HIGH byte of the record count (5 banks: 676 / 677
// "Yokozuna Champ Single ..", 731 "Andre_the_Giant_with_Ted_DiBias", 757 "Randy_Savage_with_Miss_Elizabet", 777 "Primo & Epico & Rosa
// Mendes (CA"; their count reads 0x69000010 ..). So the name ends at +0x41 and the count is the low 24 bits (kEvCountMask) [C: the 5
// tables decode to 16-19 records with the mask, every other bank's count has a zero high byte; out\s98\props_general\notes.md].
constexpr uint32_t kEvCountMask = 0x00FFFFFF;
inline std::string EvName(launcher::GameFiles& files, int bank) {
  Bytes t;
  if (!EvTable(files, bank, t)) return "";
  std::string s;
  for (size_t i = 0x22; i < 0x41 && i < t.size() && t[i]; ++i) s.push_back(char(t[i]));
  return s;
}
// Session 98 (gen check 5): the champion bank named after a wrestler's own entrance: an Entrance06 "<Name>_Champ" section (600-699)
// whose name (letters only, lower case, up to "champ") starts the wrestler's base table name (bank = wrestler id). -1: none.
// [U] NOT the game's choice rule: in run 6821 Del Rio (the save's champion; 655 Alberto_Del_Rio_Champ exists) entered with bank 416
// and the belt on his waist; in run 6820 Cena entered with 602. Used only by the viewer's "title":"auto".
inline int ChampBank(launcher::GameFiles& files, int wrestler) {
  auto norm = [](const std::string& s) {
    std::string o;
    for (char ch : s) if (std::isalpha((unsigned char)ch)) o.push_back(char(std::tolower((unsigned char)ch)));
    return o;
  };
  const std::string me = norm(EvName(files, wrestler));
  if (me.size() < 4) return -1;
  Bytes b;
  std::string e;
  if (!entrance_light::detail::ReadPac(files, "pac/evt/Entrance06.pac", b, e)) return -1;  // (read once: every champ table)
  for (size_t o = 0x80C; o + 16 <= 0x4000 && o + 16 <= b.size(); o += 16) {
    const std::string nm = entrance_light::detail::Name8(b.data() + o);
    if (nm.empty() || nm.find_first_not_of("0123456789") != std::string::npos) break;
    const int id = std::atoi(nm.c_str());
    if (id < 600 || id >= 700) continue;
    const size_t pos = 0x4000 + size_t(b[o + 8] | (b[o + 9] << 8) | (b[o + 10] << 16) | (uint32_t(b[o + 11]) << 24)) * 0x800;
    if (pos + 16 > b.size()) continue;
    const Bytes t = pac::Decode(b.data() + pos, b.size() - pos);
    if (t.size() < 0x48 || std::memcmp(t.data(), "0FOP", 4) != 0) continue;
    std::string s;
    for (size_t i = 0x22; i < 0x41 && t[i]; ++i) s.push_back(char(t[i]));
    const std::string n = norm(s);
    const size_t c = n.find("champ");
    if (c == std::string::npos || c < 4) continue;
    if (me.compare(0, c, n, 0, c) == 0) return id;
  }
  return -1;
}
// Session 98 (props general): one EV record: X and the kind bytes (+16..+23). The kind byte that is set names the act's class
// [C: all 347 banks, every leaf with motions (out\s98\props_general\notes.md rule 1)]: +16 = 1 an ACTOR (a body: the entrant, a
// companion, a partner; X = character * 100 + 2 / 4 / 0 or a template id), +18 = 1 a PROP (X = attach * 1000 + item), +19 / +20 = 1
// a rope act (X 0 / 1 / 2).
struct Record {
  uint32_t x = 0;
  uint8_t kind[8] = {};
  bool actor() const { return kind[0] != 0; }
  bool prop() const { return kind[2] != 0; }
};
inline std::map<int, Record> Records(launcher::GameFiles& files, int bank);
inline std::map<int, uint32_t> ActX(launcher::GameFiles& files, int bank) {
  std::map<int, uint32_t> out;
  for (const auto& [act, r] : Records(files, bank)) out[act] = r.x;
  return out;
}
inline std::map<int, Record> Records(launcher::GameFiles& files, int bank) {
  std::map<int, Record> out;
  for (const std::string& rel : dlc_files::EntranceFiles(files)) {  // Session 98: + the DLC packs' Entrance_dlcPP (dlc_files.h)
    Bytes b;
    std::string e;
    if (!entrance_light::detail::ReadPac(files, rel, b, e)) continue;
    for (size_t o = 0x80C; o + 16 <= 0x4000 && o + 16 <= b.size(); o += 16) {
      const std::string nm = entrance_light::detail::Name8(b.data() + o);
      if (nm.empty() || nm.find_first_not_of("0123456789") != std::string::npos) break;
      if (std::atoi(nm.c_str()) != bank) continue;
      const size_t pos = 0x4000 + size_t(b[o + 8] | (b[o + 9] << 8) | (b[o + 10] << 16) | (uint32_t(b[o + 11]) << 24)) * 0x800;
      if (pos + 16 > b.size()) return out;
      const Bytes t = pac::Decode(b.data() + pos, b.size() - pos);
      auto be32 = [&](size_t q) { return q + 4 <= t.size() ? (uint32_t(t[q]) << 24 | uint32_t(t[q + 1]) << 16 | uint32_t(t[q + 2]) << 8 | t[q + 3]) : 0u; };
      if (t.size() < 0x48 || std::memcmp(t.data(), "0FOP", 4) != 0) return out;
      const uint32_t nrec = std::min<uint32_t>(be32(0x40) & kEvCountMask, 64);
      // Session 98 (gen check 6): record index N carries the X of act 25000 + N = section leaf N (it was read as act 25000 + N - 1).
      // [C: bank 416's prop leaves name a track "<X>_1" after their record's X (hash inversion: 76ED2E22 = "2774_1" in leaves 12 / 13 =
      // records 12 / 13 X 2774; 462C2E22 = "2410_1" in leaves 15 / 16 = records 15 / 16; E88BB069 = "3038_wwe_mic_01" in leaf 14 =
      // record 14 X 3038); the ropes (leaves 17 / 18 / 19, heights 34 / 76 / 118) = records 17-19 X 0 / 1 / 2; the companion's body
      // (leaf 6) = record 6 X 32302 (Ricardo 323)].
      for (uint32_t r = 0; r < nrec; ++r) {
        const size_t q = 0x44 + 32 * size_t(r);
        const uint32_t idx = be32(q + 8);
        if (idx >= 64 || q + 24 > t.size()) continue;
        Record rc;
        rc.x = be32(q + 12);
        std::memcpy(rc.kind, t.data() + q + 16, 8);
        out[int(25000 + idx)] = rc;
      }
      return out;
    }
  }
  return out;
}

// ---- Session 99 (announcer) ---------------------------------------------------------------------------------------------------------
constexpr int kAnnouncerLeaf = 8, kAnnouncerMicLeaf = 9;
// One EV row (212 bytes; tools\assets\fop.py Row): +2 enabled, +4 act, +8 sub, +16 / +20 / +24 x y z (file space, -Y up), +28 heading.
struct ActRow {
  int sub = 0;
  bool en = false;
  double pos[3] = {0, 0, 0};
  double heading = 0;
};
// The rows of bank `bank` whose OWN act field is `act`, by sub. (The records' X words are indexed by leaf, while each record's row
// block carries the act of the leaf before it: record 9 (X 2038, the mic) holds the act-25008 rows [C: every bank - record k's rows say
// act 25000 + k - 1 and are enabled exactly in the cues where leaf k - 1 has motions: 416's 25006 / 25007, 616's 25008 in 15 / 16 / 20].
// So the rows are found by their act field, not by the record.) Empty when the table is missing.
inline std::map<int, ActRow> ActRows(launcher::GameFiles& files, int bank, int act) {
  std::map<int, ActRow> out;
  Bytes t;
  if (!EvTable(files, bank, t)) return out;
  auto be32 = [&](size_t q) { return q + 4 <= t.size() ? (uint32_t(t[q]) << 24 | uint32_t(t[q + 1]) << 16 | uint32_t(t[q + 2]) << 8 | t[q + 3]) : 0u; };
  auto bef = [&](size_t q) {
    const uint32_t v = be32(q);
    float f;
    std::memcpy(&f, &v, 4);
    return double(f);
  };
  const uint32_t nrec = std::min<uint32_t>(be32(0x40) & kEvCountMask, 64);
  for (uint32_t r = 0; r < nrec; ++r) {
    const size_t q = 0x44 + 32 * size_t(r);
    if (q + 8 > t.size()) break;
    const uint32_t nrows = std::min<uint32_t>(uint32_t(t[q]) << 8 | t[q + 1], 64), off = be32(q + 4);
    for (uint32_t k = 0; k < nrows; ++k) {
      const size_t o = size_t(off) + 8 + 212 * size_t(k);
      if (o + 32 > t.size()) break;
      if (be32(o + 4) != uint32_t(act)) continue;
      ActRow w;
      w.sub = int(be32(o + 8));
      w.en = t[o + 2] != 0;
      for (int i = 0; i < 3; ++i) w.pos[i] = bef(o + 16 + 4 * size_t(i));
      w.heading = bef(o + 28);
      if (w.sub >= 0 && w.sub < 1000 && std::isfinite(w.heading) && std::isfinite(w.pos[0]) && std::isfinite(w.pos[1]) && std::isfinite(w.pos[2]))
        out[w.sub] = w;
    }
  }
  return out;
}
// The shared announcer act of pac\evt\evtmot.pac: the entry whose name table names "25008..." (p0 entry 6, "25008Announcer_M_def":
// motions 00000100 / 00050100 / 00100100 / 00150100 / 00200100 / 00200110 + act 25009's 00000100 = the mic) [C: the file]. The bytes
// are untrusted (pac.h bounds-checks every entry).
inline bool AnnouncerAct(launcher::GameFiles& files, motion::Act& out, std::string& err) {
  pac::Pac p;
  if (!pac::Open(files, "pac/evt/evtmot.pac", p, err)) return false;
  for (const pac::Pach& ph : p.pachs)
    for (const pac::Ent& e : ph.ents) {
      if (e.off + 4 > p.bytes.size() || std::memcmp(p.bytes.data() + e.off, "PACH", 4) == 0) continue;  // (nested tables: not an act)
      Bytes blk = pac::Decode(p.bytes.data() + e.off, e.size);
      if (blk.size() < 0x28) continue;
      const uint32_t end_names = Be32(blk.data()), name_off = Be32(blk.data() + 8);
      if (end_names > blk.size() || name_off > end_names) continue;
      bool hit = false;
      for (size_t o = name_off; o + 4 + 6 <= end_names && o < name_off + 40 * 4; o += 40)
        if (std::memcmp(blk.data() + o + 4, "25008", 5) == 0 && !(blk[o + 9] >= '0' && blk[o + 9] <= '9')) hit = true;
      if (hit) return motion::ParseAct(std::move(blk), out) ? true : (err = "evtmot.pac: the announcer act does not parse", false);
    }
  return err = "evtmot.pac: no act 25008", false;
}
// Put a LOCAL motion (root at x / z 0, feet at the motion's own floor) at an EV row's spot: the Bip path and the leg targets turned by
// `ang` about the file's Y axis and moved by `pos` (file space, -Y up: the row's y -120 = the ring canvas, like the entrant's in-ring
// Bip y ~1-4); the Bip rotation turned by the same angle. [C: the spot (probe 6824); the sign of the turn: out\s98\announcer\notes.md]
inline void PlaceAt(motion::Motion& m, const double pos[3], double ang) {
  const uint32_t bip = motion::NodeHash("Bip");
  const motion::Quat qh = motion::AxisQuat(1, ang);
  const double qa[4] = {qh[0], qh[1], qh[2], qh[3]};
  const motion::M3 R = motion::MFromQuat(qa);
  auto turn = [&](double* v) {
    const double x = v[0], y = v[1], z = v[2];
    for (int r = 0; r < 3; ++r) v[r] = R[size_t(r * 3)] * x + R[size_t(r * 3 + 1)] * y + R[size_t(r * 3 + 2)] * z + pos[r];
  };
  for (motion::Track& tr : m.tracks) {
    if (tr.hash == bip) {
      if (tr.has_pos) {
        for (motion::PosKey& k : tr.pos) turn(k.p);
        turn(tr.pos_c);
      }
      if (!tr.rot.empty()) tr.rot.insert(tr.rot.begin(), std::vector<motion::RotKey>{motion::RotKey{0, qh}});
    }
    for (auto& k : tr.ik) {
      double v[3] = {k[1], k[2], k[3]};
      turn(v);
      k[1] = v[0], k[2] = v[1], k[3] = v[2];
    }
  }
}
// The arena's ring announcer when the bank names none: Howard Finkel (252) in the WrestleMania arenas 28-55 (the 30 Years of
// WrestleMania tab, db\ids\arenas.json), else Justin Roberts (187) [C: arena 44 only - CH252 loads in CH187's place there (run 492,
// db\ids\characters.json id 252); U: arenas 28-43 / 45-55, never run - extended from the evidence line "Jarrod: Howard Finkel is the
// ring announcer in the WrestleMania arenas" in that entry (characters.json line 803), not seen first-hand].
// (review) The range is data, not code: db\ids\arenas.json "announcer" (+ "announcer_evidence" / "announcer_confidence" per arena:
// 44 high, the others unknown) -> id_names.h kArenaAnnouncer (tools\assets\gen_id_names.py).
inline int ArenaAnnouncer(int arena) { return ArenaAnnouncerId(arena, 187); }

// Session 98 (gen check 6): only the light rig (act 25011, leaf 11) of entrance bank `bank`. An entrance played from ANOTHER bank (a
// champion section of Entrance06, a companion bank like 416) is lit by the WRESTLER's own bank's rig, not the played bank's [C: probe
// run 6820 frames 5950 / 5951, Cena in bank 602: 22 distinct colours of bank 139's rig appear in the frame's constants (e.g. slot 0
// light 2 (0.722, 0.878, 0.980), light 1 (0.392, 0.510, 0.613)), 8 of 602's - all values 602 shares with 139 - and none of 602's own
// greens (pools 54 / 55 (0.04, 0.94, 0.24), sky (0.24, 0.71, 0.29)); the 602 rig made the viewer's stage floor green where the game's
// is warm white (139's slot 1 sky (0.96, 0.98, 0.67)). Run 6821 (Del Rio, bank 416 vs his own 322): the two rigs are identical.]
inline entrance_light::Rig LoadRig(launcher::GameFiles& files, int bank, std::string& err) {
  entrance_light::Rig rig;
  rig.wrestler = bank;
  Bytes sec;
  pac::Pach ph;
  std::string file;
  if (!entrance_light::LoadSection(files, bank, 11, sec, ph, file, err)) return rig;
  motion::Act lit;
  if (!motion::ParseAct(pac::Get(sec, ph, 11), lit)) return err = file + ": no act 25011 leaf", rig;
  entrance_light::RigFromAct(lit, rig);
  rig.file = file;
  return rig;
}

inline Show Load(launcher::GameFiles& files, int wrestler, std::string& err) {
  Show s;
  s.wrestler = wrestler;
  Bytes sec;
  pac::Pach ph;
  if (!entrance_light::LoadSection(files, wrestler, 0, sec, ph, s.file, err)) return s;
  motion::Act body;
  if (!motion::ParseAct(pac::Get(sec, ph, 0), body)) return err = s.file + ": no act 25000 leaf", s;
  {
    std::string all = "body leaf motions (" + std::to_string(body.yanm.size()) + " YANM): ";
    for (const std::string& n : body.motion_names) all += n + " ";
    s.notes.push_back(all);
  }
  for (size_t i = 0; i < body.motion_names.size(); ++i) {
    int sub, tr;
    detail::SplitName(body.motion_names[i], sub, tr);
    if (tr != 0 || sub < 0) continue;
    motion::Motion mo = motion::ParseMotion(body, i);
    if (!mo.unsupported.empty()) { s.notes.push_back("body " + body.motion_names[i] + ": " + mo.unsupported); continue; }
    Cue c;
    c.sub = sub, c.len = mo.length, c.body = std::move(mo);
    s.cues.push_back(std::move(c));
  }
  std::sort(s.cues.begin(), s.cues.end(), [](const Cue& a, const Cue& b) { return a.sub < b.sub; });
  if (s.cues.empty()) return err = s.file + ": act 25000 has no readable cue motion", s;
  motion::Act cam;
  if (motion::ParseAct(pac::Get(sec, ph, 10), cam)) {
    for (size_t i = 0; i < cam.motion_names.size(); ++i) {
      int sub, tr;
      detail::SplitName(cam.motion_names[i], sub, tr);
      if (sub < 0) continue;
      motion::Motion mo = motion::ParseMotion(cam, i);
      if (!mo.unsupported.empty()) { s.notes.push_back("camera " + cam.motion_names[i] + ": " + mo.unsupported); continue; }
      for (Cue& c : s.cues)
        if (c.sub == sub) c.cam[tr] = std::move(mo);
    }
  }
  motion::Act lit;
  s.rig.wrestler = wrestler;
  if (motion::ParseAct(pac::Get(sec, ph, 11), lit)) {
    entrance_light::RigFromAct(lit, s.rig);
    s.rig.file = s.file;
  }
  // Session 98 (props general): companions and props by the EV record's class (Record::actor / prop), in any leaf but the body (0),
  // the camera (10) and the light rig (11). (Before: the companion only in leaf 6, by "X % 100 == 2 and not 19202"; props from leaf 7.)
  const std::map<int, Record> recs = Records(files, wrestler);
  const uint32_t bip = motion::NodeHash("Bip"), base_h = motion::NodeHash("base");
  std::vector<uint32_t> templ;  // the X of actor records whose leaf holds no body motion = the bank's template ids
  std::vector<Show::Companion> bodies;
  Show::Companion ann_leaf;  // Session 99: leaf 8's own body motions (the announcer's, when the bank has any)
  for (uint32_t leaf = 1; leaf < 40; ++leaf) {
    if (leaf == 10 || leaf == 11) continue;
    auto rit = recs.find(int(25000 + leaf));
    if (rit == recs.end()) continue;
    const Record& rc = rit->second;
    motion::Act pa;
    const bool has = pac::Find(ph, leaf) && motion::ParseAct(pac::Get(sec, ph, leaf), pa);
    if (rc.actor()) {
      Show::Companion cp;
      cp.leaf = int(leaf), cp.x = rc.x;
      for (size_t i = 0; has && i < pa.motion_names.size(); ++i) {
        int sub, tr;
        detail::SplitName(pa.motion_names[i], sub, tr);
        if (tr != 0 || sub < 0) continue;
        motion::Motion mo = motion::ParseMotion(pa, i);
        if (!mo.unsupported.empty()) { s.notes.push_back("companion leaf " + std::to_string(leaf) + " " + pa.motion_names[i] + ": " + mo.unsupported); continue; }
        if (mo.Find(bip)) cp.by_sub[sub] = std::move(mo);
      }
      if (cp.by_sub.empty()) templ.push_back(rc.x);
      else if (leaf == uint32_t(kAnnouncerLeaf)) ann_leaf = std::move(cp);  // Session 99: the announcer's own motions, not a companion
      else bodies.push_back(std::move(cp));
      if (leaf == uint32_t(kAnnouncerLeaf)) s.ann.x = rc.x;
      continue;
    }
    if (leaf == uint32_t(kAnnouncerMicLeaf) && rc.prop()) {  // Session 99: the announcer's microphone (his hand, not the entrant's)
      if (rc.x >= 1000 && rc.x < 10000) s.ann.mic_id = rc.x;
      for (size_t i = 0; has && i < pa.motion_names.size(); ++i) {
        int sub, tr;
        detail::SplitName(pa.motion_names[i], sub, tr);
        if (tr != 0 || sub < 0) continue;
        motion::Motion mo = motion::ParseMotion(pa, i);
        if (mo.unsupported.empty() && mo.Find(base_h)) s.ann.mic_own[sub] = std::move(mo);
      }
      continue;
    }
    if (!rc.prop() || rc.x < 1000 || rc.x >= 10000 || !has) continue;  // (ropes: X 0 / 1 / 2, not drawn)
    Show::PropAct p;
    p.leaf = int(leaf), p.prop_id = rc.x;
    for (size_t i = 0; i < pa.motion_names.size(); ++i) {
      int sub, tr;
      detail::SplitName(pa.motion_names[i], sub, tr);
      if (tr != 0 || sub < 0) continue;
      motion::Motion mo = motion::ParseMotion(pa, i);
      if (!mo.unsupported.empty() || mo.tracks.empty()) continue;
      int mode = kPropZero;  // (no base track: nothing places it; never seen in the files)
      if (const motion::Track* tb = mo.Find(base_h)) {
        double mx = 0, mn = 1e30, dmin = 1;
        motion::Quat q0{0, 0, 0, 1};
        motion::SampleRot(*tb, 0, q0);
        const int step = std::max(1, mo.length / 64);
        for (int f = 0;; f += step) {
          const int ff = std::min(f, mo.length);
          double q[3] = {0, 0, 0};
          motion::SamplePos(*tb, ff, q);
          const double l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
          mx = std::max(mx, l), mn = std::min(mn, l);
          motion::Quat r{0, 0, 0, 1};
          motion::SampleRot(*tb, ff, r);
          dmin = std::min(dmin, std::fabs(r[0] * q0[0] + r[1] * q0[1] + r[2] * q0[2] + r[3] * q0[3]));
          if (ff >= mo.length) break;
        }
        // zero = the position stays at 0 AND the rotation never moves (1 deg) AND nothing else animates (<= 2 tracks): 91 such motions;
        // Bret Hart's 2077 in cue 21 (0.45 off, turning 3.8 deg) is held. A still zero base under hundreds of node tracks (412's 2762,
        // 413's 2414: no evtobj model either) = a rig whose own nodes carry the motion: placed in the motion space [U].
        const bool still = mx < kZeroBase && dmin > 0.99996;
        mode = still ? (mo.tracks.size() <= 2 ? kPropZero : kPropWorld) : mn > kHandReach ? kPropWorld : kPropHand;
      }
      p.mode[sub] = mode;
      p.by_sub[sub] = std::move(mo);
    }
    if (!p.by_sub.empty()) s.props.push_back(std::move(p));
  }
  for (Show::Companion& cp : bodies) {
    const bool is_templ = std::find(templ.begin(), templ.end(), cp.x) != templ.end();
    const int who = int(cp.x / 100);
    // (review fix) `who` must name a real character: one in our id tables (db\ids\characters.json + dlc_characters.json via
    // id_names.h) OR one whose own file pac\ch\chNNN.pac exists on the disc (the tables are incomplete: Edge 110 and 27 more
    // Entrance07 / 08 partner ids are missing from them; the file is the rule, the table only a name). Else not placed, one note.
    char ch_rel[32];
    std::snprintf(ch_rel, sizeof ch_rel, "pac\\ch\\ch%03d.pac", who);
    const bool known = *CharacterName(who) != 0 || (who > 0 && who < 1000 && files.Exists(ch_rel));
    cp.char_id = (!is_templ && cp.x >= 10000 && who != wrestler && known) ? who : -1;
    if (cp.char_id < 0)
      s.notes.push_back("companion leaf " + std::to_string(cp.leaf) + " X " + std::to_string(cp.x) + ": " +
                        (is_templ ? "the bank's template id" : !known ? "names no known character" : "no named performer") + ", not placed");
    s.comps.push_back(std::move(cp));
  }
  for (const Show::Companion& cp : s.comps)
    if (cp.char_id > 0) { s.comp_id = cp.char_id; break; }
  {  // Session 99 (announcer): see Show::Announcer
    Show::Announcer& an = s.ann;
    if (!ann_leaf.by_sub.empty()) {
      const int who = int(an.x / 100);
      char ch_rel[32];
      std::snprintf(ch_rel, sizeof ch_rel, "pac\\ch\\ch%03d.pac", who);
      const bool is_templ = std::find(templ.begin(), templ.end(), an.x) != templ.end();
      const bool known = *CharacterName(who) != 0 || (who > 0 && who < 1000 && files.Exists(ch_rel));
      an.char_id = (!is_templ && an.x >= 10000 && who != wrestler && known) ? who : -1;
    }
    const std::map<int, ActRow> rows = ActRows(files, wrestler, 25000 + kAnnouncerLeaf);
    motion::Act shared;
    std::string aerr;
    bool have_shared = false;
    for (const Cue& c : s.cues) {
      auto rw = rows.find(c.sub);
      if (rw == rows.end()) continue;
      const ActRow& row = rw->second;
      Show::Announcer::CueMo cm;
      auto own = ann_leaf.by_sub.find(c.sub);
      if (own != ann_leaf.by_sub.end()) {
        cm.mo = own->second, cm.own = true, cm.src = "leaf 8";
      } else if (row.en) {
        continue;  // (enabled with no motion of his in the bank: never seen in the files) [U]
      } else {
        if (std::fabs(row.pos[0]) + std::fabs(row.pos[1]) + std::fabs(row.pos[2]) < 1e-3) continue;  // absent (scene banks) [U]
        if (!have_shared && aerr.empty()) {
          have_shared = AnnouncerAct(files, shared, aerr);
          if (!have_shared) s.notes.push_back("announcer: " + aerr);
        }
        if (!have_shared) continue;
        const int q = 5 * (c.sub / 5);
        char nm[16];
        std::snprintf(nm, sizeof nm, "%04d0100", q);
        // rows 15-20: act -1 (sub_8230B680), sub_82320878 plays {25008, 20, 110} = "00200110" (or {25008, 15, 100}) [C: probe run 6818,
        // Orton ring-in / ring: 00200110 fits the game's bones at 10 / 8 units (best phase), 00150100 / 00200100 at no phase better than
        // 22 / 19 (out\s98\announcer\notes.md)]; until it starts he keeps the earlier motion (the viewer: AnnouncerLateAt)
        if (q >= 15) std::snprintf(nm, sizeof nm, "00200110"), cm.late = true;
        size_t idx = SIZE_MAX;
        for (size_t i = 0; i < shared.motion_names.size(); ++i) if (shared.motion_names[i] == nm) { idx = i; break; }
        if (idx == SIZE_MAX) { s.notes.push_back(std::string("announcer: evtmot has no motion ") + nm); continue; }
        motion::Motion mo = motion::ParseMotion(shared, idx);
        if (!mo.unsupported.empty() || !mo.Find(bip)) { s.notes.push_back(std::string("announcer: evtmot ") + nm + " not readable"); continue; }
        cm.mo = std::move(mo), cm.src = std::string("evtmot ") + nm;
        for (int i = 0; i < 3; ++i) cm.pos[i] = row.pos[i];
        cm.heading = row.heading;
      }
      an.by_sub[c.sub] = std::move(cm);
    }
    // (review) the shared act also when only the mic needs it (a cue with no own leaf-9 motion falls back to it, per cue)
    if (!have_shared && aerr.empty() && an.mic_id != 0 && !an.by_sub.empty()) {
      have_shared = AnnouncerAct(files, shared, aerr);
      if (!have_shared) s.notes.push_back("announcer: " + aerr);
    }
    // the microphone: act 25009's motion (one `base` track) of the shared act (the viewer uses it in a cue with no own leaf-9 motion);
    // the shared body motions by name (first of each name = act 25008's)
    for (size_t i = 0; have_shared && i < shared.yanm.size(); ++i) {
      motion::Motion mo = motion::ParseMotion(shared, i);
      if (!mo.unsupported.empty()) continue;
      if (mo.tracks.size() == 1 && mo.Find(base_h)) {
        if (!an.has_mic) an.mic = std::move(mo), an.has_mic = true;
      } else if (mo.Find(bip) && !an.shared.count(mo.name)) {
        an.shared[mo.name] = std::move(mo);
      }
    }
  }
  for (uint32_t id = 12; id < 24; ++id)
    if (pac::Find(ph, id)) ++s.ropes;
  s.ok = true;
  return s;
}

// ---- the timeline -----------------------------------------------------------------------------------------------------------------------

struct Timeline {
  std::vector<double> start, len;  // per cue, motion frames (len = the time the cue lasts: cue 0 includes the intro hold)
  double total = 0;
};
inline Timeline Build(const Show& s, double intro_hold_sec) {
  Timeline t;
  double at = 0;
  for (const Cue& c : s.cues) {
    double l = c.len;
    if (c.sub == 0) l += std::max(0.0, intro_hold_sec) * 60.0;  // (held on the last pose: the cue's clock stops at its length)
    t.start.push_back(at), t.len.push_back(l);
    at += l;
  }
  t.total = at;
  return t;
}
// The cue index and the cue-local motion frame at scene time `t` (motion frames); past the end = the last frame of the last cue.
inline void Locate(const Show& s, const Timeline& tl, double t, size_t& cue, double& f) {
  cue = 0;
  for (size_t i = 0; i < tl.start.size(); ++i)
    if (t >= tl.start[i]) cue = i;
  f = std::max(0.0, std::min(double(s.cues[cue].len), t - tl.start[cue]));  // (clamped: held after the last key)
}

// The root (`Bip` position) of cue `c` at local frame f: motion space (x, y up, z), cm. False when the track has no position.
inline bool Root(const Cue& c, double f, double p[3]) {
  const motion::Track* tr = c.body.Find(motion::NodeHash("Bip"));
  return tr && motion::SamplePos(*tr, f, p);
}

// (Session 98 motion rule: the ankle targets are not shifted. The `Bip` sample is a delta on the bind Bip translation (0, -96.55, 0)
// like every position track (motion.h PoseOpts::root_delta, sub_823D9A40 0x823DA06C), which puts the hip a leg above the targets;
// the earlier per-wrestler target shift IkDy is gone.)

// "chain" joins: the game integrates root motion as deltas (sub_823D9A40 stores the sampled delta, cutscene_motion.md) and cue 0's row has
// no placement, so a cue should start where the previous one ended. Orton's cues already join exactly; bank 109's miss by 27 / 87 units
// in the file. off[i] = the shift of cue i's Bip path (and the leg targets, which follow the world path) so it starts at the previous
// cue's (shifted) end. Cue 0 is the hidden pyro pose and cue 5 the first shown one: both stay as authored. [U: the hypothesis]
// Session 98 (gen check 6): REFUTED for bank 416 - the game plays cue 20 on the motion's own path although cue 15 starts 124 units off
// cue 10's end (probe run 6821, viewer_main DoEntrance "join"). The viewer's default is "absolute"; this stays for the A/B.
inline std::vector<std::array<double, 3>> ChainOffsets(const Show& s) {
  std::vector<std::array<double, 3>> off(s.cues.size(), std::array<double, 3>{0, 0, 0});
  for (size_t i = 1; i < s.cues.size(); ++i) {
    if (s.cues[i - 1].sub == 0) continue;  // (a join from cue 0 is a teleport: the hidden pose to the first shown cue)
    double e[3], b[3];
    if (!Root(s.cues[i - 1], s.cues[i - 1].len, e) || !Root(s.cues[i], 0, b)) continue;
    off[i][0] = e[0] + off[i - 1][0] - b[0];  // x / z only: y is a height (see Shift / root_y in the viewer)
    off[i][2] = e[2] + off[i - 1][2] - b[2];
  }
  return off;
}
inline void Shift(motion::Motion& m, const std::array<double, 3>& o) {
  const uint32_t bip = motion::NodeHash("Bip");
  for (motion::Track& tr : m.tracks) {
    if (tr.hash == bip && tr.has_pos) {
      for (motion::PosKey& k : tr.pos)
        for (int a = 0; a < 3; ++a) k.p[a] += o[size_t(a)];
      for (int a = 0; a < 3; ++a) tr.pos_c[a] += o[size_t(a)];
    }
    for (auto& k : tr.ik) k[1] += o[0], k[2] += o[1], k[3] += o[2];
  }
}

// ---- Session 98 (gen check 6): the PROPS of an entrance --------------------------------------------------------------------------------
// A prop act (Show::props) names its prop by the record's X (ActX): X = attach * 1000 + item, and the evtobj.pac model whose node is
// named "<X>" or "<X>_..." is the prop (2774 -> PACH 231, 3038 -> 301 "3038_wwe_mic_02", 2410 -> 66 / 68, 2009 -> 357, 3009 -> 407,
// 5009 -> 459 / 482, 2038 -> 4 "2038_wwe_mic"). attach: 2 = the LEFT hand, 3 = the RIGHT hand, 5 = the waist (Bip Spine). [C: run 6821
// cue 5 f301 / cue 10 f301 - Ricardo's 2774 (leaf 12) and 2410 (leaf 15) in his Bip L Hand, his microphone 3038 (leaf 14) in his Bip R
// Hand; run 6820 - Cena's title (bank 602 leaf 12, X 2009) in his Bip L Hand; run 6821 - Del Rio's 5009 waist belt rigid on Bip Spine.
// Item 9 = the title belt: the game draws the champion's own belt (352 for Cena), not the generic 2009 model.]
// The motion's `base` track (hash 0C5879E5 = "base") is the prop's root in the hand's frame:
//   prop root (file space) = Hand * T(o) * S * Base(f),  S = diag(1, -1, -1),  o = (-1.48, +-0.105, 6.55) (+ left, - right)
// [C: rotation exact (1e-3) in all three Ricardo cases; o from them to 0.01; Cena's belt at f372: rotation 1.7 deg, position 2.1 off
// (o was measured on Ricardo's hands)]. The other tracks animate the prop's own nodes (the belt's strap: within 0.2-4 units of the
// game's measured bends at f372, belt_pose.h). [U] who carries a prop (the viewer: the companion when there is one, else the entrant);
// [U] when a prop is shown (a base far from the hand = parked off screen: hidden past kPropParked).
constexpr uint32_t kHashBase = 0x0C5879E5;  // yanm.node_hash("base") [C]
constexpr double kPropParked = kHandReach;  // (props general: was 60, which hid in-hand belt keys at 60-96 - banks 612 / 613 / 633 / 649)
inline int PropAttachKind(uint32_t prop_id) { return int(prop_id / 1000); }  // 2 left hand, 3 right hand, 5 waist
// The prop root relative to the hand bone (file space, column vectors, 3x4 row-major R | t) at cue-local frame f. False: no base track,
// or parked (|base| > kPropParked).
inline bool PropAttach(const motion::Motion& mo, double f, bool right_hand, float out[12]) {
  const motion::Track* tb = mo.Find(kHashBase);
  if (!tb) return false;
  double p[3] = {0, 0, 0};
  motion::Quat q{0, 0, 0, 1};
  motion::SamplePos(*tb, f, p);
  motion::SampleRot(*tb, f, q);
  const double len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
  if (len > kPropParked) return false;
  // (props general: a motion whose base is zero in every key is an unused slot - Show::PropAct::mode kPropZero, never posed; a single
  // zero key inside a hand motion is the hand's own origin and is drawn)
  const double qq[4] = {q[0], q[1], q[2], q[3]};
  const motion::M3 R = motion::MFromQuat(qq);
  const double S[3] = {1, -1, -1}, o[3] = {-1.48, right_hand ? -0.105 : 0.105, 6.55};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) out[r * 4 + c] = float(S[r] * R[size_t(r * 3 + c)]);
    out[r * 4 + 3] = float(o[r] + S[r] * p[r]);
  }
  return true;
}
// Session 98 (props general): a kPropWorld motion's root in the entrance's motion space (file space, -Y up, 3x4 R | t) at frame f: the
// base sample as it is (no bind offset: a prop has no default-pose node). The caller puts it where the body's root space goes (the
// entrance origin, the canvas-height rule and the raised-stage lift; viewer_main UpdateProps). False: no base track.
inline bool PropWorld(const motion::Motion& mo, double f, float out[12]) {
  const motion::Track* tb = mo.Find(kHashBase);
  if (!tb) return false;
  double p[3] = {0, 0, 0};
  motion::Quat q{0, 0, 0, 1};
  motion::SamplePos(*tb, f, p);
  motion::SampleRot(*tb, f, q);
  const double qq[4] = {q[0], q[1], q[2], q[3]};
  const motion::M3 R = motion::MFromQuat(qq);
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) out[r * 4 + c] = float(R[size_t(r * 3 + c)]);
    out[r * 4 + 3] = float(p[r]);
  }
  return true;
}

// ---- Session 98 (timeline bar): what each cue DOES, named from where its root walks ----------------------------------------------------
// Zones, in this priority: RING (the canvas, |x| and |z| <= 318.8 + 12), the arena's stage / ramp rectangles (spots.h, pac entry 50003:
// light set 200 = stage, 201 = ramp; 20 units of margin), RINGSIDE (inside 700 units of the ring's centre), else FLOOR. The cue's root path
// (Bip x / z with the chain offset) is sampled every 4 frames; the zones in order of appearance name the cue. [U: the thresholds]
enum Zone { kZoneFloor = 0, kZoneStage, kZoneRamp, kZoneRingside, kZoneRing };
struct ZoneRect {
  float x0 = 0, z0 = 0, x1 = 0, z1 = 0;
  int kind = kZoneStage;  // kZoneStage | kZoneRamp
};
inline const char* ZoneName(int z) {
  static const char* const n[] = {"FLOOR", "STAGE", "RAMP", "RINGSIDE", "RING"};
  return n[z < 0 || z > 4 ? 0 : z];
}
inline int ZoneAt(double x, double z, const std::vector<ZoneRect>& rects) {
  if (std::fabs(x) <= 330.8 && std::fabs(z) <= 330.8) return kZoneRing;
  int best = -1;
  for (const ZoneRect& r : rects) {
    const double m = 20;
    if (x >= std::min(r.x0, r.x1) - m && x <= std::max(r.x0, r.x1) + m && z >= std::min(r.z0, r.z1) - m && z <= std::max(r.z0, r.z1) + m)
      if (best < 0 || r.kind == kZoneStage) best = r.kind;  // (a stage rect wins over an overlapping ramp rect)
  }
  if (best >= 0) return best;
  {  // the walkway: the ramp rects are only the light zones (bg05: z 850..1900) while the path runs the whole way from the stage (z >= 2900)
    // to the ringside; between them, inside the ramp rects' x range, is still RAMP (+Z = toward the stage, spots.h). [U: the corridor]
    double rx0 = 1e30, rx1 = -1e30, sz = -1e30;
    for (const ZoneRect& r : rects) {
      if (r.kind == kZoneRamp) rx0 = std::min<double>(rx0, std::min(r.x0, r.x1)), rx1 = std::max<double>(rx1, std::max(r.x0, r.x1));
      else sz = sz < -1e29 ? std::min(r.z0, r.z1) : std::min<double>(sz, std::min(r.z0, r.z1));
    }
    if (rx0 < rx1 && sz > -1e29 && x >= rx0 - 20 && x <= rx1 + 20 && z > 700 && z < sz) return kZoneRamp;
  }
  return std::max(std::fabs(x), std::fabs(z)) < 700 ? kZoneRingside : kZoneFloor;
}
struct Segment {
  std::string label;                          // INTRO | STAGE | RAMP | RING IN | IN RING | ...
  std::vector<int> zones;                     // the zones the root passes, in order of appearance
  double a[3] = {0, 0, 0}, b[3] = {0, 0, 0};  // the root at the cue's first / last frame (motion space, chain offset applied)
};
inline std::vector<Segment> Segments(const Show& s, const std::vector<std::array<double, 3>>& off, const std::vector<ZoneRect>& rects) {
  std::vector<Segment> out;
  for (size_t i = 0; i < s.cues.size(); ++i) {
    const Cue& c = s.cues[i];
    Segment g;
    const double o[3] = {i < off.size() ? off[i][0] : 0.0, 0.0, i < off.size() ? off[i][2] : 0.0};
    for (int f = 0; f <= c.len + 3; f += 4) {
      const double ff = std::min<double>(f, c.len);
      double p[3];
      if (!Root(c, ff, p)) break;
      if (f == 0) for (int k = 0; k < 3; ++k) g.a[k] = p[k] + o[k];
      for (int k = 0; k < 3; ++k) g.b[k] = p[k] + o[k];
      const int z = ZoneAt(p[0] + o[0], p[2] + o[2], rects);
      if (g.zones.empty() || g.zones.back() != z) g.zones.push_back(z);
    }
    for (size_t k = 1; k + 1 < g.zones.size();)  // a FLOOR blip between two other zones is noise
      if (g.zones[k] == kZoneFloor && g.zones[k - 1] != kZoneFloor) g.zones.erase(g.zones.begin() + (std::ptrdiff_t)k);
      else ++k;
    if (c.sub == 0) g.label = "INTRO";  // the hidden pyro pose + the intro video wait
    else if (g.zones.empty()) g.label = "CUE " + std::to_string(c.sub);
    else {
      const int first = g.zones.front(), last = g.zones.back();
      if (first == last) g.label = first == kZoneRing ? "IN RING" : first == kZoneFloor ? "ARENA FLOOR" : ZoneName(first);
      else if (last == kZoneRing) g.label = "RING IN";
      else g.label = std::string(ZoneName(first)) + " TO " + ZoneName(last);
    }
    out.push_back(std::move(g));
  }
  return out;
}

// The take (a suffix of the camera motions): `take` = the suffix itself when present, else an index into the available ones.
inline const motion::Motion* CameraMotion(const Cue& c, int take) {
  if (c.cam.empty()) return nullptr;
  auto it = c.cam.find(take);
  if (it != c.cam.end()) return &it->second;
  size_t k = size_t(std::max(0, take)) % c.cam.size();
  it = c.cam.begin();
  std::advance(it, (std::ptrdiff_t)k);
  return &it->second;
}

struct EntCam {
  bool ok = false;
  double eye[3] = {0, 0, 0}, aim[3] = {0, 0, 0};  // file space (-Y up, like the Bip y here: the caller negates y)
  motion::Quat q{0, 0, 0, 1};                     // Camera01's rotation key
  bool has_q = false;
  double fov = 0;  // vertical FOV in degrees from the fov track's 3rd component (radians; run 6816), 0 = no track
  // A camera without a rotation key (the viewer's own): its axes in file space (-Y up) for the camera-attached lights.
  bool has_axes = false;
  double right[3] = {1, 0, 0}, up[3] = {0, -1, 0}, fwd[3] = {0, 0, 1};
};
constexpr uint32_t kHashFov = 0x70D304A4;  // the flags-0x100 3-float track of act 25010 (entrance.md)
inline EntCam Camera(const Cue& c, int take, double f) {
  EntCam r;
  const motion::Motion* mo = CameraMotion(c, take);
  if (!mo) return r;
  const motion::Track* te = mo->Find(kHashCamera);
  const motion::Track* ta = mo->Find(kHashAim);
  if (!te || !ta || !motion::SamplePos(*te, f, r.eye) || !motion::SamplePos(*ta, f, r.aim)) return r;
  r.has_q = motion::SampleRot(*te, f, r.q);
  if (const motion::Track* tf = mo->Find(kHashFov)) {
    double v[3];
    if (motion::SamplePos(*tf, f, v) && v[2] > 0.05 && v[2] < 3.0) r.fov = v[2] * 57.29577951308232;
  }
  r.ok = true;
  return r;
}

// Entrance light directions are camera-attached (leftovers.md section 4): dir_world = R_cam^T * dir_key, R_cam = the 3x3 of Camera01's
// rotation key (probe: swap 6728, residuals 0.0-0.5 deg). `variant` picks the convention while it is being proven (entrance_info
// "light_probe" prints every one against the game's numbers): bit 0 = R instead of R^T, bit 1 = the look-at basis of eye / aim (+Y
// forward, no roll) instead of the rotation key, bit 2 = conjugated by the Y mirror (S R S, S = diag(1,-1,1)).
// (Run 6816, analysis.md 3: the rig's directions d are VIEW-space (x right, y up, z forward), so the world direction is V^T d, which in the
// file's own space (-Y up) is dir_file = R * (d.x, d.z, -d.y) with R = the 3x3 of Camera01's rotation key (columns: right, forward, -up);
// 26 frames matched with zero residual. A camera without a key (the viewer's own free / orbit camera, a cutaway) gives its axes:
// dir_file = right * d.x + up * d.y + fwd * d.z, the same thing. `variant` is unused now.)
inline bool RotateDir(const EntCam& cp, int /*variant*/, const double d[3], double o[3]) {
  if (cp.has_axes) {
    for (int k = 0; k < 3; ++k) o[k] = cp.right[k] * d[0] + cp.up[k] * d[1] + cp.fwd[k] * d[2];
    return true;
  }
  if (!cp.has_q) return false;
  const double q[4] = {cp.q[0], cp.q[1], cp.q[2], cp.q[3]};
  const motion::M3 R = motion::MFromQuat(q);
  const double v[3] = {d[0], d[2], -d[1]};
  for (size_t r = 0; r < 3; ++r) o[r] = R[r * 3 + 0] * v[0] + R[r * 3 + 1] * v[1] + R[r * 3 + 2] * v[2];
  return true;
}
inline bool RotateDirs(LightSet& ls, const EntCam& cp, int variant) {
  LightSet t = ls;
  for (int i = 0; i < 4; ++i) {
    const double d[3] = {ls.dir[i][0], ls.dir[i][1], ls.dir[i][2]};
    double o[3];
    if (!RotateDir(cp, variant, d, o)) return false;
    for (int k = 0; k < 3; ++k) t.dir[i][k] = float(o[k]);
  }
  {  // Session 98 (skin fix): the highlight light (light 4) is camera-attached the same way
    const double d[3] = {ls.hl_dir[0], ls.hl_dir[1], ls.hl_dir[2]};
    double o[3];
    if (RotateDir(cp, variant, d, o))
      for (int k = 0; k < 3; ++k) t.hl_dir[k] = float(o[k]);
  }
  ls = t;
  return true;
}

}  // namespace entrance
}  // namespace assets
