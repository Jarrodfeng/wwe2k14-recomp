// Lane 136 (SH3 slice 1) unit test: the circle blob shadows' record, writer and pure functions. 0 game runs, no GPU. Build: tools\shadow\build_circles_unit.ps1.
//   circles_unit.exe FILE.circ [FILE.circ ...] [--live-mesh FILE.lmesh] [--plant NAME]
// FILE = tools\shadow\circles_oracle.py's dump of a capture (Check B, run 7079, Bryan). Everything here is the SHIPPED code: record_io (the sections), circle_hook.h (the writer's tracker,
// driven with a fake guest memory laid out big-endian where sub_82812E90 holds the constants), circle_shadow_core.h (BuildRows, the mesh conversion, the board), circle_oracle.h.
//   A. the record: write -> read -> write is byte-identical; a record without the sections reads as empty; a mesh index past its vertices is refused.
//   B. the writer (half (a) of the acceptance): per frame the tracker's entries recomputed through BuildRows equal the stream's circle constants bit for bit (every c0, c4-c7, c8-c28 word),
//      the entries carry the hook's stamp, the slot and the character of the guest; the mesh read from the fake object equals the stream's vertex/index bytes.
//   C. the guards: a wrong r4 / r6, points without parameters, an unreadable pointer, a bad slot, too many entries: each counted, nothing recorded.
//   D. the board: entries by stamp, the previous batch, a record delivered twice.
//   E. (lane 205, check K3) the mesh against LIVE bytes: --live-mesh FILE (tools\shadow\circles_live_mesh.py dump of a case zip: the record's mesh as the writer wrote it, the stream's circle vertex bytes). The guest
//      vertex buffer is reconstructed WITHOUT the rule under test: an OLD record (written with the id word kept in memory order, so it differs from the stream) holds that word's raw guest bytes and every other word
//      swapped, so the guest is rebuilt from it word by word; a NEW record (equal to the stream) is checked equal as it stands. Then the shipped VertexGuestToHost must give the stream's bytes.
// --plant NAME (tests only): id_word_kept (E, and the tracker's mesh read), bind_pose, join_by_index, no_fade (check B: circles.rows); mesh_header_as_data (F1, today's wrong code), stale_viewproj (F7): the tracker checks. Each must FAIL (exit 1); a clean run exits 0.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/renderer_scene/circle_oracle.h"
#include "../../src/renderer_scene/live_scene.h"

using namespace rdr_scene;
using namespace rdr_scene::circles;
using rdr_scene::circles::oracle::File;
using rdr_scene::circles::oracle::Frame;

