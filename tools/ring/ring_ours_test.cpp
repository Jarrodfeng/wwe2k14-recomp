// ring_ours_test: the unit test of our ring (Session 101, lane 70 = RO; docs\renderer\RING_PROMOTION_MAP.md sections 2-5). 0 game runs, no GPU.
//   ring_ours_test --game DIR --files [--arena N ...]                         the files-only checks (arenas 4 and 29 by default)
//   ring_ours_test --game DIR --capture DIR [--arena N] [--frames A-B] [--step N] [--fault NAME[,NAME]] [--live-style]
// The capture mode runs the SHIPPED join and per-draw checks (src\renderer_scene\ring_ours_core.cpp, the code RingOurs calls) on the colour-site model-part draws of a capture and checks
// them against an INDEPENDENT oracle (a brute-force bit-equal position match against every mesh of every ring entry, ignoring the part and the counts):
//   - every draw the oracle names a ring mesh for is replaced; none is replaced that the oracle does not name
//   - the object's joined entry is one the oracle names for every draw it replaced, and the origin's part is the mesh's record
//   - our vertex bytes and our index buffer equal the stream's captured bytes (written here by memcmp, not by the core's own check)
//   - the file texture put into a slot equals the stream image's mip 0 byte for byte
//   - instances per piece per frame (post 4, turnbuckle 12, canvas 1, cover 12, rope 12, skirt 3)
// --fault NAME plants one of the map's faults inside the same code path; the run then must FAIL (tools\ring\ring_ours_selftest.py asserts which check).
// --live-style: the geometry reads go through a key store with decoys (the live path: no capture-offset arithmetic) and the core verifies the first use of each blob key only.
// Exit: 0 all hold, 1 a check failed, 2 a loud failure (cannot read the input), 3 bad usage. Lines: "PASS name (...)" / "FAIL name (...)", then "RESULT ...".
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../src/assets/ring_game_space.h"
#include "../../src/renderer/shaders/spv/fx_registry.h"
#include "../../src/renderer_adapter/capture_reader.h"
#include "../../src/renderer_adapter/image_content_table.h"
#include "../../src/renderer_scene/proof_frames.h"
#include "../../src/renderer_scene/ring_hash.h"
#include "../../src/renderer_scene/ring_ours_core.h"
#include "../../src/renderer_scene/record_io.h"

