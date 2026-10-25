// wwe2k14 - Session 66 (NATIVE 2, track F2): the native function factory's hook and verify mode.
//
// A hot guest function gets a native C++ version (our own code, written from what the recompiled listing does) and
// is hooked through WWE2K14_NATIVE_FN. Each function has a mode:
//   off     the recompiled original only
//   verify  both on the same input: the native version first, its outputs saved, the inputs put back, then the
//           recompiled original (whose results the game keeps); every output byte and the returned registers are
//           compared and mismatches logged with the input that caused them
//   on      the native version only
// The spec of a function (a struct) says where it may write: Writes() lists the guest memory ranges, computed from
// the input registers before the call. Verify snapshots those ranges plus a guard band of kGuard bytes on each side,
// so a write just outside the declared ranges (by either version) shows up as a mismatch too ("outside").
// The guest stack within kDeadStack bytes below the caller's r1 is never compared (dead after the return): a native
// version does not mirror the original's frame or scratch writes there, and need not declare them.
// Registers compared: the ones the spec names in kRegs (r3, r4, f1, v2, cr0-7), the FP mode word, and the
// non-volatile r1, r2, r13-r31, f14-f31 (a native version must leave them alone).
//
// Spec (see native_skin.h for a worked one):
//   struct Foo {
//     static constexpr const char* kName = "foo";      // log name
//     static constexpr uint32_t kGuest = 0x82xxxxxx;   // the hooked function
//     static constexpr uint32_t kRegs = kR3;           // returned registers
//     static Mode GetMode();                           // usually ModeFromCvar(REXCVAR_GET(wwe2k14_nf_foo))
//     static void Writes(const PPCContext& ctx, uint8_t* base, Ranges& out);
//     static void Native(PPCContext& ctx, uint8_t* base);
//   };
//   WWE2K14_NATIVE_FN(sub_82xxxxxx, Foo)
//
// Cvars: one per function (the spec's own, "off | verify | on"); --wwe2k14_nf_all=off|verify|on overrides every
// function (empty = each its own; settable live: `cvar wwe2k14_nf_all verify`). --wwe2k14_nf_log_s: seconds between
// the [wwe2k14][nf] count lines (calls, mismatching calls and bytes, native / recompiled time; verify measures both).

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_STRING(wwe2k14_nf_all, "", "wwe2k14",
                      "Session 66 (native function factory): off | verify | on for EVERY native function (empty = each "
                      "function's own cvar)");
REXCVAR_DEFINE_BOOL(wwe2k14_nf_verify_strict, true, "wwe2k14",
                    "Session 66: verify also compares every volatile register (r0, r4-r12, f0-f13, v0-v31, CR, CTR, XER) and the "
                    "512 bytes below the stack pointer (what the original leaves there)");
REXCVAR_DEFINE_INT32(wwe2k14_nf_log_s, 5, "wwe2k14",
                     "Session 66: seconds between [wwe2k14][nf] count lines (0 = never)");

// The default of a function verified with 0 mismatches over a launched match and both entrances (twice) and faster in
// an in-run A/B: on where it was verified (x86-64: runs 4187 / 4197; A/B 4259: main game thread -11.5% CPU, skinning
// thread -16.2%), off elsewhere until a run there verifies it (Android: the FP mode handling differs).
#if 0  // Session 66 02:15: off again - with them on the determinism replay diverged (check_item nf_on, 4269 / 4270)
#define WWE2K14_NF_VERIFIED_DEFAULT "on"
#else
#define WWE2K14_NF_VERIFIED_DEFAULT "off"
#endif

// Session 66 (03:30): on for x86-64 only, per function: strict verify 0 mismatches (4313), determinism identical
// (4316-4319) and a gain in the in-run A/B with the strict-clean version (skin_3bone: skinning thread -15%).
#if defined(__x86_64__) || defined(_M_X64)
#define WWE2K14_NF_X86_ON "on"
#else
#define WWE2K14_NF_X86_ON "off"
#endif

