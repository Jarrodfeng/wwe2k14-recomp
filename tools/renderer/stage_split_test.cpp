// S102 R21-B (docs\renderer\STAGE_SPLIT.md): host test of the shipped decision (src\renderer_adapter\stage_split.h Decide) and of the append-only table
// (src\renderer\stable_table.h) the two stages share. No GPU.
//   stage_split_test            -> exit 0, every line PASS
//   stage_split_test no_fallback -> the twin split_no_fallback (a tween frame goes through the split): the decision table must FAIL (exit 1)
// The table stress: one writer appends 60000 entries (each holding its own index) while two readers read every published entry by id; an entry seen
// with the wrong content is a torn publish. (The racy std::vector twin is not run: it would crash rather than fail; tools\renderer\stage_split_lint.py
// fails a tree whose programs / layouts are vectors again.)
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "../../src/renderer/stable_table.h"
#include "../../src/renderer_adapter/stage_split.h"

namespace ss = rdr_adapter::stage_split;

static int fails = 0;
static void Say(bool ok, const char* text) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", text);
  if (!ok) ++fails;
}

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "no_fallback") == 0) ss::g_decide_plant.store(1);
  // The decision table: every reason that reads the renderer's result sends the frame to the one-thread path.
  struct Row {
    const char* name;
    ss::Inputs in;
    int want;
  };
  const auto with = [](void (*set)(ss::Inputs&)) {
    ss::Inputs in;
    in.on = true;
    set(in);
    return in;
  };
  const Row rows[] = {
      {"switch off: not split, not counted", ss::Inputs{}, -2},
      {"plain frame: split", with([](ss::Inputs&) {}), -1},
      {"tween: sync", with([](ss::Inputs& i) { i.tween = true; }), ss::kFbTween},
      {"dlss: sync", with([](ss::Inputs& i) { i.dlss = true; }), ss::kFbDlss},
      {"named picture / png: sync", with([](ss::Inputs& i) { i.picture = true; }), ss::kFbPicture},
      {"photo request: sync", with([](ss::Inputs& i) { i.photo = true; }), ss::kFbPhoto},
      {"read-back due / in flight: sync", with([](ss::Inputs& i) { i.readback = true; }), ss::kFbReadback},
      {"DrawAndRead part: sync", with([](ss::Inputs& i) { i.read_part = true; }), ss::kFbPart},
      {"ko switch: sync", with([](ss::Inputs& i) { i.ko = true; }), ss::kFbKo},
      {"tween + picture: sync (tween first)", with([](ss::Inputs& i) { i.tween = true, i.picture = true; }), ss::kFbTween},
  };
  for (const Row& r : rows) {
    const int got = ss::Decide(r.in);
    char line[160];
    std::snprintf(line, sizeof line, "decide: %s (got %d, want %d)", r.name, got, r.want);
    Say(got == r.want, line);
  }
  // The table: torn publishes.
  rdr::StableTable<uint64_t> table;
  constexpr uint64_t kN = 60000;
  std::atomic<bool> done{false};
  std::atomic<uint64_t> bad{0}, reads{0}, during{0};
  std::atomic<int> ready{0};
  const auto reader = [&] {
    ready.fetch_add(1);
    for (bool last = false; !last;) {
      last = done.load(std::memory_order_acquire);  // one more full pass after the writer is done
      const size_t n = table.size();
      for (size_t i = n > 512 && !last ? n - 512 : 0; i < n; ++i) {
        if (table[i] != i * 2654435761ull) bad.fetch_add(1);
        reads.fetch_add(1, std::memory_order_relaxed);
        if (!last) during.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };
  std::thread r1(reader), r2(reader);
  while (ready.load() < 2) std::this_thread::yield();
  for (uint64_t i = 0; i < kN; ++i) {
    table.push_back(i * 2654435761ull);
    if ((i & 255) == 0) std::this_thread::yield();  // let the readers in while the table grows (new chunks are being published)
  }
  done.store(true, std::memory_order_release);
  r1.join(), r2.join();
  char line[160];
  std::snprintf(line, sizeof line, "table: %zu appended under 2 readers (%llu reads, %llu while it grew), %llu torn", table.size(), static_cast<unsigned long long>(reads.load()),
                static_cast<unsigned long long>(during.load()), static_cast<unsigned long long>(bad.load()));
  Say(table.size() == kN && bad.load() == 0 && during.load() > 0, line);
  uint64_t n = 0;
  for (const uint64_t& v : table) n += v == n * 2654435761ull ? 1 : 0;
  Say(n == kN, "table: range-for visits every published entry");
  {  // review F5: a full table refuses the next entry (false, size unchanged); renderer.cpp turns that into a named Problem and kNoTableId, never the last id
    rdr::StableTable<int, 2, 4> small;  // capacity 16 (4 chunks of 4)
    bool all = true;
    for (int i = 0; i < 16; ++i) all = all && small.push_back(i);
    const bool refused = !small.push_back(16);
    std::snprintf(line, sizeof line, "table full: 16 of 16 taken %s, the 17th refused %s, size %zu", all ? "yes" : "no", refused ? "yes" : "no", small.size());
    Say(all && refused && small.size() == 16 && small[15] == 15, line);
  }
  std::printf("[stage_split_test] %s\n", fails ? "FAIL" : "PASS");
  return fails ? 1 : 0;
}
