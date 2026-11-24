// S108 lane SHOT-A: offline proof of the subject-shot snapshot sections (src\renderer_scene\rows_snapshot.h: 68 body scales, 69 liquid images, 70 their bytes).
// docs\research\s108\SHOT_plan.md sections 1, 7. 0 game runs: every row and byte is synthetic, made here.
//   snapshot_unit.exe                     the real checks: write -> parse round trip, an older reader skips 68-70, a bad row drops the three together, an unflagged record is unchanged
//   snapshot_unit.exe --plant NAME        the planted twins; each must FAIL (exit 1):
//     drop_scales   section 68 is dropped from the file (the saver "forgot" it)  -> SnapshotProblem must name it (the check expects none: FAIL)
//     drop_images   section 69 dropped
//     no_flag       the writer does not set kSceneSnapshot (the sections are then not written): the round trip finds no rows
//     no_fill       the saver does not fill the created-superstar bytes from the store: the rows keep kGmNoBytes
//     no_scales     the saver does not snapshot the body scales: no wrestler would be drawn
// Build: pwsh -NoProfile -File tools\scene\build_snapshot_unit.ps1
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/renderer_scene/record_all.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/snapshot_save.h"

using namespace rdr_scene;

static int g_fail = 0;
static std::string g_plant;
static void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}

static SceneTables Synthetic(bool flagged) {
  SceneTables t;
  t.header.guest_frame = 4242;
  t.has_camera = true;
  t.camera.eye[0] = 1.f, t.camera.eye[1] = 2.f, t.camera.eye[2] = 3.f;
  t.strings = std::string(1, '\0') + "model:x";
  if (flagged && g_plant != "no_flag") t.header.flags |= kSceneSnapshot;
  BodyScaleRow a, b;
  a.object = 0x8A001000u, a.scale = 0.89453125f;
  b.object = 0x8A002000u, b.scale = 1.01f;
  t.body_scales = {a, b};
  for (int i = 0; i < 2; ++i) {
    LiquidImageRow r;
    r.map = 0x70000000u + uint32_t(i), r.object = i ? b.object : a.object, r.width = 8, r.height = 4, r.nbytes = 8 * 4 * 4, r.offset = uint64_t(i) * r.nbytes;
    t.liquid_images.push_back(r);
  }
  t.liquid_image_bytes.resize(2 * 128);
  for (size_t i = 0; i < t.liquid_image_bytes.size(); ++i) t.liquid_image_bytes[i] = uint8_t(i * 7 + 3);
  return t;
}

