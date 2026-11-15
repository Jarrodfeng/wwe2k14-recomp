// S102 lane SW1 (wwe2k14_rdr_swap_once bit 1, swap_once.h): the live scene record's tables are made once and recycled.
//
// What it removes: BuildFrame made `std::make_shared<SceneTables>()` every frame and every vector grew from empty (the contributors reserve a frame's worth: ~200 KB for the objects,
// the string blob copy of the pool, ...): a heap allocation per vector, fresh pages (page faults) for the big ones, and the same again freed on the render thread when the record was
// dropped. Sizes repeat frame to frame (R20 4b: operator new 0.44 ms of the swap thread in Bryan's heavy stretch). Now a table the last owner drops is cleared (SceneTables::Recycle: every
// vector emptied, capacity kept, every scalar and index back to a fresh table's) and put on a small free list; the next record takes it. The FramePool pattern of frame_handoff.h.
//
// Safety: a table is given back only by its LAST owner (the shared_ptr's deleter), so no sink, keeper, queue or render-thread message can still hold it. Each record gets a NEW control block
// (the pooled object is wrapped again), so a weak_ptr keyed on the record (ExpandedForReading) can never match a recycled table's later record. Give() takes only the pool's own small mutex:
// it may run on any thread, under the writer mutex or not. A table that would not fit the free list is deleted.
// No game or SDK includes.
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "record_io.h"
#include "swap_once.h"

namespace rdr_scene {

class TablesPool {
 public:
  static constexpr size_t kKeep = 8;   // free tables kept (a record is held by the keeper, the render queue and the render thread's current / previous frames; more in flight are made fresh)

  struct Recycler {
    TablesPool* pool = nullptr;   // null = a plain table (the pool is off): deleted
    void operator()(SceneTables* t) const {
      if (pool) pool->Give(t);
      else delete t;
    }
  };
  using Handle = std::unique_ptr<SceneTables, Recycler>;

  // A cleared table: recycled when one is free, new otherwise.
  Handle Take() {
    SceneTables* t = nullptr;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (!free_.empty()) {
        t = free_.back();
        free_.pop_back();
        n_free_.store(free_.size(), std::memory_order_relaxed);
      }
    }
    if (t) swap_once::g_count.tables_reused.fetch_add(1, std::memory_order_relaxed);
    else t = new SceneTables, swap_once::g_count.tables_made.fetch_add(1, std::memory_order_relaxed);
    return Handle(t, Recycler{this});
  }
  // A plain table that is never recycled (the switch is off for this record).
  static Handle Plain() { return Handle(new SceneTables, Recycler{nullptr}); }

  // Bit 1 read 0: the free tables (each at its peak capacity) are released. One relaxed load when there is nothing to release.
  void Trim() {
    if (!n_free_.load(std::memory_order_relaxed)) return;
    std::vector<SceneTables*> gone;
    {
      std::lock_guard<std::mutex> lk(mu_);
      gone.swap(free_);
      n_free_.store(0, std::memory_order_relaxed);
    }
    for (SceneTables* t : gone) delete t;
  }

  size_t FreeCount() {
    std::lock_guard<std::mutex> lk(mu_);
    return free_.size();
  }

#ifdef RDR_SWAP_PLANTS
  inline static int plant = 0;   // test only (tools\swap\swap_once_test.cpp): 1 = Give() does not clear the table (a recycled table keeps the last record's content)
#endif

 private:
  void Give(SceneTables* t) {
#ifdef RDR_SWAP_PLANTS
    if (plant != 1) t->Recycle();
#else
    t->Recycle();
#endif
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (free_.size() < kKeep) {
        free_.push_back(t);
        n_free_.store(free_.size(), std::memory_order_relaxed);
        return;
      }
    }
    swap_once::g_count.tables_dropped.fetch_add(1, std::memory_order_relaxed);
    delete t;
  }
  std::mutex mu_;
  std::vector<SceneTables*> free_;
  std::atomic<size_t> n_free_{0};
};

}  // namespace rdr_scene
