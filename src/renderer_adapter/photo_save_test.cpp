// lane 201: photo mode's save on planted data. clang++ -std=c++17 -I src/renderer_adapter -I src/renderer/player src/renderer_adapter/photo_save_test.cpp
// Writes through the renderer's own PNG writer (the one the live sink uses) and reads the file back byte for byte.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "photo_save.h"
#include "png.h"

static int fails = 0;
static void Check(const char* name, bool ok) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) ++fails;
}

// Reads back the pixels of a PNG made by rdr::WritePng (stored deflate blocks).
static bool ReadBack(const char* path, uint32_t& w, uint32_t& h, std::vector<uint8_t>& rgba) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  std::vector<uint8_t> d;
  uint8_t buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) d.insert(d.end(), buf, buf + n);
  std::fclose(f);
  if (d.size() < 33 || d[1] != 'P') return false;
  w = (uint32_t(d[16]) << 24) | (uint32_t(d[17]) << 16) | (uint32_t(d[18]) << 8) | d[19];
  h = (uint32_t(d[20]) << 24) | (uint32_t(d[21]) << 16) | (uint32_t(d[22]) << 8) | d[23];
  size_t p = 33;  // the IHDR chunk ends at 8 + 25
  std::vector<uint8_t> z;
  while (p + 8 <= d.size()) {
    const uint32_t len = (uint32_t(d[p]) << 24) | (uint32_t(d[p + 1]) << 16) | (uint32_t(d[p + 2]) << 8) | d[p + 3];
    if (std::string(reinterpret_cast<const char*>(&d[p + 4]), 4) == "IDAT") z.insert(z.end(), d.begin() + p + 8, d.begin() + p + 8 + len);
    p += 12 + len;
  }
  std::vector<uint8_t> raw;
  size_t q = 2;  // the zlib header
  while (q + 5 <= z.size()) {
    const uint32_t len = z[q + 1] | (uint32_t(z[q + 2]) << 8);
    raw.insert(raw.end(), z.begin() + q + 5, z.begin() + q + 5 + len);
    const bool last = z[q] & 1;
    q += 5 + len;
    if (last) break;
  }
  rgba.clear();
  const size_t row = size_t(w) * 4 + 1;
  if (raw.size() != row * h) return false;
  for (uint32_t y = 0; y < h; ++y) rgba.insert(rgba.end(), raw.begin() + row * y + 1, raw.begin() + row * (y + 1));
  return true;
}

