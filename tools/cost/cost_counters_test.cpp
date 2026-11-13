// S101 lane 83: unit test of src/cost_counters.h (the shipped header, fake clock). Usage: cost_counters_test [fault NAME]
// Exit 0 = every check passed; 1 = a check failed (the planted faults MUST give 1); 2 = bad usage.
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../../src/cost_counters.h"

using namespace wwe2k14::cost;
static int g_failed = 0;
#define CHECK(c, ...)                                          \
  do {                                                         \
    if (!(c)) {                                                \
      ++g_failed;                                              \
      std::printf("FAIL %s:%d: %s  ", __FILE__, __LINE__, #c); \
      std::printf(__VA_ARGS__);                                \
      std::printf("\n");                                       \
    }                                                          \
  } while (0)
#define U(x) static_cast<unsigned long long>(x)

static void Advance(uint64_t t) { g_fake_ticks.fetch_add(t); }

int main(int argc, char** argv) {
  if (argc >= 3 && !std::strcmp(argv[1], "fault")) {
    if (!std::strcmp(argv[2], "wrong_class")) g_fault = Fault::WrongClass;
    else if (!std::strcmp(argv[2], "wrong_replay_op")) g_fault = Fault::WrongReplayOp;
    else if (!std::strcmp(argv[2], "reader_to_record")) g_fault = Fault::ReaderToRecord;
    else if (!std::strcmp(argv[2], "skip_op")) g_fault = Fault::SkipOp;
    else return 2;
  } else if (argc != 1) {
    return 2;
  }
  g_fake_clock = true;
  g_sample_shift = 0;  // exact sums: every record timed (the sampling arm is tested below)
  ResetForTest();
  ResetPrinterForTest();

  // Off: nothing counts.
  g_on = false;
  { RecordSpan s(0x1000); Advance(500); }
  CHECK(Snapshot().record[kOther] == 0 && Snapshot().record[kArena] == 0, "spans must be free when off");
  g_on = true;

  NoteObject(0x1000, kArena), NoteObject(0x2000, kWrestlers), NoteObject(0x3000, kRing);
  CHECK(ObjectClass(0x1000) == kArena && ObjectClass(0x2000) == kWrestlers && ObjectClass(0x3000) == kRing && ObjectClass(0x9999) == kOther, "table lookup");

  {  // an arena span of 100 ticks holding a 30-tick part draw and a nested span (must not double count)
    RecordSpan s(0x1000);
    Advance(20);
    { PartSpan p(0x1000, 0x82C0A950u); Advance(30); }
    { RecordSpan inner(0x1000); Advance(10); }
    Advance(40);
  }
  { RecordSpan s(0x2000); Advance(200); }          // wrestlers span
  { PartSpan p(0x777, kFlashSite); Advance(50); }  // a flash part draw outside any span
  { CrowdSpan c; Advance(70); }
  {  // replay: op 15 holding a nested dispatch (op 21): the outermost counts once, as op 15
    ReplaySpan r(15);
    Advance(300);
    { ReplaySpan inner(21); Advance(100); }
  }
  { ReplaySpan r(21); Advance(60); }
  { ReplaySpan r(99); Advance(5); }  // unknown op -> other
  for (int i = 0; i < 2; ++i) { ReaderSpan r; r.SetClass(kArena); Advance(10); }
  { ReaderSpan r; r.SetClass(kWrestlers); Advance(7); }
  PublishReplaceMs(kArena, 4.5), PublishReplaceMs(kRing, 1.0);

  const Totals t = Snapshot();
  CHECK(t.record[kArena] == 100 && t.record_n[kArena] == 1, "arena record %llu n %llu", U(t.record[kArena]), U(t.record_n[kArena]));
  CHECK(t.part[kArena] == 30 && t.part_n[kArena] == 1, "arena part %llu", U(t.part[kArena]));
  CHECK(t.record[kWrestlers] == 200 && t.part[kWrestlers] == 0, "wrestlers record %llu", U(t.record[kWrestlers]));
  CHECK(t.record[kFlash] == 50 && t.part[kFlash] == 50, "flash record %llu part %llu", U(t.record[kFlash]), U(t.part[kFlash]));
  CHECK(t.record[kCrowd] == 70, "crowd record %llu", U(t.record[kCrowd]));
  CHECK(t.record[kRing] == 0 && t.record[kOther] == 0, "no stray record ticks");
  CHECK(t.replay == 465 && t.replay_n == 3, "replay %llu n %llu", U(t.replay), U(t.replay_n));
  CHECK(t.replay_op[OpSlot(15)] == 400 && t.replay_op[OpSlot(21)] == 60 && t.replay_op[OpSlot(99)] == 5, "replay by op: 15=%llu 21=%llu other=%llu",
        U(t.replay_op[OpSlot(15)]), U(t.replay_op[OpSlot(21)]), U(t.replay_op[OpSlot(99)]));
  CHECK(t.reader[kArena] == 20 && t.reader_n[kArena] == 2 && t.reader[kWrestlers] == 7 && t.reader[kOther] == 0, "reader arena %llu wrestlers %llu",
        U(t.reader[kArena]), U(t.reader[kWrestlers]));
  CHECK(t.record[kArena] + t.record[kWrestlers] + t.record[kFlash] + t.record[kCrowd] == 420, "record sum (reader ticks must not leak into record)");
  CHECK(t.replace_ms[kArena] == 4.5 && t.replace_ms[kRing] == 1.0, "replace ms");

  // The formatted line (ticks_per_ns 1 => 1 tick = 1 ns; 10 frames; 1e6 ticks = 1 ms so scale the totals up).
  Totals big = t;
  for (int i = 0; i < kClasses; ++i) big.record[i] *= 10000, big.part[i] *= 10000, big.reader[i] *= 10000;
  big.replay *= 10000;
  for (int i = 0; i < kOps; ++i) big.replay_op[i] *= 10000;
  for (int i = 0; i < kAllOps; ++i) big.op_est[i] *= 10000;
  const std::string line = FormatLine("", 10, big, 1.0);
  std::printf("%s\n", line.c_str());
  CHECK(line.find("[cost] frames 10, record ms/f {arena 0.100 wrestlers 0.200 ring 0.000 flash 0.050 crowd 0.070 other 0.000}") == 0, "record block");
  CHECK(line.find("part ms/f {arena 0.030 wrestlers 0.000 ring 0.000 flash 0.050") != std::string::npos, "part block");
  CHECK(line.find("replay ms/f 0.465 (0.3 rec/f, excl reader 0.438; op15 0.400/0.1 op19 0.000/0.0 op21 0.060/0.1") != std::string::npos, "replay block");
  CHECK(line.find("reader us/draw {arena 100.00 wrestlers 70.00 ring 0.00") != std::string::npos, "reader block");
  CHECK(line.find("replace ms/f {arena 0.450 wrestlers 0.000 ring 0.100") != std::string::npos, "replace block");

  CHECK(t.op_n[15] == 1 && t.op_est[15] == 400 && t.op_n[21] == 1 && t.op_est[21] == 60 && t.op_n[99] == 1 && t.op_est[99] == 5, "per-op table of the first block");
  CHECK(line.find(", ovh ns/span 0.0 (subtracted; replay timed 1/1), replay top ops ms/f/count/f {op15 0.400/0.1 op21 0.060/0.1 op99 0.005/0.1}, replay groups ms/f/count/f {setters 0.000/0.0") != std::string::npos,
        "ops/groups block: %s", line.c_str());

  // Per-op table, all 128 opcodes: op k gets (k+1) records of (k+1)*3 ticks. A sampler that skips one opcode (or books it to the next) must fail here.
  ResetForTest();
  for (int op = 0; op < kAllOps; ++op)
    for (int r = 0; r <= op; ++r) { ReplaySpan sp{uint32_t(op)}; Advance(uint64_t(op + 1) * 3); }
  { ReplaySpan sp(500); Advance(2); }  // beyond the table -> the last slot
  const Totals pa = Snapshot();
  for (int op = 0; op < kAllOps; ++op) {
    const uint64_t n = uint64_t(op + 1) + (op == kAllOps - 1 ? 1 : 0);
    const uint64_t ticks = uint64_t(op + 1) * uint64_t(op + 1) * 3 + (op == kAllOps - 1 ? 2 : 0);
    CHECK(pa.op_n[op] == n && pa.op_est[op] == ticks, "op %d: n %llu (want %llu) ticks %llu (want %llu)", op, U(pa.op_n[op]), U(n), U(pa.op_est[op]), U(ticks));
  }
  {
    uint64_t grp_sum = 0;
    for (int op = 0; op < kAllOps; ++op) grp_sum += pa.op_est[op];
    CHECK(pa.replay == grp_sum, "replay total is the sum of the per-op table");
    const std::string pl = FormatLine("", 1, pa, 1.0);
    const size_t at = pl.find("replay top ops ms/f/count/f {");
    const size_t end = at == std::string::npos ? at : pl.find('}', at);
    CHECK(at != std::string::npos && end != std::string::npos, "top ops block present");
    if (at != std::string::npos && end != std::string::npos) {
      const std::string blk = pl.substr(at + 28, end - at - 28);
      int count = 0;
      for (size_t q = blk.find(" op"); q != std::string::npos; q = blk.find(" op", q + 1)) ++count;
      CHECK(blk.find("{op127 ") != std::string::npos && count == 11, "top 12 ops by ms, biggest first: %s", blk.c_str());
      CHECK(blk.find(" op116 ") != std::string::npos && blk.find(" op115 ") == std::string::npos, "the 12th is op116, not op115: %s", blk.c_str());
    }
    CHECK(pl.find("replay groups ms/f/count/f {setters ") != std::string::npos && pl.find(" rest ") != std::string::npos, "groups block present");
    // the S50 groups add up to the table
    uint64_t gt = 0, gn = 0;
    for (int op = 0; op < kAllOps; ++op) gt += pa.op_est[op], gn += pa.op_n[op];
    CHECK(GroupOf(3) == kGSetters && GroupOf(5) == kGSetters && GroupOf(80) == kGSetters && GroupOf(14) == kGStream && GroupOf(41) == kGBools && GroupOf(83) == kGBanked &&
          GroupOf(103) == kGBanked && GroupOf(25) == kGComps && GroupOf(40) == kGComps && GroupOf(9) == kGEffect && GroupOf(44) == kGObjCall && GroupOf(72) == kGPasses &&
          GroupOf(82) == kGRet && GroupOf(55) == kGResource && GroupOf(15) == kGListed && GroupOf(1) == kGRest && GroupOf(104) == kGRest,
          "group map");
    CHECK(gn == pa.replay_n && gt == pa.replay, "groups cover the table");
  }

  // Sampling (1 in 16, rotating random): exact counts, unbiased ticks, even for a stream with a period-16 cost pattern (a fixed 1-in-16 would alias).
  ResetForTest();
  g_sample_shift = 4;
  constexpr int kRecs = 640000;
  uint64_t true_ticks = 0;
  for (int i = 0; i < kRecs; ++i) {
    const uint64_t dt = (i % 16 == 0) ? 1000 : 10;
    ReplaySpan sp(22);
    Advance(dt);
    true_ticks += dt;
  }
  const Totals sa = Snapshot();
  const double err = double(sa.op_est[22]) / double(true_ticks) - 1.0;
  CHECK(sa.op_n[22] == uint64_t(kRecs), "sampled: the count stays exact (%llu)", U(sa.op_n[22]));
  CHECK(err > -0.05 && err < 0.05, "sampled estimate off by %.3f", err);
  g_sample_shift = 0;
  // Overhead: subtracted per counted span (reader 2 spans x 10 ticks - 2 x 3 = 14), clamped at 0.
  ResetForTest();
  g_overhead_ticks = 3.0;
  for (int i = 0; i < 2; ++i) { ReaderSpan r; r.SetClass(kArena); Advance(10); }
  { ReaderSpan r; r.SetClass(kRing); Advance(1); }
  { ReplaySpan sp(21); Advance(13); }
  const Totals oa = Snapshot();
  CHECK(oa.reader[kArena] == 14 && oa.reader[kRing] == 0 && oa.op_est[21] == 10, "overhead subtraction: arena %llu ring %llu op21 %llu", U(oa.reader[kArena]), U(oa.reader[kRing]),
        U(oa.op_est[21]));
  ResetForTest();
  NoteObject(0x1000, kArena);

  // The window: first call starts, 600 frames later one line of the delta, then a final line of the whole run.
  g_on = false;
  CHECK(Update(true, 100, 1.0).empty(), "first frame starts the window");
  CHECK(Update(true, 699, 1.0).empty(), "window not full");
  const std::string w = Update(true, 700, 1.0);
  CHECK(w.find("[cost] frames 600, ") == 0, "window line: %s", w.c_str());
  { RecordSpan s(0x1000); Advance(6000000); }
  CHECK(Update(true, 1299, 1.0).empty(), "next window not full");
  const std::string w2 = Update(true, 1300, 1.0);
  CHECK(w2.find("[cost] frames 600, record ms/f {arena 0.010 ") == 0, "second window line is a delta: %s", w2.c_str());
  const std::string fin = FinalLine(1.0);
  CHECK(fin.find("[cost] final: frames 1200, ") == 0, "final line: %s", fin.c_str());
  CHECK(Update(false, 1400, 1.0).empty() && !On(), "off switches the counters off");

  // Two threads recording at once: every count lands.
  g_on = true;
  ResetForTest();
  NoteObject(0x1000, kArena);
  std::vector<std::thread> th;
  for (int k = 0; k < 2; ++k)
    th.emplace_back([] {
      for (int i = 0; i < 20000; ++i) {
        PartSpan p(0x1000, 0);
        ReaderSpan r;
        r.SetClass(kArena);
      }
    });
  for (auto& x : th) x.join();
  CHECK(Snapshot().part_n[kArena] == 40000 && Snapshot().record_n[kArena] == 40000 && Snapshot().reader_n[kArena] == 40000, "two threads: %llu",
        U(Snapshot().part_n[kArena]));

  std::printf(g_failed ? "cost_counters_test: %d FAILED\n" : "cost_counters_test: ALL PASSED\n", g_failed);
  return g_failed ? 1 : 0;
}
