// S111 FXSKIP (docs\research\s111\FXSKIP.md): the record reader's one rule for a row kind it does not know.
// Every row of a record section is the section's element_size bytes (SectionEntry::element_size: one stride for the whole section, Load in record_io.cpp
// refuses a size it does not know), so a row's length never depends on its kind and a reader skips a newer writer's row by the stride it already walks.
// KeepKnownKinds keeps the rows whose kind is in [lo, hi], in their order, and counts every other row into `errors`: a new kind costs an older reader
// that kind's rows only, never the section (the kScStageLook rule of the S103 D1 review, now every kind-tagged section's). An unknown LAYOUT is a
// different row shape and still drops its section (each reader checks it before calling this).
#pragma once
#include <cstdint>
#include <vector>

namespace rdr_scene {

#ifdef RDR_RECORD_KIND_PLANTS
inline int g_kind_plant = 0;   // test only (tools\scene\record_kinds_unit.cpp): 1 = an unknown kind drops the whole section (the rule before FXSKIP), 2 = an unknown kind is kept as a row
#endif

template <class Row>
void KeepKnownKinds(std::vector<Row>& rows, uint8_t lo, uint8_t hi, uint32_t& errors) {
  size_t kept = 0;
  for (size_t i = 0; i < rows.size(); ++i) {
    const bool known = rows[i].kind >= lo && rows[i].kind <= hi;
#ifdef RDR_RECORD_KIND_PLANTS
    if (!known && g_kind_plant == 1) {
      rows.clear();
      ++errors;
      return;
    }
    if (!known && g_kind_plant == 2) {
      rows[kept++] = rows[i];
      continue;
    }
#endif
    if (known) rows[kept++] = rows[i];
    else ++errors;
  }
  rows.resize(kept);
}

}  // namespace rdr_scene
