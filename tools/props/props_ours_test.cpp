// props_ours_test: the unit test of our props of other pacs (S101 lane 84 = PO; docs\renderer\UNCLAIMED_MODEL_PARTS.md 3.2). 0 game runs, no GPU.
//   props_ours_test --game DIR --files
//   props_ours_test --game DIR --capture DIR [--frames A-B] [--step N] [--fault NAME[,NAME]] [--live-style] [--no-content] [--content-fault]
// The capture mode runs the SHIPPED join and per-draw checks (src\renderer_scene\props_ours_core.cpp, the code PropsOurs calls) on the colour-site model-part draws of a capture and checks
// them against an INDEPENDENT oracle (a brute-force bit-equal position match against every mesh of every source, ignoring the part and the counts):
//   - every draw the oracle names a prop mesh for is replaced; none is replaced that the oracle does not name
//   - the object's joined source is one the oracle names for every draw it replaced, and the origin's part is the mesh's record
//   - our vertex bytes and our index buffer equal the stream's captured bytes (memcmp here, not the core's own check)
//   - the file texture put into a slot equals the stream image byte for byte on every mip it holds, and our mip count is the stream image's
//   - with the content feed: the joins are {table: evtobj PACH 305, mic: evtobj PACH 4, steps: the "32" family}; draws per frame: table 7, mic 4, steps 6 (17)
// --fault NAME plants one of the faults inside the same code path; the run then must FAIL (tools\props\props_ours_selftest.py asserts which check).
// Exit: 0 all hold, 1 a check failed, 2 a loud failure (cannot read the input), 3 bad usage. Lines: "PASS name (...)" / "FAIL name (...)", then "RESULT ...".
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../src/assets/prop_game_space.h"
#include "../../src/assets/prop_host_vertex.h"
#include "../../src/renderer/shaders/spv/fx_registry.h"
#include "../../src/renderer_adapter/capture_reader.h"
#include "../../src/renderer_adapter/image_content_table.h"
#include "../../src/renderer_scene/props_ours_core.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/ring_hash.h"

