// Lane 67's unit test of the shipped run key and packer (src/renderer_adapter/particle_runs.h), on a REAL frame of a capture, with doctored twins.
//   runs_test CAPTURE_DIR [FRAME]       (the default frame 224: the pyro, 2,750 particle draws in 37 runs)
// No GPU, no game. Every line is PASS or FAIL; exit code 1 on any FAIL. A check that cannot read its input FAILs loudly.
//   1. the clean key finds runs in the frame (the first run has at least 3 members);
//   2. DOCTORED: the second member's ZOffset (c200.x) changed -> the clean key splits the run (length 1); the planted `zoffset_masked` (masks c200.x) does NOT split it (the
//      fault exists and would merge two emitters with different ZOffset: the review's correction);
//   3. DOCTORED: a NaN in c200.y of the second member (what the game leaves in the sprites) -> the clean key keeps the run; the planted `float_key` splits it;
//   4. DOCTORED: a different LocalWorld row in the second member -> the clean key keeps the run (that is what a run is);
//   5. the packer: stride 96, 6 indices a quad (0,1,2 / 2,1,3), the LocalWorld rows are the member's own four (64 bytes); `drop_last` packs one particle less,
//      `strip_as_list` changes the second triangle, `world_first` copies the first member's rows to all.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/renderer_adapter/particle_runs.h"

using namespace rdr_adapter;
using namespace rdr_adapter::particles;

static int g_fail = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}

static size_t RunLength(const LoadedFrame& frame, uint32_t op, Fault fault) {
  Census census;
  std::vector<uint32_t> members;
  FindRun(frame, op, fault, census, members);
  return members.size();
}

