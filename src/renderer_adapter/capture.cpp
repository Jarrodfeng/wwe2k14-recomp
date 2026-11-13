// The disk side of a capture: one thread, a bounded queue, a byte budget. No game or SDK includes.
#include "capture.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <system_error>
#include <thread>

#define XXH_INLINE_ALL
#include "../../thirdparty/rexglue-sdk-src/thirdparty/xxHash/xxhash.h"

namespace rdr_adapter {

uint64_t Xxh3(const void* data, size_t bytes) { return XXH3_64bits(data, bytes); }

namespace {

constexpr uint64_t kMaxQueuedBytes = 1024ull * 1024 * 1024;  // past this the game thread waits for the disk (a hitch: capture runs are not timed)

std::FILE* OpenForWrite(const std::filesystem::path& p) {
#if defined(_WIN32)
  return _wfopen(p.c_str(), L"wb");
#else
  return std::fopen(p.c_str(), "wb");
#endif
}

struct Job {
  enum Kind { kAppend, kWhole } kind;
  Stream stream = Stream::Geometry;
  std::string relative;
  std::vector<uint8_t> bytes;
};

}  // namespace

struct CaptureWriter::Impl {
  std::filesystem::path folder;
  uint64_t budget = 0;
  std::atomic<uint64_t> used{0};
  std::atomic<bool> over{false};
  bool running = false;

  std::FILE* streams[2] = {nullptr, nullptr};
  uint64_t stream_size[2] = {0, 0};

  std::mutex m;
  std::condition_variable wake, drained;
  std::deque<Job> jobs;
  uint64_t queued_bytes = 0;
  bool stop = false;
  std::string error;
  std::thread thread;

  void Fail(std::string what) {
    std::lock_guard lock(m);
    if (error.empty()) error = std::move(what);
  }

  void WriteWhole(const std::string& relative, const std::vector<uint8_t>& bytes) {
    const std::filesystem::path p = folder / relative;
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::FILE* f = OpenForWrite(p);
    if (!f) return Fail("cannot open " + p.string());
    if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), f) != bytes.size()) Fail("short write to " + p.string());
    std::fclose(f);
  }

  void Run() {
    for (;;) {
      Job job;
      {
        std::unique_lock lock(m);
        wake.wait(lock, [this] { return stop || !jobs.empty(); });
        if (jobs.empty()) {
          if (stop) return;
          continue;
        }
        job = std::move(jobs.front());
        jobs.pop_front();
      }
      if (job.kind == Job::kWhole) {
        WriteWhole(job.relative, job.bytes);
      } else {
        std::FILE* f = streams[uint32_t(job.stream)];
        if (f && !job.bytes.empty() && std::fwrite(job.bytes.data(), 1, job.bytes.size(), f) != job.bytes.size()) {
          Fail(job.stream == Stream::Geometry ? "short write to geometry.bin" : "short write to textures.bin");
        }
      }
      {
        std::lock_guard lock(m);
        queued_bytes -= job.bytes.size();
      }
      drained.notify_all();
    }
  }

  bool Reserve(size_t n) {
    uint64_t now = used.load(std::memory_order_relaxed);
    if (now + n > budget) {
      over.store(true, std::memory_order_relaxed);
      return false;
    }
    used.store(now + n, std::memory_order_relaxed);  // (one caller, under the adapter's lock)
    return true;
  }

  void Push(Job&& job) {
    std::unique_lock lock(m);
    drained.wait(lock, [this] { return queued_bytes < kMaxQueuedBytes; });
    queued_bytes += job.bytes.size();
    jobs.push_back(std::move(job));
    lock.unlock();
    wake.notify_one();
  }
};

CaptureWriter::CaptureWriter() : impl_(new Impl) {}
CaptureWriter::~CaptureWriter() {
  End();
  delete impl_;
}

bool CaptureWriter::active() const { return impl_->running; }
uint64_t CaptureWriter::bytes_used() const { return impl_->used.load(std::memory_order_relaxed); }
uint64_t CaptureWriter::budget() const { return impl_->budget; }
bool CaptureWriter::over_budget() const { return impl_->over.load(std::memory_order_relaxed); }

std::string CaptureWriter::TakeError() {
  std::lock_guard lock(impl_->m);
  std::string e;
  e.swap(impl_->error);
  return e;
}

bool CaptureWriter::Begin(const std::filesystem::path& folder, uint64_t budget_bytes) {
  if (impl_->running) return false;
  std::error_code ec;
  std::filesystem::create_directories(folder / "frames", ec);
  if (ec) return false;
  Impl& s = *impl_;
  s.folder = folder;
  s.budget = budget_bytes;
  s.used.store(0);
  s.over.store(false);
  s.stop = false;
  s.error.clear();
  s.queued_bytes = 0;
  s.streams[0] = OpenForWrite(folder / "geometry.bin");
  s.streams[1] = OpenForWrite(folder / "textures.bin");
  s.stream_size[0] = s.stream_size[1] = 0;
  if (!s.streams[0] || !s.streams[1]) {
    if (s.streams[0]) std::fclose(s.streams[0]);
    if (s.streams[1]) std::fclose(s.streams[1]);
    s.streams[0] = s.streams[1] = nullptr;
    return false;
  }
  s.thread = std::thread([p = impl_] { p->Run(); });
  s.running = true;
  return true;
}

void CaptureWriter::End() {
  Impl& s = *impl_;
  if (!s.running) return;
  {
    std::lock_guard lock(s.m);
    s.stop = true;
  }
  s.wake.notify_one();
  s.thread.join();
  if (s.streams[0]) std::fclose(s.streams[0]);
  if (s.streams[1]) std::fclose(s.streams[1]);
  s.streams[0] = s.streams[1] = nullptr;
  s.running = false;
}

uint64_t CaptureWriter::Append(Stream stream, std::vector<uint8_t>&& bytes) {
  Impl& s = *impl_;
  if (!s.running || !s.Reserve(bytes.size())) return UINT64_MAX;
  const uint64_t at = s.stream_size[uint32_t(stream)];
  s.stream_size[uint32_t(stream)] += bytes.size();
  Job job;
  job.kind = Job::kAppend;
  job.stream = stream;
  job.bytes = std::move(bytes);
  s.Push(std::move(job));
  return at;
}

bool CaptureWriter::WriteFile(const std::string& relative, std::vector<uint8_t>&& bytes) {
  Impl& s = *impl_;
  if (!s.running || !s.Reserve(bytes.size())) return false;
  Job job;
  job.kind = Job::kWhole;
  job.relative = relative;
  job.bytes = std::move(bytes);
  s.Push(std::move(job));
  return true;
}

}  // namespace rdr_adapter
