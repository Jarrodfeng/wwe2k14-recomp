// The lists our render thread keeps across frames (Session 103, lane G; docs\renderer\SCENE_PACING.md 3b).
//
// FrontEnd::Record used to rebuild every draw of every frame from scratch, its two constant blocks packed by resolving every field BY NAME (a chain of string
// compares per field). Almost all of that is the same from one frame to the next. Here a draw is kept per (object id, asset, mesh, instance, pass): its two
// constant blocks with their field lists. Each frame the draw's six STAMPS (hashes of what the
// record can change: world, palette, camera, time, light set, material + tint) are compared with the last frame's, and only the fields whose class
// intersects the changed stamps are re-resolved. The first frame of a draw builds everything; an object that left the record frees its draws.
//
// Header + one small cpp; includes the renderer's api.h only (no renderer, no assets): the unit harness (tools\scene\pacing_unit.cpp) drives it alone.
// A FrontEnd with no DrawLists attached (the pointer is null) draws exactly as before.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace rdr_scene {

// What a constant field can depend on. A field is rewritten when (changed stamps & its mask) != 0; mask 0 = static (written when the block is built).
enum StampBit : uint32_t {
  kStWorld = 1u << 0,     // the object's / instance's world matrix
  kStPalette = 1u << 1,   // the skin palette (bones)
  kStCamera = 1u << 2,    // view-projection and eye
  kStTime = 1u << 3,      // frame time
  kStLight = 1u << 4,     // the light set the draw uses
  kStMaterial = 1u << 5,  // the material's values + the object's tint (alpha)
  kStAll = (1u << 6) - 1,
};
constexpr int kStampCount = 6;
struct Stamps {
  uint64_t h[kStampCount] = {0, 0, 0, 0, 0, 0};
};

// The stamps a constant field named `name` depends on (draw_lists.cpp). A name the table does not know is kStAll: rewritten whenever anything changed
// (never stale). Static fields (constants of the effect) are 0.
uint32_t FieldMask(const char* name);
// Test only (the offline checks' planted faults): FieldMask(name) answers `mask` from now on. Call before the first frame.
void PlantFieldMask(const char* name, uint32_t mask);

inline uint64_t HashBytes(const void* p, size_t n, uint64_t seed = 0xcbf29ce484222325ull) {  // FNV-1a, 8 bytes at a time then the tail
  const uint8_t* b = static_cast<const uint8_t*>(p);
  uint64_t h = seed;
  while (n >= 8) {
    uint64_t v;
    std::memcpy(&v, b, 8);
    h = (h ^ v) * 0x100000001b3ull;
    h ^= h >> 29;
    b += 8, n -= 8;
  }
  while (n--) h = (h ^ *b++) * 0x100000001b3ull;
  return h;
}

struct KeptField {
  const char* name = nullptr;  // outlives the list (the effect table's literal)
  uint32_t offset = 0;         // bytes into the block
  uint32_t vec4s = 0;
  uint32_t mask = kStAll;      // FieldMask(name)
};

// One constant block (vertex or fragment) of a kept draw.
struct KeptBlock {
  std::vector<uint8_t> bytes;
  std::vector<KeptField> fields;
  bool valid = false;  // fields listed and every field resolved once
  void Begin(uint32_t size) { bytes.assign(size, 0), fields.clear(), valid = false; }
  void Add(const char* name, uint32_t offset, uint32_t vec4s) { fields.push_back({name, offset, vec4s, FieldMask(name)}); }
};

struct DrawKey {
  uint32_t object_id = 0, asset = 0, mesh = 0;
  uint16_t instance = 0;
  uint8_t pass = 0;
};

struct KeptDraw {
  uint64_t last_frame = 0, record_seq = 0;  // record_seq: the Record call that last took this draw (a second take in the same Record = a duplicate key)
  Stamps stamps;
  // What the kept blocks were built for (the front-end's binding of the material: program, layout, variants). A different key = a different draw: rebuilt whole.
  const void* bind_key = nullptr;
  KeptBlock vs, fs;  // the two constant blocks and their field lists. The DrawState / texture set are NOT kept: building them costs less than checking them (SCENE_PACING.md 3b)
};

struct ListCounts {
  uint64_t frames = 0;
  uint64_t built = 0;            // draws built (first frame of a draw)
  uint64_t reused = 0;           // draws found kept
  uint64_t blocks_clean = 0, blocks_partial = 0, blocks_full = 0;  // constant blocks: untouched, some fields rewritten, built whole
  uint64_t fields_resolved = 0;  // resolver calls
  uint64_t freed_draws = 0, freed_objects = 0;
  uint64_t dup_keys = 0;         // draws that took an entry already taken in the same Record (many objects with one id: they share a list and rewrite it every draw)
  uint64_t live_draws = 0, live_objects = 0;
};

