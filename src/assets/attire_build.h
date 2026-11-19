// S103 ARM2 (docs\agents\s103\HNN.md "Open", the diagnosis run wwe2k14_046; docs\systems\attire_build.md "ARM2"): the composite attire's colours, read WHEN THE GAME USES THEM.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The game paints a composite attire's mask steps in sub_83418580 (CEditExSSCompositTexture, one call per mask step, r3 = the composite, r4 = the step 0..3; the function
// does nothing for r4 >= 4). The colour rule it reads [C: q decomp sub_83418580 +DC / +188..+1C4, listing wwe2k14_recomp.137.cpp:31970]:
//     P = [composite+100]            (the character load descriptor, the same P as [reader+100]: sub_834175E8 +4C)
//     set = extsb([P+44])            (a SIGNED BYTE: the in-memory colour set; NOT the tens digit of the attire key)
//     colours(step k, channel c) = the three bytes (R, G, B) at [P+16] + 132 * set + 1320 + 3 * (4 k + c)
// The "Default SS Change Color Model" rule (sub_822CE2C0 0x822CE43C loop / sub_8230ED80, CMP2) copies the attire slot's 16 colours (misc01_Start p0/600 +64 + 48 s) into colour set 0 of
// that block and sets P+44 = 0 and P+40 = 1: so for Heenan a1 (24912, "colour slot 1") the game reads set 0, and run wwe2k14_046 printed P+44 0 at his build step. Reading `slot 1`
// (the key's tens digit) or desc+48 at the link would pick other bytes / bytes not yet written. The mode word [P+40]: sub_83419478 (Create) builds for any mode that is neither 0 nor 2
// (both go to the same exit @83419808), so a build step runs only in a "build" mode (1 in every log); mode 2 never reaches this function, and a mode 2 seen here is counted, not composed.
//
// This file: the pure read of one build step (template over the guest word reader, so the host test drives it with a byte map) and the process-wide latch the hook fills on the game
// thread and the record side (WrestlerContributor::Collect, on its own thread) only READS. No host read of guest memory happens outside the hook.
//   * Key: the CHOSEN key (desc+92 = 24912) with the owner (desc address, loaded key desc+28 = 24902). A body linked by ModelLinkRead matches by desc + loaded key; a body named by its
//     participant slot key (Heenan in 042 / 046: the link hook never logged him) matches by the chosen key.
//   * Steps 1..3 refresh only their own 12 bytes (the game reads them at that step), counted when they differ.
//   * A mode 0 / 2 link read of the same chosen key forgets its capture (that load did not build; ModelLinkRead), so a stock load never inherits an earlier edited colour set.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "attire_compose.h"
#include "../scene_live/slot_key.h"

