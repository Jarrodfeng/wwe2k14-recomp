// wwe2k14 - Session 101 lane 17 (case 7041, fixed in fec23058): the binder's rule for a texture content whose format no longer fits the image.
// A content taken for an older record of an image the reader recreated in another format would overrun the upload (and kept stale bytes), so
// it is skipped and counted; the image's newer content follows. Split out so the offline harness runs the real rule. No game or SDK includes.
#pragma once

#include <cstdint>
#include <vector>

namespace rdr_adapter {

// image_formats: the storage format each image id was last made with (0 = not made yet). content_format: the format the content was stored in.
inline bool ContentIsStale(const std::vector<uint32_t>& image_formats, uint32_t image, uint32_t content_format) {
  return image < image_formats.size() && image_formats[image] && content_format != image_formats[image];
}

// lane 195 (a record refreshed for a new shape while an older-shape content was still in the frame): the upload copies the extent of the IMAGE's
// mip, so a subresource whose mip / layer is outside the synced record, or whose texel extent is not that mip's, would read past its staging
// buffer (the run 7022 / Adreno class) or index past the image's mip states. rec_*: the synced ImageRecord of the image.
inline bool SubresourceFits(uint32_t rec_width, uint32_t rec_height, uint32_t rec_mips, uint32_t rec_layers, uint32_t mip, uint32_t layer, uint32_t width,
                            uint32_t height) {
  if (mip >= rec_mips || layer >= rec_layers) return false;
  const uint32_t mw = (rec_width >> mip) ? (rec_width >> mip) : 1u, mh = (rec_height >> mip) ? (rec_height >> mip) : 1u;
  return width == mw && height == mh;
}

}  // namespace rdr_adapter
