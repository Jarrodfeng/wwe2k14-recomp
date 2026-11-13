// Lane 235: the window-surface lifecycle on planted sequences. Host test, no Vulkan:
//   clang++ -std=c++17 -I src/renderer src/renderer/surface_lifecycle_test.cpp -o sl_test && ./sl_test
// Plant (must FAIL): add -DWWE2K14_LIFE_PLANT_NO_RECREATE (the old surface kept after a new window / a loss).
#include <cstdio>
#include <string>

#include "surface_lifecycle.h"

using rdr::SurfaceEvents;
using rdr::SurfaceLifecycle;
using A = rdr::SurfaceLifecycle::Action;

// A fake swapchain owner: applies the lifecycle's actions to a fake surface bound to a fake window, as Swapchain::Acquire does.
struct Fake {
  SurfaceEvents events;
  SurfaceLifecycle life{&events};
  void* window = nullptr;          // what SDL says right now
  void* surface_window = nullptr;  // the window the live surface was made from
  bool surface_alive = false;      // false: Android destroyed the window under the surface (the surface is lost)
  bool window_alive_for_create = true;
  int creates = 0, releases = 0, presented = 0, skipped = 0;
  bool create_fails = false;

  // One frame. Returns true when a picture was presented.
  bool Frame() {
    switch (life.Decide(window)) {
      case A::kSkip: ++skipped; return false;
      case A::kRelease: surface_window = nullptr; life.Released(); ++releases; return false;
      case A::kCreate: {
        ++creates;
        const bool ok = !create_fails;
        surface_window = ok ? window : nullptr;
        surface_alive = ok;
        life.Created(window, ok);
        if (!ok) return false;
        break;
      }
      case A::kPresent: break;
    }
    if (!surface_alive) { life.Lost(); return false; }  // acquire says VK_ERROR_SURFACE_LOST_KHR
    ++presented;
    return true;
  }
  void Start(void* w, bool android = false) { life.SetWaitForEventOnLoss(android); window = w; creates = 1; surface_window = w; surface_alive = true; life.Created(w, true); }
};

static int fails = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++fails;
}

