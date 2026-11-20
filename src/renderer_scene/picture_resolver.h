// PictureResolver (Session 103, landing 2, pieces PN2 / PN3 wiring; docs\renderer\PICTURES_NO_STREAM.md 5, 6, 10): the host's `HostOptions::picture` over the stream-free PictureSource. Copyright (c) the
// wwe2k14-recomp authors. BSD-3-Clause.
//
// HEADER-ONLY (a new .cpp is not in the main checkout's build.ninja until it is reconfigured, so the OFF link test could not see it; the unit that includes this header, the host's, compiles it).
// KEPT in the stream-free build: reaches only renderer\api.h, renderer.h, scene_record.h, ui_file_index.h and scene_live\picture_source.h (no stream header; tools\scene\keep_stream_lint.py).
// Host thread only for Resolve / EndFrame / Stop (the same thread as Renderer::RenderFrame; src\renderer_host\scene_host.h calls HostOptions::picture on it), plus ONE worker thread (LoadWorker) that does the
// file reads. The resolver never reads guest memory: the source's answer was made on the game's drawing thread at the row's first sight.
//   File answer   our image of the player's own pac picture, cached by index entry (outlives the game's handle; released after `kFileUnusedFrames` frames unused). The pac read + decode (77-124 ms for one picture,
//                 S103 PN4 finding) is done by LoadWorker (queue under a mutex, results on a done list); the host thread only drains the done list and uploads (CreateImage / UpdateImage: the renderer is host-thread
//                 only), within the per-frame upload budget. A row whose file is not ready is dropped `pending` (counted, retried next frame); nothing on the host thread ever reads a pac (lint: picture_resolver_lint.py).
//   Guest answer  our image of the guest bytes (BC1 / BC2 / BC3 / BGRA8, mip 0), cached by (key, serial): replaced when the source answers the key again (a caught refill), released when the key is gone;
//                 when the source rejoins the key to a FILE (more pacs indexed) the guest image keeps drawing until the file's image is ready (D3: no vanish, no stall);
//   Refused       an empty slot (the host drops the row `picture`, counted); the reason is counted here by name (gpu_picture / movie / format / size ...).
// Wiring: `opts.picture = resolver->Function();` + `opts.frame_done = EndFrame(frame)` + `opts.on_stop = Stop / destroy` (src\renderer_host\host_start.cpp); the game side (the source's feeders) is picture_hooks.h / picture_tap.h.
#pragma once
#include "../thread_name.h"  // S103 H1
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../renderer/api.h"
#include "../renderer/renderer.h"
#include "../scene_live/picture_source.h"
#include "scene_record.h"
#include "ui_file_index.h"

