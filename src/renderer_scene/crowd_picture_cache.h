// The prop pictures' image cache (Session 103, lane D6; review 1 / 2 / 6). Pure over a Backend (the unit uses a fake, LiveCrowd the renderer): one lookup per key per FRAME (BeginFrame clears the frame's map, so a row whose
// bytes changed is re-made), a row is used only when CrowdPictureValid says it is exactly the bytes of its size, and an image no row of the frame names is released at EndFrame.
// Backend: uint32_t Create(const CrowdPictureRow&) (0 = failed), void Update(uint32_t id, const CrowdPictureRow&), void Destroy(uint32_t id), uint32_t Grey().
#pragma once
#include <cstdint>
#include <map>
#include <type_traits>
#include <utility>
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
  template <class E, class B>
  uint32_t For(uint32_t key, const std::vector<E>& rows, B& b) {   // S105 A12: Row = CrowdPictureRow or LiquidPictureRow (PictureValid, scene_record.h)
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
  template <class R> static const R& Deref(const R& r) { return r; }
  template <class R> static const R& Deref(const R* p) { return *p; }   // a list of pointers into the record (S105 A12: E2Input holds no copies of the 32 KB rows)
  template <class E, class B>
  uint32_t Make(uint32_t key, const std::vector<E>& rows, B& b) {
    using Row = std::decay_t<decltype(Deref(std::declval<const E&>()))>;
    const Row* row = nullptr;
    for (const E& e : rows) {
      const Row& r = Deref(e);
      if (r.key == key && PictureValid(r)) { row = &r; break; }
    }
    if (!row) return b.Grey();
    const uint64_t h = PictureHash(*row);   // a liquid row carries it (sealed once at the read); a crowd row is hashed here as before
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
