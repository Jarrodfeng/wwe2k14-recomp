// screens_ours_test: the unit test of our stage screens (S101 lane 115 = SO; docs\renderer\UNCLAIMED_MODEL_PARTS.md 3.3). 0 game runs, no GPU.
//   screens_ours_test --game DIR --files [--arena N ...]
//   screens_ours_test --game DIR --capture DIR [--arena N] [--wrong-arena N] [--frames A-B] [--step N] [--fault NAME[,NAME]] [--live-style]
// The capture mode runs the SHIPPED join and per-draw checks (src\renderer_scene\screens_ours_core.cpp, the code ScreensOurs calls) on the pass-site-0x82C0F4BC model-part draws of a capture
// and checks them against an INDEPENDENT oracle (a brute-force bit-equal position match against every mesh of keys 800-809, ignoring the part):
//   - every draw the oracle names a screen mesh for is replaced; none is replaced that the oracle does not name
//   - the object's joined key is one the oracle names, and the origin's part is the mesh's record
//   - pairs: when several oracle keys have RAW-IDENTICAL records (the four streams and the index groups memcmp'd here, not through the writer) the lowest is the join ("ties stated")
//   - our vertex bytes and our index buffer equal the stream's captured bytes (memcmp here, not the core's own check)
//   - the keys joined over the run are in the stated set; per frame the screens replaced are those of the stream
// --fault NAME plants one of the faults inside the same code path; --wrong-arena N loads another arena's screens than the capture's. The run then must FAIL (screens_ours_selftest.py asserts which check).
// Exit: 0 all hold, 1 a check failed, 2 a loud failure (cannot read the input), 3 bad usage. Lines: "PASS name (...)" / "FAIL name (...)", then "RESULT ...".
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../src/assets/screen_game_space.h"
#include "../../src/renderer/shaders/spv/fx_registry.h"
#include "../../src/renderer_adapter/capture_reader.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/screens_ours_core.h"

