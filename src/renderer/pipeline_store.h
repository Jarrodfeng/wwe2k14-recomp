// Lane 276: the pipeline store - a list of the pipelines the renderer built (keys only) and the driver's VkPipelineCache, both kept on disk in the player's
// own cache folder, so the next session can build the pipelines a scene needs BEFORE the scene draws them (SKIP_GAME_WORK_MAP 19.10).
//
// Nothing here is game data and nothing is shipped: the list holds draw states, vertex layouts, pass formats and the 64-bit hashes of the SPIR-V words of
// OUR OWN shaders (src\renderer\shaders, compiled into the exe) - no shader code, no SPIR-V, no game bytes (hard rule 1). The cache file is the driver's
// own blob for this machine. Both are made on the player's machine and never committed or shipped.
//
// This file is pure data and file handling (no Vulkan calls): the format, the checks that refuse a file, the atomic write. renderer.cpp uses it.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "api.h"

namespace rdr::pstore {

constexpr uint32_t kListMagic = 0x4C535057;   // "WPSL"
constexpr uint32_t kCacheMagic = 0x43535057;  // "WPSC"
constexpr uint32_t kVersion = 1;         // the cache wrapper
constexpr uint32_t kListVersion = 2;     // 2: vendor / device / format-caps mask in the header (lane 276 S4)
constexpr uint32_t kMaxRecords = 4096;
constexpr uint32_t kKeepSessions = 12;         // a record unused for this many sessions is dropped at the next save
constexpr uint64_t kMaxCacheBytes = 256ull << 20;
constexpr int kAttachmentSlots = kMaxColorTargets + 1;
constexpr size_t kNameBytes = 96;

static_assert(sizeof(DrawState) == 56, "DrawState has padding: the store writes it as raw bytes");

inline uint64_t HashBytes(const void* data, size_t bytes, uint64_t seed = 0xcbf29ce484222325ull) {
  const auto* p = static_cast<const uint8_t*>(data);
  uint64_t h = seed;
  for (size_t i = 0; i < bytes; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}
// SPIR-V is hashed a word at a time (the catalog holds megabytes of it).
inline uint64_t HashWords(const uint32_t* words, size_t bytes) {
  uint64_t h = 0xcbf29ce484222325ull ^ bytes;
  const size_t count = bytes / 4;
  for (size_t i = 0; i < count; ++i) {
    h = (h ^ words[i]) * 0x9E3779B97F4A7C15ull;
    h ^= h >> 29;
  }
  return h ? h : 1;  // 0 is "no fragment"
}

// The device a cache blob belongs to.
struct DeviceId {
  uint32_t vendor = 0, device = 0, driver = 0;
  uint8_t uuid[16] = {};
  bool operator==(const DeviceId& o) const { return vendor == o.vendor && device == o.device && driver == o.driver && std::memcmp(uuid, o.uuid, 16) == 0; }
};

struct PassData {
  uint16_t formats[kAttachmentSlots] = {};
  uint8_t color_count = 0, has_depth = 0, samples = 1, pad = 0;
};

// One pipeline, with no session-local id in it: the program is named by the hashes of its SPIR-V halves, the layout and pass class by their contents.
struct Record {
  DrawState state{};        // program = vertex_layout = 0
  uint64_t vhash = 0, fhash = 0;
  VertexLayoutDesc layout{};  // canonical: attributes past attribute_count zero
  PassData pass{};
  uint32_t seen = 0;        // the session this record was last built or used in
  char name[kNameBytes] = {};  // the program's name, for logs
  // Runtime only, not stored:
  bool built = false;       // a pipeline for it exists in this session
  bool dead = false;        // the driver refused it (or it names nothing buildable): dropped at the next save
};

inline VertexLayoutDesc Canonical(const VertexLayoutDesc& in) {
  VertexLayoutDesc out{};
  out.attribute_count = in.attribute_count > uint32_t(kMaxVertexAttributes) ? uint32_t(kMaxVertexAttributes) : in.attribute_count;
  for (uint32_t i = 0; i < out.attribute_count; ++i) out.attributes[i] = in.attributes[i];
  out.strides[0] = in.strides[0];
  out.strides[1] = in.strides[1];
  out.per_instance[0] = in.per_instance[0];
  out.per_instance[1] = in.per_instance[1];
  return out;
}
inline uint64_t HashLayout(const VertexLayoutDesc& in) {
  const VertexLayoutDesc l = Canonical(in);
  uint64_t h = HashBytes(&l.attribute_count, sizeof(l.attribute_count));
  for (uint32_t i = 0; i < l.attribute_count; ++i) h = HashBytes(&l.attributes[i], sizeof(VertexAttribute), h);
  h = HashBytes(l.strides, sizeof(l.strides), h);
  return HashBytes(l.per_instance, sizeof(l.per_instance), h);
}

// ---- serialisation: explicit bytes, little endian as the host (the files never leave the machine) ----
struct Writer {
  std::vector<uint8_t> bytes;
  template <class T>
  void Put(const T& v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    bytes.insert(bytes.end(), p, p + sizeof(T));
  }
  void PutRaw(const void* d, size_t n) {
    const auto* p = static_cast<const uint8_t*>(d);
    bytes.insert(bytes.end(), p, p + n);
  }
};
struct Reader {
  const uint8_t* p;
  size_t left;
  bool ok = true;
  template <class T>
  T Get() {
    T v{};
    if (left < sizeof(T)) {
      ok = false;
      return v;
    }
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    left -= sizeof(T);
    return v;
  }
  void GetRaw(void* d, size_t n) {
    if (left < n) {
      ok = false;
      return;
    }
    std::memcpy(d, p, n);
    p += n;
    left -= n;
  }
};

constexpr size_t kRecordBytes = sizeof(DrawState) + 16 + 4 + 16 * 8 + 8 + 2 + sizeof(uint16_t) * kAttachmentSlots + 4 + 4 + kNameBytes;

inline void PutRecord(Writer& w, const Record& r) {
  w.PutRaw(&r.state, sizeof(DrawState));
  w.Put(r.vhash);
  w.Put(r.fhash);
  const VertexLayoutDesc l = Canonical(r.layout);
  w.Put(l.attribute_count);
  for (int i = 0; i < kMaxVertexAttributes; ++i) {
    w.Put(l.attributes[i].location);
    w.Put(l.attributes[i].binding);
    w.Put(l.attributes[i].format);
    w.Put(l.attributes[i].offset);
  }
  w.Put(l.strides[0]);
  w.Put(l.strides[1]);
  w.Put(l.per_instance[0]);
  w.Put(l.per_instance[1]);
  for (int i = 0; i < kAttachmentSlots; ++i) w.Put(r.pass.formats[i]);
  w.Put(r.pass.color_count);
  w.Put(r.pass.has_depth);
  w.Put(r.pass.samples);
  w.Put(r.pass.pad);
  w.Put(r.seen);
  char name[kNameBytes] = {};
  std::strncpy(name, r.name, kNameBytes - 1);
  w.PutRaw(name, kNameBytes);
}

// Range checks on every field that reaches a Vulkan call: a file that passes its checksum but was written by something else is still refused.
inline bool Sane(const Record& r) {
  const DrawState& s = r.state;
  if (r.layout.attribute_count > uint32_t(kMaxVertexAttributes)) return false;
  for (uint32_t i = 0; i < r.layout.attribute_count; ++i)
    if (r.layout.attributes[i].format > 184 || r.layout.attributes[i].binding > 1) return false;
  if (r.layout.per_instance[0] > 1 || r.layout.per_instance[1] > 1) return false;
  if (r.pass.color_count > uint8_t(kMaxColorTargets) || r.pass.has_depth > 1) return false;
  if (r.pass.samples == 0 || r.pass.samples > 64 || (r.pass.samples & (r.pass.samples - 1))) return false;
  if (r.pass.pad != 0) return false;  // the swapchain's own class is never stored
  for (int i = 0; i < kAttachmentSlots; ++i)
    if (r.pass.formats[i] > 184) return false;
  if (s.topology > 10 || s.cull > 3 || s.front_face > 1 || s.depth_compare > 7) return false;
  if (s.program != 0 || s.vertex_layout != 0) return false;
  const StencilFace faces[2] = {s.front, s.back};
  for (const StencilFace& f : faces)
    if (f.fail > 7 || f.pass > 7 || f.depth_fail > 7 || f.compare > 7) return false;
  for (int i = 0; i < kMaxColorTargets; ++i) {
    const BlendState& b = s.blend[i];
    if (b.src_color > 18 || b.dst_color > 18 || b.src_alpha > 18 || b.dst_alpha > 18 || b.color_op > 4 || b.alpha_op > 4 || b.write_mask > 15) return false;
  }
  return r.name[kNameBytes - 1] == 0;
}

inline bool GetRecord(Reader& rd, Record& r) {
  rd.GetRaw(&r.state, sizeof(DrawState));
  r.vhash = rd.Get<uint64_t>();
  r.fhash = rd.Get<uint64_t>();
  r.layout.attribute_count = rd.Get<uint32_t>();
  for (int i = 0; i < kMaxVertexAttributes; ++i) {
    r.layout.attributes[i].location = rd.Get<uint8_t>();
    r.layout.attributes[i].binding = rd.Get<uint8_t>();
    r.layout.attributes[i].format = rd.Get<uint16_t>();
    r.layout.attributes[i].offset = rd.Get<uint32_t>();
  }
  r.layout.strides[0] = rd.Get<uint32_t>();
  r.layout.strides[1] = rd.Get<uint32_t>();
  r.layout.per_instance[0] = rd.Get<uint8_t>();
  r.layout.per_instance[1] = rd.Get<uint8_t>();
  for (int i = 0; i < kAttachmentSlots; ++i) r.pass.formats[i] = rd.Get<uint16_t>();
  r.pass.color_count = rd.Get<uint8_t>();
  r.pass.has_depth = rd.Get<uint8_t>();
  r.pass.samples = rd.Get<uint8_t>();
  r.pass.pad = rd.Get<uint8_t>();
  r.seen = rd.Get<uint32_t>();
  rd.GetRaw(r.name, kNameBytes);
  return rd.ok;  // (the caller checks Sane(r): one bad record is dropped, not the whole list)
}

// The identity of a record: everything but `seen` and the name.
inline uint64_t RecordKey(const Record& r) {
  Writer w;
  Record copy = r;
  copy.seen = 0;
  std::memset(copy.name, 0, kNameBytes);
  PutRecord(w, copy);
  return HashBytes(w.bytes.data(), w.bytes.size());
}

// ---- files ----
inline std::filesystem::path PathOf(const std::string& utf8) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}
inline bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& out, uint64_t max_bytes) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size > max_bytes) return false;
  std::FILE* f = nullptr;
