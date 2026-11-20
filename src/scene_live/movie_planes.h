// The movie planes from GUEST MEMORY at the game's plane flush (S103 lane P4, landing 2; docs\renderer\LANDING2_PLAN.md 4 item 4 / 5 row P4; docs\systems\video_tron.md "movie planes").
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only; NO game, SDK or renderer includes: the game tap (movie_planes_tap.h), MovieOurs (src\renderer_scene\movie_ours.*) and the offline
// unit / rig (tools\movie\movie_planes_unit.cpp) run THESE functions. Marks: [C] read in a listing / measured (cited), [L] a reading, [U] unknown.
//
// WHAT IT REPLACES. MovieOurs (lane 131) drew the very images the stream draw binds: the stream's reader decoded each plane texture out of guest memory (reader.h EnsureContent) and the binder mapped
// the draw's slot images. That makes the 2D layer read a stream image. The movie present (sub_827E0018) copies each decoded frame into the movie's DISPLAY set and calls the plane flush (sub_827FB540,
// r3 = the set): "these planes now hold a complete new frame" [C screen_planes_core.h header, listing 0x827E0098-0x827E00B0]. This store copies that frame AT THE FLUSH (every set, no channel
// binding: the stage screens' store, screen_planes_core.h, copies only the sets a stage screen shows) and serves it to the 2D movie draw by (set, draw order); PlaneImages uploads the four planes into OUR images.
//
// THE CROSSING [C B1F, screen_planes_core.h]: data plane k of the set is 0 Y, 1 cR, 2 cB, 3 A; sub_827F9D88 hands DRAW plane 1 (the shader's Cb, slot 1) DATA plane 2 and DRAW plane 2 (Cr, slot 2) DATA
// plane 1. Frame::cb (draw slot 1) = data plane 2, Frame::cr (draw slot 2) = data plane 1: CopyDisplay (the one copy routine; this file does not have a second one) stores it so. Draw slot order here is
// ALWAYS Y, Cb, Cr, A = Frame::y, cb, cr, a (the plant swap_chroma = screens::Plants::swap_chroma exchanges them).
//
// U3 (LANDING2_PLAN 3): did the plane flush happen BEFORE the frame's movie draw? A draw takes the newest frame of its set flushed no later than itself. The order is known two ways: by the flush
// serial (`seq`: a count of flushes; a recorder that stamps the draw with Store().seq() gets an exact order) or by the guest frame (flush gf < draw gf = before, gf == draw gf = same frame, order not
// knowable from the frame number, gf > draw gf = AFTER: the plane was not there when the game drew). Counted: flush_before / flush_same / flush_after (= the newest frame of the set came after the draw; the
// older frame the draw saw is served when the ring holds it) / no_flush (no frame of the set yet: the stream's draw is kept by MovieOurs).
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <mutex>
#include <vector>

#include "screen_planes_core.h"

