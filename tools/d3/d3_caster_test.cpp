// Lane 211 (Caster D3 E2 binder part + E3 register binding, INERT; docs\renderer\SKIP_GAME_WORK_MAP.md 17.8): the SHIPPED binder core (src\renderer_adapter\d3_binder.h, d3_record.h, light_state_rows.h) driven with
// SYNTHETIC frames in the caster class's shape, 0 game runs. Build (tools\d3\d3_caster_selftest.py does it): clang++ -std=c++20 this + src\renderer_scene\record_io.cpp.
//
//   d3_caster_test.exe                      the honest arm: every check PASS (exit 0)
//   d3_caster_test.exe plant NAME           the same arm with a planted fault; it must FAIL the check named in the selftest (exit 1)
//   d3_caster_test.exe camsrc               the two answers of the [U] (17.2 item 2): Entry / ColourCall, both fixtures
//
// The fixture (kept small on purpose; the oracle is written here, NOT taken from the code under test): 3 objects. Two wrestlers (8 bones, 3 parts with 5 / 3 / 6 palette slots, slot k of part p = bone (p + 3k) % 8) draw
// map A (collapse flag 1), map B and the rim pass a frame; a props object draws its map (1 part, no palette) and its rim (2 parts). Every frame the cameras, the bones and the stale slots (past a part's list) change.
// The scene record of hook frame f is delivered with stream frame f + 1 (the +1 of lane 152). Expected: props map, wrestler map B -> marked; map A -> "MapACollapse" unsourced; rim -> "RimDepth" unsourced.
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../../src/renderer_adapter/d3_binder.h"

using namespace rdr_adapter;
namespace d3 = rdr_adapter::d3;

namespace {
int g_fail = 0;
void Check(bool ok, const char* name, const std::string& detail = "") {
  std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : "  ", detail.c_str());
  if (!ok) ++g_fail;
}

constexpr uint32_t kW1 = 0x40001000, kW2 = 0x40002000, kProps = 0x40003000;
constexpr uint32_t kBones = 8;
constexpr uint32_t kNpal[3] = {5, 3, 6};
inline uint32_t NodeOf(uint32_t part, uint32_t slot) { return (part + 3 * slot) % kBones; }   // the oracle's slot -> bone rule

uint32_t H(uint32_t a, uint32_t b, uint32_t c = 0, uint32_t d = 0) {
  uint32_t x = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA6Bu ^ (c + 0x165667B1u) * 0xC2B2AE35u ^ (d + 0x27D4EB2Fu) * 0x1B873593u;
  x ^= x >> 15, x *= 0x2C1B3C6Du, x ^= x >> 12, x *= 0x297A2D39u, x ^= x >> 15;
  return x;
}
float F(uint32_t h) { return float(int32_t(h % 20001) - 10000) / 64.0f; }   // a small deterministic float
uint32_t g_dup_from = ~0u, g_dup_to = 0;

rdr_scene::Mat4 BoneMat(uint32_t f, uint32_t obj, uint32_t node) {
  rdr_scene::Mat4 m;
  if (obj == 0 && node == 5 && (f < 20 || (f >= g_dup_from && f < g_dup_to))) node = 2;   // SF2 fixture: W1 bone 5 duplicates bone 2 in frames 0-19 (an ambiguous slot -> bone match); lane 301 S3: and in [g_dup_from, g_dup_to)
  for (int i = 0; i < 16; ++i) m.m[i] = F(H(f, obj, node, uint32_t(i)));
  return m;
}
rdr_scene::LightState CamState(uint32_t f, uint32_t which) {   // which: 1 props camera, 2 A / B camera, 3 the colour call's R
  rdr_scene::LightState s;
  std::memset(&s, 0, sizeof s);
  for (int m = 0; m < 3; ++m)
    for (int i = 0; i < 16; ++i) s.shadow_m[m][i] = F(H(f, 0xCA00 + which, uint32_t(m), uint32_t(i)));
  for (int i = 0; i < 15; ++i) s.shadow_f[i] = F(H(f, 0xCF00 + which, uint32_t(i)));
  return s;
}

struct Bank { std::array<uint32_t, 1024> v{}; };
void SetReg(Bank& b, uint32_t reg, const float* x) { std::memcpy(&b.v[reg * 4], x, 16); }
void SetRegBits(Bank& b, uint32_t reg, const uint32_t* x) { std::memcpy(&b.v[reg * 4], x, 16); }

// ---- the oracle: what the stream's banks hold for a caster part (written from the contract, independent of BuildCasterRows / CasterRecordRegister) ----
struct Cam { float vp[4][4], depth[4], trap[4][4], prm[3][4]; };
Cam OracleCam(const rdr_scene::LightState& s) {
  Cam c;
  for (int i = 0; i < 4; ++i)
    for (int k = 0; k < 4; ++k) c.vp[i][k] = s.shadow_m[0][k * 4 + i];   // c56 + i = column i of M(R+912)
  for (int k = 0; k < 4; ++k) c.depth[k] = c.vp[3][k];
  for (int i = 0; i < 3; ++i)
    for (int k = 0; k < 4; ++k) c.trap[i][k] = s.shadow_m[2][k * 4 + i];
  for (int k = 0; k < 4; ++k) c.trap[3][k] = s.shadow_m[1][k * 4 + 3];
  const float* f = s.shadow_f;   // f[i] = R float at byte 1104 + 4 i
  const float p[3][4] = {{f[1], f[2], f[0], f[4]}, {f[6], f[8], f[9], f[10]}, {f[11], f[12], f[13], f[14]}};
  std::memcpy(c.prm, p, sizeof p);
  return c;
}