namespace {
using namespace rdr_scene;
using rdr_adapter::CaptureReader;
using rdr_adapter::LoadedFrame;

int g_fail = 0;
void Check(const char* name, bool ok, const char* fmt = "", ...) {
  char detail[512] = {};
  if (fmt && *fmt) {
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
  }
  std::printf("%s  %s%s%s%s\n", ok ? "PASS" : "FAIL", name, detail[0] ? "  (" : "", detail, detail[0] ? ")" : "");
  if (!ok) ++g_fail;
}

int FindVariant(uint64_t hash) {
  const auto* first = std::begin(rdr::fx::kHashes);
  const auto* last = std::end(rdr::fx::kHashes);
  const auto* it = std::lower_bound(first, last, hash, [](const rdr::fx::HashEntry& e, uint64_t v) { return e.hash < v; });
  return it != last && it->hash == hash ? int(it->variant) : -1;
}

struct Options {
  std::string game, capture, fault;
  bool files = false, live_style = false, all_arenas = false, arena_switch = false;
  uint32_t only_object = 0;   // lane 115 F1: judge only this object (the ribbon band 0xAAE0B0F0 on a signed-u capture: its pair is the first thing the arena's u rule is learned from)
  int expect_uv = -1, expect_key = -1;
  int first = 0, last = -1, step = 1, arena = -1, wrong_arena = -1;
  std::vector<int> arenas;
};

using LiveStore = std::unordered_map<uint64_t, std::vector<uint8_t>>;
void BuildLiveStore(const CaptureReader& cap, const LoadedFrame& L, LiveStore& store) {
  store.clear();
  for (uint32_t d = 0; d < L.draws.count; ++d) {
    const rdr_adapter::Draw& dr = L.draws[d];
    if (dr.layout == 0xFFFFFFFFu || (dr.vertex_bytes == 0 && dr.index_bytes == 0) || (dr.flags & rdr_adapter::kDrawNoGeometry)) continue;
    std::vector<uint8_t> b;
    if (dr.vertex_bytes && cap.ReadGeometry(dr.vertex_offset, dr.vertex_bytes, b)) store.emplace(dr.vertex_offset, std::move(b));
    std::vector<uint8_t> ib;
    if (dr.index_bytes && cap.ReadGeometry(dr.index_offset, dr.index_bytes, ib)) store.emplace(dr.index_offset, std::move(ib));
  }
  for (uint32_t d = 0; d < L.draws.count; ++d) {
    const rdr_adapter::Draw& dr = L.draws[d];
    if (dr.layout == 0xFFFFFFFFu || dr.vertex_bytes == 0 || dr.vertex_count < 2 || dr.layout >= L.layouts.count) continue;
    store.emplace(dr.vertex_offset + uint64_t(dr.vertex_count - 1) * L.layouts[dr.layout].host_stride, std::vector<uint8_t>(L.layouts[dr.layout].host_stride, 0xAB));
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

// Two file records are RAW-IDENTICAL when their counts, the four streams and every index group are byte-equal (the pair question, answered without the writer).
bool RawIdentical(const assets::ring_gs::RingMesh& a, const assets::ring_gs::RingMesh& b) {
  const assets::jboy_parts::Part &p = a.data, &q = b.data;
  if (p.vertices != q.vertices || p.index_count != q.index_count || p.groups.size() != q.groups.size()) return false;
  for (int s = 0; s < 4; ++s) {
    if (p.stride[s] != q.stride[s]) return false;
    if (p.stride[s] && std::memcmp(p.s[s], q.s[s], size_t(p.stride[s]) * p.vertices) != 0) return false;
  }
  for (size_t g = 0; g < p.groups.size(); ++g)
    if (p.groups[g].count != q.groups[g].count || std::memcmp(p.groups[g].data, q.groups[g].data, 2ull * p.groups[g].count) != 0) return false;
  return true;
}

// Which streams differ between two records (for the files report): "positions", "skin", "uv", "tangent", "indices".
std::string DiffersIn(const assets::ring_gs::RingMesh& a, const assets::ring_gs::RingMesh& b) {
  const assets::jboy_parts::Part &p = a.data, &q = b.data;
  if (p.vertices != q.vertices) return "vertex counts";
  std::string r;
  const char* names[4] = {"S0(pos/normal/colour)", "S1(skin)", "S2(uv)", "S3(tangent)"};
  for (int s = 0; s < 4; ++s) {
    if (p.stride[s] != q.stride[s]) r += std::string(r.empty() ? "" : ", ") + names[s] + " stride";
    else if (p.stride[s] && std::memcmp(p.s[s], q.s[s], size_t(p.stride[s]) * p.vertices) != 0) r += std::string(r.empty() ? "" : ", ") + names[s];
  }
  bool idx = p.groups.size() != q.groups.size();
  for (size_t g = 0; !idx && g < p.groups.size(); ++g) idx = p.groups[g].count != q.groups[g].count || std::memcmp(p.groups[g].data, q.groups[g].data, 2ull * p.groups[g].count) != 0;
  if (idx) r += std::string(r.empty() ? "" : ", ") + "indices";
  return r.empty() ? "nothing (raw-identical)" : r;
}

// ---- files-only ---------------------------------------------------------------------------------------------------------------------
int FilesChecks(const Options& opt) {
  // S101 lane 115 F5: with --all-arenas every bg pac is read (arenas without screens, and pacs that do not exist, are INFO, never FAIL); otherwise the named arenas (default 4, 29), which must load
  std::vector<int> arenas = opt.arenas.empty() ? std::vector<int>{4, 29} : opt.arenas;
  if (opt.all_arenas) {
    arenas.clear();
    for (int a = 0; a < 100; ++a) arenas.push_back(a);
  }
  int with_screens = 0;
  for (int a : arenas) {
    assets::screen_gs::ScreenSet set;
    std::string err;
    if (!assets::screen_gs::Load(a, set, err)) {
      if (opt.all_arenas) {
        if (err.find("no screen entries") != std::string::npos) std::printf("INFO  arena %d: no screens (%s)\n", a, err.c_str());
        continue;   // (a number with no pac)
      }
      std::printf("FAIL  files.arena%d.load  (%s)\n", a, err.c_str());
      return 2;
    }
    ++with_screens;
    char tag[48];
    std::snprintf(tag, sizeof tag, "files.arena%d", a);
    size_t bad_index = 0, non_yBG = 0, tangent = 0, multi_pair = 0;
    std::map<int, std::vector<const assets::ring_gs::RingMesh*>> by_key;
    for (const assets::ring_gs::RingMesh& m : set.meshes) {
      by_key[m.entry].push_back(&m);
      bad_index += assets::ring_gs::BuildIndices(m).size() != m.StreamIndexCount();
      non_yBG += m.effect.compare(0, 10, "yBG_Defaul") != 0;
      tangent += m.data.stride[3] != 0;
      multi_pair += m.pairs > 1;
    }
    std::printf("INFO  arena %d: keys", a);
    for (const auto& k : by_key) {
      size_t neg = 0;
      for (uint32_t v = 0; v < k.second[0]->vertices; ++v) neg += assets::BeF(k.second[0]->data.s[2] + 8ull * v) < 0.0f;
      std::printf(" %d(%zu parts, %u vertices, effect %s, first negative u %d, %zu negative u)", k.first, k.second.size(), k.second[0]->vertices, k.second[0]->effect.c_str(), k.second[0]->first_neg_u, neg);
    }
    std::printf("\n");
    Check((std::string(tag) + ".has_screens").c_str(), !by_key.empty(), "%zu keys", by_key.size());
    Check((std::string(tag) + ".index_group_rule").c_str(), bad_index == 0, "%zu meshes whose built index buffer is not strip + (groups - 1)", bad_index);
    Check((std::string(tag) + ".effect_is_yBG_Default").c_str(), non_yBG == 0, "%zu meshes whose file effect is not yBG_Default* (the per-key exception: stream yMovieDefault)", non_yBG);
    std::printf("INFO  arena %d: meshes with a tangent stream %zu, with more than one skin pair %zu\n", a, tangent, multi_pair);
    // the pairs: do the members differ in any non-position byte?
    const int pairs[3][2] = {{801, 802}, {803, 804}, {806, 807}};
    for (const auto& pr : pairs) {
      const auto x = by_key.find(pr[0]), y = by_key.find(pr[1]);
      if (x == by_key.end() || y == by_key.end() || x->second.size() != y->second.size()) {
        std::printf("INFO  arena %d: pair %d/%d: one member is absent or has another part count\n", a, pr[0], pr[1]);
        continue;
      }
      for (size_t i = 0; i < x->second.size(); ++i) {
        const bool pos = x->second[i]->vertices == y->second[i]->vertices && std::memcmp(x->second[i]->data.s[0], y->second[i]->data.s[0], 28ull * x->second[i]->vertices) == 0;
        std::printf("INFO  arena %d: pair %d/%d part %zu: whole S0 equal %s; differs in: %s\n", a, pr[0], pr[1], i, pos ? "yes" : "no", DiffersIn(*x->second[i], *y->second[i]).c_str());
      }
    }
  }
  if (opt.all_arenas) std::printf("INFO  %d arenas have screens\n", with_screens);
  return g_fail ? 1 : 0;
}

// ---- capture mode -------------------------------------------------------------------------------------------------------------------
int ArenaOfCapture(CaptureReader& cap) {
  for (uint32_t f = 0; f < cap.frame_count(); ++f) {
    if (!cap.HasSceneRecord(f)) continue;
    SceneTables rec;
    std::string err;
    if (!ReadRecord(cap.SceneRecordPath(f).string(), rec, err)) continue;
    const int a = ScreensCore::ArenaOfRecord(rec);
    if (a >= 0) return a;
  }
  return -1;
}

int CaptureChecks(const Options& opt) {
  ScreensFaults faults;
  std::string ferr;
  if (!ScreensFaults::Parse(opt.fault, faults, ferr)) {
    std::fprintf(stderr, "screens_ours_test: %s\n", ferr.c_str());
    return 3;
  }
  CaptureReader cap;
  if (!cap.Open(opt.capture)) {
    std::fprintf(stderr, "screens_ours_test: FAIL cannot open the capture at %s\n", opt.capture.c_str());
    return 2;
  }
  if (!assets::screen_gs::Init(opt.game)) {
    std::fprintf(stderr, "screens_ours_test: FAIL no game files at %s\n", opt.game.c_str());
    return 2;
  }
  const int arena_id = opt.arena >= 0 ? opt.arena : ArenaOfCapture(cap);
  if (arena_id < 0) {
    std::fprintf(stderr, "screens_ours_test: FAIL no record in %s names an arena pac (pass --arena N)\n", opt.capture.c_str());
    return 2;
  }
  const int load_arena = opt.wrong_arena >= 0 ? opt.wrong_arena : arena_id;   // the planted wrong_arena: another arena's screens
  auto set = std::make_shared<assets::screen_gs::ScreenSet>();
  std::string err;
  if (!assets::screen_gs::Load(load_arena, *set, err)) {
    std::fprintf(stderr, "screens_ours_test: FAIL arena %d: %s\n", load_arena, err.c_str());
    return 2;
  }
  const int nframes = int(cap.frame_count());
  const int last = opt.last < 0 || opt.last >= nframes ? nframes - 1 : opt.last;
  ScreensOursOptions o;
  o.game_root = opt.game;
  o.faults = faults;
  o.verify_all = !opt.live_style;
  ScreensCore core(o);
  core.SetScreens(set);
  std::shared_ptr<const assets::screen_gs::ScreenSet> cur = set;   // the set the core and the oracle hold now (arena_switch changes it)
  auto other = std::make_shared<assets::screen_gs::ScreenSet>();   // arena 29's screens (803 / 804 only): "another arena", for the clock probe and the switch
  {
    std::string e2;
    if (!assets::screen_gs::Load(arena_id == 29 ? 4 : 29, *other, e2)) {
      std::fprintf(stderr, "screens_ours_test: FAIL the other arena's screens: %s\n", e2.c_str());
      return 2;
    }
  }
  bool clock_done = false, clock_ok = false;   // lane 115 F2
  uint64_t phase_replaced[3] = {0, 0, 0}, phase_mismatch[3] = {0, 0, 0};
  int phase = 0;
  bool switch_clean = true;
  std::string switch_note;

  uint64_t site_draws = 0, oracle_draws = 0, replaced = 0, missed = 0, spurious = 0, wrong_key = 0, wrong_part = 0, pair_wrong = 0, vertex_bad = 0, index_bad = 0, vertex_cmp = 0, mismatch = 0, unjoined = 0, tied_objects = 0;
  std::map<std::string, uint64_t> kept, programs;
  std::set<int> keys_seen;
  std::map<uint32_t, std::set<int>> object_keys;
  std::map<int, uint32_t> per_frame;
  uint64_t frames_run = 0;
  LoadedFrame L;
  for (int f = 0; f <= last; ++f) {
    if (!cap.LoadFrame(uint32_t(f), L)) {
      std::fprintf(stderr, "screens_ours_test: FAIL cannot load frame %d\n", f);
      return 2;
    }
    if (f < opt.first || (f - opt.first) % opt.step) continue;
    if (opt.arena_switch) {   // A, B, A over three equal parts; right after each switch the core must hold NOTHING of the arena before: no joins, no buffers, the u rule unknown
      const int span = std::max(3, last - opt.first + 1), now = std::min(2, (f - opt.first) * 3 / span);
      if (now != phase) {
        phase = now;
        cur = phase == 1 ? std::shared_ptr<const assets::screen_gs::ScreenSet>(other) : std::shared_ptr<const assets::screen_gs::ScreenSet>(set);
        core.SetScreens(cur);
        const bool clean = core.prepared_count() == 0 && core.uv_mode() == 0 && core.KeyOfObject(0xAADFC4F0u) == -1 && core.KeyOfObject(0xAAE0B0F0u) == -1;
        if (!clean) switch_clean = false, switch_note = "after the switch to part " + std::to_string(phase) + " the core still held state (prepared " + std::to_string(core.prepared_count()) + ", u rule " + std::to_string(core.uv_mode()) + ")";
      }
    }
    core.ResetFrame();
    ++frames_run;
    LiveStore live;
    if (opt.live_style) BuildLiveStore(cap, L, live);
    uint32_t count = 0;
    for (uint32_t d = 0; d < L.draws.count; ++d) {
      const rdr_adapter::DrawOrigin* origin = L.OriginOf(d);
      if (!origin || origin->klass != rdr_adapter::DrawClass::ModelPart || origin->pass_site != kModelPartScreensSite) continue;
      if (opt.only_object && origin->object != opt.only_object) continue;
      const rdr_adapter::Draw& dr = L.draws[d];
      if ((dr.flags & rdr_adapter::kDrawNoGeometry) || dr.layout >= L.layouts.count) continue;
      const rdr_adapter::VertexLayout& lay = L.layouts[dr.layout];
      const int vs = FindVariant(dr.vs_hash), ps = dr.ps_hash ? FindVariant(dr.ps_hash) : -1;
      if (vs < 0) continue;
      ++site_draws;
      ++programs[std::string(rdr::fx::kVariants[vs].name) + (ps >= 0 ? std::string(" + ") + rdr::fx::kVariants[ps].name : std::string())];
      // ---- the independent oracle: every mesh of keys 800-809 whose counts and positions equal the stream's ----
      std::vector<uint8_t> vb;
      std::set<int> oracle_keys;
      uint32_t pos_off = 0;
      bool have_pos = false;
      for (uint32_t e = 0; e < lay.element_count; ++e) {
        const rdr_adapter::VertexElement& el = L.elements[lay.element_first + e];
        if (el.source_format == 38 && el.guest_offset == 0 && el.vk_format == 109) pos_off = el.host_offset, have_pos = true;
      }
      std::map<int, const assets::ring_gs::RingMesh*> oracle_mesh;
      if (have_pos && cap.ReadGeometry(dr.vertex_offset, dr.vertex_count * lay.host_stride, vb)) {
        for (const assets::ring_gs::RingMesh& m : cur->meshes) {
          if (m.vertices != dr.vertex_count || m.StreamIndexCount() != dr.index_count) continue;
          bool all = true;
          for (uint32_t v = 0; v < m.vertices && all; ++v) {
            float a[3], b[3];
            std::memcpy(a, vb.data() + size_t(v) * lay.host_stride + pos_off, 12);
            for (int c = 0; c < 3; ++c) b[c] = assets::BeF(m.data.s[0] + 28ull * v + 4 * c);
            all = std::memcmp(a, b, 12) == 0;
          }
          if (all) oracle_keys.insert(m.entry), oracle_mesh.emplace(m.entry, &m);
        }
      }
      if (!oracle_keys.empty()) ++oracle_draws;
      if (!core.Claims(*origin)) {
        if (!oracle_keys.empty()) ++missed;
        continue;
      }
      rdr_adapter::ReplaceIn in;
      in.frame = &L;
      in.draw = &dr;
      in.draw_index = d;
      in.origin = origin;
      in.vs = vs, in.ps = ps;
      in.vertex_count = dr.vertex_count, in.index_count = dr.index_count;
      in.vertex_offset = dr.vertex_offset, in.index_offset = dr.index_offset;
      in.vertex_stride = lay.host_stride;
      in.stream_layout = 0;
      in.stream_topology = 5;
      in.stream_index_32bit = dr.index_32bit;
      if (opt.live_style) in.read_geometry = [&live](uint64_t key, uint32_t bytes, std::vector<uint8_t>& out) { return ReadLive(live, key, bytes, out); };
      else in.read_geometry = [&cap](uint64_t off, uint32_t bytes, std::vector<uint8_t>& out) { return cap.ReadGeometry(off, bytes, out); };
      if (!clock_done) {   // lane 115 F2 (frozen-clock plant): a FRESH core holding another arena's screens judges this draw foreign; then only NoClaimFrame-style frames pass: after 300 of them the verdict must be gone (Claims true again)
        ScreensCore probe(o);
        probe.SetScreens(other);
        probe.ResetFrame();
        (void)probe.Decide(in, ps >= 0 ? rdr::fx::kVariants[ps].name : nullptr);
        if (!probe.Claims(*origin)) {
          clock_done = true;
          for (int i = 0; i < 310; ++i) probe.NoClaimFrame();
          clock_ok = probe.Claims(*origin);
        }
      }
      const ScreensCore::Decision dec = core.Decide(in, ps >= 0 ? rdr::fx::kVariants[ps].name : nullptr);
      if (!dec.ok) {
        ++kept[std::string(rdr_adapter::KeepWhyName(dec.why)) + ": " + dec.reason];
        mismatch += dec.why == rdr_adapter::KeepWhy::Mismatch;
        phase_mismatch[phase] += dec.why == rdr_adapter::KeepWhy::Mismatch;
        unjoined += dec.why == rdr_adapter::KeepWhy::Unjoined;
        if (!oracle_keys.empty()) ++missed;
        continue;
      }
      ++replaced, ++count;
      const assets::ring_gs::RingMesh& m = cur->meshes[size_t(dec.mesh)];
      ++phase_replaced[phase];
      keys_seen.insert(m.entry);
      object_keys[origin->object].insert(m.entry);
      if (oracle_keys.empty()) ++spurious;
      if (!oracle_keys.count(m.entry)) ++wrong_key;
      if (int(origin->part) != m.part) ++wrong_part;
      if (oracle_keys.size() > 1) {   // pairs stated: the lowest key of the raw-identical members (if the members' records differ the stream's bytes decide, checked below)
        ++tied_objects;
        for (const auto& om : oracle_mesh)
          if (om.first < m.entry && RawIdentical(*om.second, m)) {
            ++pair_wrong;
            break;
          }
      }
      ++vertex_cmp;
      if (vb.size() < dec.prepared->vertex.size() || std::memcmp(vb.data(), dec.prepared->vertex.data(), dec.prepared->vertex.size()) != 0) ++vertex_bad;
      std::vector<uint8_t> ib;
      if (!cap.ReadGeometry(dr.index_offset, dr.index_count * 2, ib) || ib.size() != dec.prepared->index.size() * 2 || std::memcmp(ib.data(), dec.prepared->index.data(), ib.size()) != 0) ++index_bad;
    }
    per_frame[f] = count;
  }
  std::printf("INFO  arena %d%s: %llu frames; %llu screen-site draws, %llu named by the oracle, %llu replaced; keys joined: ", arena_id, opt.wrong_arena >= 0 ? " (screens of another arena loaded)" : "", (unsigned long long)frames_run,
              (unsigned long long)site_draws, (unsigned long long)oracle_draws, (unsigned long long)replaced);
  for (int k : keys_seen) std::printf("%d ", k);
  std::printf("\n");
  for (const auto& k : programs) std::printf("INFO  stream program %-80s %llu draws\n", k.first.c_str(), (unsigned long long)k.second);
  for (const auto& k : kept) std::printf("INFO  kept %-50s %llu\n", k.first.c_str(), (unsigned long long)k.second);
  const ScreensCore::Stats& st = core.stats();
  std::printf("INFO  %s\n", core.Report().c_str());
  uint32_t most = 0;
  for (const auto& pf : per_frame) most = std::max(most, pf.second);
  const auto f200 = per_frame.find(200);
  // the stated set: 800-809 members the stream can draw; the acceptance joins are {801|802, 803|804, 805, 806|807}
  bool stated = true;
  for (int k : keys_seen) stated = stated && std::find(cur->keys.begin(), cur->keys.end(), k) != cur->keys.end();   // lane 115 F5: the file's own key set (800-809 exist in many arenas), not a hard-coded 801-807
  Check("capture.screens_replaced", replaced > 0 && oracle_draws > 0, "%llu replaced of %llu oracle-named draws", (unsigned long long)replaced, (unsigned long long)oracle_draws);
  Check("capture.oracle_all_replaced", missed == 0, "%llu oracle-named draws not replaced", (unsigned long long)missed);
  Check("capture.no_spurious", spurious == 0, "%llu replaced draws the oracle does not name", (unsigned long long)spurious);
  Check("capture.joins_correct", wrong_key == 0 && wrong_part == 0 && pair_wrong == 0 && stated, "%llu draws joined to a key the oracle does not name, %llu with a part that is not the origin's, %llu pair draws joined above a raw-identical lower key; keys in the file's set: %s", (unsigned long long)wrong_key,
        (unsigned long long)wrong_part, (unsigned long long)pair_wrong, stated ? "yes" : "NO");
  Check("capture.vertex_bytes_equal_stream", vertex_cmp > 0 && vertex_bad == 0, "%llu of %llu replaced draws' vertex buffers differ from the stream's bytes", (unsigned long long)vertex_bad, (unsigned long long)vertex_cmp);
  Check("capture.index_bytes_equal_stream", vertex_cmp > 0 && index_bad == 0, "%llu of %llu replaced draws' index buffers differ from the stream's", (unsigned long long)index_bad, (unsigned long long)vertex_cmp);
  Check("capture.draws_per_frame", most > 0 && (f200 == per_frame.end() || opt.arena_switch || f200->second == (opt.only_object ? 1u : 4u)), "most screens replaced in a frame %u; frame 200: %s", most, f200 == per_frame.end() ? "not run" : (std::to_string(f200->second) + " draws").c_str());
  Check("capture.draw_checks", mismatch == 0 && unjoined == 0 && st.joins_dropped == 0 && st.bad_bytes == 0 && st.bad_class == 0, "mismatch %llu, unjoined %llu, joins dropped %llu, byte checks failed %llu, class refused %llu", (unsigned long long)mismatch, (unsigned long long)unjoined,
        (unsigned long long)st.joins_dropped, (unsigned long long)st.bad_bytes, (unsigned long long)st.bad_class);
  Check("clock.foreign_expires", clock_done && clock_ok, "%s", !clock_done ? "no foreign verdict could be provoked" : clock_ok ? "a foreign verdict expired after 310 frames in which nothing was claimed (NoClaimFrame only)" : "a foreign verdict never expired: the clock does not run on frames where nothing is claimed");   // lane 115 F2
  Check("class.refused_counted", (!faults.class_refuse && !faults.file_effect) || (replaced == 0 && st.bad_class > 0), "class refused %llu, replaced %llu (class_refuse / file_effect: every draw kept and the refusal counted)", (unsigned long long)st.bad_class, (unsigned long long)replaced);
  if (opt.arena_switch) {
    Check("switch.state_rebuilt", switch_clean && phase_replaced[0] > 0 && phase_replaced[1] == 0 && phase_mismatch[1] == 0 && phase_replaced[2] > 0 && phase_mismatch[2] == 0,
          "A -> B -> A: replaced %llu / %llu / %llu, mismatch %llu / %llu / %llu; %s", (unsigned long long)phase_replaced[0], (unsigned long long)phase_replaced[1], (unsigned long long)phase_replaced[2], (unsigned long long)phase_mismatch[0],
          (unsigned long long)phase_mismatch[1], (unsigned long long)phase_mismatch[2], switch_note.empty() ? "the core held nothing of the arena before after each switch" : switch_note.c_str());
  }
  if (opt.expect_uv >= 0) Check("expect.uv_rule", core.uv_mode() == opt.expect_uv, "u rule %d, wanted %d", core.uv_mode(), opt.expect_uv);
  if (opt.expect_key >= 0) Check("expect.key", keys_seen.size() == 1 && *keys_seen.begin() == opt.expect_key, "keys joined %zu (first %d), wanted only %d", keys_seen.size(), keys_seen.empty() ? -1 : *keys_seen.begin(), opt.expect_key);
  if (opt.live_style) Check("capture.live_reads_first_use_only", st.verify_runs < replaced, "%llu byte verifications for %llu replaced draws", (unsigned long long)st.verify_runs, (unsigned long long)replaced);
  std::printf("INFO  pair objects (several keys with equal positions): %llu draws; object -> key: ", (unsigned long long)tied_objects);
  for (const auto& ok : object_keys) {
    std::printf("0x%08X=", ok.first);
    for (int k : ok.second) std::printf("%d%s", k, "/");
    std::printf(" ");
  }
  std::printf("\n");
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
    else if (a == "--live-style") opt.live_style = true;
    else if (a == "--all-arenas") opt.all_arenas = true;
    else if (a == "--arena-switch") opt.arena_switch = true;   // lane 115: the screens of arena A, then B (29), then A again over three equal parts of the frame range
    else if (a == "--only-object") opt.only_object = uint32_t(std::strtoul(next(), nullptr, 0));
    else if (a == "--expect-uv") opt.expect_uv = std::atoi(next());
    else if (a == "--expect-key") opt.expect_key = std::atoi(next());
    else if (a == "--step") opt.step = std::max(1, std::atoi(next()));
    else if (a == "--arena") {
      opt.arena = std::atoi(next());
      opt.arenas.push_back(opt.arena);
    } else if (a == "--wrong-arena") opt.wrong_arena = std::atoi(next());
    else if (a == "--frames") {
      int x = 0, y = -1;
      if (std::sscanf(next(), "%d-%d", &x, &y) >= 1) opt.first = x, opt.last = y;
    } else {
      std::fprintf(stderr, "screens_ours_test: unknown argument %s\n", a.c_str());
      return 3;
    }
  }
  if (opt.game.empty() || (!opt.files && opt.capture.empty())) {
    std::fprintf(stderr, "usage: screens_ours_test --game DIR (--files [--arena N] | --capture DIR [--arena N] [--wrong-arena N] [--frames A-B] [--step N] [--fault NAME[,NAME]] [--live-style])\n");
    return 3;
  }
  ScreensFaults probe;
  std::string ferr;
  if (!ScreensFaults::Parse(opt.fault, probe, ferr)) {
    std::fprintf(stderr, "screens_ours_test: %s\n", ferr.c_str());
    return 3;
  }
  if (!assets::screen_gs::Init(opt.game)) {
    std::fprintf(stderr, "screens_ours_test: FAIL no game files at %s\n", opt.game.c_str());
    return 2;
  }
  return opt.files ? FilesChecks(opt) : CaptureChecks(opt);
}
