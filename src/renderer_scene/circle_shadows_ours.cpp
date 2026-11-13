// See circle_shadows_ours.h. Session 101, lane 136 (SH3 slice 1).
#include "circle_shadows_ours.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#include "../renderer/shaders/spv/fx_registry.h"

namespace rdr_scene {

using rdr_adapter::KeepWhy;

CircleShadowsOurs::CircleShadowsOurs(rdr::Renderer& renderer, const circles::Faults& faults, bool live) : renderer_(renderer), faults_(faults) { (void)live; }

CircleShadowsOurs::~CircleShadowsOurs() { ReleaseMesh(); }

void CircleShadowsOurs::ReleaseMesh() {
  if (mesh_gpu_[0]) renderer_.DestroyBuffer(rdr::BufferId(mesh_gpu_[0]));
  if (mesh_gpu_[1]) renderer_.DestroyBuffer(rdr::BufferId(mesh_gpu_[1]));
  mesh_gpu_[0] = mesh_gpu_[1] = 0;
  mesh_uploaded_ = mesh_verified_ = false;
}

void CircleShadowsOurs::SetRecord(const std::shared_ptr<const SceneTables>& record) {
  if (!record) return;
  if (faults_.late_record && ++records_seen_ > 40) {   // plant: from the 40th record on, each record reaches the board one record late
    std::shared_ptr<const SceneTables> older = held_;
    held_ = record;
    if (older) SetRecordNow(older);
    return;
  }
  SetRecordNow(record);
}

void CircleShadowsOurs::SetRecordNow(const std::shared_ptr<const SceneTables>& record) {
  if (!record) return;
  if (!record->circle_shadows.empty()) {
    for (const CircleShadow& e : record->circle_shadows) first_pose_.emplace((uint64_t(e.slot) << 32) | e.character, e);   // (emplace keeps the first)
    if (faults_.offset_shift && ++shift_seen_ > 40) {   // plant: the stamp -> frame offset moves by one mid-run
      std::vector<CircleShadow> shifted = record->circle_shadows;
      for (CircleShadow& e : shifted) ++e.frame;
      board_.Add(shifted);
    } else {
      board_.Add(record->circle_shadows);
    }
  }
  if (!record->circle_mesh.empty()) {
    const CircleMesh& m = record->circle_mesh[0];
    if (!board_.has_mesh() || std::memcmp(&board_.mesh(), &m, sizeof m) != 0) {   // a different mesh (a new arena? never seen): the buffers are rebuilt
      board_.SetMesh(m);
      ReleaseMesh();
    }
  }
}

bool CircleShadowsOurs::FrameReady(const rdr_adapter::LoadedFrame& frame) {
  ++stats_.frames;
  stream_stamp_ = uint32_t(frame.header.guest_frame);
  ordinal_ = 0;
  if (!board_.has_mesh() || !board_.stamps()) {
    ++stats_.frames_not_ready;
    return false;
  }
  return true;
}

bool CircleShadowsOurs::UploadMesh() {
  if (mesh_uploaded_) return true;
  CircleMesh m = board_.mesh();
  if (faults_.index_corrupt && m.index_count > 5) m.indices[5] = uint16_t((m.indices[5] + 1) % m.vertex_count);   // plant: one index of the uploaded mesh is wrong
  const auto t0 = std::chrono::steady_clock::now();
  mesh_gpu_[0] = uint64_t(renderer_.CreateBuffer(size_t(m.vertex_count) * kCircleVertexStride, m.vertices));
  mesh_gpu_[1] = uint64_t(renderer_.CreateBuffer(size_t(m.index_count) * 2, m.indices));
  stats_.upload_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (!mesh_gpu_[0] || !mesh_gpu_[1]) {
    ReleaseMesh();
    ++stats_.upload_failed;
    return false;
  }
  mesh_ = m;
  mesh_uploaded_ = true;
  return true;
}

bool CircleShadowsOurs::Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, KeepWhy& why) {
  ++stats_.claimed;
  const uint32_t k = ordinal_++;
  // The program: only the generated yCircleShadow vertex stage, whose three constant runs are at the block offsets we write (and read back to verify).
  uint32_t off_c0 = 0, off_vp = 0, off_pts = 0;
  bool have_c0 = false, have_vp = false, have_pts = false;
  if (in.vs < 0 || size_t(in.vs) >= std::size(rdr::fx::kVariants) || std::strncmp(rdr::fx::kVariants[in.vs].name, "yCircleShadow", 13) != 0) {
    ++stats_.kept_variant;
    why = KeepWhy::Variant;
    return false;
  }
  {
    const rdr::fx::Variant& v = rdr::fx::kVariants[in.vs];
    for (uint32_t r = 0; r < v.constant_count; ++r) {
      const rdr::fx::ConstantRange& c = v.constants[r];
      if (c.reg == 0 && c.count >= 1) off_c0 = c.offset, have_c0 = true;
      else if (c.reg == 4 && c.count >= 4) off_vp = c.offset, have_vp = true;
      else if (c.reg == 8 && c.count >= kCirclePoints) off_pts = c.offset, have_pts = true;
    }
  }
  circles::StreamRows stream;
  const bool have_stream = have_c0 && have_vp && have_pts && circles::ReadStreamRows(in.vs_block, in.vs_block_bytes, off_c0, off_vp, off_pts, stream);
  if (!have_stream && !faults_.no_verify) {
    ++stats_.kept_variant;
    why = KeepWhy::Variant;
    return false;
  }
  if (!board_.has_mesh() || in.index_count != board_.mesh().index_count || in.vertex_count != board_.mesh().vertex_count || in.stream_index_32bit || in.vertex_stride != kCircleVertexStride / 4u /* (the layout's guest stride is in dwords) */) {
    ++stats_.kept_mismatch;   // not the mesh we hold (counts, stride, index width): the stream's
    if (!stats_.kept_shape++) stats_.last_shape[0] = in.vertex_count, stats_.last_shape[1] = in.index_count, stats_.last_shape[2] = in.vertex_stride, stats_.last_shape[3] = in.stream_index_32bit, stats_.last_shape[4] = in.vs_block_bytes;
    why = KeepWhy::Mismatch;
    return false;
  }

  // ---- the join: by the stamp. join_by_index (plant) takes the previous record's entries instead.
  const CircleShadow* entry = nullptr;
  int used_delta = 0;
  circles::RowDiff local_diff;
  uint32_t local_stamp = 0;
  const auto try_stamp = [&](const std::vector<CircleShadow>* v, int d) {
    if (!v || k >= v->size()) return false;
    const CircleShadow& e = (*v)[k];
    if ((e.flags & (kCircleHaveViewProj | kCircleHaveParam)) != (kCircleHaveViewProj | kCircleHaveParam)) return false;   // an entry the writer could not complete (F7): never drawn
    const auto first = first_pose_.find((uint64_t(e.slot) << 32) | e.character);
    circles::BuildRows(e, faults_, first == first_pose_.end() ? nullptr : &first->second, rows_);
    if (!faults_.no_verify) {
      circles::RowDiff diff;
      if (!circles::RowsMatch(rows_, stream, faults_.float_compare, &diff)) {
        if (!local_diff.found) local_diff = diff, local_stamp = e.frame;   // lane 215: kept local; committed only if the draw is refused (a relearn probe that later succeeds is not a refusal)
        return false;
      }
    }
    entry = &e, used_delta = d;
    return true;
  };
  bool any_candidate = false;
  if (faults_.join_by_index) {
    const std::vector<CircleShadow>* v = board_.PreviousBatch();
    any_candidate = v && k < v->size();
    try_stamp(v, 0);
  } else if (delta_known_) {
    const std::vector<CircleShadow>* v = board_.At(stream_stamp_ - uint32_t(delta_));
    any_candidate = v && k < v->size();
    try_stamp(v, delta_);
  } else {
    for (int d = circles::kDeltaFirst; d <= circles::kDeltaLast && !entry; ++d) {
      const std::vector<CircleShadow>* v = board_.At(stream_stamp_ - uint32_t(d));
      any_candidate = any_candidate || (v && k < v->size());
      try_stamp(v, d);
    }
  }
  if (!entry) {
    if (local_diff.found && !stats_.first_diff.found) stats_.first_diff = local_diff, stats_.first_diff_stamp = local_stamp, stats_.first_diff_ordinal = k;   // lane 215 (SF3); with a known offset there is one attempt: the planned diagnostic
    // F3: why. A real constants mismatch (an entry was there and it was not the stream's); LATE (no entry for this frame and the newest stamp held is older than the one wanted: the record
    // is behind the stream); or unjoined (nothing near this frame). A known offset that misses counts toward the relearn in all three cases: a late record is an offset that moved.
    uint32_t newest = 0;
    const uint32_t wanted = stream_stamp_ - uint32_t(delta_known_ ? delta_ : circles::kDeltaLast);
    const bool older_only = board_.newest_stamp(newest) && int32_t(newest - wanted) < 0;
    if (any_candidate) {
      ++stats_.kept_mismatch;
      ++stats_.kept_rows;
      why = KeepWhy::Mismatch;
    } else if (older_only) {
      ++stats_.kept_mismatch;
      ++stats_.kept_late;
      why = KeepWhy::Mismatch;
    } else {
      ++stats_.kept_unjoined;
      why = KeepWhy::Unjoined;
    }
    if (delta_known_ && !faults_.static_lock && ++misses_ >= circles::kRelearn) delta_known_ = false, misses_ = 0, ++stats_.relearned;
    return false;
  }
  if (!faults_.join_by_index && !delta_known_) delta_known_ = true, delta_ = used_delta, misses_ = 0;
  else misses_ = 0;

  // ---- the mesh: uploaded once; checked once against the stream draw's own vertex AND index bytes (F2). Unreadable stream bytes keep the draw the stream's and the check is retried on the
  // next draw: the mesh is never taken on the record's word.
  if (!UploadMesh()) {
    why = KeepWhy::Mismatch;
    return false;
  }
  if (!mesh_verified_ && !faults_.no_verify) {
    std::vector<uint8_t> vbytes, ibytes;
    const size_t vn = size_t(in.vertex_count) * kCircleVertexStride, in_n = size_t(in.index_count) * 2;
    const bool readable = !faults_.mesh_unreadable && in.read_geometry && in.read_geometry(in.vertex_offset, uint32_t(vn), vbytes) && vbytes.size() == vn &&
                          in.read_geometry(in.index_offset, uint32_t(in_n), ibytes) && ibytes.size() == in_n;
    if (!readable) {
      ++stats_.mesh_unreadable;
      ++stats_.kept_mismatch;
      ++stats_.kept_mesh_wait;
      why = KeepWhy::Mismatch;
      return false;
    }
    ++stats_.mesh_checked;
    if (std::memcmp(vbytes.data(), mesh_.vertices, vn) != 0 || std::memcmp(ibytes.data(), mesh_.indices, in_n) != 0) {
      ++stats_.mesh_bad;
      if (std::memcmp(ibytes.data(), mesh_.indices, in_n) != 0) ++stats_.index_bad;
      ++stats_.kept_mismatch;
      why = KeepWhy::Mismatch;
      return false;
    }
    mesh_verified_ = true;
  }

  out.vertex = {rdr::BufferId(mesh_gpu_[0]), 0};
  out.index = {rdr::BufferId(mesh_gpu_[1]), 0};
  out.index_count = mesh_.index_count;
  out.index_32bit = 0;
  out.vertex_layout = in.stream_layout;   // the record's mesh IS in the stream's host layout
  out.topology = in.stream_topology;
  out.first_register = circles::kFirstRegister, out.rows = circles::kRows, out.row_data = rows_;
  ++stats_.replaced;
  ++stats_.by_delta[used_delta - circles::kDeltaFirst < 0 ? 0 : size_t(used_delta - circles::kDeltaFirst)];
  return true;
}

