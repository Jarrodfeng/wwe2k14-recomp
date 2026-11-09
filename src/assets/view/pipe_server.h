// Named-pipe command server: one client at a time, one JSON line in, one JSON line out.
// The pipe thread only reads/writes; commands run on the render thread (the owner pumps the mailbox between frames).
// Session 98 phone port (stage C): elsewhere (the phone) the same protocol on a TCP socket bound to 127.0.0.1 (the name is a
// port number, default 47140; from the PC: adb forward tcp:47140 tcp:47140). Only the transport differs.
#pragma once
#ifdef _WIN32
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace view {

class Mailbox {
 public:
  // Called from the pipe thread: blocks until the render thread answers.
  std::string Submit(const std::string& line) {
    auto job = std::make_shared<Job>();
    job->line = line;
    std::future<std::string> fut = job->reply.get_future();
    pending_++;  // (before the push: a quit answered by the render thread must never leave Pending() at 0 too early)
    {
      std::lock_guard<std::mutex> lk(mu_);
      q_.push_back(job);
    }
    cv_.notify_all();
    return fut.get();
  }
  // Render thread: run every queued command with `fn`. Returns the number handled.
  template <class F>
  int Pump(F&& fn) {
    int n = 0;
    for (;;) {
      std::shared_ptr<Job> j;
      {
        std::lock_guard<std::mutex> lk(mu_);
        if (q_.empty()) break;
        j = q_.front();
        q_.pop_front();
      }
      std::string r;
      try { r = fn(j->line); } catch (const std::exception& e) { r = std::string("{\"ok\":false,\"error\":\"exception: ") + e.what() + "\"}"; }
      catch (...) { r = "{\"ok\":false,\"error\":\"unknown exception\"}"; }
      j->reply.set_value(r);
      n++;
    }
    return n;
  }
  void Wait(int ms) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait_for(lk, std::chrono::milliseconds(ms), [&] { return !q_.empty(); });
  }
  void Done() { pending_--; }
  int Pending() const { return pending_.load(); }

 private:
  struct Job { std::string line; std::promise<std::string> reply; };
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::shared_ptr<Job>> q_;
  std::atomic<int> pending_{0};
};

#ifdef _WIN32
class PipeServer {
 public:
  PipeServer(Mailbox& mb, std::string name) : mb_(mb), name_("\\\\.\\pipe\\" + name) {}
  // Creates the first pipe instance here (FIRST_PIPE_INSTANCE: a stale viewer that owns the name makes this fail) and
  // returns false with `err` set; then serves on a detached thread.
  bool Start(std::string& err) {
    first_ = Create(true);
    if (first_ == INVALID_HANDLE_VALUE) {
      err = "cannot create pipe " + name_ + " (error " + std::to_string(GetLastError()) + "; another viewer owns the name?)";
      return false;
    }
    th_ = std::thread([this] { Run(); });
    th_.detach();
    return true;
  }

