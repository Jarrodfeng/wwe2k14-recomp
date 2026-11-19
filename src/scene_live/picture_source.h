// PictureSource (Session 103, landing 2, piece PN2; docs\renderer\PICTURES_NO_STREAM.md sections 1, 4, 5): the stream-free answer to "which picture does this 2D row name?". Copyright (c) the wwe2k14-recomp
// authors. BSD-3-Clause.
//
// A 2D row names its picture by the game's texture handle key (word 0 of the 8-byte handle at sprite + 400 / glyph + 416: low 16 bits = the slot of the handle registry [0x8392DD9C], high 16 bits = a birth
// counter). The counter is the game's global texture birth counter in steps of 2, so a key is unique for 32768 births, NOT forever: after a wrap the same key can name another texture (the old answer is
// then dropped by OnBorn, which erases the key's answer: that is the wrap's handling, kept on purpose). This class turns a key into OUR picture with no command stream and no captured image:
//   File     the texture's bytes were read from guest memory ONCE (the first time a row names the key), hashed (xxh3 of mip 0, the contract's ONE hash, image_content_table.h) and found in the player's own
//            pac index (UiFileIndex::Find / FindAll, the join the stream path's UiCore made with the same hash): the answer is the index entry, our UiFileLoader serves the file;
//   Guest    no indexed file holds those bytes: the mip 0 blocks as read (BC1 / BC2 / BC3 and k_8_8_8_8), uploaded as our own image;
//   Refused  by name, never a guess: a key born by a render-target creator (`gpu`: the pause blur is P5's, the CAW preview P10's), a movie plane (`movie`: P4's), or what guest_tex says (format, size, memory ...).
// Lifetime: the game's birth sub_822951F0 and death sub_82294FD0 are the events (OnBorn / OnDied, picture_hooks.h). Hits never re-read or re-hash (the I2 table, quads2d_pictures.h F2, re-read mip 0 on
// EVERY hit); the death (sub_82BAF210, the registry's slot free: every path) leaves a tombstone and the HOST drops the key once it has drawn a record newer than the death (TrimDrawn; never the drawing thread: it
// may be several records ahead of the host). U-R1 (a refill in place without a birth) is [U]: a cheap
// guard (a 64-block sample hash, once per key per frame) re-reads the key when it moves and counts `refill_unannounced`.
// Threads: OnBorn / OnDied / Seen run on the game's threads (the drawing thread reads guest memory: the texture is alive and filled at the row's op-19), Lookup / Trim on the host thread; one mutex, the
// guest read and the hash are outside it. Pure: no game or SDK includes; the memory is a template parameter (guest_texture.h's contract: `M::u32(addr)` + mutable `fail`, `P(addr, n, dst)` physical reads).
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "../renderer_adapter/image_content_table.h"
#include "../renderer_scene/ui_file_index.h"
#include "guest_texture.h"

