// wwe2k14 - probe plugin API (Session 30, #4). Our code; C ABI, so a plugin needs nothing but this header.
//
// A probe plugin is a DLL (probes/probes.cpp builds wwe2k14_probes.dll) that the COVERAGE build loads at run time
// (script / live: `plugin load [PATH]`, `plugin reload`, `plugin unload`; src/probe_plugin.h). It names the guest
// functions it wants; each call of those functions then runs the plugin's enter() before the function body and leave()
// after it returns, on the guest thread that made the call. Rebuild the DLL while the game runs
// (pwsh -File tools\probes\build_probes.ps1) and `plugin reload`: the game loads a copy, so the file stays writable.
//
// Rules for plugin code:
//   - Read guest memory only through the host's read functions (checked: an unmapped address returns 0, no fault).
//   - enter() / leave() can run on several guest threads at once: keep state per thread or behind a lock.
//   - Keep them short (they run inside the game's call); log through host->log (rate-limit yourself).
//   - A function the plugin takes is not traced by --wwe2k14_trace at the same time.
//   - A fault inside enter() / leave() unloads nothing but disarms the plugin (logged); fix it and reload.
#pragma once

#include <stdint.h>

#define WWE2K14_PROBE_API_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Wwe2k14ProbeHost {
  uint32_t version;  // WWE2K14_PROBE_API_VERSION of the game
  // Guest memory (big-endian values converted to host order). 1 = ok, 0 = unreadable / unmapped.
  int (*read8)(uint32_t guest, uint8_t* out);
  int (*read16)(uint32_t guest, uint16_t* out);
  int (*read32)(uint32_t guest, uint32_t* out);
  int (*read_bytes)(uint32_t guest, void* dst, uint32_t n);  // raw bytes, every word checked first
  int (*read_string)(uint32_t guest, char* dst, uint32_t cap);  // ASCII up to NUL or cap-1; non-printable -> '?'
  // Registers of the call (ctx = the pointer enter() / leave() got). n = 0..31.
  uint64_t (*get_gpr)(void* ctx, int n);
  void (*set_gpr)(void* ctx, int n, uint64_t value);  // e.g. change an argument in enter(), a result in leave()
  double (*get_fpr)(void* ctx, int n);
  void (*set_fpr)(void* ctx, int n, double value);
  uint32_t (*get_lr)(void* ctx);  // the caller's return address
  // Misc.
  void (*log)(const char* text);  // one run-log line: "[wwe2k14][probe] text"
  uint64_t (*frame)(void);        // the guest frame (VdSwap count)
  uint32_t (*thread_id)(void);    // the host thread id
} Wwe2k14ProbeHost;

// Exports a plugin provides (extern "C" __declspec(dllexport)):
//   int  wwe2k14_probe_init(const Wwe2k14ProbeHost* host);   return WWE2K14_PROBE_API_VERSION (else not loaded)
//   int  wwe2k14_probe_functions(uint32_t* out, int cap);    the guest addresses it wants; returns how many (<= cap)
//   void wwe2k14_probe_enter(void* ctx, uint32_t addr);      before the body (registers = the arguments)
//   void wwe2k14_probe_leave(void* ctx, uint32_t addr);      optional: after the return (r3 / f1 = the result)
//   void wwe2k14_probe_shutdown(void);                       optional: before the DLL is unloaded

#ifdef __cplusplus
}
#endif
