// S111 MOUSESCROLL: unit for src/menu_scroll.h (the SmartMenu list scroll at SM + 1388 + 5292 * [SM+268]). Pure:
// planted guest words in a map, no game, no SDK.
//   menu_scroll_unit                 run every case (exit 0 = all pass)
//   menu_scroll_unit --plant NAME    NAME = fixed_1388 (the old rule: +1388 whatever the context) | no_range (no context
//                                    check): the checks must SEE the plant, so it exits 1 (prove: expect-fail)
#include <cstdio>
#include <map>
#include <string>

#include "../../src/menu_scroll.h"

using namespace wwe2k14::menu_scroll;

static int g_fail = 0;
#define CHECK(c, ...)                          \
  do {                                         \
    if (!(c)) {                                \
      ++g_fail;                                \
      std::printf("FAIL line %d: ", __LINE__); \
      std::printf(__VA_ARGS__);                \
      std::printf("\n");                       \
    }                                          \
  } while (0)

static std::map<uint32_t, uint32_t> g_mem;
static uint32_t rd(uint32_t a) {
  auto it = g_mem.find(a);
  return it == g_mem.end() ? 0u : it->second;
}
static void put(uint32_t a, uint32_t v) { g_mem[a] = v; }

static std::string g_plant;

static bool ScrollUnderTest(uint32_t sm, int& scroll) {
  if (g_plant == "fixed_1388") {
    scroll = int(rd(sm + 1388));
    return true;
  }
  if (g_plant == "no_range") {
    scroll = int(rd(sm + kListRecordOffset + kListRecordStride * rd(sm + kContextOffset)));
    return true;
  }
  return ListScroll(sm, rd, scroll);
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) g_plant = argv[++i];

  const uint32_t sm = 0x40100000u;
  CHECK(ListRecord(sm, 0) == sm + 1388 && ListRecord(sm, 1) == sm + 6680, "record addresses");

  // 1. Context 0: the word at +1388.
  {
    g_mem.clear();
    put(sm + 268, 0), put(sm + 1388, 1), put(sm + 6680, 5);
    int s = -1;
    CHECK(ScrollUnderTest(sm, s) && s == 1, "context 0 reads +1388 (got %d)", s);
  }
  // 2. Context 1: the word at +1388 + 5292, not the stale context-0 word.
  {
    g_mem.clear();
    put(sm + 268, 1), put(sm + 1388, 1), put(sm + 6680, 3);
    int s = -1;
    CHECK(ScrollUnderTest(sm, s) && s == 3, "context 1 reads +6680 (got %d)", s);
  }
  // 3. A context word outside 0 / 1 (2, -1): no scroll, as the game's readers skip the record.
  for (uint32_t bad : {2u, 0xFFFFFFFFu}) {
    g_mem.clear();
    put(sm + 268, bad), put(sm + 1388, 1), put(sm + 6680, 3);
    int s = -7;
    CHECK(!ScrollUnderTest(sm, s) && s == -7, "context 0x%X gives no scroll", bad);
  }

  std::printf("menu_scroll_unit: %d failed%s%s\n", g_fail, g_plant.empty() ? "" : " (plant ",
              g_plant.empty() ? "" : (g_plant + ")").c_str());
  return g_fail ? 1 : 0;  // a planted twin is caught when this is 1 (the proof runs it as expect-fail)
}