namespace attire_build {

constexpr int kStepBytes = 12;    // one mask step = 4 channels x RGB
constexpr int kMaxSteps = 4;      // sub_83418580 runs its colour loop for r4 < 4
constexpr size_t kMaxEntries = 64;

// Planted faults for the offline proof; never set in the game.
enum class Fault { kNone, kSetFromKeySlot, kLinkTimeSet, kComposeMode2 };

struct Capture {
  uint32_t chosen = 0, loaded = 0, mode = 0, desc = 0;
  int32_t set = 0;
  uint8_t colours[attire_compose::kColourBytes] = {};
  uint64_t hash = 0;
};

enum class Step {
  kCaptured,      // step 0 of a build: a new capture (all 48 bytes)
  kRefreshed,     // step 1..3: its 12 bytes re-read
  kNotAStep,      // r4 >= 4: the function does nothing with colours
  kModeBase,      // mode 0 / 2: the game does not build (a contradiction at a build step: counted, nothing composed)
  kUnreadable,    // a guest read failed
  kNoComposite,   // P / desc not pointers
  kBadBlock,      // [P+16] not a pointer
  kBadSet,        // set outside 0..15 or the block's end past the 3376-byte table
};

// The pure read. `rw(addr, uint32_t&) -> bool` reads one big-endian guest word (the game's ReadWord). Reads exactly what sub_83418580 reads for this step (and, at step 0, the whole 48 bytes).
template <class ReadWordFn>
inline Step ReadStep(ReadWordFn rw, uint32_t composite, uint32_t step, Capture& out, Fault fault = Fault::kNone) {
  if (step >= uint32_t(kMaxSteps)) return Step::kNotAStep;
  uint32_t P = 0, desc = 0;
  if (!rw(composite + 100, P) || !rw(composite + 140, desc)) return Step::kUnreadable;
  if (P < 0x10000u || desc < 0x10000u) return Step::kNoComposite;
  uint32_t mode = 0, p44 = 0, block = 0, chosen = 0, loaded = 0;
  if (!rw(P + 40, mode) || !rw(P + 44, p44) || !rw(P + 16, block) || !rw(desc + 92, chosen) || !rw(desc + 28, loaded)) return Step::kUnreadable;
  out.mode = mode, out.chosen = chosen, out.loaded = loaded, out.desc = desc;
  if (mode == 0 || (mode == 2 && fault != Fault::kComposeMode2)) return Step::kModeBase;   // (the planted twin folds mode 2 into a build)
  if (block < 0x10000u) return Step::kBadBlock;
  int32_t set = int32_t(int8_t(p44 >> 24));   // extsb of the byte at P+44 (big-endian word: the byte is its top byte)
  if (fault == Fault::kSetFromKeySlot) set = int32_t((chosen / 10) % 10);   // planted: the tens digit of the key
  if (fault == Fault::kLinkTimeSet) {
    uint32_t d48 = 0;
    if (!rw(desc + 48, d48)) return Step::kUnreadable;
    set = int32_t(d48);   // planted: the link's copy (desc+48), read as the link does
  }
  if (set < 0 || set > 15) return Step::kBadSet;
  const uint64_t at = uint64_t(block) + 132u * uint64_t(set) + 1320u;
  if (at + attire_compose::kColourBytes > 0xFFFFFFFFull) return Step::kBadBlock;
  out.set = set;
  const int first = step == 0 ? 0 : int(step) * kStepBytes, last = step == 0 ? attire_compose::kColourBytes : int(step + 1) * kStepBytes;
  for (int i = first / 4; i < last / 4; ++i) {
    uint32_t w = 0;
    if (!rw(uint32_t(at) + 4u * uint32_t(i), w)) return Step::kUnreadable;
    for (int b = 0; b < 4; ++b) out.colours[4 * i + b] = uint8_t(w >> (24 - 8 * b));   // big-endian word -> bytes in memory order
  }
  if (step == 0) out.hash = attire_compose::HashColours(out.colours);
  return step == 0 ? Step::kCaptured : Step::kRefreshed;
}

struct Counters {
  std::atomic<uint64_t> steps0_mode1{0}, steps0_mode2{0}, steps0_other{0};   // build steps (step 0) by mode: mode 1 / mode 2 / any other non-zero mode
  std::atomic<uint64_t> refused{0}, refreshed{0}, late_change{0}, orphan{0}, recolour{0}, forgotten{0}, evicted{0};
  std::atomic<uint64_t> applied{0};   // captures used for a record's entry (first use of each capture)
  std::atomic<uint64_t> missing{0};   // a remapped key the loader loaded as the plain base: no capture (the game did not build it, or the build comes later)
};
inline Counters& Count() {
  static Counters c;
  return c;
}

// The census line (step check): composite at build step N (mode 1 N / mode 2 N), colours applied N, missing N.
inline std::string CensusLine() {
  const Counters& c = Count();
  char b[384];
  std::snprintf(b, sizeof b, "[attire_build] composite at build step %llu (mode 1 %llu / mode 2 %llu / other %llu), colours applied %llu, missing %llu; refused %llu, refreshed %llu (late change %llu), orphan steps %llu, recoloured %llu, forgotten %llu",
                (unsigned long long)(c.steps0_mode1 + c.steps0_mode2 + c.steps0_other), (unsigned long long)c.steps0_mode1.load(), (unsigned long long)c.steps0_mode2.load(), (unsigned long long)c.steps0_other.load(),
                (unsigned long long)c.applied.load(), (unsigned long long)c.missing.load(), (unsigned long long)c.refused.load(), (unsigned long long)c.refreshed.load(), (unsigned long long)c.late_change.load(),
                (unsigned long long)c.orphan.load(), (unsigned long long)c.recolour.load(), (unsigned long long)c.forgotten.load());
  return b;
}

class Latch {
 public:
  struct Entry {
    Capture cap;
    uint64_t gen = 0;
    bool applied = false;   // counted once
  };
  static Latch& Instance() {
    static Latch l;
    return l;
  }