struct Spec { uint32_t object, pass, ordinal, nparts; bool wrestler, collapse; };
// lane 299 (review SF2): the plane-guard scenario adds the ring plane proxy (kRingP, pass 2, kPlaneCalls calls of 1 part; kPlaneCalls - 1 at the flip frame). Off in the honest arm.
constexpr uint32_t kRingP = 0x40004000, kPlaneCalls = 4, kPlaneFlip = 100;
bool g_plane_on = false;
uint32_t g_plane_f = 0;
std::vector<Spec> FrameSpecs() {
  std::vector<Spec> v = {{kProps, 1, 0, 1, false, false},   {kW1, 1, 0, 3, true, true}, {kW1, 1, 1, 3, true, false}, {kW2, 1, 0, 3, true, true}, {kW2, 1, 1, 3, true, false},
          {kW1, 3, 0, 3, true, false},       {kW2, 3, 0, 3, true, false}, {kProps, 3, 0, 2, false, false}};
  if (g_plane_on)
    for (uint32_t c = 0; c < (g_plane_f == kPlaneFlip ? kPlaneCalls - 1 : kPlaneCalls); ++c) v.push_back({kRingP, 2, c, 1, false, false});
  return v;
}
uint32_t SiteOf(uint32_t pass) { return pass == 1 ? d3::kCasterSiteMap : pass == 2 ? d3::kCasterSitePlane : d3::kCasterSiteRim; }
uint32_t VsHash(const Spec& s) { return s.pass == 2 ? 0xC1 : s.pass == 3 ? (s.wrestler ? 0xB1 : 0xB0) : (s.wrestler ? 0xA1 : 0xA0); }
uint32_t PsHash(const Spec& s) { return s.pass == 1 ? 0xA2 : 0; }

struct Fixture {
  bool cam_is_colour = false;   // true: R at the entry IS the colour call's R (the other answer of 17.2 item 2)
};
rdr_scene::LightState StateFor(const Fixture& fx, uint32_t f, const Spec& s) {
  if (s.object == kProps && s.pass == 1) return CamState(f, fx.cam_is_colour ? 3 : 1);
  return CamState(f, fx.cam_is_colour ? 3 : 2);
}

// The stream's bank of draw `part` of run `s` at hook frame f.
void TruthBanks(const Fixture& fx, uint32_t f, const Spec& s, uint32_t part, uint32_t draw_ix, Bank& vs, Bank& ps) {
  vs = Bank{}, ps = Bank{};
  const Cam c = OracleCam(StateFor(fx, f, s));
  if (s.pass == 1) {
    for (int i = 0; i < 4; ++i) SetReg(vs, 56 + i, c.vp[i]);
    SetReg(vs, 54, c.depth);
    for (int i = 0; i < 4; ++i) SetReg(vs, 64 + i, c.trap[i]);
    SetReg(ps, 168, c.prm[0]);
  } else if (s.pass == 3) {
    for (int i = 0; i < 4; ++i) SetReg(vs, 56 + i, c.vp[i]);
    const float depth[4] = {F(H(f, 0x162, s.object)), F(H(f, 0x163, s.object)), 0.f, 0.f};   // RimLightDepth: no record carries it
    SetReg(vs, 162, depth);
  }
  if (s.wrestler) {
    const uint32_t npal = kNpal[part];
    const uint32_t obj_ix = s.object == kW1 ? 0 : 1;
    for (uint32_t slot = 0; slot < 21; ++slot)
      for (uint32_t j = 0; j < 3; ++j) {
        float row[4];
        if (slot < npal && !s.collapse) {
          const rdr_scene::Mat4 m = BoneMat(f, obj_ix, NodeOf(part, slot));
          for (int cc = 0; cc < 4; ++cc) row[cc] = m.m[cc * 4 + j];
        } else if (slot < npal) {   // map A: the collapsed rows (a translation point): not the bones
          for (int cc = 0; cc < 4; ++cc) row[cc] = F(H(f, 0xA, slot, cc));
        } else {                    // a slot past the part's list: the previous draw's rows (junk that changes every draw)
          for (int cc = 0; cc < 4; ++cc) row[cc] = F(H(f, 0x57A1E, draw_ix * 21 + slot, cc * 3 + j));
        }
        SetReg(vs, 76 + 3 * slot + j, row);
      }
  }
}

