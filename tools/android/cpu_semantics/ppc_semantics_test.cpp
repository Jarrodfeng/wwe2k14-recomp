// Session 73: does the recompiled game's arithmetic give the same bits on a phone (ARM64) as on the PC (x86-64)?
// Each case below is the exact statement the SDK's codegen emits for one PowerPC instruction (src/codegen/builders/
// floating_point.cpp, vector.cpp, context.cpp), run on edge inputs (NaN, infinities, overflow, denormals, -0, halves)
// in the FPSCR mode the generated code sets. Prints one line per result: "name input... -> hex". Built for both hosts
// by run.ps1 (x86-64 here, ARM64 static under QEMU in Docker) and the outputs diffed.
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <rex/ppc/context.h>
#include <rex/ppc/intrinsics.h>

using rex::ppc::FPSCRRegister;

namespace {

struct Ctx {
  PPCFPSCRRegister fpscr;
  PPCRegister f0, f1, f2, f3;
  PPCVRegister v0, v1, v2, v3;
};

uint64_t Bits(double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; }
uint32_t Bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
double D(uint64_t u) { double d; std::memcpy(&d, &u, 8); return d; }
float F(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

volatile double kDoubles[] = {0.0, -0.0, 0.5, -0.5, 1.5, 2.5, -2.5, 3.7, -3.7, 2147483647.0, 2147483647.5, 2147483648.0,
                           -2147483648.0, -2147483648.5, -2147483649.0, 4294967296.0, 9.3e18, -9.3e18, 1e300, -1e300,
                           INFINITY, -INFINITY, D(0x7FF8000000000000ull), D(0xFFF8000000000000ull),
                           D(0x7FF0000000000001ull), D(0x0000000000000001ull), D(0x8000000000000001ull),
                           D(0x000FFFFFFFFFFFFFull), 1.17549435e-38, 1.0e-39, -1.0e-39, 1.0e-45, 3.4028235e38,
                           3.4028236e38, 1e39};
volatile float kFloats[] = {0.0f, -0.0f, 0.5f, -0.5f, 1.5f, 2.5f, -2.5f, 3.7f, -3.7f, 2147483520.0f, 2147483648.0f,
                         -2147483648.0f, -2147483904.0f, 4294967040.0f, 4294967296.0f, 1e20f, -1e20f, INFINITY,
                         -INFINITY, F(0x7FC00000u), F(0xFFC00000u), F(0x7F800001u), F(0x00000001u), F(0x80000001u),
                         F(0x007FFFFFu), 1.17549435e-38f, 3.4028235e38f, 65536.0f, -1.0f, 1e-20f};

void ScalarCases(Ctx& ctx) {
  ctx.fpscr.disableFlushModeUnconditional();
  for (size_t ai = 0; ai < sizeof(kDoubles) / sizeof(kDoubles[0]); ++ai) {
    const double a = kDoubles[ai];
    ctx.f1.f64 = a;
    // fctiw / fctiwz / fctid / fctidz
    ctx.f0.s64 = std::isnan(ctx.f1.f64) ? int64_t(0x80000000U) : (ctx.f1.f64 >= double(INT_MAX)) ? INT_MAX : rex::ppc::cvt_f64_i32_current(ctx.f1.f64);
    std::printf("fctiw %016llx -> %016llx\n", (unsigned long long)Bits(a), (unsigned long long)ctx.f0.u64);
    ctx.f0.s64 = std::isnan(ctx.f1.f64) ? int64_t(0x80000000U) : (ctx.f1.f64 >= double(INT_MAX)) ? INT_MAX : simde_mm_cvttsd_si32(simde_mm_load_sd(&ctx.f1.f64));
    std::printf("fctiwz %016llx -> %016llx\n", (unsigned long long)Bits(a), (unsigned long long)ctx.f0.u64);
    ctx.f0.s64 = std::isnan(ctx.f1.f64) ? int64_t(0x8000000000000000ULL) : (ctx.f1.f64 > double(LLONG_MAX)) ? LLONG_MAX : rex::ppc::cvt_f64_i64_current(ctx.f1.f64);
    std::printf("fctid %016llx -> %016llx\n", (unsigned long long)Bits(a), (unsigned long long)ctx.f0.u64);
    ctx.f0.s64 = std::isnan(ctx.f1.f64) ? int64_t(0x8000000000000000ULL) : (ctx.f1.f64 > double(LLONG_MAX)) ? LLONG_MAX : simde_mm_cvttsd_si64(simde_mm_load_sd(&ctx.f1.f64));
    std::printf("fctidz %016llx -> %016llx\n", (unsigned long long)Bits(a), (unsigned long long)ctx.f0.u64);
    // frsp / fres / frsqrte / fsqrt / fsel
    ctx.f0.f64 = double(float(ctx.f1.f64));
    std::printf("frsp %016llx -> %016llx\n", (unsigned long long)Bits(a), (unsigned long long)Bits(ctx.f0.f64));
    ctx.f0.f64 = double(float(1.0 / ctx.f1.f64));
    std::printf("fres %016llx -> %016llx\n", (unsigned long long)Bits(a), (unsigned long long)Bits(ctx.f0.f64));
    ctx.f0.f64 = double(float(1.0 / sqrt(ctx.f1.f64)));
    std::printf("frsqrte %016llx -> %016llx\n", (unsigned long long)Bits(a), (unsigned long long)Bits(ctx.f0.f64));
    ctx.f0.f64 = sqrt(ctx.f1.f64);
    std::printf("fsqrt %016llx -> %016llx\n", (unsigned long long)Bits(a), (unsigned long long)Bits(ctx.f0.f64));
    ctx.f2.f64 = 1.0; ctx.f3.f64 = 2.0;
    ctx.f0.f64 = ctx.f1.f64 >= 0.0 ? ctx.f2.f64 : ctx.f3.f64;
    std::printf("fsel %016llx -> %016llx\n", (unsigned long long)Bits(a), (unsigned long long)Bits(ctx.f0.f64));
    for (double b : {0.0, 1.0, -1.0, double(INFINITY), 1e-310, 3.0}) {
      ctx.f2.f64 = b;
      ctx.f3.f64 = -0.25;
      ctx.f0.f64 = double(float(ctx.f1.f64 * ctx.f2.f64));
      std::printf("fmuls %016llx %016llx -> %016llx\n", (unsigned long long)Bits(a), (unsigned long long)Bits(b), (unsigned long long)Bits(ctx.f0.f64));
      ctx.f0.f64 = double(float(ctx.f1.f64 + ctx.f2.f64));
      std::printf("fadds %016llx %016llx -> %016llx\n", (unsigned long long)Bits(a), (unsigned long long)Bits(b), (unsigned long long)Bits(ctx.f0.f64));
      ctx.f0.f64 = std::fma(ctx.f1.f64, ctx.f2.f64, ctx.f3.f64);
      std::printf("fmadd %016llx %016llx -> %016llx\n", (unsigned long long)Bits(a), (unsigned long long)Bits(b), (unsigned long long)Bits(ctx.f0.f64));
      ctx.f0.f64 = double(float(std::fma(ctx.f1.f64, ctx.f2.f64, ctx.f3.f64)));
      std::printf("fmadds %016llx %016llx -> %016llx\n", (unsigned long long)Bits(a), (unsigned long long)Bits(b), (unsigned long long)Bits(ctx.f0.f64));
      ctx.f0.f64 = ctx.f1.f64 / ctx.f2.f64;
      std::printf("fdiv %016llx %016llx -> %016llx\n", (unsigned long long)Bits(a), (unsigned long long)Bits(b), (unsigned long long)Bits(ctx.f0.f64));
    }
  }
  // Rounding modes (mtfsf / mtfsb): fctiw in each guest mode, and the mode read back (mffs) after a partial store.
  for (uint32_t mode = 0; mode < 4; ++mode) {
    ctx.fpscr.storeFromGuest(mode);
    const uint32_t back = ctx.fpscr.loadFromHost();
    // mtfsf with a field mask that leaves RN alone (codegen: storeFromGuest((loadFromHost() & mask) | (v & ~mask))).
    ctx.fpscr.storeFromGuest((ctx.fpscr.loadFromHost() & 0x00000003u) | (0u & ~0x00000003u));
    const uint32_t after_partial = ctx.fpscr.loadFromHost();
    static volatile double kModeInputs[] = {2.5, -2.5, 3.7, -3.7, 0.5};
    for (size_t ai = 0; ai < 5; ++ai) {
      const double a = kModeInputs[ai];
      ctx.f1.f64 = a;
      ctx.f0.s64 = std::isnan(ctx.f1.f64) ? int64_t(0x80000000U) : (ctx.f1.f64 >= double(INT_MAX)) ? INT_MAX : rex::ppc::cvt_f64_i32_current(ctx.f1.f64);
      std::printf("mode%u fctiw %.1f -> %lld (mffs %u, after partial mtfsf %u)\n", mode, a, (long long)ctx.f0.s64, back, after_partial);
      ctx.f0.f64 = double(float(ctx.f1.f64 / 3.0));
      std::printf("mode%u fdivs %.1f/3 -> %016llx\n", mode, a, (unsigned long long)Bits(ctx.f0.f64));
    }
  }
  ctx.fpscr.storeFromGuest(0);
}

void PrintV(const char* name, const PPCVRegister& in, const PPCVRegister& out) {
  std::printf("%s %08x %08x %08x %08x -> %08x %08x %08x %08x\n", name, in.u32[0], in.u32[1], in.u32[2], in.u32[3],
              out.u32[0], out.u32[1], out.u32[2], out.u32[3]);
}

void VectorCases(Ctx& ctx) {
  ctx.fpscr.enableFlushModeUnconditional();
  const size_t n = sizeof(kFloats) / sizeof(kFloats[0]);
  for (size_t i = 0; i < n; ++i) {
    for (int k = 0; k < 4; ++k) {
      ctx.v1.f32[k] = kFloats[(i + k) % n];
      ctx.v2.f32[k] = kFloats[(i + 3 * k + 1) % n];
      ctx.v3.f32[k] = kFloats[(i + 5 * k + 2) % n];
    }
    simde_mm_store_si128((simde__m128i*)ctx.v0.s32, rex::ppc::simde_mm_vctsxs(simde_mm_load_ps(ctx.v1.f32)));
    PrintV("vctsxs", ctx.v1, ctx.v0);
    simde_mm_store_si128((simde__m128i*)ctx.v0.s32, rex::ppc::simde_mm_vctsxs(simde_mm_mul_ps(simde_mm_load_ps(ctx.v1.f32), simde_mm_set1_ps(256))));
    PrintV("vctsxs8", ctx.v1, ctx.v0);
    simde_mm_store_si128((simde__m128i*)ctx.v0.u32, rex::ppc::simde_mm_vctuxs(simde_mm_load_ps(ctx.v1.f32)));
    PrintV("vctuxs", ctx.v1, ctx.v0);
    simde_mm_store_ps(ctx.v0.f32, simde_mm_cvtepi32_ps(simde_mm_load_si128((simde__m128i*)ctx.v1.u32)));
    PrintV("vcfsx", ctx.v1, ctx.v0);
    simde_mm_store_ps(ctx.v0.f32, simde_mm_div_ps(simde_mm_set1_ps(1), simde_mm_load_ps(ctx.v1.f32)));
    PrintV("vrefp", ctx.v1, ctx.v0);
    simde_mm_store_ps(ctx.v0.f32, simde_mm_div_ps(simde_mm_set1_ps(1), simde_mm_sqrt_ps(simde_mm_load_ps(ctx.v1.f32))));
    PrintV("vrsqrtefp", ctx.v1, ctx.v0);
    simde_mm_store_ps(ctx.v0.f32, simde_mm_add_ps(simde_mm_load_ps(ctx.v1.f32), simde_mm_load_ps(ctx.v2.f32)));
    PrintV("vaddfp", ctx.v1, ctx.v0);
    simde_mm_store_ps(ctx.v0.f32, simde_mm_mul_ps(simde_mm_load_ps(ctx.v1.f32), simde_mm_load_ps(ctx.v2.f32)));
    PrintV("vmulfp", ctx.v1, ctx.v0);
    simde_mm_store_ps(ctx.v0.f32, simde_mm_add_ps(simde_mm_mul_ps(simde_mm_load_ps(ctx.v1.f32), simde_mm_load_ps(ctx.v2.f32)), simde_mm_load_ps(ctx.v3.f32)));
    PrintV("vmaddfp", ctx.v1, ctx.v0);
    simde_mm_store_ps(ctx.v0.f32, rex::ppc::simde_mm_vmaxfp(simde_mm_load_ps(ctx.v1.f32), simde_mm_load_ps(ctx.v2.f32)));
    PrintV("vmaxfp", ctx.v1, ctx.v0);
    simde_mm_store_ps(ctx.v0.f32, rex::ppc::simde_mm_vminfp(simde_mm_load_ps(ctx.v1.f32), simde_mm_load_ps(ctx.v2.f32)));
    PrintV("vminfp", ctx.v1, ctx.v0);
    simde_mm_store_ps(ctx.v0.f32, simde_mm_cmpge_ps(simde_mm_load_ps(ctx.v1.f32), simde_mm_load_ps(ctx.v2.f32)));
    PrintV("vcmpgefp", ctx.v1, ctx.v0);
    simde_mm_store_ps(ctx.v0.f32, simde_mm_cmpeq_ps(simde_mm_load_ps(ctx.v1.f32), simde_mm_load_ps(ctx.v2.f32)));
    PrintV("vcmpeqfp", ctx.v1, ctx.v0);
    simde_mm_store_ps(ctx.v0.f32, simde_mm_round_ps(simde_mm_load_ps(ctx.v1.f32), SIMDE_MM_FROUND_TO_NEG_INF | SIMDE_MM_FROUND_NO_EXC));
    PrintV("vrfim", ctx.v1, ctx.v0);
    simde_mm_store_ps(ctx.v0.f32, simde_mm_round_ps(simde_mm_load_ps(ctx.v1.f32), SIMDE_MM_FROUND_TO_NEAREST_INT | SIMDE_MM_FROUND_NO_EXC));
    PrintV("vrfin", ctx.v1, ctx.v0);
    simde_mm_store_ps(ctx.v0.f32, simde_mm_round_ps(simde_mm_load_ps(ctx.v1.f32), SIMDE_MM_FROUND_TO_ZERO | SIMDE_MM_FROUND_NO_EXC));
    PrintV("vrfiz", ctx.v1, ctx.v0);
    for (int k = 0; k < 4; ++k) ctx.v0.f32[k] = log2f(ctx.v1.f32[k]);
    PrintV("vlogefp", ctx.v1, ctx.v0);
  }
  ctx.fpscr.disableFlushModeUnconditional();
}

}  // namespace

int main() {
  Ctx ctx = {};
  ctx.fpscr.InitHost();
  ScalarCases(ctx);
  VectorCases(ctx);
  return 0;
}
