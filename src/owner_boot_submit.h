// wwe2k14 - Session 101, lane 231 (docs\renderer\LEAVE_THE_COMMAND_STREAM.md step 3a): in owner mode the SDK's command processor parses exactly one
// ring submit, the boot's (run 7149 log line 2301: "submits skipped 8493, run 1 (raw ring writes 1)"). This header is the whole of our answer to
// it, with no game, SDK or rex header in it, so tools\owner\boot_submit_test.cpp builds the very functions the game calls.
//
// What the boot submit is [C: listing 0x82CB7228, 0x82CB7078, 0x82CB5EF0, 0x82CB5560]: the init-idle 0x82CB7228 writes the primary ring's
// ME_INIT packet (19 dwords, through the raw ring writer 0x82CB5560 at 0x82CB7538; the ring write index device+11088 = 19) and the first
// segment's register packets (0x0D02, 0x0A02-0x0A05, SCRATCH_ADDR 0x1DD, SCRATCH_UMSK 0x1DC; 0x82CB7078 adds WAIT_UNTIL 0x5C8 and 0x0D04).
// The first ring submit after it hands that segment to the processor. Memory the processor writes for packets (SDK command_processor.cpp:
// 1119 read-pointer write-back, 1340 scratch write-back, 3311 MEM_WRITE, 3331 COND_WRITE, 3292 REG_TO_MEM, 3396-3490 EVENT_WRITE*) is what
// the game could ever see of it. The reader (docs\renderer\LEAVE_THE_COMMAND_STREAM.md "3a result") found the only word with a reader that
// the boot submit alone leaves behind: the read pointer at fence block +60, read by the ring-room check 0x82CB54B0 (only caller chain 0x82CB5560
// / 0x82CB5EF0, both skipped or never reached in owner mode). Everything else the processor would write for packets of the classes below is
// written again, every frame, by submits that owner mode already skips (8,493 of them), so the game does not need it.
//
// Answer(): scan the first segment; if every packet is of a class the game does not need, the answer is the read pointer word (:= the ring
// write index, the state "the processor has consumed what the library wrote"); any packet that can write a guest word the steady state does
// not rewrite (MEM_WRITE, COND_WRITE to memory, REG_TO_MEM, EVENT_WRITE_EXT / ZPD, INTERRUPT, VIZ_QUERY, an indirect buffer, an unknown
// opcode, a malformed packet) hands the submit to the original, as before this lane.
#pragma once

#include <cstdint>
#include <vector>

