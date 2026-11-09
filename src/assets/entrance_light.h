// Session 97 (entrance light): a wrestler's ENTRANCE lighting, read from the player's own game files at run time (CLAUDE.md
// rules 1 and 2: nothing from the game is kept or committed). Evidence and numbers: out\runtime\s97\entrance_light.md,
// reference decoder tools\assets\entrance_light.py.
//
// Source [C, live trace run 070 + probe swap 6728]: evt\Entrance0N.pac. Its directory (EVP, 16-byte entries {name[8], LE u32
// start in 2 KB sectors from 0x4000, LE u32 size in 256 B}) lists the wrestler banks; the BASE banks (id < 1000, e.g. 161 =
// Orton) are followed, after the directory's last byte, by one "BPE " section per base bank IN THE SAME ORDER (header: tag,
// version, LE u32 packed size at +8, expanded size at +12; sections are padded to 2 KB: next = pos + align(packed + 16)).
// A section = a PACH; its leaf 11 is the motion act 25011 of that wrestler ("25011NNN_A11_def"; NNN = the bank id): a name
// table plus one YANM per motion named SSSSTTTT (decimal sub SSSS, track TTTT), the format motion.h reads.
//   sub = 10 * cue + 11 + slot, cue in {0,5,10,15,20} ([S+8] of sub_8233B418 = sub_82316E50 = the entrance phase), slot 0..8.
//   track 0 = sky colour, 1 = ground colour (flags 0x0100: float RGB keys), 10..14 = lights 0..4: a b14 rotation (the light's
//   direction) + a flags-0x0100 RGB key track (colour, may exceed 1.0). Tracks 50..59 (sub with extra rigs) are not used here.
// The game (sub_8233AF98 / sub_827F6508) then REMAPS light-set ids of the arena to these slots (value = pool index):
//   slot 0 -> id 10;  slot 1 -> 50 and every 1xx / 2xx / 3xx / 5xx;  slot 2 -> 20, 21;  slot 3 -> 30, 31, 32;
//   slot 4 -> 14;  slot 5 -> 11;  slot 6 -> 12;  slot 7 -> 13;  slots 4..8 absent in a pac fall back to slot 0.
// Numeric check (Orton, cue 0, slot 1, t = 68): the 15 numbers sky / ground / light 0..2 colours equal the live draw tuple of
// swap 6728 (sum of abs differences 0.004). Time unit = the motion's frames (key times 0, 16, 32 ...); [U] which cue / time
// the game is at when (the phase counter's schedule is not decoded): the caller chooses them.
// [U] light DIRECTIONS: dir = R(q) * (0, 1, 0) reproduces one probe direction (light 0 of the example, 0.6/0.8) but not the
// other two, so the directions are provisional.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "asset_model.h"
#include "dlc_files.h"
#include "motion.h"
#include "pac.h"

