// S102 lane T8: the in-memory hand-over (frame_handoff.h) against the byte path, GPU free, on every frame of a case zip.
//   clang++ -std=c++20 -I src/renderer_adapter -I src handoff_test.cpp -o handoff_test.exe
//   handoff_test ZIP [plant 1..5]
// For every capture\frames\frame_NNNNNN.bin of the (stored) zip: A = the frame as the render thread parses it today (ParseFrame of the file's bytes). The tables of A are
// copied into a FrameData (what the reader's FillFrameData builds from g_frame), then
//   B = Serialize(FrameData) -> ParseFrame: the byte path of the new code (a capture, the case ring); the section list is the same one the hand-over uses;
//   C = BindInto(FrameData): the in-memory path, with the lazy banks on ONE outside pool (as live does).
// What the binder reads must be equal in A, B and C: the header, every table (count + bytes), the strings, thin / d3 stamps, and every constant bank (read in a seeded random
// order from the lazy view). B's bytes are also compared with the file's own bytes (informational: an older frame may lack a section).
// Planted twins (handoff::g_plant, in BindInto only): 1 = origins not handed over, 2 = header flags, 3 = bank patches, 4 = strings, 5 = thin facts. Each must FAIL (exit 1).
// Time (direction only, this PC): the swap thread's serialise vs a pooled vector swap; the render thread's parse vs bind.
// Exit 0 = all equal, 1 = a difference (or a plant that was caught), 2 = unreadable input.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "frame_handoff.h"

using namespace rdr_adapter;