namespace rdr_scene::live::picsrc {

namespace gt = rdr_scene::live::guest_tex;

// Test only (the unit's planted twins): 1 death_ignored (OnDied / TrimDrawn keep everything), 2 counter_ignored (a key is looked up by its slot only), 3 rehash_every_hit (a hit re-reads and re-hashes),
// 4 rt_as_file (a render-target creator's key is read as bytes), 5 wrong_mip (the join hashes the blocks without their first row: not mip 0's bytes), 6 race_ignored (a read that finished after the key's death
// stores a LIVE answer), 7 unknown_8888 (an Unknown creator's uncompressed picture is read), 8 file_any_order (a 4-byte picture joins whatever file holds the bytes, any channel order),
// 9 no_budget (the resident cap is ignored), 10 keep_blocks (ReleaseBlocks does nothing).
inline int g_pic_plant = 0;
inline bool g_pic_keep_bridge = false;   // T2F: set by the PictureResolver (the only consumer that uploads and releases a File answer's bridge bytes); without one a File answer keeps no blocks
inline uint64_t g_pic_resident_cap = 192ull << 20;   // A6: Guest blocks held at once (test: the unit lowers it); past it a first sight is refused `memory_budget`

enum class Kind : uint8_t { None, File, Guest, Refused };
enum class Why : uint8_t { Ok, NoKey, Gpu, Movie, NoHandle, NotPlain2D, Format, Size, Memory, Unreadable, Unclassified, Budget, Count };
inline const char* WhyName(Why w) {
  static const char* const k[] = {"ok", "no_key", "gpu_picture", "movie", "no_handle", "not_plain_2d", "format", "size", "memory", "unreadable", "unclassified_creator", "memory_budget"};
  return uint8_t(w) < uint8_t(Why::Count) ? k[uint8_t(w)] : "?";
}

// Who made the key (from the birth hook's call site = lr - 4; docs\renderer\PICTURES_NO_STREAM.md section 2; the table below is `q callers 0x822951F0`, linted by tools\scene\picture_creators_lint.py).
enum class Creator : uint8_t { Unknown, Loader, Movie, Gpu };
struct CreatorSite {
  uint32_t site;
  Creator what;
};
// Loader = the named-texture loaders from a pac in memory (sub_82BAA6F8 / 82BAA568 / 82BA9D68 / 82BA9EF8 / 82BAA048 / 82BAA958 / 82BA9CB8 / 82BAA158 / 82B124A0): FILE bytes. Movie = sub_827FBB10 (P4's).
// Gpu = the render-target creators (sub_825BB398, 827E8CC8 x4, 827EA4F0 x2, 828135A0, 827506A0, 82BC39E0, 82BCB9B8: render_targets.md / H_textures 1.3). Unknown = [U] which family (PN3's census answers).
inline constexpr CreatorSite kCreators[] = {
    {0x8229620C, Creator::Unknown}, {0x82296234, Creator::Unknown}, {0x8229F1C0, Creator::Unknown}, {0x8234E19C, Creator::Unknown}, {0x825BB4E8, Creator::Gpu},     {0x827507A4, Creator::Gpu},
    {0x827E8D64, Creator::Gpu},     {0x827E8E78, Creator::Gpu},     {0x827E8F34, Creator::Gpu},     {0x827E9040, Creator::Gpu},     {0x827EA5FC, Creator::Gpu},     {0x827EA6E4, Creator::Gpu},
    {0x827FBB84, Creator::Movie},   {0x82813750, Creator::Gpu},     {0x828255F4, Creator::Unknown}, {0x8294023C, Creator::Unknown}, {0x82B12620, Creator::Loader},  {0x82B126F0, Creator::Loader},
    {0x82BA9D40, Creator::Loader},  {0x82BA9EBC, Creator::Loader},  {0x82BAA020, Creator::Loader},  {0x82BAA12C, Creator::Loader},  {0x82BAA1A0, Creator::Loader},  {0x82BAA6C0, Creator::Loader},
    {0x82BAA908, Creator::Loader},  {0x82BAACD0, Creator::Loader},  {0x82BB07C4, Creator::Unknown}, {0x82BB081C, Creator::Unknown}, {0x82BB0874, Creator::Unknown}, {0x82BB08D4, Creator::Unknown},
    {0x82BB092C, Creator::Unknown}, {0x82BB097C, Creator::Unknown}, {0x82BB0C98, Creator::Unknown}, {0x82BB0D00, Creator::Unknown}, {0x82BB0D50, Creator::Unknown}, {0x82BC3A80, Creator::Gpu},
    {0x82BCBA30, Creator::Gpu},     {0x82C20A60, Creator::Unknown}, {0x82C2219C, Creator::Unknown}, {0x82C221D8, Creator::Unknown}, {0x82C2AC58, Creator::Unknown}, {0x83327E5C, Creator::Unknown},
    {0x83343BEC, Creator::Unknown}, {0x833A1ED0, Creator::Unknown}, {0x83418F50, Creator::Unknown}};
inline Creator CreatorOfSite(uint32_t site) {
  for (const CreatorSite& c : kCreators)
    if (c.site == site) return c.what;
  return Creator::Unknown;
}

// One key's answer (a copy the host owns; `blocks` is shared and immutable).
struct Answer {
  Kind kind = Kind::None;
  Why why = Why::Ok;
  uint32_t key = 0;
  uint64_t serial = 0;                       // changes whenever the key's answer is made again (a refill the guard caught): the host's image cache keys on (key, serial)
  uint32_t entry = 0;                        // File: the UiFileIndex entry
  uint32_t width = 0, height = 0;
  rdr::Format format = rdr::Format::Undefined;   // File / Guest: the picture's format class (BC1 / BC2 / BC3 / BGRA8)
  uint64_t hash = 0;                         // xxh3 of mip 0 as read (the join's hash)
  std::shared_ptr<const std::vector<uint8_t>> blocks;   // Guest: mip 0, linear little-endian blocks
  gt::Shape shape;                           // Guest
};

struct Counters {
  uint64_t born = 0, died = 0, died_unseen = 0, seen = 0, hits = 0;   // `seen` = first-sight reads; `hits` = a key already answered (no read, no hash)
  uint64_t joined = 0, guest_copied = 0, refused[uint8_t(Why::Count)] = {};
  uint64_t reads = 0, hashes = 0;            // guest reads / hashes made (reads > seen means a re-read: the guard or a plant)
  uint64_t guard_checks = 0, refill_unannounced = 0, rejoined = 0;   // rejoined: a Guest answer that found its file after more pacs were indexed
  uint64_t trimmed = 0, lookups = 0, lookup_missing = 0;
  uint64_t resident_bytes = 0, blocks_released = 0;                  // Guest blocks held (A6: capped by kMaxResidentBytes) / freed after the host uploaded them
  uint64_t guest_parity_ok = 0, guest_parity_bad = 0, file_parity_ok = 0, file_parity_bad = 0, parity_skipped = 0, parity_no_answer = 0;   // dev: the stream path's content hash of the same key vs ours (NoteStreamHash)
};

class PictureSource {
 public:
  static constexpr uint32_t kGuardBlocks = 64;
  static constexpr uint32_t kMaxPictureBytes = 4u << 20;   // a first-sight read of one picture (a 512 x 1024 8888 menu picture is 2 MB; 1280 x 720 8888 is 3.5 MB)
  explicit PictureSource(const UiFileIndex* index = nullptr) : index_(index) {}
  // The index the joins are made against (the owner of the player's pac index: set once it exists; null = every key is a Guest copy until then, and the rejoin below picks them up).
  void SetIndex(const UiFileIndex* index) { index_.store(index, std::memory_order_release); }