namespace {
using namespace rdr_scene;
using rdr_adapter::CaptureReader;
using rdr_adapter::LoadedFrame;

int g_fail = 0;
std::string g_mip_note;
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
  std::vector<std::string> gate;   // lane 254: PATH=idle|load
  bool gate_synth = false;
  bool files = false, live_style = false, no_content = false, content_fault = false, reverse_sources = false, expect_scene = false;   // lane 137
  int first = 0, last = -1, step = 1;
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

// lane 137 (review 1): the expected scene joins on WM30 run 7084: the cage is either twin (PACH 1 / 2 entries 0, 3, 6), the desk bgEtc p9/106, the monitors p9/107, the k_color prop p9/14/14 ; lane 220: the victory scene (run 7133) draws the monitor of p9/7 or its twin p62/7 (positions and bytes equal; the tie stays open)
bool SceneJoinOk(const std::string& l) {
  return l.rfind("bgEtc p1/1/", 0) == 0 || l.rfind("bgEtc p2/1/", 0) == 0 || l == "bgEtc p9/106/0" || l == "bgEtc p9/107/0" || l == "bgEtc p9/7/0" || l == "bgEtc p62/7/0" || l == "bgEtc p9/14/14/0";
}

// ---- files-only ---------------------------------------------------------------------------------------------------------------------
int FilesChecks(const Options&) {
  assets::prop_gs::PropSet set;
  std::string err;
  if (!assets::prop_gs::Load(set, err)) {
    std::printf("FAIL  files.load  (%s)\n", err.c_str());
    return 2;
  }
  size_t table = 0, mic = 0, steps = 0, scene = 0, fam = 0, bad_index = 0, no_diffuse = 0, non_single = 0;
  std::set<int> have;
  for (const assets::prop_gs::Source& s : set.sources) {
    table += s.group == assets::prop_gs::Group::Table, mic += s.group == assets::prop_gs::Group::Mic, steps += s.group == assets::prop_gs::Group::Steps, scene += s.group == assets::prop_gs::Group::Scene, fam += s.family32;   // lane 137
  }
  for (const assets::ring_gs::RingMesh& m : set.meshes) {
    have.insert(m.entry);
    bad_index += assets::ring_gs::BuildIndices(m).size() != m.StreamIndexCount();
    no_diffuse += m.diffuse < 0;
    if (m.diffuse < 0) std::printf("INFO  no diffuse: %s part %d\n", set.SourceOf(m.entry) ? set.SourceOf(m.entry)->label.c_str() : "?", m.part);   // lane 137
    non_single += m.pairs != 1;
  }
  Check("files.table_variants", table == 17, "%zu evtobj PACHs with the table's 7-mesh fingerprint (want 17)", table);
  Check("files.microphones", mic == 3, "%zu evtobj PACHs with the microphone's 4-mesh fingerprint (want 3: 4, 135, 301)", mic);
  Check("files.steps_leaves", steps == 4 && fam == 1, "%zu step leaves (want 4: bgEtc p9 29/32/35 + p62/32; m2.pac is only read when bgEtc gave no 32-family leaf), %zu in the 32 family (want 1)", steps, fam);
  Check("files.scene_leaves", scene == 62, "%zu scene leaves of bgEtc.pac (want 62: the cage p1 / p2 x 3, the desk p9 105 / 106, the monitors p9 7 / 107 + p62 7, the p9 14/k family of 51)", scene);   // lane 137
  Check("files.every_source_has_meshes", have.size() == set.sources.size(), "%zu sources with meshes of %zu", have.size(), set.sources.size());
  Check("files.index_group_rule", bad_index == 0, "%zu meshes whose built index buffer is not strip + (groups - 1)", bad_index);
  std::printf("INFO  %zu meshes, %zu textures, %.1f MB; meshes without a diffuse %zu; meshes with other than one skin pair %zu\n", set.meshes.size(), set.textures.size(), double(set.Bytes()) / 1048576.0, no_diffuse, non_single);
  return g_fail ? 1 : 0;
}

// ---- capture mode -------------------------------------------------------------------------------------------------------------------
int CaptureChecks(const Options& opt) {
  PropsFaults faults;
  std::string ferr;
  if (!PropsFaults::Parse(opt.fault, faults, ferr)) {
    std::fprintf(stderr, "props_ours_test: %s\n", ferr.c_str());
    return 3;
  }
  CaptureReader cap;
  if (!cap.Open(opt.capture)) {
    std::fprintf(stderr, "props_ours_test: FAIL cannot open the capture at %s\n", opt.capture.c_str());
    return 2;
  }
  if (!assets::prop_gs::Init(opt.game)) {
    std::fprintf(stderr, "props_ours_test: FAIL no game files at %s\n", opt.game.c_str());
    return 2;
  }
  auto set = std::make_shared<assets::prop_gs::PropSet>();
  std::string err;
  if (!assets::prop_gs::Load(*set, err)) {
    std::fprintf(stderr, "props_ours_test: FAIL props: %s\n", err.c_str());
    return 2;
  }
  if (opt.reverse_sources) set->Reverse();   // review F3: the pac's order would otherwise already put the right members first
  const int nframes = int(cap.frame_count());
  const int last = opt.last < 0 || opt.last >= nframes ? nframes - 1 : opt.last;
  std::map<uint32_t, rdr_adapter::ContentRef> contents;
  // The stream image behind a record id against a file texture, byte for byte on every mip the stream holds: 1 equal and the same mip count as ours, 0 differ, 3 different mip count, -1 cannot tell.
  const auto texture_cmp = [&](uint32_t image, const assets::Texture& t, uint32_t our_mips) -> int {
    const auto c = contents.find(image);
    if (c == contents.end()) return -1;
    rdr_adapter::LoadedTexture lt;
    if (!cap.ReadTexture(c->second, lt)) return -1;
    size_t file_off = 0;
    std::vector<size_t> offs;
    for (uint32_t m = 0; m < std::max(1u, t.mips); ++m) offs.push_back(file_off), file_off += MipBytes(t.format, std::max(1u, t.width >> m), std::max(1u, t.height >> m));
    bool seen0 = false;
    uint32_t stream_mips = 0;
    for (uint32_t s = 0; s < lt.header.subresource_count; ++s) {
      const rdr_adapter::Subresource& sr = lt.subresources[s];
      if (sr.layer != 0) continue;
      ++stream_mips;
      if (sr.mip == 0) {
        seen0 = true;
        if (sr.width != t.width || sr.height != t.height) return 0;
      }
      if (sr.mip >= offs.size()) return 0;
      const size_t n = size_t(sr.row_bytes) * sr.rows;
      if (n != MipBytes(t.format, std::max(1u, t.width >> sr.mip), std::max(1u, t.height >> sr.mip)) || offs[sr.mip] + n > t.data.size()) return 0;
      if (std::memcmp(lt.data + sr.offset, t.data.data() + offs[sr.mip], n) != 0) return 0;
    }
    if (!seen0) return -1;
    if (stream_mips != our_mips) return g_mip_note = std::to_string(t.width) + "x" + std::to_string(t.height) + " stream " + std::to_string(stream_mips) + " ours " + std::to_string(our_mips), 3;
    return 1;
  };
  PropsOursOptions o;
  o.game_root = opt.game;
  o.faults = faults;
  o.verify_all = !opt.live_style;
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
  PropsCore core(o);
  bool clock_done = false, clock_ok = false;   // lane 115 F2
  core.SetProps(set);

  uint64_t site_draws = 0, oracle_draws = 0, replaced = 0, missed = 0, spurious = 0, wrong_source = 0, wrong_part = 0, vertex_bad = 0, index_bad = 0, vertex_cmp = 0;
  uint64_t tex_cmp = 0, tex_bad = 0, tex_nocmp = 0, mismatch = 0, unjoined = 0;
  std::map<std::string, uint64_t> kept, tex_by_src;
  uint64_t frames_run = 0, tm_replaced = 0;
  std::set<std::string> joins_seen, joins_wrong;
  std::map<int, std::array<uint32_t, 4>> per_frame;   // frame -> replaced draws per group (table, mic, steps, scene)   // lane 137: + scene
  LoadedFrame L;
  for (int f = 0; f <= last; ++f) {
    if (!cap.LoadFrame(uint32_t(f), L)) {
      std::fprintf(stderr, "props_ours_test: FAIL cannot load frame %d\n", f);
      return 2;
    }
    for (const rdr_adapter::ContentRef& c : L.contents) contents[c.image] = c;
    if (f < opt.first || (f - opt.first) % opt.step) continue;
    core.ResetFrame();
    ++frames_run;
    LiveStore live;
    if (opt.live_style) BuildLiveStore(cap, L, live);
    std::array<uint32_t, 4> counts = {0, 0, 0, 0};   // lane 137: + scene
    for (uint32_t d = 0; d < L.draws.count; ++d) {
      const rdr_adapter::DrawOrigin* origin = L.OriginOf(d);
      if (!origin || origin->klass != rdr_adapter::DrawClass::ModelPart || origin->pass_site != rdr_adapter::kModelPartColourSite) continue;
      const rdr_adapter::Draw& dr = L.draws[d];
      if ((dr.flags & rdr_adapter::kDrawNoGeometry) || dr.layout >= L.layouts.count) continue;
      const rdr_adapter::VertexLayout& lay = L.layouts[dr.layout];
      const int vs = FindVariant(dr.vs_hash), ps = dr.ps_hash ? FindVariant(dr.ps_hash) : -1;
      if (vs < 0) continue;
      ++site_draws;
      // ---- the independent oracle: every mesh of every source whose positions equal the stream's, counts and part ignored except the vertex count (a position compare needs it) ----
      std::vector<uint8_t> vb;
      std::set<int> oracle_sources;
      uint32_t pos_off = 0;
      bool have_pos = false;
      for (uint32_t e = 0; e < lay.element_count; ++e) {
        const rdr_adapter::VertexElement& el = L.elements[lay.element_first + e];
        if (el.source_format == 38 && el.guest_offset == 0 && el.vk_format == 109) pos_off = el.host_offset, have_pos = true;
      }
      if (have_pos && cap.ReadGeometry(dr.vertex_offset, dr.vertex_count * lay.host_stride, vb)) {
        for (const assets::ring_gs::RingMesh& m : set->meshes) {
          if (m.vertices != dr.vertex_count || m.StreamIndexCount() != dr.index_count) continue;
          bool all = true;
          for (uint32_t v = 0; v < m.vertices && all; ++v) {
            float a[3], b[3];
            std::memcpy(a, vb.data() + size_t(v) * lay.host_stride + pos_off, 12);
            for (int c = 0; c < 3; ++c) b[c] = assets::BeF(m.data.s[0] + 28ull * v + 4 * c);
            all = std::memcmp(a, b, 12) == 0;
          }
          if (all) oracle_sources.insert(m.entry);
        }
      }
      if (!oracle_sources.empty()) ++oracle_draws;
      if (!core.Claims(*origin)) {
        if (!oracle_sources.empty()) ++missed;
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
      if (!clock_done) {   // lane 115 F2 (frozen-clock plant): a FRESH core is shown this draw; once it holds a foreign verdict, only NoClaimFrame-style frames pass: after 300 of them the verdict must be gone (Claims true again)
        PropsCore probe(o);
        probe.SetProps(set);
        probe.ResetFrame();
        (void)probe.Decide(in, slots, n);
        if (!probe.Claims(*origin)) {
          clock_done = true;
          for (int i = 0; i < 310; ++i) probe.NoClaimFrame();
          clock_ok = probe.Claims(*origin);
        }
      }
      const PropsCore::Decision dec = core.Decide(in, slots, n);
      if (!dec.ok) {
        ++kept[std::string(rdr_adapter::KeepWhyName(dec.why)) + ": " + dec.reason];
        mismatch += dec.why == rdr_adapter::KeepWhy::Mismatch;
        if (dec.why == rdr_adapter::KeepWhy::Mismatch && std::getenv("PROPS_WHY")) {   // S101 lane 220: names the kept draw (object, part, counts), the check text and the sources whose positions equal the stream's
          std::string who;
          for (int e : oracle_sources) who += (who.empty() ? "" : ", ") + set->SourceOf(e)->label;
          for (const assets::ring_gs::RingMesh& m : set->meshes) {   // the uv halves of every vertex whose half differs from the file's (|u| and signed u both tried): the file's float, the stream's half
            if (!oracle_sources.count(m.entry) || int(origin->part) != m.part || m.vertices != dr.vertex_count) continue;
            uint32_t uv_off = 0;
            bool have_uv = false;
            for (uint32_t e = 0; e < lay.element_count; ++e) {
              const rdr_adapter::VertexElement& el = L.elements[lay.element_first + e];
              if (el.source_format == 31 && el.vk_format == 83) uv_off = el.host_offset, have_uv = true;
            }
            if (!have_uv || vb.size() < size_t(m.vertices) * lay.host_stride) continue;
            for (uint32_t v = 0; v < m.vertices; ++v) {
              uint16_t h[2];
              std::memcpy(h, vb.data() + size_t(v) * lay.host_stride + uv_off, 4);
              const float fu = assets::BeF(m.data.s[2] + 8ull * v), fv = assets::BeF(m.data.s[2] + 8ull * v + 4);
              const uint16_t tu = assets::prop_host::FloatToHalfTruncate(fu), tv = assets::prop_host::FloatToHalfTruncate(fv), au = assets::prop_host::FloatToHalfTruncate(std::fabs(fu));
              if (h[0] != tu || h[1] != tv || h[0] != au) std::fprintf(stderr, "WHY   source %s vertex %u: file u %.9g v %.9g | stream halves %04X %04X | signed-truncate %04X %04X, |u| %04X\n", set->SourceOf(m.entry)->label.c_str(), v, fu, fv, h[0], h[1], tu, tv, au);
            }
          }
          if (have_pos && vb.size() >= pos_off + 12) {   // the first vertex's position as the file stores it (big-endian), for tools\assets\find_bytes.py
            char hx[40];
            for (int c = 0; c < 12; ++c) std::snprintf(hx + 2 * c, 3, "%02X", vb[pos_off + (c & ~3) + (3 - (c & 3))]);
            std::fprintf(stderr, "WHY   first position (file byte order) %s\n", hx);
          }
          std::fprintf(stderr, "WHY   frame %u draw %u object 0x%08X part %u counts %u/%u: %s | %s | positions equal: [%s]\n", f, d, origin->object, unsigned(origin->part), dr.vertex_count, dr.index_count, dec.reason ? dec.reason : "", core.TakeFirstFail().c_str(), who.c_str());
        }
        unjoined += dec.why == rdr_adapter::KeepWhy::Unjoined;
        if (!oracle_sources.empty()) ++missed;
        continue;
      }
      ++replaced;
      const assets::ring_gs::RingMesh& m = set->meshes[size_t(dec.mesh)];
      const assets::prop_gs::Source& src = *set->SourceOf(m.entry);
      ++counts[size_t(src.group)];
      joins_seen.insert(src.label);
      tm_replaced += src.group != assets::prop_gs::Group::Steps;
      if (oracle_sources.empty()) ++spurious;
      if (!oracle_sources.count(m.entry)) ++wrong_source;
      if (int(origin->part) != m.part) ++wrong_part;
      if (!opt.no_content) {   // the acceptance joins (Check B / F): table = evtobj PACH 305, mic = evtobj PACH 4, steps = the "32" family
        const bool ok = src.group == assets::prop_gs::Group::Table ? src.label == "evtobj PACH 305" : src.group == assets::prop_gs::Group::Mic ? src.label == "evtobj PACH 4" : src.group == assets::prop_gs::Group::Scene ? SceneJoinOk(src.label) : src.family32;   // lane 137: scene = any of the byte-identical twins
        if (!ok) joins_wrong.insert(std::string(assets::prop_gs::GroupName(src.group)) + " -> " + src.label);
      }
      ++vertex_cmp;
      if (vb.size() < dec.prepared->vertex.size() || std::memcmp(vb.data(), dec.prepared->vertex.data(), dec.prepared->vertex.size()) != 0) ++vertex_bad;
      std::vector<uint8_t> ib;
      if (!cap.ReadGeometry(dr.index_offset, dr.index_count * 2, ib) || ib.size() != dec.prepared->index.size() * 2 || std::memcmp(ib.data(), dec.prepared->index.data(), ib.size()) != 0) ++index_bad;
      const int picks[3] = {dec.diffuse, dec.normal_map, dec.specular};
      const uint32_t pmips[3] = {dec.diffuse_mips, dec.normal_mips, dec.specular_mips};
      const uint8_t pslot[3] = {0, 4, 5};
      for (int r = 0; r < 3; ++r) {
        if (picks[r] < 0) continue;
        uint32_t image = 0;
        for (uint32_t t = 0; t < dr.texture_count; ++t)
          if (L.texture_slots[dr.texture_first + t].slot == pslot[r]) image = L.texture_slots[dr.texture_first + t].image;
        const int eq = texture_cmp(image, set->textures[size_t(picks[r])], pmips[r]);
        if (eq < 0) ++tex_nocmp;
        else {
          ++tex_cmp, tex_bad += eq != 1;
          if (eq != 1) ++tex_by_src[src.label + " part " + std::to_string(m.part) + " slot " + std::to_string(pslot[r]) + (eq == 3 ? " mips " + g_mip_note : " differ")];
        }
      }
    }
    per_frame[f] = counts;
  }
  std::printf("INFO  %llu frames; %llu colour-site draws, %llu named by the oracle, %llu replaced; joins: ", (unsigned long long)frames_run, (unsigned long long)site_draws, (unsigned long long)oracle_draws, (unsigned long long)replaced);
  for (const std::string& j : joins_seen) std::printf("[%s] ", j.c_str());
  std::printf("\n");
  for (const auto& k : tex_by_src) std::printf("INFO  texture not byte-equal: %-60s %llu draws\n", k.first.c_str(), (unsigned long long)k.second);
  for (const auto& k : kept) std::printf("INFO  kept %-50s %llu\n", k.first.c_str(), (unsigned long long)k.second);
  const PropsCore::Stats& st = core.stats();
  std::printf("INFO  %s\n", core.Report().c_str());
  // draws per frame: table 7, mic 4, steps 6 on every frame where the group is drawn at all, and on frame 200 (the acceptance frame) all three
  uint64_t frames_with_props = 0, frames_off = 0;
  std::string off_note;
  for (const auto& pf : per_frame) {
    const uint32_t want[3] = {7, 4, 6};
    bool any = false, off = false;
    for (int g = 0; g < 3; ++g) any = any || pf.second[g], off = off || (pf.second[g] != 0 && pf.second[g] != want[g]);
    frames_with_props += any;
    if (off) {
      ++frames_off;
      if (off_note.empty()) off_note = "frame " + std::to_string(pf.first) + " table " + std::to_string(pf.second[0]) + " mic " + std::to_string(pf.second[1]) + " steps " + std::to_string(pf.second[2]);
    }
  }
  const auto f200 = per_frame.find(200);
  Check("capture.props_replaced", replaced > 0 && oracle_draws > 0, "%llu replaced of %llu oracle-named draws", (unsigned long long)replaced, (unsigned long long)oracle_draws);
  Check("capture.oracle_all_replaced", missed == 0, "%llu oracle-named draws not replaced", (unsigned long long)missed);
  Check("capture.no_spurious", spurious == 0, "%llu replaced draws the oracle does not name", (unsigned long long)spurious);
  Check("capture.joins_correct", wrong_source == 0 && wrong_part == 0 && joins_wrong.empty(), "%llu draws joined to a source the oracle does not name, %llu with a part that is not the origin's; expected joins violated: %s", (unsigned long long)wrong_source,
        (unsigned long long)wrong_part, joins_wrong.empty() ? "none" : joins_wrong.begin()->c_str());
  Check("capture.vertex_bytes_equal_stream", vertex_cmp > 0 && vertex_bad == 0, "%llu of %llu replaced draws' vertex buffers differ from the stream's bytes", (unsigned long long)vertex_bad, (unsigned long long)vertex_cmp);
  Check("capture.index_bytes_equal_stream", vertex_cmp > 0 && index_bad == 0, "%llu of %llu replaced draws' index buffers differ from the stream's", (unsigned long long)index_bad, (unsigned long long)vertex_cmp);
  Check("capture.textures_equal_stream", (opt.no_content || tm_replaced == 0 ? true : tex_cmp > 0) && tex_bad == 0,   // (the steps' stair textures never hash equal: a capture with steps only compares nothing)
         "%llu slots compared byte for byte (every mip the stream holds), %llu differ, %llu could not be compared", (unsigned long long)tex_cmp,
        (unsigned long long)tex_bad, (unsigned long long)tex_nocmp);
  // lane 137 (N9): WM30 (--expect-scene) draws 14 scene-prop draws a frame (cage 3 + 1 + 1, desk 4, monitors 4, k_color 1) at frame 300; every other capture none
  uint64_t scene_total = 0, scene_frames = 0;
  for (const auto& pf : per_frame) scene_total += pf.second[3], scene_frames += pf.second[3] != 0;
  std::printf("INFO  scene props: %llu draws replaced in %llu frames\n", (unsigned long long)scene_total, (unsigned long long)scene_frames);
  if (opt.expect_scene) {   // review 1: EVERY frame run must hold the 14 draws (frame 300 included when the sampling hits it), not just scene_total > 0
    uint64_t scene_bad = 0;
    std::string scene_note;
    for (const auto& pf : per_frame)
      if (pf.second[3] != 14) ++scene_bad, scene_note = scene_note.empty() ? "frame " + std::to_string(pf.first) + " has " + std::to_string(pf.second[3]) : scene_note;
    Check("capture.scene_props", scene_total > 0 && !per_frame.empty() && scene_bad == 0, "%llu scene draws replaced; frames run %zu, frames without exactly 14: %llu %s", (unsigned long long)scene_total, per_frame.size(), (unsigned long long)scene_bad, scene_note.c_str());
  }
  Check("capture.draws_per_frame", frames_with_props > 0 && frames_off == 0 && (f200 == per_frame.end() || (f200->second[0] == 7 && f200->second[1] == 4 && f200->second[2] == 6)), "%llu frames with a prop; frames off (table 7, mic 4, steps 6): %llu %s; frame 200: %s",
        (unsigned long long)frames_with_props, (unsigned long long)frames_off, off_note.c_str(), f200 == per_frame.end() ? "not run" : (std::to_string(f200->second[0] + f200->second[1] + f200->second[2]) + " draws").c_str());
  // WM30 run 7084 (10,144 draws kept as mismatch): an object that fails its byte compare must be given up, not retried every draw. Bound: a few draws per run, whatever the fault.
  Check("capture.mismatch_bounded", mismatch <= 12, "%llu draws kept as mismatch (bar: <= 12 in the whole run; lookalike objects given up %llu)", (unsigned long long)mismatch, (unsigned long long)st.lookalike);
  Check("capture.draw_checks", mismatch == 0 && unjoined == 0 && st.joins_dropped == 0 && st.bad_bytes == 0, "mismatch %llu, unjoined %llu, joins dropped %llu, byte checks failed %llu", (unsigned long long)mismatch, (unsigned long long)unjoined,
        (unsigned long long)st.joins_dropped, (unsigned long long)st.bad_bytes);
  if (opt.live_style) Check("capture.live_reads_first_use_only", st.verify_runs < replaced, "%llu byte verifications for %llu replaced draws", (unsigned long long)st.verify_runs, (unsigned long long)replaced);
  Check("clock.foreign_expires", clock_done && clock_ok, "%%s", !clock_done ? "no foreign verdict could be provoked" : clock_ok ? "a foreign verdict expired after 310 frames in which nothing was claimed (NoClaimFrame only)" : "a foreign verdict never expired: the clock does not run on frames where nothing is claimed");   // lane 115 F2
  std::printf("RESULT %s fault=%s frames=%llu replaced=%llu failed=%d\n", g_fail ? "FAIL" : "OK", opt.fault.empty() ? "none" : opt.fault.c_str(), (unsigned long long)frames_run, (unsigned long long)replaced, g_fail);
  return g_fail ? 1 : 0;
}


// ---- lane 254: the "in a match" decision (no game files, no GPU) -----------------------------------------------------------------------------------------------------------
// Part 1: synthetic records (an arena asset "pac\bg\bgNN.pac" plus N actor / crowd objects). Part 2: every scene record of the given capture folders / .rsc files through the SHIPPED decision
// (PropsCore::InMatch, the code PropsOurs calls), with the expectation per input: NAME=idle (the Create-a-Superstar editor and its pages: no frame may be a match) or NAME=load (a match /
// entrance / victory: every frame must be a match). The planted fault editor_as_match swaps in the old rule (any named arena).
SceneTables Synth(int arena, int actors, int crowd) {
  SceneTables t;
  if (arena >= 0) {
    AssetRef a{};
    char pac[64];
    std::snprintf(pac, sizeof pac, "pac\\bg\\bg%02d.pac", arena);
    a.pac = t.AddString(pac);
    t.assets.push_back(a);
  }
  auto add = [&](const char* prefix, int n) {
    for (int i = 0; i < n; ++i) {
      Object o;
      o.id = t.AddString(std::string(prefix) + std::to_string(i));
      t.objects.push_back(o);
    }
  };
  add("actor:slot:", actors);
  add("crowd:floor:0:member:", crowd);
  add("arena:slot:", 5);
  return t;
}

int GateChecks(const Options& opt, const std::vector<std::string>& inputs) {
  PropsFaults f;
  std::string err;
  PropsFaults::Parse(opt.fault, f, err);
  const bool plant = f.editor_as_match;
  const bool special = f.special_ids;
  struct Case { const char* name; int arena, actors, crowd; bool want; };
  const Case cases[] = {
      {"editor_bg00_one_actor", 0, 1, 0, false},       {"editor_pages_bg23_none", 23, 0, 0, false},      {"editor_bg00_empty", 0, 0, 0, false},
      {"match_bg00_two_actors", 0, 2, 0, true},        {"match_bg00_crowd_only", 0, 0, 40, true},        {"match_bg23_full", 23, 4, 300, true},
      {"match_bg04_full", 4, 5, 300, true},            {"editor_like_bg04_no_people", 4, 0, 0, false},   {"editor_like_bg44_one_actor", 44, 1, 0, false},
      {"menu_bg66_never_a_match", 66, 5, 300, false},  {"menu_bg68_never_a_match", 68, 4, 0, false},     {"menu_bg80_never_a_match", 80, 2, 10, false},
      {"no_arena_never_a_match", -1, 5, 300, false}};
  for (const Case& c : cases) {
    const SceneTables t = Synth(c.arena, c.actors, c.crowd);
    Check((std::string("gate.synth.") + c.name).c_str(), PropsCore::InMatch(t, plant, special) == c.want, "arena %d, actors %d, crowd %d -> %s", c.arena, c.actors, c.crowd, PropsCore::InMatch(t, plant, special) ? "load" : "idle");
  }
  uint64_t frames_total = 0;
  for (const std::string& in : inputs) {
    const size_t eq = in.rfind('=');
    if (eq == std::string::npos) { std::fprintf(stderr, "props_ours_test: --gate input %s wants PATH=idle|load\n", in.c_str()); return 3; }
    const std::string path = in.substr(0, eq), want = in.substr(eq + 1);
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
      std::filesystem::path d = path;
      if (std::filesystem::is_directory(d / "capture" / "scene", ec)) d = d / "capture" / "scene";
      else if (std::filesystem::is_directory(d / "scene", ec)) d = d / "scene";
      for (const auto& e : std::filesystem::directory_iterator(d, ec)) if (e.path().extension() == ".rsc") files.push_back(e.path());
      std::sort(files.begin(), files.end());
    } else {
      files.push_back(path);
    }
    if (files.empty()) { std::fprintf(stderr, "props_ours_test: FAIL no scene records at %s\n", path.c_str()); return 2; }
    uint64_t match = 0, idle = 0, bad = 0;
    PropsCore::MatchEvidence last;
    for (size_t i = 0; i < files.size(); i += size_t(std::max(1, opt.step))) {
      SceneTables t;
      std::string e;
      if (!ReadRecord(files[i].string(), t, e)) { ++bad; continue; }
      last = PropsCore::EvidenceOf(t);
      (PropsCore::InMatch(t, plant, special) ? match : idle)++;
    }
    frames_total += match + idle;
    const bool ok = bad == 0 && (want == "idle" ? match == 0 && idle > 0 : idle == 0 && match > 0);
    Check((std::string("gate.capture.") + std::filesystem::path(path).filename().string() + "." + want).c_str(), ok, "%llu records: %llu load, %llu idle, %llu unreadable; last: arena %d, actors %d, crowd %d", (unsigned long long)(match + idle + bad), (unsigned long long)match, (unsigned long long)idle, (unsigned long long)bad, last.arena, last.actors, last.crowd);
  }
  std::printf("RESULT %s fault=%s frames=%llu replaced=0 failed=%d\n", g_fail ? "FAIL" : "OK", opt.fault.empty() ? "none" : opt.fault.c_str(), (unsigned long long)frames_total, g_fail);
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
    else if (a == "--gate-synth") opt.gate_synth = true;
    else if (a == "--gate") opt.gate.push_back(next());   // lane 254: the in-a-match decision on synthetic records and on scene records (no game files)
    else if (a == "--live-style") opt.live_style = true;
    else if (a == "--content-fault") opt.content_fault = true;
    else if (a == "--no-content") opt.no_content = true;
    else if (a == "--expect-scene") opt.expect_scene = true;   // lane 137: WM30: the scene props (cage, desk, monitors) must be ours
    else if (a == "--reverse-sources") opt.reverse_sources = true;   // TEST ONLY (review F3): the sources in the opposite order: the joins must still be {table 305, mic 4, steps 32 family} (the content tie-break at work)
    else if (a == "--step") opt.step = std::max(1, std::atoi(next()));
    else if (a == "--frames") {
      int x = 0, y = -1;
      if (std::sscanf(next(), "%d-%d", &x, &y) >= 1) opt.first = x, opt.last = y;
    } else {
      std::fprintf(stderr, "props_ours_test: unknown argument %s\n", a.c_str());
      return 3;
    }
  }
  if (!opt.gate.empty() || opt.gate_synth) return GateChecks(opt, opt.gate);
  if (opt.game.empty() || (!opt.files && opt.capture.empty())) {
    std::fprintf(stderr, "usage: props_ours_test --game DIR (--files | --capture DIR [--frames A-B] [--step N] [--fault NAME[,NAME]] [--live-style] [--no-content] [--content-fault])\n");
    return 3;
  }
  PropsFaults probe;
  std::string ferr;
  if (!PropsFaults::Parse(opt.fault, probe, ferr)) {
    std::fprintf(stderr, "props_ours_test: %s\n", ferr.c_str());
    return 3;
  }
  if (!assets::prop_gs::Init(opt.game)) {
    std::fprintf(stderr, "props_ours_test: FAIL no game files at %s\n", opt.game.c_str());
    return 2;
  }
  return opt.files ? FilesChecks(opt) : CaptureChecks(opt);
}
