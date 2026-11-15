// S102 lane T8: hand a frame to the render thread in memory (no serialise on the swap thread, no parse on the render thread).
// No game or SDK includes (rplay, fview and the offline test include it).
//
// Today the reader serialises g_frame into one byte vector (reader.h FrameBytes) and the render thread parses it back (CaptureReader::ParseFrame): two
// passes of ~1-3 MB copies. Here the reader moves the frame's table vectors into a pooled FrameData; the render thread points its LoadedFrame tables at them.
// ONE list (WWE2K14_HANDOFF_TABLES) generates the FrameData members, Clear, the section list, BindInto, SwapTables, Trim and the test's MakeData, so a table
// cannot be handed over by one path and forgotten by another (tools\renderer\handoff_unit.py also lints the list against ParseFrame's case ids and FrameTables).
//   wwe2k14_rdr_handoff_memory (PerFrame, default off): read on the swap thread once per guest frame (reader.h), carried with the message (live.cpp);
//   the render thread never reads it. The case ring and a capture need bytes: Decide() says so, and counts it (the 600-frame [rdr][live] line).
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "capture_reader.h"

// X(section id, element type, FrameData / FrameTables member, LoadedFrame member): the tables in the file's order (the single / string sections follow in ForEachSection).
#define WWE2K14_HANDOFF_TABLES(X)                                  \
  X(kSecOps, Op, ops, ops)                                         \
  X(kSecTargets, rdr::TargetsOp, targets, targets)                 \
  X(kSecClears, rdr::ClearOp, clears, clears)                      \
  X(kSecDraws, Draw, draws, draws)                                 \
  X(kSecCopies, rdr::CopyOp, copies, copies)                       \
  X(kSecPresents, rdr::PresentOp, presents, presents)              \
  X(kSecFixeds, DrawFixed, fixeds, fixeds)                         \
  X(kSecDynamics, DrawDynamic, dynamics, dynamics)                 \
  X(kSecSamplers, rdr::SamplerDesc, samplers, samplers)            \
  X(kSecTextureSlots, TextureSlot, texture_slots, texture_slots)   \
  X(kSecLayouts, VertexLayout, layouts, layouts)                   \
  X(kSecElements, VertexElement, elements, elements)               \
  X(kSecConstantBanks, ConstantBank, constant_banks, full_banks)   \
  X(kSecBoolLoops, BoolLoop, bool_loops, bool_loops)               \
  X(kSecContents, ContentRef, contents, contents)                  \
  X(kSecCopyExtras, CopyExtra, copy_extras, copy_extras)           \
  X(kSecBankDefs, BankDef, bank_defs, bank_defs)                   \
  X(kSecBankPatches, BankPatch, bank_patches, bank_patches)        \
  X(kSecDrawOrigins, DrawOrigin, origins, origins)                 \
  X(kSecProvenance, TextureProvenance, provenance, provenance)     \
  X(kSecTextRuns, TextRun, text_runs, text_runs)

namespace rdr_adapter {
namespace handoff {

// Test plants (the gate's twins; 0 = off). 1 = the draw origins are not handed over, 2 = the header flags, 3 = the bank patches, 4 = the strings, 5 = the thin facts
// (all in BindInto only: the byte path stays the reference); 6 = Serialize writes its first two sections swapped.
inline std::atomic<int> g_plant{0};
inline bool PlantedOut(int plant, const char* member) {
  return (plant == 1 && std::strcmp(member, "origins") == 0) || (plant == 3 && std::strcmp(member, "bank_patches") == 0);
}

// How a live frame leaves the reader. The switch is read once per guest frame by the caller (rdr_sw::BoolSeen); the case ring needs bytes (case_hook::OnFrame).
enum class Way { kBytesSwitchOff, kBytesRingOn, kMemory };
inline std::atomic<uint64_t> g_in_memory{0}, g_ring_forced_bytes{0};
inline Way Decide(bool switch_on, bool ring_on, bool* first_forced = nullptr) {
  if (first_forced) *first_forced = false;
  if (!switch_on) return Way::kBytesSwitchOff;
  if (ring_on) {
    if (first_forced) *first_forced = g_ring_forced_bytes.fetch_add(1, std::memory_order_relaxed) == 0;
    else g_ring_forced_bytes.fetch_add(1, std::memory_order_relaxed);
    return Way::kBytesRingOn;
  }
  g_in_memory.fetch_add(1, std::memory_order_relaxed);
  return Way::kMemory;
}

struct FrameData {
  FrameFileHeader h;  // the header the serialiser writes (section_count is filled by Serialize / Bind)
#define T8_MEMBER(sec, T, member, loaded) std::vector<T> member;
  WWE2K14_HANDOFF_TABLES(T8_MEMBER)
#undef T8_MEMBER
  std::vector<ThinDrawFacts> thin_facts;  // handed over only when it is parallel to draws (else count 0, as FrameBytes)
  std::string strings;
  bool has_thin = false;
  ThinFrameStamp thin_stamp;
  bool has_d3 = false;
  D3FrameCounts d3_counts;