namespace rdr_scene {

// Rig twins only (tools\scene\host_rig.cpp --mode resolver --plant ...; never set by the game): 1 = c1_old (a file picture is stamped with the PREVIOUS record's frame, not the record that drew it), 2 = d3_old (no guest
// fallback while a rejoined file loads), 3 = d4_permanent (a failed file is never retried), 4 = t2f_old (no bridge: a first-sight File join draws nothing until its file loads), 5 = t2f_forever (a bridge image is never dropped once the file is ready).
inline int g_resolver_plant = 0;

class PictureResolver {
 public:
  static constexpr uint64_t kFileUnusedFrames = 3600;   // a file picture no row named for this many frames (~1 minute of a menu) is released
  static constexpr uint64_t kFailedRetryFrames = 600;   // D4: a file that failed to load is tried again after this many frames (~10 s of a menu); a transient read error is not permanent
  // A6: the per-frame budget of NEW pictures UPLOADED by the host thread (a decoded file's CreateImage + UpdateImage, or a guest-copy upload). Past either limit the rest waits (rows `pending`, counted, retried next frame:
  // nothing is marked failed). The first upload of a frame is always allowed, so a picture bigger than the time budget still arrives. The pac reads are not in this budget: they are on the worker.
  static constexpr uint32_t kLoadsPerFrame = 24;
  static constexpr double kLoadMsPerFrame = 6.0;
  struct Stats {
    uint64_t asked = 0, file = 0, guest = 0, missing = 0, load_failed = 0, upload_failed = 0, uploads_file = 0, uploads_guest = 0, released = 0;
    uint64_t scene_copy = 0;   // S105 A4: rows of the game's scene-colour copy answered with the host's own 3D picture (not a refusal)
    uint64_t pending = 0, loads = 0, max_loads_frame = 0;   // pending = rows dropped while their picture was not ready (queued on the worker or over the upload budget); max_loads_frame = most uploads in one frame
    uint64_t pending_frames = 0, last_pending_frame = 0, fallback_rows = 0, bridge_rows = 0, failed_retries = 0;   // pending_frames = records that dropped at least one row `pending`; fallback_rows = rows drawn from the guest image while a rejoined file loads
    double load_ms = 0, max_load_ms = 0, max_frame_load_ms = 0;   // load_ms / max_load_ms = the worker's pac reads; max_frame_load_ms = the most host-thread upload time in one frame
    uint64_t refused[uint8_t(live::picsrc::Why::Count)] = {};
    uint32_t first_refused_key = 0;   // BHF4 (COV7): the first key a refusal named in this report window (0 = none yet), its reason, and its creator / birth site as seen when it was first refused (the key may be gone by report time)
    live::picsrc::Why first_refused_why = live::picsrc::Why::Ok;
    live::picsrc::Creator first_refused_creator = live::picsrc::Creator::Unknown;
    uint32_t first_refused_site = 0;
    bool first_refused_born = false;
  };
  PictureResolver(rdr::Renderer& renderer, live::picsrc::PictureSource& source, const UiFileIndex& index, const std::string& game_root, const UiFileFaults& faults = {});
  ~PictureResolver();
  // Names the worker thread (called on it before its first job); set before the first Resolve.
  void SetWorkerStart(std::function<void()> fn) { worker_start_ = std::move(fn); }
  // S105 A4: the host's own 3D picture of the frame being drawn (kNoImage = none this frame). Set once by the host, before the first Resolve; null = the old answer (the scene-copy row is dropped `picture`).
  void SetScenePicture(std::function<rdr::ImageId(const Quad2D&)> fn) { scene_picture_ = std::move(fn); }
  // The picture of a row (`tex_key[0]`): an empty slot (image == rdr::kNoImage) = the row is dropped `picture`.
  rdr::TextureSlot Resolve(const Quad2D& row);
  // Once per handled frame, AFTER its rows are drawn; `frame` = the guest frame of the record just drawn. Trims the source (A5: the host trims, TrimDrawn), releases the images of keys the source dropped and the
  // file pictures nothing named for kFileUnusedFrames, and resets the per-frame upload budget.
  void EndFrame(uint64_t frame);
  // B1: stops and joins the worker (the loader dies with it). Safe twice; the destructor calls it. After it no row resolves a file (rows are dropped `pending`).
  void Stop();
  // Nothing queued, in flight or waiting for its upload (the rig steps frames until this holds).
  bool Idle() const;
  std::function<rdr::TextureSlot(const Quad2D&)> Function() {
    return [this](const Quad2D& r) { return Resolve(r); };
  }
  const Stats& stats() const { return stats_; }
  std::string Report() const;
  // BHF4 (COV7): empty while no picture was refused since the last call; else `refused[Why] counts + the first refused key and its creator class / birth site`. The host prints it with Report() every 600 frames.
  std::string RefusalReport();

 private:
  enum class FileState : uint8_t { Idle, Queued, Failed };
  struct FileImage {
    rdr::ImageId image = rdr::kNoImage;
    uint64_t last_used = 0;
    uint64_t failed_at = 0;
    FileState state = FileState::Idle;
    bool touched = false;   // C1: a row named this picture since the last EndFrame (EndFrame stamps it with the record's frame)
  };
  struct GuestImage {
    rdr::ImageId image = rdr::kNoImage;
    uint64_t serial = 0;
    uint64_t hash = 0;     // BHF4: the answer's content hash: a rejoin (a new serial, the same picture) may keep drawing this image
    bool bridge = false;   // T2F: uploaded for a File answer whose file image was not ready (dropped at EndFrame once it is)
  };
  struct Done {   // the worker's result for one entry
    uint32_t entry = 0;
    bool ok = false;
    UiLoaded l;
    std::string err;
    double ms = 0;
  };
  rdr::ImageId FileImageOf(uint32_t entry, bool* waiting = nullptr);   // `waiting` = the picture is on its way (the caller counts a pending row when it draws nothing else)
  rdr::ImageId GuestImageOf(const live::picsrc::Answer& a);
  void Drain();   // host thread: uploads the worker's finished pictures, within the budget
  void LoadWorker();
  bool OverBudget() const { return frame_loads_ > 0 && (frame_loads_ >= kLoadsPerFrame || frame_load_ms_ >= kLoadMsPerFrame); }
  void NoteUpload(std::chrono::steady_clock::time_point t0) {
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    ++frame_loads_, frame_load_ms_ += ms;
  }