  // ---- the game's events (any thread) --------------------------------------------------------------------------------------------------------------------
  // Birth: sub_822951F0 returned the pair {key, cache}; `site` = the caller's call site (lr - 4).
  void OnBorn(uint32_t key, uint32_t site) {
    if (!key) return;
    std::lock_guard<std::mutex> l(mu_);
    ++n_.born;
    Life& life = lives_[Slot(key)];
    life = Life{};
    life.key = key, life.creator = CreatorOfSite(site), life.site = site;
    if (g_pic_plant != 2) EraseAnswer(Slot(key));   // a new life in a slot (or a counter wrap): the old answer (if any) is not this texture's (plant counter_ignored: the birth does not drop it)
  }
  // Death: sub_82BAF210 (the registry's slot free) returned a match; `frame` = the guest frame. A2: it ALWAYS leaves a tombstone (dead, died_at), also for a key never seen: a Read that is in flight on the
  // drawing thread (guest read outside the lock) must not store a live answer for a dead key (Read keeps dead / died_at when the key's entry is dead). TrimDrawn drops it.
  void OnDied(uint32_t key, uint64_t frame) {
    if (!key || g_pic_plant == 1) return;
    std::lock_guard<std::mutex> l(mu_);
    ++n_.died;
    const auto it = answers_.find(Slot(key));
    if (it == answers_.end() || it->second.a.key != key) {
      ++n_.died_unseen;
      if (it != answers_.end()) EraseAnswer(Slot(key));   // (plant counter_ignored only: another key's entry in the slot)
      Entry& t = answers_[Slot(key)];
      t = Entry{};
      t.a.key = key, t.dead = true, t.died_at = frame;   // kind None: never answered, Lookup says missing
    } else {
      it->second.died_at = frame, it->second.dead = true;
    }
    lives_.erase(Slot(key));
  }

