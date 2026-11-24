// S110 FEEDREC unit: the read recorder of src/feed_seen.h. 0 game runs. Build/run: tools\tests\feed_seen_test.ps1.
// Recording off records nothing; a read at a new (file:line, addr) is one row and calls storewatch::Add once; the same read
// twice is one row; one site reading more than the cap keeps 64 addresses and counts the rest; the reader type notes the
// read at its CALLER's line and still reads; the writer gives `file line addr width n_addrs_dropped`.
#define WWE2K14_FEEDSEEN_TEST 1
#include "feed_seen.h"
#include <atomic>
#include <chrono>
#include <future>
#include <thread>

static int bad = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++bad; } } while (0)

extern "C" uint32_t wwe2k14_feedrec_on = 0;
static int adds = 0;
static uint32_t last_add = 0, last_width = 0;
namespace wwe2k14::storewatch {
void Add(uint32_t addr, uint32_t width) { ++adds, last_add = addr, last_width = width; }
}  // namespace wwe2k14::storewatch

static bool Leaf(uint32_t, void* dst, size_t n) { std::memset(dst, 0xAB, n); return true; }
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
static int Rows(const std::string& s) { return (int)std::count(s.begin(), s.end(), '\n') - 1; }

// A helper that forwards the caller's location, as the production helpers do.
static bool Word(wwe2k14::feedseen::ReadFnT read, uint32_t a, uint32_t& v WWE_FEED_LOC_PARAM) {
  uint32_t raw = 0;
  if (!read(a, &raw, 4 WWE_FEED_LOC_ARG)) return false;
  v = raw;
  return true;
}

