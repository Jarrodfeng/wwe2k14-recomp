// Loads a capture written by the adapter (docs\renderer\CAPTURE_FORMAT.md), for the player tool. Header-only, no game or SDK includes.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "adapter_frame.h"
#include "bank_switch.h"  // S102 T1
#include "names.h"  // S101 names: the frame's additive name sections (docs\renderer\NAMES.md)

namespace rdr_adapter {

template <class T>
struct Table {
  const T* data = nullptr;
  uint32_t count = 0;
  const T& operator[](uint32_t i) const { return data[i]; }
  const T* begin() const { return data; }
  const T* end() const { return data + count; }
};

// S102 T1: constant banks resolved on use. A frame carries bank defs (a full bank or a base plus patches); the old reader expanded every def
// into a fresh 4 KB bank at parse time (up to ~18 MB a frame, dropped draws included). BankView keeps the defs and resolves def i only when
// something reads constant_banks[i], into pooled memory that is reused from frame to frame (the pool only ever grows). The default (switch
// wwe2k14_rdr_lazy_banks off) expands everything at parse time exactly as before, fresh memory every frame. Not thread-safe: a frame is read by
// one thread (the binder), as before.

struct BankPool {
  std::vector<ConstantBank> banks;
  std::vector<uint8_t> done;
  std::vector<uint32_t> chain;
  std::vector<uint32_t> maps;   // S102 T1b: 64 entries (256 B) a def, the block map (BankView)
};

static_assert(sizeof(ConstantBank) == 4096, "BankView's block maps address a full-bank array as contiguous 64-byte blocks");

class BankView;
// S102 T1b: a bank as the packers read it: a plain ConstantBank (every old caller, converts implicitly) or def `index` of a BankView, whose block map lets the
// packers copy just the register ranges they need, block by block, without the bank ever being assembled.
struct BankRef {
  const ConstantBank* bank = nullptr;
  const BankView* view = nullptr;
  uint32_t index = 0;
  BankRef(const ConstantBank& b) : bank(&b) {}   // NOLINT: implicit on purpose
  BankRef(const BankView& v, uint32_t i) : view(&v), index(i) {}
  inline void CopyBytes(uint32_t off, uint32_t n, uint8_t* dst) const;   // bytes [off, off + n) of the bank, clamped to its 4096 bytes (defined after BankView)
};

class BankView {
 public:
  uint32_t count = 0;
  BankView() = default;
  BankView(const ConstantBank* data, uint32_t n) : count(n), direct_(data) {}   // plain pointer + count (tests, mini frames): no defs
  ~BankView() { Flush(); }
  // A read past the table gives a zero bank (a doctored or damaged draw), never a stray read.
  const ConstantBank& operator[](uint32_t i) const {
    if (i >= count) return ZeroBank();
    if (direct_) return direct_[i];
    BankPool& p = Pool();
    if (!p.done[i]) Resolve(p, i);
    return p.banks[i];
  }
  static const ConstantBank& ZeroBank() {
    static const ConstantBank zero{};
    return zero;
  }
  // Pooled memory shared with other frames (the live path hands one pool to every frame it parses); null = this view's own.
  // One pool serves ONE live frame at a time (the next SetDefs resets it).
  void UsePool(BankPool* pool) { pool_ = pool; }
  void SetDirect(const ConstantBank* data, uint32_t n) { direct_ = data; count = n; defs_ = nullptr; }
  // Defs given: eager = expand every def now (the old behaviour: fresh memory, zeroed then copied), else resolve on use into the pool.
  // blocks (S102 T1b, wwe2k14_rdr_bank_blocks): implies lazy, and builds the 64-entry block map of every def now (one forward pass), so Block / CopyBytes / Ref read
  // a bank without assembling it; operator[] still assembles into the pool on first read (whole-bank readers).
  void SetDefs(Table<ConstantBank> fulls, Table<BankDef> defs, Table<BankPatch> patches, bool lazy, bool blocks = false) {
    Flush();
    if (blocks) lazy = true;
    blocks_on_ = blocks;
    fulls_ = fulls, defs_ = defs.data, patches_ = patches;
    direct_ = nullptr;
    count = defs.count;
    lazy_build_ = lazy;
    if (!lazy) own_ = BankPool{};
    BankPool& p = lazy ? Pool() : own_;
    if (p.banks.size() < count) {   // growth only: a reused pool is not zeroed again
      acc_.bytes += uint64_t(count - p.banks.size()) * sizeof(ConstantBank);
      p.banks.resize(count);
    }
    if (g_bank_plant.load(std::memory_order_relaxed) == 2 && p.done.size() >= count) {
      // planted fault (gate twin): the done flags of the pool's previous frame are kept
    } else {
      p.done.assign(count, 0);
    }
    if (!lazy)
      for (uint32_t i = 0; i < count; ++i) Resolve(p, i);
    if (blocks) BuildMaps(p);
    direct_ = lazy ? nullptr : p.banks.data();   // eager: a plain pointer into the frame's own banks, exactly the old expanded_banks
    if (!lazy) Flush();
    g_bank_frames.fetch_add(1, std::memory_order_relaxed);
  }
  // S102 T1b: the 64 bytes (16 dwords) of block `b` of bank `i`; a bank past the table, or a block past 63, is zeros. Without block maps this reads through operator[].
  const uint32_t* Block(uint32_t i, uint32_t b) const {
    if (i >= count || b >= 64) return ZeroBlock();
    if (direct_) return direct_[i].dwords + 16 * b;
    if (!blocks_on_) return (*this)[i].dwords + 16 * b;
    const uint32_t e = Pool().maps[size_t(i) * 64 + b];
    if (e == kBlockZero) return ZeroBlock();
    if (e & kBlockPatch) return patches_[e & ~kBlockPatch].dwords;
    return fulls_.data->dwords + 16 * size_t(e);   // full banks are one array: entry = full * 64 + block
  }
  static const uint32_t* ZeroBlock() {
    static const uint32_t zero[16] = {};
    return zero;
  }
  // Bytes [off, off + n) of bank `i` into dst (clamped to the bank's 4096 bytes: the rest of dst is left alone), block by block when block maps are on.
  void CopyBytes(uint32_t i, uint32_t off, uint32_t n, uint8_t* dst) const {
    if (off >= 4096) return;
    if (n > 4096 - off) n = 4096 - off;
    if (!blocks_on_ || direct_ || i >= count) {
      std::memcpy(dst, reinterpret_cast<const uint8_t*>((*this)[i].dwords) + off, n);
      return;
    }
    while (n) {
      const uint32_t within = off & 63, take = std::min<uint32_t>(n, 64 - within);
      std::memcpy(dst, reinterpret_cast<const uint8_t*>(Block(i, off >> 6)) + within, take);
      dst += take, off += take, n -= take;
    }
  }
  BankRef Ref(uint32_t i) const { return blocks_on_ && !direct_ ? BankRef(*this, i) : BankRef((*this)[i]); }
  // Equal block maps mean equal banks (a quick yes for a memcmp of two banks); different maps say nothing (bytes may still match).
  bool SameMap(uint32_t a, uint32_t b) const {
    if (a == b) return true;
    if (!blocks_on_ || direct_ || a >= count || b >= count) return false;
    const BankPool& p = Pool();
    return std::memcmp(&p.maps[size_t(a) * 64], &p.maps[size_t(b) * 64], g_bank_plant.load(std::memory_order_relaxed) == 6 ? 252 : 256) == 0;   // plant 6 (gate twin): the last entry is not compared
  }
  bool blocks_mode() const { return blocks_on_ && !direct_; }
  bool lazy_mode() const { return direct_ == nullptr && defs_ != nullptr; }
  // A complete copy, for the few callers that want the whole table as a plain array.
  std::vector<ConstantBank> ToVector() const {
    std::vector<ConstantBank> v;
    v.reserve(count);
    for (uint32_t i = 0; i < count; ++i) v.push_back((*this)[i]);
    return v;
  }
  // The frame's counters go to the globals once (here, at the next SetDefs and at destruction), not once a bank.
  void Flush() const {
    if (acc_.bytes) g_bank_bytes.fetch_add(acc_.bytes, std::memory_order_relaxed);
    if (acc_.resolved) g_bank_resolved.fetch_add(acc_.resolved, std::memory_order_relaxed);
    if (acc_.map_frames) g_bank_map_frames.fetch_add(acc_.map_frames, std::memory_order_relaxed);
    acc_.bytes = acc_.resolved = acc_.map_frames = 0;
  }

