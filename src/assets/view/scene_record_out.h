// Session 101, lane 14: the viewer's `record` command: writes the scene it shows as a scene record file (src\renderer_scene\record_io.h,
// docs\renderer\SCENE_RECORD.md). Reads the renderer's models (public) and the camera; any backend (the record holds no draws).
#pragma once
#include <string>

#include "camera.h"
#include "renderer.h"

namespace view {

// Writes `path` (a .rsc): the camera for a w x h picture, every model the viewer knows (arena / wrestler / prop by pac + entry + attire,
// its place as a world matrix, a skin palette when posed), the lights left out (v1: the file's own sets are used). Returns the command's
// JSON reply ({"ok":true,...} or {"ok":false,"error":...}).
std::string RecordScene(const Renderer& r, const Camera& cam, int w, int h, const std::string& path, long long frame);

}  // namespace view
