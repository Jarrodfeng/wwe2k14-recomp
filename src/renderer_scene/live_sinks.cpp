// The capture folder's scene sink (Session 101, lane 26). See live_sinks.h.
#include "live_sinks.h"

#include <cstdio>
#include <exception>
#include <filesystem>

#include "record_io.h"

namespace rdr_scene::live {

FolderSink::FolderSink(size_t max_queue) : max_queue_(max_queue ? max_queue : 1) {}

FolderSink::~FolderSink() { End(); }

bool FolderSink::Begin(const std::string& capture_folder, std::string& error) {
  std::lock_guard<std::mutex> lock(mu_);
  if (running_) {
    error = "already started";
    return false;
  }
  std::error_code ec;
  const std::filesystem::path dir = std::filesystem::path(capture_folder) / "scene";
  std::filesystem::create_directories(dir, ec);
  if (ec || !std::filesystem::is_directory(dir)) {
    error = "cannot create " + dir.string() + (ec ? ": " + ec.message() : "");
    return false;
  }
  folder_ = dir.string();
  stop_ = false;
  running_ = true;
  seq_ = 0;
  try {
    thread_ = std::thread([this] { Run(); });
  } catch (const std::exception& e) {  // no thread: nothing started, nothing thrown into the caller
    running_ = false;
    error = std::string("cannot start the writer thread: ") + e.what();
    return false;
  }
  return true;
}

void FolderSink::End() {
  {
    std::unique_lock<std::mutex> lock(mu_);
    if (!running_) return;
    paused_ = false;
    cv_.notify_all();
    idle_cv_.wait(lock, [&] { return queue_.empty() && !busy_; });
    stop_ = true;
    cv_.notify_all();
  }
  if (thread_.joinable()) thread_.join();
  std::lock_guard<std::mutex> lock(mu_);
  running_ = false;
}

void FolderSink::PauseWriterForTest(bool pause) {
  std::lock_guard<std::mutex> lock(mu_);
  paused_ = pause;
  cv_.notify_all();
}

void FolderSink::OnRecord(const FrameInfo& f, const SceneTables& t) { OnRecordShared(f, std::make_shared<const SceneTables>(t)); }

void FolderSink::OnRecordShared(const FrameInfo& f, std::shared_ptr<const SceneTables> t) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!running_ || stop_) return;
  if (queue_.size() >= max_queue_) {  // full: drop and count, never block the game
    ++dropped_;
    ++window_dropped_;
    return;
  }
  if (f.capture_frame == kNoCaptureFrame && !allow_unnumbered_) {  // no frame number to name the file by: never guess one
    ++dropped_;
    ++window_dropped_;
    return;
  }
  Item item;
  ++window_accepted_;
  item.number = f.capture_frame != kNoCaptureFrame ? f.capture_frame : seq_;
  ++seq_;
  item.tables = std::move(t);
  queue_.push_back(std::move(item));
  cv_.notify_one();
}

void FolderSink::Census(std::string& line) {
  std::lock_guard<std::mutex> lock(mu_);
  line += "folder accepted " + std::to_string(window_accepted_) + ", written " + std::to_string(window_written_) + ", dropped " + std::to_string(window_dropped_) + ", failed " + std::to_string(window_failed_) +
          ", queued " + std::to_string(queue_.size());
  window_accepted_ = window_written_ = window_dropped_ = window_failed_ = 0;
}

void FolderSink::Run() {
  for (;;) {
    Item item;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [&] { return stop_ || (!paused_ && !queue_.empty()); });
      if (queue_.empty() || paused_) {
        if (stop_) return;
        continue;
      }
      item = std::move(queue_.front());
      queue_.pop_front();
      busy_ = true;
    }
    char name[48];
    std::snprintf(name, sizeof(name), "frame_%06u.rsc", item.number);
    std::string error;
    const bool ok = WriteRecord((std::filesystem::path(folder_) / name).string(), *item.tables, error);
    {
      std::lock_guard<std::mutex> lock(mu_);
      busy_ = false;
      if (ok) {
        ++written_;
        ++window_written_;
      } else {
        ++failed_;
        ++window_failed_;
        if (first_error_.empty()) first_error_ = error;
      }
      if (queue_.empty()) idle_cv_.notify_all();
    }
  }
}

}  // namespace rdr_scene::live