#if defined(_WIN32)
  f = _wfopen(path.c_str(), L"rb");
#else
  f = std::fopen(path.string().c_str(), "rb");
#endif
  if (!f) return false;
  out.resize(size_t(size));
  const bool ok = out.empty() || std::fread(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}
inline unsigned ProcessId() {
#if defined(_WIN32)
  return unsigned(_getpid());
#else
  return unsigned(getpid());
#endif
}
// Temp file + rename: a crash while writing leaves the old file, never half a new one.
inline bool WriteFileAtomic(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::filesystem::path tmp = path;
  tmp += ".tmp" + std::to_string(unsigned(ProcessId()));  // (two processes sharing the folder never write the same temp file)
  std::FILE* f = nullptr;
#if defined(_WIN32)
  f = _wfopen(tmp.c_str(), L"wb");
#else
  f = std::fopen(tmp.string().c_str(), "wb");
#endif
  if (!f) return false;
  const bool ok = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  const bool closed = std::fclose(f) == 0;
  if (!ok || !closed) {
    std::filesystem::remove(tmp, ec);
    return false;
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    return false;
  }
  return true;
}

// ---- the list ----
// header: magic, version, session, count, payload bytes, the device it was made on (a list from another GPU or with other format-caps limits builds
// other pipelines), checksum of the payload. Payload: count x kRecordBytes.
struct ListHeader {
  uint32_t magic, version, session, count, payload_bytes;
  uint32_t vendor, device, caps_mask;
  uint64_t checksum;
};
static_assert(sizeof(ListHeader) == 40, "");