namespace wwe2k14::owner_boot {

// Guest facts. Device offsets: the listings of 0x82CB7228 (stw 60(fence block), stw 11088, lwz 15304) and 0x82CB54B0.
inline constexpr uint32_t kBootRingWriteReturn = 0x82CB753C;  // lr of the raw ring write inside 0x82CB7228 (bl at 0x82CB7538): the boot's only
inline constexpr uint32_t kDevFenceBlock = 11024;             // = d3d_noring kDevFenceBlock (owner_timing.h static_asserts it)
inline constexpr uint32_t kDevRingWrite = 11088;              // the software ring write index (dwords)
inline constexpr uint32_t kDevRingMask = 15304;               // ring size in dwords - 1
inline constexpr uint32_t kFenceRptr = 60;                    // fence block +60: the read pointer the processor writes back

// Packet classes.
enum Kind : uint32_t {
  kState,          // registers, constants, shader loads, bin state, waits for idle: no guest memory
  kDraw,           // DRAW_*, D3D_COPY: no guest memory (gpu_no_draw / null copies)
  kNop,            // type 1, type 2, NOP
  kMeInit,
  kEvent,          // EVENT_WRITE without an address
  kEventWriteShd,  // the per-present flip-tracking slots (fence block +64..+95) and the fence pair: rewritten every frame by skipped submits
  kWait,           // WAIT_REG_MEM, WAIT_REG_EQ / GTE: blocks the processor only
  kSwap,           // XE_SWAP (VdSwap): the presentation, none in owner mode
  kCondWriteReg,   // COND_WRITE to a register (the gamma ramp emitters 0x82CBABB0 / 0x82CBACA0)
  kIndirectFollowed,  // an INDIRECT_BUFFER whose target was read and scanned too (depth <= 2)
  // below: the original is needed
  kScratchReg,     // a type-0 write to SCRATCH_REG0-7 writes the scratch block (flip word, interrupt mask) through SCRATCH_ADDR / UMSK: in owner mode our
                   // thread writes those (the flip handshake is hooked), so a packet that does is unexpected and goes to the original
  kEventWriteElsewhere,  // EVENT_WRITE_SHD whose address is outside the unread flip-tracking slots (fence block +64..+95)
  kMemWrite,
  kCondWriteMem,
  kRegToMem,
  kEventWriteExt,
  kEventWriteZpd,
  kInterrupt,
  kVizQuery,
  kIndirect,
  kUnknown,
  kBad,  // overruns the segment
  kKindCount
};
inline constexpr uint32_t kFirstNeedsOriginal = kScratchReg;  // (kIndirectFollowed sits below it)

inline const char* KindName(uint32_t k) {
  static const char* const names[kKindCount] = {"state",        "draw",          "nop",          "me_init",      "event",
                                                "event_write_shd", "wait",       "swap",         "cond_write_reg", "indirect_followed", "scratch_reg", "event_write_elsewhere",
                                                "mem_write",    "cond_write_mem", "reg_to_mem", "event_write_ext", "event_write_zpd",
                                                "interrupt",    "viz_query",     "indirect",     "unknown",      "bad"};
  return k < kKindCount ? names[k] : "?";
}

struct Write {
  uint32_t addr = 0, value = 0;
};

struct Census {
  std::vector<Write> regs;  // type-0 / type-1 register writes below 0x2000, in order (addr = register index): the processor's register file after the segment
  std::vector<Write> scratch;  // scratch write-backs the segment makes: addr = byte offset in the scratch block, in stream order
  uint32_t first_bad_depth = 0;  // the indirect-buffer depth of the first offender (0 = the segment itself)
  uint32_t packets = 0;
  uint32_t n[kKindCount] = {};
  uint32_t first_bad_kind = kKindCount;  // the first class that needs the original (kKindCount: none)
  uint32_t first_bad_at = 0;             // its dword index in the segment
  uint32_t first_bad_opcode = 0;
  bool NeedsOriginal() const { return first_bad_kind != kKindCount; }
};

// Opcodes (SDK include\rex\graphics\xenos.h:1557-1619, the numbers the processor's switch uses).
namespace op {
inline constexpr uint32_t kNop = 0x10, kWaitForIdle = 0x26, kRegRmw = 0x21, kDrawIndx = 0x22, kVizQuery = 0x23, kSetState = 0x25, kImLoad = 0x27,
                          kImLoadImmediate = 0x2B, kImStore = 0x2C, kSetConstant = 0x2D, kLoadContext = 0x2E, kLoadAluConstant = 0x2F, kDrawIndx2Bin = 0x35,
                          kDrawIndx2 = 0x36, kIndirectPfd = 0x37, kDrawIndxBin = 0x34, kInvalidateState = 0x3B, kWaitRegMem = 0x3C, kMemWrite = 0x3D,
                          kRegToMem = 0x3E, kIndirect = 0x3F, kCondExec = 0x44, kCondWrite = 0x45, kEventWrite = 0x46, kMeInit = 0x48,
                          kSetShaderBases = 0x4A, kSetBinBaseOffset = 0x4B, kMemWriteCntr = 0x4F, kSetBinMask = 0x50, kSetBinSelect = 0x51,
                          kWaitRegEq = 0x52, kWaitRegGte = 0x53, kInterrupt = 0x54, kSetConstant2 = 0x55, kSetShaderConstants = 0x56,
                          kEventWriteShd = 0x58, kEventWriteCfl = 0x59, kEventWriteExt = 0x5A, kEventWriteZpd = 0x5B, kWaitUntilRead = 0x5C,
                          kWaitIbPfdComplete = 0x5D, kContextUpdate = 0x5E, kSetBinMaskLo = 0x60, kSetBinMaskHi = 0x61, kSetBinSelectLo = 0x62,
                          kSetBinSelectHi = 0x63, kXeSwap = 0x64, kD3dCopy = 0x7D;
}  // namespace op

inline constexpr uint32_t kScratchReg0 = 0x0578, kScratchReg7 = 0x057F;  // SDK register_table.inc:60-67
inline constexpr uint32_t kCoherStatusHost = 0x0A31;  // a write sets a dirty flag in the SDK (WriteRegister): not copied raw

inline bool HitsScratch(uint32_t first, uint32_t n) { return n && first <= kScratchReg7 && first + n - 1 >= kScratchReg0; }

// p: the packet's data dwords; fence_gpu: the fence block in the GPU form the packets use (physical).
inline uint32_t Type3Kind(uint32_t opcode, const uint32_t* p, uint32_t data_dwords, uint32_t fence_gpu) {
  const uint32_t first_dword = p[0];
  switch (opcode) {
    case op::kNop: return kNop;
    case op::kMeInit: return kMeInit;
    case op::kEventWrite: return data_dwords <= 1 ? kEvent : kUnknown;  // (with an address the SDK asserts: not a class we model)
    case op::kEventWriteShd:
      // words: initiator, address, value (SDK :3422). Accepted only for the flip-tracking slots (fence block +64..+95, 0x82CC42A8), which nobody reads.
      return data_dwords == 3 && (p[1] & ~3u) >= fence_gpu + 64 && (p[1] & ~3u) < fence_gpu + 96 ? kEventWriteShd : kEventWriteElsewhere;
    case op::kEventWriteCfl: return kEvent;
    case op::kEventWriteExt: return kEventWriteExt;
    case op::kEventWriteZpd: return kEventWriteZpd;
    case op::kMemWrite: return kMemWrite;
    case op::kMemWriteCntr: return kMemWrite;
    case op::kRegToMem: return kRegToMem;
    case op::kInterrupt: return kInterrupt;
    case op::kVizQuery: return kVizQuery;
    case op::kIndirect:
    case op::kIndirectPfd: return kIndirect;
    case op::kCondWrite:
      // SDK command_processor.cpp:3331-3394: word 0 (wait_info) bit 0x100 = the write goes to memory, else to a register.
      if (first_dword & 0x100) return kCondWriteMem;
      return data_dwords >= 6 && HitsScratch(p[4], 1) ? kScratchReg : kCondWriteReg;  // write_reg_addr = word 4
    case op::kWaitRegMem:
    case op::kWaitRegEq:
    case op::kWaitRegGte:
    case op::kWaitUntilRead:
    case op::kWaitIbPfdComplete: return kWait;
    case op::kXeSwap: return kSwap;
    case op::kDrawIndx:
    case op::kDrawIndx2:
    case op::kDrawIndxBin:
    case op::kDrawIndx2Bin:
    case op::kD3dCopy: return kDraw;
    case op::kSetConstant2:
    case op::kSetShaderConstants:
      // registers [p[0] & 0xFFFF, + count - 1) (SDK :4633, :4690)
      return HitsScratch(p[0] & 0xFFFF, data_dwords - 1) ? kScratchReg : kState;
    case op::kRegRmw: return (p[0] & 0x1FFF) >= kScratchReg0 && (p[0] & 0x1FFF) <= kScratchReg7 ? kScratchReg : kState;  // SDK :3259
    case op::kSetState:
    case op::kSetConstant:  // (types 0-3 are constants, type 4 is register 0x2000 + index: never a scratch register)
    case op::kLoadAluConstant:
    case op::kLoadContext:
    case op::kImLoad:
    case op::kImLoadImmediate:
    case op::kInvalidateState:
    case op::kSetShaderBases:
    case op::kSetBinBaseOffset:
    case op::kSetBinMask:
    case op::kSetBinSelect:
    case op::kSetBinMaskLo:
    case op::kSetBinMaskHi:
    case op::kSetBinSelectLo:
    case op::kSetBinSelectHi:
    case op::kContextUpdate:
    case op::kWaitForIdle: return kState;
    default: return kUnknown;
  }
}

// Indirect buffers: the packet words are (GPU address, dwords). The caller supplies the reader (the runtime reads the 0xA0000000-view alias of the physical address).
struct IbReader {
  virtual bool Read(uint32_t gpu_address, uint32_t dwords, std::vector<uint32_t>& out) = 0;
  virtual ~IbReader() = default;
};
inline constexpr uint32_t kMaxIbDepth = 2, kMaxIbDwords = 0x10000, kMaxScanDwords = 0x40000;

// Run 7157 [C]: the real boot segment is 19 packets: 15 type-0 (7228's and 7078's register packets, 0x82CCACE8's REG_RMWs and a SCRATCH_REG0-7 reset) and 3
// INDIRECT_BUFFER packets. A SCRATCH_REG write is the processor's scratch write-back (command_processor.cpp:1332-1342): when SCRATCH_UMSK bit n is set the value goes
// to SCRATCH_ADDR + 4n. The scan tracks the two registers in stream order and, when SCRATCH_ADDR is the scratch block, records the exact words (`Census::scratch`,
// offset from the block, in order) instead of refusing; a write-back anywhere else, or with the registers unknown, still goes to the original.
struct Scanner {
  Census c;
  uint32_t fence_gpu, scratch_gpu, blind;
  IbReader* ib;
  uint32_t budget = kMaxScanDwords;
  bool addr_set = false, umsk_set = false;
  uint32_t addr = 0, umsk = 0;

