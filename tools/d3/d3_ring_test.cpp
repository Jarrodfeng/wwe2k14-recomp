// Lane 229 (Ring D3 R3, INERT; docs\renderer\SKIP_GAME_WORK_MAP.md 16.8): the SHIPPED binder core (src\renderer_adapter\d3_binder.h, d3_record.h) in the ring class's route, driven with SYNTHETIC frames and a SYNTHETIC ring-rows
// record, 0 game runs. No kept capture holds the ring's banks together with a ring-rows record (16.4 item 3), so the RECORD route of the ring (the rows join, the slot -> node map, the don't-care slots, the call-count guard,
// the plants) is proved here against an oracle written from the contract, not from the code under test; the STREAM route is proved on the kept captures by tools\d3\d3_ring_check.py.
// Build (tools\d3\d3_ring_selftest.py does it): clang++ -std=c++20 this + src\renderer_scene\record_io.cpp.
//
//   d3_ring_test.exe                      the honest arm: every check PASS (exit 0)
//   d3_ring_test.exe plant NAME           the same arm with a planted fault; it must FAIL the check named in the selftest (exit 1)
//
// The fixture: a post (rigid variant, 4 calls a frame, 1 part, one node: its instance transform, static), a rope (skinned variant, 3 calls sharing ONE r5, 2 parts with 4 / 3 palette slots, 6 nodes a call, every row moves every
// frame) and, at frame 100, one frame in which the rope has only 2 calls (an LOD flip). Part p, slot k of the rope reads node (2 p + k) % 6 + 1 (1-based). The slots past a part's palette hold junk that changes every draw (the
// stale rows of the previous draw: ring.md Part D). The scene record of hook frame f is delivered with stream frame f + 1 (the +1 of lane 152).
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../../src/renderer_adapter/d3_binder.h"
#include "../../src/scene_live/ring_rows_core.h"

using namespace rdr_adapter;
namespace d3 = rdr_adapter::d3;

namespace {
int g_fail = 0;
void Check(bool ok, const char* name, const std::string& detail = "") {
  std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : "  ", detail.c_str());
  if (!ok) ++g_fail;
}

constexpr uint32_t kPost = 0x50001000, kRope = 0x50002000;
constexpr uint32_t kPostCalls = 4, kRopeCalls = 3, kNodes = 6, kRopeR5 = 0x30000040;
constexpr uint32_t kNpal[2] = {4, 3};
constexpr uint32_t kFlipFrame = 100;
inline uint32_t NodeOf(uint32_t part, uint32_t slot) { return (2 * part + slot) % kNodes + 1; }   // the oracle's slot -> node rule (1-based)
inline uint32_t RopeCalls(uint32_t f) { return f == kFlipFrame ? 2u : kRopeCalls; }

uint32_t H(uint32_t a, uint32_t b, uint32_t c = 0, uint32_t d = 0) {
  uint32_t x = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA6Bu ^ (c + 0x165667B1u) * 0xC2B2AE35u ^ (d + 0x27D4EB2Fu) * 0x1B873593u;
  x ^= x >> 15, x *= 0x2C1B3C6Du, x ^= x >> 12, x *= 0x297A2D39u, x ^= x >> 15;
  return x;
}
float F(uint32_t h) { return float(int32_t(h % 20001) - 10000) / 64.0f; }

struct Bank { std::array<uint32_t, 1024> v{}; };
void SetReg(Bank& b, uint32_t reg, const float* x) { std::memcpy(&b.v[reg * 4], x, 16); }

// ---- the oracle: the node rows a call holds (what the game wrote at r5) and the rows the stream's bank shows ----
// rows of node n (1-based) of call c of object `obj` at hook frame f: 12 floats, row j = floats [4 j, 4 j + 3]
void NodeRows(uint32_t f, uint32_t obj, uint32_t call, uint32_t node, float out[12]) {
  for (uint32_t i = 0; i < 12; ++i) {
    // a post never moves; a rope's nodes move every frame
    out[i] = F(H(obj == kPost ? 0u : f, obj, call * 16 + node, i));
  }
}

