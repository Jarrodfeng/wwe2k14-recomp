// S102 T1 (review F7, F3): the lazy bank view against the eager expansion, GPU free, on every frame of a case zip.
//   clang++ -std=c++20 -I src/renderer_adapter -I src lazy_banks_test.cpp -o lazy_banks_test.exe
//   lazy_banks_test ZIP [plant 1|2|3]
// For every capture\frames\frame_NNNNNN.bin in the (stored) zip: A = the frame parsed with the switch off (fresh banks, the old reader); B = a fresh LoadedFrame with the
// switch on that shares ONE outside pool with every other frame (as live does), banks read in reverse order; C = another fresh one on the same pool, banks read in a
// seeded random order. Every bank of A must equal B's and C's byte for byte, and a read one past the table must give zeros (F3), also for a doctored draw.
// Planted twins (g_bank_plant): 1 = patches skipped, 2 = the done flags of the pool's previous frame kept, 3 = a def's base off by one. Each must FAIL (exit 1).
// Exit 0 = all equal, 1 = a difference (or a plant that was caught), 2 = unreadable input.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "capture_reader.h"

using namespace rdr_adapter;

namespace {
struct Entry {
  std::string name;
  uint64_t offset = 0, size = 0;
};

uint32_t Rd32(const std::vector<uint8_t>& b, size_t o) {
  uint32_t v;
  std::memcpy(&v, b.data() + o, 4);
  return v;
}
uint16_t Rd16(const std::vector<uint8_t>& b, size_t o) {
  uint16_t v;
  std::memcpy(&v, b.data() + o, 2);
  return v;
}

// A stored zip (the case writer never compresses): central directory -> the frame entries.
bool ListFrames(std::ifstream& f, uint64_t file_size, std::vector<Entry>& out) {
  const uint64_t tail_n = std::min<uint64_t>(file_size, 70000);
  std::vector<uint8_t> tail(tail_n);
  f.seekg(std::streamoff(file_size - tail_n));
  f.read(reinterpret_cast<char*>(tail.data()), std::streamsize(tail_n));
  size_t eocd = std::string::npos;
  for (size_t i = tail_n >= 22 ? tail_n - 22 : 0; i + 1 > 0; --i) {
    if (Rd32(tail, i) == 0x06054b50u) { eocd = i; break; }
    if (i == 0) break;
  }
  if (eocd == std::string::npos) return false;
  const uint32_t entries = Rd16(tail, eocd + 10), cd_size = Rd32(tail, eocd + 12), cd_off = Rd32(tail, eocd + 16);
  if (cd_off == 0xFFFFFFFFu) return false;   // zip64: not written by the case writer
  std::vector<uint8_t> cd(cd_size);
  f.seekg(cd_off);
  f.read(reinterpret_cast<char*>(cd.data()), cd_size);
  size_t p = 0;
  for (uint32_t e = 0; e < entries && p + 46 <= cd.size(); ++e) {
    if (Rd32(cd, p) != 0x02014b50u) return false;
    const uint16_t method = Rd16(cd, p + 10), nl = Rd16(cd, p + 28), xl = Rd16(cd, p + 30), cl = Rd16(cd, p + 32);
    const uint32_t csize = Rd32(cd, p + 20), local = Rd32(cd, p + 42);
    std::string name(reinterpret_cast<const char*>(cd.data() + p + 46), nl);
    p += 46 + nl + xl + cl;
    if (name.find("frames/frame_") == std::string::npos || name.size() < 4 || name.compare(name.size() - 4, 4, ".bin") != 0) continue;
    if (method != 0) return false;
    // local header: 30 + name + extra
    std::vector<uint8_t> lh(30);
    f.seekg(local);
    f.read(reinterpret_cast<char*>(lh.data()), 30);
    out.push_back({name, uint64_t(local) + 30 + Rd16(lh, 26) + Rd16(lh, 28), csize});
  }
  return !out.empty();
}

int fails = 0;
void Fail(const char* what, uint32_t frame, uint32_t bank) {
  if (fails < 8) std::printf("DIFF %s: frame %u bank %u\n", what, frame, bank);
  ++fails;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: lazy_banks_test ZIP [plant 1|2|3]\n");
    return 2;
  }
  if (argc >= 4 && std::strcmp(argv[2], "plant") == 0) g_bank_plant = std::atoi(argv[3]);
  std::ifstream f(argv[1], std::ios::binary | std::ios::ate);
  if (!f) {
    std::printf("FAIL cannot open %s\n", argv[1]);
    return 2;
  }
  const uint64_t size = uint64_t(f.tellg());
  std::vector<Entry> frames;
  if (!ListFrames(f, size, frames)) {
    std::printf("FAIL no stored frames in %s\n", argv[1]);
    return 2;
  }
  BankPool pool;   // ONE outside pool for every frame, as the live sink has
  std::mt19937 rng(12345);
  uint64_t compared = 0, with_defs = 0, patched = 0;
  bool zero_checked = false;
  for (uint32_t fi = 0; fi < frames.size(); ++fi) {
    std::vector<uint8_t> bytes(frames[fi].size);
    f.seekg(std::streamoff(frames[fi].offset));
    f.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    LoadedFrame a;   // switch off: the old reader
    a.bytes = bytes;
    if (!CaptureReader::ParseFrame(a, 0)) {
      std::printf("FAIL frame %u unreadable\n", fi);
      return 2;
    }
    const uint32_t n = a.constant_banks.count;
    with_defs += a.bank_defs.count != 0;
    for (uint32_t d = 0; d < a.bank_defs.count; ++d) patched += a.bank_defs[d].patch_count != 0;
    {   // B: fresh frame, pool shared, reverse order
      LoadedFrame b;
      b.constant_banks.UsePool(&pool);
      b.bytes = bytes;
      CaptureReader::ParseFrame(b, 1);
      if (b.constant_banks.count != n) Fail("count", fi, 0);
      for (uint32_t i = n; i-- > 0;)
        if (std::memcmp(&a.constant_banks[i], &b.constant_banks[i], sizeof(ConstantBank)) != 0) Fail("reverse", fi, i);
      compared += n;
    }
    {   // C: fresh frame on the same pool, seeded random order, each bank read twice
      LoadedFrame c;
      c.constant_banks.UsePool(&pool);
      c.bytes = bytes;
      CaptureReader::ParseFrame(c, 1);
      std::vector<uint32_t> order(n);
      std::iota(order.begin(), order.end(), 0u);
      std::shuffle(order.begin(), order.end(), rng);
      for (uint32_t i : order) {
        if (std::memcmp(&a.constant_banks[i], &c.constant_banks[i], sizeof(ConstantBank)) != 0) Fail("random", fi, i);
        if (std::memcmp(&a.constant_banks[i], &c.constant_banks[i], sizeof(ConstantBank)) != 0) Fail("random again", fi, i);
      }
      compared += n;
      if (!zero_checked && n && a.draws.count) {   // F3: a doctored draw one past the table, and a plain read one past, give zero banks (eager, lazy and direct views)
        zero_checked = true;
        Draw doctored = a.draws[0];
        doctored.vs_bank = n;
        ConstantBank zero{};
        if (std::memcmp(&a.constant_banks[doctored.vs_bank], &zero, sizeof zero) != 0) Fail("eager one past the table is not zero", fi, n);
        if (std::memcmp(&c.constant_banks[doctored.vs_bank], &zero, sizeof zero) != 0) Fail("lazy one past the table is not zero", fi, n);
        const BankView direct(&zero, 1);
        if (std::memcmp(&direct[7], &zero, sizeof zero) != 0) Fail("direct one past the table is not zero", fi, 7);
      }
    }
  }
  std::printf("%s: %zu frames (%llu with defs, %llu patched defs), %llu banks compared; %d differences\n", fails ? "FAIL" : "PASS", frames.size(),
              (unsigned long long)with_defs, (unsigned long long)patched, (unsigned long long)compared, fails);
  return fails ? 1 : 0;
}
