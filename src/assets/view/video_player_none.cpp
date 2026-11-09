// Session 98 (Arena Viewer phone port, stage C): video_player.h where there is no video decoder (the phone). Open always fails, so
// the stage's tron slots keep their stills (tron_video.h falls back to the PNG frames of the tron cache, or the game's own picture).
// (Since the phone tron video work the phone builds video_player_android.cpp; this stays for a platform with no decoder.)
#include "video_player.h"

namespace view {

struct VideoPlayer::Impl {
  std::string path;
};

VideoPlayer::VideoPlayer() : p_(std::make_unique<Impl>()) {}
VideoPlayer::~VideoPlayer() = default;
bool VideoPlayer::Open(const std::string& utf8_path, std::string& err) {
  (void)utf8_path;
  err = "no video playback on this platform (stills only)";
  return false;
}
void VideoPlayer::Close() {}
bool VideoPlayer::IsOpen() const { return false; }
const std::string& VideoPlayer::Path() const { return p_->path; }
double VideoPlayer::Duration() const { return 0; }
bool VideoPlayer::FrameAt(double, VideoFrame&, std::string& err) {
  err = "no video playback on this platform";
  return false;
}
void VideoPlayer::Request(double) {}
bool VideoPlayer::TakeLatest(VideoFrame&, uint64_t&) { return false; }

}  // namespace view
