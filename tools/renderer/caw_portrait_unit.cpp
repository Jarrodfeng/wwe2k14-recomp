// S103 CPB: the offline proof of src\renderer_game\caw_portrait_core.h (the Create a Superstar portrait's G3 untile filter + writer, G2 tick gate). 0 game runs, no GPU, no SDK.
// Built and run by tools\renderer\caw_portrait_unit.py; planted twins are compile-time (-DWWE2K14_CAW_PLANT=N), each must exit 1:
//   1 the writer's byte order is R,G,B,A instead of A,R,G,B   2 the G3 filter ignores the return address   3 G2 never counts a late portrait (N = 0 runs the original silently)
#include "../../src/renderer_game/caw_portrait_core.h"
#include "../../src/renderer_host/portrait_mailbox.h"

#include <cstdio>
#include <map>
#include <vector>

using namespace wwe2k14::caw_portrait;

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL " __VA_ARGS__); std::printf("\n"); } else { std::printf("ok   " __VA_ARGS__); std::printf("\n"); } } while (0)

// A fake guest memory: word map, big-endian already decoded.
struct Mem {
  std::map<uint32_t, uint32_t> w;
  bool operator()(uint32_t a, uint32_t& out) const {
    auto it = w.find(a);
    if (it == w.end()) return false;
    out = it->second;
    return true;
  }
};

static UntileCall Good() { return UntileCall{kUntileLr, 0x40000000, 2944, 0x70001000, 736, 1280, 0x70001010, 4}; }
static Mem GoodMem() {
  Mem m;
  m.w[0x70001000] = 0; m.w[0x70001004] = 0;                                              // dest point (0, 0)
  m.w[0x70001010] = 0; m.w[0x70001014] = 0; m.w[0x70001018] = 736; m.w[0x7000101C] = 1280;   // source rect
  return m;
}

