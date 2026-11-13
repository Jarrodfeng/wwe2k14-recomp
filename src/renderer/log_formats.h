// The renderer log lines the case zip reads back (Session 101, lane 19 review fix 5). The renderer prints them with these formats and
// src/case/case_info.h parses them; tools/case's selftest builds its lines from the same macros, so changing one side without the other breaks the
// test instead of silently emptying the manifest's GPU / driver / display. Plain macros: no includes, no dependencies.
#pragma once
#define RDR_LOG_DEVICE_PREFIX "[rdr] device: "
#define RDR_LOG_DEVICE_FMT RDR_LOG_DEVICE_PREFIX "%s, Vulkan %u.%u.%u, driver %08X, unified memory %d, lazy memory %d, BC %d, ASTC %d"
#define RDR_LOG_SWAPCHAIN_PREFIX "[rdr] swapchain "
#define RDR_LOG_SWAPCHAIN_FMT RDR_LOG_SWAPCHAIN_PREFIX "%ux%u, %u images, quarter turns %u, present mode %d"
#define RDR_LOG_DISPLAY_TIMING_PREFIX "[rdr] display timing: "
#define RDR_LOG_DISPLAY_TIMING_FMT RDR_LOG_DISPLAY_TIMING_PREFIX "refresh %.3f ms"
#define RDR_LOG_DISPLAY_TIMING_NOW_FMT RDR_LOG_DISPLAY_TIMING_PREFIX "the refresh is now %.3f ms (was %.3f): a new swapchain"
