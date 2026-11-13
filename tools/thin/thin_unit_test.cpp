// Session 101 lane 122 (KR): the unit test of the SHIPPED headers src\renderer_adapter\{thin_draw,reader_thin,thin_store}.h and src\scene_live\span_timer.h. No GPU, no game.
//   thin_unit_test.exe                 every check on the honest build: exit 0 or 1
//   thin_unit_test.exe plant NAME      the same check with one planted fault: must exit 1 (the gate requires it)
// Plants: release_without_ack (R: lane 50's hole: a template dropped while frames that need it are in flight), stale_generation (the reader decides from an older set
// than it stamps), descriptor_stride_64 (the 68-byte descriptor stride read as 64).
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "../../src/renderer_adapter/reader_thin.h"
#include "../../src/renderer_adapter/thin_store.h"
#include "../../src/scene_live/span_timer.h"

using namespace rdr_adapter::thin;

namespace {

int g_failed = 0;
void Check(bool ok, const char* what, const char* detail = "") {
  std::printf("%s  %s %s\n", ok ? "PASS" : "FAIL", what, detail);
  if (!ok) ++g_failed;
}

struct Rng {
  uint64_t s;
  uint32_t Next() {
    s ^= s << 13, s ^= s >> 7, s ^= s << 17;
    return uint32_t(s >> 11);
  }
};

// ---- the two-thread handshake ---------------------------------------------------------------------------------------------------------------------------------------
struct DrawRec {
  uint32_t site, object, part;
  uint64_t key;   // the binder's key (ComputeKeys), 0 = not a candidate
  bool lite;
};
struct FrameRec {
  uint64_t frame;
  uint32_t stamp;
  std::vector<DrawRec> draws;
};

// A scene of ~120 draws: 40 objects, parts 0..2, some triples twice (ordinal 1), both sites.
std::vector<std::array<uint32_t, 3>> Scene() {
  std::vector<std::array<uint32_t, 3>> d;
  for (uint32_t o = 0; o < 40; ++o)
    for (uint32_t p = 0; p < 3; ++p) {
      d.push_back({o % 3 ? kSiteColour : kSiteMirror, 0x1000 + o, p});
      if ((o + p) % 5 == 0) d.push_back({o % 3 ? kSiteColour : kSiteMirror, 0x1000 + o, p});  // the same triple again: ordinal 1
    }
  return d;
}

struct HandshakeResult {
  uint64_t frames = 0, lite = 0, holes = 0, learned = 0, unpinned = 0, released = 0;
};

HandshakeResult RunHandshake(uint32_t frames, const std::string& plant) {
  PinPublisher pub;
  ThinStore store(&pub);
  store.SetReleaseFrames(0);  // the handshake alone must be sound (the 300-frame minimum is on top of it)
  if (plant == "release_without_ack") store.SetFault(Fault::ReleaseWithoutAck);
  const bool stale = plant == "stale_generation";
  const auto scene = Scene();

  std::mutex mu;
  std::condition_variable cv;
  std::deque<FrameRec> queue;
  std::atomic<bool> done{false};
  std::atomic<uint64_t> produced{0}, consumed{0};
  HandshakeResult r;

  std::thread reader([&] {  // the game's render thread: frame rate, takes the newest set at each frame
    FrameLite lite;
    std::vector<std::shared_ptr<const PinSet>> history;  // the stale plant decides from a set 3 frames old
    for (uint32_t f = 0; f < frames; ++f) {
      {  // a binder 1-3 frames behind: the reader waits when it is more than 3 ahead (the live queue's depth)
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [&] { return produced.load() - consumed.load() < 3; });
      }
      auto snap = pub.Acquire();
      history.push_back(snap);
      if (history.size() > 4) history.erase(history.begin());
      FrameRec fr;
      fr.frame = f;
      if (stale && history.size() >= 4) lite.Begin(history.front(), f, kProofK, snap ? int64_t(snap->generation) : 0);  // decides from the old set, stamps the newest
      else lite.Begin(snap, f);
      fr.stamp = lite.stamp();
      for (const auto& d : scene) {
        DrawRec rec{d[0], d[1], d[2], 0, false};
        rec.lite = lite.Query(d[0], d[1], d[2]);
        fr.draws.push_back(rec);
      }
      {
        std::lock_guard<std::mutex> lock(mu);
        queue.push_back(std::move(fr));
        ++produced;
      }
      cv.notify_all();
    }
    {
      std::lock_guard<std::mutex> lock(mu);
      done = true;
    }
    cv.notify_all();
  });

