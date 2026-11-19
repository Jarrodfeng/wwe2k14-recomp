// The prop pictures' image cache (Session 103, lane D6; review 1 / 2 / 6). Pure over a Backend (the unit uses a fake, LiveCrowd the renderer): one lookup per key per FRAME (BeginFrame clears the frame's map, so a row whose
// bytes changed is re-made), a row is used only when CrowdPictureValid says it is exactly the bytes of its size, and an image no row of the frame names is released at EndFrame.
// Backend: uint32_t Create(const CrowdPictureRow&) (0 = failed), void Update(uint32_t id, const CrowdPictureRow&), void Destroy(uint32_t id), uint32_t Grey().
#pragma once
#include <cstdint>
#include <map>
#include <vector>

#include "scene_record.h"

namespace rdr_scene::live {

class PictureCache {
 public:
  // Test only: 1 = the frame's map is NOT cleared (the fault of review 1: a picture is never looked at again after its first frame).
  int plant = 0;
  void BeginFrame() {
    if (plant != 1) now_.clear();
  }
  template <class B>
  uint32_t For(uint32_t key, const std::vector<CrowdPictureRow>& rows, B& b) {
    if (const auto it = now_.find(key); it != now_.end()) return it->second;
    return now_[key] = Make(key, rows, b);
  }
  template <class B>
  void EndFrame(B& b) {   // release the images no row of this frame names
    for (auto it = images_.begin(); it != images_.end();) {
      if (now_.count(it->first)) { ++it; continue; }
      if (it->second.id) b.Destroy(it->second.id);
      it = images_.erase(it);
    }
  }
  template <class B>
  void Clear(B& b) {
    for (auto& kv : images_)
      if (kv.second.id) b.Destroy(kv.second.id);
    images_.clear(), now_.clear();
  }
  size_t Resident() const { return images_.size(); }
  uint64_t made = 0, remade = 0;

 private:
  struct Img { uint32_t id = 0; uint64_t hash = 0; };
  template <class B>
  uint32_t Make(uint32_t key, const std::vector<CrowdPictureRow>& rows, B& b) {
    const CrowdPictureRow* row = nullptr;
    for (const CrowdPictureRow& r : rows)
      if (r.key == key && CrowdPictureValid(r)) { row = &r; break; }
    if (!row) return b.Grey();
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&](const uint8_t* p, size_t n) { for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull; };
    const uint32_t head[3] = {row->width, row->height, row->format};
    mix(reinterpret_cast<const uint8_t*>(head), sizeof head), mix(row->bytes, row->nbytes);
    Img& im = images_[key];
    if (im.id && im.hash == h) return im.id;
    if (im.id) b.Destroy(im.id), im.id = 0, ++remade;
    im.id = b.Create(*row), im.hash = h;
    if (!im.id) return b.Grey();
    b.Update(im.id, *row);
    ++made;
    return im.id;
  }
  std::map<uint32_t, Img> images_;
  std::map<uint32_t, uint32_t> now_;
};

}  // namespace rdr_scene::live
