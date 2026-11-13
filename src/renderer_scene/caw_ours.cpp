// See caw_ours.h. Session 101, lane 153.
#include "caw_ours.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "../renderer/problem.h"
#include "shadow_join_board.h"   // lane 225
#include "../renderer/shaders/spv/fx_registry.h"   // lane 169: the vertex variant's constant ranges (the plants)

namespace rdr_scene {

using rdr_adapter::KeepWhy;

CawOurs::CawOurs(rdr::Renderer& renderer, const CawFaults& faults, bool live) : renderer_(renderer), faults_(faults), live_(live), core_(roster_, faults, /*sync=*/!live) {
  core_.SetReleaseHook([this](WrestlerPrepared& p) { Release(p); });
  {   // lane 225: the editor's body parts, read only by the shadow casters and the rim map (ShadowJoinBoard::caw_join)
    ShadowJoinBoard& board = g_shadow_join_board;
    board.caw_owner = this;
    board.caw_join = [this](uint32_t object, uint32_t part, CawShadowPart& out) { return core_.JoinedPart(object, part, out.model, out.mesh, out.group, out.scale); };
  }
}

CawOurs::~CawOurs() {
  if (g_shadow_join_board.caw_owner == this) g_shadow_join_board.caw_owner = nullptr, g_shadow_join_board.caw_join = nullptr;   // lane 225
  core_.Clear();
  roster_.Join();
}

void CawOurs::SetGameRoot(const std::string& root) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (root.empty() || roster_started_ || root == root_) return;
    root_ = root;
    if (!live_) return;   // fview / the checker build the index in FrameReady, synchronously
    roster_started_ = true;
  }
  // lane 243: live, the edit-part index starts as soon as the game folder is known (the live reader tells every frame), not at the first claimed 3D frame. A cold index (no cache file yet: the
  // first start, or a test run whose user-data copy is fresh) takes 30-60 s on its worker; started at the first claim it was still loading while the editor's 3D pages went by (run 7160: ready
  // for 8 of 339 frames), and the hook answered NotOurs for all of them.
  roster_.BuildEditAsync(root, CachePath());
}

bool CawOurs::FrameReady(const rdr_adapter::LoadedFrame& frame) {
  std::string root;
  bool start = false;
  {
    std::lock_guard<std::mutex> lock(mu_);   // (review notes: roster_started_ is read and set under mu_, like SetGameRoot)
    root = root_;
    if (!roster_started_ && !root.empty()) roster_started_ = true, start = true;
  }
  if (start) {
    if (live_) roster_.BuildEditAsync(root, CachePath());   // the counts index is cached on the player's machine (ids and counts only): only the first start inflates the edit pacs (review S3)
    else {
      std::string err;
      if (!roster_.BuildEdit(root, err)) rdr::Log("[rdr][caw] the edit parts could not be read: %s", err.c_str());   // (fview / checker: no cache file is written)
    }
  }
  if (roster_.Poll() == 2 && !said_failed_) {
    said_failed_ = true;
    rdr::Log("[rdr][caw] the edit parts could not be read: %s", roster_.error().c_str());
  }
  return core_.BeginFrame(frame);
}

std::string CawOurs::CachePath() const {
  // The game's cache root (--cache_root, set from runtime()->cache_root() in wwe2k14_app.h, next to the shader list import). Never the launcher's settings folder; run_list points the
  // cache root at out\bench for test runs. Empty (fview, the checker, a build without it) = no cache file is read or written.
  const std::string& root = rdr_adapter::g_caw_cache_root;
  if (root.empty()) return std::string();
  std::error_code ec;
  const std::filesystem::path dir = std::filesystem::u8path(root);
  std::filesystem::create_directories(dir, ec);
  if (!std::filesystem::is_directory(dir, ec)) return std::string();
  return (dir / "edit_index.bin").string();
}

