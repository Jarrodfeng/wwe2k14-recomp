// Session 27 item 1 (docs\research\audio_boot_crash_s27.md). Build (after . .\tools\env.ps1), SDK = thirdparty\rexglue-sdk-src:
//   clang-cl /O2 /Zi /MD /I<SDK>\thirdparty\sdl3\include sdl_audio_race_test.c /link /DEBUG <SDK>\out\win-amd64\Release\SDL3-static.lib
//   setupapi.lib winmm.lib imm32.lib version.lib ole32.lib oleaut32.lib uuid.lib advapi32.lib gdi32.lib user32.lib shell32.lib cfgmgr32.lib
// Stress test: SDL audio init on a worker thread while the main thread pumps events
// (what ReXGlue does at boot). Usage: race.exe [iterations] [fix]
// "fix" = the main thread initialises audio itself before pumping (the proposed SDK fix).
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static volatile LONG phase = 0;  // 1 = worker is initialising, main pumps; 2 = quit
static volatile LONG main_idle = 0;
static int iterations = 200;
static int fixmode = 0;
static volatile LONG done_iter = 0;

static LONG WINAPI on_crash(EXCEPTION_POINTERS *e) {
  fprintf(stderr, "CRASH 0x%08lX at iteration %ld\n", e->ExceptionRecord->ExceptionCode, done_iter);
  void *fr[16]; USHORT n = RtlCaptureStackBackTrace(0, 16, fr, NULL); char *base = (char *)GetModuleHandleA(NULL);
  for (USHORT k = 0; k < n; ++k) fprintf(stderr, "  #%u exe+0x%llX\n", k, (unsigned long long)((char *)fr[k] - base));
  fflush(stderr);
  ExitProcess(3);
  return EXCEPTION_EXECUTE_HANDLER;
}

static int SDLCALL worker(void *u) {
  (void)u;
  for (int i = 0; i < iterations; ++i) {
    InterlockedExchange(&phase, 1);
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      fprintf(stderr, "init failed: %s\n", SDL_GetError());
    }
    InterlockedExchange(&main_idle, 0);
    InterlockedExchange(&phase, 2);
    while (!InterlockedCompareExchange(&main_idle, 0, 0)) SwitchToThread();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    InterlockedExchange(&done_iter, i + 1);
  }
  InterlockedExchange(&phase, 3);
  return 0;
}

int main(int argc, char **argv) {
  if (argc > 1) iterations = atoi(argv[1]);
  if (argc > 2 && strcmp(argv[2], "fix") == 0) fixmode = 1;
  SetUnhandledExceptionFilter(on_crash);
  AddVectoredExceptionHandler(1, on_crash);
  if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) { fprintf(stderr, "events: %s\n", SDL_GetError()); return 1; }
  if (fixmode) {
    // The fix: the pumping thread owns the audio subsystem's first init; the worker's init only adds a reference.
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { fprintf(stderr, "audio: %s\n", SDL_GetError()); return 1; }
  }
  SDL_Thread *t = SDL_CreateThread(worker, "worker", NULL);
  for (;;) {
    LONG p = InterlockedCompareExchange(&phase, 0, 0);
    if (p == 3) break;
    if (p == 1 || p == 0) {
      SDL_PumpEvents();
    } else {
      InterlockedExchange(&main_idle, 1);
      SwitchToThread();
    }
  }
  SDL_WaitThread(t, NULL);
  if (fixmode) SDL_QuitSubSystem(SDL_INIT_AUDIO);
  printf("OK %d iterations (%s)\n", iterations, fixmode ? "fix" : "as is");
  SDL_Quit();
  return 0;
}