  // ---- the drawing thread: a row names `key` (first sight reads; later sights are hits) --------------------------------------------------------------------
  // `frame` = the guest frame of the row. Mem / Phys: see the header.
  template <class Mem, class Phys>
  void Seen(uint32_t key, uint64_t frame, const Mem& mem, Phys&& phys) {
    if (!key) return;
    Creator creator = Creator::Unknown;
    bool have_entry = false, guard = false;
    gt::Shape guard_shape;
    uint64_t guard_hash = 0;
    Creator guard_creator = Creator::Unknown;
    {
      std::lock_guard<std::mutex> l(mu_);
      if (const auto lf = lives_.find(Slot(key)); lf != lives_.end() && lf->second.key == key) creator = lf->second.creator;
      const auto it = answers_.find(Slot(key));
      if (it != answers_.end() && (it->second.a.key == key || g_pic_plant == 2)) {
        Entry& e = it->second;
        if (e.dead) return;   // a key the game released: its answer serves out its grace, it is never read again (the registry no longer resolves it)
        have_entry = true;
        const UiFileIndex* ix = index_.load(std::memory_order_acquire);
        if (e.a.kind == Kind::Guest && ix && e.index_gen != ix->generation()) {   // more pacs were indexed since the join missed (UiCore's `unjoined, retry when the index moves`): the held hash is looked up again, no guest read
          e.index_gen = ix->generation();
          uint32_t id = 0;
          if (FindFile(*ix, e.a.shape, e.a.format, e.a.hash, id)) ReleaseBytes(e), e.a.kind = Kind::File, e.a.entry = id, e.a.blocks.reset(), e.a.serial = ++serial_, ++n_.rejoined, --n_.guest_copied, ++n_.joined;
        }
        {
          ++n_.hits;
          if (e.guard_frame != frame && (e.a.kind == Kind::File || e.a.kind == Kind::Guest)) guard = true, guard_shape = e.a.shape, guard_hash = e.sample, guard_creator = e.creator, e.guard_frame = frame;
          if (g_pic_plant == 3) have_entry = false;   // rehash_every_hit
        }
      }
    }
    if (have_entry) {
      if (guard) Guard(key, frame, guard_creator, guard_shape, guard_hash, mem, phys);
      return;
    }
    Read(key, frame, creator, mem, phys);
  }