namespace {
struct Entry {
  std::string name;
  uint64_t offset = 0, size = 0;
};
uint32_t Rd32(const std::vector<uint8_t>& b, size_t o) { uint32_t v; std::memcpy(&v, b.data() + o, 4); return v; }
uint16_t Rd16(const std::vector<uint8_t>& b, size_t o) { uint16_t v; std::memcpy(&v, b.data() + o, 2); return v; }

bool ListFrames(std::ifstream& f, uint64_t file_size, std::vector<Entry>& out) {   // a stored zip (the case writer never compresses)
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
  if (cd_off == 0xFFFFFFFFu) return false;
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
void Fail(const char* what, uint32_t frame, uint32_t at = 0) {
  if (fails < 8) std::printf("DIFF %s: frame %u item %u\n", what, frame, at);
  ++fails;
}

template <class T>
void Copy(const Table<T>& t, std::vector<T>& v) { v.assign(t.data, t.data + t.count); }

handoff::FrameData MakeData(const LoadedFrame& a) {   // the reader's FillFrameData result for these tables
  handoff::FrameData d;
  d.h = a.header;
#define T8_COPY(sec, T, member, loaded) Copy(a.loaded, d.member);
  WWE2K14_HANDOFF_TABLES(T8_COPY)
#undef T8_COPY
  Copy(a.thin_facts, d.thin_facts);
  d.strings.assign(a.strings.data, a.strings.data + a.strings.count);
  d.has_thin = a.thin.count != 0;
  if (d.has_thin) d.thin_stamp = a.thin[0];
  d.has_d3 = a.d3.count != 0;
  if (d.has_d3) d.d3_counts = a.d3[0];
  return d;
}

// The pre-T8 FrameBytes, frozen (its Part list and layout code as they were in reader.h before this lane), on a FrameData. Serialize must equal it byte for byte.
std::vector<uint8_t> OldBytes(const handoff::FrameData& t) {
  struct Part { uint32_t id; size_t element, count; const void* data; };
  const Part parts[] = {
      {kSecOps, sizeof(Op), t.ops.size(), t.ops.data()},
      {kSecTargets, sizeof(rdr::TargetsOp), t.targets.size(), t.targets.data()},
      {kSecClears, sizeof(rdr::ClearOp), t.clears.size(), t.clears.data()},
      {kSecDraws, sizeof(Draw), t.draws.size(), t.draws.data()},
      {kSecCopies, sizeof(rdr::CopyOp), t.copies.size(), t.copies.data()},
      {kSecPresents, sizeof(rdr::PresentOp), t.presents.size(), t.presents.data()},
      {kSecFixeds, sizeof(DrawFixed), t.fixeds.size(), t.fixeds.data()},
      {kSecDynamics, sizeof(DrawDynamic), t.dynamics.size(), t.dynamics.data()},
      {kSecSamplers, sizeof(rdr::SamplerDesc), t.samplers.size(), t.samplers.data()},
      {kSecTextureSlots, sizeof(TextureSlot), t.texture_slots.size(), t.texture_slots.data()},
      {kSecLayouts, sizeof(VertexLayout), t.layouts.size(), t.layouts.data()},
      {kSecElements, sizeof(VertexElement), t.elements.size(), t.elements.data()},
      {kSecConstantBanks, sizeof(ConstantBank), t.constant_banks.size(), t.constant_banks.data()},
      {kSecBoolLoops, sizeof(BoolLoop), t.bool_loops.size(), t.bool_loops.data()},
      {kSecContents, sizeof(ContentRef), t.contents.size(), t.contents.data()},
      {kSecCopyExtras, sizeof(CopyExtra), t.copy_extras.size(), t.copy_extras.data()},
      {kSecBankDefs, sizeof(BankDef), t.bank_defs.size(), t.bank_defs.data()},
      {kSecBankPatches, sizeof(BankPatch), t.bank_patches.size(), t.bank_patches.data()},
      {kSecDrawOrigins, sizeof(DrawOrigin), t.origins.size(), t.origins.data()},
      {kSecProvenance, sizeof(TextureProvenance), t.provenance.size(), t.provenance.data()},
      {kSecTextRuns, sizeof(TextRun), t.text_runs.size(), t.text_runs.data()},
      {kSecThin, sizeof(ThinFrameStamp), t.has_thin ? 1u : 0u, &t.thin_stamp},
      {kSecThinFacts, sizeof(ThinDrawFacts), t.thin_facts.size() == t.draws.size() ? t.thin_facts.size() : size_t(0), t.thin_facts.data()},
      {kSecD3, sizeof(D3FrameCounts), t.has_d3 ? 1u : 0u, &t.d3_counts},
      {kSecStrings, 1, t.strings.size(), t.strings.data()},
  };
  FrameFileHeader h = t.h;
  h.section_count = uint32_t(std::size(parts));
  std::vector<SectionEntry> entries(std::size(parts));
  size_t offset = sizeof(FrameFileHeader) + entries.size() * sizeof(SectionEntry);
  size_t total = offset;
  for (size_t i = 0; i < std::size(parts); ++i) {
    entries[i].id = parts[i].id;
    entries[i].element_size = uint32_t(parts[i].element);
    entries[i].count = parts[i].count;
    entries[i].offset = offset;
    offset += parts[i].element * parts[i].count;
    total = offset;
  }
  std::vector<uint8_t> file;
  file.reserve(total);
  auto put = [&](const void* p, size_t n) { if (!n) return; const size_t at = file.size(); file.resize(at + n); std::memcpy(file.data() + at, p, n); };
  put(&h, sizeof h);
  put(entries.data(), entries.size() * sizeof(SectionEntry));
  for (const Part& p : parts) put(p.data, p.element * p.count);
  return file;
}

// A struct with FrameTables' member names (the reader's FrameTables is game-side): SwapTables must move every table, both ways.
struct FakeTables {
#define T8_FAKE(sec, T, member, loaded) std::vector<T> member;
  WWE2K14_HANDOFF_TABLES(T8_FAKE)
#undef T8_FAKE
  std::vector<ThinDrawFacts> thin_facts;
  std::string strings;
};

template <class T>
bool SameTable(const Table<T>& x, const Table<T>& y) {
  return x.count == y.count && (x.count == 0 || std::memcmp(x.data, y.data, sizeof(T) * x.count) == 0);
}

// Everything the binder can read from a LoadedFrame; false = a difference (named in `what`).
bool Same(const LoadedFrame& x, const LoadedFrame& y, uint32_t frame, std::mt19937& rng, bool header_sections) {
  bool ok = true;
  auto no = [&](const char* what) { Fail(what, frame); ok = false; };
  FrameFileHeader hx = x.header, hy = y.header;
  if (!header_sections) hx.section_count = hy.section_count = 0;
  if (std::memcmp(&hx, &hy, sizeof hx) != 0) no("header");
#define T8_SAME(f) if (!SameTable(x.f, y.f)) no(#f);
  T8_SAME(ops) T8_SAME(targets) T8_SAME(clears) T8_SAME(draws) T8_SAME(copies) T8_SAME(presents) T8_SAME(fixeds) T8_SAME(dynamics) T8_SAME(samplers)
  T8_SAME(texture_slots) T8_SAME(layouts) T8_SAME(elements) T8_SAME(full_banks) T8_SAME(bool_loops) T8_SAME(contents) T8_SAME(copy_extras) T8_SAME(bank_defs)
  T8_SAME(bank_patches) T8_SAME(origins) T8_SAME(provenance) T8_SAME(strings) T8_SAME(text_runs) T8_SAME(thin_facts) T8_SAME(d3) T8_SAME(thin)
#undef T8_SAME
  if (x.constant_banks.count != y.constant_banks.count) {
    no("bank count");
  } else {
    std::vector<uint32_t> order(x.constant_banks.count);
    std::iota(order.begin(), order.end(), 0u);
    std::shuffle(order.begin(), order.end(), rng);
    for (uint32_t i : order)
      if (std::memcmp(&x.constant_banks[i], &y.constant_banks[i], sizeof(ConstantBank)) != 0) { Fail("bank", frame, i); ok = false; break; }
  }
  return ok;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: handoff_test ZIP [plant 1..5]\n");
    return 2;
  }
  if (argc >= 4 && std::strcmp(argv[2], "plant") == 0) handoff::g_plant = std::atoi(argv[3]);
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
  BankPool pool;
  std::mt19937 rng(777);
  uint64_t byte_identical = 0, with_origins = 0, with_patches = 0, with_strings = 0, with_thin_facts = 0, total_bytes = 0;
  using Clock = std::chrono::steady_clock;
  double t_serialize = 0, t_parse = 0, t_bind = 0, t_swap = 0;
  for (uint32_t fi = 0; fi < frames.size(); ++fi) {
    std::vector<uint8_t> bytes(frames[fi].size);
    f.seekg(std::streamoff(frames[fi].offset));
    f.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    total_bytes += bytes.size();
    LoadedFrame a;   // A: today
    a.bytes = bytes;
    if (!CaptureReader::ParseFrame(a, 1)) {
      std::printf("FAIL frame %u unreadable\n", fi);
      return 2;
    }
    handoff::FrameData d = MakeData(a);
    with_origins += d.origins.size() != 0, with_patches += d.bank_patches.size() != 0, with_strings += d.strings.size() != 0, with_thin_facts += d.thin_facts.size() != 0;
    // B: the byte path of the new code
    const auto s0 = Clock::now();
    std::vector<uint8_t> ser = handoff::Serialize(d);
    const auto s1 = Clock::now();
    if (ser != OldBytes(d)) Fail("Serialize differs from the frozen pre-T8 writer", fi);
    LoadedFrame b;
    b.bytes = std::move(ser);
    b.constant_banks.UsePool(&pool);
    const bool parsed_b = CaptureReader::ParseFrame(b, 1);
    const auto s2 = Clock::now();
    if (!parsed_b) { Fail("serialised frame unreadable", fi); continue; }
    if (b.bytes == bytes) ++byte_identical;
    Same(a, b, fi, rng, false);   // (the file may carry sections the reader no longer writes: section_count is not compared)
    // C: the in-memory path (the pool's table swap stands for the reader's SwapFrameTables)
    handoff::FrameData taken;
    const auto s3 = Clock::now();
    std::swap(taken, d);   // (three moves of the whole struct: more than the reader's 22 member swaps, so the hand-over cost shown is an over-estimate)
    std::swap(taken, d);
    const auto s4 = Clock::now();
    LoadedFrame c;
    c.constant_banks.UsePool(&pool);
    const auto s5 = Clock::now();
    handoff::BindInto(d, c, 1);
    const auto s6 = Clock::now();
    Same(a, c, fi, rng, false);
    t_serialize += std::chrono::duration<double, std::milli>(s1 - s0).count();
    t_parse += std::chrono::duration<double, std::milli>(s2 - s1).count();
    t_swap += std::chrono::duration<double, std::milli>(s4 - s3).count() / 2;
    t_bind += std::chrono::duration<double, std::milli>(s6 - s5).count();
    if (d.SectionCount() != b.header.section_count) Fail("section count", fi);
  }
  {   // The captures hold no thin facts and no D3 counts (older frames): one synthetic frame carries them, byte path vs hand-over (the planted twin 5 needs it).
    std::vector<uint8_t> bytes(frames[0].size);
    f.seekg(std::streamoff(frames[0].offset));
    f.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    LoadedFrame a;
    a.bytes = bytes;
    CaptureReader::ParseFrame(a, 1);
    handoff::FrameData d = MakeData(a);
    d.thin_facts.assign(d.draws.size(), ThinDrawFacts{});
    for (size_t i = 0; i < d.thin_facts.size(); ++i) std::memset(&d.thin_facts[i], int(1 + i % 200), sizeof(ThinDrawFacts));
    d.has_thin = true, d.thin_stamp = ThinFrameStamp{};
    std::memset(&d.thin_stamp, 0x5A, sizeof d.thin_stamp);
    d.has_d3 = true;
    std::memset(&d.d3_counts, 0x33, sizeof d.d3_counts);
    LoadedFrame b;
    b.bytes = handoff::Serialize(d);
    b.constant_banks.UsePool(&pool);
    if (!CaptureReader::ParseFrame(b, 1) || b.thin_facts.count != d.draws.size() || b.thin.count != 1 || b.d3.count != 1) Fail("synthetic frame unreadable", 0);
    LoadedFrame c;
    c.constant_banks.UsePool(&pool);
    handoff::BindInto(d, c, 1);
    Same(b, c, 100000, rng, true);
    std::printf("synthetic frame (thin facts %zu, thin stamp, d3 counts): %s\n", d.thin_facts.size(), fails ? "differs" : "equal");
  }
  {   // F3: all four thin / D3 combinations, new writer vs the frozen old one, and the parse of each
    std::vector<uint8_t> bytes(frames[0].size);
    f.seekg(std::streamoff(frames[0].offset));
    f.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    LoadedFrame a;
    a.bytes = bytes;
    CaptureReader::ParseFrame(a, 1);
    for (int combo = 0; combo < 4; ++combo) {
      handoff::FrameData d = MakeData(a);
      d.thin_facts.assign(d.draws.size(), ThinDrawFacts{});
      for (size_t i = 0; i < d.thin_facts.size(); ++i) std::memset(&d.thin_facts[i], int(1 + i % 200), sizeof(ThinDrawFacts));
      d.has_thin = (combo & 1) != 0, d.has_d3 = (combo & 2) != 0;
      std::memset(&d.thin_stamp, d.has_thin ? 0x5A : 0, sizeof d.thin_stamp);
      std::memset(&d.d3_counts, d.has_d3 ? 0x33 : 0, sizeof d.d3_counts);
      if (handoff::Serialize(d) != OldBytes(d)) Fail("combo: Serialize differs from the frozen pre-T8 writer", 90000 + combo);
      LoadedFrame b, c;
      b.bytes = handoff::Serialize(d);
      b.constant_banks.UsePool(&pool);
      CaptureReader::ParseFrame(b, 1);
      c.constant_banks.UsePool(&pool);
      handoff::BindInto(d, c, 1);
      Same(b, c, 90000 + combo, rng, true);
      if (b.thin.count != (d.has_thin ? 1u : 0u) || b.d3.count != (d.has_d3 ? 1u : 0u) || c.thin.count != b.thin.count || c.d3.count != b.d3.count) Fail("combo: thin / d3 presence", 90000 + combo);
    }
    // a reused LoadedFrame must not keep the previous frame's d3 / thin (F5)
    handoff::FrameData withs = MakeData(a), without = MakeData(a);
    withs.has_d3 = withs.has_thin = true;
    without.has_d3 = without.has_thin = false;
    LoadedFrame r;
    handoff::BindInto(withs, r, 1);
    handoff::BindInto(without, r, 1);
    if (r.d3.count || r.thin.count) Fail("a reused LoadedFrame kept the previous frame's d3 / thin", 95000);
    // SwapTables moves every table, both ways (FakeTables has FrameTables' member names)
    {
      handoff::FrameData src = MakeData(a), dst;
      FakeTables ft;
      handoff::SwapTables(ft, src);   // ft takes src's tables
      handoff::SwapTables(ft, dst);   // dst takes them from ft
      handoff::FrameData want = MakeData(a);
      dst.h = want.h, dst.has_thin = want.has_thin, dst.has_d3 = want.has_d3, dst.thin_stamp = want.thin_stamp, dst.d3_counts = want.d3_counts;   // (SwapTables moves tables only)
      if (handoff::Serialize(dst) != handoff::Serialize(want)) Fail("SwapTables lost a table", 96000);
      if (!ft.draws.empty() || !src.draws.empty()) Fail("SwapTables left a table behind", 96001);
    }
    // the decision: switch off -> bytes; switch on + ring on -> bytes, count 0, the reason line (first_forced once); switch on + ring off -> memory, counted
    {
      bool first = false;
      const uint64_t m0 = handoff::g_in_memory.load(), r0 = handoff::g_ring_forced_bytes.load();
      if (handoff::Decide(false, true, &first) != handoff::Way::kBytesSwitchOff || first) Fail("decide: switch off", 97000);
      if (handoff::Decide(true, true, &first) != handoff::Way::kBytesRingOn || !first) Fail("decide: ring on + switch on must serialise and say why once", 97001);
      if (handoff::Decide(true, true, &first) != handoff::Way::kBytesRingOn || first) Fail("decide: the reason is given once", 97002);
      if (handoff::g_in_memory.load() != m0) Fail("decide: ring on counted a frame in memory", 97003);
      if (handoff::Decide(true, false, &first) != handoff::Way::kMemory || handoff::g_in_memory.load() != m0 + 1) Fail("decide: ring off + switch on must hand over and count", 97004);
      if (handoff::g_ring_forced_bytes.load() != r0 + 2) Fail("decide: forced count", 97005);
    }
    // the pool: an oversized vector is dropped, the idle count is capped by SetKeep
    {
      handoff::Pool pl;
      pl.SetKeep(2);
      { auto p = pl.Take(); p->draws.assign(100000, Draw{}); p->draws.resize(10); }
      auto q = pl.Take();
      if (q->draws.capacity() > 4 * 64) Fail("pool kept an oversized vector", 98000);
      std::vector<std::shared_ptr<handoff::FrameData>> hold;
      for (int i = 0; i < 5; ++i) hold.push_back(pl.Take());
      hold.clear();
      q.reset();
      if (pl.idle() > 2) Fail("pool kept more than SetKeep", 98001);
    }
  }
  const double n = double(frames.size());
  std::printf("%s: %zu frames, %.0f KB a frame on average (origins in %llu frames, patches in %llu, strings in %llu, thin facts in %llu); the byte path's own bytes equal the file's in %llu frames; %d differences\n",
              fails ? "FAIL" : "PASS", frames.size(), double(total_bytes) / n / 1024.0, (unsigned long long)with_origins, (unsigned long long)with_patches, (unsigned long long)with_strings,
              (unsigned long long)with_thin_facts, (unsigned long long)byte_identical, fails);
  std::printf("TIME ms a frame on this PC (direction only, -O1, one thread, warm): swap thread: serialise %.3f vs hand over (table swaps) %.4f; render thread: ParseFrame %.4f vs BindInto %.4f\n",
              t_serialize / n, t_swap / n, t_parse / n, t_bind / n);
  return fails ? 1 : 0;
}