namespace wwe2k14::native_fn {

enum class Mode : uint8_t { kOff, kVerify, kOn };
enum : uint32_t { kR3 = 1, kR4 = 2, kF1 = 4, kV2 = 8, kCr = 16 };
constexpr uint32_t kGuard = 64;
constexpr uint32_t kDeadStack = 16384;  // bytes below the stack pointer verify ignores (not in strict mode)
constexpr uint32_t kStackProbe = 512;   // strict verify: the window below the stack pointer compared
// Guest address -> host pointer (0xE0000000+ sits 4 KB further on Windows: rex::memory::GuestPtr).
inline uint8_t* Host(uint8_t* base, uint32_t addr) { return rex::memory::GuestPtr(base, addr); }

inline Mode ModeFromText(const std::string& s) {
  if (s.empty()) return Mode::kOff;
  if (s[0] == 'v' || s[0] == 'V') return Mode::kVerify;
  if (s == "on" || s == "ON" || s == "1" || s == "true") return Mode::kOn;
  return Mode::kOff;
}
// The global override first, then the function's own setting.
inline Mode ModeFromCvar(const std::string& own) {
  const std::string& all = REXCVAR_GET(wwe2k14_nf_all);
  return all.empty() ? ModeFromText(own) : ModeFromText(all);
}

struct Ranges {
  struct Range {
    uint32_t addr, size;
  };
  Range r[16];
  int n = 0;
  bool overflow = false;
  void Add(uint32_t addr, uint32_t size) {
    if (!size) return;
    if (n == 16) {
      overflow = true;
      return;
    }
    r[n++] = {addr, size};
  }
};

struct Stats {
  const char* name = "";
  uint32_t guest = 0;
  std::atomic<uint64_t> calls{0}, verified{0}, mismatch_calls{0}, mismatch_bytes{0}, outside_calls{0},
      reg_mismatch_calls{0}, race_calls{0}, native_ns{0}, guest_ns{0};
  std::atomic<uint32_t> reported{0};
  Stats* next = nullptr;
};

struct Registry {
  std::mutex mutex;
  Stats* head = nullptr;
  std::chrono::steady_clock::time_point last_log{};
};
inline Registry& GetRegistry() {
  static Registry r;
  return r;
}
inline void Register(Stats& s, const char* name, uint32_t guest) {
  Registry& r = GetRegistry();
  std::lock_guard lock(r.mutex);
  s.name = name;
  s.guest = guest;
  s.next = r.head;
  r.head = &s;
}

inline void MaybeLog() {
  const int32_t every = REXCVAR_GET(wwe2k14_nf_log_s);
  if (every <= 0) return;
  Registry& r = GetRegistry();
  const auto now = std::chrono::steady_clock::now();
  if (now - r.last_log < std::chrono::seconds(every)) return;
  std::unique_lock lock(r.mutex, std::try_to_lock);
  if (!lock.owns_lock() || now - r.last_log < std::chrono::seconds(every)) return;
  r.last_log = now;
  for (Stats* s = r.head; s; s = s->next) {
    const uint64_t calls = s->calls.load(), verified = s->verified.load();
    if (!calls) continue;
    const double nms = s->native_ns.load() / 1e6, gms = s->guest_ns.load() / 1e6;
    REXLOG_INFO("[wwe2k14][nf] {} ({:08X}): calls {}, verified {}, mismatching calls {} (bytes {}, outside {}, "
                "registers {}), races {}, native {:.1f} ms, recompiled {:.1f} ms{}",
                s->name, s->guest, calls, verified, s->mismatch_calls.load(), s->mismatch_bytes.load(),
                s->outside_calls.load(), s->reg_mismatch_calls.load(), s->race_calls.load(), nms, gms,
                verified && nms > 0 ? fmt::format(" (verified calls: {:.2f}x)", gms / nms) : std::string());
  }
}

inline std::string InputText(const PPCContext& c) {
  return fmt::format("r3 {:08X} r4 {:08X} r5 {:08X} r6 {:08X} r7 {:08X} r8 {:08X} r9 {:08X} r10 {:08X} "
                     "f1 {:016X} f2 {:016X} f3 {:016X} f4 {:016X} lr {:08X}",
                     c.r3.u32, c.r4.u32, c.r5.u32, c.r6.u32, c.r7.u32, c.r8.u32, c.r9.u32, c.r10.u32, c.f1.u64,
                     c.f2.u64, c.f3.u64, c.f4.u64, uint32_t(c.lr));
}

// Registers a native version must return like the original (kRegs) or leave alone (the non-volatile ones).
inline const char* RegDiff(const PPCContext& a, const PPCContext& b, uint32_t regs) {
  if ((regs & kR3) && a.r3.u64 != b.r3.u64) return "r3";
  if ((regs & kR4) && a.r4.u64 != b.r4.u64) return "r4";
  if ((regs & kF1) && a.f1.u64 != b.f1.u64) return "f1";
  if ((regs & kV2) && std::memcmp(&a.v2, &b.v2, sizeof(a.v2)) != 0) return "v2";
  if ((regs & kCr) && (std::memcmp(&a.cr0, &b.cr0, sizeof(a.cr0) * 8) != 0)) return "cr";
  if (a.r1.u64 != b.r1.u64 || a.r2.u64 != b.r2.u64) return "r1/r2";
  if (std::memcmp(&a.r13, &b.r13, sizeof(a.r13) * 19) != 0) return "r13-r31";
  if (std::memcmp(&a.f14, &b.f14, sizeof(a.f14) * 18) != 0) return "f14-f31";
  // Only the guest-owned bits (rounding, flush mode): the rest of the host word holds sticky exception flags, which
  // verify's own restore picks up from the host.
  if ((a.fpscr.csr ^ b.fpscr.csr) & decltype(a.fpscr)::GuestMask) return "fpscr";
  return nullptr;
}

// Session 66 (strict verify, after check_item nf_on diverged with outputs byte-identical): the volatile registers too -
// the game's compiler may rely on what a callee leaves in them (whole-program register allocation) - and CR, CTR, XER.
inline const char* StrictRegDiff(const PPCContext& a, const PPCContext& b) {
  if (a.r0.u64 != b.r0.u64) return "r0";
  if (std::memcmp(&a.r4, &b.r4, sizeof(a.r4) * 9) != 0) return "r4-r12";
  if (std::memcmp(&a.f0, &b.f0, sizeof(a.f0) * 14) != 0) return "f0-f13";
  if (std::memcmp(&a.v0, &b.v0, sizeof(a.v0) * 128) != 0) return "v0-v127";  // VMX128: 128 registers
  if (std::memcmp(&a.cr0, &b.cr0, sizeof(a.cr0) * 8) != 0) return "cr";
  if (a.ctr.u64 != b.ctr.u64) return "ctr";
  if (std::memcmp(&a.xer, &b.xer, sizeof(a.xer)) != 0) return "xer";
  return nullptr;
}

struct Scratch {
  std::vector<uint8_t> before, native, after;
};
// One per nesting depth: a verified function's recompiled original may call another verified function.
inline thread_local Scratch t_scratch[8];
inline thread_local uint32_t t_depth = 0;
struct DepthGuard {
  DepthGuard() { ++t_depth; }
  ~DepthGuard() { --t_depth; }
};

template <class Spec>
Stats& StatsOf() {
  static Stats s;
  static const bool registered = (Register(s, Spec::kName, Spec::kGuest), true);
  (void)registered;
  return s;
}

// The function's mode, read from its cvars once every 256 calls per thread (string compares on every call cost ~10%
// of the small helpers' native time); a live change shows within microseconds.
template <class Spec>
Mode CachedMode() {
  static thread_local uint32_t tick = 0;
  static thread_local Mode mode = Mode::kOff;
  if ((tick++ & 255) == 0) mode = Spec::GetMode();
  return mode;
}

template <class Spec>
void Dispatch(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*)) {
  const Mode mode = CachedMode<Spec>();
  if (mode == Mode::kOff) {
    original(ctx, base);
    return;
  }
  if (mode == Mode::kOn) {
    // Nothing per call but the native version: no clock reads, no shared atomics (both cost more than the small
    // helpers themselves - in-run A/B 4254: the main game thread +33% CPU with them per call). Calls are counted per
    // thread and published every 4096.
    Spec::Native(ctx, base);
    static thread_local uint32_t pending = 0;
    if (++pending == 4096) {
      StatsOf<Spec>().calls.fetch_add(pending, std::memory_order_relaxed);
      pending = 0;
      MaybeLog();
    }
    return;
  }
  Stats& st = StatsOf<Spec>();
  st.calls.fetch_add(1, std::memory_order_relaxed);
  // Verify: the guarded ranges' bytes before the call, the native version, its bytes, the inputs back, the original.
  Ranges ranges;
  Spec::Writes(ctx, base, ranges);
  const bool strict = REXCVAR_GET(wwe2k14_nf_verify_strict);
  if (strict) ranges.Add(ctx.r1.u32 - kStackProbe, kStackProbe);
  Scratch& sc = t_scratch[t_depth < 8 ? t_depth : 7];
  const DepthGuard depth_guard;
  size_t total = 0;
  for (int i = 0; i < ranges.n; ++i) total += ranges.r[i].size + 2 * kGuard;
  sc.before.resize(total);
  sc.native.resize(total);
  const auto guarded = [&](int i) { return ranges.r[i].addr - kGuard; };
  size_t o = 0;
  for (int i = 0; i < ranges.n; ++i) {
    const size_t len = ranges.r[i].size + 2 * kGuard;
    std::memcpy(sc.before.data() + o, Host(base, guarded(i)), len);
    o += len;
  }
  const PPCContext saved = ctx;
  const auto t0 = std::chrono::steady_clock::now();
  Spec::Native(ctx, base);
  const auto t1 = std::chrono::steady_clock::now();
  const PPCContext native_ctx = ctx;
  // Window-shaped copies (windows of neighbouring ranges can overlap: every copy is taken before anything is put
  // back). Put back = only the bytes of the declared ranges that differ: never the guard band, never a whole window,
  // so a write another thread makes meanwhile is not undone (verify run 4184 hung on such a lost update).
  const auto capture = [&](std::vector<uint8_t>& into) {
    size_t at = 0;
    for (int i = 0; i < ranges.n; ++i) {
      const size_t len = ranges.r[i].size + 2 * kGuard;
      std::memcpy(into.data() + at, Host(base, guarded(i)), len);
      at += len;
    }
  };
  const auto put_back = [&](const std::vector<uint8_t>& from) {
    size_t at = 0;
    for (int i = 0; i < ranges.n; ++i) {
      uint8_t* g = Host(base, guarded(i));
      for (size_t j = kGuard; j < kGuard + ranges.r[i].size; ++j) {
        if (g[j] != from[at + j]) g[j] = from[at + j];
      }
      at += ranges.r[i].size + 2 * kGuard;
    }
  };
  capture(sc.native);
  put_back(sc.before);
  ctx = saved;
  ctx.fpscr.restoreGuestBits(saved.fpscr.csr);  // the host FP control register back to the cached word's mode
  const auto t2 = std::chrono::steady_clock::now();
  original(ctx, base);
  const auto t3 = std::chrono::steady_clock::now();
  st.verified.fetch_add(1, std::memory_order_relaxed);
  st.native_ns.fetch_add(uint64_t((t1 - t0).count()), std::memory_order_relaxed);
  st.guest_ns.fetch_add(uint64_t((t3 - t2).count()), std::memory_order_relaxed);
  // Compare.
  uint64_t bad = 0;
  bool outside = false;
  int first_range = -1;
  size_t first_off = 0;
  // The guest stack below the caller's stack pointer is dead after the return (the original's frame and scratch):
  // not compared, so a native version need not mirror what the original leaves there.
  const uint32_t sp = saved.r1.u32;
  const auto dead = [&](uint32_t addr) { return !strict && addr < sp && sp - addr <= kDeadStack; };
  // A guard byte inside another declared range is that range's (written legitimately).
  const auto declared = [&](uint32_t addr) {
    for (int k = 0; k < ranges.n; ++k) {
      if (addr - ranges.r[k].addr < ranges.r[k].size) return true;
    }
    return false;
  };
  o = 0;
  for (int i = 0; i < ranges.n; ++i) {
    const size_t len = ranges.r[i].size + 2 * kGuard;
    const uint8_t* g = Host(base, guarded(i));
    for (size_t j = 0; j < len; ++j) {
      if (sc.native[o + j] == g[j] || dead(guarded(i) + uint32_t(j))) continue;
      ++bad;
      if ((j < kGuard || j >= kGuard + ranges.r[i].size) && !declared(guarded(i) + uint32_t(j))) outside = true;
      if (first_range < 0) first_range = i, first_off = j;
    }
    // A guard byte the original changed (equal in both) means the declared ranges are too small.
    for (size_t j = 0; j < len; ++j) {
      if ((j < kGuard || j >= kGuard + ranges.r[i].size) && g[j] != sc.before[o + j] && !dead(guarded(i) + uint32_t(j)) &&
          !declared(guarded(i) + uint32_t(j))) {
        outside = true;
        if (first_range < 0) first_range = i, first_off = j;
        break;
      }
    }
    o += len;
  }
  const char* reg = RegDiff(native_ctx, ctx, Spec::kRegs);
  if (!reg && strict) reg = StrictRegDiff(native_ctx, ctx);
  // A difference in the outputs or registers may be a race: another thread changed an input between the native run
  // and the recompiled one. Then the native version, run once more now on the same (restored) memory, gives what the
  // original gave: counted as a race, not a mismatch. (Guard-band differences are not re-checked.)
  if ((bad || reg) && !outside && !ranges.overflow) {
    sc.after.resize(total);
    capture(sc.after);
    put_back(sc.before);
    PPCContext again = saved;
    again.fpscr.restoreGuestBits(saved.fpscr.csr);
    Spec::Native(again, base);
    bool same = RegDiff(again, ctx, Spec::kRegs) == nullptr && (!strict || StrictRegDiff(again, ctx) == nullptr);
    o = 0;
    for (int i = 0; i < ranges.n; ++i) {
      const size_t len = ranges.r[i].size + 2 * kGuard;
      uint8_t* g = Host(base, guarded(i));
      for (size_t j = kGuard; same && j < kGuard + ranges.r[i].size; ++j) {
        same = g[j] == sc.after[o + j] || dead(guarded(i) + uint32_t(j));
      }
      o += len;
    }
    put_back(sc.after);  // the original's results stay
    ctx.fpscr.restoreGuestBits(ctx.fpscr.csr);
    if (same) {
      st.race_calls.fetch_add(1, std::memory_order_relaxed);
      MaybeLog();
      return;
    }
  }
  if (bad || outside || reg || ranges.overflow) {
    st.mismatch_calls.fetch_add(1, std::memory_order_relaxed);
    st.mismatch_bytes.fetch_add(bad, std::memory_order_relaxed);
    if (outside) st.outside_calls.fetch_add(1, std::memory_order_relaxed);
    if (reg) st.reg_mismatch_calls.fetch_add(1, std::memory_order_relaxed);
    if (st.reported.fetch_add(1, std::memory_order_relaxed) < 8) {
      std::string where = "none";
      if (first_range >= 0) {
        size_t off = 0;
        for (int i = 0; i < first_range; ++i) off += ranges.r[i].size + 2 * kGuard;
        const uint32_t addr = guarded(first_range) + uint32_t(first_off);
        where = fmt::format("{:08X} (range {} at {:08X}+{}{}) native {:02X} recompiled {:02X} before {:02X}", addr,
                            first_range, ranges.r[first_range].addr,
                            int64_t(first_off) - int64_t(kGuard), outside ? ", outside a declared range" : "",
                            sc.native[off + first_off], *Host(base, addr), sc.before[off + first_off]);
      }
      REXLOG_WARN("[wwe2k14][nf] {} mismatch: {} bytes, first {}; register {}{}; input {}", Spec::kName, bad, where,
                  reg ? reg : "ok", ranges.overflow ? "; too many ranges" : "", InputText(saved));
    }
  }
  MaybeLog();
}

}  // namespace wwe2k14::native_fn

#define WWE2K14_NATIVE_FN(sub, Spec) \
  REX_EXTERN(__imp__##sub);          \
  REX_HOOK_RAW(sub) { ::wwe2k14::native_fn::Dispatch<Spec>(ctx, base, __imp__##sub); }
