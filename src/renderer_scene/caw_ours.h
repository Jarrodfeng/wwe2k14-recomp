// The Create a Superstar editor's wrestler drawn by us in place (Session 101, lane 153 = N3; docs\renderer\CAW_OURS.md). A DrawReplaceHook for the main-pass model-part draws (pass sites
// 0x82C0A950 / 0x82C0B94C): each claimed stream draw whose counts and BYTES equal a mesh of a pac\edit\edNN.pac leaf is replaced IN PLACE by our geometry (the file's strip in the stream
// draw's host layout); the bone palette, the state, the textures and the draw's place stay the stream's (no scene record is needed: rows = 0). Every decision is CawCore's (caw_core.h,
// GPU-free, shared with the offline checker). Registered after the wrestlers: a body that fits a chNNN.pac model is theirs, everything else passes here, and a draw that fits no edit leaf
// is NotOurs and goes on to the next hook. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Threads: SetGameRoot from any thread; everything else on the render thread.
#pragma once
#include <memory>
#include <mutex>
#include <string>

#include "../assets/wrestler_roster.h"
#include "../renderer/renderer.h"
#include "../renderer_adapter/draw_replace_hook.h"
#include "caw_core.h"
#include "replace_factories.h"

namespace rdr_scene {

class CawOurs final : public rdr_adapter::DrawReplaceHook {
 public:
  CawOurs(rdr::Renderer& renderer, const CawFaults& faults, bool live);
  ~CawOurs() override;
  CawOurs(const CawOurs&) = delete;
  CawOurs& operator=(const CawOurs&) = delete;
  const char* Name() const override { return "caw"; }
  bool Claims(const rdr_adapter::DrawOrigin& origin) const override {
    return (origin.pass_site == rdr_adapter::kModelPartColourSite || origin.pass_site == rdr_adapter::kModelPartMirrorSite) && origin.object != 0;
  }
  bool FrameReady(const rdr_adapter::LoadedFrame& frame) override;
  void NoClaimFrame() override { core_.IdleFrame(); }   // review SF-b (lane 225)
  bool Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, rdr_adapter::KeepWhy& why) override;
  std::string WindowDetail() const override { return Report(); }
  void SetGameRoot(const std::string& root);
  std::string Report() const;

 private:
  std::string CachePath() const;
  void Upload(WrestlerPrepared& p);
  void Release(WrestlerPrepared& p);
  rdr::Renderer& renderer_;
  CawFaults faults_;                       // lane 169: the hook-level plants (frozen_scroll, wrong_picture)
  rdr::ImageId bg_image_[2] = {rdr::kNoImage, rdr::kNoImage};   // lane 169 (wrong_picture plant only): the last slot-0 picture of each backdrop piece
  bool live_;
  assets::wrestler_roster::Roster roster_;
  CawCore core_;
  std::mutex mu_;
  std::string root_;
  bool roster_started_ = false, said_failed_ = false;
  double upload_ms_ = 0;
};

ReplaceHookHandle MakeCawHook(const ReplaceFactoryArgs& args);   // for InstallReplaceFactories() (the faults come from --replace-fault)

}  // namespace rdr_scene