struct Spec { uint32_t object, call, nparts; };
std::vector<Spec> FrameSpecs(uint32_t f) {
  std::vector<Spec> v;
  for (uint32_t c = 0; c < kPostCalls; ++c) v.push_back({kPost, c, 1});
  for (uint32_t c = 0; c < RopeCalls(f); ++c) v.push_back({kRope, c, 2});
  return v;
}

constexpr uint64_t kCam = 0xC0FFEE;
void TruthBank(uint32_t f, const Spec& s, uint32_t part, uint32_t draw_ix, Bank& vs) {
  vs = Bank{};
  for (int i = 0; i < 4; ++i) {   // the pass bank: the camera, shared by every colour draw of the frame
    float cam[4];
    for (int k = 0; k < 4; ++k) cam[k] = F(H(f, uint32_t(kCam), uint32_t(i), uint32_t(k)));
    SetReg(vs, 56 + i, cam);
  }
  const float fog[4] = {1.f, 2.f, 3.f, 4.f};
  SetReg(vs, 184, fog);
  if (s.object == kPost) {
    float rows[12];
    NodeRows(f, kPost, s.call, 1, rows);
    for (uint32_t j = 0; j < 3; ++j) SetReg(vs, 76 + j, rows + 4 * j);
    return;
  }
  for (uint32_t slot = 0; slot < 21; ++slot)
    for (uint32_t j = 0; j < 3; ++j) {
      float row[4];
      if (slot < kNpal[part]) {
        float rows[12];
        NodeRows(f, kRope, s.call, NodeOf(part, slot), rows);
        std::memcpy(row, rows + 4 * j, 16);
      } else {   // a slot past the part's palette: the previous draw's rows (junk that changes every draw)
        for (int c = 0; c < 4; ++c) row[c] = F(H(f, 0x57A1E, draw_ix * 21 + slot, uint32_t(c * 3) + j));
      }
      SetReg(vs, 76 + 3 * slot + j, row);
    }
}

struct StreamFrame {
  LoadedFrame frame;
  std::vector<Op> ops;
  std::vector<Draw> draws;
  std::vector<DrawFixed> fixeds;
  std::vector<DrawDynamic> dynamics;
  std::vector<rdr::SamplerDesc> samplers;
  std::vector<TextureSlot> slots;
  std::vector<ConstantBank> banks;
  std::vector<BoolLoop> bools;
  std::vector<DrawOrigin> origins;
  std::string strings;
  std::vector<Bank> truth;
  std::vector<Spec> spec_of_draw;
  std::vector<uint64_t> keys;
  std::vector<thin::ThinKey> fulls;
};
void BuildFrame(uint32_t f, StreamFrame& sf) {
  sf = StreamFrame{};
  sf.fixeds.resize(1), sf.dynamics.resize(1), sf.bools.resize(1);
  sf.strings.push_back('\0');
  uint32_t ix = 0;
  for (const Spec& s : FrameSpecs(f)) {
    sf.ops.push_back({OpKind::Clear, 0});   // a non-draw op ends a run: two calls of one model are two runs (the game's state changes between calls)
    for (uint32_t part = 0; part < s.nparts; ++part, ++ix) {
      Bank vs;
      TruthBank(f, s, part, ix, vs);
      ConstantBank cv, cp;
      std::memcpy(cv.dwords, vs.v.data(), sizeof cv.dwords);
      std::memset(cp.dwords, 0, sizeof cp.dwords);
      Draw d;
      d.vs_hash = s.object == kPost ? 0xB1 : 0xB2, d.ps_hash = 0;
      d.fixed = d.dynamic = d.bool_loop = 0;
      d.vs_bank = uint32_t(sf.banks.size());
      sf.banks.push_back(cv);
      d.ps_bank = uint32_t(sf.banks.size());
      sf.banks.push_back(cp);
      DrawOrigin o;
      o.klass = DrawClass::ModelPart, o.op = 15, o.pass_site = thin::kSiteColour, o.object = s.object, o.part = part;
      sf.ops.push_back({OpKind::Draw, uint32_t(sf.draws.size())});
      sf.draws.push_back(d);
      sf.origins.push_back(o);
      sf.truth.push_back(vs);
      sf.spec_of_draw.push_back(s);
      sf.keys.push_back(thin::Mix(thin::Mix(thin::Mix(0x717, s.object), part), 7) | 1ull);   // the thin key: one per (object, part) (the ordinals share it, as the D1 template does)
      sf.fulls.push_back(thin::ThinKey{});
    }
  }
  sf.frame.header.guest_frame = uint64_t(f) + 1;
  sf.frame.ops = {sf.ops.data(), uint32_t(sf.ops.size())};
  sf.frame.draws = {sf.draws.data(), uint32_t(sf.draws.size())};
  sf.frame.fixeds = {sf.fixeds.data(), 1};
  sf.frame.dynamics = {sf.dynamics.data(), 1};
  sf.frame.samplers = {sf.samplers.data(), 0};
  sf.frame.texture_slots = {sf.slots.data(), 0};
  sf.frame.constant_banks = {sf.banks.data(), uint32_t(sf.banks.size())};
  sf.frame.bool_loops = {sf.bools.data(), 1};
  sf.frame.origins = {sf.origins.data(), uint32_t(sf.origins.size())};
  sf.frame.strings = {sf.strings.data(), uint32_t(sf.strings.size())};
}

