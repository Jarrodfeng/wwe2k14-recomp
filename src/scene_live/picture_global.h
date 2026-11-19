// The process-wide PictureSource (S103 landing 2, piece PN4). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// One instance, two users in two translation units: the game side's feeders (picture_tap.h, in the game unit: births, deaths and first sights) and the scene host's resolver (src\renderer_host\host_start.cpp: its own
// unit, which must NOT include picture_tap.h: that header defines the --wwe2k14_rdr_picture_source cvar). An `inline` function's static is one object across units, so both reach the same source through here.
// KEPT in the stream-free build: reaches only picture_source.h (no stream header).
#pragma once
#include "picture_source.h"

namespace rdr_scene::live::picsrc {

inline PictureSource& TheSource() {
  static PictureSource s;
  return s;
}

}  // namespace rdr_scene::live::picsrc