  rdr::Renderer& renderer_;
  live::picsrc::PictureSource& source_;
  const UiFileIndex& index_;
  UiFileLoader loader_;                // the worker's: nothing else touches it (the destructor / Stop join the worker first)
  std::map<uint32_t, FileImage> files_;
  std::map<uint32_t, GuestImage> guests_;
  uint64_t frame_ = 0;
  uint32_t frame_loads_ = 0;
  double frame_load_ms_ = 0;
  bool frame_pending_ = false;
  Stats stats_;
  uint64_t refusals_reported_ = 0;
  std::function<void()> worker_start_;
  std::function<rdr::ImageId(const Quad2D&)> scene_picture_;   // S105 A4 (SetScenePicture)
  mutable std::mutex mu_;              // the queue, the done list, the flags below
  std::condition_variable cv_;
  std::deque<uint32_t> queue_;
  std::deque<Done> done_;
  uint32_t in_flight_ = 0;
  bool stopping_ = false;
  std::thread worker_;
};



inline PictureResolver::PictureResolver(rdr::Renderer& renderer, live::picsrc::PictureSource& source, const UiFileIndex& index, const std::string& game_root, const UiFileFaults& faults)
    : renderer_(renderer), source_(source), index_(index), loader_(game_root, faults) {
  live::picsrc::g_pic_keep_bridge = true;   // T2F: a File answer keeps its mip 0 for this resolver's bridge
}

inline PictureResolver::~PictureResolver() {
  live::picsrc::g_pic_keep_bridge = false;
  Stop();
  for (auto& [e, f] : files_)
    if (f.image != rdr::kNoImage) renderer_.DestroyImage(f.image);
  for (auto& [k, g] : guests_)
    if (g.image != rdr::kNoImage) renderer_.DestroyImage(g.image);
}

inline void PictureResolver::Stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stopping_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

inline bool PictureResolver::Idle() const {
  std::lock_guard<std::mutex> lock(mu_);
  return queue_.empty() && in_flight_ == 0 && done_.empty();
}

// The only place a pac is read (lint: picture_resolver_lint.py): the worker thread. A batch of loads done = the decoded pacs (hundreds of MB) are released (D2).
inline void PictureResolver::LoadWorker() {
  if (worker_start_) worker_start_();
  bool loaded_since_release = false;
  for (;;) {
    uint32_t entry = 0;
    {
      std::unique_lock<std::mutex> lock(mu_);
      if (queue_.empty() && loaded_since_release) {
        lock.unlock();
        loader_.ReleasePacs();
        loaded_since_release = false;
        lock.lock();
      }
      cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
      if (stopping_) return;
      entry = queue_.front();
      queue_.pop_front();
      ++in_flight_;
    }
    Done d;
    d.entry = entry;
    const auto t0 = std::chrono::steady_clock::now();
    UiFileEntry fe;
    d.ok = index_.Get(entry, fe) && loader_.Load(fe, fe.mips, d.l, d.err);   // (a stale index entry or a read failure: the host counts it and drops the row)
    d.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    loaded_since_release = true;
    std::lock_guard<std::mutex> lock(mu_);
    --in_flight_;
    done_.push_back(std::move(d));
  }
}

inline void PictureResolver::Drain() {
  for (;;) {
    if (OverBudget()) return;
    Done d;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (done_.empty()) return;
      d = std::move(done_.front());
      done_.pop_front();
    }
    ++stats_.loads, stats_.load_ms += d.ms;
    if (d.ms > stats_.max_load_ms) stats_.max_load_ms = d.ms;
    const auto it = files_.find(d.entry);
    if (it == files_.end() || it->second.state != FileState::Queued) continue;   // (released while it loaded: nothing wants it)
    FileImage& f = it->second;
    // render-clock: perf - resolver timer feeding its counters
    const auto t0 = std::chrono::steady_clock::now();
    struct Timer {
      PictureResolver* self;
      std::chrono::steady_clock::time_point t0;
      ~Timer() { self->NoteUpload(t0); }
    } timer{this, t0};
    f.state = FileState::Failed, f.failed_at = frame_;   // (every early return below is a failure; success sets Idle)
    if (!d.ok) {
      ++stats_.load_failed;
      continue;
    }
    UiLoaded& l = d.l;
    rdr::ImageDesc desc;
    desc.width = uint16_t(l.width), desc.height = uint16_t(l.height), desc.mips = uint8_t(l.mips), desc.format = l.format, desc.usage = rdr::kUsageSampled;
    const rdr::ImageId id = renderer_.CreateImage(desc);
    if (id == rdr::kNoImage) {
      ++stats_.upload_failed;
      continue;
    }
    size_t at = 0;
    bool short_data = false;
    for (uint32_t m = 0; m < l.mips; ++m) {
      const size_t bytes = UiMipBytes(l.format, l.width >> m, l.height >> m);
      if (at + bytes > l.data.size()) {
        short_data = true;
        break;
      }
      renderer_.UpdateImage(id, m, 0, l.data.data() + at, bytes);
      at += bytes;
    }
    if (short_data) {
      renderer_.DestroyImage(id);
      ++stats_.upload_failed;
      continue;
    }
    ++stats_.uploads_file;
    f.image = id, f.state = FileState::Idle;
  }
}

