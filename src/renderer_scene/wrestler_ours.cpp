// See wrestler_ours.h. Session 101, lane 45.
#include "wrestler_ours.h"

#include <chrono>
#include <cstdio>

#include "../render_signal.h"   // lane 147
#include "../renderer/problem.h"

namespace rdr_scene {

using rdr_adapter::KeepWhy;

WrestlerOurs::WrestlerOurs(rdr::Renderer& renderer, const WrestlerFaults& faults, bool live)
    : renderer_(renderer), live_(live), core_(roster_, faults, /*sync=*/!live, /*verify_all=*/!live) {
  core_.SetReleaseHook([this](WrestlerPrepared& p) { Release(p); });
  {   // S101 lane 99: the joins the shadow casters read (ShadowJoinBoard), read only
    ShadowJoinBoard& board = g_shadow_join_board;
    board.wrestlers_owner = this;
    board.wrestlers_begin = [this](const rdr_adapter::LoadedFrame& frame, uint64_t serial) {
      const uint64_t key = FrameKey(frame);
      // lane 124 (review B1): deduped on the frame key alone (early_ is cleared at the end of every bind, so a split frame's second bind starts again, once); the plant serial_running keys on the number too (the old rule)
      if (early_ && early_key_ == key && (!g_wrestlers_starts.serial_keyed_plant || early_serial_ == serial)) return ++g_wrestlers_starts.early_hits, early_ready_;
      // lane 129 review F3: the thin binder (or any first claimed draw) already ran this bind's FrameReady: that start IS this bind's, whoever calls first; the shadow side takes its answer (one begin per bind)
      if (bind_started_ && bind_key_ == key && !g_wrestlers_starts.serial_keyed_plant) return bind_started_ = false, ++g_wrestlers_starts.early_hits, bind_ready_;
      early_ready_ = FrameReadyImpl(frame);
      early_ = true, early_key_ = key, early_serial_ = serial;
      return early_ready_;
    };
    board.wrestlers_join = [this](const rdr_adapter::LoadedFrame& frame, uint32_t object, const ShadowReadGeometry& read, ShadowBody& out) { return core_.JoinOf(frame, object, read, out); };
  }
}

WrestlerOurs::~WrestlerOurs() {
  if (g_shadow_join_board.wrestlers_owner == this) g_shadow_join_board.wrestlers_owner = nullptr, g_shadow_join_board.wrestlers_begin = nullptr, g_shadow_join_board.wrestlers_join = nullptr;   // S101 lane 99
  core_.Clear();   // (releases every buffer through Release)
  roster_.Join();
}

void WrestlerOurs::SetRecord(std::shared_ptr<const SceneTables> record) {
  std::lock_guard<std::mutex> lock(mu_);
  pending_record_ = std::move(record);
  record_dirty_ = true;
}

void WrestlerOurs::SetGameRoot(const std::string& root) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (root.empty() || roster_started_ || root == root_) return;
    root_ = root;
    if (!live_) return;   // fview / the checkers build the roster in FrameReadyImpl, synchronously
    roster_started_ = true;
  }
  // lane 253 (as lane 243 did for the editor's index): live, the roster's counts scan (102 pacs, 6-7 s on its worker) starts as soon as the game folder is known (the live reader tells it every
  // frame), not at the first frame that claims a model draw. Started at the first claim, a match that loaded within seconds of the start had its first frames refused as "not ready" (board:
  // Wrestlers ours 79-91 % of FRAMES in the sweep runs; fallback frames stop growing once the roster is ready, thin share per draw is already ~100 %).
  roster_.BuildAsync(root);
}

bool WrestlerOurs::FrameReady(const rdr_adapter::LoadedFrame& frame) {
  ++g_wrestlers_starts.binds;   // lane 124
  if (early_) {   // S101 lane 99: the shadow casters started this bind's frame (the shadow passes draw before the colour pass); only for the SAME frame (header frame numbers), else it is stale
    early_ = false;
    if (early_key_ == FrameKey(frame)) return early_ready_;
  }
  const bool ready = FrameReadyImpl(frame);
  bind_started_ = true, bind_key_ = FrameKey(frame), bind_ready_ = ready;   // lane 129 review F3
  return ready;
}

bool WrestlerOurs::FrameReadyImpl(const rdr_adapter::LoadedFrame& frame) {
  ++g_wrestlers_starts.starts;   // lane 124
  std::string root;
  bool start_roster = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (record_dirty_) {
      core_.SetRecord(pending_record_);
      record_dirty_ = false;
    }
    root = root_;
    start_roster = !roster_started_ && !root.empty();
    if (start_roster) roster_started_ = true;
  }
  if (start_roster) {
    if (live_) roster_.BuildAsync(root);   // (normally already started by SetGameRoot; this is the path for a root that arrived by another way)
    else {
      std::string err;
      if (!roster_.Build(root, err)) rdr::Log("[rdr][wrestlers] the roster could not be read: %s", err.c_str());
    }
  }
  const int roster_state = roster_.Poll();   // lane 147
  if (roster_state == 2 && !said_roster_failed_) {   // the worker's failure is said once, at the first frame after it (not silently "not ready" forever)
    said_roster_failed_ = true;
    rdr::Log("[rdr][wrestlers] the roster could not be read: %s", roster_.error().c_str());
    wwe2k14::render_signal::Post("roster_failed", "");   // lane 147: queued; the guest frame's event bus emits it
  }
  if (roster_state == 1 && !said_roster_ready_) {   // lane 147: once per roster build; the counts are the worker's, read only after state 1
    said_roster_ready_ = true;
    char f[160];
    std::snprintf(f, sizeof f, "\"pacs\": %zu, \"models\": %zu, \"seconds\": %.1f", roster_.pacs(), roster_.entries().size(), roster_.seconds());
    wwe2k14::render_signal::Post("roster_ready", f);
  }
  return core_.BeginFrame(frame);
}