 private:
  BankPool& Pool() const { return pool_ ? *pool_ : own_; }
  static constexpr uint32_t kBlockZero = 0xFFFFFFFFu, kBlockPatch = 0x80000000u;
  // One forward pass: def i's map = its base's map (or its full bank's blocks) with its patches' blocks replaced. 256 bytes a def instead of 4 KB; no bank byte moves.
  void BuildMaps(BankPool& p) const {
    // planted twins (gate): 4 = patches skipped, 5 = a def's base off by one
    const int plant = g_bank_plant.load(std::memory_order_relaxed);
    if (p.maps.size() < size_t(count) * 64) p.maps.resize(size_t(count) * 64);
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t* m = &p.maps[size_t(i) * 64];
      const BankDef& d = defs_[i];
      if (d.base == kBankNoBase) {
        for (uint32_t b = 0; b < 64; ++b) m[b] = d.full < fulls_.count ? d.full * 64 + b : kBlockZero;
      } else if (d.base < i) {
        std::memcpy(m, &p.maps[size_t(plant == 5 && d.base + 1 < i ? d.base + 1 : d.base) * 64], 256);
      } else {
        for (uint32_t b = 0; b < 64; ++b) m[b] = kBlockZero;
      }
      if (plant != 4)
        for (uint32_t q = 0; q < d.patch_count && d.patch_first + q < patches_.count; ++q)
          if (patches_[d.patch_first + q].block < 64) m[patches_[d.patch_first + q].block] = kBlockPatch | (d.patch_first + q);
    }
    acc_.bytes += uint64_t(count) * 256;
    ++acc_.map_frames;
  }
  void Resolve(BankPool& p, uint32_t i) const {
    // Walk down the base chain to the first resolved def (or a def with a full bank / no usable base), then build forward.
    const int plant = lazy_build_ ? g_bank_plant.load(std::memory_order_relaxed) : 0;   // 1 = patches skipped, 3 = base off by one (gate twins; the lazy build only, so the eager arm stays the reference)
    p.chain.clear();
    uint32_t at = i;
    while (!p.done[at]) {
      p.chain.push_back(at);
      const BankDef& d = defs_[at];
      if (d.base == kBankNoBase || d.base >= at) break;
      at = d.base;
    }
    uint64_t bytes = 0;
    for (size_t k = p.chain.size(); k-- > 0;) {
      const uint32_t j = p.chain[k];
      const BankDef& d = defs_[j];
      ConstantBank& b = p.banks[j];
      if (d.base == kBankNoBase) {
        if (d.full < fulls_.count) b = fulls_[d.full]; else b = ConstantBank{};
      } else if (d.base < j) {
        b = p.banks[plant == 3 && d.base + 1 < j ? d.base + 1 : d.base];
      } else {
        b = ConstantBank{};
      }
      bytes += sizeof(ConstantBank);
      if (plant != 1) {
        for (uint32_t q = 0; q < d.patch_count && d.patch_first + q < patches_.count; ++q) {
          const BankPatch& patch = patches_[d.patch_first + q];
          if (patch.block < 64) { std::memcpy(b.dwords + 16 * patch.block, patch.dwords, sizeof(patch.dwords)); bytes += sizeof(patch.dwords); }
        }
      }
      p.done[j] = 1;
    }
    acc_.bytes += bytes;
    acc_.resolved += p.chain.size();
  }
  // Counters kept per frame (a copy of the view starts at zero, so a copied frame never counts twice).
  struct Acc {
    uint64_t bytes = 0, resolved = 0, map_frames = 0;
    Acc() = default;
    Acc(const Acc&) {}
    Acc& operator=(const Acc&) { return *this; }
  };
  const ConstantBank* direct_ = nullptr;
  const BankDef* defs_ = nullptr;
  Table<ConstantBank> fulls_;
  Table<BankPatch> patches_;
  BankPool* pool_ = nullptr;
  bool lazy_build_ = false;
  bool blocks_on_ = false;   // S102 T1b
  mutable BankPool own_;
  mutable Acc acc_;
};

