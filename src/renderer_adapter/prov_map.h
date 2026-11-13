// wwe2k14 - Session 101 lane 17 (arc A1c, docs\renderer\NAMES.md "Texture provenance"): who wrote each 4 KB page last, and the rule that turns
// that + the reader's own flags into a texture's provenance label. Split out of reader.h so the offline harness (tools\prov\prov_test.cpp)
// runs the real code. No game or SDK includes.
//
// The page map is a flat array of 64-bit atomics indexed by page, so the host writers (any thread) never take a lock and a reader never sees
// a torn entry: entry = (sequence << 32) | writer id. Writer id (32 bits, interned): bit 31 valid, bit 30 inherited (the file is the
// source's, "guessed"), bits 16-23 the host_write::Writer, bits 0-15 the file (interned in FileTable; 0 = none). Sequence = a counter
// stepped once per reported write (and per image flush, Stamp()): event order across threads, which the guest frame is not (a worker's write
// and VdSwap's frame count are unrelated clocks). The frame of a write is looked up from the sequence (FrameOf). Entry 0 = never written.
//
// Two spaces: the physical views (texture memory) by physical page; the heap (below 0xA0000000) by virtual page, kept only for the loaders'
// bytes (a file read, a decompressor) so a later memcpy of them into a texture inherits FILE, and cleared by any other reported heap write.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../guest_view.h"
#include "../host_write.h"
#include "names.h"

namespace wwe2k14::prov {

using W = wwe2k14::host_write::Writer;

constexpr uint32_t kPhysPages = ((0x1FFFFFFFu + 0x1000u) >> 12) + 2;  // ToPhysical's largest result is 0x20000FFF
constexpr uint32_t kHeapPages = 0xA0000000u >> 12;
constexpr uint32_t kMaxRangePages = 65536;  // a longer range is not a texture's
constexpr uint32_t kIdValid = 0x80000000u, kIdInherited = 0x40000000u;
// A Memcpy entry also says what its source was (carried along a chain of copies): no entry at all, or a non-loader writer's bytes.
constexpr uint32_t kIdSrcNone = 0x20000000u, kIdSrcCpu = 0x10000000u;
// A heap page a non-loader has written since a loader did: bytes are CPU's now (not "nothing known"). A physical page is `never written` only when 0.
constexpr uint64_t kCpuMarker = uint64_t(kIdValid | kIdSrcCpu | (uint32_t(W::Memcpy) << 16));

inline bool IsLoader(W w) { return w == W::FileRead || w == W::Decompress; }

struct Entry {
  bool valid = false;
  bool inherited = false;
  bool src_none = false, src_cpu = false;  // Memcpy: what its source (or the source's own source) was
  W writer = W::Unknown;
  uint16_t file = 0;
  uint32_t seq = 0;    // event order (see above)
  uint32_t frame = 0;  // the guest frame of the write (filled by PageMap::Load*)
};

inline uint64_t Pack(W w, bool inherited, uint16_t file, uint32_t seq, uint32_t extra = 0) {
  const uint32_t id = kIdValid | (inherited ? kIdInherited : 0u) | extra | (uint32_t(w) << 16) | file;
  return (uint64_t(seq) << 32) | id;
}
inline Entry Unpack(uint64_t v) {
  Entry e;
  const uint32_t id = uint32_t(v);
  e.valid = (id & kIdValid) != 0;
  if (!e.valid) return e;
  e.inherited = (id & kIdInherited) != 0;
  e.src_none = (id & kIdSrcNone) != 0;
  e.src_cpu = (id & kIdSrcCpu) != 0;
  e.writer = W((id >> 16) & 0xFF);
  e.file = uint16_t(id & 0xFFFF);
  e.seq = uint32_t(v >> 32);
  return e;
}

// File paths interned to 16 bits. Names live in a deque: c_str() stays valid (the frame's string table keys on the pointer).
class FileTable {
 public:
  uint16_t Intern(const char* path) {
    std::lock_guard lock(mu_);
    auto it = ids_.find(path);
    if (it != ids_.end()) return it->second;
    if (names_.size() >= 65535) { ++overflow_; return 0; }
    names_.emplace_back(path);
    const uint16_t id = uint16_t(names_.size());
    ids_.emplace(names_.back(), id);
    return id;
  }
  const char* Name(uint16_t id) const {
    std::lock_guard lock(mu_);
    return id && id <= names_.size() ? names_[id - 1].c_str() : nullptr;
  }
  uint64_t overflow() const { return overflow_; }

