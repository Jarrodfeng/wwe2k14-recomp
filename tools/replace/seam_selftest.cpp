// S101 lane 41: offline test of the replace seam's vertex-constant row override (src\renderer_adapter\replace_seam.h ApplyRowOverride) on the REAL effect
// tables (rdr::fx::kVariants), no GPU. For every variant with constant ranges and several override shapes (a run inside one range, across two ranges,
// partly outside every range, the whole table), the block after the override must hold the override's row at EVERY register it names that some range
// maps, and be byte-identical to the original everywhere else (the rows NOT named stay). The checker is an independent per-register model, not the
// function's own loop. Planted twins (each must FAIL the checker): rows applied one register late, one register early, one row too many, rows not applied.
// Exit 0: all real shapes pass AND all plants are caught.
//   out\tools\replace_seam_selftest.exe          (built by tools\replace\replace_seam_check.py)
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../../src/renderer/shaders/spv/fx_registry.h"
#include "../../src/renderer_adapter/replace_seam.h"

namespace {

// The independent model: which block byte offset holds register `reg` (or -1: no range maps it).
long OffsetOf(const rdr::fx::Variant& v, uint32_t reg) {
  for (uint32_t r = 0; r < v.constant_count; ++r)
    if (reg >= v.constants[r].reg && reg < uint32_t(v.constants[r].reg) + v.constants[r].count) return long(v.constants[r].offset) + long(reg - v.constants[r].reg) * 16;
  return -1;
}

// 0 = the block is exactly what the override says; else a count of violations.
int Check(const rdr::fx::Variant& v, const std::vector<uint8_t>& before, const std::vector<uint8_t>& after, uint32_t first, uint32_t rows, const std::vector<float>& data, uint32_t* named_rows) {
  std::vector<uint8_t> expect = before;
  uint32_t named = 0;
  for (uint32_t i = 0; i < rows; ++i) {
    const long at = OffsetOf(v, first + i);
    if (at < 0) continue;
    ++named;
    std::memcpy(expect.data() + at, data.data() + size_t(i) * 4, 16);
  }
  if (named_rows) *named_rows = named;
  int bad = 0;
  for (size_t i = 0; i < expect.size(); ++i) bad += expect[i] != after[i];
  return bad;
}

}  // namespace

int main() {
  int variants_tested = 0, shapes = 0, failures = 0, plants_missed = 0, plants = 0;
  for (size_t vi = 0; vi < std::size(rdr::fx::kVariants); ++vi) {
    const rdr::fx::Variant& v = rdr::fx::kVariants[vi];
    if (!v.constant_count || v.block_bytes < 16 || v.block_bytes > 8192) continue;
    ++variants_tested;
    const uint32_t bytes = v.block_bytes;
    std::vector<uint8_t> before(bytes);
    for (uint32_t i = 0; i < bytes; ++i) before[i] = uint8_t(0xA5 ^ (i * 7));  // a block the override must not disturb outside its rows
    // shapes: [first, rows)
    struct Shape { uint32_t first, rows; };
    const uint32_t reg0 = v.constants[0].reg, count0 = v.constants[0].count, regL = v.constants[v.constant_count - 1].reg, countL = v.constants[v.constant_count - 1].count;
    const Shape list[] = {{reg0, count0}, {reg0, 1}, {reg0 + count0 / 2, count0 > 1 ? count0 - count0 / 2 : 1}, {reg0 > 2 ? reg0 - 2 : 0, count0 + 4},  // around the first range
                          {regL, countL}, {regL + countL - 1, 3},                                                                                     // the last range's tail and past it
                          {0, 256}, {9999, 4}};                                                                                                       // the whole register file / nothing mapped
    for (const Shape& s : list) {
      std::vector<float> data(size_t(s.rows) * 4);
      for (size_t i = 0; i < data.size(); ++i) data[i] = 1000.0f + float(i) + float(s.first);
      std::vector<uint8_t> after = before;
      rdr_adapter::ApplyRowOverride(v.constants, v.constant_count, after.data(), bytes, s.first, s.rows, data.data());
      uint32_t named = 0;
      const int bad = Check(v, before, after, s.first, s.rows, data, &named);
      ++shapes;
      if (bad) {
        ++failures;
        std::printf("FAIL  %s first %u rows %u: %d bytes differ from the model\n", v.name, s.first, s.rows, bad);
      }
      if (!named) continue;
      // Planted twins: each must be caught by the same checker.
      const auto plant = [&](const char* what, auto&& apply) {
        std::vector<uint8_t> copy = before;
        apply(copy);
        ++plants;
        if (Check(v, before, copy, s.first, s.rows, data, nullptr) == 0) {
          ++plants_missed;
          std::printf("FAIL  plant '%s' on %s first %u rows %u was NOT caught\n", what, v.name, s.first, s.rows);
        }
      };
      plant("late by one register", [&](std::vector<uint8_t>& b) { rdr_adapter::ApplyRowOverride(v.constants, v.constant_count, b.data(), bytes, s.first + 1, s.rows, data.data()); });
      if (s.first) plant("early by one register", [&](std::vector<uint8_t>& b) { rdr_adapter::ApplyRowOverride(v.constants, v.constant_count, b.data(), bytes, s.first - 1, s.rows, data.data()); });
      if (OffsetOf(v, s.first + s.rows) >= 0) plant("one row too many", [&](std::vector<uint8_t>& b) {  // (only when the extra register is mapped: else the plant changes nothing)
        std::vector<float> more = data;
        more.resize(data.size() + 4, -1.0f);
        rdr_adapter::ApplyRowOverride(v.constants, v.constant_count, b.data(), bytes, s.first, s.rows + 1, more.data());
      });
      plant("rows not applied", [&](std::vector<uint8_t>&) {});
    }
  }
  std::printf("variants %d, shapes %d, failures %d, planted faults %d, missed %d\n", variants_tested, shapes, failures, plants, plants_missed);
  if (!variants_tested || !shapes || !plants) {
    std::printf("FAIL  nothing was tested (the registry is empty?)\n");
    return 2;
  }
  return failures || plants_missed ? 1 : 0;
}