inline void BankRef::CopyBytes(uint32_t off, uint32_t n, uint8_t* dst) const {
  if (view) { view->CopyBytes(index, off, n, dst); return; }
  if (off >= 4096) return;
  if (n > 4096 - off) n = 4096 - off;
  std::memcpy(dst, reinterpret_cast<const uint8_t*>(bank->dwords) + off, n);
}

// One loaded frame; its tables point into `bytes`.
struct LoadedFrame {
  std::vector<uint8_t> bytes;
  FrameFileHeader header;
  Table<Op> ops;
  Table<rdr::TargetsOp> targets;
  Table<rdr::ClearOp> clears;
  Table<Draw> draws;
  Table<rdr::CopyOp> copies;
  Table<rdr::PresentOp> presents;
  Table<DrawFixed> fixeds;
  Table<DrawDynamic> dynamics;
  Table<rdr::SamplerDesc> samplers;
  Table<TextureSlot> texture_slots;
  Table<VertexLayout> layouts;
  Table<VertexElement> elements;
  Table<ConstantBank> full_banks;   // S102 T1: the kSecConstantBanks section as stored; constant_banks (the view) is what draws index
  BankView constant_banks;
  Table<BoolLoop> bool_loops;
  Table<ContentRef> contents;
  Table<CopyExtra> copy_extras;  // parallel to copies
  Table<BankDef> bank_defs;
  Table<BankPatch> bank_patches;
  // S101 names (absent in older frames: count 0 = every draw unnamed)
  Table<DrawOrigin> origins;            // parallel to draws
  Table<SurfaceRoleEntry> surface_roles;
  Table<TextureProvenance> provenance;
  Table<char> strings;
  Table<FrameWhere> where;
  Table<TextRun> text_runs;             // S101 U1: the whole string of a glyph draw (kSecTextRuns); count 0 in older frames
  Table<ThinDrawFacts> thin_facts;      // S101 lane 122 (review 6): kSecThinFacts, parallel to draws or empty
  Table<D3FrameCounts> d3;              // lane 138: kSecD3 (the frame's marker / dropped counts); count 0 when the switch was off
  Table<ThinFrameStamp> thin;           // S101 lane 122: the pin-set generation the reader used (kSecThin); count 0 in older frames and in frames with no thin draws
  const char* String(uint32_t name) const {  // a name field (offset + 1) -> text; "" for none
    return name && name <= strings.count ? strings.data + (name - 1) : "";
  }
  const DrawOrigin* OriginOf(uint32_t draw) const { return draw < origins.count ? &origins[draw] : nullptr; }
};