// One frame's scene record (hook frame f) and its stream frame (guest frame f + 1).
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
  std::vector<Bank> truth_vs, truth_ps;
  std::vector<Spec> run_of_draw;
  std::vector<uint32_t> part_of_draw;
};
void BuildFrame(const Fixture& fx, uint32_t f, StreamFrame& sf) {
  sf = StreamFrame{};
  sf.fixeds.resize(1), sf.dynamics.resize(1), sf.bools.resize(1);
  sf.strings.push_back('\0');
  uint32_t ix = 0;
  for (const Spec& s : FrameSpecs()) {
    for (uint32_t part = 0; part < s.nparts; ++part, ++ix) {
      Bank vs, ps;
      TruthBanks(fx, f, s, part, ix, vs, ps);
      ConstantBank cv, cp;
      std::memcpy(cv.dwords, vs.v.data(), sizeof cv.dwords);
      std::memcpy(cp.dwords, ps.v.data(), sizeof cp.dwords);
      Draw d;
      d.vs_hash = VsHash(s), d.ps_hash = PsHash(s);
      d.fixed = d.dynamic = d.bool_loop = 0;
      d.vs_bank = uint32_t(sf.banks.size());
      sf.banks.push_back(cv);
      d.ps_bank = uint32_t(sf.banks.size());
      sf.banks.push_back(cp);
      d.vertex_bytes = 1000 + part, d.vertex_count = 10 + part, d.index_count = 30 + part;   // the learned geometry facts (a caster part keeps them)
      DrawOrigin o;
      o.klass = DrawClass::ModelPart, o.op = 15, o.pass_site = SiteOf(s.pass), o.object = s.object, o.part = part;
      sf.ops.push_back({OpKind::Draw, uint32_t(sf.draws.size())});
      sf.draws.push_back(d);
      sf.origins.push_back(o);
      sf.truth_vs.push_back(vs), sf.truth_ps.push_back(ps);
      sf.run_of_draw.push_back(s);
      sf.part_of_draw.push_back(part);
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

std::shared_ptr<const rdr_scene::SceneTables> BuildRecord(const Fixture& fx, uint32_t f) {
  auto t = std::make_shared<rdr_scene::SceneTables>();
  t->header.guest_frame = uint64_t(f) + 1;
  for (uint32_t w = 0; w < 2; ++w) {
    rdr_scene::Object o;
    o.klass = DrawClass::ModelPart, o.flags = rdr_scene::kObjVisible | rdr_scene::kObjSkinned;
    o.bones_first = w * kBones, o.bones_count = kBones;
    t->objects.push_back(o);
    t->engine_objects.push_back(w == 0 ? kW1 : kW2);
    for (uint32_t n = 0; n < kBones; ++n) t->bones.push_back(BoneMat(f + 1, w, n));   // live (run 7138): the bones of a call stamped s are in the record with header s, i.e. record s-1: this record (stamp f) carries the bones of hook frame f + 1
  }
  t->caster_states.push_back(CamState(f, fx.cam_is_colour ? 3 : 1));
  t->caster_states.push_back(CamState(f, fx.cam_is_colour ? 3 : 2));
  t->light_states.push_back(CamState(f, 3));
  uint32_t ord_in_pass[0x100] = {};
  (void)ord_in_pass;
  for (const Spec& s : FrameSpecs()) {
    rdr_scene::CasterCall c;
    c.pass = uint8_t(s.pass), c.object = s.object, c.frame = f, c.ordinal = s.ordinal, c.collapse = s.collapse ? 1 : 0;
    c.state = (s.object == kProps && s.pass == 1) ? 0 : 1;
    c.n_nodes = s.wrestler ? uint16_t(kBones) : uint16_t(0);
    c.flags = s.wrestler ? 0u : uint32_t(rdr_scene::kCcNoPalette);
    t->caster_calls.push_back(c);
  }
  if (f > 0) {   // SF1 fixture: a catch-up record also carries the previous hook frame's calls (their bones are NOT this record's)
    for (const Spec& s : FrameSpecs()) {
      rdr_scene::CasterCall c;
      c.pass = uint8_t(s.pass), c.object = s.object, c.frame = f - 1, c.ordinal = s.ordinal, c.collapse = s.collapse ? 1 : 0;
      c.state = (s.object == kProps && s.pass == 1) ? 0 : 1;
      c.n_nodes = s.wrestler ? uint16_t(kBones) : uint16_t(0);
      c.flags = s.wrestler ? 0u : uint32_t(rdr_scene::kCcNoPalette);
      t->caster_calls.push_back(c);
    }
  }
  for (uint32_t o : {kW1, kW2, kProps}) {
    rdr_scene::LightRef r;
    r.object = o, r.ordinal = 0, r.state = 0, r.frame = f;
    t->light_refs.push_back(r);
  }
  return t;
}

struct Rig {
  d3::D3Core core;
  d3::PacketTable packets{4096};
  d3::MarkPublisher marks;
  d3::Gate gate;
  d3::Hooks hooks;
  std::vector<std::shared_ptr<const rdr_scene::SceneTables>> keep;
  int variant_vs_skinned = -1, variant_ps = -1, variant_rim_skinned = -1, variant_base = -1, variant_rim_base = -1;
  static int Find(const char* name) {
    for (size_t i = 0; i < sizeof(rdr::fx::kVariants) / sizeof(rdr::fx::kVariants[0]); ++i)
      if (std::strcmp(rdr::fx::kVariants[i].name, name) == 0) return int(i);
    return -1;
  }
  Rig(d3::Fault fault, d3::CamSource cs) {
    core.route = thin::Cls::Casters;
    core.mode = d3::Mode::Record;
    core.fault = fault;
    core.records.overwrite_unpaired = fault == d3::Fault::CatchupRecord;
    core.packets = &packets, core.marks = &marks, core.gate = &gate;
    core.records.cam_source = cs;
    variant_vs_skinned = Find("yShadowMap_skinned_vertex");
    variant_ps = Find("yShadowMap_base_fragment");
    variant_rim_skinned = Find("yRimLightMap_skinned_vertex");
    variant_base = Find("yShadowMap_base_vertex");
    variant_rim_base = Find("yRimLightMap_base_vertex");
    hooks.variant = [this](uint64_t h) -> int {
      switch (h) {
        case 0xA1: return variant_vs_skinned;
        case 0xA0: return variant_base;
        case 0xA2: return variant_ps;
        case 0xB1: return variant_rim_skinned;
        case 0xB0: return variant_rim_base;
        case 0xC1: return Find("yPlaneShadowMap_skinned_vertex");
        default: return -1;
      }
    };
    hooks.palette_count = [](uint32_t object, uint32_t part, uint32_t) -> uint32_t { return object == kRingP ? 1u : (object == kW1 || object == kW2) && part < 3 ? kNpal[part] : 0; };
  }
  // Frame f: the record (hook stamp f) is delivered, then the stream frame (guest frame f + 1) is observed and the mark set published.
  void Run(const Fixture& fx, uint32_t f, StreamFrame& sf) {
    g_plane_f = f;
    BuildFrame(fx, f, sf);
    auto rec = BuildRecord(fx, f);
    keep.push_back(rec);
    core.records.Add(rec);
    { auto early = BuildRecord(fx, f + 1); keep.push_back(early); core.records.Add(early); }   // SF1: the next record arrives early (a catch-up delivery): it carries this frame's calls again, unpaired
    core.records.Evict(f);
    std::vector<d3::Run> runs;
    core.ScanFor(sf.frame, runs, nullptr, true);
    std::vector<uint64_t> no_keys;
    std::vector<thin::ThinKey> no_fulls;
    core.Observe(sf.frame, runs, no_keys, no_fulls, hooks);
    core.Publish(hooks, sf.frame.header.guest_frame);
  }
  const d3::Sequence* Seq(uint32_t object, uint32_t pass, uint32_t ordinal) const {
    const auto it = core.seqs.find(d3::MarkKey(d3::CasterObjectId(object, pass), ordinal));
    return it == core.seqs.end() ? nullptr : &it->second;
  }
};

struct Result {
  uint64_t marked = 0, proof_mismatch = 0, record_missing = 0, proof_objects = 0;
  bool props_ok = false, w1b_ok = false, w2b_ok = false, a_unmarked = false, rim_unmarked = false;
  bool reason_a = false, reason_rim = false, slot_map_ok = false, marker_equal = false, marker_hole = false;
  std::string detail;
};

Result Scenario(d3::Fault fault, d3::CamSource cs, bool cam_is_colour, uint32_t frames = 100) {
  Fixture fx;
  fx.cam_is_colour = cam_is_colour;
  Rig rig(fault, cs);
  StreamFrame sf;
  for (uint32_t f = 0; f < frames; ++f) rig.Run(fx, f, sf);
  Result r;
  r.marked = rig.core.skippable, r.proof_mismatch = rig.core.c.proof_mismatch_parts, r.record_missing = rig.core.c.record_missing, r.proof_objects = rig.core.c.proof_objects;
  const auto complete = [&](uint32_t o, uint32_t p, uint32_t n) { const d3::Sequence* s = rig.Seq(o, p, n); return s && s->complete; };
  r.props_ok = complete(kProps, 1, 0), r.w1b_ok = complete(kW1, 1, 1), r.w2b_ok = complete(kW2, 1, 1);
  r.a_unmarked = !complete(kW1, 1, 0) && !complete(kW2, 1, 0);
  r.rim_unmarked = !complete(kW1, 3, 0) && !complete(kW2, 3, 0) && !complete(kProps, 3, 0);
  for (const auto& kv : rig.core.reasons) {
    if (kv.first.find("map A's collapsed rows") != std::string::npos) r.reason_a = true;
    if (kv.first.find("RimLightDepth") != std::string::npos) r.reason_rim = true;
  }
  // the slot map of W1's map-B part 1 against the oracle
  if (const d3::Sequence* s = rig.Seq(kW1, 1, 1); s && s->parts.size() == 3) {
    const d3::CasterPartInfo& ci = s->parts[1].cpi;
    bool ok = ci.learned && ci.npal == kNpal[1];
    for (uint32_t k = 0; k < kNpal[1]; ++k) ok = ok && ci.slot_node[k] == NodeOf(1, k);
    r.slot_map_ok = ok;
  }
  // a marker at the last frame: the packet (as the hook would write it), the marker draw, the mini frame; compared with the stream's truth over the variant's ranges minus the don't-care slots
  const uint32_t F0 = frames - 1;
  StreamFrame truth;
  BuildFrame(fx, F0, truth);   // the same frame again: the stream's banks the marker's composed banks must equal
  struct Probe { uint32_t object, pass, ordinal, run_first_draw, nparts; Spec spec; };
  std::vector<Probe> probes;
  {
    uint32_t ix = 0;
    for (const Spec& s : FrameSpecs()) {
      if ((s.object == kProps && s.pass == 1) || (s.wrestler && s.pass == 1 && s.ordinal == 1)) probes.push_back({s.object, s.pass, s.ordinal, ix, s.nparts, s});
      ix += s.nparts;
    }
  }
  bool all_equal = !probes.empty();
  bool any_hole = false;
  for (const Probe& pr : probes) {
    d3::Packet pk;
    pk.frame = F0, pk.object = d3::CasterObjectId(pr.object, pr.pass), pk.ordinal = pr.ordinal, pk.call = pr.ordinal;
    rig.packets.Put(std::move(pk));
    std::vector<Op> ops{{OpKind::Draw, 0}};
    Draw m;
    m.flags = kDrawNoGeometry;
    m.vertex_offset = F0;   // the hook's frame the marker carries
    m.index_offset = 0;
    DrawOrigin mo;
    mo.op = 15, mo.call_site = d3::CasterMarkerCall(pr.pass, pr.ordinal), mo.object = pr.object, mo.pass_site = d3::kMarkerPass;
    std::vector<Draw> draws{m};
    std::vector<DrawOrigin> origins{mo};
    std::vector<ConstantBank> banks(2);
    LoadedFrame mf;
    mf.header.guest_frame = F0 + 1;
    mf.ops = {ops.data(), 1};
    mf.draws = {draws.data(), 1};
    mf.origins = {origins.data(), 1};
    mf.constant_banks = {banks.data(), 2};
    d3::Bank pass_vs{}, pass_ps{};
    std::vector<uint8_t> hv(256, 0), hp(256, 0);
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
      const uint32_t ti = pr.run_first_draw + k;
      const Spec& s = truth.run_of_draw[ti];
      const int vi = rig.hooks.variant(VsHash(s));
      const rdr::fx::Variant& v = rdr::fx::kVariants[vi];
      for (uint32_t rr = 0; rr < v.constant_count; ++rr)
        for (uint32_t reg = v.constants[rr].reg; reg < uint32_t(v.constants[rr].reg) + v.constants[rr].count; ++reg) {
          if (s.wrestler && reg >= 76 && reg <= 138 && (reg - 76) / 3 >= kNpal[k]) continue;   // don't care
          if (std::memcmp(&mini.banks[2 * k].dwords[reg * 4], &truth.truth_vs[ti].v[reg * 4], 16) != 0) {
            all_equal = false;
            if (r.detail.empty()) r.detail = "marker bank differs: object " + std::to_string(pr.object) + " part " + std::to_string(k) + " VS c" + std::to_string(reg);
          }
        }
      if (PsHash(s)) {
        if (std::memcmp(&mini.banks[2 * k + 1].dwords[168 * 4], &truth.truth_ps[ti].v[168 * 4], 16) != 0) {
          all_equal = false;
          if (r.detail.empty()) r.detail = "marker PS c168 differs";
        }
      }
    }
  }
  r.marker_equal = all_equal, r.marker_hole = any_hole;
  return r;
}

// lane 299 (review SF2): the caster route's call-count guard for the plane pass. The ring plane proxy is a many-call object whose EyePos c72 is held per ordinal (4 light points, one per side): when the number of its keyed calls
// changes (a rope instance left), a learned ordinal stands for another instance, so the object leaves the mark set for 2 x K frames. Plant caster_plane_no_guard: no guard on the caster route (the old behaviour).
void PlaneGuard(d3::Fault fault) {
  g_plane_on = true;
  Fixture fx;
  Rig rig(fault, d3::CamSource::Entry);
  rig.core.plane_marks_allowed = true, rig.core.force_mark = true;
  StreamFrame sf;
  const auto marked = [&](uint32_t c) { const auto ms = rig.marks.Acquire(); return ms && ms->Has(d3::MarkKey(d3::CasterObjectId(kRingP, 2), c)); };
  bool before = false, after = true;
  for (uint32_t f = 0; f < 130; ++f) {
    rig.Run(fx, f, sf);
    if (f == kPlaneFlip - 1) before = marked(0) && marked(1) && marked(2) && marked(3);
    if (f == kPlaneFlip + 5) after = marked(0) || marked(1) || marked(2) || marked(3);
  }
  g_plane_on = false;
  Check(before, "caster.plane_marked_before_flip", "the ring plane proxy's 4 ordinals are in the mark set before the flip");
  Check(!after, "caster.plane_guard", std::string("the proxy loses a call at frame 100: its ordinals are out of the mark set 5 frames later") + (after ? " (STILL MARKED: no guard)" : ""));
}

// lane 217 (review S1): the caster chain is self-contained from construction. It is exercised (publish, evict, gate report, tally) and the arena's live globals must not move.
// Plant shared_globals = the chain wired to g_packets / g_marks / g_gate / g_tally (what the core was before the review): the check fails.
void Isolation(bool shared_globals) {
  using namespace rdr_adapter::d3;
  g_marks.Publish({42});
  { Packet p; p.frame = 90, p.object = 7, p.ordinal = 0; g_packets.Put(std::move(p)); }
  const uint64_t put0 = g_packets.stats().put, ev0 = g_packets.stats().evicted_unconsumed, rep0 = g_gate.reports.load(), res0 = g_gate.resets.load(), mk0 = g_tally.markers.load();
  CasterChain ch;
  if (shared_globals) ch.d3c_.packets = &g_packets, ch.d3c_.marks = &g_marks, ch.d3c_.gate = &g_gate, ch.d3c_.hook_tally = &g_tally;
  const bool wired = ch.d3c_.caster() && ch.d3c_.packets == &ch.d3c_own_packets_ && ch.d3c_.marks == &ch.d3c_own_marks_ && ch.d3c_.gate == &ch.d3c_.local_gate && ch.d3c_.hook_tally == &ch.d3c_.tally;
  ch.d3c_.Publish(Hooks{}, 7);                 // an empty mark set
  ch.d3c_.packets->Evict(100000);              // the eviction of a frame's packets
  ch.d3c_.gate->Report(true, 5, 16, 5);        // a bad frame: resets the gate
  ch.d3c_.hook_tally->Note(Why::Skip);
  const auto m = g_marks.Acquire();
  const bool untouched = m && m->keys.size() == 1 && m->keys[0] == 42 && g_packets.stats().put == put0 && g_packets.stats().evicted_unconsumed == ev0 && g_packets.Take(90, 7, 0) != nullptr &&
                         g_gate.reports.load() == rep0 && g_gate.resets.load() == res0 && g_tally.markers.load() == mk0;
  Check(wired && untouched, "caster.isolated_from_arena", "the caster chain owns its packets / marks / gate / tally from construction; after a publish, an eviction, a gate report and a tally note the arena's g_marks (keys {42}), g_packets (its packet still takeable, evicted_unconsumed " +
                                                          std::to_string(g_packets.stats().evicted_unconsumed) + "), g_gate (reports " + std::to_string(g_gate.reports.load()) + ") and g_tally are untouched");
}

// lane 301 review S3: in a PROOF frame LearnCasterSlots re-learns every slot into `fresh`; two bones with bit-identical rows (W1 bone 5 = bone 2 in frames [70, 110)) make the match ambiguous (kNoNode) while the proof,
// composed on the OLD map, still passes. Before: the sequence stayed complete and `fresh` (a slot lost) was installed -> every marker would hole on NoSrc::PaletteSlot until the next proof. After: the certified
// map is frozen, the object leaves the mark set (counted `drift after mark`), no marked sequence ever carries a slot the certified one resolved. Plant freeze_off = the code before lane 301 (core.freeze_on = false).
void SlotsFrozen(bool freeze_on) {
  g_dup_from = 70, g_dup_to = 110;
  Fixture fx;
  Rig rig(d3::Fault::None, d3::CamSource::Entry);
  rig.core.freeze_on = freeze_on;
  StreamFrame sf;
  uint8_t snap[3][d3::kCasterMaxSlots];
  bool have_snap = false, lost_while_marked = false;
  for (uint32_t f = 0; f < 150; ++f) {
    rig.Run(fx, f, sf);
    const d3::Sequence* s = rig.Seq(kW1, 1, 1);
    if (!s || s->parts.size() != 3) continue;
    if (f == 69) {
      for (size_t k = 0; k < 3; ++k)
        for (uint32_t sl = 0; sl < d3::kCasterMaxSlots; ++sl) snap[k][sl] = s->parts[k].cpi.slot_node[sl];
      have_snap = s->complete;
    }
    if (f >= 70 && f < 150 && s->complete && have_snap)
      for (size_t k = 0; k < 3; ++k)
        for (uint32_t sl = 0; sl < d3::kCasterMaxSlots; ++sl)
          if (snap[k][sl] != d3::kNoNode && s->parts[k].cpi.slot_node[sl] == d3::kNoNode) lost_while_marked = true;
  }
  g_dup_from = ~0u, g_dup_to = 0;
  Check(have_snap && !lost_while_marked && rig.core.c.drift_after_mark > 0, "caster.slots_frozen",
        std::string("two bones with equal rows in a proof frame: the certified slot map is frozen, the object leaves the mark set (drift after mark ") + std::to_string(rig.core.c.drift_after_mark) + "), a marked sequence never lost a resolved slot" + (lost_while_marked ? " (LOST: markers would hole on PaletteSlot)" : "") + (have_snap ? "" : " (NO snapshot: the object was not marked at frame 69)"));
}

void Honest() {
  const Result r = Scenario(d3::Fault::None, d3::CamSource::Entry, false);
  Check(r.proof_objects > 0, "caster.proofs_ran", "proof objects " + std::to_string(r.proof_objects));
  Check(r.marked == 3 && r.props_ok && r.w1b_ok && r.w2b_ok, "caster.marks", "marked " + std::to_string(r.marked) + " (props map, W1 map B, W2 map B expected)");
  Check(r.proof_mismatch == 0 && r.record_missing == 0, "caster.proof", "proof mismatching parts " + std::to_string(r.proof_mismatch) + ", record missing " + std::to_string(r.record_missing));
  Check(r.a_unmarked && r.reason_a, "caster.map_a_unmarked_named", "map A is never marked (MapACollapse, E4)");
  Check(r.rim_unmarked && r.reason_rim, "caster.rim_unmarked_named", "the rim is never marked (RimLightDepth has no source)");
  Check(r.slot_map_ok, "caster.slot_map", "W1 map B part 1: slot -> bone learned from the record equals the oracle");
  Check(r.marker_equal && !r.marker_hole, "caster.marker_bank_equal", r.detail);
  // the claim decision shared with the arena (lane 204): a marker is claimed when the replay delivers 2 indices; the V4 plant (0) never claims
  Check(d3::IsMarkerReplay(d3::kMarkerPrim, d3::ReplayIndexCount(d3::kMarkerPrimMul, 0, d3::kMarkerPrimAdd), false) && !d3::IsMarkerReplay(d3::kMarkerPrim, 2, false, d3::Fault::MarkerCount0), "marker.claim_replayed");
  Check(d3::IsCasterMarkerCall(d3::CasterMarkerCall(3, 7)) && !d3::IsMarkerCall(d3::CasterMarkerCall(3, 7)) && !d3::IsCasterMarkerCall(d3::kMarkerCall | 5) && d3::CasterPassOfMarker(d3::CasterMarkerCall(3, 7)) == 3, "marker.call_spaces");
  // BuildCasterRows against the oracle on a few states (pure rules)
  bool rows_ok = true;
  for (uint32_t f = 0; f < 20; ++f) {
    const rdr_scene::LightState s = CamState(f, 2);
    const rdr_scene::CasterRows cr = rdr_scene::BuildCasterRows(s);
    const Cam o = OracleCam(s);
    rows_ok = rows_ok && std::memcmp(cr.view_proj, o.vp, sizeof o.vp) == 0 && std::memcmp(cr.view_depth, o.depth, 16) == 0 && std::memcmp(cr.trapezoid, o.trap, sizeof o.trap) == 0 &&
              std::memcmp(cr.shadow_prm, o.prm, sizeof o.prm) == 0 && std::memcmp(cr.view_depth, cr.view_proj[3], 16) == 0 && !cr.rim_light_depth_valid;
  }
  Check(rows_ok, "rows.build_caster_rows", "ViewProj = columns of M(R+912), ViewDepth = c59, Trapezoidal = M(R+1040) cols 0-2 + M(R+976) col 3, ShadowPrm = the light rows; RimLightDepth has no source");
}

void CamSrc() {
  // the two answers of 17.2 item 2, both fixtures: the stream's camera is the entry's state (the entry carries the A / B camera, != the colour call's R) or the entry state IS the colour call's R.
  const Result a = Scenario(d3::Fault::None, d3::CamSource::Entry, false);
  const Result b = Scenario(d3::Fault::None, d3::CamSource::ColourCall, false);
  const Result c = Scenario(d3::Fault::None, d3::CamSource::ColourCall, true);
  const Result d = Scenario(d3::Fault::None, d3::CamSource::Entry, true);
  Check(a.marked == 3 && a.proof_mismatch == 0, "camsrc.entry_when_entry_carries_AB", "marked " + std::to_string(a.marked));
  Check(b.proof_mismatch > 0 || b.marked < 3, "camsrc.colour_call_fails_when_entry_carries_AB", "the wrong answer is caught: mismatching " + std::to_string(b.proof_mismatch) + ", marked " + std::to_string(b.marked));
  Check(c.marked == 3 && c.proof_mismatch == 0, "camsrc.colour_call_when_entry_is_colour_R", "marked " + std::to_string(c.marked));
  Check(d.marked == 3 && d.proof_mismatch == 0, "camsrc.entry_also_fine_when_equal", "marked " + std::to_string(d.marked));
}

}  // namespace

// ---- `cam VECTORS [PLANT]`: the shipped BuildCasterRows against the numbers of a kept capture (tools\caster\caster_cam_vectors.py) -------------------------------------------------------------------
// Prints, per pass, how many of its draws carry each camera register equal (bit for bit) to the row built from SOME colour-call state of the same frame, and checks the bars (SKIP_GAME_WORK_MAP.md 17.8).
#include <map>
#include <sstream>
namespace {
int CamMode(const char* path, const char* plant) {
  std::FILE* fh = std::fopen(path, "rb");
  if (!fh) {
    std::printf("FAIL cam.input_missing  %s\n", path);
    return 1;
  }
  struct DrawRow { uint32_t frame, site; uint32_t w[52]; };
  std::map<uint32_t, std::vector<rdr_scene::LightState>> states;
  std::vector<DrawRow> draws;
  static char line[4096];
  while (std::fgets(line, sizeof line, fh)) {
    std::istringstream is(line);
    std::string tag;
    is >> tag;
    if (tag == "STATE") {
      uint32_t f, k;
      is >> std::dec >> f >> k;
      rdr_scene::LightState s;
      std::memset(&s, 0, sizeof s);
      uint32_t w[64];
      for (int i = 0; i < 64; ++i) is >> std::hex >> w[i];
      std::memcpy(s.shadow_m, w, 48 * 4);
      std::memcpy(s.shadow_f, w + 48, 16 * 4);
      states[f].push_back(s);
    } else if (tag == "DRAW") {
      DrawRow d{};
      std::string site;
      is >> std::dec >> d.frame >> site;
      d.site = uint32_t(std::strtoul(site.c_str(), nullptr, 16));
      for (int i = 0; i < 52; ++i) is >> std::hex >> d.w[i];
      draws.push_back(d);
    }
  }
  std::fclose(fh);
  const bool wrong = plant && std::string(plant) == "camera_viewproj_wrong";
  struct Acc { uint64_t draws = 0, c54 = 0, vp[4] = {}, tr[4] = {}, prm[3] = {}; };
  std::map<uint32_t, Acc> acc;
  std::map<uint32_t, std::vector<rdr_scene::CasterRows>> rows;
  for (auto& kv : states)
    for (rdr_scene::LightState s : kv.second) {
      if (wrong) std::memcpy(s.shadow_m[0], s.shadow_m[1], sizeof s.shadow_m[0]);
      rows[kv.first].push_back(rdr_scene::BuildCasterRows(s));
    }
  for (const DrawRow& d : draws) {
    Acc& a = acc[d.site];
    ++a.draws;
    const auto& rs = rows[d.frame];
    const auto eq = [&](const float* r, int w0) { return std::memcmp(r, d.w + w0, 16) == 0; };
    bool b54 = false, bvp[4] = {}, btr[4] = {}, bprm[3] = {};
    for (const rdr_scene::CasterRows& r : rs) {
      b54 = b54 || eq(r.view_depth, 0);
      for (int i = 0; i < 4; ++i) bvp[i] = bvp[i] || eq(r.view_proj[i], 4 + 4 * i), btr[i] = btr[i] || eq(r.trapezoid[i], 20 + 4 * i);
      for (int i = 0; i < 3; ++i) bprm[i] = bprm[i] || eq(r.shadow_prm[i], 40 + 4 * i);
    }
    a.c54 += b54;
    for (int i = 0; i < 4; ++i) a.vp[i] += bvp[i], a.tr[i] += btr[i];
    for (int i = 0; i < 3; ++i) a.prm[i] += bprm[i];
  }
  const auto name = [](uint32_t site) { return site == d3::kCasterSiteMap ? "map" : site == d3::kCasterSitePlane ? "plane" : "rim"; };
  for (const auto& kv : acc) {
    const Acc& a = kv.second;
    std::printf("cam %-5s draws %llu: c54 %llu, c56-59 %llu/%llu/%llu/%llu, c64-67 %llu/%llu/%llu/%llu, PS c168-170 %llu/%llu/%llu (RimLightDepth c162 has no source)\n", name(kv.first), (unsigned long long)a.draws,
                (unsigned long long)a.c54, (unsigned long long)a.vp[0], (unsigned long long)a.vp[1], (unsigned long long)a.vp[2], (unsigned long long)a.vp[3], (unsigned long long)a.tr[0], (unsigned long long)a.tr[1],
                (unsigned long long)a.tr[2], (unsigned long long)a.tr[3], (unsigned long long)a.prm[0], (unsigned long long)a.prm[1], (unsigned long long)a.prm[2]);
  }
  const Acc& plane = acc[d3::kCasterSitePlane];
  const Acc& map = acc[d3::kCasterSiteMap];
  // bars: the plane mask's four Trapezoidal rows are rows of the colour-call states on every draw (the layout rule: M(R+1040) columns 0-2, M(R+976) column 3) and the props map's ViewProj / ViewDepth are
  // M(R+912)'s columns (some map draws: the props map's; the wrestlers' A / B maps are no colour-call state, [U]).
  Check(plane.draws > 0 && plane.tr[0] == plane.draws && plane.tr[1] == plane.draws && plane.tr[2] == plane.draws && plane.tr[3] == plane.draws, "cam.plane_trapezoid_all_draws",
        "plane draws " + std::to_string(plane.draws) + ", c64-67 matched " + std::to_string(plane.tr[0]) + "/" + std::to_string(plane.tr[1]) + "/" + std::to_string(plane.tr[2]) + "/" + std::to_string(plane.tr[3]));
  Check(map.draws > 0 && map.tr[2] == map.draws && map.tr[3] == map.draws, "cam.map_trapezoid_c66_c67_all_draws", "map draws " + std::to_string(map.draws) + ", c66 " + std::to_string(map.tr[2]) + ", c67 " + std::to_string(map.tr[3]));
  Check(map.vp[0] > 0 && map.vp[0] == map.vp[1] && map.vp[1] == map.vp[2] && map.vp[2] == map.vp[3] && map.vp[3] == map.c54, "cam.props_map_viewproj_and_depth",
        "map draws with ViewProj = M(R+912) columns: " + std::to_string(map.vp[0]) + ", c54 = c59: " + std::to_string(map.c54));
  std::printf("%s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}
}  // namespace

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "";
  if (mode == "cam" && argc > 2) return CamMode(argv[2], argc > 3 ? argv[3] : nullptr);
  if (mode == "plant" && argc > 2 && std::string(argv[2]) == "shared_globals") {
    Isolation(true);
    std::printf("%s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
  }
  if (mode == "plant" && argc > 2 && std::string(argv[2]) == "freeze_off") {   // lane 301 S3: the code before the freeze
    SlotsFrozen(false);
    std::printf("%s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
  }
  if (mode == "plant" && argc > 2 && std::string(argv[2]) == "caster_plane_no_guard") {
    PlaneGuard(d3::Fault::CasterPlaneNoGuard);
    std::printf("%s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
  }
  if (mode == "plant" && argc > 2) {
    d3::Fault f;
    if (!d3::ParseFault(argv[2], f)) {
      std::printf("unknown plant %s\n", argv[2]);
      return 2;
    }
    const Result r = Scenario(f, d3::CamSource::Entry, false);
    // the same checks as the honest arm: a plant must fail the one(s) named in the selftest
    Check(r.marked == 3 && r.props_ok && r.w1b_ok && r.w2b_ok, "caster.marks", "marked " + std::to_string(r.marked));
    Check(r.proof_mismatch == 0 && r.record_missing == 0, "caster.proof", "proof mismatching parts " + std::to_string(r.proof_mismatch) + ", record missing " + std::to_string(r.record_missing));
    Check(r.marker_equal && !r.marker_hole, "caster.marker_bank_equal", r.detail);
    Check(d3::IsMarkerReplay(d3::kMarkerPrim, d3::ReplayIndexCount(d3::kMarkerPrimMul, 0, d3::kMarkerPrimAdd), false, f), "marker.claim_replayed");   // lane 204's past bug (the claim at count 0)
    return g_fail ? 1 : 0;
  }
  if (mode == "camsrc") {
    CamSrc();
  } else {
    Honest();
    CamSrc();
    Isolation(false);
    PlaneGuard(d3::Fault::None);
    SlotsFrozen(true);   // lane 301 review S3
  }
  std::printf("%s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}
