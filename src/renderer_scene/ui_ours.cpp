// See ui_ours.h. Session 101, lane 66.
#include "ui_ours.h"

#include <chrono>
#include <algorithm>
#include <cstdio>

#include "arena_ours_core.h"   // ArenaCore::ArenaOfRecord (lane 126: the record's arena, the one rule the arena hook uses)

namespace rdr_scene {
namespace {
using Clock = std::chrono::steady_clock;
double MsSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
constexpr uint64_t kUnusedFrames = 3600;   // a picture no draw named for this many ui frames (~1 minute of a menu) is released
}  // namespace

UiOurs::UiOurs(rdr::Renderer& renderer, const UiOursOptions& options)
    : renderer_(renderer), options_(options), core_(options.faults, options.image_content, &index_), root_(options.game_root) {
  if (options.quads) quads_ = std::make_unique<UiQuads>(renderer, options.quad_faults);   // lane 148
}

UiOurs::~UiOurs() {
  stopping_ = true;
  cv_.notify_all();
  if (index_thread_.joinable()) index_thread_.join();
  deep_cv_.notify_all();
  if (deep_thread_.joinable()) deep_thread_.join();   // lane 126
  if (load_thread_.joinable()) load_thread_.join();
  renderer_.WaitIdle();
  for (auto& [entry, s] : slots_)
    if (s.state == 2 && s.texture.handle) renderer_.DestroyImage(rdr::ImageId(s.texture.handle));
  slots_.clear();
}

void UiOurs::SetGameRoot(const std::string& root) {
  std::lock_guard<std::mutex> lock(mu_);
  if (root_.empty()) root_ = root;   // (the first root wins: the index is built from it)
}

void UiOurs::IndexAll() {
  const auto t0 = Clock::now();
  for (uint8_t pac = 0; pac < uint8_t(kUiTier1Count) && !stopping_; ++pac) IndexOne(pac, nullptr);
  index_ms_ = MsSince(t0);
  index_done_ = true;
}

// Walks one pac into the index (a worker's or the calling thread's body). `ok` (lane 126, review F2): whether the walk completed. A missing pac is an error, logged: the
// second tier asks only for pacs that should exist (bgEtc and the current arena's bgNN).
bool UiOurs::IndexOne(uint8_t pac, uint64_t* entry_count) {
  UiIndexOptions o;
  {
    std::lock_guard<std::mutex> lock(mu_);
    o.game_root = root_;
  }
  o.cache_dir = options_.cache_dir;
  if (options_.faults.tier_boundary) o.names_only_from = kUiBgEtc;   // plant: the tier boundary one too low
  std::vector<UiFileEntry> entries;
  std::string err;
  if (!BuildPacIndex(o, pac, entries, err)) {
    std::lock_guard<std::mutex> lock(mu_);
    index_error_ += (index_error_.empty() ? "" : "; ") + err;   // (loud in the report: the pac's pictures stay unjoined)
    return false;
  }
  if (entry_count) *entry_count += entries.size();
  index_.AddPac(pac, std::move(entries));
  return true;
}

// lane 126: the second tier (review F1). Started by the first claimed draw that stays unjoined AFTER the first tier is done (menus never start it: every menu picture joins in
// the first tier). It holds bgEtc (unless low RAM) and the CURRENT arena's bgNN only (the arena id from the scene record, as ArenaOurs); a later arena change adds the new
// arena's pac. Offline it runs on the calling thread (the draw is then decided again, see Replace); live on a worker, the draws stay the stream's until it publishes a pac
// (the index generation moves and the unjoined keys re-join).
void UiOurs::RequestDeep(uint8_t pac) {
  {
    std::lock_guard<std::mutex> lock(deep_mu_);
    if (!deep_requested_.insert(pac).second) return;
    deep_queue_.push_back(pac);
    ++deep_pending_;
  }
  if (options_.live) {
    if (!deep_thread_.joinable()) deep_thread_ = std::thread([this] { DeepWorker(); });
    deep_cv_.notify_one();
  }
}

bool UiOurs::DeepStep() {   // one queued pac; false: nothing queued
  uint8_t pac = 0;
  {
    std::lock_guard<std::mutex> lock(deep_mu_);
    if (deep_queue_.empty()) return false;
    pac = deep_queue_.front();
    deep_queue_.pop_front();
  }
  const auto t0 = Clock::now();
  uint64_t entries = 0;
  const bool ok = IndexOne(pac, &entries);
  std::lock_guard<std::mutex> lock(deep_mu_);
  deep_ms_ = deep_ms_ + MsSince(t0);
  deep_entries_ += entries;
  ++(ok ? deep_walked_ : deep_failed_);
  --deep_pending_;
  return true;
}

void UiOurs::DeepWorker() {
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(deep_mu_);
      deep_cv_.wait(lock, [&] { return stopping_ || !deep_queue_.empty(); });
      if (stopping_) return;
    }
    DeepStep();
  }
}

