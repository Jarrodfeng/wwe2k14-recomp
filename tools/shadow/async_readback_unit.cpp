// Lane 232 unit test: the async read-back mailbox (src\renderer_adapter\async_readback.h, the code reader.h OnTextureLock and live.cpp PublishAsync run). 0 game runs, no GPU.
//   async_readback_unit.exe [--plant NAME]     build: tools\shadow\build_async_readback_unit.ps1
//   A. ordering: a finished copy of an older frame never replaces a newer one; Take returns the newest.
//   B. a frame-late answer: the game's newest copy (NoteCopy) is ahead of the finished copy by up to max_lag frames -> Served (lag reported); beyond it -> TooLag (the blocking fallback).
//   C. the fallback: not watched -> NotWatched; watched, nothing finished -> NothingYet; Unwatch (image released) forgets the copy; another level / layer is its own key.
//   D. the stale copy: a finished copy older than the game's newest copy by more than the bound, or older than max_age at the lock, is never served. Plant `no_bounds` (an unbounded lag and age)
//      serves them: checks D1 / D2 / B3 FAIL.
//   S. (SF1) lag 0: a finished copy older than the newest copy the game made is never served, the lock's own frame falls back; plant `lag4` (the old default) fails S1 / S2. (SF2) a picture copied into
//      in at least half the frames of a window is "busy": not read back, not served (S4), a sparse one is served (S5).
//   L. per-site lag (0x8274FADC = 30, unnamed = 0; plant `lag_everywhere`). T. the renderer's bookkeeping (readback_tracker.h): handed out only after the frame's fence, slot reuse, cancel; plant `early`.
//   Z. the deferred delivery (demand / first message after it / fresh picture / board / guard); plants `deliver_wrong` (guard skipped) fails Z12-Z15, `any_thread` Z17, `busy_demand` Z19.
//   E. cadence: Due() asks for a read-back at most once in every_frames. Plant `no_cadence` (every_frames 1 against the expected 2) fails E1.
//   F. two threads: a publisher writes frames (every pixel = the frame number) while a locker takes; every served buffer is whole (one frame's value, never a mix), never older than the bound.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../../src/renderer/readback_tracker.h"
#include "../../src/renderer_adapter/async_readback.h"
#include "../../src/renderer_adapter/thumb_defer_core.h"

using rdr_adapter::AsyncReadback;
using Key = AsyncReadback::Key;
using R = AsyncReadback::Result;

