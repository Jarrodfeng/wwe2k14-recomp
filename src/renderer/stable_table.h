// S102 R21-B (docs\renderer\STAGE_SPLIT.md hazard 1): an append-only table whose elements never move, for the renderer's programs and vertex layouts.
// The bind stage appends (AddProgram / AddVertexLayout, under the renderer's tables_mutex: one writer at a time) while the record stage reads by id
// inside Render with no lock. A std::vector reallocated on push_back and freed the array Render was reading; here an element is built in its chunk
// first and only then published by a release store of the count, so a reader that sees the count (acquire) sees the whole element. Ids handed to the
// record stage inside a frame were published before the hand-over (the stage's mutex), so a reader never asks for an id past the count.
// Std only, header only (tools\renderer\stable_table_test.cpp tests it, with a two-thread stress and a planted twin: a plain vector).
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace rdr {

template <class T, uint32_t kChunkBits = 8, uint32_t kChunks = 256>
class StableTable {
 public:
  static constexpr uint32_t kChunk = 1u << kChunkBits;
  static constexpr size_t kCapacity = size_t(kChunk) * kChunks;

  StableTable() = default;
  StableTable(const StableTable&) = delete;
  StableTable& operator=(const StableTable&) = delete;
  ~StableTable() {
    for (uint32_t c = 0; c < kChunks; ++c) delete[] chunks_[c].load(std::memory_order_relaxed);
  }

  // Writer only (one at a time: the caller holds its lock). Returns false when full (the caller reports it).
  bool push_back(const T& value) {
    const size_t n = count_.load(std::memory_order_relaxed);
    if (n >= kCapacity) return false;
    const uint32_t c = uint32_t(n >> kChunkBits);
    T* chunk = chunks_[c].load(std::memory_order_relaxed);
    if (!chunk) {
      chunk = new T[kChunk];
      chunks_[c].store(chunk, std::memory_order_release);
    }
    chunk[n & (kChunk - 1)] = value;
    count_.store(n + 1, std::memory_order_release);  // publish: the element is complete
    return true;
  }
  size_t size() const { return count_.load(std::memory_order_acquire); }
  bool empty() const { return size() == 0; }
  T& operator[](size_t i) { return chunks_[i >> kChunkBits].load(std::memory_order_acquire)[i & (kChunk - 1)]; }
  const T& operator[](size_t i) const { return chunks_[i >> kChunkBits].load(std::memory_order_acquire)[i & (kChunk - 1)]; }

  // Range-for over the published elements (shutdown, census loops).
  template <class Table, class Ref>
  struct Iter {
    Table* t;
    size_t i;
    Ref operator*() const { return (*t)[i]; }
    Iter& operator++() { ++i; return *this; }
    bool operator!=(const Iter& o) const { return i != o.i; }
  };
  Iter<StableTable, T&> begin() { return {this, 0}; }
  Iter<StableTable, T&> end() { return {this, size()}; }
  Iter<const StableTable, const T&> begin() const { return {this, 0}; }
  Iter<const StableTable, const T&> end() const { return {this, size()}; }

 private:
  std::atomic<T*> chunks_[kChunks] = {};
  std::atomic<size_t> count_{0};
};

}  // namespace rdr