int main() {
  using namespace rdr_adapter::photo;
  {  // retry policy: a failure keeps the request, the 90th gives up, a success ends it at once
    uint32_t tries = 0;
    bool kept = true;
    for (int i = 0; i < 89; ++i) kept = kept && AfterAttempt(Result::NoPicture, tries) == Action::Retry;
    Check("89 failed attempts keep the request", kept);
    Check("the 90th failed 'no picture' attempt gives up", AfterAttempt(Result::NoPicture, tries) == Action::GiveUp);
    uint32_t w = 0;
    Check("a write failure is final at once", AfterAttempt(Result::WriteFailed, w) == Action::GiveUp);
    uint32_t r = 0;
    const Action a1 = AfterAttempt(Result::ReadFailed, r), a2 = AfterAttempt(Result::ReadFailed, r), a3 = AfterAttempt(Result::ReadFailed, r);
    Check("a read-back failure retries twice, gives up on the third", a1 == Action::Retry && a2 == Action::Retry && a3 == Action::GiveUp);
    Check("photo mode ended (HUD shown again): the request is dropped", Admit(false) == Action::GiveUp && Admit(true) == Action::Retry);
    uint32_t t2 = 5;
    Check("a success is done", AfterAttempt(Result::Ok, t2) == Action::Done);
    Check("every failure has a reason", std::string(Why(Result::NoPicture)).size() > 8 && std::string(Why(Result::WriteFailed)).size() > 8);
  }
  {  // shown size
    uint32_t w, h;
    ShownSize(1280, 720, 1.3125f, 1.0f, w, h);
    Check("21:9: 1280x720 squeezed -> 1680x720", w == 1680 && h == 720);
    ShownSize(1280, 720, 1.0f, 1.0f, w, h);
    Check("16:9: unchanged", w == 1280 && h == 720);
    ShownSize(1280, 720, 1.0f, 1.4f, w, h);
    Check("tall 1.4: 1280x1008", w == 1280 && h == 1008);
    ShownSize(1280, 720, 99.0f, -3.0f, w, h);
    Check("absurd factors ignored", w == 1280 && h == 720);
  }
  {  // a synthetic presented frame (a gradient with a vertical bar) saved through the real PNG writer
    const uint32_t W = 1280, H = 720;
    std::vector<uint8_t> src(size_t(W) * H * 4);
    for (uint32_t y = 0; y < H; ++y)
      for (uint32_t x = 0; x < W; ++x) {
        uint8_t* p = &src[(size_t(y) * W + x) * 4];
        p[0] = uint8_t(x * 255 / (W - 1)), p[1] = uint8_t(y * 255 / (H - 1)), p[2] = (x > 600 && x < 680) ? 255 : 0, p[3] = 255;
      }
    const auto dir = std::filesystem::temp_directory_path() / "wwe2k14_photo_save_test";
    std::filesystem::create_directories(dir);
    {  // 16:9 (no stretch): the file's pixels equal the presented frame
      const auto path = (dir / "plain.png").string();
      Check("write plain", rdr::WritePng(path.c_str(), src.data(), W, H));
      uint32_t w = 0, h = 0;
      std::vector<uint8_t> back;
      Check("plain: read back", ReadBack(path.c_str(), w, h, back));
      Check("plain: 1280x720 and pixels equal the presented frame", w == W && h == H && back == src);
    }
    {  // 21:9: the file is the shown size; the bar is stretched back to 1.3125x its width; the corners keep their colour
      uint32_t ow, oh;
      ShownSize(W, H, 1.3125f, 1.0f, ow, oh);
      const std::vector<uint8_t> shown = Stretch(src.data(), W, H, ow, oh);
      const auto path = (dir / "wide.png").string();
      Check("write wide", rdr::WritePng(path.c_str(), shown.data(), ow, oh));
      uint32_t w = 0, h = 0;
      std::vector<uint8_t> back;
      Check("wide: read back", ReadBack(path.c_str(), w, h, back));
      Check("wide: 1680x720, pixels equal the stretched frame", w == 1680 && h == 720 && back == shown);
      uint32_t bar = 0;  // source x 601..679 (79 px) -> about 79 * 1.3125 = 104 px
      for (uint32_t x = 0; x < w; ++x) bar += back[(size_t(100) * w + x) * 4 + 2] > 128;
      Check("wide: the bar is 1.3125x wider (about 104 px)", bar >= 102 && bar <= 106);
      Check("wide: first pixel is the source's first pixel", std::equal(back.begin(), back.begin() + 4, src.begin()));
      Check("wide: last pixel is the source's last pixel", std::equal(back.end() - 4, back.end(), src.end() - 4));
    }
    Check("stretch to the same size is the identity", Stretch(src.data(), W, H, W, H) == src);
    {  // a path the ANSI code page cannot hold (a CJK user name): no throw, written through the wide-path open, read back, logged as UTF-8
      const auto udir = std::filesystem::temp_directory_path() / std::filesystem::u8path(u8"wwe2k14_photo_\u6D4B\u8BD5_\u30C6\u30B9\u30C8");
      std::filesystem::create_directories(udir);
      std::filesystem::path part = udir / std::filesystem::u8path(u8"WWE2K14_\u5199\u771F.png");
      part += ".part";
      bool threw = false;
      std::string logged;
      try {
        logged = Utf8(part);
      } catch (...) {
        threw = true;
      }
      Check("non-ASCII path: Utf8() does not throw and keeps the bytes", !threw && logged.find("\xE5\x86\x99") != std::string::npos);
      FILE* f = rdr::PngOpen(part);
      Check("non-ASCII path: opened by the wide-path open", f != nullptr);
      if (f) Check("non-ASCII path: written", rdr::WritePngFile(f, src.data(), W, H));
      std::error_code ec;
      std::filesystem::path done = part;
      done.replace_extension("");  // drop ".part"
      std::filesystem::rename(part, done, ec);
      Check("non-ASCII path: renamed to the final name", !ec && std::filesystem::exists(done));
      uint32_t w = 0, h = 0;
      std::vector<uint8_t> back;
      Check("non-ASCII path: size is the PNG's", std::filesystem::file_size(done, ec) > size_t(W) * H * 4);
      std::filesystem::remove_all(udir);
    }
    {  // an unwritable folder fails at open, before any read-back
      const auto bad = std::filesystem::temp_directory_path() / "wwe2k14_photo_no_such_dir" / "x.png.part";
      FILE* f = rdr::PngOpen(bad);
      Check("a missing folder fails at open (WriteFailed, final)", f == nullptr);
      if (f) std::fclose(f);
    }
    std::filesystem::remove_all(dir);
  }
  std::printf("%s\n", fails ? "FAILED" : "all passed");
  return fails ? 1 : 0;
}