  std::thread binder([&] {  // our render thread: binds each frame, learns, unpins at times, publishes
    Rng rng{0x9E3779B97F4A7C15ull};
    for (;;) {
      FrameRec fr;
      {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [&] { return !queue.empty() || done.load(); });
        if (queue.empty()) break;
        fr = std::move(queue.front());
        queue.pop_front();
      }
      // a binder 1-3 frames behind: a little work per frame
      for (uint32_t spin = rng.Next() % 400; spin; --spin) std::this_thread::yield();  // (a sleep rounds to a scheduler tick on Windows: 100k frames would take minutes)
      store.BeginFrame(fr.frame, fr.stamp);
      std::vector<uint64_t> keys;
      std::vector<ThinKey> full;
      ComputeKeys(uint32_t(fr.draws.size()), [&](uint32_t i, uint32_t& s, uint32_t& o, uint32_t& p, uint64_t&) { s = fr.draws[i].site, o = fr.draws[i].object, p = fr.draws[i].part; return true; }, keys, &full);
      for (size_t i = 0; i < fr.draws.size(); ++i) {
        if (fr.draws[i].lite) {
          ++r.lite;
          if (!store.Find(keys[i])) ++r.holes;
        } else {
          ThinTemplate t;
          t.vs = 1, t.ps = 2;
          if (!store.Known(keys[i])) {
            if (store.Learn(full[i], t)) ++r.learned;
          } else if (store.Pinned(keys[i]) && IsProofFrame(keys[i], fr.frame)) {
            store.Find(keys[i]);  // a proof frame: the compare would run here
          }
        }
      }
      if (fr.frame % 7 == 3) {  // churn: a failed check unpins one pinned key
        const auto pinned = store.PinnedKeys();
        if (!pinned.empty() && store.Unpin(pinned[rng.Next() % pinned.size()])) ++r.unpinned;
      }
      store.EndFrame();
      ++r.frames;
      {
        std::lock_guard<std::mutex> lock(mu);
        ++consumed;
      }
      cv.notify_all();
    }
  });
  reader.join();
  binder.join();
  r.released = store.released();
  return r;
}

// ---- fake guest memory for the span timer ---------------------------------------------------------------------------------------------------------------------------
struct FakeMem {
  std::unordered_map<uint32_t, uint32_t> words;
  uint32_t Ld32(uint32_t a) const {
    const auto it = words.find(a);
    return it == words.end() ? 0 : it->second;
  }
  void St32(uint32_t a, uint32_t v) { words[a] = v; }
};

// The list as the game builds it: G at 0x8392D9B8, descriptors (68 bytes) at G+104's pointer, the per-thread buffer number through G+156's pointer and the thread block.
struct FakeList {
  FakeMem mem;
  static constexpr uint32_t kG = 0x70000000, kDescs = 0x70010000, kIdx = 0x70020000, kR13 = 0x70030000, kBlock = 0x70040000, kThread = 0x70050000, kBuf0 = 0x71000000, kBuf1 = 0x72000000;
  FakeList() {
    mem.St32(kListGlobal_(), kG);
    mem.St32(kG + 156, kIdx), mem.St32(kG + 104, kDescs);
    mem.St32(kR13, kBlock), mem.St32(kBlock + 1048, kThread);
    mem.St32(kThread + 32, 1);        // this thread's index
    mem.St32(kIdx + 4 * 1, 2);        // index 1 -> buffer number 2
    mem.St32(kIdx + 4 * 0, 1);        // (another thread's: buffer 1)
    mem.St32(kDescs + 68 * 1 + 0, kBuf0), mem.St32(kDescs + 68 * 1 + 4, 5);   // buffer 1 (not ours), cursor 5
    mem.St32(kDescs + 68 * 2 + 0, kBuf1), mem.St32(kDescs + 68 * 2 + 4, 0);   // buffer 2 (ours), cursor 0
  }
  static uint32_t kListGlobal_() { return wwe2k14::spans::kListGlobal; }
  uint32_t& Cursor() { return mem.words[kDescs + 68 * 2 + 4]; }
  // sub_82BB1CF8's write: tag at +0, payload units at +8, cursor += payload + 1
  void Append(uint32_t tag, uint32_t payload_units) {
    const uint32_t at = Cursor();
    const uint32_t rec = kBuf1 + 16 * at;
    mem.St32(rec, tag), mem.St32(rec + 8, payload_units);
    Cursor() = at + payload_units + 1;
  }
};

