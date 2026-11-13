// wwe2k14 - Session 101 lane 17: offline harness for texture provenance (docs\renderer\NAMES.md "Texture provenance"). Runs the REAL code:
// renderer_adapter/prov_map.h (page map + label rule), host_write.h (BytesRead, the hook), renderer_adapter/content_fit.h (case 7041's rule).
// Prints one line per case, "CASE <name> PASS|FAIL"; exit 1 when any failed. tools\prov\selftest.py builds it, then builds mutated copies
// of the headers (each fix undone) and demands the matching case FAILS (the planted faults).
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "host_write.h"
#include "renderer_adapter/content_fit.h"
#include "renderer_adapter/prov_map.h"

using namespace wwe2k14;
using W = host_write::Writer;
using rdr_adapter::Provenance;

namespace {
int g_fail = 0;
void Case(const char* name, bool ok, const char* why = "") {
  std::printf("CASE %s %s%s%s\n", name, ok ? "PASS" : "FAIL", ok ? "" : " - ", ok ? "" : why);
  if (!ok) ++g_fail;
}
constexpr uint32_t kTex = 0xA0100000u;   // a physical-view texture page run
constexpr uint32_t kTex2 = 0xA0200000u;
constexpr uint32_t kHeapA = 0x40001000u;  // heap buffers
constexpr uint32_t kHeapB = 0x40100000u;

// The reader's glue in miniature: a hook that records into the map the way OnHostWriteBy does (the frame is a global here).
uint32_t g_frame = 1;
bool g_guard_on = false, g_recording = false;
void GuardedHook(uint32_t addr, uint32_t length, W w, const char* file, uint32_t source) {  // the shape of reader.h OnHostWriteBy
  if (!prov::ShouldRecord(g_guard_on, g_recording)) return;
  prov::Map().Note(addr, length, w, file, source, g_frame);
}
void TestHook(uint32_t addr, uint32_t length, W w, const char* file, uint32_t source) { prov::Map().Note(addr, length, w, file, source, g_frame); }
void Note(uint32_t addr, uint32_t len, W w, const char* file = nullptr, uint32_t src = 0) { prov::Map().Note(addr, len, w, file, src, g_frame); }
struct FakeReg { uint32_t u32 = 0; };
struct FakeCtx { FakeReg r3, r4, r5; };
struct FakeRanges {
  struct R { uint32_t addr, size; } r[16];
  int n = 0;
  void Add(uint32_t a, uint32_t z) { r[n++] = {a, z}; }
};
struct FakeMemcpy {  // the shape of nf_memcpy_fwd::Spec / nf_memcpy_vmx::Spec (memcpy(r3 dst, r4 src, r5 n))
  static constexpr bool kNotifyWrites = true;
  static constexpr auto kWriter = host_write::Writer::Memcpy;
  static uint32_t Source(const FakeCtx& c) { return c.r4.u32; }
  static void Writes(const FakeCtx& c, uint8_t*, FakeRanges& o) { o.Add(c.r3.u32, c.r5.u32); }
};
prov::Label LabelOf(uint32_t tex, prov::ImageFacts f = {}) { return prov::Classify(f, prov::Map().LoadPhysical(tex)); }
bool Has(const char* s, const char* part) { return s && std::strstr(s, part); }
bool Is(const prov::Label& l, Provenance p, const char* writer = nullptr) {
  return l.source == p && (!writer || (l.writer && std::strcmp(l.writer, writer) == 0));
}
}  // namespace

