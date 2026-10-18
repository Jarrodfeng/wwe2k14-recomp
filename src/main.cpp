// wwe2k14 - ReXGlue Recompiled Project

#include "generated/default/wwe2k14_init.h"

#include "wwe2k14_app.h"

REX_DEFINE_APP(wwe2k14, Wwe2k14App::Create)

#ifdef _WIN32
// Session 31: ask hybrid-graphics laptops (NVIDIA Optimus, AMD switchable graphics) for the dedicated GPU. The drivers
// read these exports from the exe itself; the SDK's copies live in a DLL (rexgpu-xenos), where they don't count.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif
