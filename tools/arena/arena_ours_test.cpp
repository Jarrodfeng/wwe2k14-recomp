// arena_ours_test: the unit test of our arena (Session 101, lane 40; docs\renderer\ARENA_PROMOTION_MAP.md sections 2-5). 0 game runs, no GPU.
//   arena_ours_test --game DIR --files [--arena N ...]                      the files-only checks (the stage meshes of arenas 4 and 5 by default)
//   arena_ours_test --game DIR --capture DIR [--frames A-B] [--step N] [--fault NAME] [--probe]
// The capture mode runs the SHIPPED join and per-draw checks (src\renderer_scene\arena_ours_core.cpp, the code ArenaOurs calls) on the stage draws of a capture, and
// checks them against an INDEPENDENT oracle (a brute-force position match against every mesh of the arena, ignoring the part and the counts):
//   - every stage draw the oracle names is replaced (after the first frame of its object), none is replaced wrongly (the joined mesh's positions == the stream's)
//   - per replaced draw: vertex count, index count (group rule), triangles == the stream's (strip -> list, canonical rotation, so the winding), uv within half
//     precision, normals within the packed format, the colour bytes in the byte order our vertex is built with
//   - the placed slots the record does not carry (the listed gaps)
// --fault NAME plants one of the map's faults inside the same code path (vertex_swizzle, no_mirror, first_texture, viewer_space, file_lightmap, join_shift,
// no_group_rule, winding_flip); the run then must FAIL, and tools\arena\selftest.py asserts which check fails.
// Exit: 0 all hold, 1 a check failed, 2 a loud failure (cannot read the input), 3 bad usage. Lines: "PASS name (...)" / "FAIL name (...)", then "RESULT ...".
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../../src/assets/arena_game_space.h"
#include "../../src/assets/crowd_body.h"
#include "../../src/renderer/shaders/spv/fx_registry.h"
#include "../../src/renderer_adapter/capture_reader.h"
#include "../../src/renderer_adapter/image_info_table.h"  // S101 lane 47
#include "../../src/renderer_scene/arena_ours_core.h"
#include "../../src/renderer_scene/proof_frames.h"
#include "../../src/renderer_scene/record_io.h"

