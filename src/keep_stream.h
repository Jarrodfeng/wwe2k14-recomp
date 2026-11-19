// S103 P9 (docs\renderer\LANDING2_PLAN.md section 5, docs\renderer\COMPILE_OUT_LIST.md): WWE2K14_KEEP_STREAM, the one switch that keeps or leaves out the command
// stream's reader / binder / replayer (the 360 draw stream the old renderer path walks). CMakeLists.txt passes an explicit 1 or 0 to the game exe (option
// WWE2K14_KEEP_STREAM). A target that does not pass it (fview, rplay and the unit rigs are dev tools that always have the stream) gets 1 from here.
// Every file tests it as `#if WWE2K14_STREAM()` (or `!WWE2K14_STREAM()`): a function-like macro defined ONLY here, so a file that forgets to include this header gets a
// hard preprocessor error, never an undefined name read as 0 (tools\scene\keep_stream_lint.py also fails a raw `#if WWE2K14_KEEP_STREAM`).
#pragma once

#ifndef WWE2K14_KEEP_STREAM
#define WWE2K14_KEEP_STREAM 1
#endif
#define WWE2K14_STREAM() (WWE2K14_KEEP_STREAM)