namespace rdr_scene::live::movieplanes {

using screens::Frame;
using stagefx::ReadFn;

inline constexpr uint32_t kRing = 4;        // frames kept per set (the record lags the game by a frame or two)
inline constexpr size_t kMaxSets = 8;       // display sets held; the oldest go first
inline constexpr uint64_t kIdleFrames = 120;   // a set with no flush for this many guest frames is dropped; also the oldest a frame served to a draw WITH a set hint may be
inline constexpr uint64_t kNoHintWindow = 2;   // no set hint: only sets flushed within this many guest frames of the draw (either side) are candidates, and the served frame may be at most this old (S103 P4F review F1 / F4)

// Planted faults (the offline unit; the live path never sets them). Each makes one thing wrong and must fail the check that names it.
struct Plants {
  bool stale_plane = false;    // the planes of the PREVIOUS flush of the set are served (a flush too old / the double buffer's other half): "planes.fresh"
  bool future_frame = false;   // the newest frame is served even when it was flushed AFTER the draw (U3 ignored): "order.flush_before_draw"
  bool any_size = false;       // the size the draw asked for is ignored (another movie's frame of a different size is served): "select.size"
  bool no_set_hint = false;    // the set hint is ignored (the newest flush of any set): "select.set"
  bool swap_chroma = false;    // data planes 1 and 2 handed over the old way round (screens::Plants::swap_chroma): "planes.chroma_order"
  bool wrong_buffer = false;   // the other half of the display double buffer (screens::Plants::wrong_buffer): "planes.display_buffer"
  bool no_ambiguity_guard = false;   // no set hint and several candidate sets: the newest is served anyway (review F1): "select.ambiguous_refused"
  bool no_age_limit = false;   // a frame older than the window is served (review F4): "order.too_old"
  bool leak_stream = false;    // (ServeGuest) the stream's image ids are handed out instead of ours: "guest.no_stream_image"
  bool key_ignored = false;    // the row's plane key is ignored: the set is the row's hint (the last flush of ANY movie): "select.key"
  bool half_chroma = false;    // the chroma planes copied at (w+1)/2 x (h+1)/2 instead of the set header's own size (a 1280x280 movie has 640x144 chroma): "planes.chroma_size" / the rig on the tron capture
};

// What the caller knows about the next draw (MovieOurs fills it from the stream slot-0 image's DESCRIPTION and, when the 2D record is on, the row's plane_set). 0 = unknown.
struct DrawHint {
  uint32_t set = 0, y_w = 0, y_h = 0, seq = 0;
  uint32_t key = 0;   // N2: the row's Y plane handle key (tex_key[0], the sprite object's +400); 0 = unknown
};
// What the draw knows. Every field is optional; 0 = unknown.
struct Query {
  uint32_t set = 0;            // the display set the draw's planes belong to (the 2D row's plane_set); 0 = any
  uint32_t y_w = 0, y_h = 0;   // the Y plane's size (the draw's slot 0 image); 0 = any
  uint64_t guest_frame = 0;    // the draw's guest frame
  uint32_t seq = 0;            // the flush serial the draw was stamped with (Store().seq() when the draw was recorded); 0 = unknown, the guest frame is used
  uint32_t key = 0;            // N2: the draw's own plane key; a set whose handle keys hold it is THE set (the hint is the last flush of any movie); 0 = unknown
};

// The ONE way a Query is made (MovieOurs and the offline unit / rig both call it; review F1 3).
inline Query MakeQuery(const DrawHint& h, uint64_t guest_frame) {
  Query q;
  q.set = h.set, q.y_w = h.y_w, q.y_h = h.y_h, q.seq = h.seq, q.key = h.key, q.guest_frame = guest_frame;
  return q;
}

struct Census {
  uint64_t flushes = 0, copied = 0, bytes = 0, refused = 0, sets_pruned = 0;
  uint64_t asked = 0, answered = 0;
  uint64_t flush_before = 0, flush_same = 0, flush_after = 0, no_flush = 0;   // U3
  uint64_t key_hit = 0, key_miss = 0, key_hint_differs = 0;   // N2: key_hit = a set held the row's key; key_miss = none did (the hint's rule); key_hint_differs = the key's set was not the row's hint
  uint64_t no_size = 0, ambiguous = 0, ring_miss = 0, too_old = 0;   // ambiguous = no set hint and more than one candidate set flushed near the draw: refused (the stream draw stays)
};

class MovieStore {
 public:
  Plants plants;