inline std::vector<uint8_t> SerialiseList(const std::vector<Record>& records, uint32_t session, const DeviceId& id, uint32_t caps_mask) {
  Writer payload;
  uint32_t count = 0;
  for (const Record& r : records) {
    if (r.dead) continue;
    PutRecord(payload, r);
    ++count;
  }
  ListHeader h{kListMagic, kListVersion, session, count, uint32_t(payload.bytes.size()), id.vendor, id.device, caps_mask, HashBytes(payload.bytes.data(), payload.bytes.size())};
  std::vector<uint8_t> out(sizeof(h));
  std::memcpy(out.data(), &h, sizeof(h));
  out.insert(out.end(), payload.bytes.begin(), payload.bytes.end());
  return out;
}

// Returns an empty string on success, else why the list is refused. A record that fails its range checks (after a good checksum) is dropped alone: `dropped`.
// `skip_checks` is the lane's test plant (the checksum is not checked).
inline std::string ParseList(const std::vector<uint8_t>& file, std::vector<Record>& out, uint32_t& session, const DeviceId& id, uint32_t caps_mask, uint32_t& dropped, bool skip_checks) {
  out.clear();
  dropped = 0;
  if (file.size() < sizeof(ListHeader)) return "shorter than its header";
  ListHeader h;
  std::memcpy(&h, file.data(), sizeof(h));
  if (h.magic != kListMagic) return "not a pipeline list (magic)";
  if (h.version != kListVersion) return "list version " + std::to_string(h.version) + " (this build reads " + std::to_string(kListVersion) + ")";
  if (h.count > kMaxRecords) return "more than " + std::to_string(kMaxRecords) + " records";
  if (uint64_t(h.count) * kRecordBytes != h.payload_bytes || file.size() != sizeof(ListHeader) + h.payload_bytes) return "size does not match its record count";
  const uint8_t* payload = file.data() + sizeof(ListHeader);
  if (!skip_checks && HashBytes(payload, h.payload_bytes) != h.checksum) return "checksum mismatch (corrupt)";
  if (h.vendor != id.vendor || h.device != id.device) return "made on another GPU (vendor/device differ)";
  if (h.caps_mask != caps_mask) return "made with other format-caps limits";
  Reader rd{payload, h.payload_bytes};
  out.reserve(h.count);
  for (uint32_t i = 0; i < h.count; ++i) {
    Record r;
    if (!GetRecord(rd, r)) {
      out.clear();
      return "records unreadable";
    }
    if (Sane(r)) out.push_back(r);
    else ++dropped;
  }
  session = h.session;
  return {};
}