 private:
  HANDLE Create(bool first) {
    return CreateNamedPipeA(name_.c_str(), PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 1 << 16, 1 << 16, 0,
                            nullptr);
  }
  void Run() {
    for (;;) {
      HANDLE h = first_;
      first_ = INVALID_HANDLE_VALUE;
      if (h == INVALID_HANDLE_VALUE) h = Create(false);
      if (h == INVALID_HANDLE_VALUE) { Sleep(500); continue; }
      BOOL ok = ConnectNamedPipe(h, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
      if (ok) Serve(h);
      FlushFileBuffers(h);
      DisconnectNamedPipe(h);
      CloseHandle(h);
    }
  }
  void Serve(HANDLE h) {
    std::string acc;
    char buf[4096];
    for (;;) {
      DWORD n = 0;
      if (!ReadFile(h, buf, sizeof buf, &n, nullptr) || n == 0) return;
      acc.append(buf, n);
      if (acc.size() > (1u << 20) && acc.find('\n') == std::string::npos) {  // a line over 1 MB: answer and drop the client
        static const char e[] = "{\"ok\":false,\"error\":\"line longer than 1 MB\"}\n";
        DWORD w = 0;
        WriteFile(h, e, (DWORD)sizeof(e) - 1, &w, nullptr);
        return;
      }
      size_t nl;
      while ((nl = acc.find('\n')) != std::string::npos) {
        std::string line = acc.substr(0, nl);
        acc.erase(0, nl + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        std::string reply = mb_.Submit(line) + "\n";
        DWORD w = 0;
        BOOL wok = WriteFile(h, reply.data(), (DWORD)reply.size(), &w, nullptr);
        if (wok) FlushFileBuffers(h);
        mb_.Done();
        if (!wok) return;
      }
    }
  }
  Mailbox& mb_;
  std::string name_;
  HANDLE first_ = INVALID_HANDLE_VALUE;
  std::thread th_;
};
#else
// The phone: 127.0.0.1:<port> (`name` = the port when it is a number, else 47140). One client at a time, as the pipe.
// Session 98 stage C review: 127.0.0.1 is shared by every app on a phone, so a client's FIRST line must be the run's token
// (--pipe-token, written by tools\android\run.ps1 into viewer_args.txt); a wrong one is answered with an error and the
// connection closed. No token = the server refuses to start.
class PipeServer {
 public:
  PipeServer(Mailbox& mb, std::string name) : mb_(mb) {
    const int p = std::atoi(name.c_str());
    port_ = p > 0 && p < 65536 ? p : 47140;
  }
  void SetToken(std::string t) { token_ = std::move(t); }
  bool Start(std::string& err) {
    if (token_.size() < 8) return err = "no --pipe-token (8+ characters): the command port stays closed", false;
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return err = "socket() failed (errno " + std::to_string(errno) + ")", false;
    const int one = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET, a.sin_port = htons(uint16_t(port_)), a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd_, (sockaddr*)&a, sizeof a) != 0 || listen(fd_, 1) != 0) {
      err = "cannot listen on 127.0.0.1:" + std::to_string(port_) + " (errno " + std::to_string(errno) + "; another viewer owns it?)";
      close(fd_), fd_ = -1;
      return false;
    }
    th_ = std::thread([this] { Run(); });
    th_.detach();
    return true;
  }
  int Port() const { return port_; }

 private:
  void Run() {
    for (;;) {
      const int c = accept(fd_, nullptr, nullptr);
      if (c < 0) { std::this_thread::sleep_for(std::chrono::milliseconds(500)); continue; }
      Serve(c);
      close(c);
    }
  }
  static bool WriteAll(int c, const char* p, size_t n) {
    while (n) {
      const ssize_t w = send(c, p, n, MSG_NOSIGNAL);
      if (w <= 0) return false;
      p += w, n -= size_t(w);
    }
    return true;
  }
  void Serve(int c) {
    std::string acc;
    char buf[4096];
    bool authed = false;
    for (;;) {
      const ssize_t n = recv(c, buf, sizeof buf, 0);
      if (n <= 0) return;
      acc.append(buf, size_t(n));
      if (acc.size() > (1u << 20) && acc.find('\n') == std::string::npos) {
        static const char e[] = "{\"ok\":false,\"error\":\"line longer than 1 MB\"}\n";
        WriteAll(c, e, sizeof(e) - 1);
        return;
      }
      size_t nl;
      while ((nl = acc.find('\n')) != std::string::npos) {
        std::string line = acc.substr(0, nl);
        acc.erase(0, nl + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        if (!authed) {  // the first line is the token
          if (line != token_) {
            static const char e[] = "{\"ok\":false,\"error\":\"wrong token\"}\n";
            WriteAll(c, e, sizeof(e) - 1);
            return;
          }
          authed = true;
          static const char ok_line[] = "{\"ok\":true,\"auth\":true}\n";
          if (!WriteAll(c, ok_line, sizeof(ok_line) - 1)) return;
          continue;
        }
        const std::string reply = mb_.Submit(line) + "\n";
        const bool ok = WriteAll(c, reply.data(), reply.size());
        mb_.Done();
        if (!ok) return;
      }
    }
  }
  Mailbox& mb_;
  std::string token_;
  int port_ = 47140, fd_ = -1;
  std::thread th_;
};
#endif

}  // namespace view