  // The flush of display set `set` at guest frame `gf` (called inside the movie present). Copies the display buffer. Returns true when a frame was stored.
  bool OnFlush(ReadFn read, uint32_t set, uint64_t gf) {
    uint32_t my_seq;
    {
      std::lock_guard<std::mutex> lk(m_);
      ++c_.flushes;
      my_seq = ++seq_;   // this flush's serial, taken with the count: a flush racing this one cannot give it a later number (review F5)
    }
    uint32_t keys[8] = {};   // N2: the set's 2 x 4 plane handle keys (word 0 of each pair at set + 64 b + 8 k); 0 = unreadable / none
    uint32_t nbuf = 0;   // N2 F5: only the buffers the set has (the count at set + 128)
    if (!stagefx::ReadWord(read, set + screens::kSetCount, nbuf)) nbuf = 0;
    for (uint32_t b = 0; b < 2 && b < nbuf; ++b)
      for (uint32_t k = 0; k < 4; ++k) {
        uint32_t v = 0;
        if (stagefx::ReadWord(read, set + 64 * b + 8 * k, v)) keys[b * 4 + k] = v;
      }
    screens::Plants cp;
    cp.swap_chroma = plants.swap_chroma, cp.wrong_buffer = plants.wrong_buffer;
    cp.header_chroma = !plants.half_chroma;   // the chroma textures are the header's size (the draw's uv spans the whole texture)
    std::shared_ptr<Frame> f = TakeSpare(set);
    {
      std::lock_guard<std::mutex> lk(copy_m_);   // (CopyDisplay counts into the copier's own census)
      copier_.plants = cp;
      if (!copier_.CopyDisplay(read, set, *f)) {
        std::lock_guard<std::mutex> lk2(m_);
        ++c_.refused;
        return false;
      }
    }
    f->guest_frame = gf;
    std::lock_guard<std::mutex> lk(m_);
    f->id = ++next_id_;
    Slot* s = SlotOf(set);
    s->seq[s->next % kRing] = my_seq;
    s->ring[s->next++ % kRing] = f;
    s->last_gf = gf;
    for (int i = 0; i < 8; ++i) s->keys[i] = keys[i];
    Prune(gf);
    ++c_.copied;
    c_.bytes += f->y.size() + f->cb.size() + f->cr.size() + f->a.size();
    return true;
  }

  // The frame the draw described by `q` shows: the newest frame of the chosen set flushed no later than the draw. null = none (counted).
  std::shared_ptr<const Frame> Find(const Query& q) {
    std::lock_guard<std::mutex> lk(m_);
    ++c_.asked;
    bool by_key = false;
    Slot* s = Choose(q, by_key);
    const bool hinted = (q.set && !plants.no_set_hint) || by_key;   // a key match is as exact as a hint
    if (!s) return nullptr;
    // newest first
    std::shared_ptr<const Frame> newest;
    uint32_t newest_seq = 0;
    std::shared_ptr<const Frame> pick;
    uint32_t pick_seq = 0;
    bool any_after = false;
    for (uint32_t back = 0; back < kRing && back < s->next; ++back) {
      const uint32_t i = (s->next - 1 - back) % kRing;
      const std::shared_ptr<const Frame>& f = s->ring[i];
      if (!f) continue;
      if (!newest) newest = f, newest_seq = s->seq[i];
      const bool after = q.seq ? s->seq[i] > q.seq : f->guest_frame > q.guest_frame;
      if (after && !plants.future_frame) {
        any_after = true;
        continue;
      }
      pick = f, pick_seq = s->seq[i];
      break;
    }
    if (plants.future_frame) pick = newest, pick_seq = newest_seq;
    if (!pick) {
      ++c_.no_flush;
      if (any_after) ++c_.flush_after, ++c_.ring_miss;   // the only frames of the set came AFTER the draw (U3: the plane was not there when the game drew)
      return nullptr;
    }
    if (plants.stale_plane) {   // the frame before the picked one
      std::shared_ptr<const Frame> older;
      for (uint32_t back = 0; back < kRing && back < s->next; ++back) {
        const std::shared_ptr<const Frame>& f = s->ring[(s->next - 1 - back) % kRing];
        if (f && f->id < pick->id && (!older || f->id > older->id)) older = f;
      }
      if (older) pick = older;
    }
    // F4: a frame older than the window is not what the draw shows (no hint: a few frames; with a hint: a set idle that long is dropped anyway)
    // render-clock: lookup - [U] movie idle / which flushed frame by guest-frame gap; the game's movie state is not looked up yet
    if (!plants.future_frame && !plants.no_age_limit && q.guest_frame > pick->guest_frame && q.guest_frame - pick->guest_frame > (hinted ? kIdleFrames : kNoHintWindow)) {
      ++c_.too_old;
      return nullptr;
    }
    if (any_after) ++c_.flush_after;
    (void)pick_seq;
    if (q.seq) ++c_.flush_before;   // an exact order: a frame is picked only when its serial <= the draw's stamp (a stamp read after a flush equals that flush's serial)
    else ++(pick->guest_frame < q.guest_frame ? c_.flush_before : c_.flush_same);
    ++c_.answered;
    return pick;
  }