bool SpanCheck(uint32_t stride) {
  using namespace wwe2k14::spans;
  FakeList list;
  g_on = true;
  // A span that writes: 3 x tag 21 (2 units), 2 x tag 28 (4 units), 1 x tag 15 (0 units) = 6 records, 3*3 + 2*5 + 1 = 20 units.
  {
    CursorSpan<FakeMem> span(list.mem, FakeList::kR13, stride);
    for (int i = 0; i < 3; ++i) list.Append(21, 2);
    for (int i = 0; i < 2; ++i) list.Append(28, 4);
    list.Append(15, 0);
    span.Finish(0);
  }
  // a second span of the same shape, then a span that wrote nothing
  {
    CursorSpan<FakeMem> span(list.mem, FakeList::kR13, stride);
    list.Append(21, 2);
    list.Append(35, 1);
    span.Finish(1);
  }
  {
    CursorSpan<FakeMem> span(list.mem, FakeList::kR13, stride);
    span.Finish(2);
  }
  const bool ok = g_totals.spans[0] == 1 && g_totals.records[0] == 6 && g_totals.units[0] == 20 && g_totals.by_tag[0][21] == 3 && g_totals.by_tag[0][28] == 2 &&
                  g_totals.by_tag[0][15] == 1 && g_totals.spans[1] == 1 && g_totals.records[1] == 2 && g_totals.units[1] == 5 && g_totals.spans[2] == 1 && g_totals.records[2] == 0 &&
                  g_totals.walk_bad[0] == 0 && g_totals.walk_bad[1] == 0 && g_totals.no_cursor[0] == 0;
  g_on = false;
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  std::string plant;
  if (argc >= 3 && std::strcmp(argv[1], "plant") == 0) plant = argv[2];

  // 1. the keys: the reader's ordinals equal the binder's, on a scene with repeated triples
  {
    PinPublisher pub;
    const auto scene = Scene();
    std::vector<uint64_t> keys, triples;
    std::vector<uint64_t> binder_keys;
    ComputeKeys(uint32_t(scene.size()), [&](uint32_t i, uint32_t& s, uint32_t& o, uint32_t& p, uint64_t&) { s = scene[i][0], o = scene[i][1], p = scene[i][2]; return true; }, binder_keys);
    for (size_t i = 0; i < scene.size(); ++i) keys.push_back(binder_keys[i]), triples.push_back(TripleHash(scene[i][0], scene[i][1], scene[i][2]));
    pub.Publish(keys, triples);
    FrameLite lite;
    lite.Begin(pub.Acquire(), 1, 0 /* no proof frames */);
    uint32_t lite_n = 0;
    for (const auto& d : scene) lite_n += lite.Query(d[0], d[1], d[2]);
    Check(lite_n == scene.size(), "reader ordinals = binder ordinals (every pinned draw of a repeated-triple scene goes lite)", (std::string("lite ") + std::to_string(lite_n) + " of " + std::to_string(scene.size())).c_str());
    lite.Begin(pub.Acquire(), 1);  // proof frames on: about 1 in 32 keys takes the full path
    uint32_t lite_k = 0;
    for (const auto& d : scene) lite_k += lite.Query(d[0], d[1], d[2]);
    Check(lite_k < scene.size() && lite_k * 100 > scene.size() * 85ull, "proof frames hand some keys back to the full path, most stay lite");
    PinPublisher empty;
    lite.Begin(empty.Acquire(), 1);
    uint32_t none = 0;
    for (const auto& d : scene) none += lite.Query(d[0], d[1], d[2]);
    Check(none == 0, "an empty pin set makes nothing lite");
  }

  // 2. the two-thread handshake (100k frames)
  if (plant.empty() || plant == "release_without_ack" || plant == "stale_generation") {
    const uint32_t frames = plant.empty() ? 100000 : 20000;
    const HandshakeResult r = RunHandshake(frames, plant);
    char text[256];
    std::snprintf(text, sizeof text, "(%llu frames, %llu lite draws, learned %llu, unpinned %llu, released %llu, holes %llu)", (unsigned long long)r.frames,
                  (unsigned long long)r.lite, (unsigned long long)r.learned, (unsigned long long)r.unpinned, (unsigned long long)r.released, (unsigned long long)r.holes);
    Check(r.frames == frames && r.lite > 1000, "handshake ran and the reader went lite", text);
    Check(r.unpinned > 100 && r.released > 100, "churn happened: keys were unpinned and released", text);
    Check(r.holes == 0, "holes 0 (no lite draw met a missing template)", text);
  }

  // 3. the descriptor reader on fake guest memory
  {
    const uint32_t stride = plant == "descriptor_stride_64" ? 64 : wwe2k14::spans::kDescriptorStride;
    Check(SpanCheck(stride), "span timer: records / units / tags per span on fake guest memory (68-byte descriptors)");
  }

  // 4. review 2: the arena dropped an object's join: every pinned key of that object leaves in the same call (siblings), the other objects keep theirs
  {
    PinPublisher pub;
    ThinStore store(&pub);
    if (plant == "object_sibling") store.SetFault(Fault::ObjectSibling);
    for (uint32_t part = 0; part < 3; ++part) store.Learn(ThinKey{kSiteColour, 0x2000, part, 0, 7}, ThinTemplate{});
    for (uint32_t part = 0; part < 2; ++part) store.Learn(ThinKey{kSiteColour, 0x3000, part, 0, 7}, ThinTemplate{});
    store.BeginFrame(1, 0);
    store.EndFrame();
    const uint32_t gone = store.UnpinObject(0x2000);
    store.EndFrame();
    Check(gone == 3 && store.PinnedKeys().size() == 2, "dropping an object's join unpins all its keys (3), not the other object's (2 stay)", plant == "object_sibling" ? "(planted: only the first key leaves)" : "");
  }

  // 5. lane 289 (the ring SKIP arm): a skipped call's draws never arrive. Four instances of one model (two parts each) are called back to back; instance 1 and 3 are CUT (their draws absent, a marker in their place). The reader's
  // Reserve and the binder's ComputeKeys fill count the marker's part triples at its position, so every KEPT instance (0 and 2) has the ordinal it has in the uncut stream (0 and 2), and the two sides agree. The plant `no_fill`
  // leaves the cut instances out of both counts (today's live code before lane 289): instance 2 is numbered 1.
  {
    const uint32_t kSite = kSiteColour, kObj = 0x5000;
    struct D { uint32_t part; int instance; bool marker; };
    std::vector<D> stream;   // the stream AS LIVE: kept instances' draws, and one marker (part = unused) for each cut instance
    for (int inst = 0; inst < 4; ++inst) {
      if (inst == 1 || inst == 3) stream.push_back({0, inst, true});
      else
        for (uint32_t part = 0; part < 2; ++part) stream.push_back({part, inst, false});
    }
    const std::vector<uint64_t> instance_triples = {TripleHash(kSite, kObj, 0), TripleHash(kSite, kObj, 1)};   // what the mark set carries for the call
    // the uncut stream's keys (the oracle: what the templates were learned under)
    std::vector<uint64_t> uncut_keys;
    ComputeKeys(8, [&](uint32_t i, uint32_t& s, uint32_t& o, uint32_t& p, uint64_t&) { s = kSite, o = kObj, p = i % 2; return true; }, uncut_keys);
    // the binder, over the live stream
    std::vector<uint64_t> bkeys;
    std::vector<ThinKey> bfull;
    std::unordered_map<uint32_t, const std::vector<uint64_t>*> fill;
    if (plant != "no_fill" && plant != "reader_only_fill")
      for (uint32_t i = 0; i < stream.size(); ++i)
        if (stream[i].marker) fill[i] = &instance_triples;
    ComputeKeys(uint32_t(stream.size()), [&](uint32_t i, uint32_t& s, uint32_t& o, uint32_t& p, uint64_t&) { if (stream[i].marker) return false; s = kSite, o = kObj, p = stream[i].part; return true; }, bkeys, &bfull, false, nullptr, &fill);
    bool binder_ok = true;
    for (uint32_t i = 0; i < stream.size(); ++i)
      if (!stream[i].marker) binder_ok = binder_ok && bkeys[i] == uncut_keys[size_t(stream[i].instance) * 2 + stream[i].part];
    Check(binder_ok, "binder: the kept instances keep their uncut keys when the cut instances' triples are filled at their markers");
    // the reader, over the same stream
    PinPublisher pub;
    pub.Publish(uncut_keys, {instance_triples[0], instance_triples[1]});
    FrameLite lite;
    lite.Begin(pub.Acquire(), 1, 0 /* no proof frames */);
    bool reader_ok = true, parity_ok = true;
    uint32_t lite_n = 0;
    for (uint32_t i = 0; i < stream.size(); ++i) {
      if (stream[i].marker) {
        if (plant != "no_fill") lite.Reserve(instance_triples.data(), instance_triples.size());
        continue;
      }
      lite_n += lite.Query(kSite, kObj, stream[i].part);
      reader_ok = reader_ok && lite.last_ordinal() == uint32_t(stream[i].instance);
      parity_ok = parity_ok && lite.last_ordinal() == bfull[i].ordinal;   // the reader's counter and the binder's key ordinal are ONE number for every kept draw (plant reader_only_fill: only the reader fills)
    }
    Check(parity_ok, "reader last_ordinal == binder key ordinal for every kept draw");
    Check(reader_ok && lite_n == 4, "reader: Reserve at the marker keeps the kept instances' ordinals (0 and 2) and they still go lite", (std::string("lite ") + std::to_string(lite_n) + " of 4").c_str());
  }

  std::printf("%s: %s\n", plant.empty() ? "thin_unit_test" : ("plant " + plant).c_str(), g_failed ? "FAIL" : "PASS");
  return g_failed ? 1 : 0;
}