// true: pacs were indexed synchronously (offline) so the draw just decided must be decided again.
bool UiOurs::MaybeStartDeep(uint64_t unjoined_before) {
  if (options_.faults.no_deep || !index_done_) return false;
  if (!deep_started_) {
    if (core_.census().unjoined == unjoined_before) return false;
    deep_started_ = true;
    if (options_.skip_bg_etc) {
      std::lock_guard<std::mutex> lock(mu_);
      index_error_ += (index_error_.empty() ? "" : "; ") + std::string("bgEtc skipped (low RAM)");
    } else {
      RequestDeep(uint8_t(kUiBgEtc));
    }
  }
  std::vector<int> arenas;
  {
    std::lock_guard<std::mutex> lock(deep_mu_);
    arenas = record_arenas_;
  }
  for (int arena : arenas) {
    if (options_.faults.deep_wrong_arena) ++arena;   // plant: the neighbouring arena's pac
    if (arena >= 0 && size_t(arena) <= kUiMaxArena) RequestDeep(uint8_t(kUiBgBase + size_t(arena)));
  }
  if (options_.live) return false;
  bool ran = false;
  while (DeepStep()) ran = true;
  return ran;
}

void UiOurs::SetRecord(std::shared_ptr<const SceneTables> record) {
  if (!record) return;
  const int arena = ArenaCore::ArenaOfRecord(*record);
  if (arena < 0) return;   // (kept across frames without a record)
  arena_ = arena;
  // Every bgNN pac the record's assets name (a stage can load more than one: the WM30 entrance's two lightmap pages live in two of them), at most 4, the first = ArenaOfRecord.
  std::vector<int> all{arena};
  for (const AssetRef& a : record->assets) {
    const std::string pac(record->Str(a.pac));
    const size_t p = pac.rfind("bg");
    int n = 0;
    if (p != std::string::npos && std::sscanf(pac.c_str() + p, "bg%d", &n) == 1 && n >= 0 && size_t(n) <= kUiMaxArena && std::find(all.begin(), all.end(), n) == all.end() && all.size() < 4) all.push_back(n);
  }
  std::lock_guard<std::mutex> lock(deep_mu_);
  record_arenas_ = std::move(all);
}

void UiOurs::StartIndex() {
  index_started_ = true;
  if (options_.live) index_thread_ = std::thread([this] { IndexAll(); });
  else IndexAll();
}

bool UiOurs::FrameReady(const rdr_adapter::LoadedFrame&) {
  ++frames_;
  core_.BeginFrame();
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (root_.empty()) return ++not_ready_frames_, false;   // no game folder known yet: every claimed draw stays the stream's
  }
  if (!index_started_) StartIndex();
  if (frames_ % 600 == 0) Sweep();
  ++ours_frames_;
  return true;
}