  // N2 F2: the display set whose handle keys hold `key` (either buffer's key names the same set); 0 = none. The host's R9 repeat compares sets, not raw keys (the two display buffers have two keys).
  uint32_t SetOfKey(uint32_t key) {
    if (!key) return 0;
    std::lock_guard<std::mutex> lk(m_);
    Slot* s = SlotOfKey(key, 0);
    return s ? s->set : 0;
  }
  uint32_t seq() const { std::lock_guard<std::mutex> lk(m_); return seq_; }
  Census census() const { std::lock_guard<std::mutex> lk(m_); return c_; }
  size_t sets() const { std::lock_guard<std::mutex> lk(m_); return slots_.size(); }
  void Reset() {
    std::lock_guard<std::mutex> lk(m_);
    slots_.clear(), c_ = Census{}, seq_ = 0, next_id_ = 0;
  }

 private:
  struct Slot {
    uint32_t set = 0, next = 0;
    uint64_t last_gf = 0;
    std::shared_ptr<const Frame> ring[kRing];
    uint32_t seq[kRing] = {};
    uint32_t keys[8] = {};   // N2: the plane handle keys read at the last flush
  };
  Slot* FindSlot(uint32_t set) {
    for (auto& s : slots_)
      if (s.set == set) return &s;
    return nullptr;
  }
  Slot* SlotOf(uint32_t set) {
    if (Slot* s = FindSlot(set)) return s;
    slots_.push_back(Slot{});
    slots_.back().set = set;
    return &slots_.back();
  }
  // The set the query means (caller holds m_): the hint when given and held. With no hint: the sets whose newest frame matches the size (when known) AND that flushed within kNoHintWindow guest frames of the draw;
  // exactly one = that set; none = null; more than one (an entrance flushes a banner, a titantron, a mini screen and panels together) = REFUSED (counted `ambiguous`; the stream's draw is kept) (review F1).
  Slot* SlotOfKey(uint32_t key, uint32_t prefer_set) {   // caller holds m_: the preferred set when it holds the key, else the newest holder
    Slot* hit = nullptr;
    for (auto& s : slots_) {
      bool holds = false;
      for (uint32_t k : s.keys) holds = holds || k == key;
      if (!holds) continue;
      if (!hit || s.set == prefer_set || (hit->set != prefer_set && s.last_gf > hit->last_gf)) hit = &s;
    }
    return hit;
  }
  // N2: a row's own plane key picks the set whose handle keys hold it (a hint is only the last flush of ANY movie: the 30 Years menu flushes three sets per frame). No set holds it: today's rule.
  Slot* Choose(const Query& q, bool& by_key) {
    by_key = false;
    if (q.key && !plants.key_ignored) {
      Slot* hit = SlotOfKey(q.key, q.set);
      if (hit) {
        ++c_.key_hit;
        if (q.set && hit->set != q.set) ++c_.key_hint_differs;
        by_key = true;
        return hit;
      }
      ++c_.key_miss;
    }
    if (q.set && !plants.no_set_hint) return FindSlot(q.set);
    if (!q.y_w || !q.y_h) ++c_.no_size;   // once per query
    Slot* best = nullptr;
    uint32_t near_sets = 0;
    for (auto& s : slots_) {
      if (!s.next) continue;
      const std::shared_ptr<const Frame>& f = s.ring[(s.next - 1) % kRing];
      if (!f) continue;
      if (!plants.any_size && q.y_w && q.y_h && (f->w != q.y_w || f->h != q.y_h)) continue;
      const uint64_t d = s.last_gf > q.guest_frame ? s.last_gf - q.guest_frame : q.guest_frame - s.last_gf;
      if (d > kNoHintWindow) continue;
      ++near_sets;
      best = &s;
    }
    if (near_sets > 1 && !plants.no_ambiguity_guard) {
      ++c_.ambiguous;
      return nullptr;
    }
    return best;
  }
  std::shared_ptr<Frame> TakeSpare(uint32_t set) {
    std::lock_guard<std::mutex> lk(m_);
    if (Slot* s = FindSlot(set)) {
      std::shared_ptr<const Frame>& old = s->ring[s->next % kRing];
      if (old && old.use_count() == 1) {
        std::shared_ptr<Frame> f = std::const_pointer_cast<Frame>(old);
        old.reset();
        return f;
      }
    }
    return std::make_shared<Frame>();
  }
  void Prune(uint64_t gf) {   // caller holds m_
    for (size_t i = 0; i < slots_.size();) {
      // render-clock: lookup - [U] movie set idle prune by guest-frame gap; the game's movie state is not looked up yet
      if (slots_[i].last_gf + kIdleFrames < gf) slots_.erase(slots_.begin() + long(i)), ++c_.sets_pruned;
      else ++i;
    }
    while (slots_.size() > kMaxSets) {
      size_t oldest = 0;
      for (size_t i = 1; i < slots_.size(); ++i)
        if (slots_[i].last_gf < slots_[oldest].last_gf) oldest = i;
      slots_.erase(slots_.begin() + long(oldest)), ++c_.sets_pruned;
    }
  }