int main() {
  {  // G3 filter: the portrait's own call passes, every other caller / argument set is refused
    Mem m = GoodMem();
    CHECK(IsPortraitUntile(Good(), m), "G3 filter accepts the portrait's own call");
    UntileCall c = Good(); c.lr = 0x8341B77C;
    CHECK(!IsPortraitUntile(c, m), "G3 filter refuses another return address");
    c = Good(); c.lr = 0x82000000;
    CHECK(!IsPortraitUntile(c, m), "G3 filter refuses an unrelated caller with the portrait's arguments");
    c = Good(); c.width = 512; CHECK(!IsPortraitUntile(c, m), "refuses width 512");
    c = Good(); c.height = 1024; CHECK(!IsPortraitUntile(c, m), "refuses height 1024");
    c = Good(); c.texel = 2; CHECK(!IsPortraitUntile(c, m), "refuses texel pitch 2");
    c = Good(); c.pitch = 2940; CHECK(!IsPortraitUntile(c, m), "refuses a pitch below one row");
    c = Good(); c.pitch = 3072; CHECK(IsPortraitUntile(c, m), "accepts a padded pitch 3072");
    c = Good(); c.dest = 0; CHECK(!IsPortraitUntile(c, m), "refuses a null destination");
    Mem m2 = GoodMem(); m2.w[0x70001000] = 4; CHECK(!IsPortraitUntile(Good(), m2), "refuses a non-zero destination point x");
    Mem m3 = GoodMem(); m3.w[0x70001018] = 700; CHECK(!IsPortraitUntile(Good(), m3), "refuses a source rect right edge 700");
    Mem m4 = GoodMem(); m4.w.erase(0x7000101C); CHECK(!IsPortraitUntile(Good(), m4), "refuses an unreadable rect word");
  }
  {  // writer: zeros over the whole destination, pitch padding included
    std::vector<uint8_t> buf(DestBytes(3072), 0xAB);
    WriteZero(buf.data(), 3072);
    bool z = true;
    for (uint8_t b : buf) z = z && b == 0;
    CHECK(z, "WriteZero clears every byte of pitch * height (alpha 0 = transparent)");
  }
  {  // writer: a known picture lands in the pitch-padded buffer, bytes A,R,G,B, padding zero
    const uint32_t pitch = 3072;
    std::vector<uint8_t> buf(DestBytes(pitch), 0xCD);
    std::vector<uint8_t> rgba(size_t(kWidth) * kHeight * 4);
    for (uint32_t y = 0; y < kHeight; ++y)
      for (uint32_t x = 0; x < kWidth; ++x) {
        uint8_t* p = &rgba[(size_t(y) * kWidth + x) * 4];
        p[0] = uint8_t(x); p[1] = uint8_t(y); p[2] = 0x33; p[3] = uint8_t(0x80 + (x & 1));   // R G B A
      }
    WritePicture(buf.data(), pitch, rgba.data(), kWidth, kHeight);
    const uint8_t* px = &buf[size_t(5) * pitch + size_t(7) * 4];
    CHECK(px[0] == 0x81 && px[1] == 7 && px[2] == 5 && px[3] == 0x33, "pixel (7,5): memory bytes A,R,G,B = 81 07 05 33 (got %02X %02X %02X %02X)", px[0], px[1], px[2], px[3]);
    const uint8_t* last = &buf[size_t(kHeight - 1) * pitch + size_t(kWidth - 1) * 4];
    CHECK(last[1] == uint8_t(kWidth - 1) && last[2] == uint8_t(kHeight - 1), "last pixel lands at row 1279, column 735");
    bool pad = true;
    for (uint32_t y = 0; y < kHeight; ++y)
      for (uint32_t i = kMinPitch; i < pitch; ++i) pad = pad && buf[size_t(y) * pitch + i] == 0;
    CHECK(pad, "the row padding is zeroed");
  }
  {  // a picture of another size is nearest-scaled, no overrun; no picture = zeros
    std::vector<uint8_t> buf(DestBytes(2944), 0x11);
    std::vector<uint8_t> small(4 * 4 * 4, 0xFF);
    WritePicture(buf.data(), 2944, small.data(), 4, 4);
    CHECK(buf[0] == 0xFF && buf[3] == 0xFF, "a 4x4 picture fills the destination (nearest)");
    WritePicture(buf.data(), 2944, nullptr, 0, 0);
    CHECK(buf[0] == 0 && buf[buf.size() - 1] == 0, "no picture writes zeros");
  }
  {  // G2: the tick gate
    Census c;
    TickGate g;
    CHECK(g.Step(2, true, 5, c) == TickAct::Run, "G2 runs a state other than 4 untouched");
    CHECK(g.Step(4, false, 5, c) == TickAct::Run, "G2 runs state 4 at once when no request is pending");
    int held = 0;
    TickAct a = TickAct::Hold;
    for (int i = 0; i < 20 && (a = g.Step(4, true, 5, c)) == TickAct::Hold; ++i) ++held;
    CHECK(held == 5 && a == TickAct::Run && c.late == 1, "G2 holds exactly 5 ticks while pending, then runs and counts one late portrait (held %d late %u)", held, c.late);
    Census c0;
    TickGate g0;
    CHECK(g0.Step(4, true, 0, c0) == TickAct::Run && c0.late == 1, "G2 with N = 0 runs at once and counts late (late %u)", c0.late);
    Census c1;
    TickGate g1;
    g1.Step(4, true, 5, c1); g1.Step(4, true, 5, c1);
    CHECK(g1.Step(4, false, 5, c1) == TickAct::Run && c1.late == 0, "G2 releases the moment the picture lands (no late)");
    CHECK(g1.waited == 0, "G2's wait restarts for the next portrait");
  }
  {  // the mailbox between the game thread and the host
    wwe2k14::scene_host::PortraitMailbox mb;
    using wwe2k14::scene_host::PortraitStatus;
    wwe2k14::scene_host::PortraitRequest rq;
    rq.model = 0x1234;
    const uint64_t id = mb.Post(rq);
    CHECK(mb.HasPending() && mb.Poll(id, nullptr) == PortraitStatus::Pending, "a posted request is pending");
    wwe2k14::scene_host::PortraitRequest got;
    CHECK(mb.TakePending(got) && got.id == id && got.model == 0x1234 && mb.Poll(id, nullptr) == PortraitStatus::Pending, "the host takes it and it stays pending until published");
    wwe2k14::scene_host::PortraitResult res;
    res.id = id, res.ok = true, res.width = 2, res.height = 1, res.rgba = {1, 2, 3, 4, 5, 6, 7, 8};
    mb.Publish(std::move(res));
    CHECK(!mb.HasPending(), "published: nothing pending");
    wwe2k14::scene_host::PortraitResult out;
    CHECK(mb.Poll(id, nullptr) == PortraitStatus::Ready && mb.Poll(id, &out) == PortraitStatus::Ready && out.rgba.size() == 8 && mb.Poll(id, nullptr) == PortraitStatus::None, "the result is read once");
    const uint64_t id2 = mb.Post(rq), id3 = mb.Post(rq);
    wwe2k14::scene_host::PortraitResult late;
    late.id = id2, late.ok = true;
    mb.Publish(std::move(late));
    CHECK(id3 == id2 + 1 && mb.Poll(id2, nullptr) == PortraitStatus::None && mb.Poll(id3, nullptr) == PortraitStatus::Pending, "a newer request supersedes an older one; a late answer to the old one is dropped");
    wwe2k14::scene_host::PortraitResult fail;
    fail.id = id3, fail.ok = false;
    mb.Publish(std::move(fail));
    CHECK(mb.Poll(id3, nullptr) == PortraitStatus::Failed, "a failed answer reads as Failed");
  }
  {  // D1: only the edited wrestler is drawn
    const uint32_t engine[5] = {0x1000, 0x2000, 0, 0x2000, 0x3000};   // a record: three wrestlers, an object with no engine address
    uint32_t kept = 0;
    for (uint32_t e : engine) kept += KeepsObject(e, 0x2000) ? 1 : 0;
    CHECK(kept == 2, "D1 keeps only the objects of the edited model (%u kept of 5; the model's body + one more object of the same engine address)", kept);
    CHECK(!KeepsObject(0, 0), "D1 keeps nothing when the model is 0 (an unnamed object never matches)");
    CHECK(DrawnCountOk(1) && !DrawnCountOk(0) && !DrawnCountOk(2), "D1 requires exactly one drawn object");
  }
  {  // CPB2: run 021's numbers. r31 (the character) = 2863738400 = 0xAAB12E20; the CAW census / record key is the skeleton 0xAAB142F0 = [char+48]
    const uint32_t character = 2863738400u;
    CHECK(character == 0xAAB12E20u, "2863738400 is 0xAAB12E20");
    const uint32_t census_objs[3] = {0xA0001000u, 0xAAB142F0u, 0};   // the record's engine objects (the body is the CAW census model AAB142F0)
    uint32_t kept_char = 0, kept_skel = 0;
    for (uint32_t e : census_objs) kept_char += KeepsObject(e, character) ? 1 : 0;
    auto rd = [&](uint32_t a, uint32_t& v) { if (a == character + kCharSkeletonOff) { v = 0xAAB142F0u; return true; } return false; };
    const uint32_t key = ModelKey(character, rd);
    for (uint32_t e : census_objs) kept_skel += KeepsObject(e, key) ? 1 : 0;
    CHECK(kept_char == 0, "the old key (r31 itself) matched nothing in the record (the run 021 miss)");
    CHECK(key == 0xAAB142F0u && kept_skel == 1, "the key [char+48] = 0xAAB142F0 matches exactly the CAW body (key %08X, kept %u)", key, kept_skel);
    CHECK(ModelKey(character, [](uint32_t, uint32_t&) { return false; }) == 0 && ModelKey(0, rd) == 0, "an unreadable skeleton / null character gives no key");
    const std::string c = CandidatesText(census_objs, 3);
    CHECK(c == "3 engine object(s): A0001000 AAB142F0", "the miss line lists the candidates (%s)", c.c_str());
  }
  {  // retries are per request id; Cancel stops the host asking
    RetryGate rg;
    uint32_t allowed = 0;
    while (rg.Allow(7, 5)) ++allowed;
    CHECK(allowed == 4, "the retry window ends after max tries (allowed %u asks before the last)", allowed);
    CHECK(rg.Allow(8, 5), "a new request id restarts the count (it was exhausted by id 7)");
    rg.Done();
    CHECK(rg.tries == 0, "Done clears the count");
    wwe2k14::scene_host::PortraitMailbox mb;
    wwe2k14::scene_host::PortraitRequest rq;
    const uint64_t id = mb.Post(rq);
    CHECK(!mb.Cancel(id + 1) && mb.Poll(id, nullptr) == wwe2k14::scene_host::PortraitStatus::Pending, "Cancel of another id changes nothing");
    CHECK(mb.Cancel(id) && !mb.HasPending() && mb.Poll(id, nullptr) == wwe2k14::scene_host::PortraitStatus::None, "Cancel removes the pending request: the host stops asking");
    wwe2k14::scene_host::PortraitRequest none;
    CHECK(!mb.TakePending(none), "nothing left to take after a cancel");
  }
  std::printf(g_fail ? "caw_portrait_unit: FAIL (%d)\n" : "caw_portrait_unit: PASS\n", g_fail);
  return g_fail ? 1 : 0;
}
