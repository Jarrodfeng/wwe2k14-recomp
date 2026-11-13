// Lane 231 (LEAVE_THE_COMMAND_STREAM 3a): the boot submit answered by us, against a model of what the SDK's command processor does with the same packets.
//   boot_submit_test.exe               honest run: every check must PASS (exit 0)
//   boot_submit_test.exe plant NAME    one planted fault, which must make a named check FAIL (exit 1)
// Shipped code under test: src/owner_boot_submit.h (Scan, AnswerBootSubmit, RingUsed), the very functions src/owner_timing.h calls.
// The reference is written here from the SDK's packet handlers (thirdparty/rexglue-sdk-src/src/graphics/command_processor.cpp: type-0 + scratch
// write-back 1314-1342, ME_INIT 2581, EVENT_WRITE_SHD 3422, MEM_WRITE 3311, COND_WRITE 3331, REG_TO_MEM 3292, read-pointer write-back 1117): a primary buffer
// of ME_INIT + one INDIRECT_BUFFER packet, the segment behind it, executed in order, then the read pointer stored. The words the game reads (R): fence block
// +0 / +4, scratch block +0 / +4 / +16 / +20, and what the ring-room check 0x82CB54B0 derives from the write index and the read pointer (fence block +60).
// Packet words are the ones the listings write: the init-idle 0x82CB7228 (ME_INIT 19 dwords; 0xD02, 0xA02..0xA05, SCRATCH_ADDR, SCRATCH_UMSK) and 0x82CB7078
// (WAIT_UNTIL, 0xD04), plus the per-present classes (0x82CC42A8's EVENT_WRITE_SHD, the gamma COND_WRITE to a register, VdSwap, draws, constants, waits).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/owner_boot_submit.h"

namespace ob = wwe2k14::owner_boot;
using Words = std::vector<uint32_t>;
using Mem = std::map<uint32_t, uint32_t>;
static std::map<uint32_t, Words> g_bufs;  // indirect-buffer targets by GPU address
struct TestReader : ob::IbReader {
  bool Read(uint32_t gpu, uint32_t dwords, std::vector<uint32_t>& out) override {
    auto it = g_bufs.find(gpu);
    if (it == g_bufs.end() || it->second.size() < dwords) return false;
    out.assign(it->second.begin(), it->second.begin() + dwords);
    return true;
  }
};

