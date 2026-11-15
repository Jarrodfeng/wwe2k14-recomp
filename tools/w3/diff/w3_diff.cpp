// S102 lane W3: the offline differential test for the native render-command replay (src\nf\replay_native.h).
//
// It runs the RECOMPILED sub_82BB5BE8 / sub_82BB53D0 (extracted from the local generated code by extract_ref.py, never committed) and our native
// versions on identical guest memory and compares: every byte of guest memory outside the dead stack, the non-volatile registers (r1, r2, r13-r31,
// f14-f31, lr), and the sequence of calls the code makes (handler, r3, r4, the record's words, the slot, the position word) as recorded by stubs. The stubs
// stand in for the ~95 handlers: each records the call, writes a hash of it into guest memory, scribbles the volatile registers, and (per state) may
// do what real handlers could: move the slot, change the position word, flip the buffer index, rewrite the next record.
//
//   w3_diff --states N [--seed S] [--bench]       exit 0 = all equal, 1 = a mismatch (the first ones are printed), 2 = bad usage
//
// Configurations per state, each compared with the reference R (recompiled loop; the hook stand-in with the opcode-81 check; recompiled interpreter):
//   C1 the native loop, fast path, native dispatcher (what the game runs with both switches on)
//   C2 the native loop, slow path (every record through the hook stand-in), recompiled interpreter
//   C3 the recompiled loop, native dispatcher behind the hook stand-in
// Planted twins: compiled with -DW3_PLANT=n (src\nf\replay_native.h W3_FAULT): the run must then FAIL.

#include "wwe2k14_pch.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <vector>

// The recompiled code calls indirect targets through a function map; the test records the call instead.
#undef REX_CALL_INDIRECT_FUNC
#define REX_CALL_INDIRECT_FUNC(x) w3_indirect(ctx, base, (uint32_t)(x))
static void w3_indirect(PPCContext& ctx, uint8_t* base, uint32_t target);

// The replay's bookkeeping (src/replay_guard.h: the ring, the guard, the opcode-81 call check, the per-record hook body) is compiled as the game compiles it;
// the test supplies the guard's test frame, a frame number, a silent log and its own FakeRecords.
inline int32_t g_w3_test_frame = 0;
inline uint64_t g_w3_frame = 0;
#define W3_GUARD_TEST_FRAME() g_w3_test_frame
#define W3_GUARD_FRAME() g_w3_frame
#undef REXLOG_WARN
#define REXLOG_WARN(...) ((void)0)
#include "../../../src/replay_guard.h"
#include "../../../src/nf/replay_native.h"
namespace wwe2k14::replay_watch {
inline uint32_t FakeRecords(uint8_t* base) {  // the test's: two records (104, then 105) in a page the test maps
  constexpr uint32_t at = 0x10300E00;
  for (uint32_t i = 0; i < 32; i += 4) Put32(base, at + i, 0);
  Put32(base, at, 104);
  Put32(base, at + 16, 105);
  return at;
}
}  // namespace wwe2k14::replay_watch