// ---- the driver's cache ----
struct CacheHeader {
  uint32_t magic, version;
  uint32_t vendor, device, driver, pad;
  uint8_t uuid[16];
  uint64_t size;
  uint64_t checksum;
};
static_assert(sizeof(CacheHeader) == 56, "");

inline std::vector<uint8_t> SerialiseCache(const DeviceId& id, const uint8_t* blob, size_t bytes) {
  CacheHeader h{};
  h.magic = kCacheMagic;
  h.version = kVersion;
  h.vendor = id.vendor, h.device = id.device, h.driver = id.driver;
  std::memcpy(h.uuid, id.uuid, 16);
  h.size = bytes;
  h.checksum = HashBytes(blob, bytes);
  std::vector<uint8_t> out(sizeof(h) + bytes);
  std::memcpy(out.data(), &h, sizeof(h));
  if (bytes) std::memcpy(out.data() + sizeof(h), blob, bytes);
  return out;
}

// Empty string = accepted (blob = the driver data inside); else why it is refused.
inline std::string ParseCache(const std::vector<uint8_t>& file, const DeviceId& id, const uint8_t*& blob, size_t& bytes, bool skip_checks) {
  blob = nullptr;
  bytes = 0;
  if (file.size() < sizeof(CacheHeader)) return "shorter than its header";
  CacheHeader h;
  std::memcpy(&h, file.data(), sizeof(h));
  if (h.magic != kCacheMagic) return "not a pipeline cache (magic)";
  if (h.version != kVersion) return "cache version " + std::to_string(h.version);
  if (h.size != file.size() - sizeof(h) || h.size > kMaxCacheBytes) return "size does not match";
  if (!skip_checks) {
    DeviceId stored;
    stored.vendor = h.vendor, stored.device = h.device, stored.driver = h.driver;
    std::memcpy(stored.uuid, h.uuid, 16);
    if (!(stored == id)) return "made by another device or driver (vendor/device/driver/uuid differ)";
    if (HashBytes(file.data() + sizeof(h), size_t(h.size)) != h.checksum) return "checksum mismatch (corrupt)";
    // The driver's own header (VkPipelineCacheHeaderVersionOne): length >= 32, version 1, vendor, device, uuid.
    if (h.size < 32) return "driver blob shorter than its header";
    uint32_t inner[4];
    std::memcpy(inner, file.data() + sizeof(h), 16);
    if (inner[0] < 32 || inner[1] != 1 || inner[2] != id.vendor || inner[3] != id.device) return "driver blob header names another device";
    if (std::memcmp(file.data() + sizeof(h) + 16, id.uuid, 16) != 0) return "driver blob uuid differs";
  }
  blob = file.data() + sizeof(h);
  bytes = size_t(h.size);
  return {};
}

}  // namespace rdr::pstore
