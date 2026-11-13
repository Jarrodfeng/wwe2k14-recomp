#pragma once
// Lane 235 (ANDROID_PLAN F10, owner mode): feeds SDL's app-state events to the renderer's swapchain (src/renderer/surface_lifecycle.h).
// Android destroys the window's ANativeWindow in the background and makes a new one on return; the SDK fixed that for its own presenter
// (f5d75cd, Session 71) but in owner mode our swapchain is on the window. The callback only sets atomics (it runs on SDL's thread, holding
// SDL's watcher lock: no SDL or Vulkan calls here); the present thread acts on them at its next Acquire.
// Order on a phone (src/android_lifecycle.h): WILL / DID_ENTER_BACKGROUND, later DID_ENTER_FOREGROUND once the new surface is ready.
#if defined(__ANDROID__)
#include <SDL3/SDL.h>

#include <rex/logging.h>

#include "renderer/surface_lifecycle.h"

namespace wwe2k14::android_surface_events {

inline bool SDLCALL Watch(void*, SDL_Event* event) {
  switch (event->type) {
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_DID_ENTER_BACKGROUND:
      rdr::WindowEvents().OnBackground();
      break;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
      rdr::WindowEvents().OnForeground();
      break;
    default:
      break;
  }
  return true;
}

inline void Install() {
  if (SDL_AddEventWatch(Watch, nullptr)) {
    REXLOG_INFO("[life] surface events: SDL app-state watcher on (the swapchain follows the window)");
  } else {
    REXLOG_INFO("[life] surface events: SDL_AddEventWatch failed ({}); the swapchain follows the window pointer only", SDL_GetError());
  }
}

}  // namespace wwe2k14::android_surface_events
#else
namespace wwe2k14::android_surface_events {
inline void Install() {}
}  // namespace wwe2k14::android_surface_events
#endif
