// S103 ARM (docs\agents\s103\HNN.md fix 1; docs\research\s103\CMP.md CMP2 section 4): the game's attire REMAP, read from the player's own files.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// A "composite" attire has no model of its own: Heenan's 30 Years attire (key 24912) is attire 0's mesh / base / masks (24902) recoloured with colour slot 1. The game's rule is
// sub_823BB018 -> sub_822D1598 (does the key have a remap?) -> sub_822D1930 (which key?), over the colour table pac\misc01_Start.pac entry p0/600:
//   u16 record count, then 664-byte records; record: u16 character id at +0 (key / 100); per attire slot s (0-9, s = the key's tens digit):
//     flag A at +4+s, flag B at +14+s, kind-2 word at +24+s, kind-2 attire at +34+s, kind-4 word at +44+s, kind-4 attire at +54+s, 16 RGB colours at +64+48 s.
//   remap(key): record of key / 100 not found, or slot s >= 10, or flag A != 1  -> key;
//     kind 2 (key % 10): word == 2 -> key; word 0 -> c*100 + attire*10 + 2; word 1 -> c*100 + attire*10 + 4; other -> key;
//     kind 4: the same with the kind-4 word / attire; any other kind -> key.   [C: sub_822D1598 / sub_822D1930 listings, S103 ARM read]
// The loader (live_assets.cpp) applies it only when the character's pac has no model PACH named by the key (the stream-free build has no GPU build; the colours come
// from the model link's tag, attire_compose.h). Header-only; no SDK headers; nothing of the table is kept beyond this process.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "asset_load.h"

