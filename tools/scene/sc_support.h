// S103 SC: what a layout-4 row must carry to be a valid row (scissor state read, target class + viewport read). The units' hand-built rows call ValidSC; the tap's own rows get these from FillScissor / FillTarget.
#pragma once
#include "../../src/renderer_scene/scene_record.h"
namespace rdr_scene {
inline void ValidSC(Quad2D& q) {
  q.have |= kQhScissor | kQhTarget;
  q.scissor_on = 0;
  q.pass = kQpFinal;
  q.target_size[0] = 1280, q.target_size[1] = 720;
  q.viewport[0] = 0, q.viewport[1] = 0, q.viewport[2] = 1280, q.viewport[3] = 720;
}
}  // namespace rdr_scene