  // The hook's call: one build step. Returns the step's verdict; `line` gets a log line for a new capture (empty otherwise).
  template <class ReadWordFn>
  Step OnBuildStep(ReadWordFn rw, uint32_t composite, uint32_t step, std::string& line, Fault fault = Fault::kNone) {
    line.clear();
    Capture c;
    const Step r = ReadStep(rw, composite, step, c, fault);
    if (r == Step::kNotAStep) return r;
    if (r == Step::kModeBase) {
      if (c.mode == 2 && step == 0) Count().steps0_mode2.fetch_add(1, std::memory_order_relaxed);   // (mode 0 here is a contradiction: refused only)
      Count().refused.fetch_add(1, std::memory_order_relaxed);
      return r;
    }
    if (r != Step::kCaptured && r != Step::kRefreshed) {
      Count().refused.fetch_add(1, std::memory_order_relaxed);
      return r;
    }
    std::lock_guard<std::mutex> l(mu_);
    if (r == Step::kCaptured) {
      (c.mode == 1 ? Count().steps0_mode1 : Count().steps0_other).fetch_add(1, std::memory_order_relaxed);
      auto it = by_chosen_.find(c.chosen);
      if (it != by_chosen_.end() && it->second.cap.hash != c.hash) Count().recolour.fetch_add(1, std::memory_order_relaxed);
      if (it == by_chosen_.end() && by_chosen_.size() >= kMaxEntries) {   // bounded: the oldest capture goes
        auto victim = by_chosen_.begin();
        for (auto e = by_chosen_.begin(); e != by_chosen_.end(); ++e)
          if (e->second.gen < victim->second.gen) victim = e;
        by_chosen_.erase(victim);
        Count().evicted.fetch_add(1, std::memory_order_relaxed);
      }
      for (auto o = by_chosen_.begin(); o != by_chosen_.end();)   // F1: one descriptor builds one attire at a time: another chosen key's capture of this desc is a dead build
        if (o->first != c.chosen && o->second.cap.desc == c.desc) o = by_chosen_.erase(o), Count().forgotten.fetch_add(1, std::memory_order_relaxed);
        else ++o;
      Entry& e = by_chosen_[c.chosen];
      const bool same = e.cap.hash == c.hash && e.cap.desc == c.desc && e.gen != 0;
      e.cap = c, e.gen = ++gen_;
      if (!same) e.applied = false;
      char b[256];
      std::snprintf(b, sizeof b, "[attire_build] build step 0: chosen %u loaded %u mode %u set %d desc 0x%X: colours %016llX", c.chosen, c.loaded, c.mode, c.set, c.desc, (unsigned long long)c.hash);
      line = b;
      return r;
    }
    // step 1..3: only the same owner's capture takes the step's 12 bytes (a step with no step-0 capture is an orphan: counted, nothing kept)
    auto it = by_chosen_.find(c.chosen);
    if (it == by_chosen_.end() || it->second.cap.desc != c.desc) {
      Count().orphan.fetch_add(1, std::memory_order_relaxed);
      return r;
    }
    Count().refreshed.fetch_add(1, std::memory_order_relaxed);
    Entry& e = it->second;
    if (std::memcmp(e.cap.colours + kStepBytes * step, c.colours + kStepBytes * step, kStepBytes) != 0) {
      std::memcpy(e.cap.colours + kStepBytes * step, c.colours + kStepBytes * step, kStepBytes);
      e.cap.hash = attire_compose::HashColours(e.cap.colours), e.applied = false;
      Count().late_change.fetch_add(1, std::memory_order_relaxed);
    }
    return r;
  }

