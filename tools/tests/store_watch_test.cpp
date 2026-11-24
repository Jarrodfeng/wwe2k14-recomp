// S110 FEEDREC unit: the watch set, slow path and stores.tsv writer of src/store_watch.h (host half), and the drift guard +
// fast path of its guest half (the 9 store-macro overrides). 0 game runs. Build/run: tools\tests\store_watch_test.ps1.
//
// Host mode (default): a store to an unwatched page records nothing; a watched one records one row; the same store twice
// is one row; a read noted by feed_seen.h at a new address adds it to the watch set; alias views (0x9, 0xC / 0xE) of a
// watched address hit; an unaligned store crossing into a watched page hits; width 0 (REX_RAW_ADDR) rows keep width 0.
// Guard mode (-DGUARD_STUB, with -DDRIFT or not): the guest half against stubs of the SDK macro text: it must compile
// with the stubs equal to the copies and FAIL with -DDRIFT (one token changed in REX_STORE_U32's stub). Built, it runs
// the overrides: an unwatched page never calls the slow path, a watched one does, and the memory is still written.
#ifdef GUARD_STUB
#include <cstdint>
#include <cstdio>
using u8 = uint8_t; using u16 = uint16_t; using u32 = uint32_t; using u64 = uint64_t;
namespace rex::runtime {
struct MMIOHandler {
  static MMIOHandler* global_handler() { static MMIOHandler h; return &h; }
  void CheckStore(uint32_t, uint32_t) {}
};
}  // namespace rex::runtime
#define REX_PHYS_HOST_OFFSET(addr) 0u
#define REX_IS_MMIO_ADDR(addr) ((addr) >= 0x7F000000u && (addr) < 0x80000000u)
#define REX_RAW_ADDR(x) (base + (u32)(x) + REX_PHYS_HOST_OFFSET(x))
#define REX_STORE_U8(x, y) (*(volatile u8*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = (y))
#define REX_STORE_U16(x, y) (*(volatile u16*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap16(y))
#ifdef DRIFT
#define REX_STORE_U32(x, y) (*(volatile u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap16(y))
#else
#define REX_STORE_U32(x, y) (*(volatile u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap32(y))
#endif
#define REX_STORE_U64(x, y) (*(volatile u64*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap64(y))
#define REX_MM_STORE_U8(addr, val)                                                         \
  do {                                                                                     \
    u32 _mmio_addr = (addr);                                                               \
    if (REX_IS_MMIO_ADDR(_mmio_addr)) {                                                    \
      rex::runtime::MMIOHandler::global_handler()->CheckStore(_mmio_addr,                  \
                                                              static_cast<u32>(val));      \
    } else {                                                                               \
      *(volatile u8*)(base + _mmio_addr + REX_PHYS_HOST_OFFSET(_mmio_addr)) = (val);       \
    }                                                                                      \
  } while (0)
#define REX_MM_STORE_U16(addr, val)                                                        \
  do {                                                                                     \
    u32 _mmio_addr = (addr);                                                               \
    if (REX_IS_MMIO_ADDR(_mmio_addr)) {                                                    \
      rex::runtime::MMIOHandler::global_handler()->CheckStore(_mmio_addr,                  \
                                                              static_cast<u32>(val));      \
    } else {                                                                               \
      *(volatile u16*)(base + _mmio_addr + REX_PHYS_HOST_OFFSET(_mmio_addr)) =             \
          __builtin_bswap16(val);                                                          \
    }                                                                                      \
  } while (0)
#define REX_MM_STORE_U32(addr, val)                                                        \
  do {                                                                                     \
    u32 _mmio_addr = (addr);                                                               \
    if (REX_IS_MMIO_ADDR(_mmio_addr)) {                                                    \
      rex::runtime::MMIOHandler::global_handler()->CheckStore(_mmio_addr,                  \
                                                              static_cast<u32>(val));      \
    } else {                                                                               \
      *(volatile u32*)(base + _mmio_addr + REX_PHYS_HOST_OFFSET(_mmio_addr)) =             \
          __builtin_bswap32(val);                                                          \
    }                                                                                      \
  } while (0)
