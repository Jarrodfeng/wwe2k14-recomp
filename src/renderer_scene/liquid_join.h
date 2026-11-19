// S103 SWF (review SWF-1): the join of a drawn wrestler body to its liquid map. SceneFrame::object_engine names a body by its index in frame.objects (rf_wrestlers.cpp), the same index piece_alphas uses; the
// front-end's dense `objs` list skips invisible objects, so its index is NOT that one. The lookup takes the FRAME index (ObjInfo::frame_object). Pure: tools\scene\swf_unit.cpp tests it, tools\scene\swf_lint.py
// fails any liquid_of( call that does not pass a frame_object.
#pragma once
#include <cstdint>

namespace rdr_scene {
template <class Vec>
inline uint32_t EngineObjectOf(const Vec& object_engine, uint32_t frame_index) {
  for (const auto& oe : object_engine)
    if (oe.object == frame_index) return oe.engine;
  return 0;
}
// S103 YCB: the game's model+616 mode (ObjectEngine::char_mode) of the body at FRAME index `frame_index`; -1 = the record does not carry it.
template <class Frame>
inline int CharModeOf(const Frame& frame, uint32_t frame_index) {
  int mode = -1;
  for (const auto& oe : frame.object_engine)
    if (oe.object == frame_index) mode = oe.char_mode;
  return mode;
}
// S103 WRK: the ObjectEngine row of the body at FRAME index `frame_index` (the pass mode words the census reads); null = none.
template <class Frame>
inline const typename decltype(Frame::object_engine)::value_type* ObjectEngineRowOf(const Frame& frame, uint32_t frame_index) {
  const typename decltype(Frame::object_engine)::value_type* row = nullptr;
  for (const auto& oe : frame.object_engine)
    if (oe.object == frame_index) row = &oe;
  return row;
}
}  // namespace rdr_scene
