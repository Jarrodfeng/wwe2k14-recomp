// S102 RT1 (Q9): lasting_prune.h CopyStreaks::Note with `note_flat` (rectangle lists kept across frames) against the shipped Note (a fresh hash map every frame): the per-image streak table must be
// IDENTICAL after every frame of a long random sequence (images that recur, vanish, move, and come back after a gap). `plant` runs the stale-rectangles fault, which must differ.
//   clang++ -std=c++20 -I src tools\render_lean\note_flat_test.cpp -o note_flat_test.exe    (py -I tools\render_lean\lean_unit.py does it)
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "../../src/renderer_adapter/lasting_prune.h"

using namespace rdr_adapter;

namespace {
struct Syn {
  std::vector<Op> ops;
  std::vector<rdr::CopyOp> copies;
  void CopyOp(uint32_t dst, int32_t x, int32_t y, uint32_t w, uint32_t h) {
    rdr::CopyOp c;
    c.source.image = rdr::ImageId(1);
    c.dest.image = rdr::ImageId(dst);
    c.dest_rect = {x, y, w, h};
    copies.push_back(c);
    ops.push_back({OpKind::Copy, uint32_t(copies.size() - 1)});
  }
  LoadedFrame Frame() const {
    LoadedFrame f;
    f.ops = {ops.data(), uint32_t(ops.size())};
    f.copies = {copies.data(), uint32_t(copies.size())};
    return f;
  }
};
bool Same(const lasting::CopyStreaks& a, const lasting::CopyStreaks& b) {
  if (a.img_seen.size() != b.img_seen.size()) return false;
  for (const auto& kv : a.img_seen) {
    const auto it = b.img_seen.find(kv.first);
    if (it == b.img_seen.end() || it->second.streak != kv.second.streak || it->second.last != kv.second.last || it->second.rects != kv.second.rects) return false;
  }
  return true;
}
}  // namespace

int main(int argc, char** argv) {
  const bool plant = argc > 1 && std::string(argv[1]) == "plant";
  std::mt19937 rng(12345);
  lasting::CopyStreaks off, on;
  off.by_image = on.by_image = true;
  on.note_flat = true;
  on.plant_note_flat_stale = plant;
  int bad = 0;
  uint64_t taken = 0;
  for (int frame = 0; frame < 400; ++frame) {
    Syn s;
    const int images = 1 + int(rng() % 6);
    for (int i = 0; i < images; ++i) {
      if (rng() % 5 == 0) continue;   // an image that skips a frame (streak resets, rectangles must not leak into its return)
      const uint32_t dst = 10 + uint32_t(rng() % 8);
      const int tiles = 1 + int(rng() % 3);
      for (int t = 0; t < tiles; ++t) s.CopyOp(dst, int32_t(rng() % 4) * 16 + (frame % 3), int32_t(rng() % 4) * 16, 16 + 16 * (rng() % 3), 16 + 16 * (rng() % 3));
    }
    const LoadedFrame f = s.Frame();
    off.Note(f);
    on.Note(f);
    if (!Same(off, on)) {
      if (++bad == 1) std::printf("DIFF at frame %d: the streak tables differ\n", frame);
    }
    taken += on.note_flat_frames != 0;
  }
  std::printf("%s: 400 frames, %d differing, flat frames %llu\n", plant ? "plant (stale rectangles)" : "real", bad, static_cast<unsigned long long>(on.note_flat_frames));
  if (!plant && on.note_flat_frames != 400) { std::printf("FAIL the flat path did not run on every frame\n"); return 1; }
  return bad ? 1 : 0;
}
