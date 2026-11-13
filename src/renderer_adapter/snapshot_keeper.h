// The live sink's scene-record keeper (Session 101 lane 35; lane 215 moved it here from live.cpp and added ForPart so a unit test can drive the very same code).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. No game or SDK includes.
#pragma once
#include <cstdint>
#include <memory>
#include <mutex>

#include "../renderer_scene/live_scene.h"

namespace rdr_adapter {

// S101 lane 35: the owner's scene sink: keeps the last two snapshots. EndFrame runs before SubmitFrame in the same present call, so `cur` is this call's
// snapshot and `prev` the one before it. Stream frame j goes with record j-1 (lane 36, bit-exact in 178/178 frames), so SubmitFrame takes `prev`.
//
// Lane 215 (circles / plane "late" refusals, one whole frame of draws per read-back lock): a frame the game reads back from is bound in TWO messages, the first part (DrawAndRead,
// every draw up to the copy) and the rest (SubmitFrame, which is when `prev` was taken). The first part carried NO record, so its draws were played against a board that stopped one
// stamp short: the record j-1 they need (the stamps of guest frame j-1) reached the hooks only with the second part, after the draws were over. In the captured victory runs the first part
// holds all 927 draws and the second part none, so the whole frame went the stream's way: 5 circle draws and ~50 plane draws per read-back lock, exactly the counted `late`
// (7133: 5 locks, 25 late circle draws; 7132: 3 locks, 15; 7123 / 7116: plane 100 / 250). ForPart gives the first part the record the second part will get: at that moment the present
// has not run EndFrame yet, so the last record written (`cur`) IS the previous present's, i.e. the `prev` of the coming ForSubmit.
struct SnapshotKeeper final : rdr_scene::live::Sink {
  std::mutex mu;
  std::shared_ptr<const rdr_scene::SceneTables> cur, prev;
  uint64_t prev_frame = ~0ull;  // the guest frame `prev` was written for
  uint64_t last_submitted = ~0ull;  // the guest frame of the previous ForSubmit
  uint64_t cur_frame = ~0ull;  // the guest frame `cur` was written for (a refused record never reaches the sinks, so cur / prev would not shift: record j-2 must never pair with frame j)
  uint64_t wrong_frame = 0;    // submitted frames whose `cur` was not this present's: fallback
  uint64_t no_previous = 0;  // submitted frames that had no previous snapshot (fallback)
  uint64_t part_records = 0, part_none = 0;   // lane 215: first parts given their record / left without one (nothing to pair)
  bool no_part_record = false;                // lane 215 plant (unit test only): the pre-fix behaviour, a first part never carries a record
  void OnRecord(const rdr_scene::live::FrameInfo& f, const rdr_scene::SceneTables& t) override { Keep(f.guest_frame, std::make_shared<rdr_scene::SceneTables>(t)); }
  void OnRecordShared(const rdr_scene::live::FrameInfo& f, std::shared_ptr<const rdr_scene::SceneTables> t) override { Keep(f.guest_frame, std::move(t)); }
  void Keep(uint64_t guest_frame, std::shared_ptr<const rdr_scene::SceneTables> t) {
    std::lock_guard<std::mutex> lock(mu);
    prev = std::move(cur);
    prev_frame = cur_frame;
    cur = std::move(t);
    cur_frame = guest_frame;
  }
  // lane 152 (review B1): D3 takes the present's OWN record (`cur`), only when it is this present's (null otherwise: counted by the caller as none, nothing is guessed).
  std::shared_ptr<const rdr_scene::SceneTables> ForD3(uint64_t guest_frame) {
    std::lock_guard<std::mutex> lock(mu);
    return cur_frame == guest_frame ? cur : nullptr;
  }
  // Frame j (this present's guest frame) takes the record BEFORE the one written at this present: only when `cur` is this present's own.
  std::shared_ptr<const rdr_scene::SceneTables> ForSubmit(uint64_t guest_frame) {
    std::lock_guard<std::mutex> lock(mu);
    const uint64_t before = last_submitted;
    last_submitted = guest_frame;
    // `cur` must be this present's own record AND `prev` the one written at the previous present: after a refused record k, frame k would get k-2 and frame k+1 would get k-1.
    if (cur_frame != guest_frame || (prev && prev_frame != before)) return ++wrong_frame, nullptr;
    if (!prev) ++no_previous;
    return prev;
  }
  // Lane 215: the record for a frame's FIRST PART (DrawAndRead, before this present's EndFrame). The same record the frame's SubmitFrame will take as `prev`: the last record written, when it
  // is the previous present's (it was written for the guest frame the previous ForSubmit saw). Anything else (the first frame, a refused record in between) -> none, as before: nothing is guessed.
  std::shared_ptr<const rdr_scene::SceneTables> ForPart(uint64_t guest_frame) {
    std::lock_guard<std::mutex> lock(mu);
    if (no_part_record || !cur || last_submitted == ~0ull || cur_frame != last_submitted || cur_frame + 1 != guest_frame) return ++part_none, nullptr;
    ++part_records;
    return cur;
  }
};

}  // namespace rdr_adapter