static int g_bad = 0;
static void Check(bool ok, const char* name, const std::string& why = "") {
  std::printf("%s  %-34s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_bad;
}

static bool SameBytes(const void* a, const void* b, size_t n) { return std::memcmp(a, b, n) == 0; }

static void RecordChecks() {
  SceneTables t;
  t.header.guest_frame = 77;
  for (uint32_t i = 0; i < 4; ++i) {
    CircleShadow e;
    e.frame = 70 + i, e.slot = i, e.character = 0x40000000u + i, e.flags = kCircleHaveParam | kCircleHaveViewProj;
    for (int k = 0; k < 4; ++k) e.shadow_param[k] = float(k + i);
    for (int k = 0; k < 16; ++k) e.view_proj[k] = float(k) * 0.5f;
    for (uint32_t p = 0; p < kCirclePoints; ++p)
      for (int c = 0; c < 4; ++c) e.points[p][c] = float(p * 4 + c + i);
    t.circle_shadows.push_back(e);
  }
  CircleMesh m;
  m.vertex_count = 8, m.index_count = 6;
  for (uint32_t i = 0; i < 8 * 32; ++i) m.vertices[i] = uint8_t(i * 7);
  for (uint32_t i = 0; i < 6; ++i) m.indices[i] = uint16_t(i + 1);
  t.circle_mesh.push_back(m);
  const std::vector<uint8_t> a = SerializeRecord(t);
  SceneTables back;
  std::string err;
  const bool parsed = ParseRecord(a.data(), a.size(), back, err);
  Check(parsed, "record.parses", err);
  const std::vector<uint8_t> b = parsed ? SerializeRecord(back) : std::vector<uint8_t>();
  Check(parsed && a == b, "record.round_trip_bytes", "write -> read -> write");
  Check(parsed && back.circle_shadows.size() == 4 && SameBytes(back.circle_shadows.data(), t.circle_shadows.data(), 4 * sizeof(CircleShadow)) && back.circle_mesh.size() == 1 &&
            SameBytes(&back.circle_mesh[0], &m, sizeof m),
        "record.tables_equal", "4 entries, 1 mesh");
  Check(sizeof(CircleShadow) == 432 && sizeof(CircleMesh) == 4496 && kScCircleShadows == 16 && kScCircleMesh == 17, "record.ids_pinned", "section ids 16 / 17, element sizes 432 / 4496");
  // an older record (no circle sections) reads as empty
  SceneTables old;
  old.header.guest_frame = 5;
  const std::vector<uint8_t> o = SerializeRecord(old);
  SceneTables oback;
  Check(ParseRecord(o.data(), o.size(), oback, err) && oback.circle_shadows.empty() && oback.circle_mesh.empty(), "record.old_reads_empty", err);
  // a bad mesh is refused (index past the vertices), by the reader and by the writer's Validate
  SceneTables bad = t;
  bad.circle_mesh[0].indices[2] = 9;
  const std::vector<uint8_t> bb = SerializeRecord(bad);
  SceneTables bback;
  const bool kept_record = ParseRecord(bb.data(), bb.size(), bback, err);   // F5: only the circle tables go, the record is kept (as section 15)
  Check(kept_record && bback.circle_mesh.empty() && bback.circle_shadows.empty() && bback.circle_errors == 1 && bback.header.guest_frame == 77, "record.bad_mesh_drops_circle_tables_only",
        "circle_errors " + std::to_string(bback.circle_errors));
  std::string verr;
  Check(!live::Validate(bad, verr) && live::Validate(t, verr), "record.validate", verr);
  // a CircleShadow element size from a different layout is refused
  std::vector<uint8_t> skew = a;
  SectionEntry e;
  SceneHeader h;
  std::memcpy(&h, skew.data(), sizeof h);
  bool hit = false;
  for (uint32_t i = 0; i < h.section_count; ++i) {
    std::memcpy(&e, skew.data() + sizeof h + i * sizeof e, sizeof e);
    if (e.id == kScCircleShadows) {
      e.element_size += 4;
      std::memcpy(skew.data() + sizeof h + i * sizeof e, &e, sizeof e);
      hit = true;
    }
  }
  SceneTables sback;
  Check(hit && ParseRecord(skew.data(), skew.size(), sback, err) && sback.circle_shadows.empty() && sback.circle_errors == 1 && sback.circle_mesh.size() == 1 && sback.header.guest_frame == 77,
        "record.element_size_drops_circle_tables_only", "circle_errors " + std::to_string(sback.circle_errors) + ", the mesh and the rest of the record kept");
}

static void GuardChecks() {
  oracle::FakeGuest g;
  File f;
  f.vertices.assign(128 * 32, 0);
  f.indices.assign(192, 0);
  for (uint32_t i = 0; i < 192; ++i) f.indices[i] = uint16_t(i % 128);
  oracle::SetupMesh(g, f);
  const live::circles::ReadBytes rd = [&g](uint32_t a, void* dst, size_t n) { return g.Read(a, dst, n); };
  float rows[kRows * 4] = {};
  for (size_t i = 0; i < kRows * 4; ++i) rows[i] = float(i);
  g.PutFloats(0x7F000000u + 256, rows + 16, 16);
  g.PutFloats(0x7F000000u + 80, rows, 4);
  g.PutFloats(0x7F000000u + 320, rows + 32, 4 * kCirclePoints);
  const uint32_t stack = 0x7F000000u, slot0 = live::circles::kSlotTable;
  using namespace live::circles;
  {   // the clean sequence records one entry
    Tracker t;
    t.OnConstants(kViewProjReturn, 4, stack + 256, 4, oracle::kObject, slot0, 9, rd);
    t.OnConstants(kParamReturn, 0, stack + 80, 1, oracle::kObject, slot0, 9, rd);
    t.OnConstants(kPointsReturn, 8, stack + 320, 21, oracle::kObject, slot0, 9, rd);
    std::vector<CircleShadow> v;
    CircleMesh m;
    bool mp = false;
    t.Take(v, &m, mp, true);
    Check(v.size() == 1 && v[0].frame == 9 && mp && m.vertex_count == 128 && m.index_count == 192, "guards.clean_sequence", "1 entry, stamp 9, mesh 128 / 192");
  }
  {   // wrong registers, missing parameters, unreadable pointers, a bad slot: counted, nothing recorded
    Tracker t;
    t.OnConstants(kViewProjReturn, 5, stack + 256, 4, oracle::kObject, slot0, 9, rd);     // r4 wrong
    t.OnConstants(kPointsReturn, 8, stack + 320, 21, oracle::kObject, slot0, 9, rd);      // points without parameters
    t.OnConstants(kParamReturn, 0, 0x12345678u, 1, oracle::kObject, slot0, 9, rd);        // unreadable
    t.OnConstants(kParamReturn, 0, stack + 80, 1, oracle::kObject, slot0, 9, rd);
    t.OnConstants(kPointsReturn, 8, stack + 320, 20, oracle::kObject, slot0, 9, rd);      // r6 wrong
    t.OnConstants(kParamReturn, 0, stack + 80, 1, oracle::kObject, slot0, 9, rd);
    t.OnConstants(kPointsReturn, 8, stack + 320, 21, oracle::kObject, slot0 + 2, 9, rd);  // misaligned slot
    const Stats s = t.GetStats();
    std::vector<CircleShadow> v;
    CircleMesh m;
    bool mp = false;
    t.Take(v, &m, mp, false);
    Check(s.orphans == 3 && s.unreadable == 1 && s.bad_slot == 1 && v.size() == 1 && v[0].slot == 0xFFFFFFFFu, "guards.refusals_counted",
          "orphans " + std::to_string(s.orphans) + " unreadable " + std::to_string(s.unreadable) + " bad_slot " + std::to_string(s.bad_slot));
  }
  {   // too many entries between two Takes: counted, the list stays bounded
    Tracker t;
    for (uint32_t i = 0; i < kMaxPending + 5; ++i) {
      t.OnConstants(kParamReturn, 0, stack + 80, 1, oracle::kObject, slot0, 9, rd);
      t.OnConstants(kPointsReturn, 8, stack + 320, 21, oracle::kObject, slot0, 9, rd);
    }
    std::vector<CircleShadow> v;
    CircleMesh m;
    bool mp = false;
    t.Take(v, &m, mp, false);
    Check(v.size() == kMaxPending && t.GetStats().overflow == 5, "guards.overflow_bounded", std::to_string(v.size()) + " kept, " + std::to_string(t.GetStats().overflow) + " dropped");
  }
  {   // an unreadable mesh is counted and never recorded; a bad index count too
    oracle::FakeGuest g2;
    for (uint32_t s = 0; s < kSlots; ++s) g2.PutWord(kSlotTable + 4 * s, 0x40000000u + s);
    g2.PutWord(oracle::kObject + kMeshVertexPtr, 0x70010000u);
    g2.PutWord(oracle::kObject + kMeshIndexPtr, 0x70020000u);
    g2.PutWord(oracle::kObject + kMeshIndexCount, 193);
    g2.PutFloats(stack + 80, rows, 4);
    g2.PutFloats(stack + 320, rows + 32, 4 * kCirclePoints);
    const live::circles::ReadBytes rd2 = [&g2](uint32_t a, void* dst, size_t n) { return g2.Read(a, dst, n); };
    Tracker t;
    t.OnConstants(kParamReturn, 0, stack + 80, 1, oracle::kObject, slot0, 9, rd2);
    t.OnConstants(kPointsReturn, 8, stack + 320, 21, oracle::kObject, slot0, 9, rd2);
    Check(!t.has_mesh() && t.GetStats().mesh_bad >= 1, "guards.bad_mesh_not_recorded", "mesh_bad " + std::to_string(t.GetStats().mesh_bad));
  }
}

// F1 (resource objects behind [circle+36] / [circle+40]) and F7 (a ViewProj that cannot be read leaves nothing stale). Run in the clean mode and under the tracker plants
// mesh_header_as_data (today's wrong code: the header read as the data) and stale_viewproj.
static void TrackerChecks(live::circles::Plant plant) {
  using namespace live::circles;
  File f;
  f.vertices.resize(128 * 32);
  for (size_t i = 0; i < f.vertices.size(); ++i) f.vertices[i] = uint8_t(i * 13 + 5);
  f.indices.resize(192);
  for (uint32_t i = 0; i < 192; ++i) f.indices[i] = uint16_t((i * 7) % 128);
  f.indices[0] = 127;
  float rows[kRows * 4];
  for (size_t i = 0; i < kRows * 4; ++i) rows[i] = float(i) + 0.5f;
  const uint32_t stack = 0x7F000000u, slot0 = kSlotTable;
  {   // F1: the mesh is read through the two resource objects; the low bits of the fetch word and the size are masked
    oracle::FakeGuest g;
    oracle::SetupMesh(g, f);
    g.PutFloats(stack + 80, rows, 4);
    g.PutFloats(stack + 320, rows + 32, 4 * kCirclePoints);
    const ReadBytes rd = [&g](uint32_t a, void* dst, size_t n) { return g.Read(a, dst, n); };
    Tracker t;
    t.SetPlant(plant);
    t.OnConstants(kParamReturn, 0, stack + 80, 1, oracle::kObject, slot0, 9, rd);
    t.OnConstants(kPointsReturn, 8, stack + 320, 21, oracle::kObject, slot0, 9, rd);
    std::vector<CircleShadow> v;
    CircleMesh m;
    bool mp = false;
    t.Take(v, &m, mp, true);
    CircleMesh want;
    std::memcpy(want.vertices, f.vertices.data(), f.vertices.size());
    std::memcpy(want.indices, f.indices.data(), f.indices.size() * 2);
    const bool same = mp && m.vertex_count == 128 && m.index_count == 192 && std::memcmp(m.vertices, want.vertices, f.vertices.size()) == 0 && std::memcmp(m.indices, want.indices, 192 * 2) == 0;
    Check(same, "writer.mesh_through_resource_objects", mp ? "read" : "mesh_bad " + std::to_string(t.GetStats().mesh_bad));
  }
  {   // F1: a 32-bit index buffer is refused (top bit of the word at +0 of the index buffer object, listing 0x82CB1CEC)
    oracle::FakeGuest g;
    oracle::SetupMesh(g, f, 0x80000102u);
    g.PutFloats(stack + 80, rows, 4);
    g.PutFloats(stack + 320, rows + 32, 4 * kCirclePoints);
    const ReadBytes rd = [&g](uint32_t a, void* dst, size_t n) { return g.Read(a, dst, n); };
    Tracker t;
    t.SetPlant(plant);
    t.OnConstants(kParamReturn, 0, stack + 80, 1, oracle::kObject, slot0, 9, rd);
    t.OnConstants(kPointsReturn, 8, stack + 320, 21, oracle::kObject, slot0, 9, rd);
    Check(!t.has_mesh() && t.GetStats().mesh_bad >= 1, "writer.index32_refused", "mesh_bad " + std::to_string(t.GetStats().mesh_bad));
  }
  {   // F7: the previous call's ViewProj must not survive a ViewProj that cannot be read
    oracle::FakeGuest g;
    oracle::SetupMesh(g, f);
    g.PutFloats(stack + 256, rows + 16, 16);
    g.PutFloats(stack + 80, rows, 4);
    g.PutFloats(stack + 320, rows + 32, 4 * kCirclePoints);
    const ReadBytes rd = [&g](uint32_t a, void* dst, size_t n) { return g.Read(a, dst, n); };
    Tracker t;
    t.SetPlant(plant);
    t.OnConstants(kViewProjReturn, 4, stack + 256, 4, oracle::kObject, slot0, 9, rd);
    t.OnConstants(kParamReturn, 0, stack + 80, 1, oracle::kObject, slot0, 9, rd);
    t.OnConstants(kPointsReturn, 8, stack + 320, 21, oracle::kObject, slot0, 9, rd);
    t.OnConstants(kViewProjReturn, 4, 0x12345678u, 4, oracle::kObject, slot0, 10, rd);   // the next call: unreadable
    t.OnConstants(kParamReturn, 0, stack + 80, 1, oracle::kObject, slot0, 10, rd);
    t.OnConstants(kPointsReturn, 8, stack + 320, 21, oracle::kObject, slot0, 10, rd);
    std::vector<CircleShadow> v;
    CircleMesh m;
    bool mp = false;
    t.Take(v, &m, mp, false);
    bool zero = v.size() == 2;
    if (zero)
      for (float x : v[1].view_proj) zero = zero && x == 0.0f;
    Check(v.size() == 2 && (v[0].flags & kCircleHaveViewProj) && !(v[1].flags & kCircleHaveViewProj) && zero, "writer.viewproj_not_stale", "the second entry has no ViewProj flag and a zero matrix");
  }
}

static void BoardChecks() {
  Board b;
  const auto entry = [](uint32_t frame, uint32_t slot) {
    CircleShadow e;
    e.frame = frame, e.slot = slot, e.character = 0x40000000u + slot;
    e.shadow_param[0] = float(frame * 10 + slot);
    return e;
  };
  b.Add({entry(100, 0), entry(100, 1)});
  b.Add({entry(101, 0), entry(101, 1), entry(101, 2)});
  b.Add({entry(101, 0), entry(101, 1), entry(101, 2)});   // delivered twice
  const std::vector<CircleShadow>* at100 = b.At(100);
  const std::vector<CircleShadow>* at101 = b.At(101);
  Check(at100 && at100->size() == 2 && at101 && at101->size() == 3 && !b.At(99), "board.by_stamp", "100: 2, 101: 3, 99: none");
  const std::vector<CircleShadow>* prev = b.PreviousBatch();
  Check(prev && prev->size() == 2 && (*prev)[0].frame == 100, "board.previous_batch", "a record delivered twice is one batch");
  for (uint32_t f = 102; f < 102 + 60; ++f) b.Add({entry(f, 0)});
  Check(b.stamps() <= 48 && !b.At(100) && b.At(150), "board.bounded", std::to_string(b.stamps()) + " stamps held");
}

// E (lane 205, check K3): the mesh against LIVE bytes. FILE = circles_live_mesh.py dump (u32 'LMSH', 1, vertices, indices, the record's vertex bytes, the stream's vertex bytes, u32 draws, u32 draws equal to the first).
static void LiveMeshCheck(const std::string& path, bool keep_id_word) {
  std::vector<uint8_t> b;
  if (FILE* fh = std::fopen(path.c_str(), "rb")) {
    uint8_t chunk[65536];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof chunk, fh)) > 0) b.insert(b.end(), chunk, chunk + n);
    std::fclose(fh);
  }
  uint32_t head[4] = {};
  if (b.size() < 16) return Check(false, "live_mesh.load", path), void();
  std::memcpy(head, b.data(), 16);
  const size_t nv = head[2], vb = nv * 32;
  if (head[0] != 0x48534D4Cu || head[1] != 1 || !nv || b.size() != 16 + 2 * vb + 8) return Check(false, "live_mesh.load", path + ": not a live mesh file"), void();
  const uint8_t* rec = b.data() + 16;
  const uint8_t* stream = rec + vb;
  uint32_t draws = 0, same_first = 0;
  std::memcpy(&draws, stream + vb, 4), std::memcpy(&same_first, stream + vb + 4, 4);
  const std::string label = path.substr(path.find_last_of("/\\") == std::string::npos ? 0 : path.find_last_of("/\\") + 1);
  Check(draws > 0 && same_first == draws, ("live_mesh.draws " + label).c_str(), std::to_string(same_first) + " of " + std::to_string(draws) + " circle draws carry the same vertex bytes");
  // The guest vertex buffer, without the rule under test. A NEW record equals the stream: its guest is the format's inverse of the stream. An OLD record kept word 4 as raw guest bytes (the other words host
  // order): the guest is those raw bytes, every other word the format's inverse.
  const bool is_new = SameBytes(rec, stream, vb);
  std::vector<uint8_t> guest(vb);
  for (size_t v = 0; v < nv; ++v) {
    VertexHostToGuest(rec + 32 * v, guest.data() + 32 * v);
    if (!is_new) std::memcpy(guest.data() + 32 * v + 16, rec + 32 * v + 16, 4);
  }
  size_t bad = 0;
  for (size_t v = 0; v < nv; ++v) {
    uint8_t h[32];
    VertexGuestToHost(guest.data() + 32 * v, h, keep_id_word);
    bad += !SameBytes(h, stream + 32 * v, 32);
  }
  Check(bad == 0, ("live_mesh.guest_to_host " + label).c_str(),
        std::string(is_new ? "record written by the shipped rule (equal to the stream)" : "record written by the old rule (id word raw)") + ": the conversion gives the stream's bytes for " + std::to_string(nv - bad) + " of " + std::to_string(nv) + " vertices");
}