inline rdr::ImageId PictureResolver::FileImageOf(uint32_t entry, bool* waiting) {
  FileImage& f = files_[entry];
  f.touched = true;
  if (g_resolver_plant == 1) f.last_used = frame_;
  if (f.image != rdr::kNoImage) return f.image;
  if (f.state == FileState::Failed) {
    if (g_resolver_plant == 3 || frame_ < f.failed_at + kFailedRetryFrames) return rdr::kNoImage;   // D4: not yet
    f.state = FileState::Idle, ++stats_.failed_retries;
  }
  if (f.state == FileState::Idle) {   // first sight (or a retry): the worker reads it
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (stopping_) return rdr::kNoImage;
      queue_.push_back(entry);
    }
    f.state = FileState::Queued;
    if (!worker_.joinable()) worker_ = std::thread([this] { wwe2k14::NameThread(L"pic_resolver"); LoadWorker(); });
    cv_.notify_one();
  }
  if (waiting) *waiting = true;
  return rdr::kNoImage;
}

inline rdr::ImageId PictureResolver::GuestImageOf(const live::picsrc::Answer& a) {
  GuestImage& g = guests_[a.key];
  if (g.image != rdr::kNoImage && g.serial == a.serial) return g.image;
  if (!a.blocks || a.blocks->empty()) return rdr::kNoImage;   // (a released copy of an answer whose image is gone cannot happen: the copy is released only after its upload, and the image lives until the answer moves on)
  if (OverBudget()) return ++stats_.pending, frame_pending_ = true, rdr::kNoImage;   // (the old picture, if any, keeps drawing until the new one is ready: it is replaced below)
  if (g.image != rdr::kNoImage) renderer_.DestroyImage(g.image), g.image = rdr::kNoImage, ++stats_.released;   // the source answered the key again (a caught refill): the old picture goes
  // render-clock: perf - resolver timer feeding its counters
  const auto t0 = std::chrono::steady_clock::now();
  struct Timer {
    PictureResolver* self;
    std::chrono::steady_clock::time_point t0;
    ~Timer() { self->NoteUpload(t0); }
  } timer{this, t0};
  rdr::ImageDesc d;
  d.width = uint16_t(a.width), d.height = uint16_t(a.height), d.mips = 1, d.format = a.format, d.usage = rdr::kUsageSampled;
  const rdr::ImageId id = renderer_.CreateImage(d);
  if (id == rdr::kNoImage) return ++stats_.upload_failed, rdr::kNoImage;
  renderer_.UpdateImage(id, 0, 0, a.blocks->data(), a.blocks->size());
  ++stats_.uploads_guest;
  g.image = id, g.serial = a.serial, g.hash = a.hash, g.bridge = a.kind == live::picsrc::Kind::File;
  source_.ReleaseBlocks(a.key, a.serial);   // A6: the picture is ours now: the CPU copy of the blocks is freed
  return id;
}

