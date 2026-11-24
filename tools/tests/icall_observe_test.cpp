// S110 OBSERVE unit: the pair set and the icalls.tsv writer of src/icall_observe.h (host half), and the drift guard of
// its guest half. 0 game runs. Build/run: tools\tests\icall_observe_test.ps1.
//
// Host mode (default): same site with a second target must give a second row; a repeat gives none; a different site
// or function gives its own row; a thread's repeat is deduplicated; the file is written with the header.
// Guard mode (-DGUARD_STUB, with -DDRIFT or not): the guest half against a stub of the SDK macro text: it must compile
// with the stub equal to the copy and FAIL with -DDRIFT (one token changed in the stub). Built, it also runs the
// override: recording off = no slow path even for an alternating target; on = one slow path per change of target.
#ifdef GUARD_STUB
#include <cstdint>
struct Ctx { uint64_t lr; uint32_t last_indirect_target; };
using PPCFunc = void(Ctx&, uint8_t*);
namespace rex::runtime { inline PPCFunc* ResolveIndirectFunction(uint32_t) { return nullptr; } }
using u64 = uint64_t; using u32 = uint32_t;
#define REX_PROFILE_INDIRECT_DISPATCH()
#define REX_CODE_BASE 0x82000000u
#define REX_CODE_SIZE 0x100000u
#define REX_THUNK_RESERVE_SIZE 0x1000u
#define REX_IMAGE_BASE 0
#define REX_IMAGE_SIZE 0
#define REX_LOOKUP_FUNC(x, y) (*(PPCFunc**)(x + REX_IMAGE_BASE + REX_IMAGE_SIZE + (u64(u32(y) - REX_CODE_BASE) * 2)))
#ifdef DRIFT
#define WWE_SDK_UNLIKELY [[likely]]
#else
#define WWE_SDK_UNLIKELY [[unlikely]]
#endif
#define REX_CALL_INDIRECT_FUNC(x)                                                          \
  do {                                                                                     \
    REX_PROFILE_INDIRECT_DISPATCH();                                                       \
    uint32_t rex_indirect_target_ = (uint32_t)(x);                                         \
    PPCFunc* rex_indirect_func_;                                                           \
    if ((uint32_t)(rex_indirect_target_ - REX_CODE_BASE) <                                 \
        REX_CODE_SIZE + REX_THUNK_RESERVE_SIZE) [[likely]] {                               \
      rex_indirect_func_ = REX_LOOKUP_FUNC(base, rex_indirect_target_);                    \
    } else {                                                                               \
      rex_indirect_func_ = nullptr;                                                        \
    }                                                                                      \
    if (!rex_indirect_func_) WWE_SDK_UNLIKELY {                                            \
      ctx.last_indirect_target = rex_indirect_target_;                                     \
      rex_indirect_func_ = rex::runtime::ResolveIndirectFunction(rex_indirect_target_);    \
    }                                                                                      \
    rex_indirect_func_(ctx, base);                                                         \
  } while (0)
#include "icall_observe.h"
#include <cstdio>
// The override in use (bctrl shape), run: while recording is off (wwe2k14_icall_on == 0) a site whose target alternates
// must never leave the fast path; once on, each change of target takes the slow path; the call is made every time.
static int seen_calls = 0, dispatched = 0;
extern "C" uint32_t wwe2k14_icall_on = 0;
extern "C" void wwe2k14_icall_seen(uint32_t* last, const char*, uint32_t, uint32_t target) {
  ++seen_calls;
  __atomic_store_n(last, target, __ATOMIC_RELAXED);
}
static void Target(Ctx&, uint8_t*) { ++dispatched; }
static void Use(Ctx& ctx, uint8_t* base, uint32_t ctr) { ctx.lr = 0x82000014; REX_CALL_INDIRECT_FUNC(ctr); }
int main() {
  alignas(8) static uint8_t table[0x400] = {};
  *(PPCFunc**)(table + 0x200) = &Target;  // 0x82000100
  *(PPCFunc**)(table + 0x220) = &Target;  // 0x82000110
  Ctx ctx{};
  for (int i = 0; i < 4; ++i) Use(ctx, table, i & 1 ? 0x82000110 : 0x82000100);
  int bad = 0;
  if (seen_calls != 0) { std::printf("FAIL recording off: %d slow-path calls (want 0)\n", seen_calls); ++bad; }
  wwe2k14_icall_on = 1;
  for (int i = 0; i < 4; ++i) Use(ctx, table, i & 1 ? 0x82000110 : 0x82000100);
  if (seen_calls != 4) { std::printf("FAIL recording on: %d slow-path calls (want 4)\n", seen_calls); ++bad; }
  if (dispatched != 8) { std::printf("FAIL %d dispatches (want 8)\n", dispatched); ++bad; }
  std::printf(bad ? "icall_observe guard unit: %d problem(s)\n" : "icall_observe guard unit: 0 problems\n", bad);
  return bad;
}
#else
#define WWE2K14_ICALL_TEST 1
#include <thread>
#include "icall_observe.h"

static int bad = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++bad; } } while (0)

static int Rows(const std::filesystem::path& f) {
  FILE* h = std::fopen(f.string().c_str(), "rb");
  if (!h) return -1;
  int n = 0, c;
  while ((c = std::fgetc(h)) != EOF) n += c == '\n';
  std::fclose(h);
  return n - 1;  // minus the header
}

int main(int, char** argv) {
  namespace ic = wwe2k14::icall;
  const std::filesystem::path dir = std::filesystem::path(argv[0]).parent_path();
  CHECK(ic::Write(dir) == -1);  // recording off: nothing written
  ic::g_force = true;
  uint32_t last = 0;
  const char* fn = "__imp__sub_82250000";
  wwe2k14_icall_seen(&last, fn, 0x82250014, 0x83000000);
  CHECK(last == 0x83000000);
  CHECK(ic::Write(dir) == 1);
  wwe2k14_icall_seen(&last, fn, 0x82250014, 0x83000000);  // same pair again: no new row
  CHECK(ic::Write(dir) == 1);
  wwe2k14_icall_seen(&last, fn, 0x82250014, 0x83000010);  // PLANTED: second target at the same site
  CHECK(ic::Write(dir) == 2);
  CHECK(Rows(dir / "icalls.tsv") == 2);
  wwe2k14_icall_seen(&last, fn, 0x82250020, 0x83000000);  // another site
  std::thread([&] { wwe2k14_icall_seen(&last, fn, 0x82250014, 0x83000000); }).join();  // other thread, known pair
  wwe2k14_icall_seen(&last, "sub_82260000", 0x82260004, 0x83000000);  // plain sub_ name (a tail row: lr is a caller's)
  CHECK(ic::Write(dir) == 4);
  CHECK(Rows(dir / "icalls.tsv") == 4);
  CHECK(ic::FuncAddr("__imp__sub_822504B8") == 0x822504B8);
  std::printf(bad ? "icall_observe_test: %d problem(s)\n" : "icall_observe_test: 0 problems\n", bad);
  return bad ? 1 : 0;
}
#endif