namespace {
using namespace rdr_scene;
using rdr_adapter::CaptureReader;
using rdr_adapter::LoadedFrame;

int g_fail = 0;
std::string g_mip_note, g_levels_note;
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

size_t MipBytes(assets::TexFormat f, uint32_t w, uint32_t h) {
  switch (f) {
    case assets::TexFormat::kBC1: return size_t(std::max(1u, (w + 3) / 4)) * std::max(1u, (h + 3) / 4) * 8;
    case assets::TexFormat::kBC2:
    case assets::TexFormat::kBC3: return size_t(std::max(1u, (w + 3) / 4)) * std::max(1u, (h + 3) / 4) * 16;
    case assets::TexFormat::kB4G4R4A4: return size_t(w) * h * 2;
    default: return size_t(w) * h * 4;
  }
}

struct Options {
  std::string game, capture, fault;
  bool files = false, live_style = false, no_content = false, content_fault = false, expect_proxy = false, retag_canvas = false;   // lane 137: expect_proxy
  int first = 0, last = -1, step = 1, arena = -1;
  int corrupt_at = -1;   // S101 lane 88 (with --live-style): from the frame after F on, the vertex blob of the first replaced draw of frame F reads back with its first vertex inverted (corrupted in place after the pin)
  std::vector<int> arenas;
};

// The live store (see arena_ours_test.cpp): blobs by KEY with decoys at key + (n - 1) * stride.
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

// ---- files-only ---------------------------------------------------------------------------------------------------------------------
int FilesChecks(const Options& opt) {
  const std::vector<int> arenas = opt.arenas.empty() ? std::vector<int>{4, 29} : opt.arenas;
  for (int a : arenas) {
    assets::ring_gs::RingArena ring;
    std::string err;
    if (!assets::ring_gs::Load(a, ring, err)) {
      std::printf("FAIL  files.arena%d.load  (%s)\n", a, err.c_str());
      return 2;
    }
    char tag[64], n[96];
    std::snprintf(tag, sizeof tag, "files.arena%d", a);
    std::set<int> entries;
    size_t bad_index = 0, pair2 = 0, no_diffuse = 0, rope_pair_bad = 0;
    for (const assets::ring_gs::RingMesh& m : ring.meshes) {
      entries.insert(m.entry);
      const std::vector<uint16_t> idx = assets::ring_gs::BuildIndices(m);
      bad_index += idx.size() != m.StreamIndexCount();
      pair2 += m.pairs == 2;
      rope_pair_bad += (m.pairs == 2) != (m.entry == 960);
      no_diffuse += m.diffuse < 0;
    }
    std::snprintf(n, sizeof n, "%s.loaded", tag);
    Check(n, entries.size() == 11 && ring.Find(951, 0) && ring.Find(960, 1) && ring.Find(980, 0) && !ring.textures.empty(), "%zu entries, %zu meshes, %zu textures, %.1f MB", entries.size(), ring.meshes.size(), ring.textures.size(),
          double(ring.Bytes()) / 1048576.0);
    std::snprintf(n, sizeof n, "%s.index_group_rule", tag);
    Check(n, bad_index == 0, "%zu meshes whose built index buffer is not strip + (groups - 1)", bad_index);
    std::snprintf(n, sizeof n, "%s.two_pair_rope_only", tag);
    Check(n, pair2 == 2 && rope_pair_bad == 0, "%zu meshes with two skin pairs a vertex (the high rope 960's two); mismatches %zu", pair2, rope_pair_bad);
    std::printf("INFO  %s  meshes without a diffuse: %zu\n", tag, no_diffuse);
    const assets::ring_gs::RingMesh* post = ring.Find(951, 0);
    const assets::ring_gs::RingMesh* lowpost = ring.Find(981, 0);
    std::snprintf(n, sizeof n, "%s.post_tie_on_mesh0", tag);
    Check(n, post && lowpost && post->vertices == 40 && lowpost->vertices == 40 && ring.Find(951, 1) && ring.Find(981, 1) && ring.Find(951, 1)->vertices != ring.Find(981, 1)->vertices,
          "951 / 981 mesh 0: %u / %u vertices, mesh 1: %u / %u", post ? post->vertices : 0, lowpost ? lowpost->vertices : 0, ring.Find(951, 1) ? ring.Find(951, 1)->vertices : 0, ring.Find(981, 1) ? ring.Find(981, 1)->vertices : 0);
  }
  return g_fail ? 1 : 0;
}

// ---- capture mode -------------------------------------------------------------------------------------------------------------------
int ArenaOfCapture(CaptureReader& cap) {
  for (uint32_t f = 0; f < cap.frame_count(); ++f) {
    if (!cap.HasSceneRecord(f)) continue;
    SceneTables rec;
    std::string err;
    if (!ReadRecord(cap.SceneRecordPath(f).string(), rec, err)) continue;
    const int a = RingCore::ArenaOfRecord(rec);
    if (a >= 0) return a;
  }
  return -1;
}

const char* PieceOf(int entry) {
  switch (entry) {
    case 951: case 981: return "post";
    case 952: return "turnbuckle";
    case 953: return "canvas";
    case 964: case 982: return "cover";
    case 960: case 980: return "rope";
    case 973: case 974: return "skirt";
    case 956: return "proxy";   // lane 137: the rope plane-shadow proxy, drawn at the T_Lambert site on WM30
    default: return "other";
  }
}

int CaptureChecks(const Options& opt) {
  RingFaults faults;
  std::string ferr;
  if (!RingFaults::Parse(opt.fault, faults, ferr)) {
    std::fprintf(stderr, "ring_ours_test: %s\n", ferr.c_str());
    return 3;
  }
  CaptureReader cap;
  if (!cap.Open(opt.capture)) {
    std::fprintf(stderr, "ring_ours_test: FAIL cannot open the capture at %s\n", opt.capture.c_str());
    return 2;
  }
  const int arena_id = opt.arena >= 0 ? opt.arena : ArenaOfCapture(cap);
  if (arena_id < 0) {
    std::fprintf(stderr, "ring_ours_test: FAIL no record in %s names an arena pac (pass --arena N)\n", opt.capture.c_str());
    return 2;
  }
  if (!assets::ring_gs::Init(opt.game)) {
    std::fprintf(stderr, "ring_ours_test: FAIL no game files at %s\n", opt.game.c_str());
    return 2;
  }
  auto ring = std::make_shared<assets::ring_gs::RingArena>();
  std::string err;
  if (!assets::ring_gs::Load(arena_id, *ring, err)) {
    std::fprintf(stderr, "ring_ours_test: FAIL arena %d: %s\n", arena_id, err.c_str());
    return 2;
  }
  const int nframes = int(cap.frame_count());
  const int last = opt.last < 0 || opt.last >= nframes ? nframes - 1 : opt.last;
  std::map<uint32_t, rdr_adapter::ContentRef> contents;
  // The stream image behind a record id against a file texture, byte for byte. all_mips false: mip 0 only (1 equal, 0 differ). all_mips true: 1 every mip equal, 2 only mips of at most 8 texels
  // (never mip 0) differ (the game's small-mip layout, ring.md B2.3 "tail"), 0 anything else. -1: cannot tell.
  const auto texture_cmp = [&](uint32_t image, const assets::Texture& t, bool all_mips, uint32_t our_mips = 0) -> int {
    const auto c = contents.find(image);
    if (c == contents.end()) return -1;
    rdr_adapter::LoadedTexture lt;
    if (!cap.ReadTexture(c->second, lt)) return -1;
    size_t file_off = 0;
    std::vector<size_t> offs;
    for (uint32_t m = 0; m < std::max(1u, t.mips); ++m) offs.push_back(file_off), file_off += MipBytes(t.format, std::max(1u, t.width >> m), std::max(1u, t.height >> m));
    bool seen0 = false, tail = false;
    uint32_t stream_mips = 0;
    for (uint32_t s = 0; s < lt.header.subresource_count; ++s) {
      const rdr_adapter::Subresource& sr = lt.subresources[s];
      if (sr.layer != 0) continue;
      ++stream_mips;
      if (sr.mip == 0) {
        seen0 = true;
        if (sr.width != t.width || sr.height != t.height) return 0;
      }
      if (sr.mip != 0 && !all_mips) continue;
      if (sr.mip >= offs.size()) return 0;
      const size_t n = size_t(sr.row_bytes) * sr.rows;
      if (n != MipBytes(t.format, std::max(1u, t.width >> sr.mip), std::max(1u, t.height >> sr.mip)) || offs[sr.mip] + n > t.data.size()) return 0;
      if (std::memcmp(lt.data + sr.offset, t.data.data() + offs[sr.mip], n) != 0) {
        if (sr.mip == 0 || std::max(sr.width, sr.height) > 8) return 0;
        tail = true;
      }
    }
    if (!seen0) return -1;
    if (all_mips && stream_mips != our_mips) return g_mip_note = std::to_string(t.width) + "x" + std::to_string(t.height) + " stream " + std::to_string(stream_mips) + " file " + std::to_string(t.mips) + " record mips " + std::to_string(cap.FindImage(image) ? cap.FindImage(image)->mips : 0) + " slot min/max " + g_levels_note, 3;   // the stream image holds a different number of mips than the file's chain
    return tail ? 2 : 1;
  };
  const auto texture_equal = [&](uint32_t image, const assets::Texture& t) -> int { return texture_cmp(image, t, false); };
  RingOursOptions o;
  o.game_root = opt.game;
  o.faults = faults;
  o.verify_all = !opt.live_style;
  // F1: the image-content feed as the live feeds fill it: size, the record's mip count, xxh3-64 of the layer-0 mips it holds in order, a generation per content blob.
  std::map<std::pair<uint32_t, uint64_t>, rdr_adapter::ImageContent> content_cache;
  if (!opt.no_content)
    o.image_content = [&](uint32_t image, rdr_adapter::ImageContent& out) {
      const auto c = contents.find(image);
      const rdr_adapter::ImageRecord* rec = cap.FindImage(image);
      if (c == contents.end() || !rec) return false;
      const auto key = std::make_pair(image, c->second.offset);
      const auto hit = content_cache.find(key);
      if (hit != content_cache.end()) return out = hit->second, true;
      rdr_adapter::LoadedTexture lt;
      if (!cap.ReadTexture(c->second, lt)) return false;
      std::vector<uint8_t> all;
      rdr_adapter::ImageContent ic;
      for (uint32_t s = 0; s < lt.header.subresource_count; ++s) {
        const rdr_adapter::Subresource& sr = lt.subresources[s];
        if (sr.layer != 0) continue;
        if (sr.mip == 0) ic.width = sr.width, ic.height = sr.height;
        all.insert(all.end(), lt.data + sr.offset, lt.data + sr.offset + size_t(sr.row_bytes) * sr.rows);
      }
      ic.format = rec->format, ic.mips = rec->mips, ic.hash = rdr_scene::RingXxh3(all.data(), all.size()) ^ (opt.content_fault ? 1u : 0u), ic.generation = uint32_t(c->second.offset & 0xFFFFFF) + 1;
      content_cache[key] = ic;
      out = ic;
      return true;
    };
  RingCore core(o);
  bool clock_done = false, clock_ok = false;   // lane 115 F2
  core.SetArena(ring, arena_id);

  uint64_t ring_draws = 0, oracle_draws = 0, replaced = 0, missed = 0, spurious = 0, wrong_entry = 0, wrong_part = 0, vertex_bad = 0, index_bad = 0, vertex_cmp = 0;
  uint64_t tex_cmp = 0, tex_bad = 0, tex_nocmp = 0, mismatch = 0, unjoined = 0, variant = 0;
  std::map<std::string, uint64_t> kept, tex_by_piece;
  uint64_t frames_run = 0, inst_checked = 0, inst_bad = 0;
  uint64_t corrupt_key = 0, corrupt_stride = 0, bad_before = 0, decide_ns = 0, decide_calls = 0;
  bool corrupt_set = false;
  int corrupt_found = -1;   // frames after the corruption in which the core first failed a byte check (-1 never)
  std::string inst_note;
  std::set<int> entries_replaced;
  std::set<uint32_t> objects;
  uint64_t rope_high = 0, rope_low = 0, proxy_replaced = 0, proxy_lambert = 0, proxy_bad_frames = 0, retag_total = 0, retag_ours = 0;   // lane 137
  std::map<std::string, std::set<int>> expect_inst;   // unused placeholder for symmetry
  (void)expect_inst;
  const std::map<std::string, uint32_t> want_inst = {{"post", 4}, {"turnbuckle", 12}, {"canvas", 1}, {"cover", 12}, {"rope", 12}, {"skirt", 3}};
  LoadedFrame L;
  for (int f = 0; f <= last; ++f) {
    if (!cap.LoadFrame(uint32_t(f), L)) {
      std::fprintf(stderr, "ring_ours_test: FAIL cannot load frame %d\n", f);
      return 2;
    }
    for (const rdr_adapter::ContentRef& c : L.contents) contents[c.image] = c;
    if (f < opt.first || (f - opt.first) % opt.step) continue;
    core.ResetFrame();
    ++frames_run;
    LiveStore live;
    if (opt.live_style) BuildLiveStore(cap, L, live);
    if (opt.live_style && corrupt_set && f > opt.corrupt_at) {
      auto cit = live.find(corrupt_key);
      if (cit != live.end())
        for (uint32_t i = 0; i < corrupt_stride && i < cit->second.size(); ++i) cit->second[i] = uint8_t(~cit->second[i]);
    }
    std::map<std::string, std::map<uint32_t, std::pair<uint32_t, uint32_t>>> inst;   // piece -> object -> (lowest part, draws of it)
    for (uint32_t d = 0; d < L.draws.count; ++d) {
      const rdr_adapter::DrawOrigin* origin = L.OriginOf(d);
      if (!origin || origin->klass != rdr_adapter::DrawClass::ModelPart || (origin->pass_site != rdr_adapter::kModelPartColourSite && origin->pass_site != kRingLambertSite)) continue;   // lane 137: + the Lambert site
      const rdr_adapter::Draw& dr = L.draws[d];
      if ((dr.flags & rdr_adapter::kDrawNoGeometry) || dr.layout >= L.layouts.count) continue;
      const rdr_adapter::VertexLayout& lay = L.layouts[dr.layout];
      const int vs = FindVariant(dr.vs_hash), ps = dr.ps_hash ? FindVariant(dr.ps_hash) : -1;
      if (vs < 0) continue;
      ++ring_draws;
      // ---- the independent oracle ----
      std::vector<uint8_t> vb;
      std::set<int> oracle_entries;
      uint32_t pos_off = 0;
      bool have_pos = false;
      for (uint32_t e = 0; e < lay.element_count; ++e) {
        const rdr_adapter::VertexElement& el = L.elements[lay.element_first + e];
        if (el.source_format == 38 && el.guest_offset == 0 && el.vk_format == 109) pos_off = el.host_offset, have_pos = true;
      }
      if (have_pos && cap.ReadGeometry(dr.vertex_offset, dr.vertex_count * lay.host_stride, vb)) {
        for (const assets::ring_gs::RingMesh& m : ring->meshes) {
          if (m.vertices != dr.vertex_count || m.StreamIndexCount() != dr.index_count) continue;
          bool all = true;
          for (uint32_t v = 0; v < m.vertices && all; ++v) {
            float a[3], b[3];
            std::memcpy(a, vb.data() + size_t(v) * lay.host_stride + pos_off, 12);
            for (int c = 0; c < 3; ++c) b[c] = assets::BeF(m.data.s[0] + 28ull * v + 4 * c);
            all = std::memcmp(a, b, 12) == 0;
          }
          if (all) oracle_entries.insert(m.entry);
        }
      }
      if (!oracle_entries.empty()) ++oracle_draws;
      // lane 137 (review 2): --retag-canvas re-tags every canvas colour draw after the first frame to the T_Lambert site (the object is already joined as the canvas): the ring must keep it NotOurs
      rdr_adapter::DrawOrigin retagged;
      bool is_retag = false;
      if (opt.retag_canvas && f > opt.first && oracle_entries.count(953) && origin->pass_site == rdr_adapter::kModelPartColourSite) {
        retagged = *origin, retagged.pass_site = kRingLambertSite, origin = &retagged, is_retag = true;
        ++retag_total;
      }
      // ---- the shipped decision ----
      if (!core.Claims(*origin)) {
        if (!oracle_entries.empty() && !is_retag) ++missed;
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
      uint8_t slots[rdr::kMaxTextures];
      uint32_t n = 0;
      if (ps >= 0)
        for (uint32_t s = 0; s < rdr::fx::kVariants[ps].sampler_count && n < rdr::kMaxTextures; ++s) slots[n++] = rdr::fx::kVariants[ps].samplers[s].slot;
      const auto t_decide = std::chrono::steady_clock::now();
      if (!clock_done) {   // lane 115 F2 (frozen-clock plant): a FRESH core is shown this draw; once it holds a foreign verdict, only NoClaimFrame-style frames pass: after 300 of them the verdict must be gone (Claims true again)
        RingCore probe(o);
        probe.SetArena(ring, arena_id);
        probe.ResetFrame();
        (void)probe.Decide(in, slots, n);
        if (!probe.Claims(*origin)) {
          clock_done = true;
          for (int i = 0; i < 310; ++i) probe.NoClaimFrame();
          clock_ok = probe.Claims(*origin);
        }
      }
      const RingCore::Decision dec = core.Decide(in, slots, n);
      decide_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t_decide).count()), ++decide_calls;
      if (corrupt_set && corrupt_found < 0 && core.stats().bad_bytes > bad_before) corrupt_found = f - opt.corrupt_at;
      if (!dec.ok) {
        ++kept[std::string(rdr_adapter::KeepWhyName(dec.why)) + ": " + dec.reason];
        mismatch += dec.why == rdr_adapter::KeepWhy::Mismatch;
        unjoined += dec.why == rdr_adapter::KeepWhy::Unjoined;
        variant += dec.why == rdr_adapter::KeepWhy::Variant;
        if (!oracle_entries.empty() && !is_retag) ++missed;
        continue;
      }
      if (is_retag) {   // the plant's bug: a draw at the wrong site replaced
        ++retag_ours;
        continue;
      }
      ++replaced;
      if (opt.corrupt_at >= 0 && !corrupt_set && f == opt.corrupt_at) corrupt_set = true, corrupt_key = dr.vertex_offset, corrupt_stride = lay.host_stride, bad_before = core.stats().bad_bytes;
      objects.insert(origin->object);
      const assets::ring_gs::RingMesh& m = ring->meshes[size_t(dec.mesh)];
      entries_replaced.insert(m.entry);
      if (oracle_entries.empty()) ++spurious;
      if (!oracle_entries.count(m.entry)) ++wrong_entry;
      if (int(origin->part) != m.part) ++wrong_part;
      // our bytes against the stream's, by memcmp here
      ++vertex_cmp;
      if (vb.size() < dec.prepared->vertex.size() || std::memcmp(vb.data(), dec.prepared->vertex.data(), dec.prepared->vertex.size()) != 0) ++vertex_bad;
      std::vector<uint8_t> ib;
      if (!cap.ReadGeometry(dr.index_offset, dr.index_count * 2, ib) || ib.size() != dec.prepared->index.size() * 2 || std::memcmp(ib.data(), dec.prepared->index.data(), ib.size()) != 0) ++index_bad;
      // textures put into slots: byte equal to the stream image
      const int picks[3] = {dec.diffuse, dec.normal_map, dec.specular};
      const uint32_t pmips[3] = {dec.diffuse_mips, dec.normal_mips, dec.specular_mips};
      const uint8_t pslot[3] = {0, 4, 5};
      for (int r = 0; r < 3; ++r) {
        if (picks[r] < 0) continue;
        uint32_t image = 0;
        for (uint32_t t = 0; t < dr.texture_count; ++t)
          if (L.texture_slots[dr.texture_first + t].slot == pslot[r]) image = L.texture_slots[dr.texture_first + t].image;
        for (uint32_t t = 0; t < dr.texture_count; ++t)
          if (L.texture_slots[dr.texture_first + t].slot == pslot[r]) g_levels_note = std::to_string(L.texture_slots[dr.texture_first + t].min_level) + "/" + std::to_string(L.texture_slots[dr.texture_first + t].max_level);
        const int eq = texture_cmp(image, ring->textures[size_t(picks[r])], true, pmips[r]);
        if (eq < 0) ++tex_nocmp;
        else {
          ++tex_cmp, tex_bad += eq != 1;
          if (eq != 1) ++tex_by_piece[std::string(PieceOf(m.entry)) + " slot " + std::to_string(pslot[r]) + (eq == 2 ? " tail" : eq == 3 ? " mips " + g_mip_note : " differ")];
        }
      }
      auto& e = inst[PieceOf(m.entry)][origin->object];
      if (e.second == 0 || m.part < int(e.first)) e = {uint32_t(m.part), 0};
      if (int(e.first) == m.part) ++e.second;
      if (m.entry == 956) ++proxy_replaced, proxy_lambert += origin->pass_site == kRingLambertSite;   // lane 137
      if (m.entry == 960) ++rope_high;
      if (m.entry == 980) ++rope_low;
    }
    {   // lane 137: the rope shadow proxy (WM30, T_Lambert site) is 12 instances or none in a frame, never a part of them
      uint32_t total = 0;
      const auto it = inst.find("proxy");
      if (it != inst.end())
        for (const auto& ob : it->second) total += ob.second.second;
      proxy_bad_frames += total != 0 && total != 12;
    }
    for (const auto& pc : want_inst) {
      if (opt.retag_canvas && pc.first == "canvas") continue;   // lane 137: the re-tagged canvas draws are refused on purpose
      uint32_t total = 0;
      const auto it = inst.find(pc.first);
      if (it != inst.end())
        for (const auto& ob : it->second) total += ob.second.second;
      ++inst_checked;
      if (total != pc.second && !(pc.first == "skirt" && false)) {
        ++inst_bad;
        if (inst_note.empty()) inst_note = "frame " + std::to_string(f) + " " + pc.first + " " + std::to_string(total) + " (want " + std::to_string(pc.second) + ")";
      }
    }
  }
  std::printf("INFO  arena %d: %llu frames; %llu ring-site draws, %llu named by the oracle, %llu replaced, %zu objects, entries replaced %zu\n", arena_id, (unsigned long long)frames_run, (unsigned long long)ring_draws,
              (unsigned long long)oracle_draws, (unsigned long long)replaced, objects.size(), entries_replaced.size());
  for (const auto& k : tex_by_piece) std::printf("INFO  texture not byte-equal on every mip: %-28s %llu draws\n", k.first.c_str(), (unsigned long long)k.second);
  for (const auto& k : kept) std::printf("INFO  kept %-50s %llu\n", k.first.c_str(), (unsigned long long)k.second);
  const RingCore::Stats& st = core.stats();
  std::printf("INFO  %s\n", core.Report().c_str());
  // the stream's ring-site draws that are not ring pieces (steps, evtobj props, the announcer's piece) are NotOurs and never counted by the oracle
  std::printf("INFO  decide time: %llu calls, %.3f ms in total, %.4f ms a frame\n", (unsigned long long)decide_calls, double(decide_ns) / 1e6, frames_run ? double(decide_ns) / 1e6 / double(frames_run) : 0.0);
  if (opt.corrupt_at >= 0) Check("capture.in_place_detected", corrupt_set && corrupt_found > 0 && corrupt_found <= int(kRingProofEvery), "a pinned blob inverted in place after frame %d: the byte check failed %d frames later (0 / -1 = never; allowed %u)", opt.corrupt_at, corrupt_found, kRingProofEvery);
  Check("capture.ring_replaced", replaced > 0 && oracle_draws > 0, "%llu replaced of %llu oracle-named draws", (unsigned long long)replaced, (unsigned long long)oracle_draws);
  Check("capture.oracle_all_replaced", missed == 0, "%llu oracle-named draws not replaced", (unsigned long long)missed);
  Check("capture.no_spurious", spurious == 0, "%llu replaced draws the oracle does not name", (unsigned long long)spurious);
  Check("capture.joins_correct", wrong_entry == 0 && wrong_part == 0, "%llu draws joined to an entry the oracle does not name for them, %llu with a part that is not the origin's", (unsigned long long)wrong_entry, (unsigned long long)wrong_part);
  Check("capture.vertex_bytes_equal_stream", vertex_cmp > 0 && vertex_bad == 0, "%llu of %llu replaced draws' vertex buffers differ from the stream's bytes", (unsigned long long)vertex_bad, (unsigned long long)vertex_cmp);
  Check("capture.index_bytes_equal_stream", vertex_cmp > 0 && index_bad == 0, "%llu of %llu replaced draws' index buffers differ from the stream's", (unsigned long long)index_bad, (unsigned long long)vertex_cmp);
  Check("capture.textures_equal_stream", (opt.no_content ? tex_cmp == 0 : tex_cmp > 0) && tex_bad == 0, "%llu slots compared byte for byte (every mip), %llu differ, %llu could not be compared", (unsigned long long)tex_cmp, (unsigned long long)tex_bad, (unsigned long long)tex_nocmp);
  Check("capture.instances", inst_checked > 0 && inst_bad == 0, "%llu piece counts checked, %llu off: %s", (unsigned long long)inst_checked, (unsigned long long)inst_bad, inst_note.c_str());
  Check("capture.draw_checks", mismatch == 0 && unjoined == 0 && st.joins_dropped == 0 && st.bad_bytes == 0, "mismatch %llu, unjoined %llu, joins dropped %llu, byte checks failed %llu", (unsigned long long)mismatch, (unsigned long long)unjoined,
        (unsigned long long)st.joins_dropped, (unsigned long long)st.bad_bytes);
  Check("capture.rope_lod", rope_high + rope_low > 0, "rope draws replaced: high 960 %llu, low 980 %llu", (unsigned long long)rope_high, (unsigned long long)rope_low);
  // lane 137 (N9): the rope shadow proxy at the T_Lambert site. WM30 (arena 29) must show it (--expect-proxy); every other capture draws none there. Whole instances (12) only.
  std::printf("INFO  rope shadow proxy (entry 956): %llu replaced, %llu of them at the T_Lambert site\n", (unsigned long long)proxy_replaced, (unsigned long long)proxy_lambert);
  if (opt.retag_canvas) Check("capture.retag_isolated", retag_total > 0 && retag_ours == 0 && replaced > 0, "%llu canvas draws re-tagged to the T_Lambert site, %llu of them replaced (want 0); the colour-site draws stay ours (%llu replaced)", (unsigned long long)retag_total, (unsigned long long)retag_ours, (unsigned long long)replaced);   // lane 137 review 2
  Check("capture.proxy_whole_frames", proxy_bad_frames == 0, "%llu frames with a part of the 12 proxy instances", (unsigned long long)proxy_bad_frames);
  if (opt.expect_proxy) Check("capture.proxy_lambert", proxy_lambert > 0 && proxy_lambert == proxy_replaced, "%llu proxy draws replaced at the T_Lambert site (want > 0, all of the proxy's)", (unsigned long long)proxy_lambert);
  if (opt.live_style) Check("capture.live_reads_first_use_only", st.verify_runs < replaced, "%llu byte verifications for %llu replaced draws", (unsigned long long)st.verify_runs, (unsigned long long)replaced);
  (void)variant;
  Check("clock.foreign_expires", clock_done && clock_ok, "%%s", !clock_done ? "no foreign verdict could be provoked" : clock_ok ? "a foreign verdict expired after 310 frames in which nothing was claimed (NoClaimFrame only)" : "a foreign verdict never expired: the clock does not run on frames where nothing is claimed");   // lane 115 F2
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
    else if (a == "--retag-canvas") opt.retag_canvas = true;   // lane 137 review 2
    else if (a == "--expect-proxy") opt.expect_proxy = true;   // lane 137: this capture (WM30) draws the rope shadow proxy at the T_Lambert site: it must be ours
    else if (a == "--live-style") opt.live_style = true;
    else if (a == "--corrupt-at") opt.corrupt_at = std::atoi(next());
    else if (a == "--content-fault") opt.content_fault = true;   // planted: every stream image reports a hash that is not its content (a runtime texture swap / a wrong arena): no slot may be ours
    else if (a == "--no-content") opt.no_content = true;   // no image-content feed: every slot stays the stream's, geometry only (the live path until lane 66's feed is on)
    else if (a == "--arena") {
      opt.arena = std::atoi(next());
      opt.arenas.push_back(opt.arena);
    } else if (a == "--step") opt.step = std::max(1, std::atoi(next()));
    else if (a == "--frames") {
      int x = 0, y = -1;
      if (std::sscanf(next(), "%d-%d", &x, &y) >= 1) opt.first = x, opt.last = y;
    } else {
      std::fprintf(stderr, "ring_ours_test: unknown argument %s\n", a.c_str());
      return 3;
    }
  }
  if (opt.game.empty() || (!opt.files && opt.capture.empty())) {
    std::fprintf(stderr, "usage: ring_ours_test --game DIR (--files [--arena N] | --capture DIR [--arena N] [--frames A-B] [--step N] [--fault NAME[,NAME]] [--live-style])\n");
    return 3;
  }
  RingFaults probe;
  std::string ferr;
  if (!RingFaults::Parse(opt.fault, probe, ferr)) {
    std::fprintf(stderr, "ring_ours_test: %s\n", ferr.c_str());
    return 3;
  }
  if (!assets::ring_gs::Init(opt.game)) {
    std::fprintf(stderr, "ring_ours_test: FAIL no game files at %s\n", opt.game.c_str());
    return 2;
  }
  return opt.files ? FilesChecks(opt) : CaptureChecks(opt);
}