#define REX_MM_STORE_U64(addr, val)                                                               \
  do {                                                                                            \
    u32 _mmio_addr = (addr);                                                                      \
    if (REX_IS_MMIO_ADDR(_mmio_addr)) {                                                           \
      u64 _v64 = static_cast<u64>(val);                                                           \
      rex::runtime::MMIOHandler::global_handler()->CheckStore(_mmio_addr,                         \
                                                              static_cast<u32>(_v64 >> 32));      \
      rex::runtime::MMIOHandler::global_handler()->CheckStore(_mmio_addr + 4,                     \
                                                              static_cast<u32>(_v64));            \
    } else {                                                                                      \
      *(volatile u64*)(base + _mmio_addr + REX_PHYS_HOST_OFFSET(_mmio_addr)) =                    \
          __builtin_bswap64(val);                                                                 \
    }                                                                                             \
  } while (0)
// The generated functions' context: the check reads ctx.lr (S110 COPIER).
static struct { uint64_t lr = 0; } ctx;
#include "store_watch.h"

static int hits = 0;
extern "C" uint32_t wwe2k14_stw_bits[32768] = {};
extern "C" uint64_t* wwe2k14_stw_mask[1u << 20] = {};
extern "C" uint32_t wwe2k14_feedrec_on = 0;
// The stub keeps the real contract: the slow path stores (lr << 32 | address) in the site's word once it has recorded it.
extern "C" void wwe2k14_stw_hit(uint64_t* last, const char*, uint32_t, uint32_t addr, uint32_t, uint32_t lr) {
  ++hits;
  *last = ((uint64_t)lr << 32) | addr;
  wwe2k14_stw_install((uint32_t)(uintptr_t)last, addr, lr);  // S110 STW3: and the inline recorded-cache, as the real slow path
}
extern "C" void wwe2k14_frc_register(void* block) { static_cast<wwe2k14::frc::Block*>(block)->reg = 1; }  // the guard unit has no registry