 private:
  mutable std::mutex mu_;
  std::deque<std::string> names_;
  std::unordered_map<std::string, uint16_t> ids_;
  uint64_t overflow_ = 0;
};

class PageMap {
 public:
  PageMap() : phys_(new std::atomic<uint64_t>[kPhysPages]()), heap_(new std::atomic<uint64_t>[kHeapPages]()) {}

  // One report from a host writer. `file` null unless a FileRead; a Decompress takes the newest read's file. `source` (Memcpy): the copy's
  // source address, page by page, so the destination inherits a loader's FILE owner.
  // Returns the write's sequence number (0 = nothing recorded).
  uint32_t Note(uint32_t addr, uint32_t length, W writer, const char* file, uint32_t source, uint32_t frame) {
    if (!length) return 0;
    const bool phys = addr >= 0xA0000000u;
    const uint32_t end = addr + (length - 1);
    if (end < addr || (end >= 0xA0000000u) != phys) return 0;  // wrapped, or across the two spaces
    const uint32_t first = Index(addr), last = Index(end);
    if (last < first || last - first > kMaxRangePages) return 0;
    // A block decoder's output (BPE / zlib, host_write::NoteDecoded) names its input block: the output keeps the input's FILE owner and file. An
    // input a non-loader wrote (a decode of CPU-made bytes) makes the output a plain CPU write; an input with no entry stays a guess. (The input is one block: its first page speaks for it.)
    bool decoded_from_file = false;
    uint16_t decoded_file = 0;
    uint32_t decoded_extra = 0;  // the decode downgrade to a plain CPU write keeps what its input was
    if (writer == W::Decompress && source) {
      const Entry s = LoadRaw(source);
      if (s.valid && IsLoader(s.writer)) { decoded_from_file = true; decoded_file = s.file; }
      else if (s.valid) { writer = W::Memcpy; decoded_extra = s.writer == W::Memcpy && s.src_none ? kIdSrcNone : kIdSrcCpu; }  // a known non-loader input (CPU-made bytes): a plain CPU write
      // (no entry at all: a block read before the reader started recording, the boot / menu case: keep the Decompress guess, the newest read's file)
      source = 0;  // (not a copy: no per-page source below)
    }
    const bool loader = IsLoader(writer);
    if (!phys && !loader && !(writer == W::Memcpy && source)) { Clear(first, last); return 0; }  // a heap write: drops stale ownership
    const uint32_t seq = Stamp(frame);
    uint16_t file_id = 0;
    if (decoded_from_file) {
      file_id = decoded_file;
    } else if (file) {
      file_id = files_.Intern(file);
      last_file_.store(file_id, std::memory_order_relaxed);
    } else if (writer == W::Decompress) {  // (a decompressor that names no input: the newest read's file, a guess)
      file_id = last_file_.load(std::memory_order_relaxed);
    }
    std::atomic<uint64_t>* space = phys ? phys_.get() : heap_.get();
    for (uint32_t p = first; p <= last; ++p) {
      uint32_t extra = decoded_extra;
      if (writer == W::Memcpy && source) {  // this page's source bytes: dest page start - dest start, from the source
        const uint32_t off = p == first ? 0u : ((addr & ~0xFFFu) + ((p - first) << 12)) - addr;
        const Entry s = LoadRaw(source + off);
        if (s.valid && IsLoader(s.writer)) {  // a loader's bytes moved into place stay FILE (the file is the source's)
          space[p].store(Pack(W::Decompress, true, s.file, seq), std::memory_order_release);
          if (phys && p == first) cen_[0].fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        // census (S101 lane17, Check C): where a memcpy into texture memory got its bytes
        extra = !s.valid ? kIdSrcNone : s.writer != W::Memcpy ? kIdSrcCpu : s.src_none ? kIdSrcNone : kIdSrcCpu;
        if (phys && p == first) {
          const int cat = !s.valid ? 1 : s.writer != W::Memcpy ? 2 : s.src_none ? 3 : 4;
          cen_[cat].fetch_add(1, std::memory_order_relaxed);
          if (cat == 1 || cat == 3) {  // the first few unowned sources of the window: where to look next
            const uint32_t n = sample_n_.load(std::memory_order_relaxed) < 8 ? sample_n_.fetch_add(1, std::memory_order_relaxed) : 8u;  // (a plain load first: no shared-line write once full)
            if (n < 8) { sdst_[n].store(addr, std::memory_order_relaxed); ssrc_[n].store(source, std::memory_order_relaxed); slen_[n].store(length, std::memory_order_relaxed); }
          }
        }
        if (!phys) {  // (S101 lane17 review: load first, a plain store would dirty a shared cache line on every heap memcpy)
          if (space[p].load(std::memory_order_relaxed)) space[p].store(kCpuMarker, std::memory_order_release);
          continue;
        }
      }
      space[p].store(Pack(writer, decoded_from_file, file_id, seq, extra), std::memory_order_release);
    }
    return seq;
  }

  // The census of memcpys into texture (physical) pages since the last call, by where their source stood: [0] a loader's bytes (FILE carried),
  // [1] no entry at all, [2] a non-loader writer's bytes, [3] a copy of a copy whose root had no entry, [4] a copy of a copy of CPU bytes;
  // and the first eight unowned (kinds 1 and 3) samples as dst / src / length.
  struct Census {
    uint64_t kind[5] = {};
    uint32_t samples = 0;
    uint32_t dst[8] = {}, src[8] = {}, len[8] = {};
  };
  Census TakeCensus() {
    Census c;
    for (int i = 0; i < 5; ++i) c.kind[i] = cen_[i].exchange(0, std::memory_order_relaxed);
    const uint32_t n = sample_n_.exchange(0, std::memory_order_relaxed);
    c.samples = n < 8 ? n : 8;
    for (uint32_t i = 0; i < c.samples; ++i) { c.dst[i] = sdst_[i].load(std::memory_order_relaxed); c.src[i] = ssrc_[i].load(std::memory_order_relaxed); c.len[i] = slen_[i].load(std::memory_order_relaxed); }
    return c;
  }
  // The next event number; `frame` (>= 0) notes the guest frame this sequence belongs to. Flushes call it too (frame = their frame).
  uint32_t Stamp(uint32_t frame) {
    const uint32_t s = seq_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (last_frame_.exchange(frame, std::memory_order_relaxed) != frame || !any_frame_.exchange(true, std::memory_order_relaxed)) {
      std::lock_guard lock(frames_mu_);
      const uint32_t at = frames_.empty() || s > frames_.back().first ? s : frames_.back().first;  // (a racing thread: keep the list sorted)
      frames_.emplace_back(at, frame);
    }
    return s;
  }
  // The guest frame that was current when sequence `seq` was stamped (0 before the first).
  uint32_t FrameOf(uint32_t seq) const {
    std::lock_guard lock(frames_mu_);
    uint32_t frame = 0;
    for (size_t lo = 0, hi = frames_.size(); lo < hi;) {  // the last element with first <= seq
      const size_t mid = (lo + hi) / 2;
      if (frames_[mid].first <= seq) { frame = frames_[mid].second; lo = mid + 1; } else { hi = mid; }
    }
    return frame;
  }
  Entry Load(uint32_t addr) const { return Framed(LoadRaw(addr)); }
  // A texture header's base address (a CPU view address or already physical): always the physical space.
  Entry LoadPhysical(uint32_t addr) const {
    const uint32_t i = wwe2k14::guest_view::ToPhysical(addr) >> 12;
    return i < kPhysPages ? Framed(Unpack(phys_[i].load(std::memory_order_acquire))) : Entry{};
  }
  Entry LoadPhysicalRaw(uint32_t addr) const {  // no frame lookup (no lock): the entry's own words
    const uint32_t i = wwe2k14::guest_view::ToPhysical(addr) >> 12;
    return i < kPhysPages ? Unpack(phys_[i].load(std::memory_order_acquire)) : Entry{};
  }
  const char* FileName(uint16_t id) const { return files_.Name(id); }
  FileTable& files() { return files_; }
  void Reset() {
    for (uint32_t i = 0; i < kPhysPages; ++i) phys_[i].store(0, std::memory_order_relaxed);
    for (uint32_t i = 0; i < kHeapPages; ++i) heap_[i].store(0, std::memory_order_relaxed);
    std::lock_guard lock(frames_mu_);
    frames_.clear();
    any_frame_ = false;
  }

 private:
  Entry LoadRaw(uint32_t addr) const {
    const bool phys = addr >= 0xA0000000u;
    const uint32_t i = Index(addr);
    if (i >= (phys ? kPhysPages : kHeapPages)) return Entry{};
    return Unpack((phys ? phys_.get() : heap_.get())[i].load(std::memory_order_acquire));
  }
  Entry Framed(Entry e) const {
    if (e.valid) e.frame = FrameOf(e.seq);
    return e;
  }
  static uint32_t Index(uint32_t addr) { return addr >= 0xA0000000u ? (wwe2k14::guest_view::ToPhysical(addr) >> 12) : (addr >> 12); }
  void Clear(uint32_t first, uint32_t last) {  // (heap space) only an entry that exists is touched
    for (uint32_t p = first; p <= last; ++p)
      if (heap_[p].load(std::memory_order_relaxed)) heap_[p].store(kCpuMarker, std::memory_order_release);
  }
  std::unique_ptr<std::atomic<uint64_t>[]> phys_, heap_;
  FileTable files_;
  std::atomic<uint16_t> last_file_{0};
  std::atomic<uint32_t> seq_{0};
  std::atomic<uint64_t> cen_[5] = {};
  std::atomic<uint32_t> sample_n_{0}, sdst_[8] = {}, ssrc_[8] = {}, slen_[8] = {};
  std::atomic<uint32_t> last_frame_{0};
  std::atomic<bool> any_frame_{false};
  mutable std::mutex frames_mu_;
  std::vector<std::pair<uint32_t, uint32_t>> frames_;  // (first sequence of a frame, the frame), sorted
};

// (leaked on purpose: a late writer thread at shutdown must not find it destroyed)
inline PageMap& Map() {
  static PageMap& m = *new PageMap;
  return m;
}

// ---- when provenance is recorded (S101 lane17 review: players pay nothing for diagnostics) --------------------------------------------------
// The writer hook is installed at app start (OnPreSetup, so frame-0 reads are caught) only when provenance is asked for: the cvar, the bug
// button's case ring, a +dev PC owner-mode build (the ring's own rule), or a capture armed at start. Everything else installs nothing.
inline bool WantedAtStart(bool cvar, bool case_ring, bool dev_owner_build, bool capture_frames_at_start) {
  return cvar || case_ring || dev_owner_build || capture_frames_at_start;
}
// The hook's own guard: record while provenance is on, or while a capture / the live mirror runs and the hook happens to be installed.
inline bool ShouldRecord(bool provenance_on, bool recording) { return provenance_on || recording; }

// ---- the label --------------------------------------------------------------------------------------------------------------------
// What the reader knows of an image (its own flags), and what the page map knows of its base page.
struct ImageFacts {
  bool packed = false, copy_dest = false, flush_driven = false;
  bool flushed = false;           // an unlock flush named it since its contents were last read (transient)
  bool flush_seen = false;        // the game flushed it at least once (sticky)
  uint32_t flush_frame = 0;       // the guest frame of its newest unlock flush
  uint32_t flush_seq = 0;         // S101 lane17 review: the flush's event number (PageMap::Stamp)
  uint32_t first_read_seq = 0;    // S101 lane17 review: event number of the first time the reader read / bound its contents (0 = not yet)
  uint32_t copy_source = 0;       // a copy destination / packed attire: the ImageRecord id of the surface the copy read (0 = unknown)
};
struct Label {
  rdr_adapter::Provenance source = rdr_adapter::Provenance::Unnamed;
  uint8_t flags = 0;
  const char* writer = nullptr;   // a Cpu writer's name
  const char* detail = nullptr;   // census only (never in the frame): the writer with what a memcpy's source was: memcpy_src_none / memcpy_src_cpu
  uint16_t file = 0;              // FileTable id
  uint32_t source_image = 0;
  uint32_t last_write_frame = 0;
};

inline const char* WriterLabel(W w) { return wwe2k14::host_write::WriterName(w); }

inline Label Classify(const ImageFacts& f, const Entry& e) {
  using P = rdr_adapter::Provenance;
  Label l;
  l.flags = rdr_adapter::kProvDeclared;
  if (f.packed) { l.source = P::Gpu; l.writer = "attire_pack"; l.source_image = f.copy_source; return l; }  // S101 lane17: source_image filled
  if (f.copy_dest) { l.source = P::Gpu; l.source_image = f.copy_source; return l; }
  if (f.flush_driven) { l.source = P::Cpu; l.writer = "movie_decoder"; return l; }
  l.file = e.file;
  l.last_write_frame = e.frame;
  switch (e.valid ? e.writer : W::Unknown) {
    case W::FileRead: case W::Decompress:
      // S101 lane17: guest code rewrote it after the load and flushed it: CPU, the flush names the writer. Event order, not frame order (a worker
      // thread's write and VdSwap's frame count are unrelated clocks): the flush counts only if it came after the file write AND after the reader
      // had first read / bound the contents (a loader's own unlock flush precedes the first bind). [U: not yet seen in a run]
      if (f.flush_seen && f.first_read_seq && f.first_read_seq < f.flush_seq && f.flush_seq > e.seq) { l.source = P::Cpu; l.writer = "unlock_flush"; l.last_write_frame = f.flush_frame; return l; }
      l.source = P::File;
      l.flags = (e.writer == W::Decompress || e.inherited) ? rdr_adapter::kProvGuessed : rdr_adapter::kProvDeclared;
      return l;
    case W::Memcpy: case W::CrowdSkin: case W::VertexUnlock:
      l.source = P::Cpu; l.writer = WriterLabel(e.writer);
      l.detail = e.writer != W::Memcpy ? l.writer : e.src_none ? "memcpy_src_none" : e.src_cpu ? "memcpy_src_cpu" : "memcpy";
      return l;
    default:
      l.file = 0; l.last_write_frame = 0;
      if (f.flushed) { l.source = P::Cpu; l.writer = "unlock_flush"; return l; }
      l.flags = 0;
      return l;  // unnamed: written by game code no host writer reports (or before the reader started)
  }
}

// ---- hooks ------------------------------------------------------------------------------------------------------------------------
// Installs the writer hook (reader.h passes OnHostWriteBy). Called from StartLive AND StartCapture (S101 lane17: a capture without live mode).
inline void InstallWriterHook(wwe2k14::host_write::WriterFn fn) { wwe2k14::host_write::g_writer_hook.store(fn, std::memory_order_relaxed); }

}  // namespace wwe2k14::prov
