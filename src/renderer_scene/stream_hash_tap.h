// The dev parity tap between the stream path's UI decision (ui_ours_core.cpp UiCore::Decide) and the stream-free picture source (src\scene_live\picture_tap.h): Session 103, landing 2, lane PN23.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Pure: no includes but the standard library.
// Null unless a dev build was started with --wwe2k14_rdr_picture_source: then Decide reports (key, the stream image's content hash, its mip count) so the source can compare it with its own hash of the same key
// (PictureSource::NoteStreamHash -> guest_parity_ok / bad). One relaxed load per claimed 2D draw when off. Read only.
#pragma once
#include <atomic>
#include <cstdint>

namespace rdr_scene {
using StreamHashTapFn = void (*)(uint32_t key, uint64_t hash, uint32_t mips);
inline std::atomic<StreamHashTapFn> g_stream_hash_tap{nullptr};
}  // namespace rdr_scene
