#pragma once
// Session 73 (Jarrod's idea 10 of the second ten, low memory): Android's memory warnings while the game runs
// (GameActivity.onTrimMemory -> nativeTrimMemory). Each goes into the game's log and the start-up black box
// (src\blackbox.h: "memory low", read by the launcher's LastRun), and from "running low" on, the C library hands the
// memory it holds free back to Android (bionic's M_PURGE). Nothing the game uses is dropped. When Android still closes
// the game, the launcher reads why from Android itself (LauncherActivity.writeLastGameExit, LOW_MEMORY).
#if defined(__ANDROID__)
#include <jni.h>
#include <malloc.h>

#include <rex/logging.h>

#include "blackbox.h"

extern "C" JNIEXPORT void JNICALL Java_io_github_jarrodfeng_wwe2k14_GameActivity_nativeTrimMemory(JNIEnv*, jclass,
                                                                                                 jint level) {
  // ComponentCallbacks2: 5 running moderate, 10 running low, 15 running critical, 20 UI hidden, 40+ in the background.
  const char* what = level >= 40 ? "in the background" : level >= 20 ? "UI hidden"
                     : level >= 15 ? "running critical" : level >= 10 ? "running low" : "running moderate";
  const bool purge = level >= 10;
  if (purge) mallopt(M_PURGE, 0);
  REXLOG_WARN("[wwe2k14][memory] Android: memory low (trim level {}, {}){}", int(level), what,
              purge ? "; freed memory handed back" : "");
  wwe2k14::blackbox::Note("memory low (Android trim level %d, %s)%s", int(level), what,
                          purge ? "; freed memory handed back" : "");
}
#endif