int main() {
  int a = 0, b = 0, c = 0;
  void *WA = &a, *WB = &b, *WC = &c;

  {  // steady state: no events, nothing re-created
    Fake f; f.Start(WA);
    bool all = true;
    for (int i = 0; i < 100; ++i) all = all && f.Frame();
    Check(all && f.creates == 1 && f.releases == 0, "steady: 100 frames presented, no re-create");
  }
  {  // background -> foreground with a NEW window -> present
    Fake f; f.Start(WA);
    f.Frame();
    f.events.OnBackground();
    f.surface_alive = false; f.window = nullptr;  // Android destroys the window
    Check(!f.Frame(), "background: no present");
    Check(f.releases == 0 && f.surface_window == WA, "background: no Vulkan object touched (surface kept, not released)");
    for (int i = 0; i < 5; ++i) f.Frame();
    Check(f.presented == 1, "background: nothing presented while away");
    f.window = WB; f.events.OnForeground();
    Check(f.Frame(), "foreground (new window): presents on the first frame");
    Check(f.surface_window == WB && f.creates == 2, "foreground: surface made from the NEW window pointer");
    Check(f.Frame() && f.creates == 2, "after: steady, no more creates");
  }
  {  // background, foreground comes back with the SAME address (reused)
    Fake f; f.Start(WA);
    f.events.OnBackground(); f.surface_alive = false;
    f.Frame();
    f.events.OnForeground();
    Check(f.Frame() && f.creates == 2, "foreground, same pointer value: still re-created and presents");
  }
  {  // surface lost without any event, window pointer unchanged
    Fake f; f.Start(WA);
    f.Frame();
    f.surface_alive = false;
    Check(!f.Frame(), "lost: that frame is dropped");
    bool shown = f.Frame();
    Check(shown && f.creates == 2, "lost: the next frame re-creates and presents (no stall)");
  }
  {  // Android (review SF1): a loss on the same pointer with no event must NOT create (the window may be freed); the event does
    Fake f; f.Start(WA, true);
    f.Frame();
    f.surface_alive = false;
    f.Frame(); f.Frame(); f.Frame();
    Check(f.creates == 1 && f.releases == 0, "android: loss on the same pointer, no event: no create, surface kept");
    f.events.OnBackground(); f.window = nullptr; f.Frame();
    f.window = WB; f.events.OnForeground();
    Check(f.Frame() && f.surface_window == WB, "android: then the events re-create on the new window");
  }
  {  // plant target: background must not release
    Fake f; f.Start(WA);
    f.events.OnBackground();
    for (int i = 0; i < 10; ++i) f.Frame();
    Check(f.releases == 0 && f.life.has_surface(), "background for 10 frames: still no release");
  }
  {  // Android: a loss with a DIFFERENT pointer re-creates at once
    Fake f; f.Start(WA, true);
    f.surface_alive = false; f.window = WB;
    f.Frame();
    Check(f.Frame() && f.surface_window == WB, "android: loss + new pointer: re-creates");
  }
  {  // surface lost and the window pointer changed (SDL swapped it, no events seen)
    Fake f; f.Start(WA);
    f.surface_alive = false; f.window = WB;
    f.Frame();
    Check(f.Frame() && f.surface_window == WB, "new pointer without events: follows it");
  }
  {  // double events
    Fake f; f.Start(WA);
    f.events.OnBackground(); f.events.OnBackground(); f.events.OnBackground();
    f.surface_alive = false; f.window = nullptr;
    f.Frame(); f.Frame();
    Check(f.releases == 0, "double background: nothing released");
    f.window = WB;
    f.events.OnForeground(); f.events.OnForeground();
    Check(f.Frame() && f.creates == 2, "double foreground: one re-create, presents");
    Check(f.Frame() && f.creates == 2, "double foreground: no second re-create");
  }
  {  // foreground without a background
    Fake f; f.Start(WA);
    f.Frame();
    f.events.OnForeground();
    Check(f.Frame() && f.creates == 2, "foreground alone: one harmless re-create, presents");
    Check(f.Frame() && f.creates == 2, "foreground alone: then steady");
  }
  {  // background arrives while a create is pending: never create in the background
    Fake f; f.Start(WA);
    f.surface_alive = false; f.Frame();  // lost
    f.events.OnBackground();
    f.Frame();
    Check(f.creates == 1, "background beats a pending re-create");
    f.window = WB; f.events.OnForeground();
    Check(f.Frame() && f.surface_window == WB, "then foreground re-creates on the new window");
  }
  {  // create fails (window not ready): retried later, not every frame, then recovers
    Fake f; f.Start(WA);
    f.events.OnBackground(); f.surface_alive = false; f.window = nullptr; f.Frame();
    f.window = WC; f.events.OnForeground(); f.create_fails = true;
    f.Frame();
    int tries = f.creates;
    for (int i = 0; i < 20; ++i) f.Frame();
    Check(f.creates == tries, "failed create: not retried every frame");
    f.create_fails = false;
    bool shown = false;
    for (int i = 0; i < SurfaceLifecycle::kRetryFrames + 2 && !shown; ++i) shown = f.Frame();
    Check(shown && f.surface_window == WC, "failed create: recovers after the wait");
  }
  {  // null window while in front (not yet there): skip, no create from null
    Fake f; f.Start(WA);
    f.window = nullptr; f.surface_alive = false;
    Check(!f.Frame() && f.creates == 1, "no window: no create from null");
    f.window = WB;
    Check(f.Frame() || f.Frame(), "window appears: presents");
  }
  std::printf("%s\n", fails ? "FAILED" : "all passed");
  return fails ? 1 : 0;
}
