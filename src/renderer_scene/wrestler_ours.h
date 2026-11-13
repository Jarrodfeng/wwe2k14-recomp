// The wrestlers drawn by us in place (Session 101, lane 45 = W2; docs\renderer\WRESTLER_PROMOTION_MAP.md sections 3-5 and 10-11, REPLACE_SEAM.md).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// WrestlerOurs is a DrawReplaceHook (src\renderer_adapter\draw_replace_hook.h) for the main-pass model-part draws (pass sites 0x82C0A950 / 0x82C0B94C). Each claimed
// stream draw whose object is a wrestler body of the player's own files is replaced IN PLACE: our part geometry (the file's strip, the vertices in the stream draw's
// host layout, positions times the body's one scale) and the bone palette rows from the scene record (the vertex-constant row override c76 ..); everything else - the
// state, the textures, every other constant, the draw's place in the frame - stays the stream's. A draw of any other class (the arena's, props) is NotOurs and passes
// to the next hook. Every decision is WrestlerCore's (wrestler_core.h, GPU-free and shared with the offline checker); this class uploads what Decide returns.
//
// Velocity (DLSS motion vectors) still come from the stream mesh (REPLACE_SEAM.md): our mesh and pose equal the stream's, which the checks prove per draw.
// Threads: SetRecord / SetGameRoot from any thread; everything else on the render thread.
#pragma once
#include <memory>
#include <mutex>
#include <string>

#include "../assets/wrestler_roster.h"
#include "../renderer/renderer.h"
#include "../renderer_adapter/draw_replace_hook.h"
#include "replace_factories.h"
#include "wrestler_core.h"

namespace rdr_scene {

class WrestlerOurs final : public rdr_adapter::DrawReplaceHook {
 public:
  // live: models are read on workers and the roster is built on a worker (a draw waits as "unjoined"); not live (fview): both are done on the calling thread.
  WrestlerOurs(rdr::Renderer& renderer, const WrestlerFaults& faults, bool live);
  ~WrestlerOurs() override;
  WrestlerOurs(const WrestlerOurs&) = delete;
  WrestlerOurs& operator=(const WrestlerOurs&) = delete;

  const char* Name() const override { return "wrestlers"; }
  bool Claims(const rdr_adapter::DrawOrigin& origin) const override {
    return (origin.pass_site == rdr_adapter::kModelPartColourSite || origin.pass_site == rdr_adapter::kModelPartMirrorSite) && origin.object != 0;
  }
  bool FrameReady(const rdr_adapter::LoadedFrame& frame) override;
  bool Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, rdr_adapter::KeepWhy& why) override;
  void NoClaimFrame() override { early_ = false, bind_started_ = false; core_.NoClaimFrame(); }   // S101 lane 99: early_
  void WantJoinDrops(bool on) override { core_.SetDropsWanted(on), want_ = on; }   // lane 129 reviews F2 / F6
  uint32_t BufferReleaseSeq() const override { return buf_seq_; }
  void TakeReleasedBuffers(std::vector<uint64_t>& out) override { out.insert(out.end(), buf_released_.begin(), buf_released_.end()), buf_released_.clear(); }
  void TestReleaseOne() override { core_.TestReleaseOne(); }
  uint32_t JoinDropSeq() const override { return core_.DropSeq(); }   // lane 129 (KC)
  void TakeJoinDrops(std::vector<uint32_t>& out) override { core_.TakeDrops(out); }
  uint32_t PartPaletteCount(uint32_t object, uint32_t part) const override { return core_.PartPaletteCount(object, part); }   // lane 227
  uint32_t AssetEpoch() const override { return core_.GlobalReleaseSeq(); }   // lane 129 review F2: only ReleaseAll / Clear (every part gone, a load screen) voids the class's templates; a swept part is a BufferReleaseSeq event
  std::string WindowDetail() const override { return Report(); }   // S101 lane 100: the unjoined / mismatch causes in the live log

  void SetRecord(std::shared_ptr<const SceneTables> record);
  void SetGameRoot(const std::string& root);
  std::string Report() const;
  const WrestlerCore& core() const { return core_; }

 private:
  bool FrameReadyImpl(const rdr_adapter::LoadedFrame& frame);   // S101 lane 99: the body of FrameReady, also run early for the shadow casters (ShadowJoinBoard)
  bool early_ = false, early_ready_ = false;
  bool bind_started_ = false, bind_ready_ = false;   // lane 129 review F3: this bind's FrameReady already started the core (the shadow side must not start it again)
  uint64_t bind_key_ = 0;
  uint64_t early_key_ = 0, early_serial_ = 0;   // S101 lane 99 (F2): the frame the early start was for (capture frame + guest frame) and the shadow hook's bind serial
  static uint64_t FrameKey(const rdr_adapter::LoadedFrame& f) { return (uint64_t(f.header.capture_frame) << 40) ^ f.header.guest_frame ^ 0x5a5a000000000000ull; }                       // S101 lane 99: the shadow hook already started this bind's frame; FrameReady then returns its answer instead of starting it twice
  void Upload(WrestlerPrepared& p);
  void Release(WrestlerPrepared& p);

  rdr::Renderer& renderer_;
  bool live_;
  assets::wrestler_roster::Roster roster_;
  WrestlerCore core_;
  std::mutex mu_;
  std::shared_ptr<const SceneTables> pending_record_;
  bool record_dirty_ = false;
  std::string root_;
  bool roster_started_ = false, said_roster_failed_ = false, said_roster_ready_ = false;   // lane 147: said_roster_ready_ = the render_signal line was posted
  double upload_ms_ = 0;
  bool want_ = false;
  uint32_t buf_seq_ = 0;
  std::vector<uint64_t> buf_released_;   // lane 129 review F2: the buffer ids Release destroyed, until TakeReleasedBuffers
};

// The factory for InstallReplaceFactories() (replace_factories.h): the faults come from --replace-fault.
ReplaceHookHandle MakeWrestlersHook(const ReplaceFactoryArgs& args);

}  // namespace rdr_scene