namespace assets::attire_remap {

constexpr uint32_t kTableEntry = 600;   // p0 entry 600 of pac\misc01_Start.pac
constexpr size_t kRecord = 664;
constexpr int kSlots = 10;

enum class Fault { kNone, kNoRemap, kIgnoreFlagA, kWordIgnored, kSlotZero };   // planted faults for the offline proof; never set in the game

struct Table {
  Bytes data;
  uint32_t count = 0;
  bool ok = false;
  // The record of character `cid` (first match, as sub_822D1598 scans), null when absent.
  // The game reads the id as a LE u32 at +0 (sub_822D1598 / 822D1808).
  static uint32_t Id(const uint8_t* r) { return uint32_t(r[0]) | uint32_t(r[1]) << 8 | uint32_t(r[2]) << 16 | uint32_t(r[3]) << 24; }
  const uint8_t* Record(int cid) const {
    for (uint32_t k = 0; k < count; ++k) {
      const uint8_t* r = data.data() + 2 + kRecord * k;
      if (Id(r) == uint32_t(cid)) return r;
    }
    return nullptr;
  }
  // sub_822D1598's scan: continues past an id match whose flag A (+4+s) != 1; the first match with flag A set, else null.
  const uint8_t* RecordFlagA(int cid, int s) const {
    for (uint32_t k = 0; k < count; ++k) {
      const uint8_t* r = data.data() + 2 + kRecord * k;
      if (Id(r) == uint32_t(cid) && r[4 + s] == 1) return r;
    }
    return nullptr;
  }
};

inline bool Parse(Bytes bytes, Table& t) {
  t = Table();
  if (bytes.size() < 2) return false;
  const uint32_t count = bytes[0] | (uint32_t(bytes[1]) << 8);
  if (count == 0 || 2 + kRecord * size_t(count) > bytes.size()) return false;
  t.data = std::move(bytes), t.count = count, t.ok = true;
  return true;
}

struct Remap {
  int to = 0;          // the key the game loads (== key when nothing remaps)
  int slot = 0;        // the colour slot: the tens digit of the CHOSEN key
  bool flag_a = false; // the table lists the attire (the game builds / remaps it)
  bool flag_b = false; // flag B too: the game builds it by default (a composite)
  bool remapped() const { return to != 0 && changed; }
  bool changed = false;
};

inline Remap Resolve(const Table& t, int key, Fault fault = Fault::kNone) {
  Remap out;
  out.to = key;
  if (!t.ok || key < 0) return out;
  const int cid = key / 100, s = (key / 10) % 10, kind = key % 10;
  out.slot = fault == Fault::kSlotZero ? 0 : s;
  const uint8_t* r = t.RecordFlagA(cid, s);
  if (!r) r = t.Record(cid);   // no record passes flag A: the first id match, for the flags only
  if (!r) return out;
  out.flag_a = r[4 + s] == 1, out.flag_b = r[14 + s] == 1;
  if (fault == Fault::kNoRemap) return out;
  if (!out.flag_a && fault != Fault::kIgnoreFlagA) return out;
  int word = -1, attire = 0;
  if (kind == 2) word = r[24 + s], attire = r[34 + s];
  else if (kind == 4) word = r[44 + s], attire = r[54 + s];
  else return out;
  if (fault == Fault::kWordIgnored) word = 0;
  if (word != 0 && word != 1) return out;   // 2 = no remap (sub_822D1598 returns 0), anything else = the key unchanged (sub_822D1930)
  out.to = cid * 100 + attire * 10 + (word == 0 ? 2 : 4);
  out.changed = out.to != key;
  return out;
}

// The colour block of the file for slot s of character `cid` (48 bytes, 16 RGB), null when absent. NOT used to colour a body (the colours come from the link; HNN open question).
inline const uint8_t* SlotColours(const Table& t, int cid, int s) {
  const uint8_t* r = t.Record(cid);
  return (r && s >= 0 && s < kSlots) ? r + 64 + 48 * s : nullptr;
}

// ---- the process-wide table, read from the game files at the first remapped lookup ------------------------------------------------------------------------
namespace detail {
struct State {
  std::mutex mu;
  Table table;
  bool tried = false;
};
inline State& S() { static State s; return s; }
}  // namespace detail

// Reads pac\misc01_Start.pac p0/600 (the whole pac is unpacked, the one entry kept). Fault-free; false + err when the pac / entry is missing.
inline bool ReadTable(launcher::GameFiles& files, Table& t, std::string& err) {
  pac::Pac p;
  if (!pac::Open(files, "pac\\misc01_Start.pac", p, err)) return false;
  if (p.pachs.empty()) return err = "misc01_Start.pac: no PACH", false;
  const pac::Ent* e = pac::Find(p.pachs[0], kTableEntry);
  if (!e) return err = "misc01_Start.pac: no p0/600", false;
  if (!Parse(pac::Decode(p.bytes.data() + e->off, e->size), t)) return err = "misc01_Start.pac p0/600: not a colour table", false;
  return true;
}

inline void Reset() {   // a new game folder
  auto& s = detail::S();
  std::lock_guard<std::mutex> l(s.mu);
  s.table = Table(), s.tried = false;
}

// The table, loaded once (a missing pac leaves it empty: no remap, said once by the caller's census).
inline const Table& TheTable(launcher::GameFiles& files, std::string* err_out = nullptr) {
  auto& s = detail::S();
  std::lock_guard<std::mutex> l(s.mu);
  if (!s.tried) {
    s.tried = true;
    std::string err;
    if (!ReadTable(files, s.table, err)) {
      s.table = Table();
      if (err_out) *err_out = err;
    }
  }
  return s.table;
}

// ---- the loader's pick (the production call: live_assets.cpp GameFilesSource::Load) ---------------------------------------------------------------------------------
struct Pick {
  int ord = -1;            // the model PACH ordinal the loader loads, -1 = none
  int loaded = 0;          // the key of that PACH
  int slot = 0;            // the colour slot of the CHOSEN key
  bool remapped = false;   // loaded != the named key because of the table
  bool hole_class = false; // the game REMAPS the attire (a composite): a failure is a counted hole, never the guest rows
};

// `named` = the key the record names. The pac's own PACH named `named` wins (the old rule: its ring-model fallback included); only when the pac has none does the game's remap apply.
inline Pick PickModel(launcher::GameFiles& files, const Table& t, int id, int named, Fault fault = Fault::kNone) {
  Pick p;
  const Remap r = Resolve(t, named, fault);
  p.slot = r.slot, p.hole_class = r.remapped();   // review F3: identity flag-A keys keep the old fallback (guest rows)
  auto try_key = [&](int key) {
    const int o = OrdinalOfModelKey(files, id, key);
    return o;
  };
  int o = try_key(named);
  if (o >= 0) { p.ord = o, p.loaded = named; return p; }
  if (r.remapped()) {
    o = try_key(r.to);
    if (o >= 0) { p.ord = o, p.loaded = r.to, p.remapped = true; return p; }
  }
  const int ring = named - named % 10 + 2;   // the old fallback: the same attire's kind 2 (ring) model
  o = try_key(ring);
  if (o >= 0) { p.ord = o, p.loaded = ring; return p; }
  if (r.remapped()) {
    const int rring = r.to - r.to % 10 + 2;
    o = try_key(rring);
    if (o >= 0) { p.ord = o, p.loaded = rring, p.remapped = true; }
  }
  return p;
}

}  // namespace assets::attire_remap
