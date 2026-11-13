// lane 266: the geometry forget on a realistic memo, before (all at once every 600th frame) and after (the age queue), and the plant.
//
//   clang++ -std=c++20 -O2 -DFMT_HEADER_ONLY=1 -I thirdparty/rexglue-sdk/include -I src/renderer_adapter src/renderer_adapter/geo_forget_test.cpp -o out/geo_forget_test.exe
//
// The scene is the 7175 entrance (SKIP_GAME_WORK_MAP.md 19.5): ~550 NEW geometry keys a frame (inline builds of animated content, each a
// ~3 KB byte vector), ~250 keys reused every frame (refreshed every 300 frames, as reader.h kGeometryMarkEvery does), 600-frame keep window.
// A "store" is what the forget has to touch per key: g_geo_used, g_cap.geo, the live sink's byte store, the case ring's shared_ptr store.
// Checks: (1) worst frame after << worst frame before (the old forget is the PLANT: it must be caught by the same threshold), (2) no key
// used within the keep window is ever erased, (3) every key unused for more than keep + lateness is gone, (4) the epoch rule: a frame
// that erased anything is reported, (5) the periodic line prints, (6) a time-capped sweep still drains.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "geo_forget.h"
#include "periodic_times.h"

using namespace rdr_adapter;

namespace {
int fails = 0;
void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++fails;
}

constexpr uint64_t kKeep = 600;
constexpr double kThresholdMs = 5.0;  // the bar: no single frame's forget may take longer (a 60 Hz frame is 16.7 ms)

struct Stores {
  std::unordered_map<uint64_t, uint64_t> used;                       // g_geo_used
  std::unordered_map<uint64_t, uint32_t> cap_geo;                    // g_cap.geo (key -> bytes)
  std::unordered_map<uint64_t, std::vector<uint8_t>> sink;           // the live sink's byte store
  std::unordered_map<uint64_t, std::shared_ptr<std::vector<uint8_t>>> ring;  // the case ring's store
  geoforget::Expiry expiry;
  uint64_t epoch = 1;

  void Store(uint64_t key, uint64_t frame, uint32_t bytes) {
    cap_geo[key] = bytes;
    sink[key].assign(bytes, 0xAB);
    ring[key] = std::make_shared<std::vector<uint8_t>>(bytes, 0xCD);
    if (used.insert_or_assign(key, frame).second) expiry.Note(key, frame);
  }
  void EraseStores(uint64_t key) {  // the callback: everything but the used map
    cap_geo.erase(key);
    sink.erase(key);
    ring.erase(key);
  }
};

double Ms(std::chrono::steady_clock::time_point t0) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }

struct Run {
  double worst_ms = 0, sum_ms = 0;
  uint64_t worst_frame = 0, epochs = 0, early_erase = 0, stale_left = 0, max_alive = 0;
  size_t final_alive = 0;
};

// old = true: the plant (the old all-at-once forget at record frame % 600 == 0)
Run Simulate(bool old, uint64_t frames, bool audit) {
  Stores st;
  Run r;
  std::unordered_map<uint64_t, uint64_t> truth;  // key -> last frame used (the audit's own record)
  uint64_t next_key = 1;
  std::vector<uint64_t> persistent;
  for (int i = 0; i < 250; ++i) persistent.push_back(next_key++);
  for (uint64_t k : persistent) {
    st.Store(k, 0, 3072);
    truth[k] = 0;
  }
  for (uint64_t frame = 0; frame < frames; ++frame) {
    for (uint64_t k : persistent) {  // reused every frame: the mark is refreshed every 300 frames
      if (frame - st.used[k] >= 300) {
        st.used[k] = frame;
      }
      truth[k] = frame;
    }
    for (int i = 0; i < 550; ++i) {  // new keys
      const uint64_t k = next_key++;
      st.Store(k, frame, 2048 + (i % 7) * 512);
      truth[k] = frame;
    }
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t erased = 0;
    if (old) {
      if (frame % 600 == 0) {
        for (auto it = st.used.begin(); it != st.used.end();) {
          if (frame - it->second <= kKeep) {
            ++it;
            continue;
          }
          st.EraseStores(it->first);
          it = st.used.erase(it);
          ++erased;
        }
      }
    } else {
      const geoforget::SweepResult s = st.expiry.Sweep(st.used, frame, kKeep, st.expiry.DefaultItems(), 1'500'000, [&](uint64_t key) { st.EraseStores(key); });
      erased = s.erased;
    }
    if (erased) ++r.epochs;
    const double ms = Ms(t0);
    r.sum_ms += ms;
    if (ms > r.worst_ms) r.worst_ms = ms, r.worst_frame = frame;
    if (st.used.size() > r.max_alive) r.max_alive = st.used.size();
    if (audit && frame % 50 == 0) {
      // (2) nothing used within the window is gone from any store (a sample of every 13th key, every 50th frame)
      for (const auto& [k, last] : truth) {
        if (k % 13) continue;
        if (frame - last <= kKeep && (!st.used.count(k) || !st.cap_geo.count(k) || !st.sink.count(k) || !st.ring.count(k))) ++r.early_erase;
      }
    }
  }
  r.final_alive = st.used.size();
  // (3) everything last used more than keep + 400 frames ago must be gone from every store
  if (audit) {
    for (const auto& [k, f] : truth)
      if (frames - f > kKeep + 400 && (st.used.count(k) || st.cap_geo.count(k) || st.sink.count(k) || st.ring.count(k))) ++r.stale_left;
    std::printf("  audit: %llu keys erased early (sample), %llu stale keys left after keep+400 frames\n", (unsigned long long)r.early_erase, (unsigned long long)r.stale_left);
  }
  return r;
}
}  // namespace

