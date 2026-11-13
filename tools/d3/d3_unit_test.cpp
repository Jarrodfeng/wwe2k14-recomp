// Lane 138 (D3 skip the object): the guest-side decision core, the shipped headers src/renderer_adapter/{d3_arena,d3_guest_core}.h, with no game, no SDK and no GPU.
//   d3_unit_test.exe               honest run: every check must PASS (exit 0)
//   d3_unit_test.exe plant NAME    one planted fault, which must make a named check FAIL (exit 1): hole_no_packet, skip_in_proof_frame, late_packet, gate_ignored, ordinal_off
// The scenario: a "game thread" calls N objects a frame for FRAMES frames through DecideCall (the hook's decision), a "binder thread" 1-3 frames behind publishes the mark set (every object
// complete) and consumes the markers' packets. Bars: markers == packets taken, holes 0, nothing skipped in an object's proof frame, nothing skipped before the gate proved, every Why reachable.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../src/renderer_adapter/d3_guest_core.h"
#include "../../src/renderer_adapter/d3_record.h"
#include "../../src/scene_live/ring_rows_core.h"   // lane 246: the ring's class table (RingTable / ReadRingTable) the ring hook claims calls by
#include "../../src/scene_live/light_state_hook.h"   // lane 152 (review S5 / S6): the tracker's read-only probe on a fake guest   // lane 152: RecordRegister (pure; no SceneTables is built here)

using namespace rdr_adapter::d3;

static int g_fail = 0;
static void Check(bool ok, const char* name, const char* detail) {
  std::printf("%s  %-30s %s\n", ok ? "PASS" : "FAIL", name, detail);
  g_fail += !ok;
}

struct Marker {
  uint64_t frame;
  uint32_t object, ordinal;
};