inline rdr::TextureSlot PictureResolver::Resolve(const Quad2D& row) {
  rdr::TextureSlot slot;
  ++stats_.asked;
  const uint32_t key = (row.have & kQhPlanes) ? row.tex_key[0] : 0u;
  if (!key) return ++stats_.refused[uint8_t(live::picsrc::Why::NoKey)], slot;   // (no key to name: the report counts it only)
  Drain();
  live::picsrc::Answer a;
  if (!source_.Lookup(key, a)) return ++stats_.missing, slot;
  switch (a.kind) {
    case live::picsrc::Kind::File: {
      bool waiting = false;
      slot.image = FileImageOf(a.entry, &waiting);
      if (slot.image != rdr::kNoImage) {
        ++stats_.file;
        if (a.blocks) source_.ReleaseBlocks(a.key, a.serial);   // T2F: the file image is ready: the bridge bytes (never uploaded) are not needed
      } else {
        auto g = guests_.find(a.key);
        const bool current = g != guests_.end() && g->second.image != rdr::kNoImage && g->second.serial == a.serial;
        if (g_resolver_plant != 4 && a.blocks && !current) {   // T2F bridge: first sight joined a file that is still loading: the game's own mip 0 (kept by the source) draws until the file image lands (a stale guest image never beats it)
          slot.image = GuestImageOf(a);
          g = guests_.find(a.key);
        }
        if (slot.image == rdr::kNoImage && g_resolver_plant != 2 && g != guests_.end() && g->second.image != rdr::kNoImage && (g->second.serial == a.serial || (a.hash && g->second.hash == a.hash))) slot.image = g->second.image;   // BHF4 (BH2 G3): this answer's guest image, or the same picture (the content hash) under a rejoin's new serial; never another picture's bytes   // D3: a rejoined key's guest image keeps drawing (no vanish, no stall)
        if (slot.image != rdr::kNoImage) ++(g->second.bridge ? stats_.bridge_rows : stats_.fallback_rows);
        else if (waiting) ++stats_.pending, frame_pending_ = true;
      }
      break;
    }
    case live::picsrc::Kind::Guest:
      slot.image = GuestImageOf(a);
      if (slot.image != rdr::kNoImage) ++stats_.guest;
      break;
    default:
      if (a.why == live::picsrc::Why::SceneCopy && scene_picture_) {   // S105 A4: the game's scene-colour copy drawn back as a 2D sprite: the host's own 3D picture of this frame (no copy, no guest read); none = dropped `picture` as before (never a stale image)
        slot.image = scene_picture_(row);
        if (slot.image != rdr::kNoImage) {
          ++stats_.scene_copy;
          break;
        }
      }
      if (!stats_.first_refused_key) {
        stats_.first_refused_key = key, stats_.first_refused_why = a.why;
        stats_.first_refused_born = source_.BirthOf(key, stats_.first_refused_creator, stats_.first_refused_site);
      }
      ++stats_.refused[uint8_t(a.why)];
      break;
  }
  return slot;
}

