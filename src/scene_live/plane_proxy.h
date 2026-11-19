// S8 (S103 lane A3, audit item 4): the plane module's per-frame PROXY LIST, copied before the game zeroes it. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only, no game or SDK includes.
//
// What it is [C listings: sub_827EF070 0x827EF090-0x827EF0B0 and 0x827EF0E8-0x827EF100, sub_827EEF80, sub_827EED58]: the plane node's draw entry zeroes 32 words at module + 1176 and the count at module + 1304 on
// BOTH of its paths (it draws the proxies first on one of them); the update task sub_827EE380 appends to the list (max 32) every frame, so the list is complete at the node's entry and gone after it. Under OURS
// the node does not draw (our own branch, scene_skip_nodes.h PlaneZeroProxies, only zeroes), so nothing would ever read the list: this board keeps a copy taken at the entry of the node, in walked AND
// ours frames, stamped with the hook's guest frame. The inputs contributor reads the copies at the latch and writes mask-kind PlaneDraw rows (object only: the kind decode / plane position stay [U] and are
// marked absent, `have` = 0) and stamps the plane-constants input when the frame's copy exists. module = r3 - 4 (the node is module + 4).
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace rdr_scene::live::planeproxy {

inline constexpr uint32_t kWords = 32, kListOff = 1176, kCountOff = 1304, kMaxPending = 8;

struct Snap {
  uint32_t frame = 0;     // g_guest_frame when the node ran. The plane node is a DRAW-HALF hook: this is the RECORD frame, not the hook frame (FRAME_CONTRACT.md "The draw-half clock"); match it to a record's own frame
  uint32_t count = 0;     // module + 1304
  uint32_t words[kWords] = {};   // module + 1176 .. +1303, host order
};

class Board {
 public:
  // Hook side (the plane node's entry, any thread). `raw` = the 32 list words and `raw_count` the count word, guest byte order (big-endian).
  void Note(uint32_t frame, const uint32_t* raw, uint32_t raw_count) {
    Snap s;
    s.frame = frame;
    s.count = Swap(raw_count);
    for (uint32_t i = 0; i < kWords; ++i) s.words[i] = Swap(raw[i]);
    std::lock_guard<std::mutex> lock(mu_);
    if (pending_.size() >= kMaxPending) pending_.erase(pending_.begin()), ++dropped_;
    pending_.push_back(s);
    ++noted_;
  }
  // Latch side (S103 A3, F1): the copies tagged <= `max_frame` (the record's own frame), oldest first; NEWER copies stay for the next take (they belong to a later record); copies older than the
  // previous take's frame are returned too (the caller counts them as stale).
  void TakeUpTo(uint32_t max_frame, std::vector<Snap>& out) {
    out.clear();
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Snap> keep;
    for (const Snap& s : pending_) (s.frame <= max_frame ? out : keep).push_back(s);
    pending_.swap(keep);
    taken_ += out.size();
  }
  // The copies since the last take (oldest first), all of them.
  void Take(std::vector<Snap>& out) {
    std::lock_guard<std::mutex> lock(mu_);
    out.swap(pending_);
    pending_.clear();
    taken_ += out.size();
  }
  uint64_t Noted() const { std::lock_guard<std::mutex> l(mu_); return noted_; }
  uint64_t Dropped() const { std::lock_guard<std::mutex> l(mu_); return dropped_; }
  uint64_t Taken() const { std::lock_guard<std::mutex> l(mu_); return taken_; }
  void Reset() { std::lock_guard<std::mutex> l(mu_); pending_.clear(), noted_ = dropped_ = taken_ = 0; }

 private:
  static uint32_t Swap(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }
  mutable std::mutex mu_;
  std::vector<Snap> pending_;
  uint64_t noted_ = 0, dropped_ = 0, taken_ = 0;
};

inline Board& The() {
  static Board b;
  return b;
}
// Registered once at start by the inputs glue; the hook does nothing until then (one relaxed load).
inline std::atomic<bool>& RegisteredFlag() {
  static std::atomic<bool> f{false};
  return f;
}

}  // namespace rdr_scene::live::planeproxy