  mutable std::mutex m_;
  std::mutex copy_m_;
  screens::PlaneStore copier_;   // only CopyDisplay is used: no binding, no channel chain
  std::vector<Slot> slots_;
  Census c_;
  uint32_t seq_ = 0;
  uint64_t next_id_ = 0;
};

// MovieOurs reads this at every draw: true = take the planes from the store (the plane flush's copies), never from the stream draw's images. Set once at start (wwe2k14_app.h OnPostSetup, from
// --wwe2k14_rdr_movie_planes) or by an offline tool (fview / the unit). Default false: the player path is unchanged.
inline std::atomic<bool> g_guest_planes{false};

inline MovieStore& Store() {
  static MovieStore s;
  return s;
}

// N2: the plane-key census (one line, printed next to the [host] line; empty until a movie row asked with a key). key_hit = the row's key was held by a set; key_miss = no set held it (the hint's rule served it);
// key_hint_differs = the key's set was not the row's hint (the last flush of any movie): > 0 proves the old hint was wrong.
inline std::string KeyCensusLine() {
  const Census c = Store().census();
  if (!c.key_hit && !c.key_miss) return std::string();
  return "[host][movie_planes] key_hit " + std::to_string((unsigned long long)c.key_hit) + ", key_miss " + std::to_string((unsigned long long)c.key_miss) + ", key_hint_differs " + std::to_string((unsigned long long)c.key_hint_differs) +
         ", sets " + std::to_string((unsigned long long)Store().sets()) + ", asked " + std::to_string((unsigned long long)c.asked) + ", answered " + std::to_string((unsigned long long)c.answered);
}

// ---- the upload: a frame's four planes into OUR images -----------------------------------------------------------------------------------------------
// Sink: Create(w, h) -> image id (0 = failed; one 8-bit channel), Update(id, bytes, n) (tightly packed rows), Destroy(id). MovieOurs' sink is the Renderer's CreateImage / UpdateImage / DestroyImage; the unit's records the bytes.
// One image set per display set. The images take a frame's bytes once (by Frame::id); a draw that wants an OLDER frame than the one uploaded is re-uploaded (counted `reuploads`).
struct UploadCensus {
  uint64_t created = 0, uploads = 0, reuploads = 0, bytes = 0, shared = 0, failed = 0;
};
template <class Sink>
class PlaneImages {
 public:
  explicit PlaneImages(Sink sink) : sink_(std::move(sink)) {}
  ~PlaneImages() { for (auto& e : sets_) Drop(e); }
  size_t sets() const { return sets_.size(); }
  struct Ids {
    uint32_t id[4] = {};   // draw slot order: Y, Cb, Cr, A
    bool alpha = true;     // S103 SVY: false = the movie has no alpha plane (T_SpriteYUV): id[3] is the Y image again, a stand-in the no-alpha program never reads
  };
  // false = the frame cannot be shown (a plane is missing, an image could not be made). S103 SVY: a frame with no alpha plane (the season videos: T_SpriteYUV) is served with three images; Ids::alpha says so.
  bool Get(uint32_t set, const Frame& f, Ids& out) {
    if (plant_alpha_required && !f.has_alpha) return false;   // plant (the unit): the old rule, a no-alpha frame is refused (the season video stays black): "upload.no_alpha_served"
    if (f.y.empty() || f.cb.empty() || f.cr.empty() || (f.has_alpha && f.a.empty())) return false;
    const bool alpha = f.has_alpha;
    const int planes = alpha ? 4 : 3;
    Entry* e = nullptr;
    for (auto& x : sets_)
      if (x.set == set) e = &x;
    if (!e) {
      if (sets_.size() >= kMaxSets && !plant_unbounded) {   // review F7: the image sets are bounded like the store's (least recently used goes, its images destroyed)
        size_t old = 0;
        for (size_t i = 1; i < sets_.size(); ++i)
          if (sets_[i].used < sets_[old].used) old = i;
        Drop(sets_[old]);
        sets_.erase(sets_.begin() + long(old));
      }
      sets_.push_back(Entry{}), e = &sets_.back(), e->set = set;
    }
    e->used = ++tick_;
    if (e->id[0] && (e->w != f.w || e->h != f.h || e->cw != f.cw || e->ch != f.ch || e->alpha != alpha)) Drop(*e);   // a pooled set reopened with a movie of another size
    if (!e->id[0]) {
      const uint32_t dw[4] = {f.w, f.cw, f.cw, f.w}, dh[4] = {f.h, f.ch, f.ch, f.h};
      for (int k = 0; k < planes; ++k) {
        e->id[k] = sink_.Create(dw[k], dh[k]);
        if (!e->id[k]) {
          Drop(*e);
          ++c_.failed;
          return false;
        }
        ++c_.created;
      }
      e->w = f.w, e->h = f.h, e->cw = f.cw, e->ch = f.ch, e->frame = 0, e->alpha = alpha;
    }
    if (e->frame == f.id && !plant_always_upload) ++c_.shared;
    else {
      sink_.Update(e->id[0], f.y.data(), f.y.size());
      sink_.Update(e->id[1], f.cb.data(), f.cb.size());
      sink_.Update(e->id[2], f.cr.data(), f.cr.size());
      if (alpha) sink_.Update(e->id[3], f.a.data(), f.a.size());
      ++c_.uploads;
      if (e->frame > f.id) ++c_.reuploads;
      c_.bytes += f.y.size() + f.cb.size() + f.cr.size() + (alpha ? f.a.size() : 0);
      e->frame = f.id;
    }
    for (int k = 0; k < 4; ++k) out.id[k] = e->id[k];
    out.alpha = alpha;
    if (!alpha) out.id[3] = e->id[0];   // the stand-in
    return true;
  }
  const UploadCensus& census() const { return c_; }
  // S103 P7 H2 (R9): true while set `set` still owns exactly the images `id` (draw slot order). A set that was pruned or resized has had its images destroyed: such ids are never shown again.
  bool Holds(uint32_t set, const uint32_t id[4]) const {
    for (const auto& x : sets_)
      if (x.set == set && x.id[0]) {
        for (int k = 0; k < 4; ++k)
          if ((k == 3 && !x.id[3] ? x.id[0] : x.id[k]) != id[k]) return false;   // (a no-alpha set's 4th slot is its Y image)
        return true;
      }
    return false;
  }
  bool plant_alpha_required = false;  // plant (the unit): a frame with no alpha plane is refused (S103 SVY past bug)
  bool plant_unbounded = false;       // plant (the unit): the image sets are never bounded (review F7): "upload.bounded"
  bool plant_always_upload = false;   // plant (the unit): a frame the images already hold is sent again ("upload.shared")

