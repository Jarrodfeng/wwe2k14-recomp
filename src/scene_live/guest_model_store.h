// The writer's byte store of the guest model rows (Session 103, lane CW-2b; docs\research\s103\CW2_review.md bug 6). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Pure (standard library only). The contributor (game thread) puts every vertex / index / texture byte run it read here, by content hash; the in-process consumer (renderer_scene\guest_model_draw.h)
// asks here BY HASH when a record names bytes it does not hold, so a consumer that skipped records or came late is never short of bytes. A record also carries the bytes (new hash, then every
// kGmResendFrames) for a file consumer. Hard rule 1: process memory only; nothing here is written to disk.
#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace rdr_scene::live::guest_model {

class ByteStore {
 public:
  static constexpr size_t kCapBytes = size_t(384) << 20;
  static constexpr uint64_t kIdleFrames = 1800;   // an entry nobody asked for or put for this many records goes (a model gone from the screen)
  using Bytes = std::shared_ptr<const std::vector<uint8_t>>;
  // Put: the bytes of `hash` (a second Put of the same hash only refreshes its age). `tick` = the writer's frame.
  void Put(uint64_t hash, std::vector<uint8_t>&& b, uint64_t tick) { PutShared(hash, std::make_shared<const std::vector<uint8_t>>(std::move(b)), tick); }
  void PutShared(uint64_t hash, Bytes bytes, uint64_t tick) {
    if (!bytes) return;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = m_.find(hash);
    if (it != m_.end()) { it->second.last = tick; return; }
    if (bytes_ + bytes->size() > kCapBytes) Sweep(tick, true);
    if (bytes_ + bytes->size() > kCapBytes) { ++full_; return; }
    bytes_ += bytes->size();
    Entry e;
    e.b = std::move(bytes);
    e.last = tick;
    m_.emplace(hash, std::move(e));
  }
  bool Has(uint64_t hash, uint64_t tick) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = m_.find(hash);
    if (it == m_.end()) return false;
    it->second.last = tick;
    return true;
  }
  // The consumer's lookup: never touches the entry's age (the consumer's clock is not the writer's: only the writer's Put / Has keep an entry alive).
  Bytes Peek(uint64_t hash) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = m_.find(hash);
    return it == m_.end() ? nullptr : it->second.b;
  }
  Bytes Get(uint64_t hash, uint64_t tick) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = m_.find(hash);
    if (it == m_.end()) return nullptr;
    it->second.last = tick;
    return it->second.b;
  }
  void Tick(uint64_t tick) {   // once a writer frame: drop what has been idle
    std::lock_guard<std::mutex> lk(mu_);
    if ((tick & 63) == 0) Sweep(tick, false);
  }
  size_t bytes() const { std::lock_guard<std::mutex> lk(mu_); return bytes_; }
  size_t entries() const { std::lock_guard<std::mutex> lk(mu_); return m_.size(); }
  uint64_t full() const { std::lock_guard<std::mutex> lk(mu_); return full_; }
  void Clear() { std::lock_guard<std::mutex> lk(mu_); m_.clear(), bytes_ = 0, full_ = 0; }
  static ByteStore& Process() { static ByteStore s; return s; }
 private:
  struct Entry { Bytes b; uint64_t last = 0; };
  void Sweep(uint64_t tick, bool hard) {   // mu_ held
    const uint64_t idle = hard ? kIdleFrames / 8 : kIdleFrames;
    for (auto it = m_.begin(); it != m_.end();) {
      if (tick > it->second.last && tick - it->second.last > idle) { bytes_ -= it->second.b->size(), it = m_.erase(it); }
      else ++it;
    }
  }
  mutable std::mutex mu_;
  std::unordered_map<uint64_t, Entry> m_;
  size_t bytes_ = 0;
  uint64_t full_ = 0;
};

}  // namespace rdr_scene::live::guest_model