void CawOurs::Upload(WrestlerPrepared& p) {
  const auto t0 = std::chrono::steady_clock::now();
  p.gpu[0] = renderer_.CreateBuffer(p.vertex.size(), p.vertex.data());
  p.gpu[1] = p.index.empty() ? 0 : renderer_.CreateBuffer(p.index.size() * 2, p.index.data());
  if ((!p.gpu[0] || !p.gpu[1]) && (p.gpu[0] || p.gpu[1])) Release(p);
  upload_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void CawOurs::Release(WrestlerPrepared& p) {
  if (p.gpu[0]) renderer_.DestroyBuffer(rdr::BufferId(p.gpu[0]));
  if (p.gpu[1]) renderer_.DestroyBuffer(rdr::BufferId(p.gpu[1]));
  p.gpu[0] = p.gpu[1] = 0;
}

bool CawOurs::Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, KeepWhy& why) {
  if (!in.frame || !in.draw || !in.origin) return why = KeepWhy::Mismatch, false;
  CawDecision dec;
  if (!core_.Decide(*in.frame, in.draw_index, in.read_geometry, dec, why)) return false;
  WrestlerPrepared& p = *dec.prepared;
  if (!p.gpu[0]) Upload(p);
  if (!p.gpu[0] || p.index.empty() || !p.gpu[1]) {
    core_.UploadFailed();
    return why = KeepWhy::Mismatch, false;
  }
  out.vertex = {rdr::BufferId(p.gpu[0]), 0};
  out.index = {rdr::BufferId(p.gpu[1]), 0};
  out.index_count = uint32_t(p.index.size());
  out.vertex_layout = in.stream_layout;   // the vertex we built IS in the stream draw's host layout
  out.topology = in.stream_topology;
  out.index_32bit = in.stream_index_32bit;
  out.first_register = 76, out.rows = 0, out.row_data = nullptr;   // the stream's own palette rows
  // lane 169 (N3b): a backdrop piece (bg66.pac) keeps EVERYTHING but its two buffers from the stream: its state, its pictures and its constants, so the scroll (the vertex shader's Time * UVScroll0,
  // constants r186 / r203 of the stream draw) follows the editor's own clock by itself. The plants below act on exactly those two things and must move the picture.
  if (dec.backdrop) {
    if (faults_.frozen_scroll && in.vs >= 0 && in.vs_block) {   // PLANT: the clock constant frozen at the editor's first tick (0): the card stops scrolling
      const rdr::fx::Variant& v = rdr::fx::kVariants[in.vs];
      for (uint32_t r = 0; r < v.constant_count; ++r)
        if (v.constants[r].reg == 186 && v.constants[r].count == 1) { static const float zero[4] = {0, 0, 0, 0}; out.first_register = 186, out.rows = 1, out.row_data = zero; }
    }
    const int slot_piece = dec.src_leaf == 3 ? 1 : 0;   // (plant only) which piece this is: entry 3 = CAS_moya (the card), else entry 0 = CAS_wall
    if (in.textures && in.textures->slots[0].image != rdr::kNoImage) {
      const rdr::ImageId mine = in.textures->slots[0].image;
      if (faults_.wrong_picture && bg_image_[1 - slot_piece] != rdr::kNoImage && bg_image_[1 - slot_piece] != mine) out.slots[0] = {true, bg_image_[1 - slot_piece], rdr::Format::Undefined, {0, 0, 0, 0}};   // PLANT: the other piece's picture
      bg_image_[slot_piece] = mine;
    }
  }
  return true;
}

std::string CawOurs::Report() const {
  char b[120];
  std::snprintf(b, sizeof b, " | backdrop replaced %llu | upload %.2f ms | edit parts %zu", (unsigned long long)core_.stats().backdrop_replaced, upload_ms_, roster_.edit_parts().size());
  return core_.Report() + (roster_.Poll() == 1 ? std::string(b) : std::string(" | edit parts not ready"));
}

ReplaceHookHandle MakeCawHook(const ReplaceFactoryArgs& args) {
  ReplaceHookHandle h;
  if (!args.renderer) {
    h.error = "no renderer";
    return h;
  }
  CawFaults faults;
  std::string err;
  if (!CawFaults::Parse(args.fault, faults, err)) {
    h.error = err;
    return h;
  }
  auto hook = std::make_unique<CawOurs>(*args.renderer, faults, args.live);
  CawOurs* raw = hook.get();
  if (!args.game_root.empty()) raw->SetGameRoot(args.game_root);
  h.set_game_root = [raw](const std::string& root) { raw->SetGameRoot(root); };
  h.report = [raw]() { return raw->Report(); };
  h.hook = std::move(hook);
  return h;
}

}  // namespace rdr_scene