static int g_fail = 0;
static void Check(bool ok, const char* name, const std::string& detail) {
  std::printf("%s  %-34s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
  g_fail += !ok;
}

static constexpr uint32_t kFence = 0x137FF000, kScratch = 0x137FE000, kSeg = 0x13400000, kRingMask = 8191;
static constexpr uint32_t kRing0 = 19;  // the ring write index after the boot's raw write (ME_INIT, 19 dwords)

// ---- the packets -----------------------------------------------------------------------------------------------------------------------------------------
static Words MeInit() {
  Words w = {0xC0114800, 1023, 0, 0};
  for (int i = 0; i < 8; ++i) w.push_back(0);  // the 8 words copied from a table (0x82082AD0): not read by anything
  const Words tail = {0x00010800, 7, 0, 0, 0, 0, 0};
  w.insert(w.end(), tail.begin(), tail.end());
  return w;  // 19 dwords
}
static Words BootSegment() {  // 0x82CB7228's segment words, then 0x82CB7078's
  return {0x00000D02, 0x00010800, 0x00030A02, 0xC0100000, 0x07F00000, 0xC0000000, 0x00100000, 0x000001DD, kScratch, 0x000001DC, 0x00020037,
          0x000005C8, 0x00020000, 0x00000D04, 0x00000000};
}
static Words EventWriteShd(uint32_t addr) { return {0xC0025800, 0x80000003, addr, 0xDEADBEEF}; }           // 0x82CC42A8
static Words XeSwap() { return {0xC0036400, 0x53574150, 0x1B000000, 1280, 720}; }                          // VdSwap's packet (shortened)
static Words GammaCondWrite() { return {0xC0054500, 7, 6437, 0x1234, 0xFFFFFFFF, 6434, 5}; }              // 0x82CBABB0: register form
static Words Draw() { return {0xC0013600, 0x1, 0x2}; }
static Words SetConstant() { return {0xC0012D00, 0x1, 0x2}; }
static Words WaitRegMem() { return {0xC0043C00, 0x10, kFence, 1, 0xFFFFFFFF, 10}; }
static Words MemWrite(uint32_t addr, uint32_t v) { return {0xC0013D00, addr, v}; }
static Words CondWriteMem(uint32_t addr, uint32_t v) { return {0xC0054500, 0x107, 0, 0, 0xFFFFFFFF, addr, v}; }
static Words RegToMem(uint32_t addr) { return {0xC0013E00, 0x578, addr}; }
static Words ScratchReg1(uint32_t v) { return {0x00000579, v}; }                                           // type 0, one register
static Words Cat(std::initializer_list<Words> parts) {
  Words o;
  for (auto& p : parts) o.insert(o.end(), p.begin(), p.end());
  return o;
}

// ---- the reference: the SDK's processor on the same packets ------------------------------------------------------------------------------------------------
struct RefCp {
  Mem regs;
  Mem* mem = nullptr;
  uint32_t counter = 7;
  void Write(uint32_t reg, uint32_t v) {
    regs[reg] = v;
    if (reg >= 0x578 && reg <= 0x57F && ((1u << (reg - 0x578)) & regs[0x1DC])) (*mem)[regs[0x1DD] + (reg - 0x578) * 4] = v;
  }
  void Exec(const Words& d) {
    size_t i = 0;
    while (i < d.size()) {
      const uint32_t h = d[i], type = h >> 30, count = ((h >> 16) & 0x3FFF) + 1;
      if (type != 2 && i + 1 + count > d.size()) break;  // (the SDK's overflow error: the rest of the buffer is dropped)
      if (type == 0) {
        for (uint32_t k = 0; k < count; ++k) Write(((h >> 15) & 1) ? (h & 0x7FFF) : (h & 0x7FFF) + k, d[i + 1 + k]);
        i += 1 + count;
      } else if (type == 2) {
        ++i;
      } else {
        const uint32_t opc = (h >> 8) & 0x7F;
        const uint32_t* p = &d[i + 1];
        if (opc == 0x58) (*mem)[p[1] & ~3u] = (p[0] >> 31) ? counter : p[2];
        else if (opc == 0x3D) for (uint32_t k = 0; k + 1 < count; ++k) (*mem)[(p[0] & ~3u) + 4 * k] = p[1 + k];
        else if (opc == 0x45) {
          if (p[0] & 0x100) (*mem)[p[4] & ~3u] = p[5];
          else Write(p[4], p[5]);
        } else if (opc == 0x3E) (*mem)[p[1] & ~3u] = regs[p[0]];
        else if (opc == 0x3F) {
          auto b = g_bufs.find(p[0]);
          if (b != g_bufs.end()) Exec(Words(b->second.begin(), b->second.begin() + std::min<size_t>(b->second.size(), p[1] & 0xFFFFF)));
        }
        // everything else (state, draws, waits that are satisfied, swap, ME_INIT, NOP): no guest memory
        i += 1 + count;
      }
    }
  }
};

// The original flow: the library's raw write put ME_INIT in the ring (wr = 19); the submit appends an IB packet (3 dwords: wr = 22), the processor runs ME_INIT, the
// IB (the segment) and stores the read pointer (22). `mem` starts as the library's own init stores (0x82CB7228: fence block +0 = fence-2, +60 = 0).
static void RunOriginal(Mem& mem, const Words& seg, uint32_t& wr, Mem* regs_out = nullptr) {
  RefCp cp;
  cp.mem = &mem;
  cp.Exec(MeInit());
  cp.Exec(seg);
  if (regs_out) *regs_out = cp.regs;
  wr = kRing0 + 3;
  mem[kFence + 60] = wr;
}
static Mem Init() {
  Mem m;
  m[kFence + 0] = 1;  // fence number 3 - 2
  m[kFence + 4] = 0x0100001F;
  m[kFence + 60] = 0;
  m[kScratch + 0] = 5;  // junk the boot's reset must clear
  m[kScratch + 8] = 9;
  m[kScratch + 4] = 1;
  m[kScratch + 16] = 0x1234;
  m[kScratch + 20] = 7;
  return m;
}

struct Outcome {
  Mem mem;
  uint32_t wr = 0;
  ob::Answer a;
  bool original = false;
  Mem regs;  // the processor register file after the boot (the original: the model's; ours: what AnswerBoot stored)
};
// Ours: the shipped scan + answer; a segment the scan cannot vouch for goes to the original (what owner_timing.h does).
static Outcome Ours(const Words& seg, uint32_t blind = 0, const std::string& plant = "") {
  Outcome o;
  o.mem = Init();
  o.wr = kRing0;
  TestReader reader;
  o.a = ob::AnswerBootSubmit(seg.data(), uint32_t(seg.size()), kFence, kFence, kScratch, o.wr, &reader, blind);
  if (o.a.result != ob::Result::Answered) {
    o.original = true;
    RunOriginal(o.mem, seg, o.wr, &o.regs);
    return o;
  }
  for (uint32_t i = 0; i < o.a.write_count; ++i) {
    if (plant == "rptr_unwritten") continue;                                          // the one word left unwritten
    uint32_t addr = o.a.writes[i].addr, v = o.a.writes[i].value;
    if (plant == "rptr_wrong") v += 3;                                                // the original's end index, but the write index stayed at 19
    if (plant == "wrong_block") addr = kScratch + 60;                                 // the right value in the wrong block
    o.mem[addr] = v;
  }
  if (plant != "scratch_unwritten") for (const ob::Write& w : o.a.census.scratch) o.mem[kScratch + w.addr] = w.value;  // the timing thread's queued writes
  if (plant != "no_register_writes") for (const ob::Write& w : o.a.census.regs) o.regs[w.addr] = w.value;  // AnswerBoot's copy into the SDK register file
  return o;
}
static Outcome Original(const Words& seg) {
  Outcome o;
  o.mem = Init();
  RunOriginal(o.mem, seg, o.wr, &o.regs);
  o.original = true;
  return o;
}

// R: compare every word the game reads. Returns the list of differing words.
static std::string Diff(const Outcome& ref, const Outcome& ours, bool& same) {
  std::string out;
  same = true;
  const uint32_t words[] = {kFence + 0, kFence + 4, kScratch + 0, kScratch + 4, kScratch + 8, kScratch + 16, kScratch + 20};
  const char* names[] = {"fence", "fence_marker", "scratch_cpu_mask", "scratch_flip_word", "scratch_word8", "scratch_callback", "scratch_context"};
  for (int i = 0; i < 7; ++i) {
    if (ref.mem.at(words[i]) != ours.mem.at(words[i])) {
      same = false;
      out += std::string(names[i]) + " ";
    }
  }
  const uint32_t ref_used = ob::RingUsed(ref.wr, ref.mem.at(kFence + 60), kRingMask), our_used = ob::RingUsed(ours.wr, ours.mem.at(kFence + 60), kRingMask);
  if (ref_used != our_used) {
    same = false;
    out += "ring_room(" + std::to_string(ref_used) + " vs " + std::to_string(our_used) + ") ";
  }
  return out;
}

int main(int argc, char** argv) {
  const std::string plant = argc >= 3 && std::strcmp(argv[1], "plant") == 0 ? argv[2] : "";
  uint32_t blind = 0;
  if (plant == "scan_blind_mem_write") blind = 1u << ob::kMemWrite;
  if (plant == "scan_blind_scratch") blind = 1u << ob::kScratchReg;
  if (plant == "scan_blind_ib") blind = 1u << ob::kIndirectFollowed;
  if (plant == "scan_blind_cond_write_mem") blind = 1u << ob::kCondWriteMem;
  if (plant == "scan_blind_bad") blind = 1u << ob::kBad;
  if (plant == "scan_blind_event_elsewhere") blind = 1u << ob::kEventWriteElsewhere;
  if (!plant.empty() && plant != "rptr_unwritten" && plant != "rptr_wrong" && plant != "wrong_block" && plant != "no_register_writes" && plant != "old_rule" && plant != "scratch_unwritten" && !blind) {
    std::printf("FAIL  unknown plant %s\n", plant.c_str());
    return 2;
  }
  const bool in_main = plant.empty() || plant == "rptr_unwritten" || plant == "rptr_wrong" || plant == "wrong_block" || plant == "no_register_writes" || plant == "old_rule" || plant == "scratch_unwritten";

  // ---- 1. the boot as the listings write it, with the per-present classes after it --------------------------------------------------------------------------------
  {
    const Words seg = Cat({BootSegment(), EventWriteShd(kFence + 64), XeSwap(), GammaCondWrite(), Draw(), SetConstant(), WaitRegMem()});
    Outcome ref = Original(seg), ours = Ours(seg, 0, in_main ? plant : "");
    bool same;
    const std::string d = Diff(ref, ours, same);
    Check(!ours.original, "boot.answered", "the boot segment (7228 + 7078 + present classes) scans clean and is answered, not handed to the SDK");
    Check(same, "boot.words_the_game_reads", same ? "fence, marker, scratch block and the ring room equal the original's" : "DIFFERENT: " + d);
    Check(ours.a.census.packets == 12 && ours.a.census.n[ob::kState] == 7 && ours.a.census.n[ob::kEventWriteShd] == 1 && ours.a.census.n[ob::kSwap] == 1 &&
              ours.a.census.n[ob::kCondWriteReg] == 1 && ours.a.census.n[ob::kDraw] == 1 && ours.a.census.n[ob::kWait] == 1,
          "boot.census", "12 packets: 6 boot register packets + SET_CONSTANT = 7 state, 1 event_write_shd, 1 swap, 1 cond_write(register), 1 draw, 1 wait (got " + std::to_string(ours.a.census.packets) + ")");
    // the flip-tracking slot the original wrote is NOT in R: nobody reads fence +64..+95 (reader scan: no lwz off the fence block pointer at +64..+95).
    Check(ref.mem.count(kFence + 64) == 1 && ours.mem.count(kFence + 64) == 0, "boot.unread_slot_left_out", "fence +64 (the flip-tracking counter) is written by the original only; no reader exists");
  }
  // the packet words of 0x82CB7228 / 0x82CB7078 themselves are state (no guest memory)
  {
    const Words seg = BootSegment();
    const ob::Census c = ob::Scan(seg.data(), uint32_t(seg.size()), kFence);
    Check(!c.NeedsOriginal() && c.packets == 6 && c.n[ob::kState] == 6, "boot.segment_is_state_only", "7228's four register packets (0xD02, 0xA02..0xA05, SCRATCH_ADDR, SCRATCH_UMSK) + 7078's two (WAIT_UNTIL, 0xD04) = " + std::to_string(c.packets) + " type-0 packets, no guest memory");
  }
  // ---- 2. a packet that writes a word the steady state does not rewrite goes to the original --------------------------------------------------------------------
  {
    const Words seg = Cat({BootSegment(), MemWrite(kFence + 4, 0x12345678)});
    Outcome ref = Original(seg), ours = Ours(seg, blind);
    bool same;
    const std::string d = Diff(ref, ours, same);
    Check(ours.original, "mem_write.goes_to_original", "a MEM_WRITE to the heap marker is not answered by us");
    Check(same, "mem_write.words_the_game_reads", same ? "equal to the original's" : "DIFFERENT: " + d);
  }
  {
    const Words seg = Cat({BootSegment(), ScratchReg1(2)});
    Outcome ref = Original(seg), ours = Ours(seg, blind, in_main ? plant : "");
    bool same;
    const std::string d = Diff(ref, ours, same);
    Check(!ours.original && ours.a.census.scratch.size() == 1 && ours.a.census.scratch[0].addr == 4, "scratch.answered_exactly", "a SCRATCH_REG1 write (SCRATCH_UMSK bit 1 set, SCRATCH_ADDR = the scratch block) is answered by writing scratch +4 itself");
    Check(same, "scratch.words_the_game_reads", same ? "equal to the original's" : "DIFFERENT: " + d);
  }
  {
    const Words seg = Cat({BootSegment(), CondWriteMem(kFence + 0, 99)});
    Outcome ref = Original(seg), ours = Ours(seg, blind);
    bool same;
    const std::string d = Diff(ref, ours, same);
    Check(ours.original, "cond_write_mem.goes_to_original", "a COND_WRITE to memory is not answered by us");
    Check(same, "cond_write_mem.words_the_game_reads", same ? "equal to the original's" : "DIFFERENT: " + d);
  }
  {
    Words seg = Cat({BootSegment(), MemWrite(kFence + 4, 5)});
    seg.pop_back();  // truncated: the packet overruns the segment
    Outcome ours = Ours(seg, blind);
    Check(ours.original, "truncated.goes_to_original", "a packet that overruns the segment is not answered by us (a segment cut mid-packet)");
  }

  // ---- P. run 7155: the first submit is the kick's prefix buffer, the second the open segment ------------------------------------------------------------------------
  {
    const Words prefix = {0x00000A31, 0x03000000, 0x00010A2F, 0x1000, 0x13400000, 0xC0043C00, 3, 0xA31, 0, 0x80000000, 8};  // 0x82CB61C0, the 11-dword form
    const uint32_t kPrefixGpu = 0x00400000, kSegGpu = kSeg;
    const char* why = nullptr;
    auto classify = [&](uint32_t piece, uint32_t dwords, uint32_t so_far) {
      if (plant == "old_rule") {  // the first version: any piece but the open segment is refused
        if (piece != kSegGpu) { why = "the piece is not the open segment's start"; return ob::Piece::Refused; }
      }
      return ob::ClassifyPiece(piece, kSegGpu, dwords, so_far, &why);
    };
    const ob::Piece p1 = classify(kPrefixGpu, uint32_t(prefix.size()), 0);
    const ob::Census pc = ob::Scan(prefix.data(), uint32_t(prefix.size()), kFence);
    const ob::Piece p2 = classify(kSegGpu, 15, 1);
    Check(p1 == ob::Piece::Prefix && !pc.NeedsOriginal() && p2 == ob::Piece::Segment, "prefix.then_segment",
          "submit 1 = the prefix buffer (scanned clean: " + std::to_string(pc.packets) + " packets, " + std::to_string(pc.regs.size()) + " registers kept), submit 2 = the open segment" + (why ? std::string("; refused: ") + why : ""));
    Check(classify(kPrefixGpu, 65, 0) == ob::Piece::Refused && classify(kPrefixGpu, 11, 4) == ob::Piece::Refused && classify(0x1FF80000, 11, 0) == ob::Piece::Refused && classify(kPrefixGpu, 0, 0) == ob::Piece::Refused,
          "prefix.limits", "a big, a fifth, a high-address or an empty piece is refused");
    const Words bad = Cat({prefix, MemWrite(kFence + 4, 1)});
    Check(ob::Scan(bad.data(), uint32_t(bad.size()), kFence).NeedsOriginal(), "prefix.scanned", "a prefix with a MEM_WRITE is not skipped");
    // the whole boot as the hook sees it: prefix skipped, then the segment answered: the words the game reads equal the original's (which ran the prefix + the segment)
    const Words seg = Cat({BootSegment(), EventWriteShd(kFence + 64)});
    Outcome ref = Original(Cat({prefix, seg})), ours = Ours(seg, 0, in_main ? plant : "");
    bool same;
    const std::string d = Diff(ref, ours, same);
    Check(same, "prefix.words_the_game_reads", same ? "prefix skipped + segment answered = the original's words" : "DIFFERENT: " + d);
  }
  // ---- R. run 7157: the real boot segment = register packets, REG_RMWs, a SCRATCH_REG0-7 reset and three indirect buffers --------------------------------------------
  {
    g_bufs.clear();
    Words gpr;  // 0x82CB10B8's buffer: 24 one-vertex point draws
    for (int i = 0; i < 24; ++i) gpr = Cat({gpr, Draw()});
    g_bufs[0x00500000] = gpr;
    g_bufs[0x00500400] = gpr;
    g_bufs[0x00500800] = Cat({SetConstant(), Draw()});
    const Words rmw1 = {0xC0022100, 0x81, 0xFFFFFFFF, 0x80008001}, rmw2 = {0xC0022100, 0x82, 0xFFFFFFFF, 0x1};   // 0x82CCACE8
    const Words reset = {0x00070578, 0, 0, 0, 0, 0x0BADF00D, 0, 0, 0};                                              // SCRATCH_REG0-7 (0x82CCAE9C)
    auto ib = [](uint32_t a, uint32_t n) { return Words{0xC0013F00, a, n}; };
    const Words seg = Cat({BootSegment(), rmw1, rmw2, reset, ib(0x500000, 72), ib(0x500400, 72), ib(0x500800, 6)});
    Outcome ref = Original(seg), ours = Ours(seg, blind, in_main ? plant : "");
    bool same;
    const std::string d = Diff(ref, ours, same);
    Check(!ours.original && ours.a.census.n[ob::kIndirectFollowed] == 3 && ours.a.census.scratch.size() == 5, "boot7157.answered",
          "19-packet-style segment: 3 indirect buffers followed, 5 scratch words answered (UMSK 0x20037: +0 +4 +8 +16 +20)");
    Check(same, "boot7157.words_the_game_reads", same ? "the scratch block reset (junk cleared, +16 = 0x0BADF00D) and every other word equal the original's" : "DIFFERENT: " + d);
    // the nested cases
    g_bufs[0x00600000] = Cat({ib(0x00600100, 3), Draw()});
    g_bufs[0x00600100] = Cat({ib(0x00600200, 3), Draw()});
    g_bufs[0x00600200] = Cat({ib(0x00600300, 3)});
    g_bufs[0x00600300] = Draw();
    g_bufs[0x00610000] = Cat({ib(0x00610100, 3), Draw()});
    g_bufs[0x00610100] = Cat({Draw(), Draw()});
    g_bufs[0x00610200] = Draw();
    const bool deep = Ours(Cat({BootSegment(), ib(0x00600000, 6)}), blind).original;                    // IB -> IB -> IB -> IB: depth 3
    const bool two = !Ours(Cat({BootSegment(), ib(0x00610000, 6)}), blind).original;                    // IB -> IB -> IB: depth 2 is allowed
    const bool unreadable = Ours(Cat({BootSegment(), ib(0x00700000, 6)}), blind).original;              // nothing at that address
    Check(deep && two && unreadable, "ib.limits", "a chain deeper than 2 (segment -> IB -> IB -> IB), and an unreadable buffer, go to the original; a segment -> IB -> IB chain is followed");
    g_bufs[0x00620000] = Cat({Draw(), MemWrite(kFence + 4, 0x77777777)});
    const Words seg2 = Cat({BootSegment(), ib(0x00620000, 6)});
    Outcome ref2 = Original(seg2), ours2 = Ours(seg2, blind);
    const std::string d2 = Diff(ref2, ours2, same);
    Check(ours2.original, "ib.nested_mem_write_goes_to_original", "a MEM_WRITE inside an indirect buffer is found and the submit goes to the original");
    Check(same, "ib.words_the_game_reads", same ? "equal to the original's" : "DIFFERENT: " + d2);
    // scratch write-backs that cannot be answered exactly
    Outcome elsewhere = Ours(Cat({Words{0x000001DD, kScratch + 0x1000, 0x000001DC, 0x20037}, Words{0x00000578, 1}}), blind);
    Outcome unknown = Ours(Words{0x00000578, 1}, blind);
    Outcome clear_bit = Ours(Cat({BootSegment(), Words{0x0000057B, 1}}), blind);  // SCRATCH_REG3: UMSK bit 3 clear = no write-back at all
    Check(elsewhere.original && unknown.original && !clear_bit.original && clear_bit.a.census.scratch.empty(), "scratch.unanswerable",
          "SCRATCH_ADDR elsewhere, or SCRATCH_UMSK unknown: the original; a register whose UMSK bit is clear writes nothing");
  }
  // ---- A. a later forced processor run (FellBack) needs SCRATCH_ADDR / UMSK in the SDK register file --------------------------------------------------------------
  {
    const Words seg = Cat({BootSegment(), EventWriteShd(kFence + 64)});
    Outcome ref = Original(seg), ours = Ours(seg, 0, in_main ? plant : "");
    auto forced = [&](Outcome& o) {  // the original interrupt emitter's scratch packets: SCRATCH_REG0 := cpu mask, SCRATCH_REG1 := flip word
      RefCp cp;
      cp.mem = &o.mem;
      cp.regs = o.regs;
      cp.Exec(Words{0x00010578, 4, 1});
    };
    forced(ref);
    forced(ours);
    bool same;
    const std::string d = Diff(ref, ours, same);
    Check(ours.regs[0x1DD] == kScratch && ours.regs[0x1DC] == 0x20037, "regs.scratch_addr_umsk_kept", "SCRATCH_ADDR / SCRATCH_UMSK are in the register file after the answer");
    bool sc_same = ref.mem.at(kScratch + 0) == ours.mem.at(kScratch + 0) && ref.mem.at(kScratch + 4) == ours.mem.at(kScratch + 4);
    Check(sc_same, "forced.scratch_writeback", sc_same ? "a forced run's SCRATCH_REG0/1 reach the scratch block as after the original" : "DIFFERENT: " + d);
  }
  // ---- B. every packet form that can write SCRATCH_REG0-7 (0x578-0x57F) goes to the original -------------------------------------------------------------------------
  {
    const Words t1 = {0x40000000u | (0x100u << 11) | 0x57A, 1, 2};  // type 1: SCRATCH_REG2 and register 0x100: answered (UMSK bit 2), not refused
    const std::vector<Words> hits = {{0xC0022100, 0x579, 0xFFFFFFFF, 1},    // REG_RMW (rmw_info & 0x1FFF)
                                     {0xC0025500, 0x578, 1, 2},             // SET_CONSTANT2: registers 0x578, 0x579
                                     {0xC0025600, 0x57E, 1, 2},             // SET_SHADER_CONSTANTS: 0x57E, 0x57F
                                     {0xC0054500, 7, 0, 0, 0xFFFFFFFF, 0x579, 1}};  // COND_WRITE, register form
    const std::vector<Words> fine = {{0xC0022100, 0x200, 0xFFFFFFFF, 1}, {0xC0025500, 0x4000, 1, 2}, {0xC0025600, 0x570, 1, 2},
                                     {0x40000000u | (0x101u << 11) | 0x100, 1, 2}, t1, {0xC0054500, 7, 0, 0, 0xFFFFFFFF, 0x1922, 1}};
    bool all_hit = true, all_fine = true;
    for (const Words& w : hits) all_hit = all_hit && Ours(Cat({BootSegment(), w}), blind).original;
    for (const Words& w : fine) all_fine = all_fine && !Ours(Cat({BootSegment(), w}), blind).original;
    Check(all_hit, "scratch_forms.go_to_original", "REG_RMW, SET_CONSTANT2, SET_SHADER_CONSTANTS and a register COND_WRITE that reach 0x578-0x57F are not answered by us");
    Check(all_fine, "scratch_forms.others_answered", "the same forms on other registers (0x200, 0x4000, 0x570-0x571, 0x100/0x101, 0x1922) are");
  }
  // ---- C. EVENT_WRITE_SHD is accepted only for the unread slots (fence block +64..+95) --------------------------------------------------------------------------------
  {
    const Words seg = Cat({BootSegment(), EventWriteShd(kFence + 4)});
    Outcome ref = Original(seg), ours = Ours(seg, blind);
    bool same;
    const std::string d = Diff(ref, ours, same);
    Check(ours.original, "event_write_elsewhere.goes_to_original", "an EVENT_WRITE_SHD to the heap marker is not answered by us");
    Check(same, "event_write_elsewhere.words_the_game_reads", same ? "equal to the original's" : "DIFFERENT: " + d);
    bool edges = true;
    for (uint32_t off : {60u, 96u, 0u}) edges = edges && ob::Scan(EventWriteShd(kFence + off).data(), 4, kFence).NeedsOriginal();
    for (uint32_t off : {64u, 92u}) edges = edges && !ob::Scan(EventWriteShd(kFence + off).data(), 4, kFence).NeedsOriginal();
    Check(edges, "event_write_elsewhere.edges", "+0, +60, +96 refused; +64, +92 accepted");
  }

  // ---- 3. the classes, one at a time (the table the header's contract states) ---------------------------------------------------------------------------------------
  if (plant.empty()) {
    struct Row { const char* name; Words w; uint32_t kind; bool needs; };
    const Row rows[] = {
        {"draw", Draw(), ob::kDraw, false},
        {"state(set_constant)", SetConstant(), ob::kState, false},
        {"state(type0)", Words{0x00000D04, 0}, ob::kState, false},
        {"event_write_shd", EventWriteShd(kFence + 64), ob::kEventWriteShd, false},
        {"wait_reg_mem", WaitRegMem(), ob::kWait, false},
        {"swap", XeSwap(), ob::kSwap, false},
        {"cond_write(register)", GammaCondWrite(), ob::kCondWriteReg, false},
        {"nop(type2)", Words{0x80000000}, ob::kNop, false},
        {"me_init", Words{0xC0014800, 1, 2}, ob::kMeInit, false},
        {"scratch_reg", ScratchReg1(1), ob::kScratchReg, true},
        {"event_write_elsewhere", EventWriteShd(kFence + 4), ob::kEventWriteElsewhere, true},
        {"mem_write", MemWrite(kFence + 4, 5), ob::kMemWrite, true},
        {"cond_write(memory)", CondWriteMem(kFence, 5), ob::kCondWriteMem, true},
        {"reg_to_mem", RegToMem(kFence + 4), ob::kRegToMem, true},
        {"event_write_ext", Words{0xC0015A00, 0x16, kFence}, ob::kEventWriteExt, true},
        {"event_write_zpd", Words{0xC0005B00, 0x1}, ob::kEventWriteZpd, true},
        {"interrupt", Words{0xC0005400, 4}, ob::kInterrupt, true},
        {"viz_query", Words{0xC0002300, 1}, ob::kVizQuery, true},
        {"indirect_buffer", Words{0xC0013F00, kSeg, 4}, ob::kIndirect, true},
        {"unknown_opcode", Words{0xC0000E00, 0}, ob::kUnknown, true},
    };
    bool all = true;
    std::string bad;
    for (const Row& r : rows) {
      const ob::Census c = ob::Scan(r.w.data(), uint32_t(r.w.size()), kFence);
      const bool ok = c.n[r.kind] == 1 && c.NeedsOriginal() == r.needs;
      if (!ok) { all = false; bad += std::string(r.name) + " "; }
    }
    Check(all, "classes.table", all ? "20 packet kinds classified as the header states (needs the original: scratch_reg, mem_write, cond_write(memory), reg_to_mem, event_write_ext / zpd, interrupt, viz_query, indirect_buffer, unknown)" : "WRONG: " + bad);
  }
  std::printf("%s\n", g_fail ? "FAIL" : "PASS");
  return g_fail ? 1 : 0;
}
