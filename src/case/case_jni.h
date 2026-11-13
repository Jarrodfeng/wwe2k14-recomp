// The phone's bug button, native side (Session 101, lane 5): CaseButton.java (the corner button) calls this on a tap. It only raises a flag;
// the guest-frame path takes the case (case_hook.h RequestTap / bookmark.h). Include once, in a game-exe file that is built for Android.
#pragma once
#if defined(__ANDROID__)
#include <jni.h>

#include "case_hook.h"

extern "C" JNIEXPORT void JNICALL Java_io_github_jarrodfeng_wwe2k14_CaseButton_nativeCaseTap(JNIEnv*, jclass) { case_hook::RequestTap(); }
#endif