// The scene record of hook frame f: a LightRef per call (every ring call has one) and the ring rows of every call (the nodes the packer reads at r5).
std::shared_ptr<const rdr_scene::SceneTables> BuildRecord(uint32_t f) {
  auto t = std::make_shared<rdr_scene::SceneTables>();
  t->header.guest_frame = uint64_t(f) + 1;
  rdr_scene::LightState ls;
  std::memset(&ls, 0, sizeof ls);
  t->light_states.push_back(ls);
  for (const Spec& s : FrameSpecs(f)) {
    rdr_scene::LightRef r;
    r.object = s.object, r.ordinal = s.call, r.state = 0, r.frame = f;
    t->light_refs.push_back(r);
    rdr_scene::RingRows e;
    e.klass = s.object == kPost ? rdr_scene::kRcPostHi : rdr_scene::kRcRopeHi;
    e.object = s.object, e.frame = f, e.call = s.call, e.keyed = s.call;
    e.r5 = s.object == kPost ? 0x30000000u + s.call * 64u : kRopeR5;   // the rope's three calls share ONE r5 (the side's buffer)
    e.first_node = uint32_t(t->ring_nodes.size());
    e.n_nodes = uint16_t(s.object == kPost ? 1 : kNodes);
    if (s.object == kRope) e.flags |= rdr_scene::kRrForeignR5;
    for (uint32_t n = 1; n <= e.n_nodes; ++n) {
      rdr_scene::RingNodeRows nr;
      NodeRows(f, s.object, s.call, n, nr.rows);
      t->ring_nodes.push_back(nr);
    }
    t->ring_rows.push_back(e);
  }
  return t;
}

