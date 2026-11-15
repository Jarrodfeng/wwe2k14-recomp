// S102 lane G2b unit test: exclusive_policy.h (the decisions of wwe2k14_rdr_exclusive_fullscreen). Built by tools\g2b\g2b_check.py against the real header and
// against planted twins that must FAIL it. No Vulkan, no window.
#include <cstdio>
#include <cstring>
#include "exclusive_policy.h"

using namespace rdr::excl;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
  char line[200];
  {  // switch off: nothing exclusive, ever, whatever the driver offers and whatever results come back
    Policy p;
    p.Configure(false, true);
    CHECK(p.state() == State::Off && !p.ChainStructs() && !p.ShouldRelease());
    CHECK(!p.NoteAcquire(0) && !p.NoteResult(kResultExclusiveLost));
    CHECK(p.state() == State::Off && !p.ChainStructs());
    Policy::Line(line, sizeof(line), p.state(), 2736, 1824, 60);
    CHECK(std::strcmp(line, "[gfx][present] exclusive off mode 2736x1824@60") == 0);
  }
  {  // the extension is missing: "unsupported", no structs, normal run
    Policy p;
    p.Configure(true, false);
    CHECK(p.state() == State::Unsupported && !p.ChainStructs() && !p.ShouldRelease());
    CHECK(!p.NoteAcquire(0) && !p.NoteResult(kResultExclusiveLost) && p.state() == State::Unsupported);
    Policy::Line(line, sizeof(line), p.state(), 1920, 1080, 144);
    CHECK(std::strcmp(line, "[gfx][present] exclusive unsupported mode 1920x1080@144") == 0);
  }
  {  // asked, supported, acquired; then lost at a present (alt-tab): the fallback is taken once
    Policy p;
    p.Configure(true, true);
    CHECK(p.wanted() && p.ChainStructs() && p.state() == State::Off);  // before the first acquire nothing is claimed
    CHECK(p.NoteAcquire(kResultSuccess) && p.state() == State::On && p.ShouldRelease());
    CHECK(!p.NoteResult(kResultSuccess) && !p.NoteResult(kResultSuboptimal) && !p.NoteResult(-1000001004 /* OUT_OF_DATE */) && p.state() == State::On);
    CHECK(p.NoteResult(kResultExclusiveLost));  // the transition: the caller logs one line and rebuilds
    CHECK(p.state() == State::Lost && !p.ChainStructs() && !p.ShouldRelease());  // the rebuild builds NO exclusive structs
    CHECK(!p.NoteResult(kResultExclusiveLost));  // later lost results: no second line
    Policy::Line(line, sizeof(line), State::Lost, 2736, 1824, 0);
    CHECK(std::strcmp(line, "[gfx][present] exclusive lost mode 2736x1824@0") == 0);
  }
  {  // the acquire itself is refused (another full-screen app, the window not in front): lost, normal path, no structs on a rebuild
    Policy p;
    p.Configure(true, true);
    CHECK(p.NoteAcquire(-3 /* INITIALIZATION_FAILED */) && p.state() == State::Lost && !p.ChainStructs() && !p.ShouldRelease());
  }
  {  // lost at Acquire time (the same event, the other call)
    Policy p;
    p.Configure(true, true);
    p.NoteAcquire(kResultSuccess);
    CHECK(p.NoteResult(kResultExclusiveLost) && !p.ChainStructs());
  }
  {  // F1: the driver refused the exclusive swapchain: lost, no structs on the retry, one transition
    Policy p;
    p.Configure(true, true);
    CHECK(p.ChainStructs());
    CHECK(p.NoteCreateFailed() && p.state() == State::Lost && !p.ChainStructs());
    CHECK(!p.NoteCreateFailed());
    Policy off;
    off.Configure(false, true);
    CHECK(!off.NoteCreateFailed() && off.state() == State::Off);
  }
  {  // F2: a refused acquire is lost (the caller rebuilds); a LOST result after that still rebuilds but prints no second line
    Policy p;
    p.Configure(true, true);
    CHECK(p.NoteAcquire(-3) && p.state() == State::Lost);  // the caller sets stale_ from state() == Lost
    CHECK(p.NeedsRebuild(kResultExclusiveLost));            // a repeat LOST: rebuild again
    CHECK(!p.NoteResult(kResultExclusiveLost));             // ... with no second line
    CHECK(!p.NeedsRebuild(kResultSuccess) && !p.NeedsRebuild(kResultSuboptimal));
    Policy q;
    q.Configure(true, true);
    q.NoteAcquire(kResultSuccess);
    CHECK(q.NeedsRebuild(kResultExclusiveLost) && q.NoteResult(kResultExclusiveLost));  // first: rebuild and one line
    CHECK(q.NeedsRebuild(kResultExclusiveLost) && !q.NoteResult(kResultExclusiveLost)); // again: rebuild, no line
    Policy off;
    off.Configure(false, true);
    CHECK(!off.NeedsRebuild(kResultExclusiveLost));
  }
  {  // F3: the surface says no
    Policy p;
    p.Configure(true, true);
    p.NoteSurfaceUnsupported();
    CHECK(p.state() == State::Unsupported && !p.ChainStructs() && !p.NoteAcquire(0));
    Policy off;
    off.Configure(false, true);
    off.NoteSurfaceUnsupported();
    CHECK(off.state() == State::Off);
  }
  {  // F4 (corrected): a rebuild while held releases first and acquires again on the new swapchain; Destroy releases only a held swapchain
    Policy p;
    p.Configure(true, true);
    CHECK(p.AcquireAfterCreate(true) && !p.AcquireAfterCreate(false));
    p.NoteAcquire(kResultSuccess);
    CHECK(p.state() == State::On && p.ShouldRelease());
    p.NoteReleased();                                      // Build: ReleaseExclusive(old) before vkCreateSwapchainKHR
    CHECK(p.state() == State::On && !p.ShouldRelease());   // not held now (and no line: the state did not change)
    CHECK(p.ChainStructs() && p.AcquireAfterCreate(true)); // the rebuild chains the structs and acquires again, in state On
    CHECK(!p.NoteAcquire(kResultSuccess) && p.ShouldRelease());  // held again, still one "on"
    p.NoteReleased();                                      // a rebuild whose create then fails: nothing held, Destroy must not release
    CHECK(p.NoteCreateFailed() && !p.ShouldRelease());
    Policy q;
    q.Configure(true, true);
    q.NoteAcquire(kResultSuccess);
    q.NoteResult(kResultExclusiveLost);
    CHECK(!q.ShouldRelease());
  }
  if (fails) return 1;
  std::printf("exclusive_test: ok\n");
  return 0;
}