std::string CircleShadowsOurs::Report() const {
  char b[1100];
  std::snprintf(b, sizeof b,
                "circles: frames %llu (not ready %llu) claimed %llu replaced %llu | kept: unjoined %llu, mismatch %llu (shape %llu, constants %llu, late %llu, mesh wait %llu), variant %llu | join offset %d (stamps held %zu, relearned %llu, "
                "replaced by offset -2..3: %llu %llu %llu %llu %llu %llu) | mesh checked %llu bad %llu (index %llu) unreadable %llu, upload failed %llu, %.2f ms",
                (unsigned long long)stats_.frames, (unsigned long long)stats_.frames_not_ready, (unsigned long long)stats_.claimed, (unsigned long long)stats_.replaced,
                (unsigned long long)stats_.kept_unjoined, (unsigned long long)stats_.kept_mismatch, (unsigned long long)stats_.kept_shape, (unsigned long long)stats_.kept_rows,
                (unsigned long long)stats_.kept_late, (unsigned long long)stats_.kept_mesh_wait, (unsigned long long)stats_.kept_variant, delta_known_ ? delta_ : 99, board_.stamps(),
                (unsigned long long)stats_.relearned, (unsigned long long)stats_.by_delta[0], (unsigned long long)stats_.by_delta[1], (unsigned long long)stats_.by_delta[2],
                (unsigned long long)stats_.by_delta[3], (unsigned long long)stats_.by_delta[4], (unsigned long long)stats_.by_delta[5], (unsigned long long)stats_.mesh_checked,
                (unsigned long long)stats_.mesh_bad, (unsigned long long)stats_.index_bad, (unsigned long long)stats_.mesh_unreadable, (unsigned long long)stats_.upload_failed, stats_.upload_ms);
  std::string text = b;
  if (stats_.first_diff.found) {   // lane 215
    char t[200];
    std::snprintf(t, sizeof t, " | first refused constant: c%u lane %u stamp %u draw %u stream %08X record %08X", stats_.first_diff.reg, stats_.first_diff.lane, stats_.first_diff_stamp, stats_.first_diff_ordinal,
                  stats_.first_diff.stream_bits, stats_.first_diff.record_bits);
    text += t;
  }
  if (stats_.kept_shape) {
    char t[200];
    std::snprintf(t, sizeof t, " | first shape refusal: %u vertices, %u indices, stride %u, 32-bit index %u, vs block %u bytes", stats_.last_shape[0], stats_.last_shape[1], stats_.last_shape[2], stats_.last_shape[3], stats_.last_shape[4]);
    text += t;
  }
  return text;
}

ReplaceHookHandle MakeCirclesHook(const ReplaceFactoryArgs& args) {
  ReplaceHookHandle h;
  if (!args.renderer) {
    h.error = "no renderer";
    return h;
  }
  circles::Faults f;
  std::string err;
  if (!circles::Faults::Parse(args.fault, f, err)) {
    h.error = err;
    return h;
  }
  auto hook = std::make_unique<CircleShadowsOurs>(*args.renderer, f, args.live);
  CircleShadowsOurs* raw = hook.get();
  h.report = [raw]() { return raw->Report(); };
  h.set_record = [raw](std::shared_ptr<const SceneTables> r) { raw->SetRecord(r); };
  h.hook = std::move(hook);
  return h;
}

}  // namespace rdr_scene