int main() {
  const uint64_t frames = 3000;
  const Run before = Simulate(true, frames, false);
  const Run after = Simulate(false, frames, false);
  std::printf("before (all at once, the plant): worst frame %.2f ms at frame %llu, mean %.3f ms/frame, max alive %llu keys\n", before.worst_ms,
              (unsigned long long)before.worst_frame, before.sum_ms / frames, (unsigned long long)before.max_alive);
  std::printf("after  (age queue)             : worst frame %.2f ms at frame %llu, mean %.3f ms/frame, max alive %llu keys, epoch moves %llu\n", after.worst_ms,
              (unsigned long long)after.worst_frame, after.sum_ms / frames, (unsigned long long)after.max_alive, (unsigned long long)after.epochs);

  Check(before.worst_ms > kThresholdMs, "the plant (old all-at-once forget) is caught: its worst frame is over the threshold");
  Check(after.worst_ms <= kThresholdMs, "the age queue's worst frame is under the threshold (time cap 1.5 ms + one chunk)");
  Check(after.worst_ms * 10 < before.worst_ms, "worst frame at least 10x lower than before");
  Check(after.sum_ms < before.sum_ms * 2.5, "the total forget work did not grow more than 2.5x (it is spread, not added)");

  const Run audited = Simulate(false, 2400, true);
  Check(audited.stale_left == 0, "every key unused for keep + 400 frames is gone from all four stores");
  Check(audited.early_erase == 0, "no key used within the keep window was erased early (any store)");

  // a time-capped sweep still drains: a zero-ish budget with a small count limit empties the backlog over frames
  {
    Stores st;
    for (uint64_t k = 1; k <= 20000; ++k) st.Store(k, 0, 256);
    uint64_t frames_to_drain = 0, caps = 0;
    for (uint64_t frame = 700; st.used.size() && frame < 700 + 100; ++frame, ++frames_to_drain) {
      const auto s = st.expiry.Sweep(st.used, frame, kKeep, 1024, 0, [&](uint64_t key) { st.EraseStores(key); });
      caps += s.count_capped;
    }
    Check(st.used.empty() && st.cap_geo.empty() && st.sink.empty() && st.ring.empty(), "a backlog of 20000 drains under a 1024 count cap");
    std::printf("  drained in %llu frames (count-capped %llu)\n", (unsigned long long)frames_to_drain, (unsigned long long)caps);
  }
  // used again since: queued again, not erased
  {
    Stores st;
    st.Store(7, 0, 64);
    st.used[7] = 500;  // used at frame 500
    const auto a = st.expiry.Sweep(st.used, 700, kKeep, 0, 0, [&](uint64_t key) { st.EraseStores(key); });
    Check(a.erased == 0 && a.requeued == 1 && st.used.count(7) && st.sink.count(7), "a key used again at 500 survives the sweep at 700 and is queued again");
    const auto b = st.expiry.Sweep(st.used, 1101, kKeep, 0, 0, [&](uint64_t key) { st.EraseStores(key); });
    Check(b.erased == 1 && !st.used.count(7) && !st.sink.count(7), "and is erased once it is stale at its newer mark (1101 - 500 > 600)");
  }
  // a live start clears the used map: the queue items left over are skipped
  {
    Stores st;
    st.Store(1, 0, 64);
    st.used.clear();
    const auto a = st.expiry.Sweep(st.used, 800, kKeep, 0, 0, [&](uint64_t key) { st.EraseStores(key); });
    Check(a.erased == 0 && a.looked == 1 && st.expiry.Size() == 0, "an item whose key is already gone from the used map is dropped quietly");
  }
  // the memo shortcut never returns an erased key: random mixes of use / mark / sweep against the real stores, shortcut vs truth
  {
    uint64_t rng = 12345, wrong = 0, shortcuts = 0, checks = 0;
    auto next = [&] { rng = rng * 6364136223846793005ull + 1442695040888963407ull; return rng >> 33; };
    Stores st;
    struct Memo { uint64_t key, stored_epoch, marked, clear; };
    std::vector<Memo> memos;
    uint64_t clear_epoch = 1;
    for (uint64_t frame = 0; frame < 6000; ++frame) {
      if (frame == 3000) {  // a live restart clears everything whole
        st = Stores();
        memos.clear();
        ++st.epoch, ++clear_epoch;
      }
      for (int i = 0; i < 20; ++i) {  // new keys, each with a memo entry that has just found it stored
        const uint64_t key = frame * 100 + i + 1;
        st.Store(key, frame, 64);
        memos.push_back({key, st.epoch, frame, clear_epoch});
      }
      for (Memo& m : memos) {  // a memo hit: shortcut vs the truth; the busy half of the entries refresh their mark every 300 frames
        const bool truth = st.used.count(m.key) != 0;
        const bool sc = geoforget::StillStored(m.clear, clear_epoch, m.stored_epoch, st.epoch, m.marked, frame, kKeep, true);
        ++checks;
        shortcuts += sc;
        if (sc && !truth) ++wrong;
        if (m.key % 2 == 0 && next() % 50 == 0 && truth) {  // used again: the reader writes the mark and marked_frame (only if stored)
          st.used[m.key] = frame;
          m.marked = frame;
        }
        if (sc || truth) m.stored_epoch = st.epoch;  // as the reader does when it finds it stored
      }
      const auto s = st.expiry.Sweep(st.used, frame, kKeep, st.expiry.DefaultItems(), 0, [&](uint64_t key) { st.EraseStores(key); });
      if (s.erased) ++st.epoch;
    }
    std::printf("  shortcut: %llu checks, %llu said stored, %llu wrong\n", (unsigned long long)checks, (unsigned long long)shortcuts, (unsigned long long)wrong);
    Check(wrong == 0 && shortcuts > 0, "the memo shortcut never returns an erased key (and does fire)");
    Check(!geoforget::StillStored(1, 2, 5, 5, 100, 110, kKeep, true), "a memo entry set before a whole-store clear is never stored by the shortcut");
  }
  // lane 293 (sign flicker, phone report 053): two memo entries (the double-buffered ring's two guest addresses) share ONE content key and
  // alternate frames; the key is forgotten in a long gap and stored again under a NEW sink key. Every ref a memo hit returns must name a sink
  // key the model still holds. mode 0 = the fix (ResolveStored: offsets from the store map), 1 = `stale_lookup` (today's branch: found
  // stored by the used map, hands out the entry's OLD ref), 2 = `mark_without_key` (the mark refresh writes marked_frame without the key).
  {
    struct Entry { uint64_t sink_key = 0, clear = 1, stored_epoch = 0, marked = 0; };
    auto run = [&](int mode, uint64_t& bad_frames) {
      std::unordered_map<uint64_t, uint64_t> geo;    // g_cap.geo: content key -> sink key (the ref's offsets)
      std::unordered_map<uint64_t, bool> sink;       // sink keys the live sink still holds
      std::unordered_map<uint64_t, uint64_t> used;   // g_geo_used
      geoforget::Expiry expiry;
      uint64_t epoch = 1, next_sink = 100, erased_total = 0;
      const uint64_t C = 7;
      Entry e[2];
      bad_frames = 0;
      auto store = [&](uint64_t frame) {  // GeoFindStored or GeoStore
        if (!geo.count(C)) {
          geo[C] = next_sink++;
          sink[geo[C]] = true;
        }
        if (used.insert_or_assign(C, frame).second) expiry.Note(C, frame);
        return geo[C];
      };
      const uint64_t total = 200 + 500 + 200 + 1;
      for (uint64_t frame = 0; frame < total; ++frame) {
        const bool gap = frame >= 200 && frame < 700;
        if (!gap) {
          Entry& m = e[frame % 2];
          bool stored = false, via_lookup = false;
          if (m.stored_epoch != 0) {
            if (geoforget::StillStored(m.clear, 1, m.stored_epoch, epoch, m.marked, frame, kKeep, true)) {
              stored = true;
            } else if (mode == 1) {  // today: the used map answers, the entry's own ref is handed out
              stored = used.count(C) != 0;
              via_lookup = stored;
            } else {
              uint64_t ref = 0;
              stored = geoforget::ResolveStored(false, geo, C, ref);
              if (stored) m.sink_key = ref, via_lookup = true;
            }
          }
          if (stored && via_lookup) {  // the lookup takes the used mark (reader.h: `used`)
            if (auto u = used.find(C); u != used.end()) u->second = frame, m.marked = frame;
          } else if (stored && frame - m.marked >= 300) {  // the periodic mark refresh
            const bool found = used.count(C) != 0;
            const bool refreshed = mode == 2 ? (m.marked = frame, true) : geoforget::MarkRefresh(found, m.marked, frame);
            if (found) used[C] = frame;
            if (!refreshed) stored = false;
          }
          if (stored) m.stored_epoch = epoch;
          if (!stored) {  // rebuild: found stored by content or stored anew
            m.sink_key = store(frame);
            m.stored_epoch = epoch;
            m.marked = frame;
          }
          if (!sink.count(m.sink_key)) ++bad_frames;  // the ref names a dropped sink key: zero-filled, the board invisible
        }
        const auto s = expiry.Sweep(used, frame, kKeep, expiry.DefaultItems(), 0, [&](uint64_t key) {
          if (auto g = geo.find(key); g != geo.end()) {
            sink.erase(g->second);
            geo.erase(g);
          }
        });
        if (s.erased) ++epoch, erased_total += s.erased;
      }
      return erased_total;
    };
    uint64_t bad_fix = 0, bad_stale = 0, bad_mark = 0;
    const uint64_t erased_fix = run(0, bad_fix);
    run(1, bad_stale);
    run(2, bad_mark);
    std::printf("  sign ring: key forgotten %llu time(s); frames with a dropped ref: fix %llu, stale_lookup %llu, mark_without_key %llu\n", (unsigned long long)erased_fix,
                (unsigned long long)bad_fix, (unsigned long long)bad_stale, (unsigned long long)bad_mark);
    Check(erased_fix >= 1, "the gap really forgets the shared content key");
    Check(bad_fix == 0, "fix: every returned ref names a sink key the store still holds, in every frame");
    Check(bad_stale >= 90, "plant stale_lookup (today's branch) is caught: about every second frame after the gap hands out a dropped key (the 053 signature)");
    // mark_without_key: the unit contract. A key absent from the used map must not refresh (and must not make the shortcut's second clause true).
    uint64_t marked = 5;
    Check(!geoforget::MarkRefresh(false, marked, 400) && marked == 5, "MarkRefresh: key absent -> rebuild, marked_frame untouched");
    Check(geoforget::MarkRefresh(true, marked, 400) && marked == 400, "MarkRefresh: key present -> marked");
    // plant mark_without_key: marked_frame written without the key makes StillStored's second clause "stored" for a key that is gone
    {
      uint64_t planted = 5, fixed = 5;
      planted = 400;  // the plant: marked regardless
      geoforget::MarkRefresh(false, fixed, 400);
      Check(geoforget::StillStored(1, 1, 3, 9, planted, 450, kKeep, true), "plant mark_without_key: a young marked_frame says stored for an absent key (what the fix prevents)");
      Check(!geoforget::StillStored(1, 1, 3, 9, fixed, 1000, kKeep, true), "fix: the unmarked entry is not stored by the shortcut once the epoch moved");
    }
  }
  // the line
  {
    periodic::Times t;
    t.forget.Add(0.31), t.forget.Add(0.9), t.memo.Add(0.01), t.census.Add(1.4), t.cost.Add(0.2);
    t.forget_erased = 330000, t.forget_looked = 331000, t.forget_requeued = 1000, t.queue_depth = 330500;
    const std::string line = t.Line(6600, 7200);
    std::printf("  %s\n", line.c_str());
    Check(line.find("[rdr][periodic] frames 6600-7200") == 0 && line.find("geometry forget 1.21 (worst 0.90, 2 runs)") != std::string::npos &&
              line.find("330000 keys erased") != std::string::npos,
          "the [rdr][periodic] line prints the ms, the worst run, the runs and the erased count");
  }
  std::printf("%s\n", fails ? "FAILED" : "all passed");
  return fails ? 1 : 0;
}