void WrestlerOurs::Upload(WrestlerPrepared& p) {
  const auto t0 = std::chrono::steady_clock::now();
  p.gpu[0] = renderer_.CreateBuffer(p.vertex.size(), p.vertex.data());
  p.gpu[1] = p.index.empty() ? 0 : renderer_.CreateBuffer(p.index.size() * 2, p.index.data());
  if ((!p.gpu[0] || !p.gpu[1]) && (p.gpu[0] || p.gpu[1])) Release(p);   // half made: free the half (the next draw tries again)
  upload_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void WrestlerOurs::Release(WrestlerPrepared& p) {
  if (want_) {   // lane 129 review F2: tell the thin binder which buffers went
    if (p.gpu[0]) buf_released_.push_back(p.gpu[0]);
    if (p.gpu[1]) buf_released_.push_back(p.gpu[1]);
    ++buf_seq_;
  }
  if (p.gpu[0]) renderer_.DestroyBuffer(rdr::BufferId(p.gpu[0]));
  if (p.gpu[1]) renderer_.DestroyBuffer(rdr::BufferId(p.gpu[1]));
  p.gpu[0] = p.gpu[1] = 0;
}

bool WrestlerOurs::Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, KeepWhy& why) {
  if (!in.frame || !in.draw || !in.origin) return why = KeepWhy::Mismatch, false;
  WrestlerDecision dec;
  if (!core_.Decide(*in.frame, in.draw_index, in.read_geometry, dec, why)) return false;
  WrestlerPrepared& p = *dec.prepared;
  if (!p.gpu[0]) Upload(p);
  if (!p.gpu[0] || p.index.empty() || !p.gpu[1]) {
    core_.UploadFailed(dec);   // Decide counted the draw as replaced: correct it, the stream's draw stays
    return why = KeepWhy::Mismatch, false;
  }
  out.vertex = {rdr::BufferId(p.gpu[0]), 0};
  out.index = {rdr::BufferId(p.gpu[1]), 0};
  out.index_count = uint32_t(p.index.size());
  // The stream's own geometry description: the vertex we built IS in the stream draw's host layout, the file's strip IS the stream's index buffer.
  out.vertex_layout = in.stream_layout;
  out.topology = in.stream_topology;
  out.index_32bit = in.stream_index_32bit;
  out.first_register = dec.first_register;
  out.rows = dec.rows;
  out.row_data = dec.row_data;
  return true;
}

std::string WrestlerOurs::Report() const {
  char b[200];
  char fr[160];
  std::snprintf(fr, sizeof fr, " | frames ready %llu, not ready %llu (no record %llu, roster %llu)", (unsigned long long)core_.stats().frames_ready, (unsigned long long)core_.stats().frames_not_ready,
                (unsigned long long)core_.stats().not_ready_no_record, (unsigned long long)core_.stats().not_ready_roster);   // lane 253
  // The roster's counts are the worker's until it says it is done (state 1): read them only then.
  if (roster_.Poll() == 1) std::snprintf(b, sizeof b, " | upload %.2f ms, parts resident %zu | roster %zu pacs %zu models %.1f s", upload_ms_, core_.prepared_count(), roster_.pacs(), roster_.entries().size(), roster_.seconds());
  else std::snprintf(b, sizeof b, " | upload %.2f ms, parts resident %zu | roster %s", upload_ms_, core_.prepared_count(), roster_.Poll() == 2 ? "FAILED" : "not ready");
  return core_.Report() + fr + b;
}

ReplaceHookHandle MakeWrestlersHook(const ReplaceFactoryArgs& args) {
  ReplaceHookHandle h;
  if (!args.renderer) {
    h.error = "no renderer";
    return h;
  }
  WrestlerFaults faults;
  std::string err;
  if (!WrestlerFaults::Parse(args.fault, faults, err)) {
    h.error = err;
    return h;
  }
  auto hook = std::make_unique<WrestlerOurs>(*args.renderer, faults, args.live);
  WrestlerOurs* raw = hook.get();
  if (!args.game_root.empty()) raw->SetGameRoot(args.game_root);
  h.set_record = [raw](std::shared_ptr<const SceneTables> r) { raw->SetRecord(std::move(r)); };
  h.set_game_root = [raw](const std::string& root) { raw->SetGameRoot(root); };
  h.report = [raw]() { return raw->Report(); };
  h.hook = std::move(hook);
  return h;
}

}  // namespace rdr_scene
