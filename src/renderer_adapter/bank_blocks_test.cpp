// S102 T1b: the block map of a constant bank def (BankView::Block / CopyBytes / Ref / SameMap) against the eager expansion, GPU free, on every frame of a case zip.
//   clang++ -std=c++20 -I src/renderer_adapter -I src bank_blocks_test.cpp -o bank_blocks_test.exe
//   bank_blocks_test ZIP [plant 4|5|6]
// For every capture\frames\frame_NNNNNN.bin in the (stored) zip: A = the frame parsed with both switches off (fresh expanded banks, the old reader); B = a fresh LoadedFrame with
// lazy not asked for and blocks ON sharing ONE outside pool with every other frame (as live does). Checked against A for EVERY bank and EVERY one of its 64 blocks:
//   Block(i, b) == A's bytes; CopyBytes over seeded random byte ranges (also through Ref) == A's bytes; operator[] (the assembly path) == A's bank; SameMap(i, j) true => equal banks;
//   a read one past the table (Block, CopyBytes, Ref) and a block past 63 give zeros; and a frame read only through Block / CopyBytes builds NO bank (resolved counter unchanged).
// Planted twins (g_bank_plant): 4 = a block map skips its patches, 5 = a block map's base off by one, 6 = SameMap compares 63 of the 64 entries. Each must FAIL (exit 1).
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
    std::printf("usage: bank_blocks_test ZIP [plant 4|5|6]\n");
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
  std::mt19937 rng(777);
  uint64_t blocks_compared = 0, ranges_compared = 0, pairs_same = 0, pairs_tested = 0, with_defs = 0, patched = 0, assembled_after_blocks = 0, nopatch_defs = 0;
  bool zero_checked = false;
  for (uint32_t fi = 0; fi < frames.size(); ++fi) {
    std::vector<uint8_t> bytes(frames[fi].size);
    f.seekg(std::streamoff(frames[fi].offset));
    f.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    LoadedFrame a;   // both switches off: the old reader
    a.bytes = bytes;
    if (!CaptureReader::ParseFrame(a, 0, 0)) {
      std::printf("FAIL frame %u unreadable\n", fi);
      return 2;
    }
    const uint32_t n = a.constant_banks.count;
    with_defs += a.bank_defs.count != 0;
    for (uint32_t d = 0; d < a.bank_defs.count; ++d) patched += a.bank_defs[d].patch_count != 0;
    LoadedFrame b;   // blocks on (lazy not asked for: blocks implies it)
    b.constant_banks.UsePool(&pool);
    b.bytes = bytes;
    CaptureReader::ParseFrame(b, 0, 1);
    if (b.constant_banks.count != n) Fail("count", fi, 0);
    if (a.bank_defs.count && !b.constant_banks.blocks_mode()) Fail("blocks mode not on", fi, 0);
    b.constant_banks.Flush();
    const uint64_t resolved_before = g_bank_resolved.load();
    // 1. every block of every bank, in reverse order, through Block
    for (uint32_t i = n; i-- > 0;) {
      const uint8_t* want = reinterpret_cast<const uint8_t*>(a.constant_banks[i].dwords);
      for (uint32_t k = 0; k < 64; ++k)
        if (std::memcmp(b.constant_banks.Block(i, k), want + 64 * k, 64) != 0) Fail("block", fi, i);
      blocks_compared += 64;
    }
    // 2. seeded random byte ranges through CopyBytes and Ref (the packers' calls are 16-byte aligned register ranges, but any range must work)
    for (uint32_t t = 0; t < 400 && n; ++t) {
      const uint32_t i = uint32_t(rng() % n), off = uint32_t(rng() % 4096), len = uint32_t(rng() % 700) + 1;
      std::vector<uint8_t> got(len + 8, 0xAB), got2(len + 8, 0xAB), want(len + 8, 0xAB);
      const uint32_t cl = off + len > 4096 ? 4096 - off : len;
      std::memcpy(want.data(), reinterpret_cast<const uint8_t*>(a.constant_banks[i].dwords) + off, cl);
      b.constant_banks.CopyBytes(i, off, len, got.data());
      b.constant_banks.Ref(i).CopyBytes(off, len, got2.data());
      if (got != want) Fail("CopyBytes", fi, i);
      if (got2 != want) Fail("Ref.CopyBytes", fi, i);
      ++ranges_compared;
    }
    // 3. nothing above assembled a bank
    b.constant_banks.Flush();
    assembled_after_blocks += g_bank_resolved.load() - resolved_before;
    // 4. SameMap => equal banks (and the assembly path agrees with the eager bank)
    for (uint32_t t = 0; t < 300 && n > 1; ++t) {
      const uint32_t i = uint32_t(rng() % n), j = (t % 2) ? (i + 1 < n ? i + 1 : 0) : uint32_t(rng() % n);
      ++pairs_tested;
      if (b.constant_banks.SameMap(i, j)) {
        ++pairs_same;
        if (std::memcmp(&a.constant_banks[i], &a.constant_banks[j], sizeof(ConstantBank)) != 0) Fail("SameMap but banks differ", fi, i);
      }
    }
    for (uint32_t d = 0; d < a.bank_defs.count && n; ++d) {   // review (a): a def with no patches and a valid base IS its base: SameMap must say so, and the banks agree
      const BankDef& bd = a.bank_defs[d];
      if (bd.base != kBankNoBase && bd.base < d && bd.patch_count != 0) {   // soundness on every patched def too: equal maps must mean equal banks (a patch that rewrites the same bytes is the only way)
        if (b.constant_banks.SameMap(d, bd.base) && std::memcmp(&a.constant_banks[d], &a.constant_banks[bd.base], sizeof(ConstantBank)) != 0) Fail("SameMap with its base but the banks differ", fi, d);
        continue;
      }
      if (bd.patch_count != 0 || bd.base == kBankNoBase || bd.base >= d) continue;
      ++nopatch_defs;
      if (!b.constant_banks.SameMap(d, bd.base)) Fail("a patch-free def is not SameMap with its base", fi, d);
      if (std::memcmp(&a.constant_banks[d], &a.constant_banks[bd.base], sizeof(ConstantBank)) != 0) Fail("a patch-free def differs from its base", fi, d);
    }
    for (uint32_t i = 0; i < n; i += 5)
      if (std::memcmp(&a.constant_banks[i], &b.constant_banks[i], sizeof(ConstantBank)) != 0) Fail("operator[] after blocks", fi, i);
    if (!zero_checked && n) {   // a read one past the table and a block past 63 give zeros (Block, CopyBytes, Ref; also the plain view)
      zero_checked = true;
      uint8_t z[64] = {}, got[64];
      std::memset(got, 0xAB, sizeof got);
      if (std::memcmp(b.constant_banks.Block(n, 0), z, 64) != 0) Fail("Block one past the table is not zero", fi, n);
      if (std::memcmp(b.constant_banks.Block(0, 64), z, 64) != 0) Fail("Block 64 is not zero", fi, 0);
      b.constant_banks.Ref(n).CopyBytes(0, 64, got);
      if (std::memcmp(got, z, 64) != 0) Fail("Ref one past the table is not zero", fi, n);
      ConstantBank zero{};
      const BankView direct(&zero, 1);
      if (std::memcmp(direct.Block(5, 3), z, 64) != 0) Fail("direct Block one past the table is not zero", fi, 5);
    }
  }
  if (assembled_after_blocks) {
    Fail("a bank was assembled by Block / CopyBytes reads", 0, 0);
    std::printf("  (%llu banks assembled)\n", (unsigned long long)assembled_after_blocks);
  }
  std::printf("%s: %zu frames (%llu with defs, %llu patched defs), %llu blocks and %llu byte ranges compared, %llu of %llu tested pairs share a map, %llu patch-free defs equal their base; %d differences\n", fails ? "FAIL" : "PASS", frames.size(),
              (unsigned long long)with_defs, (unsigned long long)patched, (unsigned long long)blocks_compared, (unsigned long long)ranges_compared, (unsigned long long)pairs_same,
              (unsigned long long)pairs_tested, (unsigned long long)nopatch_defs, fails);
  return fails ? 1 : 0;
}