void UiOurs::Sweep() {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto it = slots_.begin(); it != slots_.end();) {
    if (it->second.state == 2 && core_.frame() - it->second.texture.last_used > kUnusedFrames) {
      epoch_.fetch_add(1);   // (UiCore caches texture pointers per key until the epoch moves)
      renderer_.DestroyImage(rdr::ImageId(it->second.texture.handle));   // (the renderer defers the destroy past frames in flight)
      ++released_;
      it = slots_.erase(it);
    } else {
      ++it;
    }
  }
}

bool UiOurs::LoadAndUpload(const UiFileEntry& e, uint32_t mips, UiTexture& out, std::string& err) {
  std::string root;
  {
    std::lock_guard<std::mutex> lock(mu_);
    root = root_;
  }
  if (!loader_owner_) loader_owner_ = std::make_unique<UiFileLoader>(root, options_.faults);
  UiLoaded l;
  if (!loader_owner_->Load(e, mips, l, err)) return false;
  rdr::ImageDesc d;
  d.width = uint16_t(l.width), d.height = uint16_t(l.height), d.mips = uint8_t(l.mips), d.format = l.format, d.usage = rdr::kUsageSampled;
  const rdr::ImageId id = renderer_.CreateImage(d);
  if (id == rdr::kNoImage) return err = "CreateImage failed for " + e.path, false;
  size_t at = 0;
  for (uint32_t m = 0; m < l.mips; ++m) {
    const size_t bytes = UiMipBytes(l.format, l.width >> m, l.height >> m);
    if (at + bytes > l.data.size()) {
      renderer_.DestroyImage(id);
      return err = "short data for " + e.path, false;
    }
    renderer_.UpdateImage(id, m, 0, l.data.data() + at, bytes);
    at += bytes;
  }
  out.handle = uint32_t(id), out.width = l.width, out.height = l.height, out.mips = l.mips, out.format = l.format, out.prefix = std::move(l.prefix);
  std::lock_guard<std::mutex> lock(mu_);
  ++uploads_;
  upload_bytes_ += l.data.size();
  return true;
}

void UiOurs::LoadWorker() {
  for (;;) {
    uint64_t entry = 0;
    {
      std::unique_lock<std::mutex> lock(mu_);
      if (queue_.empty() && loader_owner_) {   // a batch is done: the decoded pacs (hundreds of MB) go
        lock.unlock();
        loader_owner_->ReleasePacs();
        lock.lock();
      }
      cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
      if (stopping_) return;
      entry = queue_.front();
      queue_.pop_front();
    }
    UiFileEntry fe;
    UiTexture t;
    std::string err;
    const bool ok = index_.Get(uint32_t(entry), fe) && LoadAndUpload(fe, uint32_t(entry >> 32), t, err);
    std::lock_guard<std::mutex> lock(mu_);
    Slot& s = slots_[entry];
    if (ok) s.texture = std::move(t), s.state = 2;
    else s.state = 3, ++load_failures_, index_error_ += (index_error_.empty() ? "" : "; ") + (err.empty() ? std::string("entry ") + std::to_string(entry) + " unreadable" : err);
  }
}

const UiTexture* UiOurs::Texture(uint32_t entry_id, const UiFileEntry& e, uint32_t mips, bool& pending) {
  const uint64_t entry = uint64_t(entry_id) | uint64_t(mips) << 32;
  pending = false;
  {
    std::unique_lock<std::mutex> lock(mu_);
    Slot& s = slots_[entry];
    if (s.state == 2) return &s.texture;
    if (s.state == 3) return nullptr;
    if (s.state == 1) return pending = true, nullptr;
    if (options_.live) {
      s.state = 1;
      queue_.push_back(entry);
      if (!load_thread_.joinable()) load_thread_ = std::thread([this] { LoadWorker(); });
      cv_.notify_one();
      return pending = true, nullptr;
    }
  }
  // Offline (fview / rplay / the unit test): load and upload now, on this thread, so a frame is never "pending" (the check reads the picture the first time).
  UiTexture t;
  std::string err;
  const bool ok = LoadAndUpload(e, mips, t, err);
  std::lock_guard<std::mutex> lock(mu_);
  Slot& s = slots_[entry];
  if (!ok) {
    s.state = 3, ++load_failures_;
    index_error_ += (index_error_.empty() ? "" : "; ") + err;
    return nullptr;
  }
  s.texture = std::move(t), s.state = 2;
  return &s.texture;
}