#define X(n, a) extern "C" void n(PPCContext& __restrict ctx, uint8_t* base);
#include "ref_callees.inc"
#undef X
extern "C" void __imp____savegprlr_29(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp____restgprlr_29(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __savegprlr_29(PPCContext& __restrict ctx, uint8_t* base) { __imp____savegprlr_29(ctx, base); }
extern "C" void __restgprlr_29(PPCContext& __restrict ctx, uint8_t* base) { __imp____restgprlr_29(ctx, base); }
extern "C" void __imp__sub_82BB5BE8(PPCContext& __restrict ctx, uint8_t* base);

#include "ref_funcs.inc"

namespace {

using wwe2k14::replay_check::Be32;

// ---- the guest memory layout of the test ----
constexpr uint32_t kMgr = 0x10000000, kDesc = 0x10001000, kBuf0 = 0x10100000, kBuf1 = 0x10200000, kSub = 0x10300000;
constexpr uint32_t kRecords = 4096;  // per buffer
constexpr uint32_t kGlob = 0x83929000;
constexpr uint32_t kTable = 0x82BB5000;  // the interpreter's jump table sits in the image here (its words are read, then ignored: the switch is in C++)
constexpr uint32_t kStackLo = 0x0F0C0000, kStackSize = 0x40000;
struct Region {
  uint32_t addr, size;
};
constexpr Region kRegions[] = {{kMgr, 0x1000}, {kDesc, 0x1000}, {kBuf0, kRecords * 16}, {kBuf1, kRecords * 16}, {kSub, 0x1000}, {kGlob, 0x1000}, {kTable, 0x1000}};
constexpr uint32_t kStackWindowLo = 0x2000, kStackWindowHi = 0x100;  // the stack bytes copied and compared: [r1 - lo, r1 + hi)
constexpr uint32_t kDeadBelow = 0x1000;                              // [r1 - 0x1000, r1) is dead after the call: not compared

bool Mapped(uint32_t a) {  // a..a+16 lies inside a committed region (the stubs only read memory that is there)
  for (const Region& r : kRegions)
    if (a >= r.addr && uint64_t(a) + 16 <= uint64_t(r.addr) + r.size) return true;
  return false;
}

struct Arena {
  uint8_t* base = nullptr;
  void Init() {
    void* p = VirtualAlloc(nullptr, 0x100000000ull + 0x10000, MEM_RESERVE, PAGE_READWRITE);
    if (!p) {
      std::fprintf(stderr, "w3_diff: cannot reserve 4 GB of address space\n");
      std::exit(2);
    }
    base = static_cast<uint8_t*>(p);
    for (const Region& r : kRegions)
      if (!VirtualAlloc(base + r.addr, r.size, MEM_COMMIT, PAGE_READWRITE)) std::exit(2);
    if (!VirtualAlloc(base + kStackLo, kStackSize, MEM_COMMIT, PAGE_READWRITE)) std::exit(2);
  }
  uint32_t Ld(uint32_t a) const { return Be32(base, a); }
  void St(uint32_t a, uint32_t v) {
    uint8_t* p = base + a;
    p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
  }
};

struct Snapshot {
  std::vector<uint8_t> data[sizeof(kRegions) / sizeof(kRegions[0])];
  std::vector<uint8_t> stack;
  uint32_t r1 = 0;
  void Take(const Arena& a, uint32_t sp) {
    r1 = sp;
    for (size_t i = 0; i < sizeof(kRegions) / sizeof(kRegions[0]); ++i) data[i].assign(a.base + kRegions[i].addr, a.base + kRegions[i].addr + kRegions[i].size);
    stack.assign(a.base + sp - kStackWindowLo, a.base + sp + kStackWindowHi);
  }
  void Restore(Arena& a) const {
    for (size_t i = 0; i < sizeof(kRegions) / sizeof(kRegions[0]); ++i) std::memcpy(a.base + kRegions[i].addr, data[i].data(), kRegions[i].size);
    std::memcpy(a.base + r1 - kStackWindowLo, stack.data(), stack.size());
  }
};

// ---- the recording stubs ----
struct Trace {
  uint32_t id, r3, r4, w[4], slotv, pos, lr, chain, saved_lr;
  bool operator==(const Trace& o) const { return std::memcmp(this, &o, sizeof(Trace)) == 0; }
};
std::vector<Trace> g_trace;
bool g_tracing = true;
uint32_t g_mut = 0;
uint64_t g_calls = 0;
uint8_t* g_base = nullptr;
bool g_native_dispatch = false, g_call_check = true, g_guard = false;
uint64_t g_skipped = 0;
uint32_t HandlerId(const char* name) { return uint32_t(std::strtoul(name + 4, nullptr, 16)); }  // sub_XXXXXXXX

uint64_t Mix(uint64_t x) {
  x ^= x >> 33, x *= 0xff51afd7ed558ccdull, x ^= x >> 33, x *= 0xc4ceb9fe1a85ec53ull, x ^= x >> 33;
  return x;
}

void Stub(uint32_t id, PPCContext& ctx, uint8_t* base) {
  Arena a{base};
  const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32;
  if (++g_calls > 30000) RaiseException(0xE0003001, 0, 0, nullptr);  // a replay that never ends: counted as a fault of whoever ran it (a planted twin may loop for ever)
  // the call / return pair of op 81 / 82 (the recompiled interpreter runs them inline): a return position stack in the manager's page, as the
  // game's pair keeps one. Without it an op 82 would re-read itself for ever.
  if (id == 0x82C244E0 || id == 0x82C244F0) {
    const uint32_t depth = a.Ld(kMgr + 0x3F0);
    if (id == 0x82C244E0 && depth < 16) {
      a.St(kMgr + 0x400 + 4 * depth, a.Ld(kMgr + 44));
      a.St(kMgr + 0x3F0, depth + 1);
      const uint32_t target = Mapped(r3) ? a.Ld(r3 + 16) : 0, buf = a.Ld(kMgr + 48 + 4 * a.Ld(kMgr + 36));
      if (target && !(target & 15)) a.St(kMgr + 44, (target - buf) >> 4);
    } else if (id == 0x82C244F0) {
      if (depth) a.St(kMgr + 44, a.Ld(kMgr + 0x400 + 4 * (depth - 1))), a.St(kMgr + 0x3F0, depth - 1);
      else a.St(kMgr + 44, a.Ld(kMgr + 44) + (Mapped(r3) ? a.Ld(r3 + 8) : 0) + 1);  // a return with nothing to return to: on to the next record
    }
  }
  if (g_tracing) {
    Trace t{id, r3, r4, {0, 0, 0, 0}, 0, a.Ld(kMgr + 44), uint32_t(ctx.lr), 0, 0};
    if (Mapped(r3))
      for (int i = 0; i < 4; ++i) t.w[i] = a.Ld(r3 + 4 * i);
    if (Mapped(r4)) t.slotv = a.Ld(r4);
    g_trace.push_back(t);
    // the hash of the call in guest memory: a different order or count shows in the memory compare too
    a.St(kMgr + 0x800 + uint32_t((g_calls * 4) & 0x3FC), uint32_t(Mix(id ^ (uint64_t(r3) << 20) ^ (uint64_t(r4) << 40) ^ g_calls)));
    // what real handlers could do
    if ((g_mut & 1) && id == 0x82BB4540 && Mapped(r4)) {  // move the slot on past the next record
      const uint32_t cur = a.Ld(r4);
      if (Mapped(cur + 16 * 8)) a.St(r4, cur + 16 * (a.Ld(cur + 8) + 1));
    }
    if ((g_mut & 2) && id == 0x82BB4130) a.St(kMgr + 44, a.Ld(kMgr + 44) + 1);  // change the position word
    if ((g_mut & 4) && id == 0x82C1A2A8) a.St(kMgr + 36, a.Ld(kMgr + 36) ^ 1);  // flip the buffer index
    if ((g_mut & 8) && id == 0x82BB41E8 && Mapped(r3 + 16 * 8)) {                // rewrite the record after this one
      const uint32_t next = r3 + 16 * (a.Ld(r3 + 8) + 1);
      if (Mapped(next)) a.St(next, 105);
    }
  }
  // volatile registers: what a callee may leave
  const uint64_t h = Mix(id ^ (uint64_t(r3) << 16) ^ (g_calls << 40));
  ctx.r3.u64 = h;
  ctx.r4.u64 = ~h;
  ctx.r5.u64 = h * 3, ctx.r6.u64 = h * 5, ctx.r7.u64 = h * 7, ctx.r8.u64 = h * 9, ctx.r9.u64 = h * 11, ctx.r10.u64 = h * 13, ctx.r11.u64 = h * 15, ctx.r12.u64 = h * 17;
  ctx.r0.u64 = h * 19;
}

}  // namespace

#define X(n, a) \
  extern "C" void n(PPCContext& __restrict ctx, uint8_t* base) { Stub(a, ctx, base); }
#include "ref_callees.inc"
#undef X

static void w3_indirect(PPCContext& ctx, uint8_t* base, uint32_t target) {
  Stub(0xFFFF0000u ^ target, ctx, base);
}

// The hook stand-in: sub_82BB53D0 as replay_watch.h defines it (the same HookedDispatch; the cost span is off on the PC). For an op 81 record it also
// records what a stack walker sees at the hook's entry (lr, the back chain at r1, the caller's lr saved below it).
extern "C" void sub_82BB53D0(PPCContext& __restrict ctx, uint8_t* base) {
  namespace rw = wwe2k14::replay_watch;
  rw::HookedDispatch(ctx, base, g_call_check, g_guard, g_guard, [&] {
    if (g_tracing && Be32(base, Be32(base, ctx.r4.u32)) == 81) {
      Arena a{base};
      const uint32_t chain = a.Ld(ctx.r1.u32);
      Trace t{0x82BB53D0, ctx.r3.u32, ctx.r4.u32, {0, 0, 0, 0}, 0, a.Ld(kMgr + 44), uint32_t(ctx.lr), chain, (chain >= kStackLo + 16 && chain < kStackLo + kStackSize) ? a.Ld(chain - 8) : 0};
      g_trace.push_back(t);
    }
    if (g_native_dispatch)
      wwe2k14::nf_replay::Dispatch(ctx, base);
    else
      __imp__sub_82BB53D0(ctx, base);
  });
}

namespace {

// ---- the state generator ----
struct Rng {
  std::mt19937_64 g;
  explicit Rng(uint64_t s) : g(s) {}
  uint32_t U(uint32_t n) { return uint32_t(g() % n); }
  bool Chance(uint32_t pct) { return U(100) < pct; }
};

uint32_t PickOp(Rng& r) {
  const uint32_t k = r.U(100);
  if (k < 55) {  // a Call op (weighted to the real hot ones)
    static const uint32_t hot[] = {35, 35, 35, 21, 21, 3, 3, 15, 15, 19, 22, 23, 24, 28, 36, 37, 38, 64, 65, 68, 83, 90, 97, 1, 4, 5, 6, 7, 8, 11, 14, 18, 20, 29, 30, 41, 44, 45, 47, 48, 50, 51, 59, 60, 61, 62, 63, 66, 67, 69, 70, 73, 74, 75, 76, 77, 78, 80, 84, 100, 101, 102};
    return hot[r.U(sizeof(hot) / sizeof(hot[0]))];
  }
  if (k < 63) {  // an Exit op
    static const uint32_t ex[] = {0, 2, 13, 16, 17, 71};
    return ex[r.U(6)];
  }
  if (k < 85) {  // a Fall op (recompiled interpreter)
    static const uint32_t fall[] = {9, 10, 12, 42, 43, 46, 49, 52, 53, 54, 55, 56, 57, 58, 72, 79, 81, 82, 103, 81, 82};
    return fall[r.U(sizeof(fall) / sizeof(fall[0]))];
  }
  if (k < 90) return 105 + r.U(200);  // unknown ops above the table (do nothing)
  if (k < 92) return r.Chance(50) ? 0xFFFFFFFFu : 0x80000000u + r.U(4);
  return r.U(105);  // anything in the table
}

// Lay out one list of n records at `pos` of the buffer; returns the next free position.
uint32_t LayList(Arena& a, Rng& r, uint32_t buf, uint32_t pos, uint32_t n, uint32_t sub_budget, uint32_t& sub_used) {
  for (uint32_t i = 0; i < n && pos + 8 < kRecords; ++i) {
    const uint32_t at = buf + pos * 16;
    const uint32_t op = PickOp(r);
    // the Fall cases that dereference words of their payload (objects, device slots) get a payload of pointers into the globals page, whose own
    // words are pointers again, so every chain of loads stays inside committed memory
    const bool deref = op == 9 || op == 10 || op == 42 || op == 43 || op == 46 || op == 49 || (op >= 52 && op <= 58) || op == 72 || op == 79;
    const uint32_t len = deref ? 1 + r.U(3) : r.Chance(60) ? 0 : (r.Chance(90) ? r.U(4) : r.U(9));
    a.St(at, op);
    a.St(at + 4, r.g());
    a.St(at + 8, len);
    a.St(at + 12, r.g());
    for (uint32_t k = 0; k < len && pos + 1 + k < kRecords; ++k)
      for (int w = 0; w < 4; ++w) a.St(at + 16 * (k + 1) + 4 * w, r.Chance(5) ? 104 : r.g());
    if (deref)
      for (int w = 0; w < 4; ++w) a.St(at + 16 + 4 * w, kGlob + 4 * r.U(0x380));
    if (op == 81) {  // a call: its target at +16, valid (a short list ending in 82) or not
      uint32_t target;
      if (r.Chance(50) && sub_used + 4 * 16 * 6 < sub_budget) {
        target = kSub + sub_used;
        const uint32_t n2 = 1 + r.U(4);
        for (uint32_t k = 0; k < n2; ++k) {
          static const uint32_t sub_ops[] = {3, 4, 5, 6, 7, 8, 11, 14, 15, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
          a.St(target + 16 * k, sub_ops[r.U(sizeof(sub_ops) / sizeof(sub_ops[0]))]);
          a.St(target + 16 * k + 8, 0);
        }
        a.St(target + 16 * n2, 82);
        a.St(target + 16 * n2 + 8, 0);
        sub_used += 16 * (n2 + 2);
      } else {
        static const uint32_t bad[] = {0, 8, 0x10000001, kSub + 0xF00, kBuf0 + 3};
        target = bad[r.U(5)];
        if (target == kSub + 0xF00) {  // an aligned list whose first record is not a known op
          a.St(target, 0x7777);
          a.St(target + 8, 5000);
        }
      }
      a.St(at + 16, target);
    }
    pos += 1 + len;
  }
  return pos;
}

struct State {
  PPCContext ctx;
  uint32_t r1;
};

void MakeState(Arena& a, Rng& r, State& s, uint32_t kind) {
  std::memset(a.base + kMgr, 0, 0x1000);
  std::memset(a.base + kDesc, 0, 0x1000);
  std::memset(a.base + kSub, 0, 0x1000);
  std::memset(a.base + kGlob, 0, 0x1000);
  for (uint32_t b : {kBuf0, kBuf1})
    for (uint32_t i = 0; i < kRecords; ++i) {
      a.St(b + i * 16, 105);
      a.St(b + i * 16 + 4, r.g());
      a.St(b + i * 16 + 8, 0);
      a.St(b + i * 16 + 12, r.g());
    }
  for (uint32_t i = 0; i < 0x1000; i += 4) a.St(kGlob + i, kGlob + 4 * r.U(0x380));
  const uint32_t idx = r.Chance(70) ? 0 : 1;
  a.St(kMgr + 36, idx);
  a.St(kMgr + 48, kBuf0);
  a.St(kMgr + 52, kBuf1);
  a.St(kMgr + 56, kBuf1 + 16 * 3);
  a.St(kMgr + 60, kBuf0 + 32);
  a.St(kMgr + 108, kDesc);
  a.St(kMgr + 44, r.g());  // junk: the loop overwrites it
  uint32_t nlists = 1 + r.U(6);
  if (kind % 17 == 3) nlists = 0;
  uint32_t sub_used = 0;
  uint32_t bufs[2] = {kBuf0, kBuf1};
  uint32_t cur_buf = bufs[idx == 1 ? 1 : 0];
  uint32_t pos = r.U(5);
  for (uint32_t l = 0; l < nlists && l < 15; ++l) {
    const uint32_t n = r.Chance(8) ? 0 : 1 + r.U(r.Chance(10) ? 120 : 14);
    const uint32_t start = pos;
    pos = LayList(a, r, cur_buf, pos, n, 0xC00, sub_used);
    const uint32_t term = pos;
    // the list's terminator
    if (!r.Chance(6)) {
      a.St(cur_buf + term * 16, 104);
      a.St(cur_buf + term * 16 + 8, 0);
    }
    uint32_t end = term;
    if (r.Chance(10)) end = start;                          // an empty list: skipped even when records follow
    if (r.Chance(4)) end = term + 1 + r.U(3);               // an end the walk never meets
    if (idx == 2 && r.Chance(50)) cur_buf = kBuf1;
    a.St(kDesc + l * 68 + 12, end);
    a.St(kDesc + l * 68 + 16, start);
    pos = term + 1 + r.U(2);
    // the last list ends the replay
    if (l + 1 == nlists && r.Chance(60)) {
      a.St(cur_buf + (term + 1) * 16, 105);
    }
  }
  // the other buffer gets a random list too (for the states that flip the buffer index)
  uint32_t other_used = 0;
  LayList(a, r, idx == 1 ? kBuf0 : kBuf1, r.U(5), 1 + r.U(30), 0xC00, other_used);
  a.St(kMgr + 132, nlists + (kind % 23 == 5 ? 1 + r.U(3) : 0));  // some states count lists whose descriptors are empty
  // the call-stack frame the caller left (junk the recompiled prologue overwrites)
  s.r1 = 0x0F0E0000 + 16 * r.U(0x800);
  for (uint32_t i = s.r1 - kStackWindowLo; i < s.r1 + kStackWindowHi; i += 4) a.St(i, r.g());
  std::memset(&s.ctx, 0, sizeof(s.ctx));
  PPCRegister* regs[] = {&s.ctx.r0, &s.ctx.r2, &s.ctx.r4, &s.ctx.r5, &s.ctx.r6, &s.ctx.r7, &s.ctx.r8, &s.ctx.r9, &s.ctx.r10, &s.ctx.r11, &s.ctx.r12, &s.ctx.r13,
                         &s.ctx.r14, &s.ctx.r15, &s.ctx.r16, &s.ctx.r17, &s.ctx.r18, &s.ctx.r19, &s.ctx.r20, &s.ctx.r21, &s.ctx.r22, &s.ctx.r23, &s.ctx.r24, &s.ctx.r25,
                         &s.ctx.r26, &s.ctx.r27, &s.ctx.r28, &s.ctx.r29, &s.ctx.r30, &s.ctx.r31, &s.ctx.f0, &s.ctx.f1, &s.ctx.f2, &s.ctx.f3, &s.ctx.f13, &s.ctx.f14,
                         &s.ctx.f15, &s.ctx.f20, &s.ctx.f25, &s.ctx.f31};
  for (PPCRegister* p : regs) p->u64 = r.g();
  s.ctx.r1.u64 = s.r1;
  s.ctx.r3.u64 = kMgr;
  s.ctx.lr = 0x82BB5F34;
}

// ---- the comparison ----
struct Result {
  bool ok = true;
  char why[200] = {};
};

bool SameRegs(const PPCContext& x, const PPCContext& y, char* why) {
  const PPCRegister* xs[] = {&x.r1, &x.r2, &x.r13, &x.r14, &x.r15, &x.r16, &x.r17, &x.r18, &x.r19, &x.r20, &x.r21, &x.r22, &x.r23, &x.r24, &x.r25, &x.r26, &x.r27, &x.r28,
                             &x.r29, &x.r30, &x.r31, &x.f14, &x.f15, &x.f20, &x.f25, &x.f31};
  const PPCRegister* ys[] = {&y.r1, &y.r2, &y.r13, &y.r14, &y.r15, &y.r16, &y.r17, &y.r18, &y.r19, &y.r20, &y.r21, &y.r22, &y.r23, &y.r24, &y.r25, &y.r26, &y.r27, &y.r28,
                             &y.r29, &y.r30, &y.r31, &y.f14, &y.f15, &y.f20, &y.f25, &y.f31};
  for (size_t i = 0; i < sizeof(xs) / sizeof(xs[0]); ++i)
    if (xs[i]->u64 != ys[i]->u64) {
      std::snprintf(why, 200, "non-volatile register #%zu differs: %llx vs %llx", i, (unsigned long long)xs[i]->u64, (unsigned long long)ys[i]->u64);
      return false;
    }
  if (x.lr != y.lr) {
    std::snprintf(why, 200, "lr differs: %llx vs %llx", (unsigned long long)x.lr, (unsigned long long)y.lr);
    return false;
  }
  return true;
}

bool SameMemory(const Arena& a, const Arena& b, uint32_t r1, char* why) {
  for (const Region& r : kRegions) {
    if (std::memcmp(a.base + r.addr, b.base + r.addr, r.size) != 0) {
      for (uint32_t i = 0; i < r.size; ++i)
        if (a.base[r.addr + i] != b.base[r.addr + i]) {
          std::snprintf(why, 200, "memory differs at guest %08X (region %08X + %X): %02x vs %02x", r.addr + i, r.addr, i, a.base[r.addr + i], b.base[r.addr + i]);
          return false;
        }
    }
  }
  // the caller's frame above the stack pointer
  if (std::memcmp(a.base + r1, b.base + r1, kStackWindowHi) != 0) {
    std::snprintf(why, 200, "the caller's stack frame above r1 differs");
    return false;
  }
  return true;
}

// R / C1 / C2 / C3 run with the guard off (the PC default); R4 / C4 with the replay guard on and its test frame forced to trip once (list 0), the reference being the
// recompiled loop behind the same hook stand-in.
enum class Cfg { R, C1, C2, C3, R4, C4 };
const char* Name(Cfg c) {
  switch (c) {
    case Cfg::R: return "R";
    case Cfg::C1: return "C1 native loop fast + native dispatch";
    case Cfg::C2: return "C2 native loop slow + recompiled dispatch";
    case Cfg::C3: return "C3 recompiled loop + native dispatch";
    case Cfg::R4: return "R4 (guard on)";
    default: return "C4 native loop with the replay guard on";
  }
}

uint64_t g_fault_guest = 0, g_trace_state = ~0ull;
uint32_t snap_nlists = 0, snap_idx = 0;
bool g_plant_fault_trace = false;
int W3Filter(EXCEPTION_POINTERS* e, uint8_t* base) {
  g_fault_guest = uint64_t(e->ExceptionRecord->ExceptionInformation[1]) - uint64_t(uintptr_t(base));
  return EXCEPTION_EXECUTE_HANDLER;
}
// Returns false when the code faulted (a walk off the committed memory): the state is then recorded, never silently passed.
bool RunGuarded(Cfg c, PPCContext& ctx, uint8_t* base) {
  namespace rw = wwe2k14::replay_watch;
  g_guard = c == Cfg::R4 || c == Cfg::C4;
  g_w3_test_frame = g_guard ? 1 : 0;  // the guard's own test: it ends list 0 on purpose once
  g_w3_frame = 1;
  rw::t_test_frame_done = 0;
  __try {
    rw::BeginReplay(base, ctx.r3.u32, g_guard);  // what the sub_82BB5BE8 hook does before either loop
    switch (c) {
      case Cfg::R:
      case Cfg::R4:
        g_native_dispatch = false;
        __imp__sub_82BB5BE8(ctx, base);
        break;
      case Cfg::C1:
        g_native_dispatch = true;
        wwe2k14::nf_replay::ReplayLists(ctx, base, true, true);
        break;
      case Cfg::C2:
        g_native_dispatch = false;
        wwe2k14::nf_replay::ReplayLists(ctx, base, false, false);
        break;
      case Cfg::C3:
        g_native_dispatch = true;
        __imp__sub_82BB5BE8(ctx, base);
        break;
      case Cfg::C4:
        g_native_dispatch = true;
        wwe2k14::nf_replay::ReplayLists(ctx, base, false, true);  // the guard is on: the hook passes fast = false
        break;
    }
  } __except (W3Filter(GetExceptionInformation(), base)) {
    return false;
  }
  return true;
}

bool g_crashed = false;
uint32_t g_trips = 0;
void Run(Cfg c, PPCContext& ctx, Arena& a) {
  g_trace.clear();
  g_base = a.base;
  g_calls = 0;
  g_skipped = 0;
  g_fault_guest = 0;
  wwe2k14::replay_watch::g_calls_skipped = 0;
  const uint32_t reports0 = wwe2k14::replay_watch::g_reports.load();
  g_crashed = !RunGuarded(c, ctx, a.base);
  g_skipped = wwe2k14::replay_watch::g_calls_skipped.load();  // op 81 records the hook's check skipped
  g_trips = wwe2k14::replay_watch::g_reports.load() - reports0;  // times the replay guard ended a list or the replay
}

struct RefRun {
  PPCContext ctx;
  std::vector<Trace> trace;
  uint64_t skipped = 0, fault = 0;
  uint32_t trips = 0;
  bool crashed = false;
};

int Differential(uint64_t states, uint64_t seed) {
  static Arena A, B, G;  // reference (guard off), candidate, reference (guard on)
  A.Init();
  B.Init();
  G.Init();
  uint64_t bad = 0, empty_traces = 0, total_calls = 0, skipped81 = 0, shown = 0, fault_compared = 0, guard_trips = 0;
  std::set<uint32_t> seen_ids;
  uint64_t crash_by_mut[16] = {};
  Rng rng(seed);
  for (uint64_t n = 0; n < states && shown < 3; ++n) {  // three mismatches are the answer; a planted twin stops the run early
    State st;
    MakeState(A, rng, st, uint32_t(n));
    snap_nlists = A.Ld(kMgr + 132), snap_idx = A.Ld(kMgr + 36);
    g_mut = rng.Chance(40) ? rng.U(16) : 0;
    Snapshot snap;
    snap.Take(A, st.r1);
    auto make_ref = [&](Cfg c, Arena& arena, RefRun& r) {
      r.ctx = st.ctx;
      snap.Restore(arena);
      Run(c, r.ctx, arena);
      r.trace = g_trace, r.skipped = g_skipped, r.crashed = g_crashed, r.fault = g_fault_guest, r.trips = g_trips;
    };
    RefRun ref, ref4;
    make_ref(Cfg::R, A, ref);
    if (n == g_trace_state) {
      std::printf("state %llu: lists %u idx %u mut %u, crashed %d\n", (unsigned long long)n, snap_nlists, snap_idx, g_mut, int(ref.crashed));
      for (const Trace& t : ref.trace) std::printf("  call %08X r3 %08X r4 %08X words %08X %08X %08X %08X slot %08X pos %u lr %08X\n", t.id, t.r3, t.r4, t.w[0], t.w[1], t.w[2], t.w[3], t.slotv, t.pos, t.lr);
    }
    make_ref(Cfg::R4, G, ref4);
    total_calls += ref.trace.size();
    skipped81 += ref.skipped;
    if (ref.trace.empty()) ++empty_traces;
    guard_trips += ref4.trips;  // times the guard ended a list (the test frame, a walk off the buffer) in the guard-on reference
    for (Cfg c : {Cfg::C1, Cfg::C2, Cfg::C3, Cfg::C4}) {
      const RefRun& want = c == Cfg::C4 ? ref4 : ref;
      Arena& want_arena = c == Cfg::C4 ? G : A;
      PPCContext cc = st.ctx;
      snap.Restore(B);
      Run(c, cc, B);
      if (g_plant_fault_trace && g_crashed && !g_trace.empty()) g_trace.back().r3 ^= 1;  // the fault-comparison twin: one handler call changed just before the walk left memory
      char why[240] = {};
      bool ok = true;
      if (want.crashed || g_crashed) {
        if (want.crashed != g_crashed) {
          std::snprintf(why, sizeof(why), "%s faulted and the other did not", g_crashed ? "the native code" : "the recompiled code");
          ok = false;
        } else {
          // both faulted (a generated state that walks off the test's memory, e.g. a payload read as a record): the calls made before the fault and the faulting
          // guest address must still be equal
          ++crash_by_mut[g_mut];
          ++fault_compared;
          if (want.trace != g_trace) {
            size_t i = 0;
            while (i < want.trace.size() && i < g_trace.size() && want.trace[i] == g_trace[i]) ++i;
            std::snprintf(why, sizeof(why), "both faulted, but the calls before the fault differ at call #%zu (ref %zu calls, got %zu)", i, want.trace.size(), g_trace.size());
            ok = false;
          } else if (want.fault != g_fault_guest) {
            std::snprintf(why, sizeof(why), "both faulted, at guest %llx vs %llx", (unsigned long long)want.fault, (unsigned long long)g_fault_guest);
            ok = false;
          }
        }
        if (!ok) {
          ++bad;
          if (shown++ < 6) std::printf("MISMATCH state %llu (seed %llu, mut %u) %s: %s\n", (unsigned long long)n, (unsigned long long)seed, g_mut, Name(c), why);
        }
        continue;
      }
      ok = SameRegs(want.ctx, cc, why);
      if (ok) ok = SameMemory(want_arena, B, st.r1, why);
      if (ok && want.trace != g_trace) {
        size_t i = 0;
        while (i < want.trace.size() && i < g_trace.size() && want.trace[i] == g_trace[i]) ++i;
        std::snprintf(why, sizeof(why), "call trace differs at call #%zu (ref %zu calls, got %zu; ref id %08X r3 %08X r4 %08X lr %08X, got id %08X r3 %08X r4 %08X lr %08X)", i, want.trace.size(),
                      g_trace.size(), i < want.trace.size() ? want.trace[i].id : 0, i < want.trace.size() ? want.trace[i].r3 : 0, i < want.trace.size() ? want.trace[i].r4 : 0,
                      i < want.trace.size() ? want.trace[i].lr : 0, i < g_trace.size() ? g_trace[i].id : 0, i < g_trace.size() ? g_trace[i].r3 : 0, i < g_trace.size() ? g_trace[i].r4 : 0,
                      i < g_trace.size() ? g_trace[i].lr : 0);
        ok = false;
      }
      if (ok && g_skipped != want.skipped) {
        std::snprintf(why, sizeof(why), "op 81 calls skipped by the check: ref %llu, got %llu", (unsigned long long)want.skipped, (unsigned long long)g_skipped);
        ok = false;
      }
      if (!ok) {
        ++bad;
        if (shown++ < 6) std::printf("MISMATCH state %llu (seed %llu, mut %u) %s: %s\n", (unsigned long long)n, (unsigned long long)seed, g_mut, Name(c), why);
      }
    }
    for (const Trace& t : ref.trace) seen_ids.insert(t.id);
  }
  // coverage: every handler of the native table (the Call ops) and every callee of the recompiled interpreter must have been called by the reference
  size_t table_handlers = 0, table_hit = 0;
  std::string missing;
#define T_COV(op, kind, h, lr) \
  if (std::string(#kind) == "Call") { \
    ++table_handlers; \
    const uint32_t id = HandlerId(#h); \
    if (seen_ids.count(id)) ++table_hit; else missing += std::string(#h) + " "; \
  }
  WWE2K14_RENDER_OP_TABLE(T_COV)
#undef T_COV
  std::printf("w3_diff: %llu states x 4 configurations, %llu reference calls (%llu states with none), %llu op-81 calls skipped by the check, "
              "%llu guard trips that changed the replay (C4's reference), %llu configuration runs where both sides faulted in the test's memory (calls before the fault and the fault address compared), %llu mismatches\n",
              (unsigned long long)states, (unsigned long long)total_calls, (unsigned long long)empty_traces, (unsigned long long)skipped81, (unsigned long long)guard_trips,
              (unsigned long long)fault_compared, (unsigned long long)bad);
  std::printf("w3_diff: both-faulted runs by mutation mask:");
  for (int m = 0; m < 16; ++m) std::printf(" %llu", (unsigned long long)crash_by_mut[m]);
  std::printf("\n");
  std::printf("w3_diff: coverage: %zu of %zu table handlers called by the reference, %zu distinct callees in all%s%s\n", table_hit, table_handlers, seen_ids.size(),
              missing.empty() ? "" : "; NOT CALLED: ", missing.c_str());
  return bad ? 1 : 0;
}

// ---- the microbenchmark ----
int Bench() {
  static Arena A;
  A.Init();
  Rng r(12345);
  State st;
  MakeState(A, r, st, 1);
  // a heavy-entrance-like frame: 3 lists of ~2400 records, the op mix of the real frame (R13: op 35 the commonest, then 21, 3, 15)
  std::memset(A.base + kBuf0, 0, kRecords * 16);
  for (uint32_t i = 0; i < kRecords; ++i) A.St(kBuf0 + i * 16, 105);
  A.St(kMgr + 36, 0);
  uint32_t pos = 0;
  const uint32_t per_list = 450;
  for (uint32_t l = 0; l < 3; ++l) {
    A.St(kDesc + l * 68 + 16, pos);
    uint32_t made = 0;
    while (made < per_list) {
      const uint32_t k = r.U(100);
      uint32_t op = k < 28 ? 35 : k < 45 ? 21 : k < 55 ? 3 : k < 63 ? 15 : k < 70 ? 22 : k < 75 ? 24 : k < 80 ? 64 : k < 85 ? 83 : k < 88 ? 12 : k < 90 ? 19 : 36;
      const uint32_t len = (op == 35 || op == 21) ? 2 + r.U(3) : 0;
      A.St(kBuf0 + pos * 16, op);
      A.St(kBuf0 + pos * 16 + 8, len);
      pos += 1 + len;
      ++made;
    }
    A.St(kBuf0 + pos * 16, 104);
    A.St(kDesc + l * 68 + 12, pos);
    pos += 1;
  }
  A.St(kMgr + 132, 3);
  g_tracing = false;
  g_call_check = true;
  const int reps = 3000;
  uint64_t records = 3ull * per_list;
  auto time_cfg = [&](Cfg c) {
    double best = 1e30;
    for (int round = 0; round < 7; ++round) {
      PPCContext ctx = st.ctx;
      const auto t0 = std::chrono::high_resolution_clock::now();
      for (int i = 0; i < reps; ++i) {
        ctx.r3.u64 = kMgr;
        ctx.r1.u64 = st.r1;
        Run(c, ctx, A);
      }
      const auto t1 = std::chrono::high_resolution_clock::now();
      best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / reps);
    }
    return best;
  };
  const double r_ns = time_cfg(Cfg::R), c1_ns = time_cfg(Cfg::C1), c2_ns = time_cfg(Cfg::C2), c3_ns = time_cfg(Cfg::C3);
  std::printf("bench: %llu records a replay, stubs that only count; ns per replay / per record:\n", (unsigned long long)records);
  std::printf("  R  recompiled loop + recompiled dispatcher (with the hook stand-in) %9.0f %6.1f\n", r_ns, r_ns / records);
  std::printf("  C1 native loop fast + native dispatcher                              %9.0f %6.1f   saves %.1f ns a record, %.1f%%\n", c1_ns, c1_ns / records, (r_ns - c1_ns) / records,
              100 * (r_ns - c1_ns) / r_ns);
  std::printf("  C2 native loop, every record through the hook stand-in               %9.0f %6.1f\n", c2_ns, c2_ns / records);
  std::printf("  C3 recompiled loop + native dispatcher                               %9.0f %6.1f\n", c3_ns, c3_ns / records);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  uint64_t states = 2000, seed = 1;
  bool bench = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--states") && i + 1 < argc) states = std::strtoull(argv[++i], nullptr, 10);
    else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
    else if (!std::strcmp(argv[i], "--bench")) bench = true;
    else if (!std::strcmp(argv[i], "--plant-fault-trace")) g_plant_fault_trace = true;
    else if (!std::strcmp(argv[i], "--trace-state") && i + 1 < argc) g_trace_state = std::strtoull(argv[++i], nullptr, 10);
    else {
      std::fprintf(stderr, "usage: w3_diff [--states N] [--seed S] [--bench]\n");
      return 2;
    }
  }
  if (bench) return Bench();
  return Differential(states, seed);
}