// All 9 forms, as the generated code writes them.
static void Use(uint8_t* base, uint32_t a) {
  REX_STORE_U8(a, 1);
  REX_STORE_U16(a + 2, 0x1234);
  REX_STORE_U32(a + 4, 0xAABBCCDDu);
  REX_STORE_U64(a + 8, 0x1122334455667788ull);
  REX_MM_STORE_U8(a + 16, 7);
  REX_MM_STORE_U16(a + 18, 0x0102);
  REX_MM_STORE_U32(a + 20, 0x03040506u);
  REX_MM_STORE_U64(a + 24, 0x0102030405060708ull);
  *(uint32_t*)REX_RAW_ADDR(a + 32) = 5;
}
static void One(uint8_t* base, uint32_t a) { REX_STORE_U32(a, 0x01020304u); }
static void Wide(uint8_t* base, uint32_t a) { REX_STORE_U64(a, 0x0102030405060708ull); }
static void Alt(uint8_t* base, uint32_t a) { REX_STORE_U32(a, 0x0A0B0C0Du); }  // one site
static void Raw(uint8_t* base, uint32_t a) { *(uint32_t*)REX_RAW_ADDR(a) = 7; }
static void Fixed(uint8_t* base) { REX_STORE_U32(0x100u, 1u); }  // one site, one address: only ctx.lr varies
int main() {
  static uint8_t mem[0x3000] = {};
  int bad = 0;
  uint8_t* base = mem;
  Use(base, 0x100);
  if (hits != 0) { std::printf("FAIL unwatched page: %d slow-path calls (want 0)\n", hits); ++bad; }
  if (mem[0x100] != 1 || mem[0x102] != 0x12 || mem[0x104] != 0xAA || mem[0x108] != 0x11 || mem[0x110] != 7 || mem[0x114] != 3 ||
      mem[0x118] != 1 || *(uint32_t*)(mem + 0x120) != 5) { std::printf("FAIL the overrides changed what is stored: %02X %02X %02X %02X %02X %02X %02X %08X\n", mem[0x100], mem[0x102], mem[0x104], mem[0x108], mem[0x110], mem[0x114], mem[0x118], *(uint32_t*)(mem + 0x120)); ++bad; }
  wwe2k14_stw_bits[0x100 >> 17] |= 1u << ((0x100 >> 12) & 31);   // page 0 watched
  static uint64_t m0[kWweStwMaskWords] = {}, m1[kWweStwMaskWords] = {};
  m0[4] = m0[5] = m0[6] = m0[7] = ~0ull;  // byte bits 256..511: bytes 0x100..0x1FF are watched (S110 STW4: one bit per byte); the rest is not
  wwe2k14_stw_mask[0] = m0;
  Use(base, 0x100);
  if (hits != 9) { std::printf("FAIL watched page: %d slow-path calls (want 9, one per store form)\n", hits); ++bad; }
  // the per-site memo: the same stores at the same addresses again never reach the slow path; another address does, once
  Use(base, 0x100);
  if (hits != 9) { std::printf("FAIL repeated stores: %d slow-path calls (want still 9)\n", hits); ++bad; }
  Use(base, 0x140);
  if (hits != 18) { std::printf("FAIL a second address: %d slow-path calls (want 18, each site once more)\n", hits); ++bad; }
  Use(base, 0x140);
  if (hits != 18) { std::printf("FAIL the second address again: %d slow-path calls (want 18)\n", hits); ++bad; }
  // the memo matches on (address AND lr): the same address from another caller reaches the slow path, once
  ctx.lr = 0x82001234u;
  Use(base, 0x140);
  if (hits != 27) { std::printf("FAIL a new lr at a known address: %d slow-path calls (want 27)\n", hits); ++bad; }
  Use(base, 0x140);
  if (hits != 27) { std::printf("FAIL the new lr again: %d slow-path calls (want still 27)\n", hits); ++bad; }
  // S110 STW2: the second level. A store on the watched page but in an unwatched cell makes no call (all 9 forms, raw included).
  Use(base, 0x300);
  if (hits != 27) { std::printf("FAIL watched page, unwatched cells: %d calls (want still 27)\n", hits); ++bad; }
  // a store overlapping a watched cell does
  One(base, 0x1F8);
  if (hits != 28) { std::printf("FAIL a watched cell: %d calls (want 28)\n", hits); ++bad; }
  // two cells, only the second watched: cell 65 (0x208..0x20F) set, cell 64 not; a 4-byte store at 0x206 covers both
  One(base, 0x206);
  if (hits != 28) { std::printf("FAIL cells 64,65 both unwatched: %d calls (want 28)\n", hits); ++bad; }
  m0[8] = 0xFF00ull;  // bytes 0x208..0x20F
  One(base, 0x206);
  if (hits != 29) { std::printf("FAIL two cells, only the second watched: %d calls (want 29)\n", hits); ++bad; }
  // a store clear of the watched cell is not hit
  One(base, 0x210);
  if (hits != 29) { std::printf("FAIL a store clear of the watched cell: %d calls (want 29)\n", hits); ++bad; }
  // across the page end: page 0 has its bit, page 1 has a mask with cell 0 watched; an 8-byte store at 0xFFC reaches it
  m1[0] = 1;
  wwe2k14_stw_mask[1] = m1;
  Wide(base, 0xFF0);
  if (hits != 29) { std::printf("FAIL a store ending before the next page: %d calls (want 29)\n", hits); ++bad; }
  Wide(base, 0xFFC);
  if (hits != 30) { std::printf("FAIL a store crossing into a watched cell of the next page: %d calls (want 30)\n", hits); ++bad; }
  // S110 STW3: one site storing to two watched addresses alternately: the per-site memo holds one word and misses every time;
  // the inline recorded-cache holds both, so only the first store at each address calls out (2 calls in 100 stores).
  const int before_alt = hits;
  const uint64_t inl_before = wwe2k14::frc::tl_block.n[wwe2k14::frc::S_INL_HIT] + wwe2k14::frc::tl_block.n[wwe2k14::frc::S_MEMO_HIT];
  for (int i = 0; i < 100; ++i) Alt(base, (i & 1) ? 0x108u : 0x100u);
  if (hits != before_alt + 2) { std::printf("FAIL alternating two watched addresses: %d calls (want 2)\n", hits - before_alt); ++bad; }
  {
    const wwe2k14::frc::Block& blk = wwe2k14::frc::tl_block;
    // the other 98 stores returned inline: the memo holds the address of the last call, the cache the other one
    const uint64_t inl_after = blk.n[wwe2k14::frc::S_INL_HIT] + blk.n[wwe2k14::frc::S_MEMO_HIT];
    if (inl_after - inl_before != 98) { std::printf("FAIL inline answers %llu (want 98)\n", (unsigned long long)(inl_after - inl_before)); ++bad; }
    // the page-bit hits split into cell misses, memo hits, cache hits and calls
    const uint64_t* n = blk.n;
    const uint32_t ro = wwe2k14::frc::kRawOff;
    // (S_OUT is counted by the real slow path only; the stub's own hit count stands in for it)
    if (n[wwe2k14::frc::S_PAGE_HIT] + n[wwe2k14::frc::S_PAGE_HIT + ro] != n[wwe2k14::frc::S_CELL_MISS] + n[wwe2k14::frc::S_CELL_MISS + ro] +
            n[wwe2k14::frc::S_MEMO_HIT] + n[wwe2k14::frc::S_MEMO_HIT + ro] + n[wwe2k14::frc::S_INL_HIT] + n[wwe2k14::frc::S_INL_HIT + ro] + (uint64_t)hits) {
      std::printf("FAIL the inline counters do not add up\n"); ++bad;
    }
  }
  // S110 STW4: BYTE-exact. Cell 0x240..0x247 holds one watched byte, 0x244: a 4-byte store at 0x240 (bytes 0x240..0x243) is answered
  // inline (the cell mask of STW2 called out); one at 0x244 calls. A width-0 access tests the 128 bytes from its address.
  {
    m0[9] = 1ull << 4;  // byte 0x244 = bit 580 = word 9, bit 4
    const int h0 = hits;
    One(base, 0x240);
    if (hits != h0) { std::printf("FAIL watched cell, unwatched bytes: %d calls (want 0)\n", hits - h0); ++bad; }
    One(base, 0x244);
    if (hits != h0 + 1) { std::printf("FAIL the watched byte: %d calls (want 1)\n", hits - h0); ++bad; }
    Raw(base, 0x245);  // 0x245..0x2C4: nothing watched
    if (hits != h0 + 1) { std::printf("FAIL a raw window clear of every watched byte: %d calls (want 1)\n", hits - h0); ++bad; }
    Raw(base, 0x244);
    if (hits != h0 + 2) { std::printf("FAIL a raw window starting on the watched byte: %d calls (want 2)\n", hits - h0); ++bad; }
  }
  // S110 STW4: the inline recorded-cache holds a run's rows. 100,000 distinct (site, address, lr) keys stored twice: the second
  // pass is answered inline, so about 100,000 calls, not 200,000 (a few evictions of a 3rd key in a set are allowed: <= 2%).
  {
    const int h0 = hits;
    for (int pass = 0; pass < 2; ++pass)
      for (uint32_t i = 0; i < 100000u; ++i) { ctx.lr = 0x82000000u + i * 4u; Fixed(base); }
    const int calls = hits - h0;
    if (calls < 100000 || calls > 102000) { std::printf("FAIL 100000 keys stored twice: %d calls (want 100000 .. 102000)\n", calls); ++bad; }
  }
  std::printf(bad ? "store_watch guard unit: %d problem(s)\n" : "store_watch guard unit: 0 problems\n", bad);
  return bad;
}
#else
#define WWE2K14_STW_TEST 1
#define WWE2K14_FEEDSEEN_TEST 1
#include "store_watch.h"
#include "feed_seen.h"
#include <thread>