class DrawLists {
 public:
  // grace: frames an object / draw may be absent before it is freed (an object that blinks out of one record keeps its lists).
  explicit DrawLists(uint32_t grace = 1) : grace_(grace) {}

  // The grace counts GUEST frames: the live caller calls Tick(guest_frame) before each Record (two Records in one guest frame, a seed frame or a mirror next to a
  // live one, are one tick). A caller that never ticks (the offline tools) gets one tick per BeginFrame.
  // S103 G2 (review 4): the grace counts DRAWN Records: a BeginFrame whose guest frame differs from the last Record's is one tick (frames the consumer dropped never
  // reach a Record, so they age nothing).
  void Tick(uint64_t guest_frame) { pending_guest_ = guest_frame, ticked_ = true; }
  void BeginFrame() {
    if (!ticked_) ++frame_;
    else if (pending_guest_ != last_guest_) ++frame_, last_guest_ = pending_guest_;
    ++record_seq_, ++counts_.frames;
  }
  // Frees every draw not seen for more than `grace` frames, and the objects left empty. Cheap: one pass over the kept draws.
  void EndFrame();

  // The kept draw for `key`, made if it is not there (`fresh` = true: build state / textures / blocks now).
  KeptDraw& Acquire(const DrawKey& key, bool& fresh);

  // The stamps that changed since the last frame (all of them on the first), and the draw takes the new ones.
  static uint32_t Diff(KeptDraw& d, const Stamps& now, bool fresh) {
    uint32_t changed = 0;
    for (int i = 0; i < kStampCount; ++i)
      if (fresh || d.stamps.h[i] != now.h[i]) changed |= 1u << i;
    d.stamps = now;
    return changed;
  }

  // Resolves the fields of `b` whose mask intersects `changed` (the first build of a block: kStAll). resolve(const KeptField&, float* out) writes the
  // field's vec4s * 4 floats (the block's bytes at the field's offset, zeroed first). Returns the number of fields resolved. `plant_ignore` (the unit's
  // planted fault only) drops that stamp bit from `changed`.
  template <class R>
  uint32_t Refresh(KeptBlock& b, uint32_t changed, R&& resolve, uint32_t plant_ignore = 0) {
    changed &= ~(plant_ignore | plant_ignore_);
    uint32_t n = 0;
    if (!b.valid) changed = kStAll;
    for (const KeptField& f : b.fields) {
      if (b.valid && (f.mask & changed) == 0) continue;
      if (f.offset + f.vec4s * 16u > b.bytes.size()) continue;
      float* out = reinterpret_cast<float*>(b.bytes.data() + f.offset);
      std::memset(out, 0, f.vec4s * 16u);
      resolve(f, out);
      ++n;
    }
    (b.valid ? (n ? counts_.blocks_partial : counts_.blocks_clean) : counts_.blocks_full) += 1;
    b.valid = true;
    counts_.fields_resolved += n;
    return n;
  }

  // Forgets everything (a model was released, the effects were reloaded): the next frame builds every draw again.
  void Clear() { objects_.clear(), counts_.live_draws = counts_.live_objects = 0; }
  // One model (front-end asset index) was released: only its entries go.
  void ReleaseAsset(uint32_t asset);
  // A draw found kept but rebuilt whole (its binding changed): counted as built, not reused.
  void NoteRebuilt() { if (counts_.reused) --counts_.reused; ++counts_.built; }

  const ListCounts& counts() const { return counts_; }
  uint64_t frame() const { return frame_; }
  std::string Census() const;  // "[scene][lists] ..." one line

  // Unit-only planted fault: EndFrame frees nothing.
  void PlantNoFree(bool on) { plant_nofree_ = on; }
  // Unit-only planted fault: Refresh behaves as if these stamps never changed (a stale block).
  void PlantIgnore(uint32_t stamps) { plant_ignore_ = stamps; }

 private:
  struct Object {
    uint64_t seen = 0;
    std::unordered_map<uint64_t, KeptDraw> draws;
  };
  static uint64_t ObjectKey(const DrawKey& k) { return (uint64_t(k.object_id) << 32) | k.asset; }
  static uint64_t DrawKeyBits(const DrawKey& k) { return (uint64_t(k.mesh) << 24) | (uint64_t(k.instance) << 8) | k.pass; }
  uint32_t grace_;
  uint64_t frame_ = 0, record_seq_ = 0;
  bool ticked_ = false;
  uint64_t pending_guest_ = 0, last_guest_ = ~0ull;   // S103 G2
  ListCounts counts_;
  bool plant_nofree_ = false;
  uint32_t plant_ignore_ = 0;
  std::unordered_map<uint64_t, Object> objects_;
};

}  // namespace rdr_scene