struct Rig {
  d3::D3Core core;
  d3::PacketTable packets{4096};
  d3::MarkPublisher marks;
  d3::Gate gate;
  d3::Hooks hooks;
  thin::ThinStore::Entry ring_entry;
  std::vector<std::shared_ptr<const rdr_scene::SceneTables>> keep;
  int v_rigid = -1, v_skinned = -1;
  d3::Fault fault;
  static int Find(const char* name) {
    for (size_t i = 0; i < sizeof(rdr::fx::kVariants) / sizeof(rdr::fx::kVariants[0]); ++i)
      if (std::strcmp(rdr::fx::kVariants[i].name, name) == 0) return int(i);
    return -1;
  }
  explicit Rig(d3::Fault f) : fault(f) {
    core.route = thin::Cls::Ring;
    core.mode = d3::Mode::Record;
    core.fault = f;
    core.records.dedupe_r5 = f == d3::Fault::RingDedupeModelR5;
    core.records.index_rings = true;
    core.packets = &packets, core.marks = &marks, core.gate = &gate;
    ring_entry.t.cls = thin::Cls::Ring;
    v_rigid = Find("yAudience_lm_vertex_vertex");
    v_skinned = Find("yAudDefault_skinned_frame_lm_vertex_vertex");
    hooks.variant = [this](uint64_t h) -> int { return h == 0xB1 ? v_rigid : h == 0xB2 ? v_skinned : -1; };
    hooks.pinned = [this](uint64_t) -> const thin::ThinStore::Entry* { return &ring_entry; };
    hooks.palette_count = [](uint32_t object, uint32_t part, uint32_t) -> uint32_t { return object == kRope && part < 2 ? kNpal[part] : 0; };
  }
  // Frame f: the record (hook stamp f) is delivered (plant record_late: one frame late), then the stream frame (guest frame f + 1) is observed and the mark set published.
  void Run(uint32_t f, StreamFrame& sf, std::shared_ptr<const rdr_scene::SceneTables>* late = nullptr) {
    BuildFrame(f, sf);
    auto rec = BuildRecord(f);
    keep.push_back(rec);
    if (fault == d3::Fault::RecordLate) {
      if (late && *late) core.records.Add(*late);   // the previous frame's record arrives now
      if (late) *late = rec;
    } else {
      core.records.Add(rec);
    }
    core.records.Evict(f);
    std::vector<d3::Run> runs;
    core.ScanFor(sf.frame, runs, nullptr, true);
    core.Observe(sf.frame, runs, sf.keys, sf.fulls, hooks);
    core.Publish(hooks, sf.frame.header.guest_frame);
  }
};

struct Result {
  uint64_t marked = 0, proof_mismatch = 0, record_missing = 0, ring_rows_missing = 0, proof_objects = 0, guard_trips = 0;
  bool all_marked = false, slot_map_ok = false, marker_equal = false, marker_hole = false, guard_holds = false, marked_before_flip = false;
  std::string detail, reasons;
};

