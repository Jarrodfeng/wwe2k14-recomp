// Loads a capture written by the adapter (docs\renderer\CAPTURE_FORMAT.md), for the player tool. Header-only, no game or SDK includes.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "adapter_frame.h"
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
  Table<ConstantBank> constant_banks;
  Table<BoolLoop> bool_loops;
  Table<ContentRef> contents;
  Table<CopyExtra> copy_extras;  // parallel to copies
  Table<BankDef> bank_defs;
  Table<BankPatch> bank_patches;
  std::vector<ConstantBank> expanded_banks;  // the defs as full banks; constant_banks then points here
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
  static bool ParseFrame(LoadedFrame& out) {
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
        case kSecConstantBanks: Bind(out, e, out.constant_banks); break;
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
    // Constant banks sent as changes: expanded here, so everything after sees full banks indexed as the draws index them.
    if (out.bank_defs.count) {
      out.expanded_banks.resize(out.bank_defs.count);
      for (uint32_t i = 0; i < out.bank_defs.count; ++i) {
        const BankDef& d = out.bank_defs[i];
        ConstantBank& b = out.expanded_banks[i];
        if (d.base == kBankNoBase) {
          if (d.full < out.constant_banks.count) b = out.constant_banks[d.full];
        } else if (d.base < i) {
          b = out.expanded_banks[d.base];
        }
        for (uint32_t p = 0; p < d.patch_count && d.patch_first + p < out.bank_patches.count; ++p) {
          const BankPatch& patch = out.bank_patches[d.patch_first + p];
          if (patch.block < 64) std::memcpy(b.dwords + 16 * patch.block, patch.dwords, sizeof(patch.dwords));
        }
      }
      out.constant_banks.data = out.expanded_banks.data();
      out.constant_banks.count = uint32_t(out.expanded_banks.size());
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
