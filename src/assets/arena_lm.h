// S103 B3 (docs\research\s103\T3b_lightmap_writer.md, docs\formats\misc02_ingame_arena_post.md): the stage's LightMapParam (vs / ps constant c184) from the game's OWN arena table.
// The game's writer is sub_827F22C8 (wwe2k14_recomp.253.cpp:9898-10075): c184 = (1 / (float)X, 1 / (float)Z, (float)G * 0.01f, 1) where (X, Z, G) are three int32 of the record for the arena id in
// `pac\misc02_ingame.pac` leaf p0/102/5 (sub_827F28A0 loads it as key 5 of PACH 102). The leaf is big-endian; its base is the EDAT block at leaf+8: count = Be(leaf+20) (100), the pointer array at
// leaf + 8 + Be(leaf+32); a pointer is an offset from the EDAT base, so the record = leaf + 8 + pointer = (1, X, Z, G). The table is parsed at run time from the player's own file (never committed, never a
// table of ours); this header is pure (bytes in, numbers out) so the game, the viewer and the offline rig all use it.
// The game's guards are only: id < 100 (a SIGNED compare: no lower bound), the post settings object [0x8392DD88] != 0, id < count; the table [[0x83924768]+44] is read with no null test (the game has no
// "no table" state). An unused record (0, 0, 0) is uploaded as (inf, inf, 0, 1): gain 0, the light map adds nothing; we upload a FINITE span (1, 1) with gain 0 for it (the same picture, no inf in a
// shader) and count it (g_zero). An id >= 100 / >= count: the game uploads NOTHING and the GPU keeps the last c184 written (S111 LMPREV). Ours holds the same state: Held() = the last value Compute
// produced (counted g_clamped). No table (our read of the player's file failed, counted g_no_table) takes the same no-upload path. Before any upload the held value is the register file's boot value,
// zero (the SDK's RegisterFile constructor zeroes every register) [L: no other boot writer of c184 found]. An id < 0 (the game would read before its pointer array) is no upload here. The card quad's
// factor ([0x8376383C] * 0.001, applied only when > 0) is a run-time word of the game: scene record row kSlCardFactor, card_gain.h / front_end.cpp.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace assets::arena_lm {

struct Record { int32_t x = 0, z = 0, g = 0; };
struct Table { std::vector<Record> rec; };   // index = the game's arena id (0..count-1)
// v = c184 (x = 1 / X, y = 1 / Z, z = G * 0.01, w = 1). held = no upload this time: v is the c184 the GPU still holds (the last one produced); none = held with nothing uploaded yet: v = the boot zero.
struct Param { float v[4] = {0, 0, 0, 1}; bool clamped = false; bool no_table = false; bool zero = false; bool held = false; bool none = false; };

inline std::atomic<uint64_t> g_clamped{0}, g_no_table{0}, g_zero{0};   // lookups with an id out of range / with no table at all (both: no upload, the c184 held) / of an unused (0, 0, 0) record (gain 0)

// The c184 the GPU holds: the last value an upload wrote (sub_827F22C8 writes it only when its guards pass). One per process, as the GPU register is; zero before any upload (the register's boot value).
struct HeldC184 { std::mutex mu; bool any = false; float v[4] = {0, 0, 0, 0}; };
inline HeldC184& Held() { static HeldC184 h; return h; }

inline uint32_t Be(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// The leaf p0/102/5 (decoded bytes). false = not the layout (the caller counts / falls back). Every offset is bounds-checked (the file is untrusted).
inline bool ParseLeaf(const uint8_t* d, size_t n, Table& t) {
  t.rec.clear();
  if (!d || n < 40 || std::memcmp(d + 8, "EDAT", 4) != 0) return false;
  const uint32_t count = Be(d + 20);
  const size_t arr = size_t(8) + Be(d + 32);
  if (count == 0 || count > 4096 || arr + 4 * size_t(count) > n) return false;
  t.rec.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t ptr = Be(d + arr + 4 * size_t(i));
    if (size_t(ptr) + 8 + 16 > n) return t.rec.clear(), false;
    const uint8_t* r = d + 8 + ptr;   // (1, X, Z, G)
    t.rec[i] = {int32_t(Be(r + 4)), int32_t(Be(r + 8)), int32_t(Be(r + 12))};
  }
  return true;
}

// sub_822AA2F8: an id of 200..249 is a remap; sub_822A7ED8 then returns [[0x83763544] + 21192], or 99 only when that object is null.
// S103 CAR: `bg` = the real bg arena id of a created arena ([stage + 21192], read at the latch: sub_822A7ED8 returns it); 99 only when it was not read (-1).
inline int RemapId(int id, int bg = -1) { return (id >= 200 && id <= 249) ? (bg >= 0 && bg < 400 ? bg : 99) : id; }

inline Param Compute(const Table* t, int arena, int bg = -1) {
  Param p;
  const int id = RemapId(arena, bg);
  HeldC184& h = Held();
  const bool no_table = !t || t->rec.empty();
  if (no_table || id < 0 || id >= 100 || size_t(id) >= t->rec.size()) {   // the game's guards (and ours: no table): no upload, the c184 the GPU holds stays
    if (no_table) {
      p.no_table = true;
      g_no_table.fetch_add(1, std::memory_order_relaxed);
    } else {
      p.clamped = true;
      g_clamped.fetch_add(1, std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lk(h.mu);
    p.held = true, p.none = !h.any;
    for (int k = 0; k < 4; ++k) p.v[k] = h.v[k];
    return p;
  }
  const Record r = t->rec[size_t(id)];
  if (r.x == 0 && r.z == 0 && r.g == 0) {   // an unused record: the game uploads (inf, inf, 0, 1); here a finite span, gain 0
    p.zero = true;
    g_zero.fetch_add(1, std::memory_order_relaxed);
    p.v[0] = 1.0f, p.v[1] = 1.0f, p.v[2] = 0.0f, p.v[3] = 1.0f;
  } else {
    // single precision as the game: fdivs 1 / (float)X, fmuls (float)G * 0.01f (the f32 constant at 0x82003C8C)
    p.v[0] = r.x != 0 ? 1.0f / float(r.x) : 1.0f, p.v[1] = r.z != 0 ? 1.0f / float(r.z) : 1.0f, p.v[2] = float(r.g) * 0.01f, p.v[3] = 1.0f;   // [f32]
  }
  std::lock_guard<std::mutex> lk(h.mu);   // an upload: this becomes the c184 the GPU holds
  h.any = true;
  for (int k = 0; k < 4; ++k) h.v[k] = p.v[k];
  return p;
}

}  // namespace assets::arena_lm