static int bad = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++bad; } } while (0)

// The slow path takes the call site's memo word first; the cases below that do not look at it share this one.
static uint64_t g_site_word = 0;
#define wwe2k14_stw_hit(f, l, a, w) wwe2k14_stw_hit(&g_site_word, f, l, a, w, 0u)

static bool Bit(uint32_t a) { return (wwe2k14_stw_bits[a >> 17] >> ((a >> 12) & 31)) & 1u; }
static bool Byte(uint32_t a) {  // S110 STW4: the page's mask has this BYTE's bit
  const uint64_t* m = wwe2k14_stw_mask[a >> 12];
  return m && ((m[(a & 0xFFF) >> 6] >> (a & 63)) & 1u);
}
static int Rows(const std::filesystem::path& f) {
  FILE* h = std::fopen(f.string().c_str(), "rb");
  if (!h) return -1;
  int n = 0, c;
  while ((c = std::fgetc(h)) != EOF) n += c == '\n';
  std::fclose(h);
  return n - 1;  // minus the header
}
static std::string Slurp(const std::filesystem::path& f) {
  std::string s;
  if (FILE* h = std::fopen(f.string().c_str(), "rb")) {
    char b[512];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, h)) > 0) s.append(b, n);
    std::fclose(h);
  }
  return s;
}
static const char* kF1 = "__imp__sub_82250000";
static const char* kF2 = "__imp__sub_82260000";

