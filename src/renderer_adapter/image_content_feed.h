// S101 lane 66 (UI P2a, docs\renderer\UI_PROMOTION_MAP.md 4): the ONE feeder of the image-content table (image_content_table.h), used by fview (session.cpp), rplay
// (promotion_rig.h) and the live render thread (live.cpp), so a capture and a live game fill it through the same code and the same hash.
//
// What it records per content write (a frame's ContentRef = "image X takes this content before the frame's first op"): the texture's width / height / format / mip
// count and ImageContentHash (xxh3-64, the contract's one hash, image_content_table.h) of its layer-0 mips 0..mips-1, each mip tightly packed, in mip order. The UI's file
// index (ui_ours_core.h) hashes the same bytes of the same picture from the player's pac; equal hash + equal size + byte class = the same picture.
//
// ContentHasher streams the SAME function (XXH3_64bits_update / digest: identical to the one-shot ImageContentHash over the same bytes) and can be read after any mip, so
// a file texture's every mip PREFIX hashes in one pass (a capture blob may hold a prefix of the mip chain). No game or SDK includes.
#pragma once
#include <cstdint>
#include <cstring>

#include "capture_reader.h"
#include "image_content_table.h"

namespace rdr_adapter {

class ContentHasher {
 public:
  ContentHasher() { XXH3_64bits_reset(&state_); }
  void Update(const uint8_t* p, size_t n) { XXH3_64bits_update(&state_, p, n); }
  // The digest of everything fed so far (the hasher stays usable).
  uint64_t Digest() const { return XXH3_64bits_digest(&state_); }

 private:
  XXH3_state_t state_;
};

// What a content blob (textures.bin RCTX) says about its image, in the table's terms. false: the blob is not a 2D texture this feed describes (layers > 1, no mip 0,
// a data region that does not fit): the table is not told, and the hook keeps the stream's image.
inline bool DescribeContent(const LoadedTexture& t, ImageContent& out) {
  if (!t.subresources || !t.data) return false;
  const TextureBlobHeader& h = t.header;
  if (h.data_bytes == 0 || h.data_bytes > t.bytes.size() || t.data + h.data_bytes > t.bytes.data() + t.bytes.size()) return false;
  uint32_t mips = 0, w = 0, ht = 0;
  for (uint32_t s = 0; s < h.subresource_count; ++s) {
    const Subresource& sub = t.subresources[s];
    if (sub.layer != 0) return false;  // (cube / array content: not the UI's)
    if (sub.mip == 0) w = sub.width, ht = sub.height;
    if (sub.mip + 1 > mips) mips = sub.mip + 1;
    if (sub.offset + uint64_t(sub.row_bytes) * sub.rows > h.data_bytes) return false;
  }
  if (!w || !ht || !mips || mips != h.subresource_count) return false;
  out.width = w, out.height = ht, out.format = h.format, out.mips = mips;
  // The contract's bytes: mips 0..mips-1, each tightly packed, in mip order (a subresource is row_bytes x rows, tight: CAPTURE_FORMAT.md), whatever order the blob lists them in.
  ContentHasher hasher;
  for (uint32_t m = 0; m < mips; ++m) {
    const Subresource* sub = nullptr;
    for (uint32_t s = 0; s < h.subresource_count; ++s)
      if (t.subresources[s].mip == m) sub = &t.subresources[s];
    if (!sub) return false;
    hasher.Update(t.data + sub->offset, size_t(sub->row_bytes) * sub->rows);
  }
  out.hash = hasher.Digest();
  return true;
}

// Feed one bound frame's content writes into `table`. `source` is a CaptureReader or the live source (images() + ReadTexture, as Binder::ApplyContents uses).
// Call it where ApplyContents is called, BEFORE the live store drops the blobs. Movie planes (ImageRecord::kImageMoviePlane, re-read every frame) are told to the
// table with hash 0 (width / height / format from the record) and never hashed; blobs over kMaxFeedBytes are not hashed either (the UI's art is far smaller).
// Returns the number of images put.
inline constexpr uint64_t kMaxFeedBytes = 16u << 20;
template <class Source>
uint32_t FeedImageContents(const LoadedFrame& frame, const Source& source, ImageContentTable& table, LoadedTexture& scratch, bool erase_copy_dests = true) {
  uint32_t put = 0;
  const auto& records = source.images();
  for (const ContentRef& ref : frame.contents) {
    const ImageRecord* rec = ref.image && ref.image <= records.size() && records[ref.image - 1].id == ref.image ? &records[ref.image - 1] : nullptr;
    if (rec && (rec->flags & kImageMoviePlane)) {
      ImageContent c;
      c.width = rec->width, c.height = rec->height, c.format = rec->format, c.mips = rec->mips, c.hash = 0;
      table.Put(ref.image, c);
      ++put;
      continue;
    }
    if (ref.size > kMaxFeedBytes || !source.ReadTexture(ref, scratch)) continue;
    ImageContent c;
    const bool described = DescribeContent(scratch, c);
    // A source that LENDS its bytes (the live store: ApplyContents still needs them) takes them back now (lane 66 review C1: nothing is copied under its lock).
    if constexpr (requires { source.GiveBack(ref, scratch); }) source.GiveBack(ref, scratch);
    if (!described) {
      table.Erase(ref.image);  // (a content this feed cannot describe replaces what it knew: no info = the stream's image stays)
      continue;
    }
    table.Put(ref.image, c);
    ++put;
  }
  // A GPU copy / pack into an image (it keeps its id, gets no content: reader.h) makes the CPU content the table knows stale: forget it, so a hook keeps the stream's draw
  // (lane 66 review F1). The copy runs after the frame's contents land, so this goes after the loop. `erase_copy_dests` false = the planted bug (gpu_copy_after_join).
  if (erase_copy_dests)
    for (const rdr::CopyOp& copy : frame.copies)
      if (copy.dest.image) table.Erase(copy.dest.image);
  return put;
}

}  // namespace rdr_adapter
