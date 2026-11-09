// Session 98 (tron video): plays a tron MP4 as pictures for the stage. Portable interface; the Windows backend (Media Foundation)
// is video_player.cpp. Two ways to ask for a picture at video time t (seconds; the video loops past its end):
//   FrameAt(t)            blocking, deterministic: the picture whose time stamp is the last one <= t (offline renders, shots)
//   Request(t) + TakeLatest   non-blocking: a worker thread decodes toward the newest request and drops late pictures (live view)
// Nothing here reads the game: the MP4 is the one the launcher made on the player's machine (or the player's own mod).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace view {

struct VideoFrame {
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;  // straight RGBA8, alpha 255
  bool ok() const { return w > 0 && h > 0 && rgba.size() == size_t(w) * h * 4; }
};

class VideoPlayer {
 public:
  VideoPlayer();
  ~VideoPlayer();
  VideoPlayer(const VideoPlayer&) = delete;
  VideoPlayer& operator=(const VideoPlayer&) = delete;
  // Opens `utf8_path` (stops any worker first). False + err when Windows can't read it.
  bool Open(const std::string& utf8_path, std::string& err);
  void Close();
  bool IsOpen() const;
  const std::string& Path() const;
  double Duration() const;  // seconds (0 = unknown)
  bool FrameAt(double t, VideoFrame& out, std::string& err);
  void Request(double t);  // starts the worker on first use; the latest request wins
  // Copies the newest decoded picture when its serial differs from `serial` (then updates it). False = nothing new.
  bool TakeLatest(VideoFrame& out, uint64_t& serial);

 private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};

}  // namespace view