int main() {
  prov::PageMap& map = prov::Map();
  // ---- I1: a file read labels the bytes actually read -------------------------------------------------------------------------------
  {
    const bool a = host_write::BytesRead(0, 8192, 4096) == 4096 && host_write::BytesRead(0, 8192, 8192) == 8192 &&
                   host_write::BytesRead(0, 4096, 9999) == 4096 && host_write::BytesRead(0, 8192, 0) == 0 &&
                   host_write::BytesRead(int32_t(0xC0000001u), 8192, 8192) == 0 && host_write::BytesRead(0x103, 8192, 0, false, 0x103) == 8192 &&
                   // an overlapped (ReadFileEx) read: PENDING returned, but the SDK has filled the block (status ok, count): the count rules
                   host_write::BytesRead(0x103, 8192, 4096, false, 0) == 4096 && host_write::BytesRead(0x103, 8192, 0, false, int32_t(0xC0000001u)) == 0 &&
                   host_write::BytesRead(0x103, 4096, 8192, false, 0) == 4096 &&
                   host_write::BytesRead(0, 8192, 0, true) == 8192;
    // end to end: a short read of 4096 of 8192 requested names one page, the second stays unnamed
    map.Reset();
    g_frame = 3;
    host_write::g_writer_hook.store(&TestHook);
    const uint32_t got = host_write::BytesRead(0, 8192, 4096);
    if (got) host_write::NoteBy(kTex, got, W::FileRead, "pac\\a.pac");
    const bool b = Is(LabelOf(kTex), Provenance::File) && Is(LabelOf(kTex + 4096), Provenance::Unnamed);
    host_write::g_writer_hook.store(nullptr);
    Case("I1.bytes_read", a && b, a ? "the second page was labelled FILE (requested length)" : "BytesRead wrong");
  }
  // ---- I2: a memcpy inherits its source's FILE owner --------------------------------------------------------------------------------
  {
    map.Reset();
    g_frame = 5;
    Note(kTex, 8192, W::FileRead, "pac\\b.pac");  // pages 0,1: FILE b.pac
    g_frame = 6;
    Note(kTex + 4096, 4096, W::CrowdSkin);        // page 1 rewritten by a named CPU writer
    g_frame = 7;
    Note(kTex2, 8192, W::Memcpy, nullptr, kTex);  // copy both pages
    const prov::Label p0 = LabelOf(kTex2), p1 = LabelOf(kTex2 + 4096);
    Case("I2a.memcpy_page_by_page", Is(p0, Provenance::File) && (p0.flags & rdr_adapter::kProvGuessed) && Is(p1, Provenance::Cpu, "memcpy"),
         "page 1 of the copy took page 0's FILE owner (the source's second page was CPU-written)");
    // chain: a copy of a copy stays FILE with the same file
    g_frame = 8;
    Note(kTex2 + 0x100000, 4096, W::Memcpy, nullptr, kTex2);
    const prov::Label c = LabelOf(kTex2 + 0x100000);
    Case("I2d.memcpy_chain", Is(c, Provenance::File) && c.file && c.file == p0.file, "a copy of a copy lost the file");
  }
  {  // heap staging: file -> heap A (loader), memcpy A -> heap B, memcpy B -> texture
    map.Reset();
    g_frame = 10;
    Note(kHeapA, 8192, W::FileRead, "pac\\c.pac");
    g_frame = 11;
    Note(kHeapB, 8192, W::Memcpy, nullptr, kHeapA);
    g_frame = 12;
    Note(kTex, 8192, W::Memcpy, nullptr, kHeapB);
    const prov::Label l = LabelOf(kTex);
    Case("I2b.heap_staging_chain", Is(l, Provenance::File), "a staged copy (heap to heap to texture) labelled CPU");
  }
  {  // a heap buffer overwritten by a reported non-loader write no longer lends its FILE owner
    map.Reset();
    g_frame = 20;
    Note(kHeapA, 4096, W::FileRead, "pac\\d.pac");
    g_frame = 21;
    Note(kHeapA, 4096, W::CrowdSkin);
    g_frame = 22;
    Note(kTex, 4096, W::Memcpy, nullptr, kHeapA);
    Case("I2c.heap_overwritten_not_file", Is(LabelOf(kTex), Provenance::Cpu, "memcpy") && LabelOf(kTex).detail && std::strcmp(LabelOf(kTex).detail, "memcpy_src_cpu") == 0,
         "a heap page rewritten after its load still lent FILE, or is called \"no entry\" (it is CPU bytes now)");
  }
  // ---- I2e: through the real Dispatch wrapper: the source is read before the call (the natives leave other values in r4) ----------------
  {
    map.Reset();
    g_frame = 25;
    host_write::g_writer_hook.store(&TestHook);
    Note(kTex, 4096, W::FileRead, "pac\\i.pac");
    FakeCtx ctx;
    ctx.r3.u32 = kTex2; ctx.r4.u32 = kTex; ctx.r5.u32 = 4096;
    host_write::CallReporting<FakeMemcpy, FakeRanges>(ctx, nullptr, [&] { ctx.r4.u32 += 4096 + 128; ctx.r3.u32 = 0; });  // the native clobbers r4
    host_write::g_writer_hook.store(nullptr);
    Case("I2e.dispatch_reads_source_before_call", Is(LabelOf(kTex2), Provenance::File), "the destination did not inherit: Source was read after the call");
  }
  // ---- I7: the menu path: pac entry block (ZLIB / BPE) read from a plain pac, decoded by CData::Copy's decoders, copied into the texture ------
  // (S101 lane17, Check B: the menu's textures all labelled CPU "memcpy": the decoders' output was unreported, so the copy's source had no owner)
  {
    map.Reset();
    g_frame = 60;
    host_write::g_writer_hook.store(&TestHook);
    Note(kHeapA, 8192, W::FileRead, "pac\\menu\\menuHD.pac");        // the stored block, read from the plain EPAC pac
    Note(kHeapA + 0x300000, 4096, W::FileRead, "pac\\other.pac");   // a later read of ANOTHER file: the newest read is not the block's file
    host_write::NoteDecoded(kHeapB, 32768, kHeapA);                  // BPE / zlib decode into a staging buffer (source = the stored block)
    Note(kTex, 16384, W::Memcpy, nullptr, kHeapB);                   // memcpy_vmx staging -> texture memory
    host_write::NoteDecoded(kTex2, 8192, kHeapA);                    // or decoded straight into texture memory
    Note(kTex2 + 0x80000, 4096, W::CrowdSkin);                         // an input the CPU made, not read from a file
    host_write::NoteDecoded(kTex2 + 0x10000, 4096, kTex2 + 0x80000);
    host_write::NoteDecoded(kTex2 + 0x20000, 4096, kHeapA + 0x500000);  // an input page with NO entry (read before recording started: boot / menu)
    host_write::g_writer_hook.store(nullptr);
    const prov::Label unknown_in = LabelOf(kTex2 + 0x20000);
    const prov::Label via = LabelOf(kTex), direct = LabelOf(kTex2), cpu_made = LabelOf(kTex2 + 0x10000);
    const char* fname = map.FileName(map.LoadPhysical(kTex).file);
    Case("I7.decoded_block_keeps_file_owner", Is(via, Provenance::File) && Is(direct, Provenance::File) && fname && std::strstr(fname, "menuHD.pac") &&
                                                   !std::strstr(fname, "other.pac") && Has(map.FileName(map.LoadPhysical(kTex2).file), "menuHD.pac") &&
                                                   Is(cpu_made, Provenance::Cpu, "memcpy") &&
                                                   Is(unknown_in, Provenance::File) && (unknown_in.flags & rdr_adapter::kProvGuessed),
         "a decoded (BPE / zlib) block lost its pac owner on the way to the texture");
  }
  // ---- I8: the census (Check C: "cpu 19699 memcpy, file 0"): where a memcpy into texture memory got its bytes -----------------------------
  {
    map.Reset();
    (void)map.TakeCensus();
    g_frame = 70;
    Note(kTex, 4096, W::Memcpy, nullptr, kHeapB + 0x700000);   // source never written by a hooked writer: kind 1
    const prov::Label none = LabelOf(kTex);
    Note(kTex2, 4096, W::Memcpy, nullptr, kTex);                // a copy of that copy: kind 3, still "no entry" at the root
    const prov::Label chain = LabelOf(kTex2);
    Note(kTex + 0x40000, 4096, W::CrowdSkin);
    Note(kTex2 + 0x40000, 4096, W::Memcpy, nullptr, kTex + 0x40000);  // a non-loader writer's bytes: kind 2
    const prov::Label cpu = LabelOf(kTex2 + 0x40000);
    Note(kHeapA, 4096, W::FileRead, "pac\\menu\\fontHD.pac");
    Note(kTex2 + 0x80000, 4096, W::Memcpy, nullptr, kHeapA);    // a loader's bytes: kind 0
    const prov::PageMap::Census c = map.TakeCensus();
    const bool counts = c.kind[0] == 1 && c.kind[1] == 1 && c.kind[2] == 1 && c.kind[3] == 1 && c.kind[4] == 0;
    const bool samples = c.samples == 2 && c.src[0] == kHeapB + 0x700000 && c.dst[0] == kTex && c.src[1] == kTex && c.len[0] == 4096;
    const bool details = none.detail && std::strcmp(none.detail, "memcpy_src_none") == 0 && chain.detail && std::strcmp(chain.detail, "memcpy_src_none") == 0 &&
                         cpu.detail && std::strcmp(cpu.detail, "memcpy_src_cpu") == 0 && Is(LabelOf(kTex2 + 0x80000), Provenance::File);
    Case("I8.census_memcpy_sources", counts && samples && details, "the census / detail does not say where the memcpy's bytes came from");
  }
  // The Check C miss, offline, through the real switches (prov::WantedAtStart installs the hook at app start, prov::ShouldRecord is the hook's guard):
  // the pac block was read at frame 0, before the live mirror (frame ~55); the copy into the texture came later.
  //   players (nothing asked for): no hook at start -> nothing is hooked, nothing is paid; the late copy finds a source with no entry (CPU memcpy_src_none);
  //   provenance asked for (cvar / case ring / +dev owner / capture at start): hooked at start -> the same sequence is FILE.
  {
    struct Boot { bool cvar, ring, dev, capture; };
    auto run = [&](Boot b, bool* installed_at_start, bool late_install = false) {
      map.Reset();
      host_write::g_writer_hook.store(nullptr);
      const bool on = prov::WantedAtStart(b.cvar, b.ring, b.dev, b.capture);
      if (on) prov::InstallWriterHook(&GuardedHook);
      *installed_at_start = host_write::g_writer_hook.load() != nullptr;
      g_guard_on = on;
      g_recording = false;  // boot: no capture, no live mirror yet
      g_frame = 0;
      host_write::NoteBy(kHeapA, 8192, W::FileRead, "pac\\_startup.pac");
      g_recording = true;  // the live mirror starts (~frame 55)
      if (late_install) { g_guard_on = true; prov::InstallWriterHook(&GuardedHook); }  // the old way: the hook went in at StartLive
      g_frame = 55;
      host_write::NoteBy(kTex, 8192, W::Memcpy, nullptr, kHeapA);
      const prov::Label l = LabelOf(kTex);
      host_write::g_writer_hook.store(nullptr);
      return l;
    };
    bool players_installed = true, cvar_installed = false, ring_installed = false, dev_installed = false, capture_installed = false;
    const prov::Label players = run({false, false, false, false}, &players_installed);
    const prov::Label by_cvar = run({true, false, false, false}, &cvar_installed);
    (void)run({false, true, false, false}, &ring_installed);
    (void)run({false, false, true, false}, &dev_installed);
    (void)run({false, false, false, true}, &capture_installed);
    const prov::Label by_ring = run({false, true, false, false}, &ring_installed);
    bool late_unused = false;
    const prov::Label late = run({false, false, false, false}, &late_unused, true);  // the miss itself: hooked only from StartLive
    Case("I8b.read_before_the_hook_is_the_menu_miss",
         !players_installed && Is(players, Provenance::Unnamed) && Is(late, Provenance::Cpu, "memcpy") && late.detail && std::strcmp(late.detail, "memcpy_src_none") == 0 &&
             cvar_installed && ring_installed && dev_installed && capture_installed && Is(by_cvar, Provenance::File) && Is(by_ring, Provenance::File),
         "players must install nothing; each way of asking for provenance must install at start and keep the frame-0 read's FILE owner");
  }
  // ---- I3: a FILE texture rewritten by guest code and flushed is CPU: EVENT order, not frame order ------------------------------------
  {
    map.Reset();
    g_frame = 50;
    Note(kTex, 4096, W::FileRead, "pac\\e.pac");
    prov::ImageFacts f;
    f.flush_seen = true;
    // (a) the loader's own unlock flush comes before the reader's first bind: not a rewrite, whatever the frame numbers say
    f.flush_frame = 99; f.flush_seq = map.Stamp(99); f.first_read_seq = map.Stamp(99);
    const prov::Label a = LabelOf(kTex, f);
    // (b) read first, then flushed: guest code rewrote it; and the frame counter being LOWER (a worker's clock) does not matter
    f.first_read_seq = map.Stamp(10); f.flush_frame = 10; f.flush_seq = map.Stamp(10);
    const prov::Label b = LabelOf(kTex, f);
    // (c) a host write (a reload) after that flush: FILE again
    Note(kTex, 4096, W::FileRead, "pac\\e.pac");
    const prov::Label c = LabelOf(kTex, f);
    // (d) never flushed: FILE; its write frame is the file write's
    const prov::Label d = LabelOf(kTex, prov::ImageFacts{});
    Case("I3.file_rewritten_then_flushed", Is(a, Provenance::File) && Is(b, Provenance::Cpu, "unlock_flush") && b.last_write_frame == 10 && Is(c, Provenance::File) &&
                                               Is(d, Provenance::File) && d.last_write_frame == 50,
         "a FILE texture flushed after its first read stayed FILE, or one flushed before it became CPU");
  }
  // ---- I4: source_image of a copy ------------------------------------------------------------------------------------------------
  {
    prov::ImageFacts f;
    f.copy_dest = true; f.copy_source = 17;
    const prov::Label c = prov::Classify(f, prov::Entry{});
    prov::ImageFacts p;
    p.packed = true; p.copy_dest = true; p.copy_source = 23;
    const prov::Label pk = prov::Classify(p, prov::Entry{});
    Case("I4.copy_source_image", Is(c, Provenance::Gpu) && c.source_image == 17, "a copy destination's source_image is not filled");
    Case("I4b.packed_source_image", Is(pk, Provenance::Gpu, "attire_pack") && pk.source_image == 23, "the attire pack's source_image is not filled");
  }
  // ---- I5: the writer hook is set where capture arms (the hook install, then a report lands) ---------------------------------------------
  {
    map.Reset();
    host_write::g_writer_hook.store(nullptr);
    g_frame = 40;
    host_write::NoteBy(kTex, 4096, W::FileRead, "pac\\f.pac");  // no hook: nothing recorded
    const bool before = Is(LabelOf(kTex), Provenance::Unnamed);
    prov::InstallWriterHook(&TestHook);
    host_write::NoteBy(kTex, 4096, W::FileRead, "pac\\f.pac");
    const bool after = Is(LabelOf(kTex), Provenance::File);
    host_write::g_writer_hook.store(nullptr);
    Case("I5.hook_installed", before && after, "InstallWriterHook did not install the hook");
  }
  // ---- I6: the flat map ----------------------------------------------------------------------------------------------------------------
  {
    map.Reset();
    g_frame = 50;
    Note(0xFFFFF000u, 4096, W::VertexUnlock);  // the top of the 0xE0000000 view: physical page 0x20000
    Note(0xA0000000u, 4096, W::Memcpy);        // page 0
    Note(0xBFFFF000u, 4096, W::CrowdSkin);     // last page of the 0xA0000000 view
    const bool top = map.LoadPhysical(0xFFFFF000u).writer == W::VertexUnlock && map.Load(0xA0000000u).writer == W::Memcpy &&
                     map.Load(0xBFFFF000u).writer == W::CrowdSkin && map.Load(0xA0001000u).valid == false;
    Note(0xA0010000u, 0xFFFFFFFFu, W::Memcpy);  // a wrapped range: ignored
    Note(0xA0010000u, 0x5FFE0000u, W::Memcpy);  // absurd (131000 pages): ignored
    const bool bad = !map.Load(0xA0010000u).valid;
    Case("I6.flat_map_bounds", top && bad, "page index out of range or an absurd range written");
  }
  {  // concurrency: two writers and a reader on one page, 1000 times; an entry's writer and sequence always belong together
    int torn = 0;
    for (int run = 0; run < 1000; ++run) {
      map.Reset();
      std::atomic<bool> go{false};
      std::atomic<bool> done{false};  // the writers hammer the page until the reader is finished, so all three overlap
      std::vector<uint32_t> sa, sb;  // the sequence each writer got for each of its writes (ascending per thread)
      sa.reserve(1 << 20); sb.reserve(1 << 20);
      auto writer_a = [&] { while (!go) {} for (uint32_t i = 0; !done && sa.size() < (1u << 20); ++i) sa.push_back(map.Note(kTex, 4096, W::CrowdSkin, nullptr, 0, 7)); };
      auto writer_b = [&] { while (!go) {} for (uint32_t i = 0; !done && sb.size() < (1u << 20); ++i) sb.push_back(map.Note(kTex, 4096, W::Memcpy, nullptr, 0, 7)); };
      std::vector<std::pair<W, uint32_t>> seen;
      seen.reserve(20000);
      auto reader = [&] {
        while (!go) {}
        for (int i = 0; i < 20000; ++i) {
          const prov::Entry e = map.LoadPhysicalRaw(kTex);
          if (e.valid) seen.emplace_back(e.writer, e.seq);
        }
        done = true;
      };
      std::thread ta(writer_a), tb(writer_b), tr(reader);
      go = true;
      ta.join(); tb.join(); tr.join();
      // an entry is whole when its writer and its sequence belong to the same write
      for (const auto& [w, seq] : seen) {
        const std::vector<uint32_t>& mine = w == W::CrowdSkin ? sa : sb;
        if (!std::binary_search(mine.begin(), mine.end(), seq)) ++torn;
      }
    }
    Case("I6b.lock_free_no_torn_entry", torn == 0, "a torn entry was read");
  }
  // ---- R1: Session 100's "unannounced writes" class (SESSION100_SUMMARY.md lines 152 / 153 / 160; RENDERER_ARC.md 2.1 ledger row, ~20 runs) --------
  // The bit: content changed with no report (the game's own code wrote it), found only by page watches. Sequence that bit then: a load, then a
  // guest rewrite with no announce, only the game's flush. The label must now say who wrote last (CPU), not the load (FILE).
  {
    map.Reset();
    g_frame = 100;
    Note(kTex, 8192, W::FileRead, "pac\\g.pac");     // the load
    // frame 104: guest code rewrites the texture, no host report; the game flushes it (OnUnlockFlush stamps the image)
    prov::ImageFacts f;
    f.first_read_seq = map.Stamp(102);  // the reader bound it (frame 102) ...
    f.flush_seen = true; f.flush_frame = 104; f.flush_seq = map.Stamp(104); f.flushed = true;  // ... then the game flushed its rewrite
    const prov::Label l = LabelOf(kTex, f);
    // the vertex-buffer case (run 6989: written by sub_82C14BC0 in the packer, unlocked by sub_82CACE50): the unlock report names the writer
    g_frame = 110;
    Note(kTex2, 4096, W::FileRead, "pac\\h.pac");
    g_frame = 111;
    Note(kTex2, 4096, W::VertexUnlock);
    const prov::Label v = LabelOf(kTex2);
    Case("R1.unannounced_writes_s100", Is(l, Provenance::Cpu, "unlock_flush") && l.last_write_frame == 104 && Is(v, Provenance::Cpu, "vertex_unlock") &&
                                           v.last_write_frame == 111,
         "the label named the load, not the last writer");
  }
  // ---- R2: case 7041 (fec23058): a content for an older record of an image recreated in another format is stale ------------------------------
  {
    std::vector<uint32_t> formats = {0, 7, 7, 12};  // image 3 was recreated in format 12
    const bool stale = rdr_adapter::ContentIsStale(formats, 3, 7);       // the old record's content (format 7)
    const bool fits = !rdr_adapter::ContentIsStale(formats, 3, 12) && !rdr_adapter::ContentIsStale(formats, 1, 7);
    const bool unmade = !rdr_adapter::ContentIsStale(formats, 0, 5) && !rdr_adapter::ContentIsStale(formats, 99, 5);  // not made yet / unknown id
    Case("R2.case7041_stale_content", stale && fits && unmade, "a stale content would be uploaded");
  }
  std::printf("%s: %d failed\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}