// S101 lane 132 (review 4): the soft variant's cases, on a REAL frame of the CAW capture (every particle draw there is ySoftParticle): `runs_test CAW_DIR FRAME soft`.
//   S1 the clean key finds a soft run of at least 3; the member is Kind::SoftRun;
//   S2 DOCTORED c204 (LocalWorldView) of the 2nd member: still one run (that is what a soft run is);
//   S3 DOCTORED c202 (LightVector) / c207 (projection rows): the clean key splits the run; PLANT soft_mask_wide (masks c201-c227) does NOT split it (so it fails the case);
//   S4 the packer: stride 152, the member's own LocalWorld at +24 and LocalWorldView (c203-c206) at +88; PLANTS view_first and soft_rows_at_32 change the vertices;
//   S5 DOCTORED call site 0x82A0ED2C (the third method) on a soft draw: Kind::Third, never a run (review 6).
static int SoftSection(CaptureReader& capture, LoadedFrame& frame) {
  uint32_t first_op = ~0u;
  for (uint32_t i = 0; i < frame.ops.count && first_op == ~0u; ++i) {
    if (frame.ops[i].kind != OpKind::Draw || Classify(frame, frame.ops[i].index) != Kind::SoftRun) continue;
    if (RunLength(frame, i, Fault::None) >= 3) first_op = i;
  }
  Check(first_op != ~0u, "S1. the clean key finds a soft run of at least 3 particles (Kind::SoftRun)");
  if (first_op == ~0u) return 1;
  Census census;
  std::vector<uint32_t> members;
  FindRun(frame, first_op, Fault::None, census, members);
  const size_t clean = members.size();
  std::printf("      soft run at op %u: %zu members\n", first_op, clean);
  const Draw& second = frame.draws[frame.ops[members[1]].index];
  ConstantBank& bank = const_cast<ConstantBank&>(frame.constant_banks[second.vs_bank]);
  const ConstantBank saved = bank;
  if (&bank == &frame.constant_banks[frame.draws[frame.ops[members[0]].index].vs_bank]) {
    std::printf("FAIL  the first two members share a constant bank: cannot doctor one\n");
    return 1;
  }
  const float v = 321.25f;
  std::memcpy(&bank.dwords[(kLocalWorldViewReg + 1) * 4 + 2], &v, 4);  // c204
  Check(RunLength(frame, first_op, Fault::None) == clean, "S2. DOCTORED c204 (LocalWorldView) of the 2nd member: still one run");
  bank = saved;
  for (uint32_t reg : {202u, 207u}) {
    std::memcpy(&bank.dwords[reg * 4 + 1], &v, 4);
    const std::string name = "S3. DOCTORED c" + std::to_string(reg);
    Check(RunLength(frame, first_op, Fault::None) == 1, (name + " of the 2nd member: the clean key splits the run").c_str());
    Check(RunLength(frame, first_op, Fault::SoftMaskWide) == clean, (name + ": PLANT soft_mask_wide does NOT split it (the plant fails this case)").c_str());
    bank = saved;
  }
  const auto read = [&](const Draw& d, std::vector<uint8_t>& out) { return capture.ReadGeometry(d.vertex_offset, d.vertex_bytes, out); };
  Packed packed, vfirst, at32;
  std::vector<uint8_t> scratch;
  const bool ok = Pack(frame, members, Fault::None, read, packed, scratch, true);
  Check(ok && packed.particles == clean && packed.vertices.size() == clean * 4 * kSoftMergedStride && packed.indices.size() == clean * 6, "S4a. the soft packer: K particles, stride 152, 6 indices a quad");
  bool rows_ok = ok;
  for (uint32_t p = 0; ok && p < packed.particles; ++p) {
    const Draw& d = frame.draws[frame.ops[members[p]].index];
    const uint8_t* vtx = &packed.vertices[size_t(p) * 4 * kSoftMergedStride];
    rows_ok = rows_ok && std::memcmp(vtx + kSoftVertexBytes, &frame.constant_banks[d.vs_bank].dwords[kLocalWorldReg * 4], 64) == 0 &&
              std::memcmp(vtx + kSoftViewOffset, &frame.constant_banks[d.vs_bank].dwords[kLocalWorldViewReg * 4], 64) == 0;
  }
  Check(rows_ok, "S4b. every vertex carries its own particle's LocalWorld (+24) and LocalWorldView (+88) rows");
  Pack(frame, members, Fault::ViewFirst, read, vfirst, scratch, true);
  Check(vfirst.vertices != packed.vertices, "S4c. PLANT view_first changes the vertices");
  Pack(frame, members, Fault::SoftRowsAt32, read, at32, scratch, true);
  Check(at32.vertices != packed.vertices, "S4d. PLANT soft_rows_at_32 changes the vertices");
  DrawOrigin& origin = const_cast<DrawOrigin&>(frame.origins[frame.ops[first_op].index]);
  const uint32_t site = origin.call_site;
  origin.call_site = kThirdSite;
  Check(Classify(frame, frame.ops[first_op].index) == Kind::Third, "S5. DOCTORED call site = the third method on a soft draw: Kind::Third (stays the stream's)");
  origin.call_site = site;
  Check(Classify(frame, frame.ops[first_op].index) == Kind::SoftRun, "S5b. undone: SoftRun again");
  std::printf("%s: %d failure(s)\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("FAIL  usage: runs_test CAPTURE_DIR [FRAME]\n");
    return 1;
  }
  const uint32_t frame_number = argc > 2 ? uint32_t(std::atoi(argv[2])) : 224;
  CaptureReader capture;
  if (!capture.Open(argv[1])) {
    std::printf("FAIL  cannot open the capture %s\n", argv[1]);
    return 1;
  }
  LoadedFrame frame;
  if (!capture.LoadFrame(frame_number, frame)) {
    std::printf("FAIL  cannot load frame %u\n", frame_number);
    return 1;
  }
  if (argc > 3 && std::string(argv[3]) == "soft") return SoftSection(capture, frame);  // S101 lane 132
  // The first supported particle draw whose run holds at least 3 members.
  uint32_t first_op = ~0u;
  for (uint32_t i = 0; i < frame.ops.count && first_op == ~0u; ++i) {
    if (frame.ops[i].kind != OpKind::Draw || Classify(frame, frame.ops[i].index) != Kind::Supported) continue;
    if (RunLength(frame, i, Fault::None) >= 3) first_op = i;
  }
  Check(first_op != ~0u, "1. the clean key finds a run of at least 3 particles in the frame");
  if (first_op == ~0u) return 1;
  Census census;
  std::vector<uint32_t> members;
  FindRun(frame, first_op, Fault::None, census, members);
  const size_t clean = members.size();
  std::printf("      run at op %u: %zu members\n", first_op, clean);
  const Draw& second = frame.draws[frame.ops[members[1]].index];
  ConstantBank& bank = const_cast<ConstantBank&>(frame.constant_banks[second.vs_bank]);  // (the test's doctoring: the frame's bytes are ours here)
  const ConstantBank saved = bank;
  ConstantBank& bank0 = const_cast<ConstantBank&>(frame.constant_banks[frame.draws[frame.ops[members[0]].index].vs_bank]);
  const ConstantBank saved0 = bank0;
  if (&bank == &bank0) {
    std::printf("FAIL  the first two members share a constant bank: cannot doctor one\n");
    return 1;
  }

  // 2. ZOffset
  float one = 1.0f;
  std::memcpy(&bank.dwords[kZOffsetReg * 4], &one, 4);
  Check(RunLength(frame, first_op, Fault::None) == 1, "2a. DOCTORED c200.x (ZOffset) of the 2nd member: the clean key splits the run");
  Check(RunLength(frame, first_op, Fault::ZOffsetMasked) == clean, "2b. PLANT zoffset_masked: the same doctored frame is NOT split (the plant merges different ZOffsets)");
  bank = saved;
  Check(RunLength(frame, first_op, Fault::None) == clean, "2c. the doctoring undone: the clean run is whole again");

  // 3. the NaN
  const uint32_t nan_bits = 0xFFFFE2B4u;
  std::memcpy(&bank.dwords[kZOffsetReg * 4 + 1], &nan_bits, 4);
  std::memcpy(&bank0.dwords[kZOffsetReg * 4 + 1], &nan_bits, 4);
  Check(RunLength(frame, first_op, Fault::None) == clean, "3a. DOCTORED c200.y = the game's NaN in both: the clean key (bytes) keeps the run");
  Check(RunLength(frame, first_op, Fault::FloatKey) == 1, "3b. PLANT float_key: NaN != NaN splits the run");
  bank = saved;
  bank0 = saved0;

  // 4. LocalWorld
  const float rows = 123.5f;
  std::memcpy(&bank.dwords[kLocalWorldReg * 4 + 5], &rows, 4);
  Check(RunLength(frame, first_op, Fault::None) == clean, "4. DOCTORED LocalWorld row of the 2nd member: still one run");
  bank = saved;

  // 5. the packer
  const auto read = [&](const Draw& d, std::vector<uint8_t>& out) { return capture.ReadGeometry(d.vertex_offset, d.vertex_bytes, out); };
  Packed packed;
  std::vector<uint8_t> scratch;
  bool ok = Pack(frame, members, Fault::None, read, packed, scratch);
  Check(ok && packed.particles == clean && packed.vertices.size() == clean * 4 * kMergedStride && packed.indices.size() == clean * 6, "5a. the packer: K particles, stride 96, 6 indices a quad");
  bool idx_ok = ok;
  for (uint32_t p = 0; ok && p < packed.particles; ++p) {
    const uint16_t b = uint16_t(p * 4);
    const uint16_t want[6] = {b, uint16_t(b + 1), uint16_t(b + 2), uint16_t(b + 2), uint16_t(b + 1), uint16_t(b + 3)};
    idx_ok = idx_ok && std::memcmp(&packed.indices[p * 6], want, 12) == 0;
  }
  Check(idx_ok, "5b. the indices are the strip's triangles (0,1,2) and (2,1,3)");
  bool rows_ok = ok;
  for (uint32_t p = 0; ok && p < packed.particles; ++p) {
    const Draw& d = frame.draws[frame.ops[members[p]].index];
    for (uint32_t v = 0; v < 4; ++v)
      rows_ok = rows_ok && std::memcmp(&packed.vertices[(size_t(p) * 4 + v) * kMergedStride + kLocalWorldOffset], &frame.constant_banks[d.vs_bank].dwords[kLocalWorldReg * 4], 64) == 0;
  }
  Check(rows_ok, "5c. every vertex carries its own particle's four LocalWorld rows (c228-c231, 64 bytes)");
  Packed dropped, strip, world;
  Pack(frame, members, Fault::DropLast, read, dropped, scratch);
  Check(dropped.particles == clean - 1, "5d. PLANT drop_last packs one particle less");
  Pack(frame, members, Fault::StripAsList, read, strip, scratch);
  Check(strip.indices.size() == packed.indices.size() && std::memcmp(strip.indices.data(), packed.indices.data(), strip.indices.size() * 2) != 0, "5e. PLANT strip_as_list changes the indices");
  Pack(frame, members, Fault::WorldFirst, read, world, scratch);
  Check(world.vertices != packed.vertices, "5f. PLANT world_first changes the vertices");
  // 6. F2: a 4-vertex draw that is not a strip (a fan: source prim 5 / topology 5) is not a particle run member.
  {
    Draw& d0 = const_cast<Draw&>(frame.draws[frame.ops[first_op].index]);
    const uint8_t prim = d0.source_prim;
    Check(Classify(frame, frame.ops[first_op].index) == Kind::Supported, "6a. the clean draw is Supported");
    d0.source_prim = 5;
    Check(Classify(frame, frame.ops[first_op].index) == Kind::Other, "6b. DOCTORED source prim 5 (a fan-shaped draw): Other, never merged");
    d0.source_prim = prim;
    DrawFixed& fx = const_cast<DrawFixed&>(frame.fixeds[d0.fixed]);
    const uint8_t topology = fx.state.topology;
    fx.state.topology = 5;
    Check(Classify(frame, frame.ops[first_op].index) == Kind::Other, "6c. DOCTORED topology 5 (fan): Other");
    fx.state.topology = topology;
    // F3: a vertex element somewhere else than +0 / +12 / +16 / +24 could overlap the LocalWorld rows: not a particle shape.
    VertexElement& el = const_cast<VertexElement&>(frame.elements[frame.layouts[d0.layout].element_first + 3]);
    const uint16_t offset = el.host_offset;
    el.host_offset = 28;
    Check(Classify(frame, frame.ops[first_op].index) == Kind::Other, "6d. DOCTORED element offset 28 (would overrun the 32-byte vertex): Other");
    el.host_offset = offset;
    Check(Classify(frame, frame.ops[first_op].index) == Kind::Supported, "6e. all doctoring undone: Supported again");
  }
  std::printf("%s: %d failure(s)\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}