  void Clear() {  // capacity kept
    h = FrameFileHeader{};
#define T8_CLEAR(sec, T, member, loaded) member.clear();
    WWE2K14_HANDOFF_TABLES(T8_CLEAR)
#undef T8_CLEAR
    thin_facts.clear();
    strings.clear();
    has_thin = has_d3 = false;
    thin_stamp = ThinFrameStamp{}, d3_counts = D3FrameCounts{};
  }
  // A vector whose capacity is far past what this frame used is dropped (one huge frame must not pin its memory in the pool for good).
  template <class T>
  static void TrimOne(std::vector<T>& v) {
    if (v.capacity() > 4 * std::max<size_t>(v.size(), 64)) std::vector<T>().swap(v);
  }
  void Trim() {
#define T8_TRIM(sec, T, member, loaded) TrimOne(member);
    WWE2K14_HANDOFF_TABLES(T8_TRIM)
#undef T8_TRIM
    TrimOne(thin_facts);
    if (strings.capacity() > 4 * std::max<size_t>(strings.size(), 64)) std::string().swap(strings);
  }

  // The section list, in the file's order (FrameBytes' Part list before this lane). f(section id, element size, count, data).
  template <class F>
  void ForEachSection(F&& f) const {
#define T8_SECTION(sec, T, member, loaded) f(uint32_t(sec), sizeof(T), member.size(), static_cast<const void*>(member.data()));
    WWE2K14_HANDOFF_TABLES(T8_SECTION)
#undef T8_SECTION
    f(uint32_t(kSecThin), sizeof(ThinFrameStamp), has_thin ? size_t(1) : size_t(0), static_cast<const void*>(&thin_stamp));
    f(uint32_t(kSecThinFacts), sizeof(ThinDrawFacts), thin_facts.size() == draws.size() ? thin_facts.size() : size_t(0), static_cast<const void*>(thin_facts.data()));
    f(uint32_t(kSecD3), sizeof(D3FrameCounts), has_d3 ? size_t(1) : size_t(0), static_cast<const void*>(&d3_counts));
    f(uint32_t(kSecStrings), size_t(1), strings.size(), static_cast<const void*>(strings.data()));  // (last: FrameProvenance may add strings)
  }
  static constexpr size_t kMaxSections = 32;
  uint32_t SectionCount() const {
    uint32_t n = 0;
    ForEachSection([&](uint32_t, size_t, size_t, const void*) { ++n; });
    return n;
  }
};

// Every table (and thin_facts, strings) of the reader's FrameTables <-> a FrameData: a swap, the capacity travels. Symmetric. `A` is FrameTables (reader.h) or any
// struct with the same member names (the offline test's).
template <class A>
inline void SwapTables(A& t, FrameData& d) {
#define T8_SWAP(sec, T, member, loaded) t.member.swap(d.member);
  WWE2K14_HANDOFF_TABLES(T8_SWAP)
#undef T8_SWAP
  t.thin_facts.swap(d.thin_facts);
  t.strings.swap(d.strings);
}

// The file / case-ring / read-back-part bytes: exactly what FrameBytes wrote before this lane (the test freezes the old writer and compares).
inline std::vector<uint8_t> Serialize(const FrameData& d) {
  struct E { uint32_t id; size_t element, count; const void* data; };
  std::array<E, FrameData::kMaxSections> parts{};
  size_t np = 0;
  d.ForEachSection([&](uint32_t id, size_t element, size_t count, const void* data) { if (np < parts.size()) parts[np++] = {id, element, count, data}; });
  if (g_plant.load(std::memory_order_relaxed) == 6 && np > 1) std::swap(parts[0], parts[1]);   // planted twin: two sections' order
  FrameFileHeader h = d.h;
  h.section_count = uint32_t(np);
  std::array<SectionEntry, FrameData::kMaxSections> entries{};
  size_t offset = sizeof(FrameFileHeader) + np * sizeof(SectionEntry);
  for (size_t i = 0; i < np; ++i) {
    entries[i].id = parts[i].id;
    entries[i].element_size = uint32_t(parts[i].element);
    entries[i].count = parts[i].count;
    entries[i].offset = offset;
    offset += parts[i].element * parts[i].count;
  }
  std::vector<uint8_t> file;
  file.reserve(offset);
  file.resize(sizeof(FrameFileHeader) + np * sizeof(SectionEntry));
  std::memcpy(file.data(), &h, sizeof(h));
  if (np) std::memcpy(file.data() + sizeof(FrameFileHeader), entries.data(), np * sizeof(SectionEntry));
  for (size_t i = 0; i < np; ++i) {
    if (!parts[i].count) continue;
    const size_t at = file.size();
    file.resize(at + parts[i].element * parts[i].count);
    std::memcpy(file.data() + at, parts[i].data, parts[i].element * parts[i].count);
  }
  return file;
}

// The render thread's side: the LoadedFrame's tables point into `d` (which must outlive the frame; live.cpp keeps the shared_ptr beside it). `bytes` stays empty.
// Mirrors CaptureReader::ParseFrame section for section and its constant-bank tail (compared by the gate test with every bank of every frame; the tail is the one place that
// must follow ParseFrame when T1 / T1b change it).
template <class T>
inline void BindTable(const std::vector<T>& v, Table<T>& t) {
  t.data = v.data();
  t.count = uint32_t(v.size());
}
inline void BindInto(const FrameData& d, LoadedFrame& out, int lazy = -1, int blocks = -1) {
  const int plant = g_plant.load(std::memory_order_relaxed);
  out.bytes.clear();
  out.header = d.h;
  out.header.section_count = d.SectionCount();
  if (plant == 2) out.header.flags = 0;
  out.thin = {}, out.thin_facts = {}, out.d3 = {};   // (a reused LoadedFrame must not keep the previous frame's)
#define T8_BIND(sec, T, member, loaded) \
  if (plant && PlantedOut(plant, #member)) out.loaded = {}; else BindTable(d.member, out.loaded);
  WWE2K14_HANDOFF_TABLES(T8_BIND)
#undef T8_BIND
  if (d.has_thin) { out.thin.data = &d.thin_stamp; out.thin.count = 1; }
  if (plant != 5 && d.thin_facts.size() == d.draws.size()) BindTable(d.thin_facts, out.thin_facts);
  if (d.has_d3) { out.d3.data = &d.d3_counts; out.d3.count = 1; }
  if (plant != 4) { out.strings.data = d.strings.data(); out.strings.count = uint32_t(d.strings.size()); } else out.strings = {};
  // constant banks: the tail of ParseFrame
  if (out.bank_defs.count) {
    out.constant_banks.SetDefs(out.full_banks, out.bank_defs, out.bank_patches, lazy < 0 ? g_lazy_banks.load(std::memory_order_relaxed) : lazy != 0,
                                  blocks < 0 ? g_bank_blocks.load(std::memory_order_relaxed) : blocks != 0);   // S102 T1b
  } else {
    out.constant_banks.SetDirect(out.full_banks.data, out.full_banks.count);
  }
}

// Pooled FrameData: the swap thread takes one, fills it by swapping the reader's vectors in (capacity travels), the render thread drops it when the frame
// is done and it comes back cleared (oversized vectors dropped) with its capacity. At most `keep` idle ones are kept (the reader sets it to max_queued + 2).
class Pool {
 public:
  std::shared_ptr<FrameData> Take() {
    FrameData* d = nullptr;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!idle_.empty()) { d = idle_.back().release(); idle_.pop_back(); }
    }
    if (!d) d = new FrameData();
    return std::shared_ptr<FrameData>(d, [this](FrameData* p) { Give(p); });
  }
  void SetKeep(size_t keep) { keep_.store(keep, std::memory_order_relaxed); }
  size_t idle() {
    std::lock_guard<std::mutex> lock(mu_);
    return idle_.size();
  }

 private:
  void Give(FrameData* p) {
    p->Trim();
    p->Clear();
    std::unique_ptr<FrameData> keep(p);
    std::lock_guard<std::mutex> lock(mu_);
    if (idle_.size() < keep_.load(std::memory_order_relaxed)) idle_.push_back(std::move(keep));
  }
  std::mutex mu_;
  std::atomic<size_t> keep_{4};
  std::vector<std::unique_ptr<FrameData>> idle_;
};
inline Pool& FramePool() {
  static Pool* p = new Pool();   // (never destroyed: a frame in flight at exit still returns here)
  return *p;
}

}  // namespace handoff
}  // namespace rdr_adapter
