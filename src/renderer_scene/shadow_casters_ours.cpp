// See shadow_casters_ours.h. Session 101, lane 99.
#include "shadow_casters_ours.h"

#include <chrono>
#include <cstdio>
#include <exception>

#include "../renderer/problem.h"

namespace rdr_scene {

using rdr_adapter::KeepWhy;

ShadowCastersOurs::ShadowCastersOurs(rdr::Renderer& renderer, const ShadowOptions& options, bool live) : renderer_(renderer), live_(live), core_(options) {
  core_.SetReleaseHook([this](ShadowPrepared& p) { Release(p); });
}

ShadowCastersOurs::~ShadowCastersOurs() {
  if (proxy_future_.valid()) proxy_future_.wait();
  core_.Clear();
}

void ShadowCastersOurs::SetGameRoot(const std::string& root) {
  std::lock_guard<std::mutex> lock(mu_);
  if (root.empty() || proxy_started_ || root == root_) return;
  root_ = root;
}

bool ShadowCastersOurs::FrameReady(const rdr_adapter::LoadedFrame& frame) {
  if (!proxy_started_) {
    std::string root;
    {
      std::lock_guard<std::mutex> lock(mu_);
      root = root_;
    }
    if (!root.empty()) {   // chCommon.pac entry 0: once. Live: on a worker (a plane draw waits as "not ready"); fview: here
      proxy_started_ = true;
      auto work = [root]() {
        std::string err;
        auto set = assets::chcommon_proxy::Load(root, err);
        if (!set) rdr::Log("[rdr][shadow_casters] chCommon.pac entry 0 could not be read: %s", err.c_str());
        return set;
      };
      proxy_future_ = std::async(live_ ? std::launch::async : std::launch::deferred, work);
    }
  }
  if (proxy_future_.valid() && (!live_ || proxy_future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)) {
    std::shared_ptr<const assets::chcommon_proxy::ProxySet> set;
    try {
      set = proxy_future_.get();
    } catch (const std::exception& e) {   // a worker that threw (out of memory, a bad file): the plane proxy stays the stream's (review F8)
      rdr::Log("[rdr][shadow_casters] chCommon.pac entry 0 could not be read: %s", e.what());
    } catch (...) {
      rdr::Log("[rdr][shadow_casters] chCommon.pac entry 0 could not be read");
    }
    core_.SetProxy(std::move(set));
    proxy_future_ = {};
  }
  return core_.BeginFrame(frame);
}

void ShadowCastersOurs::Release(ShadowPrepared& p) {
  if (p.gpu[0]) renderer_.DestroyBuffer(rdr::BufferId(p.gpu[0]));
  if (p.gpu[1]) renderer_.DestroyBuffer(rdr::BufferId(p.gpu[1]));
  p.gpu[0] = p.gpu[1] = 0;
}

bool ShadowCastersOurs::Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, KeepWhy& why) {
  const ShadowCore::Decision d = core_.Decide(in);
  if (!d.ok) {
    why = d.why;
    return false;
  }
  ShadowPrepared& p = *d.prepared;
  if (!p.gpu[0]) {
    const auto t0 = std::chrono::steady_clock::now();
    p.gpu[0] = renderer_.CreateBuffer(p.vertex.size(), p.vertex.data());
    p.gpu[1] = p.index.empty() ? 0 : renderer_.CreateBuffer(p.index.size() * 2, p.index.data());
    upload_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!p.gpu[0] || !p.gpu[1]) {   // a failure is remembered (no retry on every draw) and the draw stays the stream's
      Release(p);
      core_.UploadFailed(&p, d.klass, in.origin && in.origin->pass_site == rdr_adapter::kPlaneShadowSite);
      return why = KeepWhy::Mismatch, false;
    }
  }
  out.vertex = {rdr::BufferId(p.gpu[0]), 0};
  out.index = {rdr::BufferId(p.gpu[1]), 0};
  out.index_count = uint32_t(p.index.size());
  // The stream's own geometry description: our vertex IS in the stream draw's shadow host layout and our index buffer IS the stream's strip.
  out.vertex_layout = in.stream_layout;
  out.topology = in.stream_topology;
  out.index_32bit = in.stream_index_32bit;
  out.first_register = d.first_register, out.rows = d.rows, out.row_data = d.row_data;
  return true;
}

std::string ShadowCastersOurs::Report() const {
  char b[120];
  std::snprintf(b, sizeof b, " | upload %.2f ms, parts resident %zu, chCommon proxy %s", upload_ms_, core_.prepared_count(), core_.has_proxy() ? "loaded" : "not loaded");
  return core_.Report() + b;
}

ReplaceHookHandle MakeShadowCastersHook(const ReplaceFactoryArgs& args) {
  ReplaceHookHandle h;
  if (!args.renderer) {
    h.error = "no renderer";
    return h;
  }
  ShadowOptions o;
  o.verify_all = !args.live;
  std::string err;
  if (!ShadowFaults::Parse(args.fault, o.faults, err)) {
    h.error = err;
    return h;
  }
  auto hook = std::make_unique<ShadowCastersOurs>(*args.renderer, o, args.live);
  ShadowCastersOurs* raw = hook.get();
  if (!args.game_root.empty()) raw->SetGameRoot(args.game_root);
  h.set_game_root = [raw](const std::string& root) { raw->SetGameRoot(root); };
  h.report = [raw]() { return raw->Report(); };
  h.hook = std::move(hook);
  return h;
}

}  // namespace rdr_scene
