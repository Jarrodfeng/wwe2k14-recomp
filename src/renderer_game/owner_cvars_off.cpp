// S103 P9 (docs\renderer\COMPILE_OUT_LIST.md B9): the three switches kept code reads that the stream's files used to DEFINE (reader.h, live.cpp), defined here when
// WWE2K14_KEEP_STREAM is OFF (CMakeLists.txt adds this file only then; with the stream kept, reader.h / live.cpp define them and this file is not built).
// A player build without the stream has exactly one renderer, so the values are fixed, not defaults to be argued with:
//   wwe2k14_rdr_owner   true on every platform (the new renderer owns the window; there is no other way to show a picture),
//   wwe2k14_rdr_scene   2 (ours; the app pins it at start, and a frame we cannot draw repeats the last picture, COMPILE_OUT_LIST.md section 4),
//   wwe2k14_scene_records the DEFINED default is false (the capture folder's scene records are a dev tool), but the app pins it TRUE in OnPreSetup when the stream is left out:
//                       the scene reads its live record only with it on (the shipped promotions turn it on the same way), so a build without the stream has it on.
#include "../keep_stream.h"

#if !WWE2K14_STREAM()
#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(wwe2k14_rdr_owner, true, "wwe2k14",
                    "S103 P9: the new renderer owns the window (the only mode of a build without the command stream)");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_scene, 2, "wwe2k14",
                     "S103 P9: the scene renderer's mode; a build without the command stream is always 2 (ours): the app pins it at start");
REXCVAR_DEFINE_BOOL(wwe2k14_scene_records, false, "wwe2k14",
                    "S103 P9: the scene writer's records beside a capture (dev; a build without the command stream has no capture)");
#endif