Result Scenario(d3::Fault fault, uint32_t frames = 140) {
  Rig rig(fault);
  StreamFrame sf;
  std::shared_ptr<const rdr_scene::SceneTables> late;
  bool before_flip = false, at_flip_plus5 = false;
  for (uint32_t f = 0; f < frames; ++f) {
    rig.Run(f, sf, &late);
    const auto ms = rig.marks.Acquire();
    if (f == kFlipFrame - 1) before_flip = ms && ms->Has(d3::MarkKey(kRope, 0)) && ms->Has(d3::MarkKey(kRope, 2));
    if (f == kFlipFrame + 5) at_flip_plus5 = ms && (ms->Has(d3::MarkKey(kRope, 0)) || ms->Has(d3::MarkKey(kRope, 1)) || ms->Has(d3::MarkKey(kRope, 2)));
  }
  Result r;
  r.marked = rig.core.skippable, r.proof_mismatch = rig.core.c.proof_mismatch_parts, r.record_missing = rig.core.c.record_missing, r.proof_objects = rig.core.c.proof_objects;
  r.ring_rows_missing = rig.core.ring_rows_missing, r.guard_trips = rig.core.guard_trips;
  r.marked_before_flip = before_flip;
  r.guard_holds = !at_flip_plus5;
  for (const auto& kv : rig.core.reasons) r.reasons += kv.first + " (" + std::to_string(kv.second) + "); ";
  // 7 sequences (4 post ordinals, 3 rope ordinals) are marked once the flip's guard has run out (frame 100 + 64 = 164 > 140: the rope is guarded at the end of the run)
  // so the honest end state is: 4 posts marked, the 3 rope ordinals guarded; the all-marked check looks at the frames BEFORE the flip.
  r.all_marked = before_flip;
  {
    const auto it = rig.core.seqs.find(d3::MarkKey(kRope, 0));
    if (it != rig.core.seqs.end() && it->second.parts.size() == 2) {
      bool ok = true;
      for (uint32_t p = 0; p < 2; ++p) {
        const d3::CasterPartInfo& ci = it->second.parts[p].cpi;
        ok = ok && ci.learned && ci.npal == kNpal[p];
        for (uint32_t k = 0; k < kNpal[p]; ++k) ok = ok && ci.slot_node[k] == NodeOf(p, k);
      }
      r.slot_map_ok = ok;
    }
  }
  // a marker at frame F0 (the last frame): the packet as the hook would write it, the marker draw, the mini frame; its composed banks against the stream's truth over the variant's ranges minus the don't-care slots.
  // The rope is in the call-count guard's hold here, so the marker is built for the POST (and the rope when the guard is off): the bank equality is about the composition, not the guard.
  const uint32_t F0 = frames - 1;
  StreamFrame truth;
  BuildFrame(F0, truth);
  bool all_equal = true, any_hole = false;
  struct Probe { uint32_t object, ordinal, first_draw, nparts; };
  std::vector<Probe> probes;
  {
    uint32_t ix = 0;
    for (const Spec& s : FrameSpecs(F0)) {
      probes.push_back({s.object, s.call, ix, s.nparts});
      ix += s.nparts;
    }
  }
  for (const Probe& pr : probes) {
    if (pr.object == kRope && !rig.marks.Acquire()->Has(d3::MarkKey(kRope, pr.ordinal)) && fault == d3::Fault::None) continue;   // guarded in the honest run: nothing would skip it
    d3::Packet pk;
    pk.frame = F0, pk.object = pr.object, pk.ordinal = pr.ordinal, pk.call = pr.ordinal;
    rig.packets.Put(std::move(pk));
    std::vector<Op> ops{{OpKind::Draw, 0}};
    Draw m;
    m.flags = kDrawNoGeometry;
    m.vertex_offset = F0;
    m.index_offset = 0;
    DrawOrigin mo;
    mo.op = 15, mo.call_site = d3::kRingMarkerCall | (pr.ordinal & 0xFF), mo.object = pr.object, mo.pass_site = d3::kMarkerPass;
    std::vector<Draw> draws{m};
    std::vector<DrawOrigin> origins{mo};
    std::vector<ConstantBank> banks(2);
    LoadedFrame mf;
    mf.header.guest_frame = F0 + 1;
    mf.ops = {ops.data(), 1};
    mf.draws = {draws.data(), 1};
    mf.origins = {origins.data(), 1};
    mf.constant_banks = {banks.data(), 2};
    // the frame's pass bank from the stream's colour draws (as the binder does)
    d3::Bank pass_vs{}, pass_ps{};
    std::vector<uint8_t> hv, hp;
    d3::D3Core::PassBank(truth.frame, pass_vs, pass_ps, hv, hp);
    d3::D3Core::Mini mini;
    rig.core.BeginBind();
    const d3::D3Core::MarkerResult res = rig.core.BuildMini(mf, 0, &pass_vs, &pass_ps, hv, hp, mini);
    if (res != d3::D3Core::MarkerResult::Ok) {
      any_hole = true;
      all_equal = false;
      continue;
    }
    if (mini.draws.size() != pr.nparts) {
      all_equal = false;
      continue;
    }
    for (uint32_t k = 0; k < pr.nparts; ++k) {
      const uint32_t ti = pr.first_draw + k;
      const int vi = pr.object == kPost ? rig.v_rigid : rig.v_skinned;
      const rdr::fx::Variant& v = rdr::fx::kVariants[vi];
      const uint32_t npal = pr.object == kPost ? 1u : kNpal[k];
      for (uint32_t rr = 0; rr < v.constant_count; ++rr)
        for (uint32_t reg = v.constants[rr].reg; reg < uint32_t(v.constants[rr].reg) + v.constants[rr].count; ++reg) {
          if (reg >= 76 && reg <= 138 && (reg - 76) / 3 >= npal) continue;   // don't care
          if (std::memcmp(&mini.banks[2 * k].dwords[reg * 4], &truth.truth[ti].v[reg * 4], 16) != 0) {
            all_equal = false;
            if (r.detail.empty()) r.detail = "marker bank differs: object " + std::to_string(pr.object) + " call " + std::to_string(pr.ordinal) + " part " + std::to_string(k) + " VS c" + std::to_string(reg);
          }
        }
    }
  }
  r.marker_equal = all_equal, r.marker_hole = any_hole;
  return r;
}