int main(int argc, char** argv) {
  std::vector<std::string> files;
  std::string plant;
  std::vector<std::string> live_meshes;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--plant" && i + 1 < argc) plant = argv[++i];
    else if (a == "--live-mesh" && i + 1 < argc) live_meshes.push_back(argv[++i]);
    else files.push_back(a);
  }
  if (plant != "" && plant != "bind_pose" && plant != "join_by_index" && plant != "no_fade" && plant != "mesh_header_as_data" && plant != "stale_viewproj" && plant != "id_word_kept") {
    std::printf("FAIL  unknown plant '%s'\n", plant.c_str());
    return 2;
  }
  if (files.empty() && live_meshes.empty()) {
    std::printf("FAIL  no oracle file given (tools\\shadow\\circles_oracle.py dump ...)\n");
    return 2;
  }
  TrackerChecks(plant == "mesh_header_as_data" ? live::circles::Plant::MeshHeaderAsData : plant == "stale_viewproj" ? live::circles::Plant::StaleViewProj : plant == "id_word_kept" ? live::circles::Plant::IdWordKept : live::circles::Plant::None);
  if (plant.empty()) {
    RecordChecks();
    GuardChecks();
    BoardChecks();
  }
  for (const std::string& path : files) {
    File f;
    std::string err;
    if (!oracle::Load(path, f, err)) {
      Check(false, "oracle.load", err);
      continue;
    }
    oracle::FakeGuest g;
    oracle::SetupMesh(g, f);
    live::circles::Tracker tracker;
    tracker.SetPlant(plant == "mesh_header_as_data" ? live::circles::Plant::MeshHeaderAsData : plant == "stale_viewproj" ? live::circles::Plant::StaleViewProj : plant == "id_word_kept" ? live::circles::Plant::IdWordKept : live::circles::Plant::None);
    Board board;
    std::unordered_map<uint64_t, CircleShadow> first_pose;
    Faults faults;
    faults.bind_pose = plant == "bind_pose", faults.join_by_index = plant == "join_by_index", faults.no_fade = plant == "no_fade";
    uint64_t draws = 0, rows_equal = 0, rows_wrong = 0, count_wrong = 0, stamp_wrong = 0, slot_wrong = 0, joined_missing = 0;
    CircleMesh mesh;
    bool mesh_seen = false;
    for (const Frame& fr : f.frames) {
      const uint32_t stamp = fr.guest_frame - 1;   // the hook runs a frame before the header's present counter (an arbitrary offset the hook's join must not assume)
      oracle::DriveFrame(tracker, g, fr, stamp);
      std::vector<CircleShadow> entries;
      bool mp = false;
      CircleMesh m;
      tracker.Take(entries, &m, mp, true);
      if (mp) mesh = m, mesh_seen = true;
      if (entries.size() != fr.draws.size()) ++count_wrong;
      for (const CircleShadow& e : entries) first_pose.emplace((uint64_t(e.slot) << 32) | e.character, e);
      board.Add(entries);
      // the join: by the stamp (join_by_index: the previous batch)
      const std::vector<CircleShadow>* use = faults.join_by_index ? board.PreviousBatch() : board.At(stamp);
      if (!use) {
        joined_missing += fr.draws.size();
        draws += fr.draws.size();
        continue;
      }
      for (size_t i = 0; i < fr.draws.size(); ++i) {
        ++draws;
        if (i >= use->size()) {
          ++joined_missing;
          continue;
        }
        const CircleShadow& e = (*use)[i];
        if (!faults.join_by_index && e.frame != stamp) ++stamp_wrong;
        if (e.slot != i || e.character != oracle::kCharBase + 0x100 * i) ++slot_wrong;
        const auto fp = first_pose.find((uint64_t(e.slot) << 32) | e.character);
        float rows[kRows * 4];
        BuildRows(e, faults, fp == first_pose.end() ? nullptr : &fp->second, rows);
        bool same = true;
        for (size_t k = 0; k < kRows * 4 && same; ++k) same = rows[k] == fr.draws[i][k];
        (same ? rows_equal : rows_wrong) += 1;
      }
    }
    const std::string label = path.substr(path.find_last_of("/\\") == std::string::npos ? 0 : path.find_last_of("/\\") + 1);
    Check(draws > 0 && f.frames.size() > 0, ("oracle.frames " + label).c_str(), std::to_string(f.frames.size()) + " frames, " + std::to_string(draws) + " circle draws");
    Check(count_wrong == 0 && stamp_wrong == 0 && slot_wrong == 0, ("writer.entries " + label).c_str(),
          "count wrong " + std::to_string(count_wrong) + ", stamp wrong " + std::to_string(stamp_wrong) + ", slot / character wrong " + std::to_string(slot_wrong));
    Check(rows_wrong == 0 && joined_missing == 0 && rows_equal == draws, ("circles.rows " + label).c_str(),
          "rows equal to the stream's constants " + std::to_string(rows_equal) + " of " + std::to_string(draws) + ", differ " + std::to_string(rows_wrong) + ", unjoined " + std::to_string(joined_missing));
    bool mesh_ok = mesh_seen && mesh.vertex_count * 32 == f.vertices.size() && mesh.index_count == f.indices.size() && SameBytes(mesh.vertices, f.vertices.data(), f.vertices.size()) &&
                   SameBytes(mesh.indices, f.indices.data(), f.indices.size() * 2);
    Check(mesh_ok, ("writer.mesh " + label).c_str(), "the mesh read from the fake object equals the stream's vertex and index bytes");
    const live::circles::Stats s = tracker.GetStats();
    Check(s.orphans == 0 && s.unreadable == 0 && s.bad_slot == 0 && s.overflow == 0 && s.mesh_bad == 0 && s.mesh_ok >= 1, ("writer.stats " + label).c_str(),
          "entries " + std::to_string(s.entries) + " orphans " + std::to_string(s.orphans) + " mesh reads " + std::to_string(s.mesh_reads));
  }
  for (const std::string& lm : live_meshes) LiveMeshCheck(lm, plant == "id_word_kept");
  std::printf("%s: %d failed\n", g_bad ? "FAIL" : "PASS", g_bad);
  return g_bad ? 1 : 0;
}