  // ---- the host thread ------------------------------------------------------------------------------------------------------------------------------------
  // The answer for a key (false: nothing known; the row is dropped `picture`). A dead key within its grace still answers.
  bool Lookup(uint32_t key, Answer& out) {
    std::lock_guard<std::mutex> l(mu_);
    ++n_.lookups;
    const auto it = answers_.find(Slot(key));
    if (it == answers_.end() || (it->second.a.key != key && g_pic_plant != 2) || it->second.a.kind == Kind::None) return ++n_.lookup_missing, false;   // (a tombstone of a key never answered: missing)
    out = it->second.a;
    return true;
  }
  // Is the key still answered, and with which serial? (no counters: the host's per-frame release walk)
  bool Alive(uint32_t key, uint64_t& serial) const {
    std::lock_guard<std::mutex> l(mu_);
    const auto it = answers_.find(Slot(key));
    if (it == answers_.end() || it->second.a.key != key || it->second.a.kind == Kind::None) return false;
    return serial = it->second.a.serial, true;
  }
  // S103 CTX2 (CAWD site 8): who made `key` (the birth hook's creator for the key's live slot); Unknown when the key was never seen born (feeders off, or born before the hook). Any thread; no counters.
  // BirthOf: false = never seen born (feeders off, or born before the hook); true = born at `site`, whose class is `creator` (Unknown = a site the kCreators table does not classify).
  bool BirthOf(uint32_t key, Creator& creator, uint32_t& site) const {
    std::lock_guard<std::mutex> l(mu_);
    const auto lf = lives_.find(Slot(key));
    if (lf == lives_.end() || lf->second.key != key) return creator = Creator::Unknown, site = 0, false;
    return creator = lf->second.creator, site = lf->second.site, true;
  }
  Creator CreatorOf(uint32_t key) const { Creator c; uint32_t s; BirthOf(key, c, s); return c; }
  uint32_t SiteOf(uint32_t key) const { Creator c; uint32_t s; BirthOf(key, c, s); return s; }
  // A6: the host uploaded the Guest answer (key, serial) as its own image: the CPU copy of the blocks is no longer needed (the guard re-reads the guest, the rejoin uses the held hash). No-op when the answer moved on.
  void ReleaseBlocks(uint32_t key, uint64_t serial) {
    std::lock_guard<std::mutex> l(mu_);
    const auto it = answers_.find(Slot(key));
    if (g_pic_plant == 10 || it == answers_.end() || it->second.a.key != key || it->second.a.serial != serial || !it->second.a.blocks) return;
    resident_bytes_ -= it->second.a.blocks->size();
    it->second.a.blocks.reset(), ++n_.blocks_released;
  }
  // Dev parity (the first live run): the STREAM path's content hash of the same key (adapter ImageContentHash over the image's mips) against ours. Comparable only for a one-mip image: the source hashes mip 0.
  void NoteStreamHash(uint32_t key, uint64_t hash, uint32_t mips) {
    std::lock_guard<std::mutex> l(mu_);
    const auto it = answers_.find(Slot(key));
    if (it == answers_.end() || it->second.a.key != key || (it->second.a.kind != Kind::Guest && it->second.a.kind != Kind::File)) return void(++n_.parity_no_answer);
    if (mips != 1) return void(++n_.parity_skipped);
    const bool ok = it->second.a.hash == hash;
    (it->second.a.kind == Kind::Guest ? (ok ? n_.guest_parity_ok : n_.guest_parity_bad) : (ok ? n_.file_parity_ok : n_.file_parity_bad)) += 1;
  }
  // A5: the HOST trims (PictureResolver::EndFrame), with the guest frame of the record it has just drawn: every row of a frame <= that was in a record <= it, so a key that died in a frame BEFORE it (died_at <
  // drawn) is no longer named by any record still to be drawn. Exact: no grace constant, no dependence on how far ahead the drawing thread is. [U] the record's frame is the same g_guest_frame the rows carry.
  void TrimDrawn(uint64_t drawn_frame) {
    if (g_pic_plant == 1) return;
    std::lock_guard<std::mutex> l(mu_);
    for (auto it = answers_.begin(); it != answers_.end();) {
      if (it->second.dead && it->second.died_at < drawn_frame) {
        ReleaseBytes(it->second);
        it = answers_.erase(it), ++n_.trimmed;
      } else {
        ++it;
      }
    }
  }
  size_t resident() const {
    std::lock_guard<std::mutex> l(mu_);
    return answers_.size();
  }
  Counters counters() const {
    std::lock_guard<std::mutex> l(mu_);
    Counters c = n_;
    c.resident_bytes = resident_bytes_;
    return c;
  }

 private:
  struct Life {
    uint32_t key = 0;
    Creator creator = Creator::Unknown;
    uint32_t site = 0;   // S103 CTX2: the birth hook's call site (lr - 4) as the game gave it; 0 only for a key never seen born
  };
  struct Entry {
    Answer a;
    uint64_t sample = 0, guard_frame = ~0ull, died_at = 0;
    uint32_t index_gen = 0;   // the file index's generation when a Guest answer was made
    Creator creator = Creator::Unknown;   // the key's creator class when the answer was made (the guard's re-read keeps it)
    bool dead = false;
  };
  void ReleaseBytes(const Entry& e) {   // (mu_ held)
    if (e.a.blocks) resident_bytes_ -= e.a.blocks->size();
  }
  void EraseAnswer(uint32_t slot) {   // (mu_ held)
    const auto it = answers_.find(slot);
    if (it == answers_.end()) return;
    ReleaseBytes(it->second);
    answers_.erase(it);
  }
  // The slot a key lives in. Normally the WHOLE key (the birth counter is part of the identity: a reused registry slot is a new key). Plant counter_ignored: the slot index only (low 16 bits).
  static uint32_t Slot(uint32_t key) { return g_pic_plant == 2 ? (key & 0xFFFFu) : key; }