bool UiOurs::Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, rdr_adapter::KeepWhy& why) {
  if (quads_ && in.origin->call_site == kUiQuadRectSite) return quads_->Build(in, out, why);   // lane 148: a rectangle has no picture to join: the quad only
  uint32_t stream_image = 0;
  const uint8_t* swizzle = nullptr;
  uint32_t view_format = 0;
  uint32_t min_level = 0, max_level = 15;
  for (uint32_t t = 0; t < in.draw->texture_count && in.draw->texture_first + t < in.frame->texture_slots.count; ++t) {
    const rdr_adapter::TextureSlot& ts = in.frame->texture_slots[in.draw->texture_first + t];
    if (ts.slot == 0) {
      stream_image = ts.image, swizzle = ts.swizzle, view_format = ts.view_format, min_level = ts.min_level, max_level = ts.max_level;
      break;
    }
  }
  const uint64_t unjoined_before = core_.census().unjoined;   // lane 126
  UiCore::Verdict v = core_.Decide(in.origin->part, stream_image, swizzle, view_format, min_level, max_level, *this);
  const bool deep_ran = MaybeStartDeep(unjoined_before);   // lane 126
  if (deep_ran && core_.census().unjoined > unjoined_before) {   // lane 126: offline the second tier just ran synchronously: ask again about THIS draw (the first ask is not counted)
    core_.RetractUnjoined();
    v = core_.Decide(in.origin->part, stream_image, swizzle, view_format, min_level, max_level, *this);
  }
  if (quads_ && in.origin->call_site == kUiQuadSpriteSite) {   // lane 148: the sprite as an own draw (our quad and program); the picture is ours where joined, else the stream's
    rdr_adapter::KeepWhy quad_why = rdr_adapter::KeepWhy::NotOurs;
    if (quads_->Build(in, out, quad_why)) {   // (a refusal leaves `out` untouched: the previous behaviour follows)
      if (v.ours) out.slots[0].set = true, out.slots[0].image = rdr::ImageId(v.texture->handle);
      return true;
    }
  }
  if (!v.ours) return why = v.why, false;
  out.keep_stream_geometry = true;
  out.slots[0].set = true;
  out.slots[0].image = rdr::ImageId(v.texture->handle);
  return true;
}

std::string UiOurs::WindowDetail() const {   // lane 156
  std::lock_guard<std::mutex> lock(mu_);
  return quads_ ? quads_->Report() : std::string();
}

std::string UiOurs::Report() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::string text = core_.Report(index_);
  char line[256];
  std::snprintf(line, sizeof line, ", uploads %llu (%.1f MB), released %llu, index %.0f ms%s", (unsigned long long)uploads_, double(upload_bytes_) / 1048576.0,
                (unsigned long long)released_, index_ms_.load(), index_done_ ? "" : " (not finished)");
  if (deep_started_) {   // lane 126 (review F1 cost guard: the line carries the time and the entry count)
    std::lock_guard<std::mutex> dl(deep_mu_);
    char deep[200];
    std::snprintf(deep, sizeof deep, ", deep (bg lightmaps) %s: %u pacs walked (arena %d), %u failed, %u pending, %llu entries, %.0f ms", deep_pending_ == 0 && deep_failed_ == 0 ? "done" : deep_pending_ ? "running" : "FAILED",
                  deep_walked_, arena_.load(), deep_failed_, deep_pending_, (unsigned long long)deep_entries_, deep_ms_);
    text += deep;
  }
  text += line;
  if (quads_) text += ", " + quads_->Report();   // lane 148
  if (!index_error_.empty()) text += ", ERRORS: " + index_error_;
  if (not_ready_frames_) text += ", " + std::to_string(not_ready_frames_) + " frames with no game folder";
  return text;
}

}  // namespace rdr_scene