namespace {
using namespace rdr_scene;
using rdr_adapter::CaptureReader;
using rdr_adapter::LoadedFrame;
using assets::arena_gs::StageArena;
using assets::arena_gs::StageMesh;

int g_fail = 0;
std::vector<std::string> g_failed;
void Check(const char* name, bool ok, const char* fmt = "", ...) {
  char detail[512] = {};
  if (fmt && *fmt) {
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
  }
  std::printf("%s  %s%s%s%s\n", ok ? "PASS" : "FAIL", name, detail[0] ? "  (" : "", detail, detail[0] ? ")" : "");
  if (!ok) ++g_fail, g_failed.push_back(name);
}

bool NeedsSkinVariant(int vs) { return ArenaCore::NeedsSkin(vs); }

int FindVariant(uint64_t hash) {
  const auto* first = std::begin(rdr::fx::kHashes);
  const auto* last = std::end(rdr::fx::kHashes);
  const auto* it = std::lower_bound(first, last, hash, [](const rdr::fx::HashEntry& e, uint64_t v) { return e.hash < v; });
  return it != last && it->hash == hash ? int(it->variant) : -1;
}

float HalfToFloat(uint16_t h) {
  const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
  float f;
  if (e == 0) f = std::ldexp(float(m), -24);
  else if (e == 31) f = m ? NAN : INFINITY;
  else f = std::ldexp(float(m | 1024), int(e) - 25);
  return s ? -f : f;
}


// The stream's index strip -> a triangle list: the Xbox strip rule (every other triangle flipped), 0xFFFF a restart, degenerate triangles dropped. Written here, not
// taken from the loader (src\assets\jboy.h), so the check does not compare the loader with itself.
void StripToList(const std::vector<uint16_t>& idx, uint32_t vcount, std::vector<uint32_t>& out) {
  size_t run = 0;
  for (size_t i = 0; i < idx.size(); ++i) {
    if (idx[i] == 0xFFFF) { run = 0; continue; }
    if (++run < 3) continue;
    uint32_t a = idx[i - 2], b = idx[i - 1], c = idx[i];
    if (a >= vcount || b >= vcount || c >= vcount || a == b || b == c || a == c) continue;
    if ((run & 1) == 0) std::swap(a, b);
    out.push_back(a), out.push_back(b), out.push_back(c);
  }
}

// triangle -> canonical rotation (smallest index first), keeps the winding
std::array<uint32_t, 3> Canon(uint32_t a, uint32_t b, uint32_t c) {
  if (a <= b && a <= c) return {a, b, c};
  if (b <= a && b <= c) return {b, c, a};
  return {c, a, b};
}
std::multiset<std::array<uint32_t, 3>> TriSet(const std::vector<uint32_t>& list) {
  std::multiset<std::array<uint32_t, 3>> s;
  for (size_t i = 0; i + 2 < list.size(); i += 3) s.insert(Canon(list[i], list[i + 1], list[i + 2]));
  return s;
}

struct Options {
  std::string game, capture, fault, supplier_fault;   // supplier_fault (S101 lane 47): shrink | drop | shift, planted into the live supplier's table
  bool files = false, probe = false, threads = false, no_image_info = false, live_style = false, corrupt_one = false;
  int first = 0, last = -1, step = 1;
  std::vector<int> arenas;
};

ArenaFaults FaultsOf(const std::string& n, bool& known) {
  ArenaFaults f;
  known = true;
  if (n.empty()) return f;
  if (n == "vertex_swizzle") f.vertex_swizzle = true;
  else if (n == "no_mirror") f.no_mirror = true;
  else if (n == "first_texture") f.first_texture = true;
  else if (n == "viewer_space") f.viewer_space = true;
  else if (n == "file_lightmap") f.file_lightmap = true;
  else if (n == "join_shift") f.join_shift = true;
  else if (n == "no_group_rule") f.no_group_rule = true;
  else if (n == "winding_flip") f.winding_flip = true;
  else if (n == "bone_off_by_one") f.bone_off_by_one = true;
  else if (n == "bone_zero") f.bone_zero = true;
  else if (n == "no_trim_window") f.no_trim_window = true;   // lane 243
  else if (n == "window_shift") f.window_shift = true;       // lane 243
  else if (n == "no_proof_frames") f.no_proof_frames = true;   // S101 lane 88: only the corrupt-one run reads it
  else if (n == "strict_texture") f.strict_texture = true;   // lane 142
  else if (n == "uv_shift") f.uv_shift = true;   // lane 142
  else known = false;
  return f;
}

// ---- files-only ---------------------------------------------------------------------------------------------------------------------
int FilesChecks(const Options& opt) {
  std::vector<int> arenas = opt.arenas.empty() ? std::vector<int>{4, 5} : opt.arenas;
  bool known = true;
  const ArenaFaults faults = FaultsOf(opt.fault, known);
  for (int a : arenas) {
    StageArena st;
    std::string err;
    char tag[64];
    std::snprintf(tag, sizeof tag, "files.arena%d", a);
    if (!assets::arena_gs::Load(a, st, err, faults.viewer_space)) {
      std::printf("FAIL  %s.load  (%s)\n", tag, err.c_str());
      return 2;
    }
    size_t tris = 0, bad_index = 0, nonfinite = 0, multi_group = 0, no_part = 0, tex_missing = 0;
    std::set<std::pair<int, int>> keys;
    size_t dup_keys = 0;
    float ymin = 1e30f, ymax = -1e30f;
    for (const StageMesh& m : st.meshes) {
      tris += m.indices.size() / 3;
      if (m.indices.size() % 3) ++bad_index;
      for (uint32_t ix : m.indices) bad_index += ix >= m.vertices.size();
      for (const auto& v : m.vertices) {
        nonfinite += !std::isfinite(v.pos[0] + v.pos[1] + v.pos[2] + v.normal[0] + v.normal[1] + v.normal[2] + v.uv[0] + v.uv[1]);
        ymin = std::min(ymin, v.pos[1]), ymax = std::max(ymax, v.pos[1]);
      }
      multi_group += m.index_groups > 1;
      no_part += m.part < 0;
      dup_keys += !keys.emplace(m.slot, m.part).second;
      tex_missing += m.diffuse < 0;
      // the group rule is a definition here: strip + (groups - 1)
      if (m.index_groups > 1 && m.StreamIndexCount(true) != m.strip_indices + m.index_groups - 1) ++bad_index;
    }
    char n[96];
    std::snprintf(n, sizeof n, "%s.loaded", tag);
    Check(n, !st.meshes.empty() && !st.textures.empty(), "%zu stage meshes, %zu textures, %.1f MB", st.meshes.size(), st.textures.size(), double(st.Bytes()) / 1048576.0);
    std::snprintf(n, sizeof n, "%s.meshes_sane", tag);
    Check(n, bad_index == 0 && nonfinite == 0 && no_part == 0 && dup_keys == 0, "%zu triangles; bad index %zu, non-finite %zu, no part %zu, duplicate (slot, part) %zu", tris, bad_index, nonfinite, no_part,
          dup_keys);
    std::snprintf(n, sizeof n, "%s.group_rule_present", tag);
    Check(n, multi_group > 0, "%zu meshes stored as 2+ index groups", multi_group);
    std::snprintf(n, sizeof n, "%s.game_space", tag);
    // file space is -Y up (arena 5's stands rise to y -4913): the game-space meshes have their lowest y far below zero; the viewer's mirror flips it
    Check(n, ymin < -100.f && ymax < 3000.f, "y range %.0f .. %.0f", ymin, ymax);
    {
      size_t b4 = 0;
      for (const assets::Texture& t : st.textures) b4 += t.format == assets::TexFormat::kB4G4R4A4;
      uint8_t px[4];
      ArenaCore::B4G4R4A4ToRGBA8(0xF123, px);  // A = F, R = 1, G = 2, B = 3
      std::snprintf(n, sizeof n, "%s.b4g4r4a4_order", tag);
      Check(n, px[0] == 17 && px[1] == 34 && px[2] == 51 && px[3] == 255, "0xF123 -> R %u G %u B %u A %u; %zu stage textures use the format", px[0], px[1], px[2], px[3], b4);
    }
    std::printf("INFO  %s  meshes without a diffuse: %zu\n", tag, tex_missing);
    if (a == 4) {  // S101 lane 49: the placed props (slots 390-397) carry one skin pair a vertex: slot < 255, weight 1.0 -> the stream's ids (slot,255,255,255) and weights (0,0,0,255)
      size_t props = 0, skinned = 0, bad_words = 0, others_skinned = 0;
      for (const StageMesh& m : st.meshes) {
        const bool prop = m.slot >= 390 && m.slot <= 397;
        props += prop, skinned += prop && m.skinned;
        others_skinned += !prop && m.skinned;
        if (m.skinned)
          for (size_t i = 0; i < m.vertices.size(); ++i)
            bad_words += !(m.skin_ids[4 * i] < 255 && m.skin_ids[4 * i + 1] == 255 && m.skin_ids[4 * i + 2] == 255 && m.skin_ids[4 * i + 3] == 255 && m.skin_weights[4 * i] == 0 && m.skin_weights[4 * i + 1] == 0 &&
                           m.skin_weights[4 * i + 2] == 0 && m.skin_weights[4 * i + 3] == 255);
      }
      Check("files.arena4.props_skinned", props > 0 && skinned == props && bad_words == 0, "%zu prop meshes (slots 390-397), %zu with skin words, bad words %zu; %zu meshes outside the props skinned", props, skinned, bad_words, others_skinned);
    }
    if (a == 4) {  // [C: map section 1.4] slot 115 mesh 2: 725 -> 726; slot 116 mesh 2: 210 -> 211
      const StageMesh *m115 = nullptr, *m116 = nullptr;
      for (const StageMesh& m : st.meshes) {
        if (m.slot == 115 && m.part == 2) m115 = &m;
        if (m.slot == 116 && m.part == 2) m116 = &m;
      }
      Check("files.arena4.group_rule_values", m115 && m116 && m115->StreamIndexCount(true) == 726 && m116->StreamIndexCount(true) == 211 && m115->StreamIndexCount(false) == 725 &&
                                                    m116->StreamIndexCount(false) == 210,
            "slot 115 mesh 2: %u -> %u, slot 116 mesh 2: %u -> %u", m115 ? m115->StreamIndexCount(false) : 0, m115 ? m115->StreamIndexCount(true) : 0, m116 ? m116->StreamIndexCount(false) : 0,
            m116 ? m116->StreamIndexCount(true) : 0);
    }
  }
  return g_fail ? 1 : 0;
}


// Two loader threads at once (review A1): the arena's worker and the crowd's worker both go through assets::LoadArena and its process-wide caches. Several rounds, each load
// must succeed and equal the single-threaded reference (mesh and vertex counts).
int ThreadChecks(const Options& opt) {
  if (!assets::arena_gs::Init(opt.game) || !assets::crowd_body::Init(opt.game)) {
    std::fprintf(stderr, "arena_ours_test: FAIL no game files at %s\n", opt.game.c_str());
    return 2;
  }
  StageArena ref;
  assets::Model ref_body;
  std::string err;
  if (!assets::arena_gs::Load(4, ref, err) || !assets::crowd_body::LoadBodyLodGameSpace(4, 1, 3, ref_body, err)) {
    std::fprintf(stderr, "arena_ours_test: FAIL reference loads: %s\n", err.c_str());
    return 2;
  }
  size_t ref_verts = 0, ref_body_verts = 0;
  for (const StageMesh& m : ref.meshes) ref_verts += m.vertices.size();
  for (const assets::Mesh& m : ref_body.meshes) ref_body_verts += m.vertices.size();
  int bad = 0;
  const int rounds = 6;
  for (int r = 0; r < rounds; ++r) {
    StageArena a;
    assets::Model b;
    std::string ea, eb;
    bool oka = false, okb = false;
    std::thread ta([&] { oka = assets::arena_gs::Load(4, a, ea); });
    std::thread tb([&] { okb = assets::crowd_body::LoadBodyLodGameSpace(4, 1, 3, b, eb); });
    ta.join();
    tb.join();
    size_t va = 0, vb = 0;
    for (const StageMesh& m : a.meshes) va += m.vertices.size();
    for (const assets::Mesh& m : b.meshes) vb += m.vertices.size();
    if (!oka || !okb || a.meshes.size() != ref.meshes.size() || va != ref_verts || vb != ref_body_verts) {
      ++bad;
      std::printf("INFO  round %d: arena ok %d (%zu meshes, %zu vertices), crowd ok %d (%zu vertices) %s %s\n", r, oka, a.meshes.size(), va, okb, vb, ea.c_str(), eb.c_str());
    }
  }
  Check("threads.two_loaders", bad == 0, "%d rounds of the arena load and the crowd body load on two threads; %d differed from the single-threaded result", rounds, bad);
  return g_fail ? 1 : 0;
}

// What the live game's geometry store looks like to the hook (src\renderer_adapter\live.cpp LiveSource::ReadGeometry): blobs by KEY (not an address): an exact key lookup that
// returns the blob's first `bytes`, false (and zeros) when the key is absent or the blob shorter. The key is the draw's vertex_offset. For every draw a decoy is also stored at
// key + (vertex_count - 1) * stride: garbage, the neighbour blob a hook finds when it does offset arithmetic on a key (run 7067: lane 40's first / last vertex re-check did, and
// failed every frame). A hook that reads each draw's blob by its key as given never sees a decoy.
using LiveStore = std::unordered_map<uint64_t, std::vector<uint8_t>>;
void BuildLiveStore(const CaptureReader& cap, const LoadedFrame& L, LiveStore& store) {
  store.clear();
  for (uint32_t d = 0; d < L.draws.count; ++d) {
    const rdr_adapter::Draw& dr = L.draws[d];
    if (dr.layout == 0xFFFFFFFFu || dr.vertex_bytes == 0 || (dr.flags & rdr_adapter::kDrawNoGeometry)) continue;
    std::vector<uint8_t> b;
    if (cap.ReadGeometry(dr.vertex_offset, dr.vertex_bytes, b)) store.emplace(dr.vertex_offset, std::move(b));
  }
  for (uint32_t d = 0; d < L.draws.count; ++d) {
    const rdr_adapter::Draw& dr = L.draws[d];
    if (dr.layout == 0xFFFFFFFFu || dr.vertex_bytes == 0 || dr.vertex_count < 2 || dr.layout >= L.layouts.count) continue;
    const uint32_t stride = L.layouts[dr.layout].host_stride;
    store.emplace(dr.vertex_offset + uint64_t(dr.vertex_count - 1) * stride, std::vector<uint8_t>(stride, 0xAB));
  }
}
bool ReadLive(const LiveStore& store, uint64_t key, uint32_t bytes, std::vector<uint8_t>& out) {
  const auto it = store.find(key);
  if (it == store.end() || it->second.size() < bytes) {
    out.assign(bytes, 0);
    return false;
  }
  out.assign(it->second.begin(), it->second.begin() + bytes);
  return true;
}

// One bad draw keeps only itself (review of run 7067: a single failed per-draw check used to fall the whole frame back). On one frame, live-style: pass A replaces N draws; the
// first replaced draw's vertex blob is corrupted (its first vertex moved); pass B on the same core must replace all but the draws that read that blob, with no "not ready" at all.
int CorruptOneCheck(const Options& opt, CaptureReader& cap, const std::shared_ptr<StageArena>& stage, int arena_id, int frame) {
  LoadedFrame L;
  if (!cap.LoadFrame(uint32_t(frame), L)) {
    std::fprintf(stderr, "arena_ours_test: FAIL cannot load frame %d\n", frame);
    return 2;
  }
  bool fault_known = true;
  const ArenaFaults faults = FaultsOf(opt.fault, fault_known);
  SceneTables rec;
  std::string e;
  if (!cap.HasSceneRecord(uint32_t(frame)) || !ReadRecord(cap.SceneRecordPath(uint32_t(frame)).string(), rec, e)) {
    std::fprintf(stderr, "arena_ours_test: FAIL no record for frame %d\n", frame);
    return 2;
  }
  auto record = std::make_shared<SceneTables>(rec);
  ArenaCore core(faults);
  core.SetArena(stage, arena_id);
  core.SetImageInfo([&cap](uint32_t id, uint32_t& w, uint32_t& h) {
    const rdr_adapter::ImageRecord* r = cap.FindImage(id);
    if (!r) return false;
    w = r->width, h = r->height;
    return true;
  });
  LiveStore store;
  BuildLiveStore(cap, L, store);
  uint64_t reads = 0;  // geometry reads the hook made
  struct Pass {
    uint64_t replaced = 0, mismatch = 0, not_ready = 0, claimed = 0;
    std::vector<uint64_t> replaced_keys;
  };
  const auto pass = [&]() {
    Pass p;
    core.ResetFrame();
    core.SetRecord(record);
    for (uint32_t d = 0; d < L.draws.count; ++d) {
      const rdr_adapter::DrawOrigin* o = L.OriginOf(d);
      if (!o || o->klass != rdr_adapter::DrawClass::ModelPart || !core.Claims(*o)) continue;
      const rdr_adapter::Draw& dr = L.draws[d];
      if ((dr.flags & rdr_adapter::kDrawNoGeometry) || dr.layout == 0xFFFFFFFFu) continue;
      const int vs = FindVariant(dr.vs_hash), ps = dr.ps_hash ? FindVariant(dr.ps_hash) : -1;
      if (vs < 0) continue;
      rdr_adapter::ReplaceIn in;
      in.frame = &L;
      in.draw = &dr;
      in.draw_index = d;
      in.origin = o;
      in.vs = vs, in.ps = ps;
      in.vertex_count = dr.vertex_count, in.index_count = dr.index_count;
      in.vertex_offset = dr.vertex_offset, in.index_offset = dr.index_offset;
      in.vertex_stride = L.layouts[dr.layout].host_stride;
      in.read_geometry = [&store, &reads](uint64_t key, uint32_t bytes, std::vector<uint8_t>& out) { return ++reads, ReadLive(store, key, bytes, out); };
      uint8_t slots[rdr::kMaxTextures];
      uint32_t n = 0;
      if (ps >= 0)
        for (uint32_t s = 0; s < rdr::fx::kVariants[ps].sampler_count && n < rdr::kMaxTextures; ++s) slots[n++] = rdr::fx::kVariants[ps].samplers[s].slot;
      const ArenaCore::Decision dec = core.Decide(in, rdr::fx::kVariants[vs].name, ps >= 0 ? rdr::fx::kVariants[ps].name : "", slots, n, nullptr);
      ++p.claimed;
      if (dec.ok) ++p.replaced, p.replaced_keys.push_back(dr.vertex_offset);
      else if (dec.why == rdr_adapter::KeepWhy::Mismatch) ++p.mismatch;
      else if (dec.why == rdr_adapter::KeepWhy::NotReady) ++p.not_ready;
    }
    return p;
  };
  const Pass a = pass();
  Check("corrupt_one.clean_pass", a.replaced > 0 && a.mismatch == 0 && a.not_ready == 0, "frame %d live-style: %llu claimed, %llu replaced, %llu mismatch, %llu not ready", frame, (unsigned long long)a.claimed,
        (unsigned long long)a.replaced, (unsigned long long)a.mismatch, (unsigned long long)a.not_ready);
  if (a.replaced_keys.empty()) return 1;
  const uint64_t reads_first = reads;
  const Pass a2 = pass();  // steady state: every key already passed; a key is re-read only on its proof frame (S101 lane 88: one frame in kArenaProofEvery, staggered by key)
  Check("corrupt_one.steady_state_replaces_same", a2.replaced == a.replaced && a2.mismatch == 0, "second frame: %llu replaced (first %llu), %llu geometry reads (the first frame made %llu: the joins and the re-checks)", (unsigned long long)a2.replaced,
        (unsigned long long)a.replaced, (unsigned long long)(reads - reads_first), (unsigned long long)reads_first);
  {  // a whole proof period of steady frames: the proofs re-read each key once, no more than the first frame did, and nothing changes
    const uint64_t r0 = reads;
    bool same = true;
    for (uint32_t i = 0; i < kArenaProofEvery; ++i) {
      const Pass pn = pass();
      same = same && pn.replaced == a.replaced && pn.mismatch == 0 && pn.not_ready == 0;
    }
    Check("corrupt_one.proof_period_steady", same && reads - r0 <= reads_first, "%u steady frames: replaced stays %llu, %llu geometry reads (at most the first frame's %llu: each key is proved once)", kArenaProofEvery, (unsigned long long)a.replaced,
          (unsigned long long)(reads - r0), (unsigned long long)reads_first);
  }
  std::map<uint64_t, uint64_t> uses;  // blob key -> replaced draws reading it (orig + mirrored copies, equal objects share a blob)
  for (uint64_t k : a.replaced_keys) ++uses[k];
  uint64_t victim = a.replaced_keys.front(), share = 0;
  for (const auto& [k, n] : uses)  // prefer a blob read by several replaced draws: the strict equations below then say more
    if (n > share) victim = k, share = n;
  const rdr_adapter::VertexLayout* lay = nullptr;
  for (uint32_t d = 0; d < L.draws.count; ++d)
    if (L.draws[d].vertex_offset == victim && L.draws[d].layout < L.layouts.count) lay = &L.layouts[L.draws[d].layout], d = L.draws.count;
  uint32_t off = lay ? L.elements[lay->element_first].host_offset : 0;
  // The bad read: the draws that read this blob now carry a NEW key (live keys are never reused: a changed blob is a new key) whose bytes have the first vertex moved by one unit.
  const uint64_t fresh = victim + (uint64_t(1) << 40);
  std::vector<uint8_t> bad = store[victim];
  float x;
  std::memcpy(&x, bad.data() + off, 4);
  x += 1.0f;
  std::memcpy(bad.data() + off, &x, 4);
  store[fresh] = std::move(bad);
  for (uint32_t d = 0; d < L.draws.count; ++d)
    if (L.draws[d].vertex_offset == victim) const_cast<rdr_adapter::Draw&>(L.draws[d]).vertex_offset = fresh;
  const Pass b = pass();
  Check("corrupt_one.only_itself", b.mismatch >= 1 && b.mismatch <= share && b.not_ready == 0 && b.replaced < a.replaced && b.replaced + 2 * share >= a.replaced,
        "one blob corrupted (shared by %llu replaced draws): %llu replaced of %llu, %llu mismatch (its draws only), %llu not ready (must be 0: no frame fallback)", (unsigned long long)share,
        (unsigned long long)b.replaced, (unsigned long long)a.replaced, (unsigned long long)b.mismatch, (unsigned long long)b.not_ready);
  const Pass c = pass();  // the next frame: the corrupted read is still there; the dropped joins re-join by content (positions differ: soft not-ours), everything else is replaced
  Check("corrupt_one.others_unaffected", c.replaced + share == a.replaced && c.mismatch + c.not_ready == 0,
        "next frame, blob still bad: %llu replaced of %llu (exactly the %llu draws reading the bad blob are lost), %llu mismatch + %llu not ready (must be 0)", (unsigned long long)c.replaced,
        (unsigned long long)a.replaced, (unsigned long long)share, (unsigned long long)c.mismatch, (unsigned long long)c.not_ready);
  {  // S101 lane 88: a blob corrupted IN PLACE after the pin (the same key, other bytes) must be found by the key's proof frame within kArenaProofEvery frames. The planted twin no_proof_frames never re-checks.
    uint64_t victim2 = 0;
    for (uint64_t k : a.replaced_keys)
      if (k != victim && store.count(k)) victim2 = k;
    const rdr_adapter::VertexLayout* lay2 = nullptr;
    for (uint32_t d = 0; d < L.draws.count && victim2; ++d)
      if (L.draws[d].vertex_offset == victim2 && L.draws[d].layout < L.layouts.count) lay2 = &L.layouts[L.draws[d].layout], d = L.draws.count;
    if (!victim2 || !lay2) {
      Check("corrupt_one.in_place_detected", false, "no second blob to corrupt");
    } else {
      const uint32_t off2 = L.elements[lay2->element_first].host_offset;
      float x2;
      std::memcpy(&x2, store[victim2].data() + off2, 4);
      x2 += 1.0f;
      std::memcpy(store[victim2].data() + off2, &x2, 4);
      uint32_t found_at = 0;
      for (uint32_t i = 1; i <= kArenaProofEvery && !found_at; ++i)
        if (pass().mismatch >= 1) found_at = i;
      Check("corrupt_one.in_place_detected", found_at != 0, "first vertex of a pinned blob changed in place: found by frame %u of %u (0 = never found)", found_at, kArenaProofEvery);
    }
  }
  return g_fail ? 1 : 0;
}
// ---- capture mode ------------------------------------------------------------------------------------------------------------------
struct Cand {  // an oracle-known stage draw
  bool any = false;
  int slot = -1, part = -1;
};

int ArenaOfCaptureRecord(CaptureReader& cap, uint32_t frames) {
  for (uint32_t f = 0; f < frames; ++f) {
    if (!cap.HasSceneRecord(f)) continue;
    SceneTables rec;
    std::string err;
    if (!ReadRecord(cap.SceneRecordPath(f).string(), rec, err)) continue;
    const int a = ArenaCore::ArenaOfRecord(rec);
    if (a >= 0) return a;
  }
  return -1;
}

int CaptureChecks(const Options& opt) {
  bool known = true;
  const ArenaFaults faults = FaultsOf(opt.fault, known);
  if (!known) {
    std::fprintf(stderr, "arena_ours_test: unknown fault %s\n", opt.fault.c_str());
    return 3;
  }
  CaptureReader cap;
  if (!cap.Open(opt.capture)) {
    std::fprintf(stderr, "arena_ours_test: FAIL cannot open the capture at %s\n", opt.capture.c_str());
    return 2;
  }
  if (cap.scene_record_count() == 0) {
    std::fprintf(stderr, "arena_ours_test: FAIL %s has no scene records (scene\\frame_N.rsc)\n", opt.capture.c_str());
    return 2;
  }
  const int nframes = int(cap.frame_count());
  const int arena_id = ArenaOfCaptureRecord(cap, uint32_t(nframes));
  if (arena_id < 0) {
    std::fprintf(stderr, "arena_ours_test: FAIL no record in %s names an arena pac\n", opt.capture.c_str());
    return 2;
  }
  if (!assets::arena_gs::Init(opt.game)) {
    std::fprintf(stderr, "arena_ours_test: FAIL no game files at %s\n", opt.game.c_str());
    return 2;
  }
  auto stage = std::make_shared<StageArena>();
  std::string err;
  if (!assets::arena_gs::Load(arena_id, *stage, err, faults.viewer_space)) {
    std::fprintf(stderr, "arena_ours_test: FAIL arena %d: %s\n", arena_id, err.c_str());
    return 2;
  }
  // The oracle's index: vertex count -> meshes (position match is brute force over these, whatever the part and the index count).
  std::unordered_map<uint32_t, std::vector<int>> by_count;
  for (size_t i = 0; i < stage->meshes.size(); ++i) by_count[uint32_t(stage->meshes[i].vertices.size())].push_back(int(i));

  if (opt.corrupt_one) return CorruptOneCheck(opt, cap, stage, arena_id, opt.first);
  ArenaCore core(faults, /*verify_content=*/!opt.live_style);
  core.SetArena(stage, arena_id);
  // S101 lane 47: the image info comes from the LIVE supplier (image_info_table.h, the table live.cpp feeds from its image records), fed here from the capture's records through the same Set().
  rdr_adapter::ImageInfoTable live_table;
  {
    std::vector<rdr_adapter::ImageRecord> records = cap.images();
    if (opt.supplier_fault == "shrink") for (rdr_adapter::ImageRecord& r : records) r.width = uint16_t(r.width / 2);   // planted: every width halved
    else if (opt.supplier_fault == "drop" && !records.empty()) records.resize(records.size() / 2);                       // planted: the newer half of the table missing
    else if (opt.supplier_fault == "shift" && records.size() > 1) records.erase(records.begin());                         // planted: the table starts one record late (ids no longer match slots)
    live_table.Set(records.data(), records.size());
  }
  if (!opt.no_image_info) core.SetImageInfo(live_table.Supplier());
  std::map<uint32_t, rdr_adapter::ContentRef> contents;   // the newest content of each image up to the frame being judged
  std::set<std::pair<uint64_t, int>> tex_seen;            // (content offset, our texture) pairs already compared
  if (opt.last < 0 || opt.last >= nframes) {
    const_cast<Options&>(opt).last = nframes - 1;
  }

  // Tallies
  uint64_t stage_draws = 0, oracle_draws = 0, replaced = 0, wrong_join = 0, oracle_missed = 0, oracle_variant = 0, oracle_size_kept = 0, part_oracle = 0, part_oracle_eq = 0, flipped_y = 0, tex_cmp = 0, tex_slot_set = 0, tex_cmp_bad = 0, tex_nocmp = 0, var_mirror = 0, var_orig = 0, kept_stage_after_join = 0, kept_oracle_total = 0;
  uint64_t part_eq_record = 0, part_eq_ordinal = 0, part_checked = 0;
  uint64_t tri_bad = 0, tri_checked = 0, uv_bad = 0, uv_checked = 0, normal_bad = 0, normal_checked = 0, color_bad = 0, color_checked = 0;
  uint64_t tex_bad = 0, tex_checked = 0, lm_set = 0, mirror_replaced = 0, mirror_oracle = 0, orig_oracle = 0, orig_replaced = 0;
  double worst_uv = 0, worst_normal = 0;
  std::map<std::string, uint64_t> kept_reasons;
  std::set<int> gap_slots;
  std::map<std::string, uint64_t> variant_names;
  std::set<uint32_t> objects_seen;
  std::map<std::array<int, 4>, uint64_t> color_perm_votes;  // which source byte feeds each of our 4 colour bytes
  std::set<std::string> color_format_note;
  bool printed_probe = false;
  int vprobe_count = 0;
  int diag_count[4] = {};
  uint64_t skin_draws = 0, skin_vertices = 0, skin_bad_ids = 0, skin_bad_weights = 0, skin_layout_bad = 0, skin_no_words = 0;   // S101 lane 49
  std::set<int> skin_slots_replaced;
  int skin_probe = 0;
  uint64_t skin_row_vertices = 0, skin_id_nonzero = 0, skin_row_differs = 0;
  uint64_t uv_file_neg_u = 0, uv_stream_neg_u = 0, uv_file_neg_v = 0, uv_stream_neg_v = 0;
  std::map<std::string, uint64_t> uv_by_vs, col_by_vs, uv_class, neg_u_modes;
  uint64_t frames_run = 0;
  uint64_t decide_ns = 0, decide_calls = 0;   // S101 lane 88: the time inside ArenaCore::Decide
  uint64_t first_frame_unjoined = 0;
  std::set<uint32_t> joined_objects;

  LoadedFrame L;
  for (int f = 0; f <= opt.last; ++f) {
    if (!cap.LoadFrame(uint32_t(f), L)) {
      std::fprintf(stderr, "arena_ours_test: FAIL cannot load frame %d\n", f);
      return 2;
    }
    for (const rdr_adapter::ContentRef& c : L.contents) contents[c.image] = c;
    if (f < opt.first || (f - opt.first) % opt.step) continue;
    std::shared_ptr<SceneTables> rec;
    if (cap.HasSceneRecord(uint32_t(f))) {
      rec = std::make_shared<SceneTables>();
      std::string e2;
      if (!ReadRecord(cap.SceneRecordPath(uint32_t(f)).string(), *rec, e2)) {
        std::fprintf(stderr, "arena_ours_test: FAIL record %d: %s\n", f, e2.c_str());
        return 2;
      }
    }
    core.ResetFrame();
    core.SetRecord(rec);
    ++frames_run;
    LiveStore live;
    if (opt.live_style) BuildLiveStore(cap, L, live);
    for (uint32_t d = 0; d < L.draws.count; ++d) {
      const rdr_adapter::DrawOrigin* o = L.OriginOf(d);
      if (!o || o->klass != rdr_adapter::DrawClass::ModelPart) continue;
      if (o->pass_site != rdr_adapter::kModelPartColourSite && o->pass_site != rdr_adapter::kModelPartMirrorSite) continue;
      const rdr_adapter::Draw& dr = L.draws[d];
      if (dr.flags & rdr_adapter::kDrawNoGeometry) continue;
      if (dr.layout == 0xFFFFFFFFu) continue;
      const rdr_adapter::VertexLayout& lay = L.layouts[dr.layout];
      const bool mirror = o->pass_site == rdr_adapter::kModelPartMirrorSite;
      const int vs = FindVariant(dr.vs_hash), ps = dr.ps_hash ? FindVariant(dr.ps_hash) : -1;
      if (vs < 0) continue;
      const rdr::fx::Variant& vv = rdr::fx::kVariants[vs];
      ++stage_draws;

      // ---- the independent oracle: any mesh of the arena with these vertex positions ----
      std::vector<uint8_t> vb;
      Cand oracle;
      if (lay.element_count && (L.elements[lay.element_first].vk_format == 106 || L.elements[lay.element_first].vk_format == 109) &&
          cap.ReadGeometry(dr.vertex_offset, dr.vertex_count * lay.host_stride, vb)) {
        const uint32_t off = L.elements[lay.element_first].host_offset;
        auto it = by_count.find(dr.vertex_count);
        if (it != by_count.end()) {
          std::set<int> parts;
          bool flipped = false;
          for (int mi : it->second) {
            const StageMesh& m = stage->meshes[size_t(mi)];
            bool all = true, all_flip = true;
            for (size_t i = 0; i < m.vertices.size() && (all || all_flip); ++i) {
              float p[3];
              std::memcpy(p, vb.data() + i * lay.host_stride + off, 12);
              for (int c = 0; c < 3; ++c) all = all && std::fabs(p[c] - m.vertices[i].pos[c]) <= 1e-3f;
              all_flip = all_flip && std::fabs(p[0] - m.vertices[i].pos[0]) <= 1e-3f && std::fabs(p[1] + m.vertices[i].pos[1]) <= 1e-3f && std::fabs(p[2] - m.vertices[i].pos[2]) <= 1e-3f;
            }
            if (all) {
              if (!oracle.any) oracle.any = true, oracle.slot = m.slot, oracle.part = m.part;
              parts.insert(m.part);
            }
            flipped = flipped || all_flip;
          }
          if (!oracle.any && flipped) ++flipped_y;  // the stream holds this mesh with the viewer's Y mirror undone: ours would be in the wrong space
          if (oracle.any && parts.size() == 1) {  // the part is named by the geometry alone: the origin's part must be it
            ++part_oracle;
            part_oracle_eq += int(o->part) == oracle.part;
          }
        }
      }
      if (oracle.any) {
        ++oracle_draws;
        (mirror ? mirror_oracle : orig_oracle)++;
      }

      // ---- the shipped decision ----
      rdr_adapter::ReplaceIn in;
      in.frame = &L;
      in.draw = &dr;
      in.draw_index = d;
      in.origin = o;
      in.vs = vs, in.ps = ps;
      in.vertex_count = dr.vertex_count, in.index_count = dr.index_count;
      in.vertex_offset = dr.vertex_offset, in.index_offset = dr.index_offset;
      in.vertex_stride = lay.host_stride;
      if (opt.live_style) in.read_geometry = [&live](uint64_t key, uint32_t bytes, std::vector<uint8_t>& out) { return ReadLive(live, key, bytes, out); };
      else in.read_geometry = [&cap](uint64_t off, uint32_t bytes, std::vector<uint8_t>& out) { return cap.ReadGeometry(off, bytes, out); };
      uint8_t slots[rdr::kMaxTextures];
      uint32_t slot_count = 0;
      if (ps >= 0) {
        const rdr::fx::Variant& pv = rdr::fx::kVariants[ps];
        for (uint32_t s = 0; s < pv.sampler_count && slot_count < rdr::kMaxTextures; ++s) slots[slot_count++] = pv.samplers[s].slot;
      }
      float world[12] = {};
      bool have_world = false;
      if (dr.vs_bank < L.constant_banks.count) {
        const float* c = reinterpret_cast<const float*>(L.constant_banks[dr.vs_bank].dwords);
        for (uint32_t r = 0; r < vv.constant_count; ++r)
          if (vv.constants[r].reg == 76 && vv.constants[r].count >= 3) have_world = true;
        if (have_world) std::memcpy(world, c + 76 * 4, sizeof world);
      }
      uint32_t sw = 0, sh = 0;
      for (uint32_t t = 0; t < dr.texture_count; ++t) {
        const rdr_adapter::TextureSlot& ts = L.texture_slots[dr.texture_first + t];
        if (ts.slot != 0 || ts.image == 0) continue;
        if (const rdr_adapter::ImageRecord* ir = cap.FindImage(ts.image)) sw = ir->width, sh = ir->height;
      }
      const auto t_decide = std::chrono::steady_clock::now();
      const ArenaCore::Decision dec = core.Decide(in, vv.name, ps >= 0 ? rdr::fx::kVariants[ps].name : "", slots, slot_count, have_world ? world : nullptr);
      decide_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t_decide).count()), ++decide_calls;
      const bool claimed = core.Claims(*o);
      (void)claimed;

      if (opt.probe && !printed_probe && oracle.any) {
        printed_probe = true;
        std::printf("PROBE frame %d draw %u: vs %s ps %s prim %u verts %u (stride %u) indices %u (%u bytes, %s) layout:", f, d, vv.name, ps >= 0 ? rdr::fx::kVariants[ps].name : "-", dr.source_prim, dr.vertex_count,
                    lay.host_stride, dr.index_count, dr.index_bytes, dr.index_32bit ? "32" : "16");
        for (uint32_t e = 0; e < lay.element_count; ++e) {
          const auto& el = L.elements[lay.element_first + e];
          std::printf(" [vk %u off %u src %u comps %u flags %u]", el.vk_format, el.host_offset, el.source_format, el.components, el.flags);
        }
        std::printf("\n");
        std::vector<uint8_t> ib;
        if (cap.ReadGeometry(dr.index_offset, std::min<uint32_t>(dr.index_bytes, 64), ib)) {
          std::printf("PROBE first indices:");
          for (size_t i = 0; i + 1 < ib.size(); i += dr.index_32bit ? 4 : 2) std::printf(" %u", dr.index_32bit ? *reinterpret_cast<uint32_t*>(&ib[i]) : *reinterpret_cast<uint16_t*>(&ib[i]));
          std::printf("\n");
        }
        std::printf("PROBE oracle slot %d part %d; draw part %u; decision ok %d reason '%s'\n", oracle.slot, oracle.part, o->part, dec.ok, dec.reason);
      }

      if (opt.probe && !dec.ok && dec.why == rdr_adapter::KeepWhy::Variant && oracle.any && vprobe_count < 40 && (stage_draws % 37) == 0) {  // S101 lane 49: the bone-id variants' stream vertex
        ++vprobe_count;
        std::printf("VPROBE frame %d draw %u slot %d part %d vs %s verts %u stride %u elements:", f, d, oracle.slot, oracle.part, vv.name, dr.vertex_count, lay.host_stride);
        for (uint32_t e = 0; e < lay.element_count; ++e) {
          const auto& el = L.elements[lay.element_first + e];
          std::printf(" [vk %u off %u src %u comps %u flags %u mini %u]", el.vk_format, el.host_offset, el.source_format, el.components, el.flags, el.mini);
        }
        std::printf("\n");
        for (size_t i = 0; i < std::min<size_t>(vb.size() / std::max(1u, lay.host_stride), 6); ++i) {
          std::printf("VPROBE   v%zu:", i);
          for (uint32_t b = 0; b < lay.host_stride; ++b) std::printf("%s%02x", b % 4 == 0 ? " " : "", vb[i * lay.host_stride + b]);
          std::printf("\n");
        }
      }
      if (!dec.ok) {
        kept_reasons[std::string(rdr_adapter::KeepWhyName(dec.why)) + ": " + dec.reason]++;
        if (oracle.any && dec.why == rdr_adapter::KeepWhy::Texture && std::strcmp(dec.reason, "image size") == 0) {
          ++oracle_size_kept;  // expected: the stream's image has another size than the file picture (msg_score2 252x64 vs 256x64): the stream's draw stays, counted
          (mirror ? var_mirror : var_orig)++;
        } else if (oracle.any && dec.why == rdr_adapter::KeepWhy::Variant) {
          ++oracle_variant;
          (mirror ? var_mirror : var_orig)++;
          variant_names[vv.name]++;  // expected: a vertex program whose inputs our vertex lacks (bone ids): the stream's draw stays, counted
        } else if (oracle.any) {
          ++oracle_missed;
          ++kept_oracle_total;
          const assets::Texture* ot = nullptr;
          for (const StageMesh& sm : stage->meshes)
            if (sm.slot == oracle.slot && sm.part == oracle.part && sm.diffuse >= 0) ot = &stage->textures[size_t(sm.diffuse)];
          if (opt.probe && diag_count[3]++ < 3)
            std::printf("DIAG kept oracle draw: frame %d draw %u slot %d part %d reason %s: %s (stream slot0 %ux%u; ours %ux%u fmt %d name %s live %d)\n", f, d, oracle.slot, oracle.part, rdr_adapter::KeepWhyName(dec.why), dec.reason, sw, sh, ot ? ot->width : 0, ot ? ot->height : 0, ot ? int(ot->format) : -1, ot ? ot->name.c_str() : "-", ot ? int(ot->live) : -1);
        }
        continue;
      }
      ++replaced;
      (mirror ? mirror_replaced : orig_replaced)++;
      joined_objects.insert(o->object);
      objects_seen.insert(o->object);
      const StageMesh& m = stage->meshes[size_t(dec.mesh)];
      // wrong join: the oracle disagrees with the mesh we chose
      if (!oracle.any || oracle.slot != m.slot) {
        // (duplicate geometry in two slots is not wrong: the positions equal)
        bool same = false;
        if (oracle.any) same = m.vertices.size() == stage->meshes[size_t(by_count[dr.vertex_count][0])].vertices.size();
        (void)same;
        // compare the chosen mesh's positions with the stream directly
        bool all = !vb.empty();
        if (all) {
          const uint32_t off = L.elements[lay.element_first].host_offset;
          for (size_t i = 0; i < m.vertices.size() && all; ++i) {
            float p[3];
            std::memcpy(p, vb.data() + i * lay.host_stride + off, 12);
            for (int c = 0; c < 3; ++c) all = all && std::fabs(p[c] - m.vertices[i].pos[c]) <= 1e-3f;
          }
        }
        if (!all) ++wrong_join;
      }
      // part = record index
      ++part_checked;
      part_eq_record += int(o->part) == m.part;
      // texture roles
      ++tex_checked;
      tex_bad += dec.diffuse >= 0 && dec.diffuse != m.diffuse;
      tex_slot_set += (dec.diffuse >= 0) + (dec.normal_map >= 0) + (dec.specular >= 0);
      lm_set += dec.file_lightmap >= 0;
      // texture bytes: the file's diffuse mip 0 against the stream image's content, when the capture holds that content (BC1 / 2 / 3, same size)
      if (dec.diffuse >= 0 && dec.diffuse == m.diffuse) {
        const assets::Texture& ot = stage->textures[size_t(dec.diffuse)];
        const bool bc = ot.format == assets::TexFormat::kBC1 || ot.format == assets::TexFormat::kBC2 || ot.format == assets::TexFormat::kBC3;
        uint32_t img = 0;
        for (uint32_t t = 0; t < dr.texture_count; ++t)
          if (L.texture_slots[dr.texture_first + t].slot == 0) img = L.texture_slots[dr.texture_first + t].image;
        const auto cit = contents.find(img);
        if (!bc || cit == contents.end()) {
          ++tex_nocmp;
        } else if (tex_seen.insert({cit->second.offset, dec.diffuse}).second) {
          rdr_adapter::LoadedTexture lt;
          if (!cap.ReadTexture(cit->second, lt) || lt.header.subresource_count == 0) {
            ++tex_nocmp;
          } else {
            const rdr_adapter::Subresource& sub = lt.subresources[0];
            const size_t bytes = size_t(sub.row_bytes) * sub.rows;
            ++tex_cmp;
            tex_cmp_bad += !(sub.width == ot.width && sub.height == ot.height && bytes <= ot.data.size() && lt.bytes.size() >= size_t(lt.data - lt.bytes.data()) + sub.offset + bytes &&
                             std::memcmp(lt.data + sub.offset, ot.data.data(), bytes) == 0);
          }
        }
      }
      // triangles: the stream's indices as a strip (restart 0xFFFF) -> list vs ours (through the shipped FillIndices)
      std::vector<uint8_t> ib;
      if (dr.index_bytes && cap.ReadGeometry(dr.index_offset, dr.index_bytes, ib)) {
        std::vector<uint16_t> raw(dr.index_bytes / (dr.index_32bit ? 4 : 2));
        for (size_t i = 0; i < raw.size(); ++i) raw[i] = dr.index_32bit ? uint16_t(*reinterpret_cast<uint32_t*>(&ib[i * 4])) : *reinterpret_cast<uint16_t*>(&ib[i * 2]);
        std::vector<uint32_t> theirs;
        StripToList(raw, dr.vertex_count, theirs);
        std::vector<uint32_t> ours;
        core.FillIndices(m, ours);
        ++tri_checked;
        tri_bad += TriSet(theirs) != TriSet(ours);
      }
      {
        bool neg_u = false;
        for (const auto& vx : m.vertices) neg_u = neg_u || vx.uv[0] < 0.f;
        if (neg_u) {
          for (uint32_t t = 0; t < dr.texture_count; ++t) {
            const rdr_adapter::TextureSlot& ts = L.texture_slots[dr.texture_first + t];
            if (ts.slot != 0 || ts.sampler >= L.samplers.count) continue;
            const auto& sd = L.samplers[ts.sampler];
            char key[48];
            std::snprintf(key, sizeof key, "address u %u v %u", sd.address_u, sd.address_v);
            neg_u_modes[key]++;
          }
        }
      }
      // vertex content: element 1 normal, 2 colour, 3 uv (the yBG vertex's inputs in program order)
      std::vector<assets::arena_gs::Vertex> verts;
      core.FillVertices(m, verts);
      const bool skin_layout = lay.element_count == 6 && NeedsSkinVariant(vs);   // S101 lane 49: pos, weights, bone ids, normal, colour, uv
      if (skin_layout && !vb.empty()) {
        // ours (the shipped FillSkinVertices) against the stream's two words, EVERY vertex, byte for byte; and our layout's formats against the stream's elements
        std::vector<assets::arena_gs::SkinVertex> sv;
        ++skin_draws;
        if (opt.probe && skin_probe < 14 && (skin_draws % 389) == 1 && dr.vs_bank < L.constant_banks.count) {  // the bone matrices (c76 on, 3 registers a bone) the stream draw carries
          ++skin_probe;
          const float* c = reinterpret_cast<const float*>(L.constant_banks[dr.vs_bank].dwords);
          std::printf("SPROBE frame %d slot %d part %d:", f, m.slot, m.part);
          for (int bone = 0; bone < 4; ++bone) {
            std::printf(" bone%d[", bone);
            for (int k = 0; k < 12; ++k) std::printf("%s%.1f", k ? " " : "", c[(76 + 3 * bone) * 4 + k]);
            std::printf("]");
          }
          std::printf("\n");
        }
        skin_slots_replaced.insert(m.slot);
        if (!core.FillSkinVertices(m, sv) || sv.size() != dr.vertex_count) {
          ++skin_no_words;
        } else {
          const auto& ew = L.elements[lay.element_first + 1];
          const auto& ei = L.elements[lay.element_first + 2];
          rdr::VertexLayoutDesc ld;
          bool fw = false, fi = false;
          if (ArenaCore::BuildLayout(vs, ld, true)) {
            const rdr::fx::Variant& vr = rdr::fx::kVariants[vs];
            for (uint32_t a = 0; a < vr.attribute_count; ++a)
              for (uint32_t k = 0; k < ld.attribute_count; ++k)
                if (ld.attributes[k].location == vr.attributes[a].location) {
                  const auto& el = L.elements[lay.element_first + vr.attributes[a].fetch];
                  if (vr.attributes[a].fetch == 1) fw = ld.attributes[k].format == el.vk_format && ld.attributes[k].offset == offsetof(assets::arena_gs::SkinVertex, weights) && ld.strides[0] == sizeof(assets::arena_gs::SkinVertex);
                  if (vr.attributes[a].fetch == 2) fi = ld.attributes[k].format == el.vk_format && ld.attributes[k].offset == offsetof(assets::arena_gs::SkinVertex, ids);
                }
          }
          skin_layout_bad += !(fw && fi);
          for (size_t i = 0; i < sv.size(); ++i) {
            const uint8_t* base = vb.data() + i * lay.host_stride;
            ++skin_vertices;
            skin_bad_weights += std::memcmp(base + ew.host_offset, sv[i].weights, 4) != 0;
            skin_bad_ids += std::memcmp(base + ei.host_offset, sv[i].ids, 4) != 0;
            if (dr.vs_bank < L.constant_banks.count && sv[i].ids[0] < 20) {   // S101 lane 49 (review D3): does the bone row the vertex indexes differ from row 0?
              const float* c = reinterpret_cast<const float*>(L.constant_banks[dr.vs_bank].dwords);
              ++skin_row_vertices;
              skin_id_nonzero += sv[i].ids[0] != 0;
              skin_row_differs += sv[i].ids[0] != 0 && std::memcmp(c + (76 + 3 * sv[i].ids[0]) * 4, c + 76 * 4, 48) != 0;
            }
          }
        }
      }
      if (lay.element_count >= 4 && !vb.empty()) {
        const int e_n = skin_layout ? 3 : 1, e_c = skin_layout ? 4 : 2, e_u = skin_layout ? 5 : 3;
        const auto& en = L.elements[lay.element_first + e_n];
        const auto& ec = L.elements[lay.element_first + e_c];
        const auto& eu = L.elements[lay.element_first + e_u];
        const size_t step = std::max<size_t>(1, verts.size() / 64);  // up to 64 vertices a mesh, spread
        for (size_t i = 0; i < verts.size(); i += step) {
          const uint8_t* base = vb.data() + i * lay.host_stride;
          // uv: two half floats (source 31/25) or two floats
          float u[2];
          if (eu.components == 2 && eu.vk_format == 83) {  // R16G16_SFLOAT
            uint16_t h[2];
            std::memcpy(h, base + eu.host_offset, 4);
            u[0] = HalfToFloat(h[0]), u[1] = HalfToFloat(h[1]);
          } else if (eu.components == 2 && eu.vk_format == 103) {
            std::memcpy(u, base + eu.host_offset, 8);
          } else {
            u[0] = u[1] = NAN;
          }
          ++uv_checked;
          uv_file_neg_u += verts[i].uv[0] < -1e-3f;
          uv_stream_neg_u += u[0] < -1e-3f;
          uv_file_neg_v += verts[i].uv[1] < -1e-3f;
          uv_stream_neg_v += u[1] < -1e-3f;
          const double e0 = std::fabs(std::fabs(u[0]) - std::fabs(verts[i].uv[0])), e1 = std::fabs(u[1] - verts[i].uv[1]);
          const double tol = 1.5e-3 * std::max(1.0, double(std::max(std::fabs(verts[i].uv[0]), std::fabs(verts[i].uv[1]))));
          const bool uv_ok = e0 <= tol && e1 <= tol;
          if (uv_ok) worst_uv = std::max(worst_uv, std::max(e0, e1));
          uv_bad += !uv_ok;
          if (!uv_ok && opt.probe && diag_count[0]++ < 6)
            std::printf("DIAG uv off: frame %d draw %u slot %d part %d vertex %zu ours (%g %g) uv1 (%g %g) stream (%g %g) vk %u eff %s vs %s\n", f, d, m.slot, m.part, i, verts[i].uv[0], verts[i].uv[1], m.uv1[2 * i], m.uv1[2 * i + 1], u[0], u[1], eu.vk_format, m.effect.c_str(), vv.name);
          if (!uv_ok) {
            const double a0 = m.uv1[2 * i], a1 = m.uv1[2 * i + 1];
            auto close = [&](double x, double y) { return std::fabs(u[0] - x) <= 2e-3 * std::max(1.0, std::fabs(x)) && std::fabs(u[1] - y) <= 2e-3 * std::max(1.0, std::fabs(y)); };
            const char* cls = close(a0, a1) ? "uv1" : close(-a0, a1) ? "-u1,v1" : close(a0, -a1) ? "u1,-v1" : close(-a0, -a1) ? "-u1,-v1" : close(verts[i].uv[0], -verts[i].uv[1]) ? "u0,-v0" : close(-verts[i].uv[0], verts[i].uv[1]) ? "-u0,v0" : close(verts[i].uv[1], verts[i].uv[0]) ? "swap0" : "other";
            if (opt.probe && diag_count[2]++ < 40 && (diag_count[2] % 4 == 0))
              std::printf("DIAG uvclass %s slot %d part %d v %zu uv0 (%g %g) uv1 (%g %g) stream (%g %g)\n", cls, m.slot, m.part, i, verts[i].uv[0], verts[i].uv[1], a0, a1, u[0], u[1]);
            uv_class[std::string(cls) + " | " + m.effect + (m.lm ? " lm" : "") + (m.diffuse >= 0 ? "" : " nodiff")]++;
          }
          if (!uv_ok) uv_by_vs[std::string(vv.name) + " " + m.effect]++;
          // normal: 3 x 16-bit (signed unorm/snorm) after the reader's conversion
          if (en.source_format == 17 || en.source_format == 16) {
            int16_t q[3];
            std::memcpy(q, base + en.host_offset, 6);
            const bool sgn = en.flags & rdr_adapter::kElementSigned;
            double worst = 0;
            for (int c = 0; c < 3; ++c) {
              const double v = sgn ? double(q[c]) / 32767.0 : double(uint16_t(q[c])) / 65535.0 * 2.0 - 1.0;
              worst = std::max(worst, std::fabs(v - double(verts[i].normal[c])));
            }
            ++normal_checked;
            normal_bad += worst > 0.02;
            worst_normal = std::max(worst_normal, worst);
          }
          // colour: 4 bytes; find which source byte each of ours equals (a permutation, recorded as votes)
          if (ec.vk_format != 0) {
            uint8_t sc[4];
            std::memcpy(sc, base + ec.host_offset, 4);
            const uint8_t* oc = verts[i].color;
            ++color_checked;
            std::array<int, 4> perm = {-1, -1, -1, -1};
            bool unique = true;
            for (int k = 0; k < 4; ++k) {
              int found = -1, hits = 0;
              for (int s = 0; s < 4; ++s)
                if (oc[s] == sc[k]) found = s, ++hits;
              if (hits != 1) unique = false;
              perm[size_t(k)] = found;
            }
            // exact equality with the identity order (our byte k == stream byte k) is what the layout assumes
            const bool same = oc[0] == sc[0] && oc[1] == sc[1] && oc[2] == sc[2] && oc[3] == sc[3];
            if (!same) {
              ++color_bad;
              col_by_vs[std::string(vv.name) + " vk" + std::to_string(ec.vk_format)]++;
              if (opt.probe && diag_count[1]++ < 6)
                std::printf("DIAG colour off: frame %d draw %u slot %d part %d vertex %zu ours %02x%02x%02x%02x stream %02x%02x%02x%02x vk %u vs %s\n", f, d, m.slot, m.part, i, oc[0], oc[1], oc[2], oc[3], sc[0], sc[1], sc[2], sc[3], ec.vk_format, vv.name);
            }
            if (unique) color_perm_votes[perm]++;
            char note[64];
            std::snprintf(note, sizeof note, "vk %u src %u", ec.vk_format, ec.source_format);
            color_format_note.insert(note);
          } else {
            ;
          }
        }
      }
      for (int k = 0; k < 0; ++k) (void)k;
      if (rec && core.world_gap_slots().count(m.slot)) gap_slots.insert(m.slot);
    }
    if (f == opt.first) first_frame_unjoined = oracle_missed;
  }

  const ArenaCore::Stats& st = core.stats();
  std::printf("INFO  decide time: %llu calls, %.3f ms in total, %.4f ms a frame; pinned draws %llu, proof draws %llu\n", (unsigned long long)decide_calls, double(decide_ns) / 1e6, frames_run ? double(decide_ns) / 1e6 / double(frames_run) : 0.0, (unsigned long long)core.stats().pinned_draws, (unsigned long long)core.stats().proof_draws);
  std::printf("INFO  frames %llu, stage-site draws %llu, oracle-named %llu (original %llu, mirrored %llu), replaced %llu (original %llu, mirrored %llu), objects joined %zu\n",
              (unsigned long long)frames_run, (unsigned long long)stage_draws, (unsigned long long)oracle_draws, (unsigned long long)orig_oracle, (unsigned long long)mirror_oracle,
              (unsigned long long)replaced, (unsigned long long)orig_replaced, (unsigned long long)mirror_replaced, joined_objects.size());
  std::printf("INFO  %s\n", core.Report().c_str());
  std::printf("INFO  oracle-named draws kept because the stream image has another size than the file picture (kept, counted): %llu\n", (unsigned long long)oracle_size_kept);
  std::printf("INFO  oracle-named draws kept because our vertex lacks an input of the program: %llu\n", (unsigned long long)oracle_variant);
  for (const auto& [k, n] : variant_names) std::printf("INFO  kept for the vertex inputs x%llu: %s\n", (unsigned long long)n, k.c_str());
  for (const auto& [why, n] : kept_reasons) std::printf("INFO  kept %llu: %s\n", (unsigned long long)n, why.c_str());
  for (const auto& [perm, n] : color_perm_votes)
    std::printf("INFO  colour: our byte k == stream byte perm[k]: [%d %d %d %d] x %llu\n", perm[0], perm[1], perm[2], perm[3], (unsigned long long)n);
  for (const auto& s : color_format_note) std::printf("INFO  stream colour element: %s\n", s.c_str());
  for (const auto& [k, n] : neg_u_modes) std::printf("INFO  draws with a negative file u, slot-0 sampler: %s x%llu\n", k.c_str(), (unsigned long long)n);
  std::printf("INFO  vertices compared %llu: negative u in the file %llu, in the stream %llu; negative v in the file %llu, in the stream %llu\n", (unsigned long long)uv_checked, (unsigned long long)uv_file_neg_u, (unsigned long long)uv_stream_neg_u, (unsigned long long)uv_file_neg_v, (unsigned long long)uv_stream_neg_v);
  for (const auto& [k, n] : uv_class) std::printf("INFO  uv class x%llu: %s\n", (unsigned long long)n, k.c_str());
  for (const auto& [k, n] : uv_by_vs) std::printf("INFO  uv off x%llu: %s\n", (unsigned long long)n, k.c_str());
  for (const auto& [k, n] : col_by_vs) std::printf("INFO  colour off x%llu: %s\n", (unsigned long long)n, k.c_str());
  std::printf("INFO  skin rows: of %llu vertices, %llu have a bone id above 0, and for %llu of those the row c76 + 3 * id differs from row c76 in the draw's bank\n", (unsigned long long)skin_row_vertices, (unsigned long long)skin_id_nonzero, (unsigned long long)skin_row_differs);
  std::printf("INFO  skin: %llu bone-id draws replaced (%llu vertices) in slots:", (unsigned long long)skin_draws, (unsigned long long)skin_vertices);
  for (int s : skin_slots_replaced) std::printf(" %d", s);
  std::printf("\n");
  std::printf("INFO  placed slots with a world the record does not carry:");
  for (int s : gap_slots) std::printf(" %d", s);
  std::printf("\n");
  (void)first_frame_unjoined;
  (void)kept_stage_after_join;
  (void)part_eq_ordinal;

  Check("capture.joins_correct", wrong_join == 0 && st.bad_content == 0, "wrong joins %llu, content checks failed %llu", (unsigned long long)wrong_join, (unsigned long long)st.bad_content);
  Check("capture.oracle_all_replaced", oracle_draws > 0 && oracle_missed == 0, "oracle-named %llu, kept by the hook %llu", (unsigned long long)oracle_draws, (unsigned long long)oracle_missed);
  Check("capture.both_sites", orig_oracle > 0 && mirror_oracle > 0 && orig_replaced + var_orig == orig_oracle && mirror_replaced + var_mirror == mirror_oracle, "original %llu+%llu/%llu, mirrored %llu+%llu/%llu (replaced + kept for the vertex inputs / oracle)", (unsigned long long)orig_replaced,
        (unsigned long long)var_orig, (unsigned long long)orig_oracle, (unsigned long long)mirror_replaced, (unsigned long long)var_mirror, (unsigned long long)mirror_oracle);
  Check("capture.part_is_record_index", part_oracle > 0 && part_oracle_eq == part_oracle && part_checked > 0 && part_eq_record == part_checked,
        "oracle-named draws whose geometry names one part: %llu of %llu have origin.part == it; joined draws %llu of %llu", (unsigned long long)part_oracle_eq, (unsigned long long)part_oracle,
        (unsigned long long)part_eq_record, (unsigned long long)part_checked);
  Check("capture.mesh_space", flipped_y == 0, "%llu stage draws match a mesh only with its Y negated (the viewer-space fault)", (unsigned long long)flipped_y);
  Check("capture.draw_checks", st.bad_part + st.bad_vertices + st.bad_indices + st.bad_class == 0, "part %llu, vertices %llu, indices %llu, class %llu", (unsigned long long)st.bad_part,
        (unsigned long long)st.bad_vertices, (unsigned long long)st.bad_indices, (unsigned long long)st.bad_class);
  Check("capture.triangles_equal_stream", tri_checked > 0 && tri_bad == 0, "%llu draws compared, %llu differ (strip -> list, canonical rotation: the winding and the group rule)", (unsigned long long)tri_checked,
        (unsigned long long)tri_bad);
  Check("capture.uv_equal_half", uv_checked > 0 && uv_bad == 0, "%llu vertices, %llu off, worst %.5f", (unsigned long long)uv_checked, (unsigned long long)uv_bad, worst_uv);
  Check("capture.normal_equal_packed", normal_checked > 0 && normal_bad == 0, "%llu vertices, %llu off, worst %.4f", (unsigned long long)normal_checked, (unsigned long long)normal_bad, worst_normal);
  Check("capture.colour_bytes_equal", color_checked > 0 && color_bad == 0, "%llu vertices, %llu differ from the stream's bytes in our order", (unsigned long long)color_checked, (unsigned long long)color_bad);
  Check("capture.textures_ours", tex_checked > 0 && tex_bad == 0 && lm_set == 0, "%llu draws; wrong diffuse %llu; stream light-map slot replaced %llu", (unsigned long long)tex_checked, (unsigned long long)tex_bad,
        (unsigned long long)lm_set);
  Check("capture.texture_bytes_equal", opt.no_image_info || (tex_cmp > 0 && tex_cmp_bad == 0), "%llu (stream image, our picture) pairs compared at mip 0 (BC1/2/3), %llu differ; %llu draws had no comparable content", (unsigned long long)tex_cmp,
        (unsigned long long)tex_cmp_bad, (unsigned long long)tex_nocmp);
  if (opt.no_image_info)  // live today: no image records. The stream's pictures must stay; geometry is still ours.
    Check("capture.no_image_info_keeps_stream_image", replaced > 0 && st.size_unchecked > 0 && tex_slot_set == 0, "%llu draws replaced, %llu picture slots counted 'size unchecked', %llu of our pictures put in a slot (must be 0)", (unsigned long long)replaced,
          (unsigned long long)st.size_unchecked, (unsigned long long)tex_slot_set);
  else
    Check("capture.image_info_checked", st.size_unchecked == 0, "size unchecked %llu", (unsigned long long)st.size_unchecked);
  if (!opt.no_image_info) {  // S101 lane 47: the live supplier answers exactly what the capture reader answers, for every id (and past the table's end), and holds under a concurrent refresh
    uint64_t compared = 0, differ = 0;
    const uint32_t top = uint32_t(cap.images().size()) + 8;
    for (uint32_t id = 0; id <= top; ++id) {
      uint32_t lw = 0, lh = 0, cw = 0, ch = 0;
      const bool live_ok = live_table.Lookup(id, lw, lh);
      const rdr_adapter::ImageRecord* r = cap.FindImage(id);
      const bool cap_ok = r != nullptr;
      if (cap_ok) cw = r->width, ch = r->height;
      ++compared;
      if (live_ok != cap_ok || (live_ok && (lw != cw || lh != ch))) ++differ;
    }
    Check("capture.supplier_equals_capture", compared > 8 && differ == 0, "%llu image ids (0 .. records + 8) asked of the live supplier and of the capture reader; %llu answers differ", (unsigned long long)compared, (unsigned long long)differ);
    rdr_adapter::ImageInfoTable shared;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> bad_reads{0}, reads{0};
    const std::vector<rdr_adapter::ImageRecord> full = cap.images();
    shared.Set(full.data(), full.size());
    std::vector<std::thread> readers;
    for (int t = 0; t < 3; ++t)
      readers.emplace_back([&] {
        while (!stop.load()) {
          for (uint32_t id = 1; id <= full.size(); id += 7) {
            uint32_t w = 0, h = 0;
            ++reads;
            if (!shared.Lookup(id, w, h) || w != full[id - 1].width || h != full[id - 1].height) ++bad_reads;
          }
        }
      });
    // (the live thread resends the whole table whenever it grows or a record changes): refresh until 300 rounds are done AND the readers have really read during them (bounded)
    const auto t_start = std::chrono::steady_clock::now();
    int rounds_done = 0;
    while ((rounds_done < 300 || reads.load() < 20000) && std::chrono::steady_clock::now() - t_start < std::chrono::seconds(20)) {
      shared.Set(full.data(), full.size());
      ++rounds_done;
    }
    stop = true;
    for (std::thread& t : readers) t.join();
    Check("capture.supplier_threads", reads >= 20000 && bad_reads == 0, "%llu lookups on 3 threads during %d table refreshes; %llu wrong (a torn table)", (unsigned long long)reads.load(), rounds_done, (unsigned long long)bad_reads.load());
  }
  Check("capture.variant_props_replaced", oracle_variant == 0 && skin_draws > 0 && skin_no_words == 0, "oracle-named draws kept for the vertex inputs %llu (must be 0); %llu bone-id draws replaced in slots %zu, no skin words %llu", (unsigned long long)oracle_variant, (unsigned long long)skin_draws, skin_slots_replaced.size(), (unsigned long long)skin_no_words);
  Check("capture.skin_bone_ids_equal", skin_vertices > 0 && skin_bad_ids == 0, "%llu vertices of %llu replaced bone-id draws: bone-id word differs from the stream's %llu", (unsigned long long)skin_vertices, (unsigned long long)skin_draws, (unsigned long long)skin_bad_ids);
  Check("capture.skin_weights_equal", skin_vertices > 0 && skin_bad_weights == 0, "%llu vertices: weight word differs from the stream's %llu", (unsigned long long)skin_vertices, (unsigned long long)skin_bad_weights);
  Check("capture.skin_layout_by_name", skin_draws > 0 && skin_layout_bad == 0, "%llu draws: our layout (weights unorm8x4 at 36, ids uint8x4 at 40, stride 44) equals the stream's element formats; %llu differ", (unsigned long long)skin_draws, (unsigned long long)skin_layout_bad);
  Check("capture.record_gaps_listed", st.gap_slot == 0 && st.gap_twin == 0, "slot missing %llu, twin missing %llu, world differs %llu (slots %zu)", (unsigned long long)st.gap_slot, (unsigned long long)st.gap_twin,
        (unsigned long long)st.gap_world, gap_slots.size());
  std::printf("RESULT %s fault=%s frames=%llu replaced=%llu failed=%d\n", g_fail ? "FAIL" : "OK", opt.fault.empty() ? "none" : opt.fault.c_str(), (unsigned long long)frames_run, (unsigned long long)replaced, g_fail);
  return g_fail ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--game") opt.game = next();
    else if (a == "--capture") opt.capture = next();
    else if (a == "--fault") opt.fault = next();
    else if (a == "--files") opt.files = true;
    else if (a == "--probe") opt.probe = true;
    else if (a == "--threads") opt.threads = true;
    else if (a == "--no-image-info") opt.no_image_info = true;
    else if (a == "--live-style") opt.live_style = true;
    else if (a == "--corrupt-one") opt.corrupt_one = true;
    else if (a == "--supplier-fault") opt.supplier_fault = next();   // S101 lane 47
    else if (a == "--arena") opt.arenas.push_back(std::atoi(next()));
    else if (a == "--step") opt.step = std::max(1, std::atoi(next()));
    else if (a == "--frames") {
      if (std::sscanf(next(), "%d-%d", &opt.first, &opt.last) != 2) {
        std::fprintf(stderr, "bad --frames\n");
        return 3;
      }
    } else {
      std::fprintf(stderr, "arena_ours_test: unknown argument %s\n", a.c_str());
      return 3;
    }
  }
  if (opt.threads) return ThreadChecks(opt);
  if (opt.game.empty() || (!opt.files && opt.capture.empty())) {
    std::fprintf(stderr, "usage: arena_ours_test --game DIR (--files [--arena N] | --capture DIR [--frames A-B] [--step N] [--probe]) [--fault NAME]\n");
    return 3;
  }
  if (opt.files) {
    if (!assets::arena_gs::Init(opt.game)) {
      std::fprintf(stderr, "arena_ours_test: FAIL no game files at %s\n", opt.game.c_str());
      return 2;
    }
    return FilesChecks(opt);
  }
  return CaptureChecks(opt);
}

