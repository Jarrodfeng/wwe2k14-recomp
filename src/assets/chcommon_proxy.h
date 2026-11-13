// The wrestlers' plane-shadow proxy from the files (Session 101, lane 99 = SO; docs\renderer\SHADOW_PROMOTION_MAP.md sections 1.2 / 4, docs\systems\shadows.md 6.1).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Reads the player's own pac\ch\chCommon.pac at run time; nothing is kept on disk.
//
// chCommon.pac entry 0 is a JBOY of 8 meshes: the model the game makes with CCharCommonModelDataReader::Factory_ShadowMdel and draws only in the plane-shadow pass
// (positions bit-equal to the stream's, mesh index = DrawOrigin::part, measured on every plane draw of Check B). This file is the FILE side only: the meshes as stored.
// The vertex writer is shadow_caster_vertex.h (layout "chCommon proxy"), the index buffer wrestler_geom::BuildIndices (groups joined by 0xFFFF; lane 192).
// Threads: Load blocks (call it from a worker or at start-up); the result is plain data.
#pragma once
#include <memory>
#include <string>

// pac.h needs stb_image.h's declaration (stbi_zlib_decode_buffer) before it. This header only DECLARES it (the implementation is static inside chcommon_proxy.cpp).
#ifndef STB_IMAGE_IMPLEMENTATION
#if __has_include("stb_image.h")
#include "stb_image.h"
#else
#include "../../thirdparty/rexglue-sdk-src/thirdparty/stb/stb_image.h"
#endif
#endif
#include "jboy_parts.h"
#include "pac.h"   // (Bytes)

namespace assets::chcommon_proxy {

struct ProxySet {
  Bytes blob;                    // the decoded entry; the Parts' pointers point into it (never moved after Load)
  jboy_parts::Parts parts;
  const jboy_parts::Part* Mesh(int i) const {
    return i >= 0 && size_t(i) < parts.parts.size() && parts.parts[size_t(i)].ok && parts.parts[size_t(i)].tiles ? &parts.parts[size_t(i)] : nullptr;
  }
};

// Reads pac\ch\chCommon.pac of `game_root` and parses entry 0. Null + err when the pac or the entry is missing.
std::shared_ptr<const ProxySet> Load(const std::string& game_root, std::string& err);

}  // namespace assets::chcommon_proxy