// The two ComposePart plants and the don't-care rule at register level: a ring part whose variant reads c230-c237 / c185 with those registers in the drift set.
struct ComposeProbe { uint64_t unsourced_230 = 0, unsourced_185 = 0, mismatch_230 = 0, mismatch_185 = 0, mismatch_stale = 0, unsourced_stale = 0, reads = 0; };
ComposeProbe ComposeArm(d3::Fault fault) {
  ComposeProbe p;
  d3::PartDesc d;
  d.ring = true;
  d.vs_variant = Rig::Find("yProp_PSM_skinned_vertex");
  d.programmed = true;
  d.cpi.npal = 3;
  d.cpi.learned = true;
  for (uint32_t k = 0; k < 3; ++k) d.cpi.slot_node[k] = uint8_t(k + 1);
  d.drift_vs.set(230), d.drift_vs.set(185);
  // the template and the stream's truth: c230 and c185 differ from the template (they move), the stale slots hold junk
  d3::Bank tmpl{}, truth{};
  for (uint32_t i = 0; i < 1024; ++i) tmpl[i] = 0x3F800000u, truth[i] = tmpl[i];
  const float moved[4] = {5.f, 6.f, 7.f, 8.f};
  std::memcpy(&truth[230 * 4], moved, 16), std::memcpy(&truth[185 * 4], moved, 16);
  for (uint32_t reg = 76 + 9; reg <= 138; ++reg) truth[reg * 4] = 0x40400000u;   // slots past the palette: junk (never equal to what a record can give)
  std::memcpy(d.vs_bank.data(), tmpl.data(), sizeof tmpl);
  // a record with node rows for slots 0..2 equal to the truth's rows
  rdr_scene::RingRows e;
  e.n_nodes = 3, e.first_node = 0;
  std::vector<rdr_scene::RingNodeRows> pool(3);
  for (uint32_t k = 0; k < 3; ++k)
    for (uint32_t j = 0; j < 3; ++j) {
      std::memcpy(pool[k].rows + 4 * j, &truth[(76 + 3 * k + j) * 4], 16);
      std::memcpy(&truth[(76 + 3 * k + j) * 4], pool[k].rows + 4 * j, 16);
    }
  d3::RecordSrc rs;
  rs.ring = true, rs.ring_e = &e, rs.ring_pool = &pool, rs.have_light = false;
  d3::ComposeCtx cc;
  cc.mode = d3::Mode::Record, cc.fault = fault, cc.rec = &rs;
  d3::Bank pass{};
  std::vector<uint8_t> has(256, 0);
  cc.pass_vs = &pass, cc.pass_ps = &pass, cc.vs_has = &has, cc.ps_has = &has;
  d3::Bank out_vs, out_ps;
  d3::CompareStats st;
  d3::ComposePart(d, cc, out_vs, out_ps, &st, &truth, &truth);
  p.reads = st.reads;
  for (const auto& kv : st.unsourced) {
    const uint32_t reg = kv.first & 0xFFFF;
    const bool vs = ((kv.first >> 16) & 1) == 0;
    if (!vs) continue;
    if (reg == 230) p.unsourced_230 += kv.second;
    if (reg == 185) p.unsourced_185 += kv.second;
    if (reg >= 85 && reg <= 138) p.unsourced_stale += kv.second;
  }
  for (const auto& kv : st.by_reg) {
    const uint32_t reg = kv.first & 0xFFFF;
    if ((kv.first >> 16) != 0) continue;
    if (reg == 230) p.mismatch_230 += kv.second;
    if (reg == 185) p.mismatch_185 += kv.second;
    if (reg >= 85 && reg <= 138) p.mismatch_stale += kv.second;
  }
  return p;
}

