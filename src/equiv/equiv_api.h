/* wwe2k14 - the equivalence tester's C interface, version 1 (S111 EQUIV-A; docs\research\s111\EQUIV0.md "Build plan").
 *
 * The exe side (src\equiv\equiv_host.h) runs headless (--wwe2k14_equiv=<test>): guest memory and the loaded xex, no window,
 * no GPU, no game thread. It loads a test DLL and hands it this host table; the DLL calls the game's recompiled functions
 * through `call` and compares them with our ports. C only: the DLL is built on its own (piece B, equiv\equiv_tests.cpp).
 *
 * Host table (every function runs on the tester's one guest thread, the thread that called wwe2k14_equiv_run):
 *   membase        guest address A is at membase + A (big-endian bytes, as the game sees them)
 *   ctx            the thread's PPCContext (rex/ppc/context.h); get_gpr / set_gpr / get_fpr / set_fpr read and write it
 *   call(addr)     runs the guest function at addr with the registers as set: 1 = it returned (its r3 / f1 / the memory
 *                  it wrote are the results; read them with get_gpr / get_fpr), 0 = a guest fault (SEH) inside it,
 *                  -1 = addr is not a registered (allowlisted) function, or (coverage tier) the call entered one outside
 *                  the allowlist. lr is 0 during the call; r1 (the stack) and r13 (the PCR) are put back to the thread's
 *                  own values after it; every other register keeps what the callee left until the test sets it.
 *   scratch(n, a)  a guest address of n zeroed bytes aligned to a (a power of two); 0 = the arena is full
 *   scratch_reset  frees (and zeroes) every scratch block
 *   log(text)      one log line "[wwe2k14][equiv][dll] text"
 *   stub(addr, fn) coverage tier only: every entry of guest function addr runs fn instead (fn returns 1 = the body is
 *                  skipped, its results are the registers fn set; 0 = the game's body runs after fn); fn = NULL removes
 *                  the stub. Returns 1 when set, 0 outside the coverage build (the prologue seam exists only there).
 *
 * DLL exports:
 *   int wwe2k14_equiv_init(const Wwe2k14EquivHost* host)   returns WWE2K14_EQUIV_API_VERSION
 *   int wwe2k14_equiv_run(const char* test, uint64_t seed, int cases, char* report, int cap)
 *       0 = every case equal, 1 = a difference (report = the first differing inputs and both outputs), 2 = unknown test,
 *       3 = NOT-ISOLABLE (a stub refused, a guest fault, the call left the allowlist): a defined result, the exe's exit code
 */
#ifndef WWE2K14_EQUIV_API_H
#define WWE2K14_EQUIV_API_H

#include <stdint.h>

#define WWE2K14_EQUIV_API_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Wwe2k14EquivHost {
  uint32_t version;
  uint8_t* membase;
  void* ctx;
  int (*call)(uint32_t addr);
  uint64_t (*get_gpr)(int n);
  void (*set_gpr)(int n, uint64_t v);
  double (*get_fpr)(int n);
  void (*set_fpr)(int n, double v);
  uint32_t (*scratch)(uint32_t bytes, uint32_t align);
  void (*scratch_reset)(void);
  void (*log)(const char*);
  int (*stub)(uint32_t addr, int (*fn)(const struct Wwe2k14EquivHost*));
} Wwe2k14EquivHost;

typedef int (*Wwe2k14EquivInitFn)(const Wwe2k14EquivHost* host);
typedef int (*Wwe2k14EquivRunFn)(const char* test, uint64_t seed, int cases, char* report, int cap);
typedef int (*Wwe2k14EquivStubFn)(const Wwe2k14EquivHost* host);

#ifdef __cplusplus
}
#endif

#endif /* WWE2K14_EQUIV_API_H */
