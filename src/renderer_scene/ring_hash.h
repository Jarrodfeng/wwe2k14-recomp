// S101 lane 70 follow-up (F1): the content hash of the ring's file textures, the same function the image-content feeds use (xxh3-64 of the texel bytes, all mips the stream image holds,
// image_content_table.h). Header only; xxHash comes from the SDK's third-party folder (the game build) or any -I that holds xxhash.h (fview / the unit test).
#pragma once
#include <cstddef>
#include <cstdint>

#define XXH_INLINE_ALL
#if __has_include("xxhash.h")
#include "xxhash.h"
#else
#include "../../thirdparty/rexglue-sdk-src/thirdparty/xxHash/xxhash.h"
#endif

namespace rdr_scene {
inline uint64_t RingXxh3(const void* data, size_t bytes) { return XXH3_64bits(data, bytes); }  // = rdr_adapter::ImageContentHash (image_content_table.h, the contract)
}  // namespace rdr_scene