struct LoadedTexture {
  std::vector<uint8_t> bytes;
  TextureBlobHeader header;
  const Subresource* subresources = nullptr;
  const uint8_t* data = nullptr;
};

class CaptureReader {
 public:
  ~CaptureReader() { Close(); }

  bool Open(const std::filesystem::path& folder) {
    Close();
    folder_ = folder;
    if (!LoadImages(folder / "images.bin")) return false;
    frame_count_ = 0;
    while (std::filesystem::exists(FramePath(frame_count_))) ++frame_count_;
    geometry_ = OpenFile(folder / "geometry.bin");
    textures_ = OpenFile(folder / "textures.bin");
    return frame_count_ != 0;
  }
  void Close() {
    if (geometry_) std::fclose(geometry_);
    if (textures_) std::fclose(textures_);
    geometry_ = textures_ = nullptr;
    images_.clear();
    frame_count_ = 0;
  }

  uint32_t frame_count() const { return frame_count_; }
  // S101 lane 26: the live scene writer's records beside the frames (scene\frame_NNNNNN.rsc, same number as frames\frame_NNNNNN.bin). A capture
  // without them (older, or taken without --wwe2k14_scene_records) has none. The files are read with rdr_scene::ReadRecord (src\renderer_scene).
  uint32_t scene_record_count() const {
    if (scene_count_ == ~0u) {
      scene_count_ = 0;
      std::error_code ec;
      for (const auto& e : std::filesystem::directory_iterator(folder_ / "scene", ec))
        if (e.path().extension() == ".rsc") ++scene_count_;
    }
    return scene_count_;
  }
  std::filesystem::path SceneRecordPath(uint32_t index) const {
    char name[32];
    std::snprintf(name, sizeof(name), "frame_%06u.rsc", index);
    return folder_ / "scene" / name;
  }
  bool HasSceneRecord(uint32_t index) const {
    std::error_code ec;
    return std::filesystem::exists(SceneRecordPath(index), ec);
  }
  const std::vector<ImageRecord>& images() const { return images_; }
  const ImageRecord* FindImage(uint32_t id) const {
    if (id == 0 || id > images_.size()) return nullptr;
    const ImageRecord& r = images_[id - 1];
    return r.id == id ? &r : nullptr;
  }