  Scanner(uint32_t fence, uint32_t scratch, uint32_t blind_, IbReader* r) : fence_gpu(fence), scratch_gpu(scratch), blind(blind_), ib(r) {}

  void Note(uint32_t kind, uint32_t at, uint32_t opcode, uint32_t depth) {
    ++c.packets;
    if (blind & (1u << kind)) return;
    ++c.n[kind];
    if (kind >= kFirstNeedsOriginal && c.first_bad_kind == kKindCount) {
      c.first_bad_kind = kind;
      c.first_bad_at = at;
      c.first_bad_opcode = opcode;
      c.first_bad_depth = depth;
    }
  }
  // One register write as the processor's WriteRegister does it; false = a scratch write-back this scan cannot answer exactly.
  bool WriteReg(uint32_t reg, uint32_t v) {
    if (reg == 0x1DC) { umsk = v; umsk_set = true; }
    if (reg == 0x1DD) { addr = v; addr_set = true; }
    bool ok = true;
    if (reg >= kScratchReg0 && reg <= kScratchReg7) {
      const uint32_t k = reg - kScratchReg0;
      if (umsk_set && ((umsk >> k) & 1)) {
        if (addr_set && scratch_gpu && addr == scratch_gpu) c.scratch.push_back({4 * k, v});
        else ok = false;
      } else if (!umsk_set) {
        ok = false;  // (SCRATCH_UMSK unknown: the register file holds whatever an earlier run left)
      }
    }
    if (reg < 0x2000 && reg != kCoherStatusHost) c.regs.push_back({reg, v});
    return ok;
  }
  void Run(const uint32_t* dw, uint32_t n, uint32_t depth) {
    uint32_t i = 0;
    if (n > budget) { Note(kBad, 0, 0, depth); return; }
    budget -= n;
    while (i < n) {
      const uint32_t h = dw[i];
      const uint32_t type = h >> 30;
      if (type == 0) {
        const uint32_t count = ((h >> 16) & 0x3FFF) + 1, base = h & 0x7FFF, one = (h >> 15) & 1;
        if (i + 1 + count > n) { Note(kBad, i, 0, depth); return; }
        bool ok = true;
        for (uint32_t k = 0; k < count; ++k) ok = WriteReg(one ? base : base + k, dw[i + 1 + k]) && ok;
        Note(ok ? kState : kScratchReg, i, 0, depth);
        i += 1 + count;
      } else if (type == 1) {
        if (i + 3 > n) { Note(kBad, i, 0, depth); return; }
        const bool ok1 = WriteReg(h & 0x7FF, dw[i + 1]), ok2 = WriteReg((h >> 11) & 0x7FF, dw[i + 2]);  // two registers (SDK :2264)
        Note(ok1 && ok2 ? kNop : kScratchReg, i, 0, depth);
        i += 3;
      } else if (type == 2) {
        Note(kNop, i, 0, depth);
        i += 1;
      } else {
        const uint32_t count = ((h >> 16) & 0x3FFF) + 1, opcode = (h >> 8) & 0x7F;
        if (i + 1 + count > n) { Note(kBad, i, opcode, depth); return; }
        const uint32_t kind = Type3Kind(opcode, dw + i + 1, count, fence_gpu);
        if (kind == kIndirect) {
          // SDK :2948: word 0 the buffer's GPU address, word 1 its length in dwords (& 0xFFFFF).
          const uint32_t gpu = dw[i + 1], len = count >= 2 ? dw[i + 2] & 0xFFFFF : 0;
          std::vector<uint32_t> buf;
          if (blind & (1u << kIndirectFollowed)) {  // test hook: accept the IB without looking
            Note(kIndirectFollowed, i, opcode, depth);
          } else if (!ib || depth >= kMaxIbDepth || len == 0 || len > kMaxIbDwords || !ib->Read(gpu, len, buf) || buf.size() != len) {
            Note(kIndirect, i, opcode, depth);
          } else {
            Note(kIndirectFollowed, i, opcode, depth);
            Run(buf.data(), len, depth + 1);
          }
        } else {
          Note(kind, i, opcode, depth);
        }
        i += 1 + count;
      }
    }
  }
};

// Scan `n` dwords (host order) of a segment. `blind` is a test hook: a bit mask of classes the scan pretends not to see (0 in every shipped call).
inline Census Scan(const uint32_t* dw, uint32_t n, uint32_t fence_gpu, uint32_t blind = 0, uint32_t scratch_gpu = 0, IbReader* ib = nullptr) {
  Scanner s(fence_gpu, scratch_gpu, blind, ib);
  s.Run(dw, n, 0);
  return s.c;
}

// Which piece a submit hands over. Run 7155 [C]: the first submit after the boot's raw ring write is NOT the open segment but the kick's prefix buffer
// (0x82CB6EA8 -> 0x82CB61C0 -> 0x82CB6920 -> 0x82CB5EF0: coherency flush packets in their own small allocation, 6..22 dwords), and only the next submit
// (0x82CB6B20 -> 0x82CB6920) is the segment, whose GPU address is device +15332 in GPU form.
inline constexpr uint32_t kMaxPrefixPieces = 4, kMaxPrefixDwords = 64;
enum class Piece : uint32_t { Segment, Prefix, Refused };
inline Piece ClassifyPiece(uint32_t piece_gpu, uint32_t segment_gpu, uint32_t dwords, uint32_t prefixes_so_far, const char** why) {
  if (dwords == 0 || dwords > 0x80000u) { *why = "piece size out of range"; return Piece::Refused; }
  if (piece_gpu == segment_gpu) return Piece::Segment;
  if (prefixes_so_far >= kMaxPrefixPieces) { *why = "more prefix pieces than the kick makes before its segment"; return Piece::Refused; }
  if (piece_gpu >= 0x1FF00000u || dwords > kMaxPrefixDwords) { *why = "a piece that is neither the open segment nor a small buffer in the low physical range"; return Piece::Refused; }
  return Piece::Prefix;
}

enum class Result : uint32_t { Answered, NeedsOriginal };
struct Answer {
  Result result = Result::NeedsOriginal;
  Census census;
  Write writes[2];
  uint32_t write_count = 0;
};

// Our answer to the boot submit. `seg` / `seg_dwords`: the first segment (the one piece the submit hands over); `fence_block`: *(device+11024), a guest
// address, `fence_gpu` the same block in GPU (physical) form, `scratch_gpu` the scratch block in GPU form (SCRATCH_ADDR must equal it for the scratch write-backs to be
// answered; the caller writes `census.scratch` to scratchblock + offset, in order, and `ib` reads indirect buffers); the caller also copies `census.regs` into the SDK register file (A: a later forced
// processor run needs SCRATCH_ADDR / SCRATCH_UMSK to write the scratch words back); `ring_wr`: device+11088 (the ring write index, dwords) at the time of the submit.
inline Answer AnswerBootSubmit(const uint32_t* seg, uint32_t seg_dwords, uint32_t fence_block, uint32_t fence_gpu, uint32_t scratch_gpu, uint32_t ring_wr,
                                IbReader* ib = nullptr, uint32_t blind = 0) {
  Answer a;
  a.census = Scan(seg, seg_dwords, fence_gpu, blind, scratch_gpu, ib);
  if (a.census.NeedsOriginal()) return a;
  a.result = Result::Answered;
  // The read pointer: "the processor has consumed everything written into the ring" = the write index. (The original's processor ends at the write
  // index after the submit's IB packet; the submit is skipped, so the write index stays where the raw write left it, and this keeps the ring-room
  // check 0x82CB54B0 seeing an empty ring either way.)
  a.writes[a.write_count++] = {fence_block + kFenceRptr, ring_wr};
  return a;
}

// What the ring-room check 0x82CB54B0 derives from the two words: the dwords in use (it compares the new end with the read pointer modulo the ring).
inline uint32_t RingUsed(uint32_t wr, uint32_t rptr, uint32_t mask) { return (wr - rptr) & mask; }

}  // namespace wwe2k14::owner_boot
