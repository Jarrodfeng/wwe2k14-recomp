// The capture folder's scene sink (Session 101, lane 26; docs\renderer\SCENE_RECORD.md "The live writer"): writes scene\frame_NNNNNN.rsc beside a
// capture's frames\, off the game's threads. Attached when a capture arms with --wwe2k14_scene_records, detached when it ends.
// A bounded queue to one writer thread: when the queue is full the record is dropped and counted, never waited for. No game or SDK includes.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "live_scene.h"

namespace rdr_scene::live {

class FolderSink : public Sink {
 public:
  explicit FolderSink(size_t max_queue = 64);
  ~FolderSink() override;

  // Creates <capture_folder>\scene and starts the writer thread. False with `error`: nothing started.
  bool Begin(const std::string& capture_folder, std::string& error);
  // Waits for the queue to reach the disk, then stops the thread. Safe to call twice. (Detach from the writer first: RemoveSink.)
  void End();

  void OnRecord(const FrameInfo& f, const SceneTables& t) override;  // copies the tables (the slow path; EndFrame uses the shared one)
  void OnRecordShared(const FrameInfo& f, std::shared_ptr<const SceneTables> t) override;
  void Census(std::string& line) override;

  uint64_t written() const { return written_.load(); }
  uint64_t dropped() const { return dropped_.load(); }  // the queue was full
  uint64_t failed() const { return failed_.load(); }    // a file could not be written
  const std::string& folder() const { return folder_; }
  // Tests: holds the writer thread so the queue fills (a planted fault).
  void PauseWriterForTest(bool pause);
  // Tests only: also take records that carry no capture frame number, numbered by arrival. In a capture such a record (the capture stopped
  // mid-frame) is dropped and counted: arrival numbers would collide with real frame numbers after any refusal or drop.
  void AllowUnnumberedForTest(bool allow) { allow_unnumbered_ = allow; }

 private:
  struct Item {
    uint32_t number = 0;
    std::shared_ptr<const SceneTables> tables;
  };
  void Run();

  const size_t max_queue_;
  std::string folder_;
  std::mutex mu_;
  std::condition_variable cv_, idle_cv_;
  std::deque<Item> queue_;
  std::thread thread_;
  bool running_ = false, stop_ = false, paused_ = false, busy_ = false;
  bool allow_unnumbered_ = false;
  uint32_t seq_ = 0;  // numbering when a frame carries no capture frame number
  uint64_t window_accepted_ = 0, window_written_ = 0, window_dropped_ = 0, window_failed_ = 0;  // census window
  std::atomic<uint64_t> written_{0}, dropped_{0}, failed_{0};
  std::string first_error_;
};

}  // namespace rdr_scene::live