static int g_bad = 0;
static void Check(bool ok, const char* name, const std::string& why = "") {
  std::printf("%s  %-48s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_bad;
}
static std::vector<uint8_t> Pic(uint8_t v) { return std::vector<uint8_t>(128 * 128 * 4, v); }

int main(int argc, char** argv) {
  std::string plant;
  for (int i = 1; i + 1 < argc; ++i)
    if (!std::strcmp(argv[i], "--plant")) plant = argv[i + 1];
  AsyncReadback::Params p;  // defaults: lag 0 (SF1: only the newest copy), age 30, every 2, busy window 120
  AsyncReadback::Params tol = p;  // a tolerant setting for the frame-late arms (B, F): lag 4
  tol.max_lag_frames = 4;
  if (plant == "lag4") p.max_lag_frames = 4;  // the old default: serves a copy older than the one the game asked for (S1 / S2 fail)
  if (plant == "no_bounds") p.max_lag_frames = p.max_age_frames = tol.max_lag_frames = 0xFFFFFFFFu;
  const size_t expect_due_after_one = 0;  // every 2 frames: one frame after a read-back is not due
  if (plant == "no_cadence") p.every_frames = 1;

  const bool deliver_anyway = plant == "deliver_wrong";  // the delivery guard skipped: the job is run on an object that no longer waits
  const bool early = plant == "early";  // Poll hands a copy out before its frame's fence signalled
  if (plant == "lag_everywhere") rdr_adapter::g_plant_lag_everywhere = true;  // the thumbnail's allowance applied to an unnamed site
  const Key k{7, 0, 0};
  std::vector<uint8_t> rgba;
  uint32_t w = 0, h = 0, lag = 0;
  uint64_t sf = 0;

  // C
  {
    AsyncReadback a;
    a.SetParams(p);
    Check(a.Take(k, 100, rgba, w, h) == R::NotWatched, "C1 unknown image: first lock reads blocking");
    Check(!a.AnyWatched(), "C1b nothing watched yet");
    a.Watch(k);
    a.Watch(k);  // idempotent
    Check(a.GetCounts().watched == 1, "C2 Watch twice = one entry");
    Check(a.Take(k, 100, rgba, w, h) == R::NothingYet, "C3 watched, nothing finished: blocking fallback");
    Check(a.Take(Key{7, 1, 0}, 100, rgba, w, h) == R::NotWatched, "C4 another level is its own key");
    a.Publish(k, 98, Pic(98), 128, 128);
    Check(a.Take(k, 100, rgba, w, h, &sf, &lag) == R::Served && sf == 98 && w == 128 && h == 128 && rgba.size() == 128 * 128 * 4 && rgba[0] == 98, "C5 a finished copy is served");
    a.Unwatch(7);
    Check(a.Take(k, 100, rgba, w, h) == R::NotWatched && !a.Watched(7), "C6 released image: forgotten");
    a.Publish(k, 99, Pic(99), 128, 128);
    Check(a.GetCounts().publish_dropped == 1, "C7 a publish for a released image is dropped");
  }
  // A
  {
    AsyncReadback a;
    a.SetParams(p);
    a.Watch(k);
    a.Publish(k, 50, Pic(50), 128, 128);
    a.Publish(k, 52, Pic(52), 128, 128);
    a.Publish(k, 51, Pic(51), 128, 128);  // arrives late: older than the one held
    Check(a.Take(k, 53, rgba, w, h, &sf) == R::Served && sf == 52 && rgba[5] == 52, "A1 newest frame wins, an older publish never replaces it");
    Check(a.GetCounts().publish_dropped == 1, "A2 the older publish was counted dropped");
  }
  // B
  {
    AsyncReadback a;
    a.SetParams(tol);
    a.Watch(k);
    a.Publish(k, 100, Pic(100), 128, 128);
    a.NoteCopy(7, 100);
    Check(a.Take(k, 100, rgba, w, h, &sf, &lag) == R::Served && lag == 0, "B1 copy and lock in step: lag 0");
    a.NoteCopy(7, 103);  // the game copied again, the render thread has not drawn it yet
    Check(a.Take(k, 103, rgba, w, h, &sf, &lag) == R::Served && lag == 3 && sf == 100, "B2 a frame-late answer (3 frames behind) is served");
    a.NoteCopy(7, 105);
    const R r = a.Take(k, 105, rgba, w, h, &sf, &lag);
    Check(r == R::TooLag, "B3 5 frames behind the game's newest copy: fallback", r == R::Served ? "served a copy older than the game allows" : "");
    a.Publish(k, 104, Pic(104), 128, 128);
    Check(a.Take(k, 105, rgba, w, h, &sf, &lag) == R::Served && sf == 104 && lag == 1 && rgba[0] == 104, "B4 the render thread caught up: served again, lag 1");
    // Take does not consume: a second lock with no new copy gets the same bytes
    Check(a.Take(k, 106, rgba, w, h, &sf) == R::Served && sf == 104, "B5 the copy stays for the next lock");
  }
  // S (SF1, busy SF2)
  {
    AsyncReadback a;
    a.SetParams(p);
    a.Watch(k);
    a.Publish(k, 100, Pic(100), 128, 128);
    a.NoteCopy(7, 100);
    Check(a.Take(k, 105, rgba, w, h, &sf, &lag) == R::Served && lag == 0 && sf == 100, "S0 lag 0: the newest copy, finished, is served (a later lock)");
    a.NoteCopy(7, 101);
    const R r1 = a.Take(k, 101, rgba, w, h);
    Check(r1 == R::TooLag, "S1 lag 0: a copy one frame newer than the finished one is NOT answered with the older", r1 == R::Served ? "OLDER COPY SERVED" : "");
    a.NoteCopy(7, 103);  // the lock's own frame holds the copy the game waits for; the render thread has not drawn it
    const R r2 = a.Take(k, 103, rgba, w, h);
    Check(r2 == R::TooLag, "S2 lag 0: a copy made in the lock's own frame falls back to the blocking read", r2 == R::Served ? "OLDER COPY SERVED" : "");
    a.Publish(k, 103, Pic(103), 128, 128);
    Check(a.Take(k, 103, rgba, w, h, &sf, &lag) == R::Served && sf == 103 && rgba[0] == 103, "S3 lag 0: once that frame is drawn and read, it is served");
    // busy: copies in every frame of a window -> not read back, not served
    AsyncReadback b;
    b.SetParams(p);
    b.Watch(k);
    b.Publish(k, 1, Pic(1), 128, 128);
    for (uint64_t f = 1; f <= 130; ++f) b.NoteCopy(7, f);
    std::vector<Key> due;
    b.Due(7, 200, due);
    Check(due.empty() && b.Take(k, 130, rgba, w, h) == R::Busy && b.GetCounts().busy_windows == 1, "S4 copied in every frame: busy, no read-back, fallback");
    // sparse: one copy every 20 frames -> not busy
    AsyncReadback c;
    c.SetParams(p);
    c.Watch(k);
    c.Publish(k, 280, Pic(1), 128, 128);
    for (uint64_t f = 20; f <= 280; f += 20) c.NoteCopy(7, f);
    c.Due(7, 400, due);
    Check(due.size() == 1 && c.Take(k, 281, rgba, w, h) == R::Served && c.GetCounts().busy_windows == 0, "S5 copied every 20 frames: not busy, served");
  }
  // L: the per-site lag allowance (lane 236 / 232)
  {
    using rdr_adapter::SiteLagAllowance;
    Check(SiteLagAllowance(0x8274FADC, 30, 0) == 30, "L1 the replay thumbnail site gets the 30-frame allowance");
    const uint32_t other = SiteLagAllowance(0x83418024, 30, 0);
    Check(other == 0, "L2 an unnamed site stays at lag 0", other ? "UNNAMED SITE GOT THE THUMBNAIL ALLOWANCE" : "");
    AsyncReadback a;
    a.SetParams(p);
    a.Watch(k, 1, SiteLagAllowance(0x8274FADC, 30, 0));
    a.Publish(k, 100, Pic(100), 128, 128);
    a.NoteCopy(7, 125);
    Check(a.Take(k, 125, rgba, w, h, &sf, &lag) == R::Served && lag == 25 && sf == 100, "L3 thumbnail site: a copy 25 frames older than the newest is served");
    a.NoteCopy(7, 131);
    Check(a.Take(k, 131, rgba, w, h) == R::TooLag, "L4 ... 31 frames older: the blocking read");
    AsyncReadback b;
    b.SetParams(p);
    b.Watch(k, 1, SiteLagAllowance(0x83418024, 30, 0));  // an unnamed site's allowance
    b.Publish(k, 100, Pic(100), 128, 128);
    b.NoteCopy(7, 125);
    const R r = b.Take(k, 125, rgba, w, h);
    Check(r == R::TooLag, "L5 unnamed site: a copy 25 frames older is NOT served", r == R::Served ? "OLDER COPY SERVED TO AN UNNAMED SITE" : "");
    AsyncReadback c;  // the allowance site is never "busy": copied into every frame, it is still read back and served
    c.SetParams(p);
    c.Watch(k, 1, 30);
    c.Publish(k, 1, Pic(1), 128, 128);
    for (uint64_t f = 1; f <= 130; ++f) c.NoteCopy(7, f);
    std::vector<Key> due;
    c.Due(7, 200, due);
    c.Publish(k, 129, Pic(129), 128, 128);
    Check(!due.empty() && c.Take(k, 130, rgba, w, h, &sf, &lag) == R::Served && lag == 1 && c.GetCounts().busy_windows == 0, "L6 the thumbnail site copied every frame: not busy, served (lag 1)");
  }
  // T: the renderer's read-back bookkeeping (src\renderer\readback_tracker.h): a copy is handed out only after its frame's fence
  {
    using Tr = rdr::ReadbackTracker;
    Tr t;
    const Tr::Key tk{7, 0, 0};
    bool fence[3] = {false, false, false};
    auto signalled = [&](uint32_t slot) { return fence[slot]; };
    std::vector<Tr::Done> done;
    std::vector<Tr::Request> take;
    Check(t.Queue(tk, 11), "T1 queue a level");
    Check(!t.Queue(tk, 12), "T2 the same level is refused while queued (its staging would be overwritten)");
    t.TakePending(take);
    Check(take.size() == 1 && take[0].tag == 11 && !t.Queue(tk, 13), "T3 taken for recording: still refused");
    t.MarkRecorded(1, 5);  // submitted: slot 1, serial 5
    t.Poll(signalled, done, early);
    Check(done.empty(), "T4 recorded and submitted but the fence is not signalled: nothing is handed out", done.size() ? "SERVED BEFORE THE FENCE" : "");
    done.clear();
    fence[1] = true;
    t.Poll(signalled, done);
    Check(done.size() == 1 && done[0].tag == 11 && done[0].serial == 5, "T5 the fence signalled: handed out once");
    done.clear();
    t.Poll(signalled, done);
    Check(done.empty() && t.InFlight() == 0, "T6 not handed out twice");
    Check(t.Queue(tk, 14), "T7 the level is free for the next request");
    t.TakePending(take);
    t.MarkRecorded(2, 6);
    fence[2] = false;
    t.OnSlotWaited(2, 9);  // the slot's fence was waited by Render before reuse by serial 9 (so it was unsignalled again after resubmit)
    t.Poll(signalled, done);
    Check(done.size() == 1 && done[0].tag == 14, "T8 a slot reuse completes its older copy (waited), even though the fence is unsignalled again");
    done.clear();
    Check(t.Queue(tk, 15), "T9 queue again");
    t.TakePending(take);
    t.Cancel(tk);  // the recorder refused it (image gone)
    t.MarkRecorded(0, 7);
    fence[0] = true;
    t.Poll(signalled, done);
    Check(done.empty() && t.Queue(tk, 16), "T10 a cancelled request is never handed out and frees its key");
    t.TakePending(take);
    t.Requeue();
    Check(t.Pending() == 1, "T11 requeue keeps an unrecorded request");
    // SF-1: a cancelled request is reported (tag) so the caller drops its record; the key is free again and nothing stays in flight
    Tr u;
    Check(u.Queue(tk, 21, 9), "T12 queue with a source");
    u.TakePending(take);
    Check(take.size() == 1 && take[0].source == 9, "T13 the source passes through to the recorder (SF-2)");
    u.Cancel(tk);  // e.g. the source copy never ran in this frame
    std::vector<Tr::Done> gone;
    u.TakeCancelled(gone);
    Check(gone.size() == 1 && gone[0].tag == 21 && u.InFlight() == 0 && !u.HasCancelled() && u.Queue(tk, 22), "T14 cancel is reported with its tag and frees the key (never Busy forever)");
  }
  // Z: the deferred delivery (the replay thumbnail job): demand, the first message after it, one fresh picture; the board; the guard
  {
    AsyncReadback a;
    a.SetParams(p);
    a.Demand(k, 10, 500);  // 10 frames submitted so far
    std::vector<Key> due;
    a.Due(7, 500, due, 10);
    Check(due.empty(), "Z1 a message that was already submitted (seq 10) does not carry the read");
    a.Due(7, 501, due, 11);
    Check(due.size() == 1, "Z2 the first message after the demand (seq 11) does");
    a.Publish(k, 501, Pic(77), 128, 128, 10);  // a read taken at the end of an older message
    Check(!a.TakeFresh(k, rgba, w, h), "Z3 a picture read before the demand is never delivered");
    a.Publish(k, 502, Pic(78), 128, 128, 11);
    Check(a.TakeFresh(k, rgba, w, h) && rgba[0] == 78 && w == 128, "Z4 the read of the first message after the demand is delivered");
    Check(!a.TakeFresh(k, rgba, w, h), "Z5 and only once");
    a.Due(7, 503, due, 12);
    Check(due.empty(), "Z6 no cadence reads for a demand-only picture");
    using rdr_adapter::ThumbBoard;
    using rdr_adapter::ThumbEntry;
    ThumbBoard bd;
    const ThumbEntry A{0x1000, 0x2000, 7, 500, 0xAAAA}, B{0x3000, 0x4000, 8, 500, 0xAAAA};
    Check(bd.Add(A) && bd.Add(B) && !bd.Add(A), "Z7 one deferral per object");
    std::vector<ThumbBoard::Ready> ready;
    bool landed = false;
    auto pic = [&](const ThumbEntry& e) { return landed && e.obj == 0x1000; };
    bd.Step(505, 30, 0xAAAA, pic, ready);
    Check(ready.empty() && bd.Pending() == 2, "Z8 nothing before the picture lands (and before the timeout)");
    landed = true;
    bd.Step(510, 30, 0xAAAA, pic, ready);
    Check(ready.size() == 1 && ready[0].entry.obj == 0x1000 && ready[0].why == ThumbBoard::Why::Picture, "Z9 the object whose picture landed is ready; the other keeps waiting");
    ready.clear();
    bd.Step(530, 30, 0xAAAA, pic, ready);
    Check(ready.size() == 1 && ready[0].entry.obj == 0x3000 && ready[0].why == ThumbBoard::Why::TimedOut && bd.Pending() == 0, "Z10 30 frames without a picture: the job runs on what is there");
    // the guard: the captured object, still waiting (+44 == 4), same texture
    uint32_t mem44 = 4, mem52 = 0x2000, mem84 = 0x7000;
    auto ld = [&](uint32_t addr) { return addr == 0x1000 + 44 ? mem44 : addr == 0x1000 + 52 ? mem52 : addr == 0x1000 + 84 ? mem84 : 0u; };
    Check(rdr_adapter::DeliveryAllowed(A, ld, true), "Z11 the waiting object is delivered to");
    mem44 = 5;  // the writer has already saved it (or the job ran by itself)
    const bool wrong = rdr_adapter::DeliveryAllowed(A, ld, true, deliver_anyway);
    Check(!wrong, "Z12 an object that no longer waits is never touched", wrong ? "DELIVERED INTO THE WRONG STATE" : "");
    mem44 = 4, mem52 = 0x9999;  // freed and reused: another texture
    const bool other = rdr_adapter::DeliveryAllowed(A, ld, true, deliver_anyway);
    Check(!other, "Z13 an object that holds another texture is never touched", other ? "DELIVERED INTO ANOTHER OBJECT'S TEXTURE" : "");
    // B1: the destructor frees +84 (-> 0) and the texture but leaves +44 == 4 and +52 as they were
    mem44 = 4, mem52 = 0x2000, mem84 = 0;
    const bool freed = rdr_adapter::DeliveryAllowed(A, ld, true, deliver_anyway);
    Check(!freed, "Z14 (B1) a destroyed object (buffer freed, ready word and texture words stale) is never run", freed ? "RAN THE JOB ON A FREED OBJECT" : "");
    mem84 = 0x7000;
    const bool dead_tex = rdr_adapter::DeliveryAllowed(A, ld, false, deliver_anyway);
    Check(!dead_tex, "Z15 (B1) a texture no longer registered to the same image is never written", dead_tex ? "WROTE A RELEASED TEXTURE" : "");
    ThumbBoard bb;
    bb.Add(A);
    ThumbEntry gone;
    Check(bb.Remove(0x1000, &gone) && gone.image == 7 && bb.Pending() == 0 && !bb.Remove(0x1000), "Z16 (B1 / S2) the destructor / the writer take the entry off the board");
    // S1: only a present on the thread that entered the job delivers
    ThumbBoard bt;
    bt.Add(A);
    std::vector<ThumbBoard::Ready> rt;
    auto always = [](const ThumbEntry&) { return true; };
    bt.Step(510, 30, 0xBBBB, always, rt, plant == "any_thread");  // the D3D worker's present
    Check(rt.empty() && bt.Pending() == 1, "Z17 (S1) a present on another thread delivers nothing", rt.size() ? "DELIVERED FROM ANOTHER THREAD" : "");
    bt.Step(510, 30, 0xAAAA, always, rt);
    Check(rt.size() == 1, "Z18 (S1) the game's thread delivers it");
    // S3: a demand-only picture copied in most frames is never marked busy (its demand is never refused)
    rdr_adapter::g_plant_busy_demand = plant == "busy_demand";
    AsyncReadback dm;
    dm.SetParams(p);
    dm.Demand(k, 5, 1);
    for (uint64_t f = 1; f <= 130; ++f) dm.NoteCopy(7, f);
    std::vector<Key> dd;
    dm.Due(7, 200, dd, 6);
    Check(dd.size() == 1 && dm.GetCounts().busy_windows == 0, "Z19 (S3) a demand is due even when the picture was copied in every frame", dd.empty() ? "DEMAND REFUSED AS BUSY" : "");
    rdr_adapter::g_plant_busy_demand = false;
  }
  // Z20-Z23 (lane 278): the per-job step timers.
  {
    rdr_adapter::ThumbTimes t;
    t.n = 3; t.wait_ms = 68.0; t.read_ms = 0.25; t.convert_ms = 1.5; t.rerun_ms = 99.125;
    Check(rdr_adapter::FormatThumbTimes(t, 4.5) == "job 3: wait 68.00 ms, read 0.25, convert 1.50, re-run 99.12 (game), write 4.50" || rdr_adapter::FormatThumbTimes(t, 4.5) == "job 3: wait 68.00 ms, read 0.25, convert 1.50, re-run 99.13 (game), write 4.50",
          "Z20 the timer line: wait / read / convert / re-run / write", rdr_adapter::FormatThumbTimes(t, 4.5));
    Check(rdr_adapter::FormatThumbTimes(t, -1.0) == "job 3: wait 68.00 ms, read 0.25, convert 1.50, re-run 99.12 (game), write none" || rdr_adapter::FormatThumbTimes(t, -1.0).find("write none") != std::string::npos, "Z21 no writer: 'write none'");
    t.replay = false;
    Check(rdr_adapter::FormatThumbTimes(t, 0.0).find("[blocking read]") != std::string::npos, "Z22 a blocking-read job is marked");
    rdr_adapter::ThumbTimesBook book;
    book.Begin(0xA4CA6B10, 7);
    book.Update(0xA4CA6B10, [](rdr_adapter::ThumbTimes& x) { x.rerun_ms = 12.0; });
    book.Update(0x1234, [](rdr_adapter::ThumbTimes& x) { x.rerun_ms = 99.0; });  // unknown object: ignored
    rdr_adapter::ThumbTimes got;
    const bool first = book.Take(0xA4CA6B10, &got);
    Check(first && got.n == 7 && got.rerun_ms == 12.0 && !book.Take(0xA4CA6B10, &got), "Z23 the book: updates by object, the line is taken once");
  }
  // D
  {
    AsyncReadback a;
    a.SetParams(p);
    a.Watch(k);
    a.Publish(k, 10, Pic(10), 128, 128);
    a.NoteCopy(7, 40);  // the game kept copying (frames 11..40), the render thread never drew one of them
    const R r1 = a.Take(k, 40, rgba, w, h);
    Check(r1 == R::TooLag, "D1 stale (30 frames behind the newest copy): never served", r1 == R::Served ? "STALE COPY SERVED" : "");
    AsyncReadback b;
    b.SetParams(p);
    b.Watch(k);
    b.Publish(k, 10, Pic(10), 128, 128);
    b.NoteCopy(7, 10);
    const R r2 = b.Take(k, 200, rgba, w, h);  // the game stopped copying; 190 frames later it locks
    Check(r2 == R::TooOld, "D2 a long dead copy (190 frames old) is not served", r2 == R::Served ? "STALE COPY SERVED" : "");
    Check(b.Take(k, 40, rgba, w, h) == R::Served, "D3 30 frames old is still inside the age bound");
  }
  // E
  {
    AsyncReadback a;
    a.SetParams(p);
    a.Watch(k);
    std::vector<Key> due;
    a.Due(7, 10, due);
    Check(due.size() == 1, "E0 nothing finished yet: due");
    a.Publish(k, 10, Pic(1), 128, 128);
    a.Due(7, 11, due);
    Check(due.size() == expect_due_after_one, "E1 one frame after a read-back: not due (every 2)");
    a.Due(7, 12, due);
    Check(due.size() == 1, "E2 two frames after: due");
    a.Due(99, 12, due);
    Check(due.empty(), "E3 an image nobody watches is never due");
    std::vector<uint32_t> ids;
    a.Images(ids);
    Check(ids.size() == 1 && ids[0] == 7, "E4 Images lists the watched ids");
  }
  // F
  {
    AsyncReadback a;
    a.SetParams(tol);
    a.Watch(k);
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> game_frame{1}, published{1};
    std::thread render([&] {
      uint64_t f = 1;
      while (!stop) {
        const uint64_t g = game_frame.load();
        if (f + 1 <= g) {
          ++f;
          a.Publish(k, f, Pic(uint8_t(f)), 128, 128);  // value = frame mod 256
          published = f;
        }
        std::this_thread::yield();
      }
    });
    uint64_t served = 0, fallback = 0, torn = 0, too_old = 0;
    for (uint64_t g = 2; g < 20000; ++g) {
      game_frame = g;
      a.NoteCopy(7, g);
      if (g % 3) continue;
      for (int spin = 0; spin < 2000 && published.load() + 2 < g; ++spin) std::this_thread::yield();  // the render thread is a frame or two behind, as in the game
      uint64_t frame = 0;
      uint32_t lg = 0;
      std::vector<uint8_t> out;
      if (a.Take(k, g, out, w, h, &frame, &lg) == R::Served) {
        ++served;
        const uint8_t v = out[0];
        for (size_t i = 1; i < out.size(); i += 4097)
          if (out[i] != v) ++torn;
        if (v != uint8_t(frame)) ++torn;
        if (g - frame > uint64_t(tol.max_lag_frames)) ++too_old;
      } else {
        ++fallback;
      }
    }
    stop = true;
    render.join();
    Check(torn == 0, "F1 every served buffer is whole and stamped with its frame", std::to_string(torn) + " torn");
    Check(too_old == 0, "F2 nothing older than the lag bound was served", std::to_string(too_old));
    Check(served > 0, "F3 frame-late answers were served while the publisher ran", std::to_string(served) + " served, " + std::to_string(fallback) + " fallback");
  }
  std::printf("%s (%d failed)%s\n", g_bad ? "FAILED" : "ALL PASS", g_bad, plant.empty() ? "" : ("  [plant " + plant + "]").c_str());
  return g_bad ? 1 : 0;
}