int main() {
  namespace sw = wwe2k14::storewatch;
  const std::filesystem::path dir = std::filesystem::current_path() / "stw_out";
  std::filesystem::create_directories(dir);

  // recording off: Add refuses, no page is marked, hit records nothing
  sw::Add(0x82000100, 4);
  CHECK(!Bit(0x82000100));
  wwe2k14_stw_hit(kF1, 10, 0x82000100, 4);
  CHECK(sw::Write(dir) == -1);

  sw::ForceOn();
  // a store to an unwatched page records nothing (the fast path never leaves; the slow path refuses the range test)
  CHECK(!Bit(0x84000000));
  wwe2k14_stw_hit(kF1, 11, 0x84000000, 4);
  CHECK(sw::Write(dir) == 0);

  // a watched address: one row; the same store twice is still one row; another line is another row
  sw::Add(0x82000100, 4);
  CHECK(Bit(0x82000100));
  CHECK(!Bit(0x82002000));
  // S110 STW2: the cell mask of the page: the watched cell is set, the next one and a far one are not; Add sets more later
  CHECK(Byte(0x82000100) && Byte(0x82000103) && !Byte(0x82000104) && !Byte(0x820000FF) && !Byte(0x82000200));
  CHECK(!Byte(0x82000800));
  sw::Add(0x82000800, 4);
  CHECK(Byte(0x82000800) && Byte(0x82000803) && Byte(0x82000100));  // a later Add sets its bytes (the cached-no-overlap case below)
  sw::Add(0x82000C3E, 130);  // a range over three mask words: every byte of it, none outside
  CHECK(!Byte(0x82000C3D) && Byte(0x82000C3E) && Byte(0x82000C80) && Byte(0x82000CBF) && !Byte(0x82000CC0));
  wwe2k14_stw_hit(kF1, 20, 0x82000100, 4);
  wwe2k14_stw_hit(kF1, 20, 0x82000100, 4);
  CHECK(sw::Write(dir) == 1);
  wwe2k14_stw_hit(kF1, 21, 0x82000100, 4);
  wwe2k14_stw_hit(kF2, 20, 0x82000102, 2);  // inside the range: its own (func, line, addr)
  CHECK(sw::Write(dir) == 3);
  CHECK(Rows(dir / "stores.tsv") == 3);
  // a store on the watched page but outside the range records nothing
  wwe2k14_stw_hit(kF1, 22, 0x82000200, 4);
  CHECK(sw::Write(dir) == 3);
  // the exact test: a store ending right before the range, and one starting at its end
  wwe2k14_stw_hit(kF1, 23, 0x820000FC, 4);
  wwe2k14_stw_hit(kF1, 24, 0x82000104, 4);
  CHECK(sw::Write(dir) == 3);
  wwe2k14_stw_hit(kF1, 25, 0x820000FE, 4);  // overlaps the first two bytes
  CHECK(sw::Write(dir) == 4);

  // aliases: 0x8 and 0x9 share pages; 0xA / 0xC / 0xE are views of the same memory
  sw::Add(0x82005000, 4);
  CHECK(Bit(0x92005000));
  wwe2k14_stw_hit(kF1, 30, 0x92005000, 4);
  // the physical views show physical p at 0xA0000000 + p, 0xC0000000 + p and 0xE0000000 + p - 0x1000 (the 0xE view sits at
  // file offset 0x1000: generated wwe2k14_pch.h:123-128, rex/system/xmemory.h:31), each over 512 MB (0xB / 0xD / 0xF too)
  sw::Add(0xA0006000, 4);
  CHECK(Bit(0xC0006000) && Bit(0xE0005000));
  wwe2k14_stw_hit(kF1, 31, 0xE0005000, 4);
  CHECK(sw::Write(dir) == 6);
  wwe2k14_stw_hit(kF1, 32, 0xE0006000, 4);  // physical 0x7000: the next 4 KB, not the watched word
  CHECK(sw::Write(dir) == 6);
  sw::Add(0xF0007000, 4);  // physical 0x10008000 (the upper half of each view)
  CHECK(Bit(0xB0008000) && Bit(0xD0008000));
  wwe2k14_stw_hit(kF1, 33, 0xB0008000, 4);
  CHECK(sw::Write(dir) == 7);

  // page edge: a range 2 bytes into a page also marks the page before (an unaligned store crossing in)
  sw::Add(0x83001002, 4);
  CHECK(Bit(0x83001000) && Bit(0x83000000));
  wwe2k14_stw_hit(kF1, 40, 0x83000FFC, 8);  // 0x83000FFC..0x83001003: reaches the range
  CHECK(sw::Write(dir) == 8);

  // width 0 (REX_RAW_ADDR): the row keeps width 0
  sw::Add(0x84000040, 4);
  wwe2k14_stw_hit(kF2, 50, 0x84000040, 0);
  CHECK(sw::Write(dir) == 9);
  CHECK(Slurp(dir / "stores.tsv").find("0x82260000\t50\t0x84000040\t0\t0x00000000\n") != std::string::npos);

  // a read noted by feed_seen.h at a new address adds it to the watch set; a store to it then records
  CHECK(!Bit(0x85000000));
  wwe2k14::feedseen::Note(std::source_location::current(), 0x85000010, 4);
  CHECK(Bit(0x85000000));
  wwe2k14_stw_hit(kF1, 60, 0x85000010, 4);
  CHECK(sw::Write(dir) == 10);

  // the site word: set once the store is known to overlap a watched range; left alone by one that does not
  uint64_t word = 0;
  (wwe2k14_stw_hit)(&word, kF1, 70, 0x85000200, 4, 0);  // watched page, outside the range
  CHECK(word == 0);
  (wwe2k14_stw_hit)(&word, kF1, 70, 0x85000010, 4, 0);
  CHECK(word == 0x85000010);
  // the thread-local cache never hides a range that grew: a cached "no overlap" is dropped when Add runs
  word = 0;
  (wwe2k14_stw_hit)(&word, kF1, 71, 0x85000300, 4, 0);
  (wwe2k14_stw_hit)(&word, kF1, 71, 0x85000300, 4, 0);
  CHECK(word == 0 && sw::Write(dir) == 11);  // (line 70 above added one row)
  sw::Add(0x85000300, 4);
  (wwe2k14_stw_hit)(&word, kF1, 71, 0x85000300, 4, 0);
  CHECK(word == 0x85000300 && sw::Write(dir) == 12);

  // S110 COPIER: lr. Two stores to one address from one site with different lr are two rows (one of each lr); the same lr again
  // is not a third; the site word carries the lr, so the fast path's (lr, addr) compare lets a new lr through.
  word = 0;
  (wwe2k14_stw_hit)(&word, kF1, 80, 0x85000010, 4, 0x82001234u);
  CHECK(word == ((0x82001234ull << 32) | 0x85000010u));
  (wwe2k14_stw_hit)(&word, kF1, 80, 0x85000010, 4, 0x82005678u);
  (wwe2k14_stw_hit)(&word, kF1, 80, 0x85000010, 4, 0x82001234u);
  CHECK(word == ((0x82001234ull << 32) | 0x85000010u) && sw::Write(dir) == 14);
  CHECK(Slurp(dir / "stores.tsv").find("0x82250000\t80\t0x85000010\t4\t0x82005678\n") != std::string::npos);
  CHECK(Slurp(dir / "stores.tsv").find("0x82250000\t80\t0x85000010\t4\t0x82001234\n") != std::string::npos);
  CHECK(sw::LrDropped() == 0 && Slurp(dir / "stores.tsv").find("lr_dropped") == std::string::npos);
  // the cap: 16 distinct lr per (func, line, addr); the rest are counted, not kept
  for (uint32_t i = 1; i <= 20; ++i) (wwe2k14_stw_hit)(&word, kF1, 81, 0x85000010, 4, 0x82100000u + i * 4);
  CHECK(sw::Write(dir) == 14 + 16);
  CHECK(sw::LrDropped() == 4);
  CHECK(Slurp(dir / "stores.tsv").find("# lr_dropped 4\n") != std::string::npos);

  // S110 STW3: the inline recorded-cache. A recorded (site, address, lr) is installed (the site is the word's address); a store
  // that overlaps no range is not.
  {
    namespace frc = wwe2k14::frc;
    auto cached = [](const uint64_t* site_word, uint32_t addr, uint32_t lr) {
      return wwe2k14_stw_cached((uint32_t)(uintptr_t)site_word, addr, lr);
    };
    uint64_t site_a = 0, site_b = 0;
    (wwe2k14_stw_hit)(&site_a, kF1, 90, 0x85000010, 4, 0x82001234u);
    (wwe2k14_stw_hit)(&site_b, kF1, 91, 0x85000200, 4, 0x82001234u);  // watched page, outside the range
    CHECK(cached(&site_a, 0x85000010, 0x82001234u));
    CHECK(!cached(&site_b, 0x85000200, 0x82001234u));
    CHECK(!cached(&site_a, 0x85000010, 0x82009999u));  // another lr is another key
    // 600 rows read twice through the 256-entry thread cache: the second pass finds evicted rows in the thread's row set
    uint64_t site_loop = 0;
    for (int pass = 0; pass < 2; ++pass)
      for (uint32_t ln = 1000; ln < 1600; ++ln) (wwe2k14_stw_hit)(&site_loop, kF2, ln, 0x85000010, 4, 0x82001234u);
    // the counters add up: every call of the slow path is exactly one of the eight answers, raw (width 0) counted apart
    uint64_t c[frc::kCount];
    frc::Fold(c);
    for (uint32_t ro : {0u, (uint32_t)frc::kRawOff})
      CHECK(c[frc::S_OUT + ro] == c[frc::S_OUT_OFF + ro] + c[frc::S_OUT_CACHE_HIT + ro] + c[frc::S_OUT_NOOV_CACHED + ro] +
                                      c[frc::S_OUT_NOOV + ro] + c[frc::S_OUT_THREAD_SET + ro] + c[frc::S_OUT_GLOBAL_SET + ro] +
                                      c[frc::S_OUT_NEW + ro] + c[frc::S_OUT_DROP + ro]);
    CHECK(c[frc::S_OUT_OFF] == 1);
    CHECK(c[frc::S_OUT_DROP] == 4);
    CHECK(c[frc::S_OUT_NEW] > 0);
    CHECK(c[frc::S_OUT_NOOV] > 0);
    CHECK(c[frc::S_OUT_THREAD_SET] > 0);
    CHECK(c[frc::S_OUT_CACHE_HIT] == 0 && c[frc::S_OUT_NOOV_CACHED] == 0);  // STW4: the 256-entry range cache is gone
    CHECK(c[frc::S_OUT + frc::kRawOff] == 1 && c[frc::S_OUT_NEW + frc::kRawOff] == 1);  // the one width-0 call above
    // the rows mutex is taken exactly when the thread's own set did not have the row
    CHECK(c[frc::S_LOCK_ROWS] == c[frc::S_OUT_NEW] + c[frc::S_OUT_GLOBAL_SET] + c[frc::S_OUT_DROP]);
    // S110 STW4: no ranges lock: the whole scripted sequence above (Adds, no concurrent Add, hundreds of range tests) took none
    CHECK(c[frc::S_LOCK_RANGES] == 0 && c[frc::S_LOCK_RANGES + frc::kRawOff] == 0);
    // a thread's counts survive its exit; its first slow-path call registers its block
    uint64_t site_c = 0, d[frc::kCount];
    std::thread([&] { (wwe2k14_stw_hit)(&site_c, kF1, 92, 0x85000010, 4, 0x82001234u); }).join();
    frc::Fold(d);
    CHECK(d[frc::S_OUT] == c[frc::S_OUT] + 1);
    // the file: name<TAB>value, the same numbers
    CHECK(sw::Write(dir) >= 0);
    const std::string t = Slurp(dir / "feedrec_counts.tsv");
    CHECK(t.find("store_out_calls\t" + std::to_string((unsigned long long)d[frc::S_OUT]) + "\n") != std::string::npos);
    CHECK(t.find("raw_out_new_row\t1\n") != std::string::npos);
    CHECK(t.find("read_inline_capped\t") != std::string::npos);
  }

  std::printf(bad ? "store_watch_test: %d problem(s)\n" : "store_watch_test: 0 problems\n", bad);
  return bad;
}
#endif