// The chain is self-contained from construction (lane 217 review S1, for the ring): its publish, eviction, gate report and tally must not move the arena's live globals.
void Isolation(bool shared_globals) {
  using namespace rdr_adapter::d3;
  g_marks.Publish({42});
  { Packet p; p.frame = 90, p.object = 7, p.ordinal = 0; g_packets.Put(std::move(p)); }
  const uint64_t put0 = g_packets.stats().put, ev0 = g_packets.stats().evicted_unconsumed, rep0 = g_gate.reports.load(), res0 = g_gate.resets.load(), mk0 = g_tally.markers.load();
  RingChain ch;
  if (shared_globals) ch.d3r_.packets = &g_packets, ch.d3r_.marks = &g_marks, ch.d3r_.gate = &g_gate, ch.d3r_.hook_tally = &g_tally;
  const bool wired = ch.d3r_.ring() && ch.d3r_.packets == &ch.d3r_own_packets_ && ch.d3r_.marks == &ch.d3r_own_marks_ && ch.d3r_.gate == &ch.d3r_.local_gate && ch.d3r_.hook_tally == &ch.d3r_.tally;
  ch.d3r_.Publish(Hooks{}, 7);
  ch.d3r_.packets->Evict(100000);
  ch.d3r_.gate->Report(true, 5, 16, 5);
  ch.d3r_.hook_tally->Note(Why::Skip);
  const auto m = g_marks.Acquire();
  const bool untouched = m && m->keys.size() == 1 && m->keys[0] == 42 && g_packets.stats().put == put0 && g_packets.stats().evicted_unconsumed == ev0 && g_packets.Take(90, 7, 0) != nullptr &&
                         g_gate.reports.load() == rep0 && g_gate.resets.load() == res0 && g_tally.markers.load() == mk0;
  Check(wired && untouched, "ring.isolated_from_arena", "the ring chain owns its packets / marks / gate / tally from construction; the arena's g_marks, g_packets, g_gate and g_tally are untouched by its publish, eviction, gate report and tally note");
}