namespace assets {
namespace entrance_light {

struct Rig {
  bool ok = false;
  int wrestler = -1;
  std::string file;                        // e.g. "pac/evt/Entrance00.pac"
  std::map<std::pair<int, int>, motion::Motion> m;  // (sub, track) -> motion
  // Session 99 (Kane's red light): (sub, track) -> the motion's loop {start S, end T} (its YAT2 chunk, LoopEnd below); a motion
  // without a loop is absent and HOLDS its last key past its end (LoopTime).
  std::map<std::pair<int, int>, std::pair<int, int>> loop_end;
  std::vector<int> cues;                   // cues present (sub = 10 cue + 11 + slot)
  // Session 98 (arena fix 2): the entrance FILM FILTER, drawn over the finished picture (VS 032E8FAC, flat colour, alpha blended):
  // rgb / 255, alpha / 128 of the record `00 c6 0c 05 R G B A` that ends the slot-2 ground motions [C: probe run 6816, Orton (161)
  // (186, 142, 54, 10) = c1 (0.729, 0.557, 0.212, 0.078) in all his frames, Cena (166) (32, 88, 148, 8) = (0.125, 0.345, 0.580, 0.063)].
  bool film = false;
  float film_rgba[4] = {0, 0, 0, 0};
  // Session 98 (gen check 6): the record sits inside each cue's slot-2 motion (sub 10 cue + 13; Orton's 4 copies, Cena 139's 2 at cues
  // 0 / 5), not only before the leaf's 'POF0' tail: Cena's record (32, 88, 148, 8) is followed by more motion data, so the old tail-only
  // search missed it, while probe run 6820 (frames 5950 / 5951) draws exactly that film [C]. Some rigs carry a different record per cue
  // (348: cue 0 ff4b0f1e, cue 5 1e0a0032, cue 20 e51f1a1e): cue -> rgba here; which one the game shows when is [U] (the viewer takes the
  // newest cue <= the current one; film_rgba = the first cue's).
  std::map<int, std::array<float, 4>> film_cue;
  const float* FilmFor(int cue) const {
    const float* best = film ? film_rgba : nullptr;
    for (const auto& [c, v] : film_cue)
      if (c <= cue) best = v.data();
    return best;
  }
};

namespace detail {
inline bool ReadPac(launcher::GameFiles& files, const std::string& rel, Bytes& out, std::string& err) {
  Bytes raw;
  if (!files.Exists(rel)) return err = "missing " + rel, false;
  if (!files.Stream(rel, [&](const uint8_t* p, size_t n) { raw.insert(raw.end(), p, p + n); })) return err = "read failed " + rel, false;
  std::string e2;
  if (!wwe2k14::xctd::Unpack(raw, out, e2)) return err = rel + ": " + e2, false;
  return true;
}
inline std::string Name8(const uint8_t* p) {
  std::string s(reinterpret_cast<const char*>(p), 8);
  while (!s.empty() && (s.back() == ' ' || s.back() == 0)) s.pop_back();
  return s;
}
}  // namespace detail

inline std::pair<int, int> LoopEnd(const motion::Act& act, size_t i);  // (below: a motion's loop start / end events)

// Loads the wrestler's act 25011 (all motions). `wrestler` = the bank id (character id, e.g. 161 for Randy Orton).
inline Rig Load(launcher::GameFiles& files, int wrestler, std::string& err) {
  Rig rig;
  rig.wrestler = wrestler;
  for (const std::string& rel : dlc_files::EntranceFiles(files)) {  // Session 98: + the DLC packs' Entrance_dlcPP (dlc_files.h)
    if (rig.ok) break;
    Bytes b;
    std::string e;
    if (!detail::ReadPac(files, rel, b, e)) continue;
    // directory
    std::vector<int> base;
    size_t last = 0;
    for (size_t o = 0x80C; o + 16 <= 0x4000 && o + 16 <= b.size(); o += 16) {
      const std::string nm = detail::Name8(b.data() + o);
      if (nm.empty() || nm.find_first_not_of("0123456789") != std::string::npos) break;
      const uint32_t st = Le32(b, o + 8), sz = Le32(b, o + 12);
      last = std::max(last, size_t(0x4000) + size_t(st) * 0x800 + size_t(sz) * 256);
      if (std::atoi(nm.c_str()) < 1000) base.push_back(std::atoi(nm.c_str()));
    }
    int idx = -1;
    for (size_t i = 0; i < base.size(); ++i)
      if (base[i] == wrestler) idx = int(i);
    if (idx < 0) continue;
    size_t pos = (last + 0x7FF) & ~size_t(0x7FF);
    while (pos + 16 <= b.size() && std::memcmp(b.data() + pos, "BPE ", 4) != 0) pos += 0x800;  // (the gap before the first section)
    for (int i = 0; i < idx && pos + 16 <= b.size(); ++i) {
      if (std::memcmp(b.data() + pos, "BPE ", 4) != 0) {
        err = rel + std::string(": section walk lost");
        return rig;
      }
      pos += (size_t(Le32(b, pos + 8)) + 16 + 0x7FF) & ~size_t(0x7FF);
    }
    if (pos + 16 > b.size() || std::memcmp(b.data() + pos, "BPE ", 4) != 0) continue;
    const Bytes sec = pac::Decode(b.data() + pos, b.size() - pos);
    pac::Pach ph;
    if (sec.empty() || !pac::ParsePach(sec, 0, sec.size(), ph)) {
      err = std::string(rel) + ": section is not a PACH";
      continue;
    }
    Bytes leaf = pac::Get(sec, ph, 11);
    motion::Act act;
    if (leaf.empty() || !motion::ParseAct(std::move(leaf), act)) {
      err = std::string(rel) + ": no act 25011 leaf";
      continue;
    }
    for (size_t i = 0; i < act.motion_names.size(); ++i) {
      const std::string& nm = act.motion_names[i];
      if (nm.size() != 8) continue;
      const int sub = std::atoi(nm.substr(0, 4).c_str()), tr = std::atoi(nm.substr(4, 4).c_str());
      motion::Motion mo = motion::ParseMotion(act, i);
      if (!mo.unsupported.empty()) continue;
      rig.m[{sub, tr}] = std::move(mo);
      const auto le = LoopEnd(act, i);  // (Session 99: loop or hold, as RigFromAct)
      if (le.second > 0) rig.loop_end[{sub, tr}] = le;
    }
    for (int cue = 0; cue <= 20; cue += 5)
      if (rig.m.count({10 * cue + 11, 0})) rig.cues.push_back(cue);
    rig.file = rel;
    rig.ok = !rig.m.empty();
  }
  if (!rig.ok && err.empty()) err = "wrestler " + std::to_string(wrestler) + " has no entrance light act";
  return rig;
}

// Session 98 (entrance.h): the wrestler's whole entrance section (a PACH; leaf N = act 25000 + N: 0 body, 10 camera, 11 lights,
// 12.. ropes). Found in evt\Entrance0N.pac the same way Load finds it (Load itself is unchanged). `need_leaf` = a leaf the section
// must hold to count (-1: none).
inline bool LoadSection(launcher::GameFiles& files, int wrestler, int need_leaf, Bytes& sec, pac::Pach& ph, std::string& file, std::string& err) {
  for (const std::string& rel : dlc_files::EntranceFiles(files)) {  // Session 98: + the DLC packs' Entrance_dlcPP (dlc_files.h)
    Bytes b;
    std::string e;
    if (!detail::ReadPac(files, rel, b, e)) continue;
    std::vector<int> base;
    size_t last = 0;
    for (size_t o = 0x80C; o + 16 <= 0x4000 && o + 16 <= b.size(); o += 16) {
      const std::string nm = detail::Name8(b.data() + o);
      if (nm.empty() || nm.find_first_not_of("0123456789") != std::string::npos) break;
      const uint32_t st = Le32(b, o + 8), sz = Le32(b, o + 12);
      last = std::max(last, size_t(0x4000) + size_t(st) * 0x800 + size_t(sz) * 256);
      if (std::atoi(nm.c_str()) < 1000) base.push_back(std::atoi(nm.c_str()));
    }
    int idx = -1;
    for (size_t i = 0; i < base.size(); ++i)
      if (base[i] == wrestler) idx = int(i);
    if (idx < 0) continue;
    size_t pos = (last + 0x7FF) & ~size_t(0x7FF);
    while (pos + 16 <= b.size() && std::memcmp(b.data() + pos, "BPE ", 4) != 0) pos += 0x800;
    for (int i = 0; i < idx && pos + 16 <= b.size(); ++i) {
      if (std::memcmp(b.data() + pos, "BPE ", 4) != 0) return err = std::string(rel) + ": section walk lost", false;
      pos += (size_t(Le32(b, pos + 8)) + 16 + 0x7FF) & ~size_t(0x7FF);
    }
    if (pos + 16 > b.size() || std::memcmp(b.data() + pos, "BPE ", 4) != 0) continue;
    Bytes s = pac::Decode(b.data() + pos, b.size() - pos);
    pac::Pach p;
    if (s.empty() || !pac::ParsePach(s, 0, s.size(), p)) {
      err = std::string(rel) + ": section is not a PACH";
      continue;
    }
    if (need_leaf >= 0 && pac::Get(s, p, uint32_t(need_leaf)).empty()) {
      err = std::string(rel) + ": no leaf " + std::to_string(need_leaf);
      continue;
    }
    sec = std::move(s);
    ph = std::move(p);
    file = rel;
    err.clear();
    return true;
  }
  if (err.empty()) err = "wrestler " + std::to_string(wrestler) + " has no entrance section";
  return false;
}

// The film filter record (Rig::film) in the raw act 25011 leaf: the last `... 00 01 00 00 00 c6 0c 05 R G B A 00 00 00 00 'POF0'`.
inline void FindFilm(const Bytes& b, Rig& rig) {
  static const uint8_t pre[] = {0x00, 0x01, 0x00, 0x00, 0x00, 0xc6, 0x0c, 0x05};
  for (size_t o = 0; o + 20 <= b.size(); ++o) {
    if (std::memcmp(b.data() + o, pre, 8) != 0 || std::memcmp(b.data() + o + 16, "POF0", 4) != 0) continue;
    for (int k = 0; k < 3; ++k) rig.film_rgba[k] = b[o + 8 + size_t(k)] / 255.f;
    rig.film_rgba[3] = b[o + 11] / 128.f;
    rig.film = true;
  }
}

// Session 98 (gen check 6): the film record of every slot-2 motion (sub 10 cue + 13), searched inside that motion's bytes (Rig::film_cue).
inline void FindFilmPerCue(const motion::Act& act, Rig& rig) {
  static const uint8_t pre[] = {0x00, 0x01, 0x00, 0x00, 0x00, 0xc6, 0x0c, 0x05};
  const Bytes& b = act.blk;
  for (size_t i = 0; i < act.yanm.size() && i < act.motion_names.size(); ++i) {
    const std::string& nm = act.motion_names[i];
    if (nm.size() != 8) continue;
    const int sub = std::atoi(nm.substr(0, 4).c_str());
    if (sub < 11 || (sub - 11) % 10 != 2) continue;  // slot 2 only
    const int cue = (sub - 13) / 10;
    const size_t end = std::min(b.size(), i + 1 < act.yanm.size() ? act.yanm[i + 1] : b.size());
    for (size_t o = act.yanm[i]; o + 12 <= end; ++o) {
      if (std::memcmp(b.data() + o, pre, 8) != 0) continue;
      std::array<float, 4> v{b[o + 8] / 255.f, b[o + 9] / 255.f, b[o + 10] / 255.f, b[o + 11] / 128.f};
      rig.film_cue[cue] = v;
      break;
    }
  }
  if (!rig.film && !rig.film_cue.empty()) {
    const auto& v = rig.film_cue.begin()->second;
    for (int k = 0; k < 4; ++k) rig.film_rgba[k] = v[size_t(k)];
    rig.film = true;
  }
}

// Session 99 (Kane's red light): the motion's event chunk. Every motion of act 25011 is 'YANM' + 'POF0' + 'YAT2' + 'POF0' [C: the
// bytes of every rig in Entrance00-0N]. YAT2 = u16 1, u16 count, u32 entry offset, u32 data offset, entries {u16 time, u16 data
// offset, u16 command count, u16 0}, then the commands {u8 a, u8 code, u8 size (4 / 8 / 12 bytes), ...} (the film record
// `00 c6 0c 05 R G B A` is one). A loop = a START `00 57 ..` at time S and an END `01 00 ..` at time T: The Rock's / Daniel Bryan's
// cue-5 slot-0 motions (on the tracks that carry them; the others are constant or end on their own) with S = 0 and T = their last
// key 160 / 100 (the measured loops, run 6819), Kane's cyclic cue-10 motions (300); S > 0 in 300+ motions (Kane 107 sub 64 tracks
// 0 / 1: S 150, T 390; Triple H 102; banks 348 / 600 / 628 / 642 / 734 cue 5). Kane's cue-5 fade-in has no events and the game
// HOLDS its last key (run 071 f402 and run 6824 f526: sky (1, 0, 0) = the key at 360, not t = 402 mod 359) [C probe]. The game's
// light-set object (update sub_82417D98) applies nothing once its key stream is past the end (sub_823D0A48 returns -1) and its
// event parser sub_82417AA0 has commands that rewrite the frame counter (a jump) [C listing; that `01 00` is that jump: U].
// Returns {S, T} (S = the first start, 0 without one; T = the first end after S, or after 0), T = -1: no loop (hold).
inline std::pair<int, int> LoopEnd(const motion::Act& act, size_t i) {
  const std::pair<int, int> none{0, -1};
  if (i >= act.yanm.size()) return none;
  const Bytes& b = act.blk;
  size_t p = act.yanm[i];
  auto chunk = [&](const char* tag) -> long {  // length of the chunk at p with this tag, -1 if not
    if (p + 8 > b.size() || std::memcmp(b.data() + p, tag, 4) != 0) return -1;
    const size_t n = Be32(b.data() + p + 4);
    return p + 8 + n <= b.size() ? long(n) : -1;
  };
  long n = chunk("YANM");
  if (n < 0) return none;
  p += 8 + size_t(n);
  if ((n = chunk("POF0")) < 0) return none;
  p += 8 + size_t(n);
  if ((n = chunk("YAT2")) < 12) return none;
  const uint8_t* y = b.data() + p + 8;
  const size_t len = size_t(n);
  const unsigned cnt = Be16(y + 2);
  const size_t eo = Be32(y + 4), pool = Be32(y + 8);
  int start = -1;
  std::vector<int> ends;
  for (unsigned k = 0; k < cnt; ++k) {
    const size_t e = eo + 8 * size_t(k);
    if (e + 8 > len) break;
    const int time = Be16(y + e);
    size_t d = pool + Be16(y + e + 2);
    for (unsigned c = 0, nc = Be16(y + e + 4); c < nc; ++c) {  // every command of the entry, by its size byte
      if (d + 4 > len) break;
      const size_t sz = y[d + 2];
      if (sz < 4 || d + sz > len) break;
      if (y[d] == 0x00 && y[d + 1] == 0x57 && start < 0) start = time;
      if (y[d] == 0x01 && y[d + 1] == 0x00) ends.push_back(time);
      d += sz;
    }
  }
  const int s = start < 0 ? 0 : start;
  for (int t : ends)  // the first end after the start (an end at 0 hides the real one: bank 482 sub 61 track 12 ends at 0 and 120)
    if (t > s) return {s, t};
  return none;
}

// Fills a rig from the parsed act 25011 (the motions named SSSSTTTT).
inline void RigFromAct(const motion::Act& act, Rig& rig) {
  FindFilm(act.blk, rig);
  FindFilmPerCue(act, rig);
  for (size_t i = 0; i < act.motion_names.size(); ++i) {
    const std::string& nm = act.motion_names[i];
    if (nm.size() != 8) continue;
    const int sub = std::atoi(nm.substr(0, 4).c_str()), tr = std::atoi(nm.substr(4, 4).c_str());
    motion::Motion mo = motion::ParseMotion(act, i);
    if (!mo.unsupported.empty()) continue;
    rig.m[{sub, tr}] = std::move(mo);
    const auto le = LoopEnd(act, i);
    if (le.second > 0) rig.loop_end[{sub, tr}] = le;
  }
  for (int cue = 0; cue <= 20; cue += 5)
    if (rig.m.count({10 * cue + 11, 0})) rig.cues.push_back(cue);
  rig.ok = !rig.m.empty();
}

// Slot of the rig that the game remaps the arena light-set `id` to (-1: not remapped).
inline int SlotForSet(int id) {
  if (id == 10) return 0;
  // Session 98 (arena fix): set 502 (the lights-off placeholder of the truss lamps / glow sheets, `a z l(502)` slots) is NOT remapped:
  // in every entrance frame of probe run 6816 (8341-12241) its draws keep c0-c3 = 0, c181 = c182 = 0.615 (the key's own values) [C].
  if (id == 502) return -1;
  if (id == 50 || (id >= 100 && id < 600 && id / 100 != 4)) return 1;
  if (id == 20 || id == 21) return 2;
  if (id == 30 || id == 31 || id == 32) return 3;
  if (id == 14) return 4;
  if (id == 11) return 5;
  if (id == 12) return 6;
  if (id == 13) return 7;
  return -1;
}

namespace detail {
inline bool Rgb(const motion::Motion& mo, double t, float out[3]) {
  for (const motion::Track& tr : mo.tracks) {
    if (tr.flags != 0x0100) continue;
    double v[3];
    if (!motion::SamplePos(tr, t, v)) continue;
    for (int k = 0; k < 3; ++k) out[k] = float(v[k]);
    return true;
  }
  return false;
}
}  // namespace detail

// Session 98 (generalisation, probe run 6819): a rig motion LOOPS with the period (last key - 1) frames [C: The Rock's cue-5 slot-0
// motions (last key 160) and Daniel Bryan's (100) reproduce every probed frame exactly (sum of 15 abs differences 0.000) at
// t = (frames since cue 5 started) mod 159 / mod 99: 401 -> 83 / 5 in cue 5 itself, and 1391 / 1927 / 3450 (Rock) and 773 / 1205 /
// 3132 (Bryan) in cues 10 / 15 / 20, which have no rig motion at all (the cue-5 motion keeps running)].
// Session 99 (Kane's red light): only motions with a loop (start S, end T) loop; the others HOLD their last key past the end (Kane's
// cue-5 fade-in) [C, LoopEnd above]. t < T plays as is; then S + fmod(t - S, T - S - 1) (with S = 0: the measured mod 159 / 99 of the
// Rock / Bryan; the "- 1" with S > 0 is [U]: no game frame measured).
inline double LoopTime(const Rig& rig, const std::pair<int, int>& key, double t) {
  auto it = rig.loop_end.find(key);
  if (it == rig.loop_end.end()) return t;
  const double s = it->second.first, e = it->second.second;
  const double period = e - s - 1.0;
  if (period < 1.0 || t < e) return t;
  return s + std::fmod(t - s, period);
}

// Whether cue `cue` has a rig motion in any slot (sub 10 cue + 11 .. + 19).
inline bool CueHasRig(const Rig& rig, int cue) {
  auto it = rig.m.lower_bound({10 * cue + 11, -1});
  return it != rig.m.end() && it->first.first <= 10 * cue + 19;
}

// Session 98 (sweep look): the cue whose rig motions drive cue `cue` (its own when it has any, else the newest earlier cue that has
// some; -1 none), and whether a cue holds a motion for one slot.
inline int DriverCue(const Rig& rig, int cue) {
  for (int c = cue; c >= 0; c -= 5)
    if (CueHasRig(rig, c)) return c;
  return -1;
}
inline bool CueHasSlot(const Rig& rig, int cue, int slot) {
  const int sub = 10 * cue + 11 + slot;  // (review) any track of the slot (0..13: sky, ground, lights), not only 0 / 1 / 10
  auto it = rig.m.lower_bound({sub, -1});
  return it != rig.m.end() && it->first.first == sub && it->first.second <= 13;
}

// The set of one slot at cue / time (motion frames). False when the rig lacks that slot (then the game uses slot 0).
// `loop`: each motion is sampled at LoopTime (the game's playback); false = the time as given (a hold past the end).
inline bool EvalSlot(const Rig& rig, int cue, int slot, double t0, LightSet& out, bool loop = false) {
  const int sub = 10 * cue + 11 + slot;
  auto sky = rig.m.find({sub, 0});
  auto gnd = rig.m.find({sub, 1});
  if (sky == rig.m.end() && gnd == rig.m.end()) return false;
  out = LightSet();
  if (sky != rig.m.end()) detail::Rgb(sky->second, loop ? LoopTime(rig, sky->first, t0) : t0, out.sky);
  if (gnd != rig.m.end()) detail::Rgb(gnd->second, loop ? LoopTime(rig, gnd->first, t0) : t0, out.ground);
  for (int i = 0; i < 4; ++i) {
    auto it = rig.m.find({sub, 10 + i});
    if (it == rig.m.end()) continue;
    const double t = loop ? LoopTime(rig, it->first, t0) : t0;
    detail::Rgb(it->second, t, out.col[i]);
    for (const motion::Track& tr : it->second.tracks) {
      motion::Quat q;
      if (tr.flags == 0x0100 || !motion::SampleRot(tr, t, q)) continue;
      // [U] provisional: direction = R(q) * (0, 1, 0)
      const double x = q[0], y = q[1], z = q[2], w = q[3];
      out.dir[i][0] = float(2 * (x * y - z * w));
      out.dir[i][1] = float(1 - 2 * (x * x + z * z));
      out.dir[i][2] = float(2 * (y * z + x * w));
    }
  }
  return true;
}

// The entrance light set that REPLACES arena set `id` at cue / time; falls back to slot 0 like the game. False: keep the arena's.
inline bool EvalForSet(const Rig& rig, int id, int cue, double t, LightSet& out, bool loop = false) {
  const int slot = SlotForSet(id);
  if (slot < 0) return false;
  if (!EvalSlot(rig, cue, slot, t, out, loop) && !(slot >= 4 && EvalSlot(rig, cue, 0, t, out, loop))) return false;
  out.id = id;
  return true;
}

}  // namespace entrance_light
}  // namespace assets
