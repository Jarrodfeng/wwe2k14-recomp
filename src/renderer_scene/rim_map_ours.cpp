// See rim_map_ours.h. Session 101, lane 124.
#include "rim_map_ours.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace rdr_scene {

using rdr_adapter::KeepWhy;

bool RimFaults::Parse(const std::string& list, RimFaults& f, std::string& error) {
  std::string rest;   // the names that are not ours go to the shadow casters' parser (no_verify, the writer's faults)
  size_t at = 0;
  while (at <= list.size()) {
    size_t comma = list.find(',', at);
    if (comma == std::string::npos) comma = list.size();
    const std::string w = list.substr(at, comma - at);
    at = comma + 1;
    if (w.empty()) continue;
    if (w == "no_pose") f.no_pose = true;
    else if (w == "first_object") f.first_object = true;
    else if (w == "wrong_target") f.wrong_target = true;
    else if (w == "serial_running") f.serial_running = true;
    else rest += (rest.empty() ? "" : ",") + w;
  }
  if (!ShadowFaults::Parse(rest, f.core, error)) {
    error = "unknown rim map fault in '" + list + "': " + error + " (also no_pose, first_object, wrong_target, serial_running)";
    return false;
  }
  if (f.core.palette_from_colour || f.core.proxy_part_shift || f.core.arena_stale || f.core.flat_indices) {   // the shadow passes' own plants mean nothing on the rim map: refuse loudly
    error = "rim map fault '" + list + "': palette_from_colour, proxy_part_shift, arena_stale and flat_indices are the shadow casters' plants (use shadow.NAME)";
    return false;
  }
  return true;
}

RimMapOurs::RimMapOurs(rdr::Renderer& renderer, const RimFaults& faults, bool live) : renderer_(renderer), faults_(faults), core_([&] {
                                                                                         ShadowOptions o;
                                                                                         o.faults = faults.core;
                                                                                         o.verify_all = !live;
                                                                                         return o;
                                                                                       }()) {
  core_.SetReleaseHook([this](ShadowPrepared& p) { Release(p); });
  if (faults.serial_running) core_.SetSerialDrift(true), g_wrestlers_starts.serial_keyed_plant = true;
}

RimMapOurs::~RimMapOurs() {
  core_.Clear();
  g_wrestlers_starts.serial_keyed_plant = false;
}

bool RimMapOurs::Claims(const rdr_adapter::DrawOrigin& o) const {
  // wrong_target (plant): the hook claims the wrestler shadow maps' draws instead of the rim map's
  const uint32_t site = faults_.wrong_target ? rdr_adapter::kShadowMapSite : rdr_adapter::shadow_roles::kRimPassSite;
  return o.pass_site == site && o.object != 0;
}

bool RimMapOurs::FrameReady(const rdr_adapter::LoadedFrame& frame) {
  first_object_ = 0;
  return core_.BeginFrame(frame);
}

void RimMapOurs::Release(ShadowPrepared& p) {
  if (p.gpu[0]) renderer_.DestroyBuffer(rdr::BufferId(p.gpu[0]));
  if (p.gpu[1]) renderer_.DestroyBuffer(rdr::BufferId(p.gpu[1]));
  p.gpu[0] = p.gpu[1] = 0;
}

bool RimMapOurs::Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, KeepWhy& why) {
  rdr_adapter::ReplaceIn planted;
  rdr_adapter::DrawOrigin planted_origin;
  const rdr_adapter::ReplaceIn* use = &in;
  if (faults_.first_object && in.origin) {   // plant: one object's join for every object of the frame
    if (!first_object_) first_object_ = in.origin->object;
    planted = in;
    planted_origin = *in.origin;
    planted_origin.object = first_object_;
    planted.origin = &planted_origin;
    use = &planted;
  }
  ShadowCore::Decision d = core_.Decide(*use);
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
      core_.UploadFailed(&p, d.klass, false);
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
  if (faults_.no_pose) {   // plant: the bind pose (identity 3x4 bones) in place of the frame's palette
    const uint32_t bones = std::max<uint32_t>(d.palette_count, 1);
    pose_rows_.assign(size_t(bones) * 12, 0.0f);
    for (uint32_t b = 0; b < bones; ++b)
      for (uint32_t r = 0; r < 3; ++r) pose_rows_[size_t(b) * 12 + r * 4 + r] = 1.0f;
    out.first_register = 76, out.rows = bones * 3, out.row_data = pose_rows_.data();
  }
  return true;
}

std::string RimMapOurs::Report() const {
  const ShadowStats& s = core_.stats();
  char b[900];
  std::snprintf(b, sizeof b,
                "rim map: claimed %llu replaced %llu | kept: not ours %llu, unjoined %llu | verify %llu bad %llu, layout refused %llu, rows overridden %llu, joins dropped %llu, parts built %llu released %llu | "
                "upload %.2f ms, parts resident %zu | frames ready %llu not ready %llu | wrestlers binds %llu starts %llu early hits %llu",
                (unsigned long long)s.claimed, (unsigned long long)s.replaced, (unsigned long long)s.kept_map_not_ours, (unsigned long long)s.kept[uint8_t(KeepWhy::Unjoined)],
                (unsigned long long)s.verify_runs, (unsigned long long)s.verify_bad, (unsigned long long)s.layout_refused, (unsigned long long)s.rows_overridden, (unsigned long long)s.joins_dropped,
                (unsigned long long)s.prepared_built, (unsigned long long)s.released, upload_ms_, core_.prepared_count(), (unsigned long long)s.frames_ready, (unsigned long long)s.frames_not_ready,
                (unsigned long long)g_wrestlers_starts.binds, (unsigned long long)g_wrestlers_starts.starts, (unsigned long long)g_wrestlers_starts.early_hits);
  return b;
}

ReplaceHookHandle MakeRimMapHook(const ReplaceFactoryArgs& args) {
  ReplaceHookHandle h;
  if (!args.renderer) {
    h.error = "no renderer";
    return h;
  }
  RimFaults f;
  std::string err;
  if (!RimFaults::Parse(args.fault, f, err)) {
    h.error = err;
    return h;
  }
  auto hook = std::make_unique<RimMapOurs>(*args.renderer, f, args.live);
  RimMapOurs* raw = hook.get();
  h.report = [raw]() { return raw->Report(); };
  h.hook = std::move(hook);
  return h;
}

}  // namespace rdr_scene