  // A cheap sample of the picture: kGuardBlocks blocks spread over mip 0, hashed (not the whole read).
  template <class Phys>
  static bool SampleHash(const gt::Shape& s, Phys&& phys, uint64_t& out) {
    const uint32_t bx = s.BlocksX(), by = s.BlocksY(), log2_bpb = s.bpb == 8 ? 3u : (s.bpb == 4 ? 2u : 4u);
    const uint64_t total = uint64_t(bx) * by;
    if (!total) return false;
    rdr_adapter::ContentHasher h;
    uint8_t blk[16];
    for (uint32_t i = 0; i < kGuardBlocks; ++i) {
      const uint64_t at = total * i / kGuardBlocks;
      const uint32_t x = uint32_t(at % bx), y = uint32_t(at / bx);
      const uint32_t off = s.tiled ? gt::TiledOffset2D(int32_t(x), int32_t(y), s.pitch_blocks, log2_bpb) : (y * s.pitch_blocks + x) * s.bpb;
      if (!phys(s.base + off, s.bpb, blk)) return false;
      h.Update(blk, s.bpb);
    }
    out = h.Digest();
    return true;
  }

  // Resolves the key's texture object (the registry walk), reads mip 0, hashes it, joins the file index, stores the answer.
  template <class Mem, class Phys>
  void Read(uint32_t key, uint64_t frame, Creator creator, const Mem& mem, Phys&& phys) {
    Answer a;
    a.key = key;
    uint64_t sample = 0;
    uint32_t index_gen = 0;
    if (creator == Creator::Gpu && g_pic_plant != 4) {
      a.kind = Kind::Refused, a.why = Why::Gpu;   // a render target is never readable as bytes (a stale surface is the one wrong answer)
    } else if (creator == Creator::Movie) {
      a.kind = Kind::Refused, a.why = Why::Movie;
    } else {
      gt::Resolved r;
      gt::Shape shape;
      std::vector<uint8_t> blocks;
      gt::Why w = gt::ResolveHandle(mem, key, r);
      if (w == gt::Why::Ok) w = gt::ShapeOfFetch(r.fetch, shape, true);
      // A4: until the Unknown creators are classified (PN3's census), an uncompressed picture is read only from a named texture LOADER (a pac's bytes): BC is never a render target, k_8_8_8_8 can be.
      const bool unclassified = g_pic_plant != 7 && w == gt::Why::Ok && shape.game_format == 6 && creator != Creator::Loader;
      if (w == gt::Why::Ok && !unclassified) w = gt::ReadMip0(shape, phys, blocks, kMaxPictureBytes);
      {
        std::lock_guard<std::mutex> l(mu_);
        ++n_.reads;
      }
      if (unclassified) {
        a.kind = Kind::Refused, a.why = Why::Unclassified;
      } else if (w != gt::Why::Ok) {
        a.kind = Kind::Refused, a.why = Map(w);
      } else {
        const rdr::Format fmt = shape.game_format == 18 ? rdr::Format::BC1 : shape.game_format == 19 ? rdr::Format::BC2 : shape.game_format == 20 ? rdr::Format::BC3 : rdr::Format::BGRA8;
        a.width = shape.width, a.height = shape.height, a.format = fmt, a.shape = shape;
        const uint8_t* hp = blocks.data();
        size_t hn = blocks.size();
        if (g_pic_plant == 5 && hn > shape.BlocksX() * shape.bpb) hp += size_t(shape.BlocksX()) * shape.bpb, hn -= size_t(shape.BlocksX()) * shape.bpb;   // wrong_mip: not mip 0's bytes
        a.hash = rdr_adapter::ImageContentHash(hp, hn);
        {
          std::lock_guard<std::mutex> l(mu_);
          ++n_.hashes;
        }
        uint32_t entry = 0;
        const UiFileIndex* ix = index_.load(std::memory_order_acquire);
        index_gen = ix ? ix->generation() : 0;
        if (ix && FindFile(*ix, shape, fmt, a.hash, entry)) {
          a.kind = Kind::File, a.entry = entry;
          if (g_pic_keep_bridge && g_pic_plant != 11) a.blocks = std::make_shared<const std::vector<uint8_t>>(std::move(blocks));   // T2F bridge: the resolver draws these bytes until the file image lands (plant 11: the old throw-away)
        } else {
          a.kind = Kind::Guest, a.blocks = std::make_shared<const std::vector<uint8_t>>(std::move(blocks));
        }
        if (!SampleHash(shape, phys, sample)) sample = 0;
      }
    }
    std::lock_guard<std::mutex> l(mu_);
    ++n_.seen;
    // A2: the key may have died while this read was in flight (the guest read is outside the lock): its tombstone wins, the answer is stored dead (the host may still draw a row of the record) and TrimDrawn drops it.
    const auto old = answers_.find(Slot(key));
    const bool was_dead = g_pic_plant != 6 && old != answers_.end() && old->second.a.key == key && old->second.dead;
    const uint64_t died_at = was_dead ? old->second.died_at : 0;
    if (old != answers_.end()) ReleaseBytes(old->second);
    if (a.kind == Kind::File && a.blocks && (g_pic_plant == 12 ? resident_bytes_ + a.blocks->size() > g_pic_resident_cap : resident_bytes_ + a.blocks->size() > g_pic_resident_cap / 2)) a.blocks.reset();   // T2F: bridges live within half the cap, Guest copies always keep headroom (plant 12: the shared cap)   // T2F: over its half of the cap a File answer only loses its bridge bytes, the file path still serves it
    if (a.kind == Kind::Guest && a.blocks && g_pic_plant != 9 && resident_bytes_ + a.blocks->size() > g_pic_resident_cap) a.kind = Kind::Refused, a.why = Why::Budget, a.blocks.reset();   // A6
    Entry& e = answers_[Slot(key)];
    a.serial = ++serial_;
    e = Entry{};
    e.a = std::move(a), e.sample = sample, e.guard_frame = frame, e.index_gen = index_gen, e.creator = creator, e.dead = was_dead, e.died_at = died_at;
    if (e.a.blocks) resident_bytes_ += e.a.blocks->size();
    if (e.a.kind == Kind::File) ++n_.joined;
    else if (e.a.kind == Kind::Guest) ++n_.guest_copied;
    else ++n_.refused[uint8_t(e.a.why)];
  }