 private:
  struct Entry {
    uint32_t set = 0, w = 0, h = 0, cw = 0, ch = 0, id[4] = {};
    bool alpha = true;
    uint64_t frame = 0, used = 0;
  };
  void Drop(Entry& e) {
    for (auto& id : e.id)
      if (id) sink_.Destroy(id), id = 0;
    e.w = e.h = e.cw = e.ch = 0, e.frame = 0;
  }
  Sink sink_;
  std::vector<Entry> sets_;
  UploadCensus c_;
  uint64_t tick_ = 0;
};

// ---- what a draw gets: OUR images in the four slots (the one routine; MovieOurs and the offline unit run it) ---------------------------------------------
// `stream_image[s]` = the image the STREAM draw binds in slot s (0 = none). It is only a bind REQUIREMENT (a slot with no stream image has no sampler to use, so the stream's draw is kept): a stream image id is never
// returned in `out`. Plant leak_stream hands the stream's ids out ("guest.no_stream_image").
enum class Serve { Ok, NoStreamSlot, NoFrame, Refused };
// S103 SVY review: the served set's alpha-ness must match the draw's kind. A T_SpriteYUVAlpha draw needs a real alpha plane; a T_SpriteYUV (no alpha) draw must get a no-alpha set. A row's plane_set is only "the last flush of any movie"
// (Store::plane_set), so with two movies in a frame (the 30 Years menu's season previews + the Alpha backdrop) a row can be served the other movie's set: such a draw is refused, never drawn with the wrong planes.
// `stand_in` = GuestSlot::stand_in of slot 3 (the served set has no alpha plane). `plant` = the unit's twin (no guard).
inline bool SetMatchesKind(bool row_wants_alpha, bool stand_in, bool plant = false) { return plant || row_wants_alpha == !stand_in; }
// The slot view: the planes are one-channel R8 images read through their own view (format Undefined = the image's own); the swizzle is the IDENTITY (VK_COMPONENT_SWIZZLE_IDENTITY = 0 x4: the shader reads .r of an R8 image).
inline constexpr uint8_t kSwizzleIdentityComponent = 0;
struct GuestSlot {
  uint32_t image = 0;
  bool stand_in = false;   // S103 SVY: the set has no alpha plane (every slot says so); slot 3 is then the Y image again (a T_SpriteYUV draw never reads it; a T_SpriteYUVAlpha draw is refused)
  uint8_t swizzle[4] = {kSwizzleIdentityComponent, kSwizzleIdentityComponent, kSwizzleIdentityComponent, kSwizzleIdentityComponent};
};
template <class Sink>
Serve ServeGuest(MovieStore& store, PlaneImages<Sink>& images, const Query& q, const uint32_t stream_image[4], bool swap_uv, GuestSlot out[4], uint32_t* served_set = nullptr) {
  for (int s = 0; s < 4; ++s)
    if (!stream_image[s]) return Serve::NoStreamSlot;
  const std::shared_ptr<const Frame> f = store.Find(q);
  if (!f) return Serve::NoFrame;
  typename PlaneImages<Sink>::Ids ids;
  if (!images.Get(f->set, *f, ids)) return Serve::Refused;
  if (served_set) *served_set = f->set;   // S103 P7 H2 (R9): the set of the frame served (the hint in the query may be 0 = any)
  if (swap_uv) std::swap(ids.id[1], ids.id[2]);
  for (int s = 0; s < 4; ++s) {
    out[s] = GuestSlot{};
    out[s].image = store.plants.leak_stream ? stream_image[s] : ids.id[s];
    out[s].stand_in = !ids.alpha;   // every slot of a no-alpha set says so (the host asks whichever slot it is on); only slot 3 is the stand-in image
  }
  return Serve::Ok;
}

}  // namespace rdr_scene::live::movieplanes