// Rename a section's id in the directory: what a reader that does not know it sees (an id it skips).
static bool Hide(std::vector<uint8_t>& bytes, uint32_t id) {
  SceneHeader h;
  std::memcpy(&h, bytes.data(), sizeof h);
  for (uint32_t i = 0; i < h.section_count; ++i) {
    uint8_t* p = bytes.data() + sizeof(SceneHeader) + size_t(i) * sizeof(SectionEntry);
    SectionEntry e;
    std::memcpy(&e, p, sizeof e);
    if (e.id == id) { e.id = 900 + id; std::memcpy(p, &e, sizeof e); return true; }
  }
  return false;
}
static bool Parse(const std::vector<uint8_t>& b, SceneTables& out, std::string& err) { return ParseRecord(b.data(), b.size(), out, err); }

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i)
    if (!std::strcmp(argv[i], "--plant")) g_plant = argv[i + 1];

  SceneTables src = Synthetic(true);
  std::vector<uint8_t> bytes = SerializeRecord(src);
  if (g_plant == "drop_scales") Hide(bytes, kScBodyScales);
  if (g_plant == "drop_images") Hide(bytes, kScLiquidImages);

  SceneTables got;
  std::string err;
  const bool parsed = Parse(bytes, got, err);
  Check(parsed, "a flagged record parses");
  const std::string prob = SnapshotProblem(got);
  Check(prob.empty(), "the snapshot is complete (no section missing)");
  if (!prob.empty()) std::printf("      reported: %s\n", prob.c_str());
  Check(got.body_scales.size() == 2 && got.body_scales[0].object == 0x8A001000u && got.body_scales[0].scale == 0.89453125f && got.body_scales[1].scale == 1.01f, "section 68 round trip: two rows, the objects and the scales");
  Check(got.liquid_images.size() == 2 && got.liquid_images[1].map == 0x70000001u && got.liquid_images[1].offset == 128 && got.liquid_images[1].width == 8, "section 69 round trip: two rows with their offsets");
  Check(got.liquid_image_bytes == src.liquid_image_bytes && !got.liquid_image_bytes.empty(), "section 70 round trip: the bytes are the saved bytes");
  Check((got.header.flags & kSceneSnapshot) != 0 && got.header.guest_frame == 4242 && got.has_camera && got.camera.eye[2] == 3.f, "the header flag, the guest frame and the camera survive");
  Check(SerializeRecord(got) == SerializeRecord(src) || !(g_plant.empty()), "write -> parse -> write is byte-identical");

  {   // an older reader (an id it does not know is skipped): 68-70 hidden, the rest of the record reads, nothing is reported as an error
    std::vector<uint8_t> old = SerializeRecord(src);
    Hide(old, kScBodyScales), Hide(old, kScLiquidImages), Hide(old, kScLiquidBytes);
    SceneTables o;
    std::string e2;
    const bool ok = Parse(old, o, e2);
    Check(ok && o.body_scales.empty() && o.liquid_images.empty() && o.snapshot_errors == 0 && o.has_camera && o.header.guest_frame == 4242, "an older reader skips sections 68-70 and reads the rest");
    Check(!SnapshotProblem(o).empty(), "a reader that finds the flag but no sections reports it (it never draws half silently)");
  }
  {   // a record WITHOUT the flag is written exactly as before: no 68-70
    SceneTables plain = Synthetic(false);
    std::vector<uint8_t> pb = SerializeRecord(plain);
    SceneHeader h;
    std::memcpy(&h, pb.data(), sizeof h);
    SceneTables p;
    std::string e3;
    const bool ok = Parse(pb, p, e3);
    Check(ok && p.body_scales.empty() && p.snapshot_seen == 0 && SnapshotProblem(p).empty(), "an unflagged record carries no snapshot section and has no problem");
    Check(h.section_count == 2, "an unflagged record's directory is strings + camera alone (byte-identical to before)");
  }
  {   // a bad scale drops the three together and is reported
    SceneTables bad = src;
    bad.body_scales[0].scale = std::nanf("");
    SceneTables g;
    std::string e4;
    const bool ok = Parse(SerializeRecord(bad), g, e4);
    Check(ok && g.body_scales.empty() && g.liquid_images.empty() && g.liquid_image_bytes.empty() && g.snapshot_errors == 1 && !SnapshotProblem(g).empty(), "a non-finite scale drops 68-70 together and is reported");
  }
  {   // an image range past the bytes
    SceneTables bad = src;
    bad.liquid_images[1].offset = 4096;
    SceneTables g;
    std::string e5;
    const bool ok = Parse(SerializeRecord(bad), g, e5);
    Check(ok && g.liquid_images.empty() && g.body_scales.empty() && g.snapshot_errors == 1, "an image range past the bytes drops 68-70 together");
  }
  {   // the expanded copy (what a reader of a shared record sees) keeps the sections
    SceneTables e = ExpandCrowdRuns(src);
    Check(e.body_scales.size() == 2 && e.liquid_images.size() == 2 && e.liquid_image_bytes == src.liquid_image_bytes, "ExpandInto carries the snapshot");
  }
  {   // the saver's steps (src\renderer_scene\snapshot_save.h): the body scales of the wrestler objects, the guest-model bytes the store holds
    SceneTables t;
    t.strings = std::string(1, '\0');
    const uint32_t f1 = t.AddString("actor:slot:0"), f2 = t.AddString("actor:slot:1"), f3 = t.AddString("arena:slot:7");
    for (uint32_t id : {f1, f2, f3}) { Object o; o.id = id; t.objects.push_back(o); }
    t.engine_objects = {0x8A001000u, 0x8A002000u, 0x8A003000u};
    BodyScaleTable::Get().Clear();
    BodyScaleTable::Get().Set(0x8A001000u, 0.91f);   // slot 1's object has no scale known: a hole, no row
    std::vector<BodyScaleRow> rows;
    uint32_t holes = 0;
    if (g_plant != "no_scales") snapshot::BodyScaleRows(t, rows, holes);
    Check(rows.size() == 1 && rows[0].object == 0x8A001000u && rows[0].scale == 0.91f && holes == 1, "the saver snapshots the scale of the wrestler that has one, and counts the one that has none (the arena object is no wrestler)");

    SceneTables g;
    GuestModelPart p;
    p.model = 0x8A009000u, p.part = 0, p.parts = 1, p.vertex_count = 2, p.stride = 16, p.index_count = 3, p.n0 = 3;
    p.vtx_hash = 0xAA, p.idx_hash = 0xBB, p.vtx_off = kGmNoBytes, p.idx_off = kGmNoBytes;
    g.guest_models.parts.push_back(p);
    GuestModelTexture x;
    x.hash = 0xCC, x.width = 4, x.height = 4, x.format = 18, x.nbytes = 8, x.off = kGmNoBytes;   // not in the store: stays a hole
    g.guest_models.textures.push_back(x);
    g.guest_models.blobs.assign(5, 9);   // an earlier byte count not a multiple of 8: the filled bytes are appended aligned
    auto store = [](uint64_t h) -> std::shared_ptr<const std::vector<uint8_t>> {
      if (h == 0xAA) return std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>(32, 1));
      if (h == 0xBB) return std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>(6, 2));
      return nullptr;
    };
    snapshot::FillCounts fc;
    if (g_plant != "no_fill") fc = snapshot::FillGuestBytes(g, store);
    const GuestModelPart& q = g.guest_models.parts[0];
    Check(fc.filled == 2 && fc.missing == 1 && q.vtx_off != kGmNoBytes && q.idx_off != kGmNoBytes && !(q.vtx_off & 7) && !(q.idx_off & 7) && g.guest_models.textures[0].off == kGmNoBytes,
          "the saver fills the guest-model rows from the store (aligned), and leaves a hash the store lacks as a hole");
    Check(q.vtx_off != kGmNoBytes && GuestModelRowProblem(q, g.guest_models.blobs.size())[0] == 0 && g.guest_models.blobs[q.vtx_off] == 1 && g.guest_models.blobs[q.idx_off] == 2, "the filled rows pass the reader's checks and hold the store's bytes");
    g.header.flags |= kSceneSnapshot;   // the file round trip keeps the filled bytes
    SceneTables back;
    std::string e6;
    const bool ok = Parse(SerializeRecord(g), back, e6);
    Check(ok && back.guest_models.errors == 0 && back.guest_models.parts.size() == 1 && back.guest_models.parts[0].vtx_off == q.vtx_off && back.guest_models.blobs == g.guest_models.blobs, "the filled guest-model rows survive the file");
  }
  std::printf("%s\n", g_fail ? "snapshot_unit: FAIL" : "snapshot_unit: PASS");
  return g_fail ? 1 : 0;
}