  bool LoadFrame(uint32_t index, LoadedFrame& out) const {
    if (!ReadWhole(FramePath(index), out.bytes)) return false;
    out.thin = {}, out.thin_facts = {};   // lane 149: ParseFrame binds these only when the section exists; a reused LoadedFrame (replay loops, the GUI) must not keep the previous frame's
    return ParseFrame(out);
  }

  // A frame whose bytes are already in out.bytes (a file, or the live path's hand-over: the same format).
  // S102 K2 round 3 (F3): only the copy table of a serialised frame (the run-end keep peeks the NEXT queued frame): the section list is scanned and the copies copied out; nothing else is parsed, nothing big is copied.
  static bool ReadCopies(const std::vector<uint8_t>& bytes, std::vector<rdr::CopyOp>& out) {
    out.clear();
    if (bytes.size() < sizeof(FrameFileHeader)) return false;
    FrameFileHeader h;
    std::memcpy(&h, bytes.data(), sizeof h);
    if (h.magic != kFrameMagic || h.version != kFormatVersion) return false;
    const auto* sections = reinterpret_cast<const SectionEntry*>(bytes.data() + sizeof(FrameFileHeader));
    for (uint32_t s = 0; s < h.section_count; ++s) {
      const SectionEntry& e = sections[s];
      if (SectionId(e.id) != kSecCopies) continue;
      if (e.element_size != sizeof(rdr::CopyOp) || e.offset + uint64_t(e.count) * sizeof(rdr::CopyOp) > bytes.size()) return false;
      out.resize(e.count);
      if (e.count) std::memcpy(out.data(), bytes.data() + e.offset, size_t(e.count) * sizeof(rdr::CopyOp));
      return true;
    }
    return true;   // no copy section: no copies
  }
  static bool ParseFrame(LoadedFrame& out, int lazy = -1, int blocks = -1) {   // lazy / blocks (S102 T1b): -1 = the global switch (rplay, fview), 0 / 1 = the caller read it this frame (live)
    if (out.bytes.size() < sizeof(FrameFileHeader)) return false;
    std::memcpy(&out.header, out.bytes.data(), sizeof(FrameFileHeader));
    if (out.header.magic != kFrameMagic || out.header.version != kFormatVersion) return false;
    const auto* sections = reinterpret_cast<const SectionEntry*>(out.bytes.data() + sizeof(FrameFileHeader));
    for (uint32_t s = 0; s < out.header.section_count; ++s) {
      const SectionEntry& e = sections[s];
      switch (SectionId(e.id)) {
        case kSecOps: Bind(out, e, out.ops); break;
        case kSecTargets: Bind(out, e, out.targets); break;
        case kSecClears: Bind(out, e, out.clears); break;
        case kSecDraws: Bind(out, e, out.draws); break;
        case kSecCopies: Bind(out, e, out.copies); break;
        case kSecPresents: Bind(out, e, out.presents); break;
        case kSecFixeds: Bind(out, e, out.fixeds); break;
        case kSecDynamics: Bind(out, e, out.dynamics); break;
        case kSecSamplers: Bind(out, e, out.samplers); break;
        case kSecTextureSlots: Bind(out, e, out.texture_slots); break;
        case kSecLayouts: Bind(out, e, out.layouts); break;
        case kSecElements: Bind(out, e, out.elements); break;
        case kSecConstantBanks: Bind(out, e, out.full_banks); break;
        case kSecBoolLoops: Bind(out, e, out.bool_loops); break;
        case kSecContents: Bind(out, e, out.contents); break;
        case kSecCopyExtras: Bind(out, e, out.copy_extras); break;
        case kSecBankDefs: Bind(out, e, out.bank_defs); break;
        case kSecBankPatches: Bind(out, e, out.bank_patches); break;
        default:
          switch (NameSectionId(e.id)) {  // S101 names
            case kSecDrawOrigins: Bind(out, e, out.origins); break;
            case kSecSurfaceRoles: Bind(out, e, out.surface_roles); break;
            case kSecProvenance: Bind(out, e, out.provenance); break;
            case kSecStrings: Bind(out, e, out.strings); break;
            case kSecWhere: Bind(out, e, out.where); break;
            case kSecTextRuns: Bind(out, e, out.text_runs); break;
            case kSecThin: Bind(out, e, out.thin); break;  // S101 lane 122
            case kSecThinFacts: Bind(out, e, out.thin_facts); break;
            case kSecD3: Bind(out, e, out.d3); break;  // lane 138
            default: break;  // a newer section this reader does not know
          }
          break;
      }
    }
    // Constant banks sent as changes (S102 T1: resolved on use when wwe2k14_rdr_lazy_banks is on, else expanded here as before).
    if (out.bank_defs.count) {
      out.constant_banks.SetDefs(out.full_banks, out.bank_defs, out.bank_patches, lazy < 0 ? g_lazy_banks.load(std::memory_order_relaxed) : lazy != 0,
                                  blocks < 0 ? g_bank_blocks.load(std::memory_order_relaxed) : blocks != 0);   // S102 T1b
    } else {
      out.constant_banks.SetDirect(out.full_banks.data, out.full_banks.count);
    }
    return true;
  }