void Honest() {
  const Result r = Scenario(d3::Fault::None);
  Check(r.proof_objects > 0, "ring.proofs_ran", "proof objects " + std::to_string(r.proof_objects));
  Check(r.marked_before_flip, "ring.marks", "the 4 posts and the 3 rope ordinals are in the mark set at frame 99 (before the flip)");
  Check(r.proof_mismatch == 0 && r.record_missing == 0 && r.ring_rows_missing == 0, "ring.proof",
        "proof mismatching parts " + std::to_string(r.proof_mismatch) + ", record missing " + std::to_string(r.record_missing) + ", ring rows missing " + std::to_string(r.ring_rows_missing) + (r.reasons.empty() ? "" : "; " + r.reasons));
  Check(r.slot_map_ok, "ring.slot_map", "rope call 0: the slot -> node map (1-based) learned from the record equals the oracle for both parts");
  Check(r.marker_equal && !r.marker_hole, "ring.marker_bank_equal", r.detail);
  Check(r.guard_trips > 0 && r.guard_holds, "ring.guard", "call-count guard trips " + std::to_string(r.guard_trips) + "; the rope (3 -> 2 -> 3 calls at frame 100) is out of the mark set 5 frames after the flip");
  // the markers and call spaces
  Check(d3::IsRingMarkerCall(d3::kRingMarkerCall | 5) && !d3::IsMarkerCall(d3::kRingMarkerCall | 5) && !d3::IsCasterMarkerCall(d3::kRingMarkerCall | 5) && !d3::IsRingMarkerCall(d3::kMarkerCall | 5) &&
            !d3::IsRingMarkerCall(d3::CasterMarkerCall(1, 5)), "ring.marker_call_spaces");
  {   // lane 227 (review B1): the FOUR marker spaces (arena / ring / caster / wrestler): each predicate accepts its own and rejects every other's |5 value (incl. CasterMarkerCall(1, 5))
    const uint32_t v[4] = {d3::kMarkerCall | 5, d3::kRingMarkerCall | 5, d3::CasterMarkerCall(1, 5), d3::kWrMarkerCall | 5};
    bool (*const pred[4])(uint32_t) = {d3::IsMarkerCall, d3::IsRingMarkerCall, d3::IsCasterMarkerCall, d3::IsWrestlerMarkerCall};
    bool four = true;
    for (int a = 0; a < 4; ++a)
      for (int b = 0; b < 4; ++b) four = four && (pred[a](v[b]) == (a == b));
    four = four && !d3::IsWrestlerMarkerCall(d3::CasterMarkerCall(3, 255)) && !d3::IsMarkerCall(d3::kWrMarkerCall | 255);
    Check(four, "marker.four_call_spaces");
  }
  // the don't-care rule is the SAME as the hook side's (ring_rows_core.h)
  bool same = true;
  for (uint32_t npal = 0; npal <= 21; ++npal)
    for (uint32_t reg = 0; reg < 300; ++reg) same = same && d3::RingDontCare(npal, reg) == rdr_scene::live::ringrows::RowsRegisterDontCare(npal, reg);
  Check(same, "ring.dont_care_rule_same_as_hook_side", "d3_record.h RingDontCare == ring_rows_core.h RowsRegisterDontCare over npal 0-21 x c0-c299");
  // the node-rows helper agrees with the hook side's NodeRowsOf
  {
    rdr_scene::RingRows e;
    e.first_node = 2, e.n_nodes = 3;
    std::vector<rdr_scene::RingNodeRows> pool(6);
    bool ok = true;
    for (uint32_t n = 0; n <= 5; ++n) ok = ok && d3::NodeRowsOf1(pool, e, n) == rdr_scene::live::ringrows::NodeRowsOf(pool, e, n);
    Check(ok, "ring.node_rows_same_as_hook_side");
  }
  // register level: the honest arm never composes c230-c237 / c185 (they have no source: named unsourced), and a slot past the palette is don't care (not even counted)
  const ComposeProbe c = ComposeArm(d3::Fault::None);
  Check(c.unsourced_230 > 0 && c.unsourced_185 > 0 && c.mismatch_230 == 0 && c.mismatch_185 == 0 && c.mismatch_stale == 0 && c.unsourced_stale == 0, "ring.compose_registers",
        "c230 unsourced " + std::to_string(c.unsourced_230) + ", c185 unsourced " + std::to_string(c.unsourced_185) + ", stale slots unsourced " + std::to_string(c.unsourced_stale) + ", compared mismatches " + std::to_string(c.mismatch_stale));
  Isolation(false);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "";
  if (mode == "plant" && argc > 2 && std::string(argv[2]) == "shared_globals") {
    Isolation(true);
    std::printf("%s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
  }
  if (mode == "plant" && argc > 2) {
    d3::Fault f;
    if (!d3::ParseFault(argv[2], f)) {
      std::printf("unknown plant %s\n", argv[2]);
      return 2;
    }
    const Result r = Scenario(f);
    Check(r.marked_before_flip, "ring.marks", "marked before the flip: " + std::string(r.marked_before_flip ? "yes" : "no") + (r.reasons.empty() ? "" : "; " + r.reasons));
    Check(r.proof_mismatch == 0 && r.record_missing == 0 && r.ring_rows_missing == 0, "ring.proof",
          "proof mismatching parts " + std::to_string(r.proof_mismatch) + ", record missing " + std::to_string(r.record_missing) + ", ring rows missing " + std::to_string(r.ring_rows_missing));
    Check(r.marker_equal && !r.marker_hole, "ring.marker_bank_equal", r.detail);
    Check(r.guard_trips > 0 && r.guard_holds, "ring.guard", "call-count guard trips " + std::to_string(r.guard_trips) + ", holds " + std::string(r.guard_holds ? "yes" : "no"));
    const ComposeProbe c = ComposeArm(f);
    Check(c.unsourced_230 > 0 && c.unsourced_185 > 0 && c.mismatch_230 == 0 && c.mismatch_185 == 0 && c.mismatch_stale == 0 && c.unsourced_stale == 0, "ring.compose_registers",
          "c230 unsourced " + std::to_string(c.unsourced_230) + " mismatch " + std::to_string(c.mismatch_230) + ", c185 unsourced " + std::to_string(c.unsourced_185) + " mismatch " + std::to_string(c.mismatch_185) + ", stale unsourced " +
              std::to_string(c.unsourced_stale) + " mismatch " + std::to_string(c.mismatch_stale));
    return g_fail ? 1 : 0;
  }
  Honest();
  std::printf("%s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}
