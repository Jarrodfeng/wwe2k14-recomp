// S108 lane SHOT (docs\research\s108\SHOT_plan.md sections 1, 7): the host state a ONE-frame record lacks, saved with a subject-shot record so the offline rig draws it as the host did.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Three additive sections (an older reader skips them; a record without them is byte-identical to before):
//  68 kScBodyScales    = BodyScaleRow[]   one row per wrestler object the host drew: the scale BodyScaleFor gave it (the record carries no scale: without a row the rig draws that wrestler as a HOLE)
//  69 kScLiquidImages  = LiquidImageRow[] one row per live liquid map (character): its persistent 1024x512 image, mip 0, RGBA8, read back AFTER the frame's paint
//  70 kScLiquidBytes   = uint8[]          the images' bytes (element size 1); a row's bytes are [offset, offset + nbytes)
// Written together or not at all, and the header flag kSceneSnapshot says "all three sections are in the directory" (counts may be 0). A reader that finds the flag and a section missing
// reports it (SnapshotProblem); a bad row / a range past the bytes drops the three together (snapshot_errors).
// Reader rule (live_map_sweat.cpp): a map with an image row STARTS from that image and that frame's items are NOT painted again (the image already holds them).
// The created-superstar (CAW) bytes need no section: the saver fills every kGmNoBytes row of the guest-model blobs (62-64) from the host's byte store.
// Hard rule 1: a saved record holds data built from the game's files: local only, never committed, no fixture is made from one.
#pragma once
#include <cstdint>

namespace rdr_scene {

inline constexpr uint32_t kScBodyScales = 68, kScLiquidImages = 69, kScLiquidBytes = 70;   // 67 = kScChangeColour
inline constexpr uint32_t kSnapshotLayout = 1;
inline constexpr uint32_t kSceneSnapshot = 32u;   // SceneHeader::flags bit 5 (bit 0 = kSceneStagePlaying, bits 1-4 = cam_gate.h): the three snapshot sections are all present
inline constexpr uint32_t kSnapSeenScales = 1, kSnapSeenImages = 2, kSnapSeenBytes = 4;   // SceneTables::snapshot_seen: which of the sections the file's directory had
inline constexpr uint32_t kLiquidImageFormatRgba8 = 1;

struct BodyScaleRow {   // 16 bytes
  uint32_t layout = kSnapshotLayout;
  uint32_t object = 0;     // the wrestler's engine object (the skeleton address: the key of every wrestler row, SceneTables::engine_objects[i])
  float scale = 0.f;       // BodyScaleFor(true, object): 0.89-1.01, finite; a wrestler with no scale gets NO row (a hole, as in the host)
  uint32_t reserved = 0;
};
static_assert(sizeof(BodyScaleRow) == 16, "BodyScaleRow is a flat POD table");

struct LiquidImageRow {   // 40 bytes
  uint32_t layout = kSnapshotLayout;
  uint32_t map = 0;        // LiquidMapRow::map: the map object (guest address) = the persistent image's key
  uint32_t object = 0;     // LiquidMapRow::object: the character's skeleton object (0 = not known)
  uint32_t width = 0, height = 0;
  uint32_t format = kLiquidImageFormatRgba8;
  uint32_t nbytes = 0;     // width * height * 4
  uint32_t reserved = 0;
  uint64_t offset = 0;     // into the kScLiquidBytes section
};
static_assert(sizeof(LiquidImageRow) == 40, "LiquidImageRow is a flat POD table");

}  // namespace rdr_scene