  // The record side: the entry tag ("/c:<96 hex>") of the capture for a body NAMED BY ITS PARTICIPANT SLOT KEY (the chosen key), "" = none.
  // F2: only for the slot's OWN descriptor (`slot_desc`, found by the hook by the game's index rule, slotkey::SlotDescriptor: D+7 else the slot index; 0 = unknown = no tag) and only for a key the game REMAPPED (loaded != chosen: the class the
  // loader's remap table loads, so a failed tagged load is a counted hole as in ARM; F3: a chosen == loaded capture, e.g. a CAW or an edited stock attire, stays the plain key and its guest-row fallback).
  std::string TagForSlot(uint32_t chosen, uint32_t slot_desc) {
    if (slot_desc == 0) return std::string();
    std::lock_guard<std::mutex> l(mu_);
    auto it = by_chosen_.find(chosen);
    if (it == by_chosen_.end() || it->second.cap.desc != slot_desc || it->second.cap.loaded == it->second.cap.chosen) return std::string();
    return Use(it->second);
  }
  bool HasChosen(uint32_t chosen) {   // the hook's cheap question before it reads guest memory for the descriptor
    std::lock_guard<std::mutex> l(mu_);
    return by_chosen_.count(chosen) != 0;
  }
  // ... for a body LINKED by ModelLinkRead: the owner is the descriptor (desc address) AND the loaded key (desc+28) AND the chosen key (desc+92) the link read (F1: a reused descriptor never lends its colours).
  std::string TagForOwner(uint32_t desc, uint32_t loaded, uint32_t chosen) {
    if (desc == 0 || chosen == 0) return std::string();
    std::lock_guard<std::mutex> l(mu_);
    auto it = by_chosen_.find(chosen);
    if (it == by_chosen_.end() || it->second.cap.desc != desc || it->second.cap.loaded != loaded) return std::string();
    return Use(it->second);
  }
  void Clear() {   // a new game folder (ModelCache::ClearFailed): nothing captured under the old files stays
    std::lock_guard<std::mutex> l(mu_);
    by_chosen_.clear();
  }
  // A load of this chosen key that does not build (mode 0 / 2 at the link): its capture is for an earlier load.
  void Forget(uint32_t chosen) {
    std::lock_guard<std::mutex> l(mu_);
    if (by_chosen_.erase(chosen)) Count().forgotten.fetch_add(1, std::memory_order_relaxed);
  }
  size_t Size() {
    std::lock_guard<std::mutex> l(mu_);
    return by_chosen_.size();
  }
  void ResetForTest() {
    std::lock_guard<std::mutex> l(mu_);
    by_chosen_.clear(), gen_ = 0;
    Counters& c = Count();
    for (std::atomic<uint64_t>* a : {&c.steps0_mode1, &c.steps0_mode2, &c.steps0_other, &c.refused, &c.refreshed, &c.late_change, &c.orphan, &c.recolour, &c.forgotten, &c.evicted, &c.applied, &c.missing}) a->store(0);
  }

 private:
  std::string Use(Entry& e) {   // under mu_
    e.gen = ++gen_;   // F5: a capture in use is the newest, never the eviction victim
    if (!e.applied) e.applied = true, Count().applied.fetch_add(1, std::memory_order_relaxed);
    return attire_compose::EntryTag(e.cap.colours);
  }
  std::mutex mu_;
  std::map<uint32_t, Entry> by_chosen_;
  uint64_t gen_ = 0;
};

// F2 (the hook's pure half, after it found the slot's descriptor `desc` by the game's index rule, slotkey::SlotDescriptor): the slot's occupant builds only when the descriptor's mode (desc+44, the game's copy of P+40 made at the
// load, sub_82720658 0x827207D8) is neither 0 nor 2 and its chosen key (desc+92) is the slot key. Returns the descriptor to match the capture against (0 = no tag); a load that does NOT build forgets the key's capture.
template <class ReadWordFn>
inline uint32_t SlotOwner(ReadWordFn rw, uint32_t desc, uint32_t slot_key, Latch& latch) {
  uint32_t mode = 0, chosen = 0;
  if (desc == 0 || !rw(desc + 44, mode) || !rw(desc + 92, chosen) || chosen != slot_key) return 0;
  if (mode == 0 || mode == 2) {
    latch.Forget(slot_key);
    return 0;
  }
  return desc;
}

// S103 HPF: the hook's whole slot-descriptor step as one pure function (the unit drives exactly this): the character's descriptor by the game's index rule (slotkey::SlotDescriptor), then SlotOwner
// (desc+92 == the slot key stays the fail-closed identity proof). `why` names the verdict for the log line; returns the descriptor to match the capture against (0 = no tag).
enum class SlotWhy { kTag, kNoDesc, kKeyMismatch, kModeBase };
template <class ReadWordFn>
inline uint32_t SlotDescForChar(ReadWordFn rw, uint32_t slot_ch, uint32_t slot_key, Latch& latch, SlotWhy& why, wwe2k14::slotkey::SlotDescInfo* info = nullptr) {
  const uint32_t d = wwe2k14::slotkey::SlotDescriptor(rw, slot_ch, info);
  uint32_t mode = 0, chosen = 0;
  if (d == 0 || !rw(d + 44, mode) || !rw(d + 92, chosen)) return why = SlotWhy::kNoDesc, 0u;
  if (chosen != slot_key) return why = SlotWhy::kKeyMismatch, 0u;
  const uint32_t r = SlotOwner(rw, d, slot_key, latch);
  why = r ? SlotWhy::kTag : SlotWhy::kModeBase;
  return r;
}

}  // namespace attire_build