  // Reads a draw's bytes from geometry.bin.
  bool ReadGeometry(uint64_t offset, uint32_t bytes, std::vector<uint8_t>& out) const {
    out.resize(bytes);
    return ReadAt(geometry_, offset, out.data(), bytes);
  }

  bool ReadTexture(const ContentRef& ref, LoadedTexture& out) const {
    out.bytes.resize(size_t(ref.size));
    if (!ReadAt(textures_, ref.offset, out.bytes.data(), size_t(ref.size))) return false;
    return ParseTexture(out);
  }

  // A texture blob whose bytes are already in out.bytes.
  static bool ParseTexture(LoadedTexture& out) {
    if (out.bytes.size() < sizeof(TextureBlobHeader)) return false;
    std::memcpy(&out.header, out.bytes.data(), sizeof(TextureBlobHeader));
    if (out.header.magic != kTextureMagic) return false;
    out.subresources = reinterpret_cast<const Subresource*>(out.bytes.data() + sizeof(TextureBlobHeader));
    out.data = out.bytes.data() + sizeof(TextureBlobHeader) + out.header.subresource_count * sizeof(Subresource);
    return true;
  }

 private:
  template <class T>
  static void Bind(const LoadedFrame& f, const SectionEntry& e, Table<T>& t) {
    if (e.element_size != sizeof(T)) return;  // written by another layout: left empty, the caller sees count 0
    t.data = reinterpret_cast<const T*>(f.bytes.data() + e.offset);
    t.count = uint32_t(e.count);
  }
  std::filesystem::path FramePath(uint32_t index) const {
    char name[32];
    std::snprintf(name, sizeof(name), "frame_%06u.bin", index);
    return folder_ / "frames" / name;
  }
  static std::FILE* OpenFile(const std::filesystem::path& p) {
#if defined(_WIN32)
    return _wfopen(p.c_str(), L"rb");
#else
    return std::fopen(p.c_str(), "rb");
#endif
  }
  static bool ReadWhole(const std::filesystem::path& p, std::vector<uint8_t>& out) {
    std::FILE* f = OpenFile(p);
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(size_t(n));
    const bool ok = n == 0 || std::fread(out.data(), 1, size_t(n), f) == size_t(n);
    std::fclose(f);
    return ok;
  }
  static bool ReadAt(std::FILE* f, uint64_t offset, void* out, size_t bytes) {
    if (!f) return false;
#if defined(_WIN32)
    _fseeki64(f, int64_t(offset), SEEK_SET);
#else
    fseeko(f, off_t(offset), SEEK_SET);
#endif
    return std::fread(out, 1, bytes, f) == bytes;
  }
  bool LoadImages(const std::filesystem::path& p) {
    std::vector<uint8_t> raw;
    if (!ReadWhole(p, raw) || raw.size() < sizeof(ImageTableHeader)) return false;
    ImageTableHeader h;
    std::memcpy(&h, raw.data(), sizeof(h));
    if (h.magic != kImagesMagic || h.version != kFormatVersion) return false;
    images_.resize(h.count);
    if (h.count) std::memcpy(images_.data(), raw.data() + sizeof(h), h.count * sizeof(ImageRecord));
    return true;
  }

  std::filesystem::path folder_;
  std::vector<ImageRecord> images_;
  uint32_t frame_count_ = 0;
  mutable uint32_t scene_count_ = ~0u;  // S101 lane 26: counted on first ask
  std::FILE* geometry_ = nullptr;
  std::FILE* textures_ = nullptr;
};

}  // namespace rdr_adapter