int main() {
  namespace fs = wwe2k14::feedseen;
  const std::filesystem::path dir = std::filesystem::current_path() / "feed_out";
  std::filesystem::create_directories(dir);
  fs::ReadFnT read = &Leaf;
  CHECK(read);
  CHECK(!fs::ReadFnT(nullptr));

  // off: the read happens, nothing is recorded
  uint32_t v = 0;
  CHECK(Word(read, 0x82000010, v));
  CHECK(v == 0xABABABABu && adds == 0 && fs::Write(dir) == -1);

  __atomic_store_n(&wwe2k14_feedrec_on, 1u, __ATOMIC_RELAXED);
  // a new (site, addr): one row, one Add with the width
  CHECK(Word(read, 0x82000010, v));  // site A
  CHECK(adds == 1 && last_add == 0x82000010 && last_width == 4);
  CHECK(Word(read, 0x82000010, v));  // site B (another line): the helper's CALLER line is the key
  CHECK(adds == 2);
  for (int i = 0; i < 3; ++i) { uint32_t w; read(0x82000020, &w, 4); }  // one site, one address, three times: one row
  CHECK(adds == 3);
  CHECK(fs::Write(dir) == 3);
  std::string t = Slurp(dir / "feed_reads.tsv");
  CHECK(Rows(t) == 3);
  CHECK(t.find("feed_seen_test.cpp") != std::string::npos);
  CHECK(t.find("\t0x82000020\t4\t0\n") != std::string::npos);

  // the cap: one site reading 70 distinct addresses keeps 64 and counts the 6 it dropped
  adds = 0;
  for (uint32_t i = 0; i < 70; ++i) { uint32_t w; read(0x83000000 + 4 * i, &w, 4); }
  CHECK(adds == 64);
  CHECK(fs::Write(dir) == 3 + 64);
  t = Slurp(dir / "feed_reads.tsv");
  CHECK(t.find("\t0x83000000\t4\t1\n") != std::string::npos);  // S110 STW2: only the first drop is counted (>= 1 = capped); the dropped column rides on every row of the site

  // a site over its cap does no lookup and takes no lock; one dropped number per site across threads.
  // Site C: 70 distinct addresses (64 kept, 6 dropped); then ANOTHER thread, with the lock held here, reads 101 more.
  const std::source_location loc = std::source_location::current();
  for (uint32_t i = 0; i < 70; ++i) fs::Note(loc, 0x84000000 + 4 * i, 4);
  std::promise<void> primed, done;
  std::future<void> primed_f = primed.get_future(), done_f = done.get_future();
  std::atomic<bool> go{false};
  std::thread other([&] {
    fs::Note(loc, 0x85000000, 4);  // this thread's first read at the site: one lookup (counted as a drop)
    primed.set_value();
    while (!go.load()) std::this_thread::yield();
    for (uint32_t i = 0; i < 100; ++i) fs::Note(loc, 0x85001000 + 4 * i, 4);
    done.set_value();
  });
  primed_f.wait();
  {
    std::lock_guard<std::mutex> hold(fs::G().mutex);
    go.store(true);
    CHECK(done_f.wait_for(std::chrono::seconds(2)) == std::future_status::ready);  // a capped site never takes the lock
  }
  other.join();
  CHECK(fs::Write(dir) == 3 + 64 + 64);
  t = Slurp(dir / "feed_reads.tsv");
  // S110 STW2: the other thread's first read is inline-capped (no call, no count); only the drop that capped the site is counted
  CHECK(t.find("\t0x84000000\t4\t1\n") != std::string::npos);
  // a capped site makes NO out-of-line call, however many reads: counted through the test hook
  const uint64_t calls = fs::g_out_calls;
  for (uint32_t i = 0; i < 1000; ++i) fs::Note(loc, 0x86000000 + 4 * i, 4);
  CHECK(fs::g_out_calls == calls);
  // a capped site evicted from the table by another capped site goes out once and is put back
  const uint64_t key = fs::CapKey(loc.file_name(), (uint32_t)loc.line());
  fs::g_capped_tab[fs::CapSlot(key)] = 0;
  fs::Note(loc, 0x87000000, 4);
  fs::Note(loc, 0x87000004, 4);
  CHECK(fs::g_out_calls == calls + 1);

  // S110 STW3: an UNCAPPED site reading the same 10 addresses twice makes 10 out-of-line calls, not 20 (the old last-address memo
  // missed on every read of the second pass); the inline recorded-cache answers the second pass.
  namespace frc = wwe2k14::frc;
  uint64_t before[frc::kCount], after[frc::kCount];
  frc::Fold(before);
  const std::source_location loc10 = std::source_location::current();
  const uint64_t calls10 = fs::g_out_calls;
  const int adds10 = adds;
  for (int pass = 0; pass < 2; ++pass)
    for (uint32_t i = 0; i < 10; ++i) fs::Note(loc10, 0x88000000 + 4 * i, 4);
  CHECK(fs::g_out_calls == calls10 + 10);
  CHECK(adds == adds10 + 10);  // each new address reached the watch set once
  frc::Fold(after);
  CHECK(after[frc::R_OUT] - before[frc::R_OUT] == 10);
  CHECK(after[frc::R_INL_HIT] - before[frc::R_INL_HIT] == 10);
  CHECK(after[frc::R_OUT_NEW] - before[frc::R_OUT_NEW] == 10);
  CHECK(after[frc::R_LOCK_SITES] - before[frc::R_LOCK_SITES] == 10);
  // the counters add up over the whole run: every out-of-line call is exactly one of the six answers
  CHECK(after[frc::R_OUT] == after[frc::R_OUT_REINSTALL] + after[frc::R_OUT_MEMO] + after[frc::R_OUT_THREAD_SET] +
                                 after[frc::R_OUT_GLOBAL_SET] + after[frc::R_OUT_NEW] + after[frc::R_OUT_DROP]);
  CHECK(after[frc::R_OUT] == fs::g_out_calls);  // the test hook counts the same calls
  CHECK(after[frc::R_INL_CAPPED] >= 1000 && after[frc::R_OUT_DROP] == 2 && after[frc::R_OUT_REINSTALL] >= 1);  // one drop per capped site: later reads of it return inline
  // a capped read that comes from a thread that never went out of line still reaches the folded total (registered on first use)
  // and a thread's counts survive its exit
  const uint64_t cap_before = after[frc::R_INL_CAPPED];
  std::thread cap_thread([&] { for (int i = 0; i < 5; ++i) fs::Note(loc, 0x89000000 + 4 * i, 4); });
  cap_thread.join();
  frc::Fold(after);
  CHECK(after[frc::R_INL_CAPPED] == cap_before + 5);
  CHECK(frc::WriteCounts(dir) == (int)frc::kCount);
  CHECK(Slurp(dir / "feedrec_counts.tsv").find("read_inline_cache_hit\t") != std::string::npos);

  std::printf(bad ? "feed_seen_test: %d problem(s)\n" : "feed_seen_test: 0 problems\n", bad);
  return bad;
}