  // U-R1's guard: the sample moved under a live key = a refill the birth hook did not announce. The key's answer is made again.
  template <class Mem, class Phys>
  void Guard(uint32_t key, uint64_t frame, Creator creator, const gt::Shape& shape, uint64_t was, const Mem& mem, Phys&& phys) {
    uint64_t now = 0;
    const bool ok = SampleHash(shape, phys, now);
    bool trip = false;
    {
      std::lock_guard<std::mutex> l(mu_);
      ++n_.guard_checks;
      if (ok && now != was) trip = true, ++n_.refill_unannounced;
    }
    if (trip) Read(key, frame, creator, mem, phys);
  }

  static bool FindFile(const UiFileIndex& index, const gt::Shape& shape, rdr::Format fmt, uint64_t hash, uint32_t& entry) {
    std::vector<uint32_t> ids;
    index.FindAll(shape.width, shape.height, fmt, 1, hash, ids);
    if (ids.empty()) return false;
    if (fmt == rdr::Format::BGRA8 && g_pic_plant != 8) {   // A3: 4-byte pictures (ShapeOfFetch accepted only the fetch swizzle that reads memory bytes B, G, R, A, and the Guest copy is uploaded as BGRA8): only a BGRA8 file shows the same colours
      for (const uint32_t id : ids) {
        UiFileEntry fe;
        if (index.Get(id, fe) && fe.format == rdr::Format::BGRA8) return entry = id, true;
      }
      return false;   // an RGBA8 file of the same bytes would be R / B swapped: the picture stays a Guest copy
    }
    entry = ids.front();   // (BC: the class check is the format; the swizzle is the file's own)
    return true;
  }
  static Why Map(gt::Why w) {
    switch (w) {
      case gt::Why::NoRegistry:
      case gt::Why::NoHandle: return Why::NoHandle;
      case gt::Why::WrapperUnreadable:
      case gt::Why::NoHeader: return Why::Unreadable;
      case gt::Why::NotPlain2D: return Why::NotPlain2D;
      case gt::Why::Format: return Why::Format;
      case gt::Why::Size: return Why::Size;
      case gt::Why::Memory: return Why::Memory;
      default: return Why::Unreadable;
    }
  }

  std::atomic<const UiFileIndex*> index_;
  mutable std::mutex mu_;
  std::map<uint32_t, Life> lives_;
  std::map<uint32_t, Entry> answers_;
  uint64_t serial_ = 0;
  uint64_t resident_bytes_ = 0;   // Guest blocks held (A6)
  Counters n_;
};

}  // namespace rdr_scene::live::picsrc