inline void PictureResolver::EndFrame(uint64_t frame) {
  Drain();   // finished reads arrive even when no row asks this frame
  frame_ = frame;
  for (auto& [e, f] : files_)   // C1: what a row named this frame is used AT this frame (a late host start, or a gap of any length, never releases a picture that is drawn right now)
    if (f.touched) {
      if (g_resolver_plant != 1) f.last_used = frame;
      f.touched = false;
    }
  if (frame_pending_) ++stats_.pending_frames, stats_.last_pending_frame = frame;
  frame_pending_ = false;
  stats_.max_loads_frame = std::max<uint64_t>(stats_.max_loads_frame, frame_loads_);
  stats_.max_frame_load_ms = std::max(stats_.max_frame_load_ms, frame_load_ms_);
  frame_loads_ = 0, frame_load_ms_ = 0;
  source_.TrimDrawn(frame);   // A5: the host drew record `frame`: keys that died before it are no longer named by any record still to come
  for (auto it = guests_.begin(); it != guests_.end();) {
    uint64_t serial = 0;
    bool keep = source_.Alive(it->first, serial) && serial == it->second.serial;
    if (keep && g_resolver_plant != 5 && it->second.bridge && it->second.image != rdr::kNoImage) {   // T2F: a bridge image (the key answers a FILE) ends once the file image is ready
      live::picsrc::Answer a;
      if (source_.Lookup(it->first, a) && a.kind == live::picsrc::Kind::File) {
        const auto f = files_.find(a.entry);
        if (f != files_.end() && f->second.image != rdr::kNoImage) keep = false;
      }
    }
    if (!keep && it->second.image != rdr::kNoImage) {   // D3: the key now answers a FILE whose image is not ready: the guest image is the fallback until it is
      live::picsrc::Answer a;
      if (source_.Lookup(it->first, a) && a.kind == live::picsrc::Kind::File) {
        const auto f = files_.find(a.entry);
        keep = f == files_.end() || f->second.image == rdr::kNoImage;   // (a file that failed keeps its guest picture too, better than none; it ends with the key)
      }
    }
    if (!keep) {
      if (it->second.image != rdr::kNoImage) renderer_.DestroyImage(it->second.image), ++stats_.released;   // (the renderer defers the destroy past frames in flight)
      it = guests_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = files_.begin(); it != files_.end();) {
    if (frame > it->second.last_used + kFileUnusedFrames && it->second.state != FileState::Queued) {
      if (it->second.image != rdr::kNoImage) renderer_.DestroyImage(it->second.image), ++stats_.released;
      it = files_.erase(it);
    } else {
      ++it;
    }
  }
}

inline std::string PictureResolver::Report() const {
  char line[1000];
  std::snprintf(line, sizeof line,
                "pictures: asked %llu, file %llu, guest %llu, missing %llu, refused gpu %llu movie %llu, load failed %llu (retried %llu), upload failed %llu, uploads file %llu guest %llu, released %llu, scene_copy %llu, "
                "pending(rows) %llu in %llu records (last at frame %llu), guest fallback rows %llu, bridge rows %llu, file reads %llu on the worker (%.2f ms total, max %.2f ms one), uploads max %llu in a frame, %.2f ms worst frame",
                (unsigned long long)stats_.asked, (unsigned long long)stats_.file, (unsigned long long)stats_.guest, (unsigned long long)stats_.missing, (unsigned long long)stats_.refused[uint8_t(live::picsrc::Why::Gpu)],
                (unsigned long long)stats_.refused[uint8_t(live::picsrc::Why::Movie)], (unsigned long long)stats_.load_failed, (unsigned long long)stats_.failed_retries, (unsigned long long)stats_.upload_failed,
                (unsigned long long)stats_.uploads_file, (unsigned long long)stats_.uploads_guest, (unsigned long long)stats_.released, (unsigned long long)stats_.scene_copy, (unsigned long long)stats_.pending, (unsigned long long)stats_.pending_frames,
                (unsigned long long)stats_.last_pending_frame, (unsigned long long)stats_.fallback_rows, (unsigned long long)stats_.bridge_rows, (unsigned long long)stats_.loads, stats_.load_ms, stats_.max_load_ms, (unsigned long long)stats_.max_loads_frame,
                stats_.max_frame_load_ms);
  return line;
}

inline std::string PictureResolver::RefusalReport() {
  uint64_t total = 0;
  for (uint8_t w = 0; w < uint8_t(live::picsrc::Why::Count); ++w) total += stats_.refused[w];
  if (total == refusals_reported_) return {};
  refusals_reported_ = total;
  std::string out = "picture refusals " + std::to_string((unsigned long long)total) + ":";
  for (uint8_t w = 0; w < uint8_t(live::picsrc::Why::Count); ++w)
    if (stats_.refused[w]) out += std::string(" ") + live::picsrc::WhyName(live::picsrc::Why(w)) + " " + std::to_string((unsigned long long)stats_.refused[w]);
  if (stats_.first_refused_key) {
    const live::picsrc::Creator c = stats_.first_refused_creator;
    const uint32_t site = stats_.first_refused_site;
    const bool born = stats_.first_refused_born;
    static const char* const kCreatorName[] = {"unknown", "loader", "movie", "gpu", "scene_copy"};
    char b[160];
    std::snprintf(b, sizeof b, "; first refused key 0x%08X (%s): %s, birth site %s0x%08X", stats_.first_refused_key, live::picsrc::WhyName(stats_.first_refused_why), kCreatorName[uint8_t(c)], born ? "" : "(never seen born) ", site);
    out += b;
    stats_.first_refused_key = 0;   // a new window: the next report names the next first refused key
  }
  return out;
}

}  // namespace rdr_scene