int main(int argc, char** argv) {
  std::string plant = argc >= 3 && std::strcmp(argv[1], "plant") == 0 ? argv[2] : "";
  Fault fault = Fault::None;
  const bool caster_plant = plant.rfind("caster_", 0) == 0;   // lane 228: the caster block's own plants (caster_hole_no_packet, caster_skip_in_proof_frame, caster_stale_record, caster_path2_skipped, caster_withdraw_off)
  if (caster_plant) plant.clear();   // the arena scenario then runs honest: only the caster block below is planted
  const std::string cplant = caster_plant ? std::string(argv[2]).substr(7) : std::string();
  const bool ring_plant = plant.rfind("ring_", 0) == 0;   // lane 246: the ring block's own plants (ring_path2_skipped, ring_hole_no_packet, ring_outer_ignored, ring_wrong_chain, ring_rows_unbacked, ring_skip_in_proof_frame, ring_marker_unclaimed, ring_path_unset)
  if (ring_plant) plant.clear();     // the arena scenario and the caster block then run honest: only the ring block below is planted
  const std::string rplant = ring_plant ? std::string(argv[2]).substr(5) : std::string();
  const bool wrestler_plant = plant.rfind("wrestler_", 0) == 0;   // lane 245: the wrestler block's own plants (wrestler_skip_in_proof_frame, _hole_no_packet, _stale_record, _path_default_fresh1, _path2_skipped, _outer_ignored, _camera_ignored)
  if (wrestler_plant) plant.clear();   // the arena scenario then runs honest: only the wrestler block below is planted
  const std::string wplant = wrestler_plant ? std::string(argv[2]).substr(9) : std::string();
  if (!plant.empty() && !ParseFault(plant, fault) && plant != "late_packet" && plant != "gate_ignored" && plant != "ordinal_off" && plant != "path_ignored" && plant != "path2_skip" && plant != "no_end_on_skip" && plant != "no_latch" && plant != "probe_old_frame") {
    std::printf("FAIL  unknown plant %s\n", plant.c_str());
    return 2;
  }
  g_arena_d3 = true;
  const bool known_plant = plant == "late_packet" || plant == "gate_ignored" || plant == "ordinal_off" || plant == "path_ignored" || plant == "path2_skip" || plant == "no_end_on_skip" || plant == "no_latch" || plant == "probe_old_frame";

  // ---- the Why chain, one input at a time ----------------------------------------------------------------------------------------------------------------------------------------
  {
    SkipInputs in;
    in.switch_on = in.owner = in.gate_proven = in.markable = in.packet_ok = true;
    bool chain = Decide(in) == Why::Skip;
    SkipInputs a = in; a.switch_on = false; chain = chain && Decide(a) == Why::SwitchOff;
    a = in; a.owner = false; chain = chain && Decide(a) == Why::NotOwner;
    a = in; a.gate_proven = false; chain = chain && Decide(a) == Why::GateNotProven;
    a = in; a.proof_frame = true; chain = chain && Decide(a) == Why::ProofFrame;
    a = in; a.markable = false; chain = chain && Decide(a) == Why::NotMarkable;
    a = in; a.packet_ok = false; a.packet_why = Why::TableFull; chain = chain && Decide(a) == Why::TableFull;
    a = in; a.packet_ok = false; a.packet_why = Why::StateOverflow; chain = chain && Decide(a) == Why::StateOverflow;
    a = in; a.packet_ok = false; a.packet_why = Why::Unreadable; chain = chain && Decide(a) == Why::Unreadable;
    Check(chain, "decide.chain", "switch, owner, gate, proof frame, markable, packet (table full / overflow / unreadable) each decide in the contract's order");
    a = in; a.proof_frame = true;
    Check(Decide(a, Fault::SkipInProofFrame) == Why::Skip, "plant.skip_in_proof_frame", "the plant skips in a proof frame (the main scenario's bar must see it)");
  }

  // ---- lane 301: calls = runs: the tally's cached-object list (path-4 objects left out of the binder's count), the touched frame, the widened ring, the overflow ------------------------------
  {
    GuestTally t;
    t.NoteKeyedCall(500), t.NoteKeyedCall(500);
    t.NoteCachedObject(500, 0x82000001), t.NoteCachedObject(500, 0x82000001), t.NoteCachedObject(500, 7);   // a duplicate is one object
    std::vector<uint32_t> o;
    bool of = true;
    bool ok = t.CachedObjects(500, o, &of) && !of && o.size() == 2 && !t.CachedObjects(501, o, &of) && !of;
    t.TouchFrame(501);
    uint32_t n = 9;
    ok = ok && t.KeyedCalls(501, n) && n == 0;   // a frame the hook saw with no keyed call is compared (0 runs expected)
    t.TouchFrame(500);
    ok = ok && t.KeyedCalls(500, n) && n == 2;   // a touch never resets a counted frame
    for (uint32_t i = 0; i < GuestTally::kCachedMax + 5; ++i) t.NoteCachedObject(600, 0x1000 + i);
    ok = ok && !t.CachedObjects(600, o, &of) && of && t.cached_overflow.load() == 1;   // the frame past the list's bound is not compared, counted once
    t.NoteKeyedCall(500 + GuestTally::kKeyedRing);   // the ring wraps: an old frame's slot is reused, never read back as the old frame
    ok = ok && !t.KeyedCalls(500, n) && t.KeyedCalls(500 + GuestTally::kKeyedRing, n) && n == 1;
    Check(ok, "calls.cached_left_out", "the tally lists a frame's path-4 objects (distinct, bounded, overflow flagged once), a touched frame is compared with 0, the ring is 256 frames");
  }

  // ---- lane 204 (Check K1): the marker claim, fed through the replay handler's arithmetic -----------------------------------------------------------------------------------------------
  // The hook records op 15 with r4 = 6 and r7 = 0; sub_82BB41E8 replays it as draw(prim, base, start, tbl[prim].mul * rec+28 + tbl[prim].add) from the game's table at 0x82001B78 (A_replay_ops.md op 15).
  // Entry 6 = (1, 2) is the table's own data (hard-coded here as the test's input, NOT read from the header's constants); a real strip of 6 indices replays as 6.
  {
    struct Ent { uint32_t mul, add; };
    const Ent tbl6 = {1, 2};
    auto replay = [&](uint32_t rec28) { return tbl6.mul * rec28 + tbl6.add; };
    const uint32_t delivered = replay(0);   // what the replay hands the draw call for the hook's record
    const bool claims = IsMarkerReplay(6, delivered, false, fault);
    Check(delivered == 2 && claims, "marker.claim_replayed", "the hook's marker (prim 6, recorded count 0) replays with 2 indices and IS claimed (the V4 test `count == 0` never claimed it: Check K1)");
    Check(!IsMarkerReplay(6, replay(4), false) && !IsMarkerReplay(4, delivered, false) && !IsMarkerReplay(6, delivered, true) && !IsMarkerReplay(6, 0, false),
          "marker.stale_tag_passes_on", "a real strip / another primitive / inline vertices / a zero-count call carrying a stale tag is NOT claimed");
    Check(kMarkerReplayIndexCount == delivered, "marker.constant_matches_table", "the header's constant is the table arithmetic of the recorded 0");
  }

  // ---- the gate -----------------------------------------------------------------------------------------------------------------------------------------------------------------------
  {
    Gate g;
    bool ok = !g.Proven(0);
    g.Report(true, 0, 100), g.Report(true, 0, 116);
    ok = ok && !g.Proven(120);
    g.Report(true, 0, 132);
    ok = ok && g.Proven(140) && !g.Proven(100000);
    g.Report(true, 1, 148);
    ok = ok && !g.Proven(150) && g.resets.load() == 1;
    Check(ok, "gate.shape", "3 clean frames prove it, stale after 250 ms, a frame with a hole / mismatch resets it (counted)");
  }

  // ---- the packet table ---------------------------------------------------------------------------------------------------------------------------------------------------------
  {
    PacketTable t(2);
    Packet a{.frame = 5, .object = 1, .ordinal = 0}, b{.frame = 5, .object = 2, .ordinal = 0}, c{.frame = 5, .object = 3, .ordinal = 0};
    bool ok = t.Put(a) && t.Put(b) && !t.Put(c) && t.stats().full == 1;
    ok = ok && t.Take(5, 1, 0) && !t.Take(5, 1, 0) && t.Take(6, 2, 0) == nullptr && t.stats().missing == 2;
    t.Evict(100);
    ok = ok && t.size() == 0 && t.stats().evicted_unconsumed == 1;
    Check(ok, "table.shape", "a full table refuses (counted), a packet is taken once, a wrong frame finds none (counted), old packets are evicted (counted)");
  }

  // ---- the mark set -------------------------------------------------------------------------------------------------------------------------------------------------------------
  {
    MarkPublisher p;
    const uint32_t g1 = p.Publish({MarkKey(1, 0), MarkKey(2, 0)}), g2 = p.Publish({MarkKey(2, 0), MarkKey(1, 0)}), g3 = p.Publish({MarkKey(1, 0)});
    auto s = p.Acquire();
    Check(g1 == 1 && g2 == 1 && g3 == 2 && s && s->Has(MarkKey(1, 0)) && !s->Has(MarkKey(2, 0)) && p.generation() == 2, "marks.shape", "the same set keeps its generation, a change takes the next, Has() is exact");
  }

  // ---- the two-thread scenario --------------------------------------------------------------------------------------------------------------------------------------------------
  constexpr uint32_t kObjects = 40, kFrames = 3000;
  GuestObjects objs;
  PacketTable packets(plant == "late_packet" ? 30 : 4096);   // (the plant needs a table that fills)
  MarkPublisher marks;
  GuestTally tally;
  Gate gate;
  std::mutex mu;
  std::deque<std::vector<Marker>> pending;   // frames of markers the "render side" has not consumed yet
  std::atomic<uint64_t> game_frame{0}, bound_frame{0};
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> skipped_on_cache{0};
  std::atomic<uint64_t> holes{0}, taken{0}, markers_made{0}, skipped_in_proof{0}, skipped_before_gate{0}, dropped_table{0};
  uint64_t proof_calls = 0;

  {
    std::vector<uint64_t> keys;   // every object is complete before the first frame: only the gate (and the objects' own first frame) hold the early skips
    for (uint32_t o = 1; o <= kObjects; ++o) keys.push_back(MarkKey(o, 0));
    marks.Publish(keys);
  }
  std::thread binder([&] {
    // 1-3 frames behind: consumes the markers of the oldest pending frame, reports the gate, publishes the mark set (every object complete from the first frame: the gate, not the mark set, must hold the early frames)
    uint64_t done = 0;
    while (done < kFrames) {
      const uint64_t gf = game_frame.load();
      if (done + 1 + (done % 3) > gf && !stop.load()) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        continue;
      }
      std::vector<Marker> ms;
      {
        std::lock_guard<std::mutex> lock(mu);
        if (!pending.empty()) {
          ms = std::move(pending.front());
          pending.pop_front();
        }
      }
      uint64_t bad = 0;
      for (const Marker& m : ms) {
        std::shared_ptr<const Packet> p = packets.Take(m.frame, m.object, m.ordinal);
        if (!p) ++holes, ++bad;
        else ++taken;
      }
      gate.Report(true, bad, int64_t(done) * 16);
      ++done;
      bound_frame.store(done);
    }
  });

  uint64_t skips = 0, calls = 0;
  Gate always;
  for (uint32_t f = 0; f < kFrames; ++f) {
    game_frame.store(f);
    std::vector<Marker> ms;
    const std::shared_ptr<const MarkSet> set = marks.Acquire();
    for (uint32_t o = 1; o <= kObjects; ++o) {
      CallInputs in;
      in.object = o, in.frame = f, in.now_ms = int64_t(f) * 16, in.owner = true, in.state_readable = true, in.state_count = 1;
      const CachePath true_path = (o % 5 == 0 && f % 7 == 0) ? CachePath::Record3 : (o % 11 == 0 && f % 13 == 0) ? CachePath::Cached4 : (o % 3 == 0 ? CachePath::Fresh2 : CachePath::Fresh1);
      in.path = plant == "path_ignored" || (plant == "path2_skip" && true_path == CachePath::Fresh2) ? CachePath::Fresh1 : true_path;   // plant: the hook does not look at the model's cache path
      // one call a frame, 3..9 keyed parts
      if (plant == "gate_ignored")   // plant: the gate is "always proven"
        always.Report(true, 0, int64_t(f) * 16), always.Report(true, 0, int64_t(f) * 16), always.Report(true, 0, int64_t(f) * 16);
      const Gate& use_gate = plant == "gate_ignored" ? always : gate;
      CallOutcome out = DecideCall(objs, in, set.get(), packets, tally, use_gate, fault);
      ++calls;
      if (plant == "late_packet" && out.why == Why::TableFull) out.why = Why::Skip;   // plant: the decision is made before the packet (a skip with no packet)
      if (plant == "ordinal_off" && out.why == Why::Skip) out.ordinal += 1;           // plant: the marker names another ordinal than the packet's
      objs.End(o, f, out.call, CallInfo{uint16_t(3 + o % 7), 0});
      if (ObjectProofFrame(o, f)) ++proof_calls;
      if (out.why == Why::Skip) {
        ++skips;
        ++markers_made;
        if (ObjectProofFrame(o, f)) ++skipped_in_proof;
        if (true_path != CachePath::Fresh1) ++skipped_on_cache;   // lane 237: path 2 (the model's cache is being recorded) is not skippable either
        if (!gate.Proven(int64_t(f) * 16)) ++skipped_before_gate;
        ms.push_back({f, o, out.ordinal});
      } else if (out.why == Why::TableFull) {
        ++dropped_table;
      }
    }
    {
      std::lock_guard<std::mutex> lock(mu);
      pending.push_back(std::move(ms));
    }
    while (f > bound_frame.load() + 6) std::this_thread::sleep_for(std::chrono::microseconds(50));   // the render side is never more than a few frames behind in this test
  }
  stop.store(true);
  binder.join();

  char buf[300];
  std::snprintf(buf, sizeof buf, "%llu calls, %llu markers (%.1f %%), %llu packets taken", (unsigned long long)calls, (unsigned long long)skips, 100.0 * double(skips) / double(calls), (unsigned long long)taken.load());
  Check(skips > calls / 2, "scenario.skips", buf);
  std::snprintf(buf, sizeof buf, "markers %llu, packets taken %llu, holes %llu", (unsigned long long)markers_made.load(), (unsigned long long)taken.load(), (unsigned long long)holes.load());
  Check(holes.load() == 0 && taken.load() == markers_made.load(), "scenario.no_hole", buf);
  std::snprintf(buf, sizeof buf, "%llu skips in an object's proof frame of %llu proof calls", (unsigned long long)skipped_in_proof.load(), (unsigned long long)proof_calls);
  Check(skipped_in_proof.load() == 0 && proof_calls > 0, "scenario.proof_frames_full", buf);
  std::snprintf(buf, sizeof buf, "%llu skips while the gate was not proven", (unsigned long long)skipped_before_gate.load());
  Check(skipped_before_gate.load() == 0, "scenario.gate_respected", buf);
  std::snprintf(buf, sizeof buf, "%llu skips of a call whose model records or replays its own cached command list", (unsigned long long)skipped_on_cache.load());
  Check(skipped_on_cache.load() == 0, "scenario.cache_paths", buf);
  const FrameCounts fc = tally.TakeAndReset();
  std::snprintf(buf, sizeof buf, "markers %u dropped %u (proof frame %u, gate %u, no sequence yet %u)", fc.markers, fc.dropped, fc.why[uint8_t(Why::ProofFrame)], fc.why[uint8_t(Why::GateNotProven)], fc.why[uint8_t(Why::NotMarkable)]);
  Check(fc.markers == skips && fc.why[uint8_t(Why::ProofFrame)] > 0, "scenario.header_counts", buf);

  // ---- PredictPath on fake guest memory: the four paths of sub_82BD7B50 and unreadable --------------------------------------------------------------------------------------------------------
  {
    std::unordered_map<uint32_t, uint32_t> mem;
    const auto rd = [&](uint32_t a, uint32_t& v) {
      auto it = mem.find(a);
      if (it == mem.end()) return false;
      v = it->second;
      return true;
    };
    const uint32_t M = 0x1000, R13 = 0x2000, TLS = 0x3000, BLK = 0x3100, LIST = 0x4000, FLAGS = 0x5000, H848 = 0x6000, H852 = 0x6100;
    mem[M + 888] = 1, mem[M + 600] = 1, mem[R13] = TLS, mem[TLS + 1048] = BLK, mem[BLK + 32] = 1, mem[M + 896] = LIST, mem[LIST + 4] = 0, mem[M + 900] = FLAGS, mem[FLAGS + 4] = 0, mem[M + 848] = H848, mem[M + 852] = H852;
    mem[H848] = 0, mem[H852] = 7;
    bool ok = PredictPath(rd, M, R13) == CachePath::Record3;                       // handle 848 empty: the game begins recording
    mem[H848] = 9;
    ok = ok && PredictPath(rd, M, R13) == CachePath::Cached4;                      // handle 848 holds a list: op 81
    mem[FLAGS + 4] = 1;
    ok = ok && PredictPath(rd, M, R13) == CachePath::Cached4;                      // flag set: handle 852 (holds 7)
    mem[H852] = 0;
    ok = ok && PredictPath(rd, M, R13) == CachePath::Record3;
    mem[LIST + 4] = 5;
    ok = ok && PredictPath(rd, M, R13) == CachePath::Fresh2;                       // a list already in the slot
    mem[M + 600] = 0;
    ok = ok && PredictPath(rd, M, R13) == CachePath::Fresh1;                       // bit 0 of [model+600] clear: no cache
    mem[M + 600] = 1, mem[M + 888] = 0;
    ok = ok && PredictPath(rd, M, R13) == CachePath::Fresh1;
    mem[M + 888] = 1, mem.erase(M + 896);
    ok = ok && PredictPath(rd, M, R13) == CachePath::Unreadable;
    Check(ok, "path.predict", "the four paths of the model's cached command-list manager are read from the same words the function reads; an unreadable word is never a skip");
  }

  // ---- lane 237 (SKIP_GAME_WORK_MAP.md 11.9): path 2 = the model's cache is being RECORDED: no skip, no packet, no marker (the verify arm writes a marker only for Why::Skip) ----------------------
  {
    std::unordered_map<uint32_t, uint32_t> mem;
    const auto rd = [&](uint32_t a, uint32_t& v) {
      auto it = mem.find(a);
      if (it == mem.end()) return false;
      v = it->second;
      return true;
    };
    const uint32_t M = 0x1000, R13 = 0x2000, TLS = 0x3000, BLK = 0x3100, LIST = 0x4000;
    mem[M + 888] = 1, mem[M + 600] = 1, mem[R13] = TLS, mem[TLS + 1048] = BLK, mem[BLK + 32] = 1, mem[M + 896] = LIST, mem[LIST + 4] = 1;   // the slot's entry is set: recording
    GuestObjects o3;
    PacketTable p3(64);
    GuestTally t3;
    Gate g3;
    MarkPublisher m3;
    m3.Publish({MarkKey(M, 0)});
    const auto set = m3.Acquire();
    uint32_t skips2 = 0, markers2 = 0;
    for (uint32_t f = 0; f < 40; ++f) {
      g3.Report(true, 0, int64_t(f) * 16), g3.Report(true, 0, int64_t(f) * 16), g3.Report(true, 0, int64_t(f) * 16);
      CallInputs in;
      in.object = M, in.frame = f, in.now_ms = int64_t(f) * 16, in.owner = true, in.state_readable = true, in.state_count = 1;
      in.path = PredictPath(rd, M, R13);
      if (plant == "path2_skip" && in.path == CachePath::Fresh2) in.path = CachePath::Fresh1;   // plant: path 2 treated as skippable (the pre-lane-237 rule)
      GuestCall gc = BeginGuestCall(o3, in, set.get(), p3, t3, g3);
      if (gc.out.why == Why::Skip) ++skips2, ++markers2;
      EndGuestCall(o3, gc, CallInfo{3, 0}, Fault::None, &t3);
    }
    char b2[200];
    std::snprintf(b2, sizeof b2, "%u skips / markers of a model whose cache is being recorded; packets put %llu; census path2 %llu of %llu calls", skips2, (unsigned long long)p3.stats().put, (unsigned long long)t3.cum_path[1].load(), (unsigned long long)t3.cum_calls.load());
    Check(skips2 == 0 && markers2 == 0 && p3.stats().put == 0 && t3.cum_path[1].load() > 0 && t3.cum_markers.load() == 0, "path2.no_skip", b2);
  }

  // ---- two calls of one object a frame, the real begin / end sequence (review fix 2) ------------------------------------------------------------------------------------------------------
  {
    GuestObjects objs2;
    PacketTable packets2(64);
    GuestTally tally2;
    Gate always2;
    MarkPublisher marks2;
    marks2.Publish({MarkKey(900, 0), MarkKey(900, 1)});
    const auto set = marks2.Acquire();
    uint32_t steady_skips = 0, steady_calls = 0, bad_ordinal = 0;
    for (uint32_t f = 0; f < 90; ++f) {
      always2.Report(true, 0, int64_t(f) * 16), always2.Report(true, 0, int64_t(f) * 16), always2.Report(true, 0, int64_t(f) * 16);
      for (uint32_t call = 0; call < 2; ++call) {
        CallInputs in;
        in.object = 900, in.frame = f, in.now_ms = int64_t(f) * 16, in.owner = true;
        GuestCall gc = BeginGuestCall(objs2, in, set.get(), packets2, tally2, always2);
        const bool skip = gc.out.why == Why::Skip;
        if (skip && gc.out.ordinal != call) ++bad_ordinal;
        if (skip) (void)packets2.Take(f, 900, gc.out.ordinal);
        if (!(skip && plant == "no_end_on_skip")) EndGuestCall(objs2, gc, CallInfo{4, 0});   // plant: a skipped call is never ended
        if (f >= 8 && !ObjectProofFrame(900, f)) {
          ++steady_calls;
          steady_skips += skip;
        }
      }
    }
    char m[200];
    std::snprintf(m, sizeof m, "%u of %u steady-state calls skipped, %u with the wrong ordinal", steady_skips, steady_calls, bad_ordinal);
    Check(steady_calls > 0 && steady_skips == steady_calls && bad_ordinal == 0, "twocall.steady", m);
  }

  // ---- calls: a second call of an object, a non-keyed call, a fifth call --------------------------------------------------------------------------------------------------------------
  {
    GuestObjects o2;
    auto v = o2.Begin(77, 10);
    o2.End(77, 10, v.call, CallInfo{4, 0});
    auto v2 = o2.Begin(77, 10);
    o2.End(77, 10, v2.call, CallInfo{2, 1});   // the second call also drew a non-keyed part
    auto n1 = o2.Begin(77, 11);                // next frame: call 0 was keyed, call 1 was not
    auto n2 = o2.Begin(77, 11);
    bool ok = !v.eligible && n1.eligible && n1.ordinal == 0 && !n2.eligible;
    for (int i = 0; i < 6; ++i) (void)o2.Begin(77, 11);
    auto n5 = o2.Begin(77, 11);
    ok = ok && !n5.eligible;
    Check(ok, "calls.keyed_ordinal", "a call is eligible only if the same call index drew only keyed parts last time; a mixed call and a fifth call never are");
  }

  // ---- lane 152 (D3-F): (16) the light state is read BEFORE the decision --------------------------------------------------------------------------------------------------------------
  {
    g_arena_d3 = true;
    GuestObjects o3;
    GuestTally t3;
    PacketTable p3(64);
    Gate gate3;
    for (int i = 0; i < 4; ++i) gate3.Report(true, 0, 100 + 16 * i);
    MarkSet marks;
    marks.generation = 1;
    marks.keys = {MarkKey(9, 0)};
    // frame 1: the object is recorded in full once (so the call is eligible), frame 2: the decision with an unreadable state / a full table / a readable state
    {
      CallInputs in;
      in.object = 9, in.frame = 1, in.now_ms = 160;
      GuestCall c = BeginGuestCall(o3, in, &marks, p3, t3, gate3);
      EndGuestCall(o3, c, CallInfo{2, 0});
    }
    struct Case { bool readable; uint32_t count; Why want; };
    bool ok = true;
    uint32_t frame = 2;
    for (const Case& k : {Case{false, 0, Why::Unreadable}, Case{true, kMaxStates + 1, Why::StateOverflow}, Case{true, 3, Why::Skip}}) {
      struct Ctx { bool readable; uint32_t count; } ctx{k.readable, k.count};
      StateProbe probe;
      probe.ctx = &ctx;
      probe.fn = [](void* c, uint32_t, bool& readable, uint32_t& count, uint32_t&) { readable = static_cast<Ctx*>(c)->readable, count = static_cast<Ctx*>(c)->count; return readable; };
      CallInputs in;
      in.object = 9, in.frame = frame, in.now_ms = 160 + 16 * frame, in.probe = &probe;
      in.proof_skew = 0;
      // find a frame where the object is not in a proof frame (the decision chain would answer ProofFrame otherwise)
      while (ObjectProofFrame(9, in.frame)) in.frame = ++frame;
      GuestCall c = BeginGuestCall(o3, in, &marks, p3, t3, gate3, fault);
      const Why got = c.out.why;
      EndGuestCall(o3, c, CallInfo{2, 0});
      ++frame;
      if (got != k.want) ok = false;
      if (got != Why::Skip) (void)p3.Take(in.frame, 9, 0);
    }
    Check(ok, "probe.state_before_decision", "an unreadable state is Why::Unreadable and a full state table Why::StateOverflow (no packet, no skip); a readable state skips (16)");
    // (12) the proof skew: an object is not skipped in the frame the reader's schedule calls its proof frame (hook frame + skew)
    uint32_t pf = 0;
    while (!ObjectProofFrame(9, 1000 + pf)) ++pf;
    CallInputs in2;
    in2.object = 9, in2.frame = 1000 + pf - 1, in2.now_ms = 5000, in2.proof_skew = 1;
    GuestObjects o4;
    PacketTable p4(64);
    {
      CallInputs prime = in2;
      prime.frame = 900;
      GuestCall c0 = BeginGuestCall(o4, prime, &marks, p4, t3, gate3);
      EndGuestCall(o4, c0, CallInfo{2, 0});
    }
    Gate g4;
    for (int i = 0; i < 4; ++i) g4.Report(true, 0, 4900 + 16 * i);
    GuestCall c1 = BeginGuestCall(o4, in2, &marks, p4, t3, g4);
    Check(c1.out.why == Why::ProofFrame, "skew.proof_frame_window", "with the hook-to-stream offset +1 the call one frame before the reader's proof frame is not skipped (12)");
    // (17) the keyed-call ring
    GuestTally t5;
    t5.NoteKeyedCall(7), t5.NoteKeyedCall(7), t5.NoteKeyedCall(8);
    uint32_t n7 = 0, n8 = 0, n9 = 0;
    Check(t5.KeyedCalls(7, n7) && n7 == 2 && t5.KeyedCalls(8, n8) && n8 == 1 && !t5.KeyedCalls(9, n9), "calls_runs.ring", "the hook's keyed calls are counted per hook frame; a frame with none reads as absent (17)");
  }

  // ---- lane 152 review fixes: B2 latch, S3 packets, S5 / S6 the tracker probe -----------------------------------------------------------------------------------------------------------------
  {
    Gate g;
    g.latch_enabled = plant != "no_latch";
    int64_t t = 1000;
    for (int round = 0; round < 4; ++round) {   // a systematic hole: 3 clean frames, a bad one, again
      for (int i = 0; i < 3; ++i) g.Report(true, 0, t += 16);
      g.Report(true, 1, t += 16);
    }
    for (int i = 0; i < 4; ++i) g.Report(true, 0, t += 16);
    Check(g.latched.load() && !g.Proven(t), "gate.latch", "3 resets inside 10 s latch D3 off for the session: the gate is never proven again (B2)");
    Gate slow;
    int64_t ts = 1000;
    for (int round = 0; round < 4; ++round) {
      for (int i = 0; i < 3; ++i) slow.Report(true, 0, ts += 16);
      slow.Report(true, 1, ts += 16);
      ts += 20000;   // resets far apart do not latch
    }
    Check(!slow.latched.load(), "gate.no_false_latch", "resets spread over minutes do not latch");
    Gate mis;   // a marked object's proof mismatch (a real change: a light cue, a camera cut) resets the gate but never latches the session
    int64_t tm = 1000;
    for (int round = 0; round < 6; ++round) {
      for (int i = 0; i < 3; ++i) mis.Report(true, 0, tm += 16, 0);
      mis.Report(true, 2, tm += 16, 0);
    }
    Check(!mis.latched.load() && mis.resets.load() >= 5, "gate.mismatch_no_latch", "only hole-type causes count toward the latch: proof mismatches reset the gate, never latch it");
  }
  {
    GuestObjects o;
    GuestTally t;
    PacketTable p(64);
    Gate g;
    for (int i = 0; i < 4; ++i) g.Report(true, 0, 100 + 16 * i);
    MarkSet marks;
    marks.generation = 1;
    marks.keys = {MarkKey(5, 0)};
    {
      CallInputs in;
      in.object = 5, in.frame = 1, in.now_ms = 160;
      GuestCall c = BeginGuestCall(o, in, &marks, p, t, g);
      EndGuestCall(o, c, CallInfo{2, 0});
    }
    uint64_t frame = 2;
    while (ObjectProofFrame(5, frame)) ++frame;
    CallInputs in;
    in.object = 5, in.frame = frame, in.now_ms = 160 + 16 * frame, in.path = CachePath::Cached4;
    GuestCall c = BeginGuestCall(o, in, &marks, p, t, g, fault);
    Check(!c.out.packet_written && p.size() == 0, "packet.only_when_skippable", "no packet is written for a call the game's own cache keeps (cache path 3 / 4): it would never be consumed (S3)");
  }
  {
    using namespace rdr_scene::live::lights;
    Tracker tr;
    if (plant == "probe_old_frame") tr.SetPlant(Plant::ProbeOldFrame);
    auto rw = [](uint32_t a, uint32_t& v) {
      if (a == kGlobalWord) v = 0x1000;
      else if (a == 0x1004) v = 0x2000;
      else if (a == 0x3000) v = 0x3100;   // [r13]
      else if (a == 0x3100 + 1048) v = 0x4000;
      else if (a == 0x4000 + 32) v = 0;
      else v = 0;
      if (a == 0x1000 + 208 || a == 0x1000 + 212) v = 0x44000000u;
      return true;
    };
    auto rb = [](uint32_t, void* dst, size_t n) { std::memset(dst, 0, n); return true; };
    auto lg = [](const char*) {};
    tr.SetLogBudget(0);
    tr.OnModelCall(7, 0x3000, 10, rw, rb, lg);   // a NESTED call the D3 hook does not count: ordinal 0 is taken
    uint32_t states = 0, ord = 0;
    Tracker::Probe pr = tr.ProbeModelCall(7, 0x3000, 10, rw, rb, states, ord);
    bool ok = pr == Tracker::Probe::Ok && ord == 1 && states == 1;
    uint32_t s2 = 0, o2 = 0;
    const Tracker::Probe old = tr.ProbeModelCall(7, 0x3000, 9, rw, rb, s2, o2);   // a call of an already closed frame is dropped by OnModelCall
    Check(ok, "probe.ordinal_peek", "the probe returns the ordinal OnModelCall will assign (a nested call already took 0), read only (S6)");
    Check(old == Tracker::Probe::Unreadable, "probe.old_frame", "a call of a closed frame is Unreadable: nothing would be written (S5)");
    // the packet's call index = the probe's ordinal: through DecideCall
    GuestObjects o;
    GuestTally t;
    PacketTable p(64);
    Gate g;
    for (int i = 0; i < 4; ++i) g.Report(true, 0, 100 + 16 * i);
    MarkSet marks;
    marks.generation = 1;
    marks.keys = {MarkKey(7, 0)};
    {
      CallInputs in0;
      in0.object = 7, in0.frame = 1, in0.now_ms = 160;
      GuestCall c0 = BeginGuestCall(o, in0, &marks, p, t, g);
      EndGuestCall(o, c0, CallInfo{2, 0});
    }
    struct PC { Tracker* tr; decltype(rw) rw; decltype(rb) rb; } pc{&tr, rw, rb};
    StateProbe sp;
    sp.ctx = &pc;
    sp.fn = [](void* c, uint32_t object, bool& readable, uint32_t& count, uint32_t& ordinal) {
      PC& x = *static_cast<PC*>(c);
      auto r = x.tr->ProbeModelCall(object, 0x3000, 10, x.rw, x.rb, count, ordinal);
      readable = r != Tracker::Probe::Unreadable;
      if (r == Tracker::Probe::StateOverflow) count = rdr_adapter::d3::kMaxStates + 1;
      return readable;
    };
    uint64_t frame = 2;
    while (ObjectProofFrame(7, frame)) ++frame;
    CallInputs in;
    in.object = 7, in.frame = frame, in.now_ms = 160 + 16 * frame, in.probe = &sp;
    GuestCall c = BeginGuestCall(o, in, &marks, p, t, g, fault);
    auto pk = p.Take(frame, 7, 0);
    Check(c.out.why == Why::Skip && pk && pk->call == 1, "packet.call_is_ref_ordinal", "the packet carries the light-state ref's ordinal (1), not the hook's own call count (0) (S6)");
  }

  // ---- lane 152: the record route's register mapping (pure; the plants are source faults) ---------------------------------------------------------------------------------------------------
  {
    RecordSrc rs;
    rs.have_light = rs.have_world = true;
    for (int k = 0; k < 5; ++k)
      for (int c = 0; c < 4; ++c) rs.rows.dif_lgh_vec[k][c] = float(1 + k + c), rs.rows.dif_lgh_col[k][c] = float(10 + k + c);
    rs.rows.tex_proj_valid = true;
    for (int i = 0; i < 12; ++i) rs.world[i] = float(100 + i);
    uint32_t v[4];
    float f4[4];
    bool ok = RecordRegister(0, 8, Group::Light, false, rs, Fault::None, v) == NoSrc::None;
    std::memcpy(f4, v, 16);
    ok = ok && f4[0] == 1.f && f4[3] == 4.f;
    ok = ok && RecordRegister(1, 2, Group::Light, false, rs, Fault::None, v) == NoSrc::None;
    ok = ok && RecordRegister(0, 77, Group::World, false, rs, Fault::None, v) == NoSrc::None;
    std::memcpy(f4, v, 16);
    ok = ok && f4[0] == 104.f;
    ok = ok && RecordRegister(0, 186, Group::Clock, false, rs, Fault::None, v) == NoSrc::ClockNoSource;     // c186 Time has no source (J1)
    ok = ok && RecordRegister(0, 79, Group::World, false, rs, Fault::None, v) == NoSrc::WorldRows;
    ok = ok && RecordRegister(0, 72, Group::Drift, false, rs, Fault::None, v) == NoSrc::Drift;
    RecordSrc none;
    ok = ok && RecordRegister(0, 8, Group::Light, false, none, Fault::None, v) == NoSrc::NoLightRecord && RecordRegister(0, 76, Group::World, false, none, Fault::None, v) == NoSrc::NoWorldRecord;
    RecordRegister(0, 8, Group::Light, true, rs, Fault::NegateDirs, v);
    std::memcpy(f4, v, 16);
    ok = ok && f4[0] == -1.f && f4[3] == 4.f;
    RecordRegister(0, 8, Group::Light, false, rs, Fault::MirrorDirs, v);
    std::memcpy(f4, v, 16);
    ok = ok && f4[0] == 1.f;   // not mirrored: untouched
    RecordSrc ck = rs;
    ck.have_clock = true, ck.clock = 5.f;
    ok = ok && RecordRegister(0, 186, Group::Clock, false, ck, Fault::ClockOrigin, v) == NoSrc::None;   // the plant gives c186 a value the honest route refuses
    Check(ok, "record.register_map", "light / shadow / world registers come from the records; c186 Time and drift have NO source; the source plants change the values");
  }

  // ---- lane 228 (the caster hooks' decision, SKIP_GAME_WORK_MAP.md 17.12): PredictPathCaster on fake guest memory and the whole call sequence of the three passes -------------------------------------------------
  // The fake guest holds the words the three cached-list twins read [C listings 0x82BD8698-0x82BD8758 (map), 0x82BD8964-0x82BD89FC (plane), 0x82BD8A6C-0x82BD8B08 (rim)]. The bars: a skip needs path 1, a tracker record under the
  // SAME ordinal, no proof frame, a proven gate; a marker's packet is always there; the planted twins each fail their named check.
  {
    g_caster_d3 = true;
    std::unordered_map<uint32_t, uint32_t> mem;
    const auto rd = [&](uint32_t a, uint32_t& v) {
      auto it = mem.find(a);
      if (it == mem.end()) return false;
      v = it->second;
      return true;
    };
    const uint32_t R13 = 0x2000, TLS = 0x3000, BLK = 0x3100, COLL = kCasterCollapseTable + 4;   // slot 1
    mem[R13] = TLS, mem[TLS + 1048] = BLK, mem[BLK + 32] = 1;
    const auto model_at = [](uint32_t i) { return 0x10000u + i * 0x1000u; };
    const uint32_t bits[4] = {0, 2, 4, 8}, hi[4] = {0, 860, 868, 876};
    // one model of each kind, per pass: 0..3 path 1 (bit clear), 4 path 2 (a list in the slot), 5 path 3 (empty handle), 6 map-only collapse bypass, 7 and 8 fresh but with a bad tracker record, 9 path 4
    const auto setup = [&](uint32_t i, uint32_t pass) {
      const uint32_t M = model_at(i), LIST = M + 0x400, FLAGS = M + 0x500, H = M + 0x600;
      mem[M + 888] = 1, mem[M + 600] = (i <= 3 || i >= 7) && i != 9 ? 0u : bits[pass];
      mem[M + 896] = LIST, mem[LIST + 4] = i == 4 ? 5u : 0u, mem[M + 900] = FLAGS, mem[FLAGS + 4] = 0, mem[M + hi[pass] - 4] = H, mem[M + hi[pass]] = H, mem[H] = i == 9 ? 77u : 0u;
      mem[COLL] = (i == 6 && pass == 1) ? 1u : 0u;
      if (i == 9) mem[M + 600] = bits[pass];
    };
    bool paths_ok = true;
    for (uint32_t pass = 1; pass <= 3; ++pass) {
      const CachePath want[10] = {CachePath::Fresh1, CachePath::Fresh1, CachePath::Fresh1, CachePath::Fresh1, CachePath::Fresh2, CachePath::Record3, pass == 1 ? CachePath::Fresh1 : CachePath::Record3, CachePath::Fresh1, CachePath::Fresh1, CachePath::Cached4};
      for (uint32_t i = 0; i < 10; ++i) {
        setup(i, pass);
        const uint32_t M = model_at(i);
        if (i == 6) mem[M + 600] = bits[pass];   // bit set, empty list: only the collapse flag (map) makes it Fresh1
        paths_ok = paths_ok && PredictPathCaster(rd, pass, M, R13) == want[i];
      }
    }
    setup(0, 1);
    mem[model_at(0) + 600] = 2;   // the MAP's bit is 2: set -> the cache is in use (the path then follows the list), clear -> fresh
    paths_ok = paths_ok && PredictPathCaster(rd, 1, model_at(0), R13) != CachePath::Fresh1 && PredictPathCaster(rd, 2, model_at(0), R13) == CachePath::Fresh1 && PredictPathCaster(rd, 3, model_at(0), R13) == CachePath::Fresh1;   // the plane's bit is 4, the rim's 8
    mem.erase(model_at(0) + 896);
    paths_ok = paths_ok && PredictPathCaster(rd, 1, model_at(0), R13) == CachePath::Unreadable && PredictPathCaster(rd, 0, model_at(0), R13) == CachePath::Unreadable;
    Check(paths_ok, "caster.paths", "the three twins' paths are read from the words each reads (bits 2 / 4 / 8, handles 860 / 868 / 876, the map's collapse bypass); a list in the slot is Fresh2; an unreadable word is never a skip");

    GuestObjects objs;
    PacketTable packets(512);
    GuestTally tally;
    Gate gate;
    MarkPublisher marks;
    std::vector<uint64_t> keys;
    for (uint32_t i = 0; i < 10; ++i)
      for (uint32_t pass = 1; pass <= 3; ++pass) keys.push_back(MarkKey(CasterObjectId(model_at(i), pass), 0));
    marks.Publish(keys);
    const auto set = marks.Acquire();
    const Fault cf = cplant == "hole_no_packet" ? Fault::HoleNoPacket : cplant == "skip_in_proof_frame" ? Fault::SkipInProofFrame : cplant == "stale_record" ? Fault::SkipUnreadable : Fault::None;
    uint64_t outer_skips = 0, plane_skips = 0, skips = 0, holes = 0, skipped_in_proof = 0, unbacked = 0, path2_skips = 0, gate_early = 0, markers_total = 0, withdrawn = 0;
    for (uint32_t f = 0; f < 200; ++f) {
      const int64_t now = int64_t(f) * 16;
      if (f >= 5) gate.Report(true, 0, now), gate.Report(true, 0, now), gate.Report(true, 0, now);   // the gate proves from frame 5: no skip before
      std::vector<std::pair<uint32_t, uint32_t>> markers;   // (key object, ordinal)
      for (uint32_t i = 0; i < 10; ++i)
        for (uint32_t pass = 1; pass <= 3; ++pass) {
          setup(i, pass);
          const uint32_t M = model_at(i);
          if (i == 6) mem[M + 600] = bits[pass];
          CallInputs in;
          in.object = CasterObjectId(M, pass), in.frame = f, in.now_ms = now, in.owner = true, in.caster = true, in.proof_skew = kHookToStreamOffset;
          in.path = PredictPathCaster(rd, pass, M, R13);
          if (cplant == "path2_skipped" && i == 4) in.path = CachePath::Fresh1;   // the plant: a predictor that takes a list in the slot for a fresh draw
          const bool recorded = i != 7;                        // object 7: the tracker wrote no record
          const int64_t tracker_ordinal = !recorded ? -1 : (i == 8 ? 1 : 0);   // object 8: the tracker's record sits under another ordinal
          in.state_readable = recorded || cf == Fault::SkipUnreadable, in.state_count = 0;
          GuestCall gc = BeginGuestCall(objs, in, set.get(), packets, tally, gate, cf);
          const OuterCache oc = (i == 1 && f % 3 == 0) ? OuterCache::Open : (i == 2 && f % 5 == 0) ? OuterCache::Unreadable : OuterCache::Closed;   // review S2: an outer cache recording open (object 1) / unreadable (object 2) on some frames
          WithdrawIfOuterRecording(gc.out, in, oc, packets, tally, cplant == "outer_ignored");
          if (gc.out.why == Why::Skip && oc != OuterCache::Closed) ++outer_skips;
          WithdrawIfPassUnskippable(gc.out, in, pass, packets, tally, cplant == "plane_skipped");   // the production guard (non-verify arm); the plant lets a marked plane call through
          if (gc.out.why == Why::Skip && pass == 2) ++plane_skips;
          if (cplant != "withdraw_off") WithdrawIfRecordUnbacked(gc.out, in, tracker_ordinal, packets, tally, cf);
          else if (gc.out.why == Why::Skip && tracker_ordinal != int64_t(gc.out.ordinal)) ++unbacked;   // the plant skips the withdraw
          const bool skip = gc.out.why == Why::Skip;
          if (skip) {
            ++skips;
            markers.push_back({in.object, gc.out.ordinal});
            if (ObjectProofFrame(in.object, f) || ObjectProofFrame(in.object, f + kHookToStreamOffset)) ++skipped_in_proof;
            if (!recorded || tracker_ordinal != int64_t(gc.out.ordinal)) ++unbacked;
            if (i == 4) ++path2_skips;
            if (f < 5) ++gate_early;
          } else if (gc.out.why == Why::Unreadable && recorded == false) {
            ++withdrawn;
          }
          CallInfo info;
          info.keyed = 2;
          EndGuestCall(objs, gc, info, Fault::None, &tally);
        }
      for (const auto& m : markers) {   // the binder meets each marker and takes its packet by the composite key
        ++markers_total;
        if (!packets.Take(f, m.first, m.second)) ++holes;
      }
    }
    char buf[300];
    std::snprintf(buf, sizeof buf, "%llu markers, %llu packet(s) missing", (unsigned long long)markers_total, (unsigned long long)holes);
    Check(skips > 0 && holes == 0, "caster.no_hole", buf);
    std::snprintf(buf, sizeof buf, "%llu skip(s) in an object's proof frame (either clock)", (unsigned long long)skipped_in_proof);
    Check(skipped_in_proof == 0, "caster.proof_frames_full", buf);
    std::snprintf(buf, sizeof buf, "%llu skip(s) whose tracker record is missing or under another ordinal", (unsigned long long)unbacked);
    Check(unbacked == 0, "caster.record_backed", buf);
    std::snprintf(buf, sizeof buf, "%llu skip(s) of a pass with a list in the slot", (unsigned long long)path2_skips);
    Check(path2_skips == 0, "caster.path2_never_skipped", buf);
    std::snprintf(buf, sizeof buf, "%llu skip(s) of a plane pass call", (unsigned long long)plane_skips);
    Check(plane_skips == 0, "caster.plane_never_skipped", buf);
    std::snprintf(buf, sizeof buf, "%llu skip(s) / marker(s) while an outer cache recording was open or unreadable", (unsigned long long)outer_skips);
    Check(outer_skips == 0, "caster.outer_never_marked", buf);
    {   // CasterOuterRecording on fake guest words: recorder null = closed; entry = [rec+104] + 68 * [[rec+156] + 4*slot]; flag at +36
      std::unordered_map<uint32_t, uint32_t> m2;
      const auto rd2 = [&](uint32_t a, uint32_t& v) {
        auto it = m2.find(a);
        if (it == m2.end()) return false;
        v = it->second;
        return true;
      };
      const uint32_t REC = 0x7000, BASE = 0x7100, TAB = 0x7200;
      m2[R13] = TLS, m2[TLS + 1048] = BLK, m2[BLK + 32] = 1;
      m2[kCacheRecorderWord] = 0;
      bool ok2 = CasterOuterRecording(rd2, R13) == OuterCache::Closed;                 // no recorder at all
      m2[kCacheRecorderWord] = REC, m2[REC + 104] = BASE, m2[REC + 156] = TAB, m2[TAB + 4] = 3, m2[BASE + 68 * 3 + 36] = 0;
      ok2 = ok2 && CasterOuterRecording(rd2, R13) == OuterCache::Closed;               // slot 1 -> entry index 3, flag 0
      m2[BASE + 68 * 3 + 36] = 1;
      ok2 = ok2 && CasterOuterRecording(rd2, R13) == OuterCache::Open;                 // begun, not yet sealed
      m2[BASE + 68 * 3 + 36] = 0, m2[BASE + 68 * 2 + 36] = 1;
      ok2 = ok2 && CasterOuterRecording(rd2, R13) == OuterCache::Closed;               // another entry's flag is not this slot's
      m2.erase(TAB + 4);
      ok2 = ok2 && CasterOuterRecording(rd2, R13) == OuterCache::Unreadable;           // an unreadable word is refused, never read as closed
      Check(ok2, "caster.outer_recording", "the begin/seal flag of THIS slot's cache entry is read from the words sub_82BB4AE0 / sub_82BB4B98 write; no recorder = closed; unreadable = refused");
    }
    std::snprintf(buf, sizeof buf, "%llu skip(s) before the gate was proven", (unsigned long long)gate_early);
    Check(gate_early == 0, "caster.gate_respected", buf);
    const FrameCounts cfc = tally.TakeAndReset();
    std::snprintf(buf, sizeof buf, "markers %u == skips %llu (a withdrawn skip is not a marker)", cfc.markers, (unsigned long long)skips);
    Check(cfc.markers == skips, "caster.tally_markers", buf);
    (void)withdrawn;
    // the withdraw itself: the packet leaves the table, the counters move from skip to unreadable
    {
      PacketTable pt(8);
      GuestTally tl;
      CallOutcome out;
      out.why = Why::Skip, out.ordinal = 0, out.packet_written = true;
      CallInputs in;
      in.object = 77, in.frame = 5;
      Packet p;
      p.frame = 5, p.object = 77, p.ordinal = 0;
      pt.Put(std::move(p));
      tl.Note(Why::Skip);
      const bool w = WithdrawIfRecordUnbacked(out, in, 3, pt, tl);
      const bool ok = w && out.why == Why::Unreadable && !out.packet_written && pt.size() == 0 && tl.markers.load() == 0 && tl.why[uint8_t(Why::Unreadable)].load() == 1 && tl.dropped.load() == 1;
      CallOutcome same;
      same.why = Why::Skip, same.ordinal = 3;
      Check(ok && !WithdrawIfRecordUnbacked(same, in, 3, pt, tl), "caster.withdraw", "a skip whose tracker record is under another ordinal is withdrawn (packet taken back, counted unreadable); a backed one is left alone");
    }
    // lane 290: the caster ledger holds 16 calls an object; the PLANE pass tracks them all (the ring's plane proxy makes 12 a frame), map / rim keep 4. Ordinals 4-11 of the plane are eligible; a map object's fifth call never is.
    {
      GuestObjects cobjs{kMaxCasterPlaneCalls};
      const uint32_t plane_obj = CasterObjectId(0xAAF56230u, 2), map_obj = CasterObjectId(0xAAF56230u, 1);
      const uint32_t plane_cap = cplant == "plane_cap4" ? kMaxCalls : CasterCallCap(2);   // the plant: the old kMaxCalls = 4 for the plane
      uint32_t plane_eligible = 0, map_eligible = 0, ord_ok = 1, hi_ordinals = 0;
      for (uint64_t f = 0; f < 6; ++f) {
        for (uint32_t c = 0; c < 12; ++c) {
          const GuestObjects::Verdict v = cobjs.Begin(plane_obj, f, plane_cap);
          if (f >= 1 && v.eligible) {
            ++plane_eligible;
            ord_ok &= v.call == c && v.ordinal == c;
            hi_ordinals += c >= 4;
          }
          cobjs.End(plane_obj, f, v.call, CallInfo{2, 0});
          const GuestObjects::Verdict m = cobjs.Begin(map_obj, f, CasterCallCap(1));
          if (f >= 1 && m.eligible) ++map_eligible;
          cobjs.End(map_obj, f, m.call, CallInfo{2, 0});
        }
      }
      snprintf(buf, sizeof buf, "plane eligible calls %u of 60 (ordinals 4-11: %u of 40), map eligible %u of 20 (cap 4), ordinals = call index %u", plane_eligible, hi_ordinals, map_eligible, ord_ok);
      Check(plane_eligible == 60 && hi_ordinals == 40 && ord_ok && map_eligible == 20, "caster.plane_calls_4_to_11", buf);
    }
    // lane 299 (SKIP_GAME_WORK_MAP.md 17.15): the plane SKIP arm. The production decision (BeginGuestCall -> WithdrawIfPassUnskippable(allow) -> WithdrawIfPlaneUnreplayable) on the ring proxy (12 calls a frame, 2 parts each) and
    // the wrestler proxy (1 call, 8 parts). A skipped call replays its part draws (ReplayPlaneParts): the PlaneDraw sequence the plane tracker sees must equal the stream's in every frame. Refused (recorded in full) in a proof
    // frame, with no learned part list, and for an object the binder unmarked (a proof mismatch). At frame 120 the part lists CHANGE (ring call 5: {0,1} -> {0}; wrestler proxy: 8 -> 6 parts): the replay may differ from the stream
    // only until that object's next proof frame (either clock), where every call of it is recorded in full and the ledger re-learns.
    {
      GuestObjects cobjs{kMaxCasterPlaneCalls};
      PacketTable pk(512);
      GuestTally tl;
      Gate gt;
      MarkPublisher mp;
      PlanePartLedger plp;
      const uint32_t ring_model = 0xAAF56230u, wr_model = 0xAD424110u, ring_obj = CasterObjectId(ring_model, 2), wr_obj = CasterObjectId(wr_model, 2);
      std::vector<uint64_t> ring_keys, all_keys;
      for (uint32_t c = 0; c < 12; ++c) ring_keys.push_back(MarkKey(ring_obj, c));
      all_keys = ring_keys;
      all_keys.push_back(MarkKey(wr_obj, 0));
      mp.Publish(all_keys);
      const auto set_all = mp.Acquire();
      mp.Publish(ring_keys);
      const auto set_ring = mp.Acquire();   // the binder unmarks the wrestler proxy at frame 100 (a proof mismatch)
      const Fault pf = cplant == "plane_skip_in_proof_frame" ? Fault::SkipInProofFrame : Fault::None;
      const uint32_t plane_cap = cplant == "plane_cap4" ? kMaxCalls : CasterCallCap(2);   // the plant: lane 290's fault, the ledger follows 4 calls of a plane object
      constexpr uint64_t kChange = 120;
      const auto proof_of = [&](uint32_t obj, uint64_t f) { return ObjectProofFrame(obj, f) || ObjectProofFrame(obj, f + kHookToStreamOffset); };
      uint64_t ring_pf = kChange, wr_pf = kChange;   // the first proof frame of each object at or after the change
      while (!proof_of(ring_obj, ring_pf)) ++ring_pf;
      while (!proof_of(wr_obj, wr_pf)) ++wr_pf;
      uint64_t potential = 0, skipped = 0, skipped_hi = 0, skip_in_proof = 0, skip_unproven = 0, skip_unmarked = 0, seq_differs = 0, parts_stream = 0, parts_replayed = 0, parts_recorded = 0, holes2 = 0, frames_skipping = 0;
      uint64_t refused = 0, would_skip_lost = 0, refused_other = 0, change_diff_before = 0, change_diff_after = 0;
      for (uint64_t f = 0; f < 200; ++f) {
        const int64_t now = int64_t(f) * 16;
        if (f >= 5) gt.Report(true, 0, now), gt.Report(true, 0, now), gt.Report(true, 0, now);
        if (f == 50) plp.Clear();   // the part lists are lost: the next skip decisions have no list behind them
        const auto cur = f < 100 || cplant == "plane_mismatch_skip" ? set_all : set_ring;   // the plant keeps the old set: an object the binder unmarked is still skipped
        std::vector<std::pair<uint32_t, uint32_t>> truth, seen, truth_r, seen_r, truth_w, seen_w;
        std::vector<std::pair<uint32_t, uint32_t>> markers;
        uint64_t skipped_now = 0;
        for (uint32_t i = 0; i < 13; ++i) {
          const bool wr = i == 12;
          const uint32_t model = wr ? wr_model : ring_model, call = wr ? 0 : i;
          const uint32_t nparts = f < kChange ? (wr ? 8u : 2u) : (wr ? 6u : (call == 5 ? 1u : 2u));
          uint16_t parts[8];
          for (uint32_t p = 0; p < nparts; ++p) {
            parts[p] = uint16_t(p), truth.push_back({model, p});
            (wr ? truth_w : truth_r).push_back({model, p});
          }
          CallInputs in;
          in.object = CasterObjectId(model, 2), in.frame = f, in.now_ms = now, in.owner = true, in.caster = true, in.state_readable = true, in.state_count = 0, in.proof_skew = kHookToStreamOffset, in.path = CachePath::Fresh1;
          in.max_calls = plane_cap;
          const bool proof = proof_of(in.object, f);
          GuestCall gc = BeginGuestCall(cobjs, in, cur.get(), pk, tl, gt, pf);
          WithdrawIfPassUnskippable(gc.out, in, 2, pk, tl, true);   // the live plane flag: allow
          if (f >= 6 && f != 50 && !proof && (!wr || f < 100)) ++potential;   // every such call must be skipped: marked, gate proven, previous frame learned, lists present, not a proof frame
          PlanePartList pl;
          const bool have_list = plp.Lookup(model, gc.out.call, pl);   // ONE lookup: the list the skip is decided on is the list it replays
          const bool was_skip = gc.out.why == Why::Skip;
          if (WithdrawIfPlaneUnreplayable(gc.out, in, 2, have_list, pk, tl, cplant == "plane_unproven")) {
            ++refused;   // counted from the return value only (an unmarked / ineligible call is Decide's own NotMarkable, not a refusal)
            if (f != 50) ++refused_other;
          }
          if (f == 50 && was_skip && !have_list) ++would_skip_lost;
          if (gc.out.why == Why::Skip) {
            ++skipped, ++skipped_now;
            if (!wr && call >= 4) ++skipped_hi;
            if (proof) ++skip_in_proof;
            if (!have_list) ++skip_unproven;
            if (wr && f >= 100) ++skip_unmarked;   // the binder unmarked the wrestler proxy at frame 100
            markers.push_back({in.object, gc.out.ordinal});
            if (cplant != "plane_no_replay")
              parts_replayed += ReplayPlaneParts(pl, [&](uint32_t part) {
                seen.push_back({model, part});
                (wr ? seen_w : seen_r).push_back({model, part});
                return true;
              });
            EndGuestCall(cobjs, gc, {}, Fault::None, &tl);
          } else {
            for (uint32_t p = 0; p < nparts; ++p) {
              seen.push_back({model, p});
              (wr ? seen_w : seen_r).push_back({model, p});
            }
            parts_recorded += nparts;
            plp.Store(model, gc.out.call, parts, nparts, false, cplant == "plane_ledger_sticky");
            CallInfo info;
            info.keyed = uint16_t(nparts);
            EndGuestCall(cobjs, gc, info, Fault::None, &tl);
          }
        }
        parts_stream += truth.size();
        if (f < kChange && seen != truth) ++seq_differs;   // before the change: every frame equal
        if (f >= kChange) {   // after it: per object, the replay may differ only before that object's next proof frame
          if (seen_r != truth_r) (f < ring_pf ? change_diff_before : change_diff_after) += 1;
          if (seen_w != truth_w) (f < wr_pf ? change_diff_before : change_diff_after) += 1;
        }
        if (skipped_now) ++frames_skipping;
        for (const auto& m : markers)
          if (!pk.Take(f, m.first, m.second)) ++holes2;
      }
      snprintf(buf, sizeof buf, "skipped %llu of %llu skippable plane calls (frames 6-199 except 50, marked, not in a proof frame; ring calls 4-11: %llu), stream part draws up to frame 119 equal in every frame (differing frames %llu), markers without a packet %llu, replayed %llu + recorded %llu entries",
               (unsigned long long)skipped, (unsigned long long)potential, (unsigned long long)skipped_hi, (unsigned long long)seq_differs, (unsigned long long)holes2, (unsigned long long)parts_replayed, (unsigned long long)parts_recorded);
      Check(skipped >= potential && skipped * 100 <= potential * 103 && skipped_hi > 0 && parts_replayed > 0 && seq_differs == 0 && holes2 == 0, "caster.plane_skip_replay", buf);
      snprintf(buf, sizeof buf, "plane skips in a proof frame %llu, with no learned part list %llu, of an object the binder unmarked %llu; refusals (the check's own return value) %llu = the skip decisions that met the lost lists at frame 50 (%llu), refusals elsewhere %llu; frames skipping %llu",
               (unsigned long long)skip_in_proof, (unsigned long long)skip_unproven, (unsigned long long)skip_unmarked, (unsigned long long)refused, (unsigned long long)would_skip_lost, (unsigned long long)refused_other, (unsigned long long)frames_skipping);
      Check(skip_in_proof == 0 && skip_unproven == 0 && skip_unmarked == 0 && refused == would_skip_lost && refused > 0 && refused_other == 0, "caster.plane_skip_refusals", buf);
      PlanePartList after_ring, after_wr;
      const bool ring_ok = plp.Lookup(ring_model, 5, after_ring) && after_ring.n == 1, wr_ok = plp.Lookup(wr_model, 0, after_wr) && after_wr.n == 6;
      snprintf(buf, sizeof buf, "from frame 120 (ring call 5: 2 -> 1 part, wrestler proxy: 8 -> 6): frames whose replay differs from the stream before the object's next proof frame (ring %llu, wrestler %llu) %llu (allowed), after it %llu (must be 0); the ledger holds the new lists: ring call 5 %s, wrestler proxy %s",
               (unsigned long long)ring_pf, (unsigned long long)wr_pf, (unsigned long long)change_diff_before, (unsigned long long)change_diff_after, ring_ok ? "yes" : "NO", wr_ok ? "yes" : "NO");
      Check(change_diff_after == 0 && ring_ok && wr_ok, "caster.plane_part_change", buf);
    }
    g_caster_d3 = false;
  }

  // ---- lane 246 (the ring hook's decision, SKIP_GAME_WORK_MAP.md 16.9): the class table the hook claims calls by, the colour twin's path (PredictPath, set by the hook itself), the ring-only ledger / packets / tally, the ring-rows backing,
  // the outer-recording refusal and the reader's marker claim. The bars: a skip needs path 1 (lane 237), a ring-rows record under the packet's call, no outer recording, no proof frame, a proven gate; a marker's packet is always there;
  // each call is decided and counted by exactly ONE chain; the planted twins each fail their named check.
  {
    g_ring_d3 = true;
    std::unordered_map<uint32_t, uint32_t> mem;
    const auto rd = [&](uint32_t a, uint32_t& v) {
      auto it = mem.find(a);
      if (it == mem.end()) return false;
      v = it->second;
      return true;
    };
    const uint32_t R13 = 0x2000, TLS = 0x3000, BLK = 0x3100, GTAB = 0x40000;
    mem[R13] = TLS, mem[TLS + 1048] = BLK, mem[BLK + 32] = 1;   // slot 1
    const auto ring_model = [](uint32_t i) { return 0x20000u + i * 0x1000u; };
    const auto arena_object = [](uint32_t i) { return 0x50000u + i * 0x1000u; };
    const uint32_t kNRing = 10, kNArena = 5;
    // the class table: G = [0x836F4C74], the first 10 class entries made (rope hi .. canvas), the 3 skirts not made (pointer 0)
    mem[rdr_scene::live::ringrows::kTableGlobal] = GTAB;
    for (uint32_t k = 0; k < rdr_scene::live::ringrows::kClassTableCount; ++k) mem[GTAB + rdr_scene::live::ringrows::kClassTable[k].offset] = k < kNRing ? ring_model(k) : 0u;
    rdr_scene::live::ringrows::RingTable table;
    bool table_ok = rdr_scene::live::ringrows::ReadRingTable(rd, table) && table.valid;
    for (uint32_t k = 0; k < kNRing; ++k) table_ok = table_ok && table.ClassOf(ring_model(k)) == rdr_scene::live::ringrows::kClassTable[k].cls;
    for (uint32_t k = 0; k < kNArena; ++k) table_ok = table_ok && table.ClassOf(arena_object(k)) == rdr_scene::kRcNone;
    table_ok = table_ok && table.ClassOf(0) == rdr_scene::kRcNone;
    for (uint32_t i = 0; i < kRingModelSlots; ++i) g_ring_models[i].store(table.valid ? table.ptr[i] : 0u, std::memory_order_relaxed);   // what the hook publishes (d3_ring_hook.h ringclass::IsRingModel)
    bool pub_ok = true;
    for (uint32_t k = 0; k < kNRing; ++k) pub_ok = pub_ok && IsLiveRingModel(ring_model(k));
    for (uint32_t k = 0; k < kNArena; ++k) pub_ok = pub_ok && !IsLiveRingModel(arena_object(k));
    pub_ok = pub_ok && !IsLiveRingModel(0);
    Check(table_ok && pub_ok && kRingModelSlots == rdr_scene::live::ringrows::kClassTableCount, "ring.class_table", "the 13-pointer class table of [0x836F4C74] decides a ring model; an arena object, a null model and a skirt that is not made are not ring; the published set (the binder's filter) equals it");

    // the models: 0..3 path 1 (cache bit clear), 4 path 2 (a list in the slot), 5 path 3 (empty handle), 6 path 1 ([model+888] == 0), 7 path 1 but the ring-rows tracker wrote no rows, 8 path 1 but its record sits under another call, 9 path 4
    const auto setup = [&](uint32_t i) {
      const uint32_t M = ring_model(i), LIST = M + 0x400, FLAGS = M + 0x500, H = M + 0x600;
      mem[M + 888] = i == 6 ? 0u : 1u, mem[M + 600] = (i <= 3 || i == 6 || i == 7 || i == 8) ? 0u : 1u;
      mem[M + 896] = LIST, mem[LIST + 4] = i == 4 ? 5u : 0u, mem[M + 900] = FLAGS, mem[FLAGS + 4] = 0, mem[M + 848] = H, mem[M + 852] = H, mem[H] = i == 9 ? 77u : 0u;
    };
    const CachePath want[10] = {CachePath::Fresh1, CachePath::Fresh1, CachePath::Fresh1, CachePath::Fresh1, CachePath::Fresh2, CachePath::Record3, CachePath::Fresh1, CachePath::Fresh1, CachePath::Fresh1, CachePath::Cached4};
    bool paths_ok = true;
    for (uint32_t i = 0; i < kNRing; ++i) setup(i), paths_ok = paths_ok && PredictPath(rd, ring_model(i), R13) == want[i];
    mem.erase(ring_model(4) + 896);
    paths_ok = paths_ok && PredictPath(rd, ring_model(4), R13) == CachePath::Unreadable;
    setup(4);
    Check(paths_ok, "ring.paths", "the ring model's path is read from the words the colour twin reads: path 1 (cache bit clear / no list), a list in the slot is Fresh2, empty handle Record3, filled handle Cached4; an unreadable word is never a skip");

    GuestObjects robjs{kMaxRingCalls}, aobjs;   // lane 258: the ring ledger follows 16 calls an object
    PacketTable rpackets(512), apackets(512);
    GuestTally rtally, atally;
    Gate rgate, agate;
    MarkPublisher rmarks, amarks;
    std::vector<uint64_t> rkeys, akeys;
    for (uint32_t i = 0; i < kNRing; ++i) rkeys.push_back(MarkKey(ring_model(i), 0));
    for (uint32_t i = 0; i < kNArena; ++i) akeys.push_back(MarkKey(arena_object(i), 0));
    rmarks.Publish(rkeys);
    amarks.Publish(akeys);
    const auto rset = rmarks.Acquire(), aset = amarks.Acquire();
    const Fault rf = rplant == "hole_no_packet" ? Fault::HoleNoPacket : rplant == "skip_in_proof_frame" ? Fault::SkipInProofFrame : Fault::None;
    uint64_t skips = 0, holes = 0, skipped_in_proof = 0, unbacked_skips = 0, path2_skips = 0, outer_skips = 0, gate_early = 0, markers_total = 0, ring_calls = 0, arena_calls = 0, path2_calls = 0, nonfresh1_skips = 0;
    for (uint32_t f = 0; f < 200; ++f) {
      const int64_t now = int64_t(f) * 16;
      if (f >= 5) {   // the gates prove from frame 5: no skip before
        rgate.Report(true, 0, now), rgate.Report(true, 0, now), rgate.Report(true, 0, now);
        agate.Report(true, 0, now), agate.Report(true, 0, now), agate.Report(true, 0, now);
      }
      std::vector<std::pair<uint32_t, uint32_t>> markers;   // (model, ordinal)
      for (uint32_t i = 0; i < kNRing; ++i) {
        setup(i);
        const uint32_t M = ring_model(i);
        CallInputs in;
        in.object = M, in.frame = f, in.now_ms = now, in.owner = true, in.ring = true, in.proof_skew = kHookToStreamOffset, in.state_readable = true, in.state_count = 0;
        // the hook's own path (precondition 1 / 2). The plant ring_path_unset leaves CallInputs' default (Fresh1); ring_path2_skipped reads path 2 as Fresh1
        const CachePath true_path = PredictPath(rd, M, R13);
        in.path = rplant == "path_unset" || (rplant == "path2_skipped" && true_path == CachePath::Fresh2) ? CachePath::Fresh1 : true_path;
        ++ring_calls;
        if (true_path == CachePath::Fresh2) ++path2_calls;
        GuestCall gc = BeginGuestCall(robjs, in, rset.get(), rpackets, rtally, rgate, rf);
        if (rplant == "wrong_chain") {   // the plant: the arena's hook also decides (and counts) the ring's call
          CallInputs ain = in;
          ain.ring = false;
          GuestCall ag = BeginGuestCall(aobjs, ain, aset.get(), apackets, atally, agate);
          EndGuestCall(aobjs, ag, CallInfo{2, 0}, Fault::None, &atally);
        }
        const bool backed = i != 7;
        const uint32_t tracker_call = i == 8 ? 1u : 0u;
        WithdrawIfRingRowsUnbacked(gc.out, in, backed, tracker_call, rpackets, rtally, rplant == "rows_unbacked");
        const OuterCache oc = (i == 1 && f % 3 == 0) ? OuterCache::Open : (i == 2 && f % 5 == 0) ? OuterCache::Unreadable : OuterCache::Closed;
        WithdrawIfOuterRecording(gc.out, in, oc, rpackets, rtally, rplant == "outer_ignored");
        if (gc.out.why == Why::Skip) {
          ++skips;
          markers.push_back({M, gc.out.ordinal});
          if (ObjectProofFrame(M, f) || ObjectProofFrame(M, f + kHookToStreamOffset)) ++skipped_in_proof;
          if (i == 7 || i == 8) ++unbacked_skips;
          if (true_path == CachePath::Fresh2) ++path2_skips;
          if (true_path != CachePath::Fresh1) ++nonfresh1_skips;
          if (oc != OuterCache::Closed) ++outer_skips;
          if (f < 5) ++gate_early;
        }
        EndGuestCall(robjs, gc, CallInfo{2, 0}, Fault::None, &rtally);
      }
      for (uint32_t i = 0; i < kNArena; ++i) {   // the arena's objects: the arena chain, its own ledger / packets / tally
        const uint32_t A = arena_object(i);
        mem[A + 888] = 0;   // path 1
        mem[A + 600] = 0;
        CallInputs in;
        in.object = A, in.frame = f, in.now_ms = now, in.owner = true, in.proof_skew = kHookToStreamOffset, in.state_readable = true, in.state_count = 0;
        in.path = PredictPath(rd, A, R13);
        ++arena_calls;
        GuestCall gc = BeginGuestCall(aobjs, in, aset.get(), apackets, atally, agate);
        EndGuestCall(aobjs, gc, CallInfo{2, 0}, Fault::None, &atally);
        if (gc.out.why == Why::Skip) (void)apackets.Take(f, A, gc.out.ordinal);   // (the arena's own binder would take it)
      }
      for (const auto& m : markers) {   // the ring chain's binder meets each marker and takes its packet by (frame, model, ordinal)
        ++markers_total;
        if (!rpackets.Take(f, m.first, m.second)) ++holes;
      }
    }
    char buf[320];
    std::snprintf(buf, sizeof buf, "%llu markers, %llu packet(s) missing", (unsigned long long)markers_total, (unsigned long long)holes);
    Check(skips > 0 && holes == 0, "ring.no_hole", buf);
    std::snprintf(buf, sizeof buf, "%llu skip(s) in an object's proof frame (either clock)", (unsigned long long)skipped_in_proof);
    Check(skipped_in_proof == 0, "ring.proof_frames_full", buf);
    std::snprintf(buf, sizeof buf, "%llu skip(s) of a path-2 (list in the slot) call, %llu skip(s) on any path but 1", (unsigned long long)path2_skips, (unsigned long long)nonfresh1_skips);
    Check(path2_skips == 0 && nonfresh1_skips == 0, "ring.path2_never_skipped", buf);
    std::snprintf(buf, sizeof buf, "%llu skip(s) whose ring-rows record is missing or under another call", (unsigned long long)unbacked_skips);
    Check(unbacked_skips == 0, "ring.rows_backed", buf);
    std::snprintf(buf, sizeof buf, "%llu skip(s) / marker(s) while an outer cache recording was open or unreadable", (unsigned long long)outer_skips);
    Check(outer_skips == 0, "ring.outer_never_marked", buf);
    std::snprintf(buf, sizeof buf, "%llu skip(s) before the gate was proven", (unsigned long long)gate_early);
    Check(gate_early == 0, "ring.gate_respected", buf);
    std::snprintf(buf, sizeof buf, "ring tally %llu calls (hook made %llu), arena tally %llu calls (arena made %llu): one call, one chain", (unsigned long long)rtally.cum_calls.load(), (unsigned long long)ring_calls,
                  (unsigned long long)atally.cum_calls.load(), (unsigned long long)arena_calls);
    Check(rtally.cum_calls.load() == ring_calls && atally.cum_calls.load() == arena_calls, "ring.one_chain", buf);
    std::snprintf(buf, sizeof buf, "ring tally paths fresh1 %llu fresh2 %llu record3 %llu cached4 %llu unreadable %llu; path-2 calls made %llu", (unsigned long long)rtally.cum_path[0].load(), (unsigned long long)rtally.cum_path[1].load(),
                  (unsigned long long)rtally.cum_path[2].load(), (unsigned long long)rtally.cum_path[3].load(), (unsigned long long)rtally.cum_path[4].load(), (unsigned long long)path2_calls);
    Check(rtally.cum_path[1].load() == path2_calls && path2_calls > 0 && rtally.cum_path[0].load() + rtally.cum_path[1].load() + rtally.cum_path[2].load() + rtally.cum_path[3].load() + rtally.cum_path[4].load() == ring_calls,
          "ring.tally_paths", buf);
    const FrameCounts rfc = rtally.TakeAndReset();
    std::snprintf(buf, sizeof buf, "markers %u == skips %llu (a withdrawn skip is not a marker)", rfc.markers, (unsigned long long)skips);
    Check(rfc.markers == skips, "ring.tally_markers", buf);

    {   // the withdraw itself: the packet leaves the table, the counters move from skip to unreadable; a backed call under the packet's own call is left alone
      PacketTable pt(8);
      GuestTally tl;
      CallOutcome out;
      out.why = Why::Skip, out.ordinal = 0, out.packet_written = true, out.packet_call = 2;
      CallInputs in;
      in.object = 77, in.frame = 5;
      Packet p;
      p.frame = 5, p.object = 77, p.ordinal = 0, p.call = 2;
      pt.Put(std::move(p));
      tl.Note(Why::Skip);
      const bool w = WithdrawIfRingRowsUnbacked(out, in, true, 3, pt, tl);   // the record sits under call 3, the packet carries 2
      bool ok = w && out.why == Why::Unreadable && !out.packet_written && pt.size() == 0 && tl.markers.load() == 0 && tl.why[uint8_t(Why::Unreadable)].load() == 1 && tl.dropped.load() == 1;
      CallOutcome same;
      same.why = Why::Skip, same.ordinal = 0, same.packet_written = true, same.packet_call = 2;
      ok = ok && !WithdrawIfRingRowsUnbacked(same, in, true, 2, pt, tl) && same.why == Why::Skip;
      CallOutcome empty;
      empty.why = Why::Skip, empty.ordinal = 0, empty.packet_written = true, empty.packet_call = 2;
      ok = ok && WithdrawIfRingRowsUnbacked(empty, in, false, 2, pt, tl) && empty.why == Why::Unreadable;   // no rows behind the marker
      Check(ok, "ring.withdraw", "a skip whose ring-rows record is missing, empty or under another call is withdrawn (packet taken back, counted unreadable); a backed one is left alone");
    }
    {   // the reader's claim (precondition 4): a ring marker is claimed when the ring chain is on and not by any other class's test; the others are as before
      const bool saved_ring = g_ring_d3, saved_caster = g_caster_d3, saved_wr = g_wrestlers_d3, saved_arena = g_arena_d3;
      g_ring_d3 = rplant != "marker_unclaimed";   // the plant: the reader's ring test is missing
      g_caster_d3 = g_wrestlers_d3 = false, g_arena_d3 = true;
      bool ok = MarkerClaimed(kRingMarkerCall + 7) && MarkerClaimed(kRingMarkerCall) && MarkerClaimed(kMarkerCall + 3);
      ok = ok && !MarkerClaimed(CasterMarkerCall(1, 2)) && !MarkerClaimed(kWrMarkerCall + 1);   // another class's marker is not the ring's to claim
      ok = ok && IsAnyMarkerCall(kRingMarkerCall + 9) && IsAnyMarkerCall(kMarkerCall) && !IsAnyMarkerCall(0x82C0A950u) && !IsMarkerCall(kRingMarkerCall + 1) && !IsCasterMarkerCall(kRingMarkerCall + 1);
      g_ring_d3 = false, g_arena_d3 = false;
      ok = ok && !AnyD3Chain();
      g_ring_d3 = true;
      ok = ok && AnyD3Chain();   // ring alone is a chain: the reader's top gate must let its markers through
      g_ring_d3 = saved_ring, g_caster_d3 = saved_caster, g_wrestlers_d3 = saved_wr, g_arena_d3 = saved_arena;
      Check(ok, "ring.marker_claimed", "the reader claims a ring marker behind g_ring_d3 (d3_arena.h MarkerClaimed, the code reader.h runs); the other classes' markers and a real draw's call site are unchanged");
    }
    {   // lane 289 (the ring SKIP arm): the mark set carries each marked call's part triples and is found by the generation a marker carries (the reader's and the binder's ordinal fill read it)
      MarkPublisher pub;
      const uint64_t k1 = MarkKey(0xAAF52A20u, 0), k2 = MarkKey(0xAAF52A20u, 1);
      const std::vector<uint64_t> t1 = {11, 12, 12}, t2 = {21, 22};
      bool ok = true;
      const uint32_t g1 = rplant == "parts_dropped" ? pub.Publish({k1, k2}) : pub.PublishParts({{k2, t2}, {k1, t1}});   // (unsorted in: the publisher sorts keys with their parts)
      const uint32_t g2 = rplant == "parts_dropped" ? pub.Publish({k1}) : pub.PublishParts({{k1, t1}});
      const auto s1 = pub.Find(g1), s2 = pub.Find(g2);
      ok = ok && g1 == 1 && g2 == 2 && s1 && s2 && s1->generation == 1 && s2->generation == 2 && s1->Has(k2) && !s2->Has(k2);
      const std::vector<uint64_t>* p1 = s1 ? s1->PartsOf(k1) : nullptr;
      const std::vector<uint64_t>* p2 = s1 ? s1->PartsOf(k2) : nullptr;
      ok = ok && p1 && p2 && *p1 == t1 && *p2 == t2 && !(s2 && s2->PartsOf(k2));   // the older generation keeps ITS call's triples (a doubled part listed twice); a key the set lacks has none
      ok = ok && pub.PublishParts({{k1, t1}}) == g2 && pub.PublishParts({{k1, {11, 12}}}) == 3;   // the same set keeps its generation; the same keys with other parts are a new set
      for (uint32_t i = 0; i < MarkPublisher::kHistory + 5; ++i) pub.PublishParts({{k1, {100 + i}}});
      ok = ok && !pub.Find(g1) && pub.Find(pub.generation());   // the history is bounded; the newest is always found
      MarkPublisher plain;
      plain.Publish({k1, k2});
      ok = ok && plain.Find(1) && !plain.Find(1)->PartsOf(k1);   // the other chains' sets carry no parts
      Check(ok, "ring.mark_parts", "the ring's mark set carries each marked call's part triples, found by the generation a marker carries (history bounded, same set keeps its generation, plain sets carry none)");
    }
    g_ring_d3 = false;
    for (uint32_t i = 0; i < kRingModelSlots; ++i) g_ring_models[i].store(0, std::memory_order_relaxed);
  }

  // ---- lane 245 (the wrestler hook's decision, SKIP_GAME_WORK_MAP.md 18.9 = the nine preconditions of 18.7): BeginWrestlerCall on fake guest memory, the wrestler globals' stand-ins (own ledger / packets / marks / gate / tally) --------------
  // The words are the ones sub_82BD7B50 reads [C listing 0x82BD7B6C-0x82BD7C44]; the arena's PredictPath is the predictor (the colour call IS the arena's function). Nine models: 0-1 path 1, 2 path 2 (a list in the slot),
  // 3 path 3 (an empty handle), 4 path 4 (a handle with a list), 5 unreadable words, 6 path 1 with an OUTER cache recording open on some frames, 7 path 1 with the light camera / rim depth missing on some frames, 8 path 1 whose
  // light state cannot be read. The bars: only path 1 skips, with a packet, a proven gate, outside a proof frame, a readable state, a closed outer recording and both caster records; every marker has its packet; the wrestler
  // switch decides (the arena's does not); a call whose path was never predicted cannot skip.
  {
    const bool arena_was = g_arena_d3;
    g_arena_d3 = false, g_wrestlers_d3 = true;
    std::unordered_map<uint32_t, uint32_t> mem;
    const auto rd = [&](uint32_t a, uint32_t& v) {
      auto it = mem.find(a);
      if (it == mem.end()) return false;
      v = it->second;
      return true;
    };
    const uint32_t R13 = 0x2000, TLS = 0x3000, BLK = 0x3100, REC = 0x7000, BASE = 0x7100, TAB = 0x7200;   // slot 1
    mem[R13] = TLS, mem[TLS + 1048] = BLK, mem[BLK + 32] = 1;
    const auto model_at = [](uint32_t i) { return 0x20000u + i * 0x1000u; };
    const auto setup = [&](uint32_t i) {
      const uint32_t M = model_at(i), LIST = M + 0x400, FLAGS = M + 0x500, H = M + 0x600;
      mem.erase(M + 896);
      mem[M + 888] = i == 0 ? 0u : 1u, mem[M + 600] = (i == 0 || i == 1) ? (i == 0 ? 1u : 0u) : 1u;
      if (i >= 2 && i != 5) {
        mem[M + 896] = LIST, mem[LIST + 4] = i == 2 ? 5u : 0u, mem[M + 900] = FLAGS, mem[FLAGS + 4] = 0, mem[M + 848] = H, mem[M + 852] = H, mem[H] = i == 4 ? 77u : 0u;
        if (i >= 6) mem[M + 600] = 0;   // 6, 7, 8: no cache in use: path 1
      }
    };
    const CachePath want[9] = {CachePath::Fresh1, CachePath::Fresh1, CachePath::Fresh2, CachePath::Record3, CachePath::Cached4, CachePath::Unreadable, CachePath::Fresh1, CachePath::Fresh1, CachePath::Fresh1};
    bool paths_ok = true;
    for (uint32_t i = 0; i < 9; ++i) {
      setup(i);
      paths_ok = paths_ok && PredictPath(rd, model_at(i), R13) == want[i];
    }
    Check(paths_ok, "wrestler.paths", "the colour call's cached-list path is read from the words sub_82BD7B50 reads: path 1 fresh, a list in the slot is Fresh2, an empty handle Record3, a full one Cached4, an unreadable word never a skip");
    {   // precondition 1: the default is Unreadable and a call that never predicts its path cannot skip
      CallInputs fresh = WrestlerCallInputs();
      GuestObjects objs0;
      PacketTable pk0(16);
      GuestTally tl0;
      Gate gt0;
      for (int i = 0; i < 3; ++i) gt0.Report(true, 0, 1000);
      MarkPublisher mp0;
      mp0.Publish({MarkKey(model_at(0), 0)});
      const auto s0 = mp0.Acquire();
      fresh.object = model_at(0), fresh.now_ms = 1000, fresh.owner = true;
      uint64_t f0 = 100;
      while (ObjectProofFrame(fresh.object, f0) || ObjectProofFrame(fresh.object, f0 + 1) || ObjectProofFrame(fresh.object, f0 + 2) || ObjectProofFrame(fresh.object, f0 + 3)) ++f0;   // two frames outside the object's proof frames (either clock)
      fresh.frame = f0;
      GuestCall prior = BeginGuestCall(objs0, fresh, s0.get(), pk0, tl0, gt0);
      EndGuestCall(objs0, prior, CallInfo{2, 0}, Fault::None, &tl0);
      fresh.frame = f0 + 1;
      GuestCall gc0 = BeginGuestCall(objs0, fresh, s0.get(), pk0, tl0, gt0);   // eligible (it drew keyed parts last frame), marked, gate proven: only the path stops it
      Check(fresh.wrestler && fresh.path == CachePath::Unreadable && gc0.out.why == Why::Unreadable && !gc0.out.packet_written && pk0.size() == 0, "wrestler.default_unreadable",
            "CallInputs::path of a wrestler call defaults to Unreadable (never Fresh1): a call whose path was not predicted gets no packet and no marker");
    }

    GuestObjects objs;
    PacketTable packets(512);
    GuestTally tally;
    Gate gate;
    MarkPublisher marks;
    WrestlerHookStats st;
    std::vector<uint64_t> keys;
    for (uint32_t i = 0; i < 9; ++i) keys.push_back(MarkKey(model_at(i), 0));
    marks.Publish(keys);
    const auto set = marks.Acquire();
    const Fault wf = wplant == "hole_no_packet" ? Fault::HoleNoPacket : wplant == "skip_in_proof_frame" ? Fault::SkipInProofFrame : wplant == "stale_record" ? Fault::SkipUnreadable : Fault::None;
    WrestlerFaults pl;
    pl.path_unset = wplant == "path_default_fresh1", pl.path2_as_fresh = wplant == "path2_skipped", pl.outer_ignored = wplant == "outer_ignored", pl.camera_ignored = wplant == "camera_ignored", pl.sphere_ignored = wplant == "sphere_ignored";
    uint64_t skips = 0, holes = 0, skipped_in_proof = 0, unbacked = 0, nonfresh_skips = 0, nonfresh_packets = 0, gate_early = 0, markers_total = 0, outer_skips = 0, nocam_skips = 0, withdrawn_cam = 0;
    uint64_t other_class_puts = 0, nosph_skips = 0;
    for (uint32_t f = 0; f < 200; ++f) {
      const int64_t now = int64_t(f) * 16;
      if (f >= 5) gate.Report(true, 0, now), gate.Report(true, 0, now), gate.Report(true, 0, now);
      std::vector<std::pair<uint32_t, uint32_t>> markers;
      for (uint32_t i = 0; i < 9; ++i) {
        setup(i);
        const uint32_t M = model_at(i);
        const bool outer_open = i == 6 && f % 3 == 0;
        if (outer_open) mem[kCacheRecorderWord] = REC, mem[REC + 104] = BASE, mem[REC + 156] = TAB, mem[TAB + 4] = 3, mem[BASE + 68 * 3 + 36] = 1;
        else mem[kCacheRecorderWord] = 0;
        const bool no_cam = i == 7 && f % 4 == 0, no_rim = i == 7 && f % 6 == 0;
        const bool no_sph = i == 1 && f % 5 == 0;   // a known sphere skeleton whose update has not written this frame's row yet (reviewer SF2)
        CallInputs in = WrestlerCallInputs();
        in.object = M, in.frame = f, in.now_ms = now, in.owner = true, in.proof_skew = kHookToStreamOffset;
        in.state_readable = i != 8 || wf == Fault::SkipUnreadable, in.state_count = 0;
        if (pl.path_unset) in.path = CachePath::Fresh1;   // the plant: the OLD default
        GuestCall gc = BeginWrestlerCall(objs, in, R13, rd, [&](uint64_t, bool& cam, bool& rim) { cam = !no_cam, rim = !no_rim; }, [&](uint64_t, uint32_t) { return i == 1 ? (no_sph ? 2 : 1) : 0; }, set.get(), packets, tally, gate, st, wf, pl);
        const bool skip = gc.out.why == Why::Skip;
        const bool path1 = want[i] == CachePath::Fresh1;
        if (!path1 && gc.out.packet_written) ++nonfresh_packets;
        if (skip) {
          ++skips;
          markers.push_back({M, gc.out.ordinal});
          if (!path1) ++nonfresh_skips;
          if (ObjectProofFrame(M, f) || ObjectProofFrame(M, f + kHookToStreamOffset)) ++skipped_in_proof;
          if (i == 8) ++unbacked;
          if (outer_open) ++outer_skips;
          if (no_cam || no_rim) ++nocam_skips;
          if (no_sph) ++nosph_skips;
          if (f < 5) ++gate_early;
        } else if (gc.out.why == Why::Unreadable && i == 7) {
          ++withdrawn_cam;
        }
        CallInfo info;
        info.keyed = 2;
        EndGuestCall(objs, gc, info, Fault::None, &tally);
      }
      for (const auto& m : markers) {
        ++markers_total;
        if (!packets.Take(f, m.first, m.second)) ++holes;
      }
    }
    char buf[300];
    std::snprintf(buf, sizeof buf, "%llu markers, %llu packet(s) missing", (unsigned long long)markers_total, (unsigned long long)holes);
    Check(skips > 0 && holes == 0, "wrestler.no_hole", buf);
    std::snprintf(buf, sizeof buf, "%llu skip(s) in an object's proof frame (either clock)", (unsigned long long)skipped_in_proof);
    Check(skipped_in_proof == 0, "wrestler.proof_frames_full", buf);
    std::snprintf(buf, sizeof buf, "%llu skip(s) whose light state could not be read", (unsigned long long)unbacked);
    Check(unbacked == 0, "wrestler.record_backed", buf);
    std::snprintf(buf, sizeof buf, "%llu skip(s) of a call on cache path 2 / 3 / 4 / unreadable, %llu packet(s) written for one", (unsigned long long)nonfresh_skips, (unsigned long long)nonfresh_packets);
    Check(nonfresh_skips == 0 && nonfresh_packets == 0, "wrestler.only_fresh1", buf);
    std::snprintf(buf, sizeof buf, "%llu skip(s) / marker(s) while an outer cache recording was open", (unsigned long long)outer_skips);
    Check(outer_skips == 0 && st.outer_open.load() > 0, "wrestler.outer_never_marked", buf);
    std::snprintf(buf, sizeof buf, "%llu skip(s) without the hook frame's light camera or rim depth (%llu withdrawn: camera %llu, rim %llu)", (unsigned long long)nocam_skips, (unsigned long long)withdrawn_cam, (unsigned long long)st.no_camera.load(), (unsigned long long)st.no_rim.load());
    Check(nocam_skips == 0 && st.no_camera.load() > 0 && st.no_rim.load() > 0, "wrestler.camera_required", buf);
    std::snprintf(buf, sizeof buf, "%llu skip(s) of a known sphere skeleton with no sphere row this frame (%llu withdrawn)", (unsigned long long)nosph_skips, (unsigned long long)st.no_sphere.load());
    Check(nosph_skips == 0 && st.no_sphere.load() > 0, "wrestler.sphere_row_required", buf);
    std::snprintf(buf, sizeof buf, "%llu skip(s) before the gate was proven", (unsigned long long)gate_early);
    Check(gate_early == 0, "wrestler.gate_respected", buf);
    const FrameCounts wfc = tally.TakeAndReset();
    std::snprintf(buf, sizeof buf, "markers %u == skips %llu (a withdrawn skip is not a marker)", wfc.markers, (unsigned long long)skips);
    Check(wfc.markers == skips, "wrestler.tally_markers", buf);
    {   // the histogram the verify run prints: every call counted once under its predicted path, the marked ones apart; and the line carries the numbers
      uint64_t sum = 0, msum = 0;
      for (int p = 0; p < 5; ++p) sum += st.path[p].load(), msum += st.marked_path[p].load();
      const std::string line = st.Line();
      Check(st.calls.load() == 9 * 200 && sum == st.calls.load() && msum == st.marked.load() && st.marked.load() > 0 && st.path[uint8_t(CachePath::Record3)].load() == 200 && st.path[uint8_t(CachePath::Unreadable)].load() == 200 &&
                line.find("calls the mark set holds") != std::string::npos && line.find("no light camera") != std::string::npos,
            "wrestler.path_histogram", "the path histogram counts every call under the path it was predicted to take (the marked ones apart) and the log line says it");
    }
    {   // the class switch is the wrestler's own: with only the arena's on, a wrestler call never skips (and the chain touched no arena global)
      g_wrestlers_d3 = false, g_arena_d3 = true;
      GuestObjects o2;
      PacketTable p2(64);
      GuestTally t2;
      WrestlerHookStats s2;
      Gate g2;
      for (int i = 0; i < 3; ++i) g2.Report(true, 0, 5000);
      uint64_t sk2 = 0;
      for (uint32_t f = 0; f < 40; ++f) {
        setup(0);
        mem[kCacheRecorderWord] = 0;
        CallInputs in = WrestlerCallInputs();
        in.object = model_at(0), in.frame = f, in.now_ms = 5000, in.owner = true;
        GuestCall gc = BeginWrestlerCall(o2, in, R13, rd, [](uint64_t, bool& c, bool& r) { c = r = true; }, [](uint64_t, uint32_t) { return 0; }, set.get(), p2, t2, g2, s2);
        sk2 += gc.out.why == Why::Skip;
        EndGuestCall(o2, gc, CallInfo{2, 0}, Fault::None, &t2);
      }
      other_class_puts = g_packets.stats().put + g_cpackets.stats().put + g_tally.cum_calls.load() + g_ctally.cum_calls.load() + g_marks.generation();
      Check(sk2 == 0 && other_class_puts == 0, "wrestler.switch_own", "a wrestler call is decided by g_wrestlers_d3 only (the arena's switch alone never skips it) and touches none of the arena's / casters' globals");
    }
    g_arena_d3 = arena_was, g_wrestlers_d3 = false;
  }

  // ---- lane 245 (reviewer, with the ring live seam merged): the three scopes of the one hook, ring first, then the wrestler, then the arena (draw_origin.h). A ring model is claimed by the ring scope alone (the wrestler scope
  // returns uncounted: t_open is open), every other call is seen by the wrestler scope (its marks hold wrestler models only) and, if it did not skip, by the arena's. Each chain keeps its own ledger / packets / marks / gate / tally. -------
  {
    const bool a_was = g_arena_d3, r_was = g_ring_d3, w_was = g_wrestlers_d3;
    g_arena_d3 = g_ring_d3 = g_wrestlers_d3 = true;
    std::unordered_map<uint32_t, uint32_t> mem;
    const auto rd = [&](uint32_t a, uint32_t& v) {
      auto it = mem.find(a);
      if (it == mem.end()) return false;
      v = it->second;
      return true;
    };
    const uint32_t R13 = 0x2000, TLS = 0x3000, BLK = 0x3100;
    mem[R13] = TLS, mem[TLS + 1048] = BLK, mem[BLK + 32] = 1, mem[kCacheRecorderWord] = 0;
    const auto ring_m = [](uint32_t i) { return 0x40000u + i * 0x1000u; };
    const auto wr_m = [](uint32_t i) { return 0x50000u + i * 0x1000u; };
    const auto ar_m = [](uint32_t i) { return 0x60000u + i * 0x1000u; };
    for (uint32_t i = 0; i < 3; ++i)
      for (uint32_t M : {ring_m(i), wr_m(i), ar_m(i)}) mem[M + 888] = 0, mem[M + 600] = 0;   // all path 1
    for (uint32_t i = 0; i < kRingModelSlots; ++i) g_ring_models[i].store(i < 3 ? ring_m(i) : 0u, std::memory_order_relaxed);
    GuestObjects robjs, wobjs, aobjs;
    PacketTable rpk(256), wpk(256), apk(256);
    GuestTally rtl, wtl, atl;
    Gate rg, wg, ag;
    MarkPublisher rm, wm, am;
    WrestlerHookStats wst;
    std::vector<uint64_t> rk, wk, ak;
    for (uint32_t i = 0; i < 3; ++i) rk.push_back(MarkKey(ring_m(i), 0)), wk.push_back(MarkKey(wr_m(i), 0)), ak.push_back(MarkKey(ar_m(i), 0));
    rm.Publish(rk), wm.Publish(wk), am.Publish(ak);
    const auto rs = rm.Acquire(), ws = wm.Acquire(), as = am.Acquire();
    const bool wrong = wplant == "wrong_chain";   // the plant: the wrestler scope also takes the ring's calls
    uint64_t ring_calls = 0, other_calls = 0, ring_skips = 0, wr_skips = 0, ar_skips = 0, wr_on_ring = 0, ar_on_wr = 0, ring_on_other = 0, strays = 0;
    for (uint32_t f = 0; f < 120; ++f) {
      const int64_t now = int64_t(f) * 16;
      if (f >= 5) for (Gate* g : {&rg, &wg, &ag}) g->Report(true, 0, now), g->Report(true, 0, now), g->Report(true, 0, now);
      for (uint32_t k = 0; k < 9; ++k) {
        const uint32_t M = k < 3 ? ring_m(k) : k < 6 ? wr_m(k - 3) : ar_m(k - 6);
        const bool is_ring = IsLiveRingModel(M);
        if (is_ring) {
          CallInputs in;
          in.object = M, in.frame = f, in.now_ms = now, in.owner = true, in.ring = true, in.proof_skew = kHookToStreamOffset, in.state_readable = true;
          in.path = PredictPath(rd, M, R13);
          ++ring_calls;
          GuestCall gc = BeginGuestCall(robjs, in, rs.get(), rpk, rtl, rg);
          if (gc.out.why == Why::Skip) ++ring_skips, (void)rpk.Take(f, M, gc.out.ordinal);
          EndGuestCall(robjs, gc, CallInfo{2, 0}, Fault::None, &rtl);
          if (!wrong) continue;   // the real hook: the ring scope returns / opens t_open, the wrestler and arena scopes never see the call
        }
        ++(is_ring ? strays : other_calls);
        CallInputs in = WrestlerCallInputs();
        in.object = M, in.frame = f, in.now_ms = now, in.owner = true, in.proof_skew = kHookToStreamOffset, in.state_readable = true;
        GuestCall wc = BeginWrestlerCall(wobjs, in, R13, rd, [](uint64_t, bool& c, bool& r) { c = r = true; }, [](uint64_t, uint32_t) { return 0; }, ws.get(), wpk, wtl, wg, wst);
        if (is_ring) ++wr_on_ring;
        const bool wskip = wc.out.why == Why::Skip;
        if (wskip) {
          ++wr_skips;
          if (k < 3 || k >= 6) ++ar_on_wr;   // a wrestler skip of a model that is not a wrestler's
          (void)wpk.Take(f, M, wc.out.ordinal);
        }
        EndGuestCall(wobjs, wc, CallInfo{2, 0}, Fault::None, &wtl);
        if (wskip) continue;   // the wrestler scope returned for the caller: the arena's scope never runs
        CallInputs ain;
        ain.object = M, ain.frame = f, ain.now_ms = now, ain.owner = true, ain.proof_skew = kHookToStreamOffset, ain.state_readable = true;
        ain.path = PredictPath(rd, M, R13);
        GuestCall ac = BeginGuestCall(aobjs, ain, as.get(), apk, atl, ag);
        if (ac.out.why == Why::Skip) ++ar_skips, (void)apk.Take(f, M, ac.out.ordinal);
        if (ac.out.why == Why::Skip && k >= 3 && k < 6) ++ring_on_other;   // the arena skipping a wrestler's model
        EndGuestCall(aobjs, ac, CallInfo{2, 0}, Fault::None, &atl);
      }
    }
    char buf[400];
    std::snprintf(buf, sizeof buf, "ring tally %llu calls (made %llu); wrestler tally %llu calls (the non-ring calls made %llu, it saw %llu ring call(s)); skips ring %llu wrestler %llu arena %llu; wrestler skips of a non-wrestler model %llu; arena skips of a wrestler model %llu",
                  (unsigned long long)rtl.cum_calls.load(), (unsigned long long)ring_calls, (unsigned long long)wtl.cum_calls.load(), (unsigned long long)other_calls, (unsigned long long)wr_on_ring,
                  (unsigned long long)ring_skips, (unsigned long long)wr_skips, (unsigned long long)ar_skips, (unsigned long long)ar_on_wr, (unsigned long long)ring_on_other);
    Check(rtl.cum_calls.load() == ring_calls && wtl.cum_calls.load() == other_calls && wr_on_ring == 0 && ring_skips > 0 && wr_skips > 0 && ar_skips > 0 && ar_on_wr == 0 && ring_on_other == 0 && rpk.size() == 0 && wpk.size() == 0 && apk.size() == 0,
          "wrestler.three_scopes", buf);
    (void)strays;
    g_arena_d3 = a_was, g_ring_d3 = r_was, g_wrestlers_d3 = w_was;
    for (uint32_t i = 0; i < kRingModelSlots; ++i) g_ring_models[i].store(0, std::memory_order_relaxed);
  }

  // ---- lane 245: SphereOcc as a record source (SKIP_GAME_WORK_MAP.md 18.8 / 18.9): lane 242's section 27 rows joined by (hook stamp, engine object) -------------------------------------------------------------------------------------
  {
    auto t = std::make_shared<rdr_scene::SceneTables>();
    t->header.guest_frame = 41;
    t->light_states.resize(1);
    rdr_scene::LightRef ref;
    ref.frame = 40, ref.object = 0xABC, ref.ordinal = 0, ref.state = 0;
    t->light_refs.push_back(ref);
    const auto row = [&](uint32_t frame, uint32_t object, float seed, uint32_t ran) {
      rdr_scene::SphereOccRow r;
      r.flags = rdr_scene::kSoSpheres | rdr_scene::kSoSkeleton | ran, r.object = object, r.frame = frame, r.count = 2;
      for (int i = 0; i < 2; ++i)
        for (int c = 0; c < 4; ++c) r.p[i][c] = seed + float(i * 10 + c), r.v[i][c] = seed + 100.f + float(i * 10 + c);
      return r;
    };
    t->sphere_occ.push_back(row(40, 0xABC, 1.f, rdr_scene::kSoSphere0Ran | rdr_scene::kSoSphere1Ran));
    t->sphere_occ.push_back(row(39, 0xABC, 2.f, rdr_scene::kSoSphere0Ran | rdr_scene::kSoSphere1Ran));
    t->sphere_occ.push_back(row(40, 0xDEF, 3.f, rdr_scene::kSoSphere0Ran));   // another skeleton, sphere 1 not computed
    RecordIndex idx;
    idx.Add(t);
    RecordSrc rs;
    idx.FindWrestler(40, 0xABC, 0, 0, Fault::None, rs);
    uint32_t v[4];
    float f4[4];
    bool ok = rs.have_sphere && idx.sphere_rows() == 3 && rs.sphere_ran == 3 && rs.have_sphere_alt[0] && rs.have_sphere_alt[1] && !rs.have_sphere_alt[2];
    ok = ok && WrestlerRecordRegister(0, 168, Group::Shadow, false, rs, nullptr, Fault::None, v) == NoSrc::None;
    std::memcpy(f4, v, 16);
    ok = ok && f4[0] == 1.f && f4[3] == 4.f;                                                    // c168 = P0 of the pick (the call's own stamp)
    WrestlerRecordRegister(0, 171, Group::Shadow, false, rs, nullptr, Fault::None, v);
    std::memcpy(f4, v, 16);
    ok = ok && f4[0] == 1.f + 100.f + 10.f;                                                     // c171 = V1
    idx.sphere_pick = 1;                                                                        // the census names the stamp one earlier
    idx.FindWrestler(40, 0xABC, 0, 0, Fault::None, rs);
    WrestlerRecordRegister(0, 168, Group::Shadow, false, rs, nullptr, Fault::None, v);
    std::memcpy(f4, v, 16);
    ok = ok && f4[0] == 2.f;
    idx.sphere_pick = 0;
    idx.FindWrestler(40, 0xDEF, 0, 0, Fault::None, rs);
    ok = ok && WrestlerRecordRegister(0, 168, Group::Shadow, false, rs, nullptr, Fault::None, v) == NoSrc::None && WrestlerRecordRegister(0, 169, Group::Shadow, false, rs, nullptr, Fault::None, v) == NoSrc::SphereOcc;   // a sphere the game did not compute: no source
    idx.FindWrestler(40, 0x999, 0, 0, Fault::None, rs);
    ok = ok && !rs.have_sphere && WrestlerRecordRegister(0, 168, Group::Shadow, false, rs, nullptr, Fault::None, v) == NoSrc::SphereOcc;   // no row: no source, named
    {   // the census counts the stream's value against each candidate stamp
      idx.FindWrestler(40, 0xABC, 0, 0, Fault::None, rs);
      uint32_t truth[4];
      std::memcpy(truth, rs.sphere_alt[1][0], 16);   // the stream carries the row one stamp earlier
      uint64_t reads = 0, norow = 0, eq[3] = {};
      NoteSphereOffsets(reads, norow, eq, rs, 168, truth);
      ok = ok && reads == 1 && norow == 0 && eq[0] == 0 && eq[1] == 1 && eq[2] == 0;
    }
    idx.Evict(0, 0);
    ok = ok && idx.sphere_rows() == 0;
    {   // the stamp pick (reviewer SF1): adopt the decisive candidate between +0 and -1, never on too few reads, never when both or neither are equal
      const uint64_t a[3] = {100, 0, 0}, b[3] = {3, 100, 0}, both[3] = {100, 100, 0}, none[3] = {10, 10, 0}, near[3] = {98, 0, 0};
      ok = ok && ChooseSpherePick(100, a) == 0 && ChooseSpherePick(100, b) == 1 && ChooseSpherePick(100, both) == -1 && ChooseSpherePick(100, none) == -1 && ChooseSpherePick(kSpherePickReads - 1, a) == -1 && ChooseSpherePick(100, near) == -1;
    }
    Check(ok, "wrestler.sphere_source", "VS c168-c171 come from lane 242's recorder row of the call's object (P0, P1, V0, V1), a sphere the game did not compute or a missing row has NO source, the pick and the census follow the candidate stamps, eviction removes the rows");
  }

  std::printf("%s\n", g_fail ? "FAIL" : "PASS");
  return g_fail ? 1 : 0;
}
