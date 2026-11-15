// The arena contributor (Session 101, lane 28). See arena_contrib.h. Host code only: no game memory, no SDK.
#include "arena_contrib.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../renderer_scene/record_io.h"
#include "tail_latch.h"  // S102 U1

namespace rdr_scene::live::arena {
namespace {

std::mutex g_lock;
ArenaSnapshot g_snap;
uint64_t g_seq = 0;        // bumped by every publish
bool g_have_slots = false;
int32_t g_slots_arena = -1;
Totals g_totals;

// S101 lane 264: what the "unknown" fields of the [scene][live] line are made of, per census window (Emit adds on the writer thread, Census reads and resets).
struct Window {
  std::atomic<uint64_t> arena_unknown{0};         // frames with no arena id (nothing is named)
  std::atomic<uint64_t> lights_unknown{0};        // no evaluated light set, or a set / light the hook could not read
  std::atomic<uint64_t> slot_table_unknown{0};    // frames whose slot table (the provider's) was not there
  std::atomic<uint64_t> look_unknown_slots{0};    // `e` / `g` slots written invisible because the look was not decided
  std::atomic<uint64_t> screen_planes_unknown{0}; // stage screens whose four plane image ids are not filled (see arena_provider.h)
};
Window g_win;

Mat4 Diag(float x, float y, float z) {
  Mat4 r{};
  r.m[0] = x, r.m[5] = y, r.m[10] = z, r.m[15] = 1.f;
  return r;
}

bool Finite3(const float* v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

class ArenaContributor final : public Contributor {
 public:
  const char* Name() const override { return "arena"; }
  void Emit(const FrameInfo&, SceneTables& t, Counters& c) override;
  bool Latch(const FrameInfo& f) override {   // S102 U1 (tail_latch.h): Emit's snapshot copy, while the game threads are parked
    std::lock_guard<std::mutex> lk(g_lock);
    latch_snap_ = g_snap;
    latch_seq_ = g_seq;
    latch_.Set(f.guest_frame);
    return true;
  }
  void Census(std::string& out) override {  // S101 lane 264: the pieces of fields_unknown, since the last census
    out += "[look_unknown_slots " + std::to_string(g_win.look_unknown_slots.exchange(0)) + ", screen_planes_unknown " + std::to_string(g_win.screen_planes_unknown.exchange(0)) +
           ", slot_table_unknown " + std::to_string(g_win.slot_table_unknown.exchange(0)) + ", lights_unknown " + std::to_string(g_win.lights_unknown.exchange(0)) +
           ", arena_unknown " + std::to_string(g_win.arena_unknown.exchange(0)) + "]";
  }

 private:
  uint64_t last_seq_ = ~0ull;  // Emit runs on the writer's thread only
  LatchMark latch_;            // S102 U1
  ArenaSnapshot latch_snap_;
  uint64_t latch_seq_ = 0;
  // S102 T11b (wwe2k14_rdr_scene_strings_once; string_pool.h): the pool field of each string this contributor writes, kept so a seeded table is written with no formatting, no
  // concatenation and no AddString. A slot's strings depend only on its number, the pac string only on the arena id. Dropped when the table's pool is another one.
  struct SlotFields { PooledField entry, base, r, y; };
  static constexpr uint32_t kMaxSlotFields = 4096;   // slots past this (or an arena id past kMaxArenas) keep the old path
  static constexpr int kMaxArenas = 128;
  std::vector<SlotFields> slot_fields_;
  std::array<PooledField, kMaxArenas> pac_;
  PooledField motion_, when_, vis_unknown_;
  std::array<PooledField, 10> screen_name_, screen_entry_;
  uint32_t pooled_for_ = 0;
};

void ArenaContributor::Emit(const FrameInfo& f, SceneTables& t, Counters& c) {
  ArenaSnapshot s;
  uint64_t seq;
  if (latch_.Use(f.guest_frame, Name())) {   // S102 U1: the snapshot the latch copied
    s = std::move(latch_snap_);
    seq = latch_seq_;
  } else {
    std::lock_guard<std::mutex> lk(g_lock);
    s = g_snap;
    seq = g_seq;
  }
  StringPool* pool = t.SeedPool();   // S102 T11b
  if (pool) pool->NoteArena(s.arena_id);   // another arena = a new pool at the next record (string_pool.h)
  const bool stale = seq == last_seq_;
  if (stale) ++c.stale;  // no hook ran since the last record
  last_seq_ = seq;
  Totals tot;
  tot.emits = 1;
  tot.stale = stale ? 1 : 0;
  if (s.arena_id < 0) {  // which arena is live is unknown: nothing to name
    ++c.unknown;
    tot.unknown = 1;
    g_win.arena_unknown.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_lock);
    g_totals.emits += tot.emits, g_totals.unknown += tot.unknown, g_totals.stale += tot.stale;
    return;
  }
  // S102 T11b: with a table seeded from the string pool the strings below are the pool's fields (kept per slot / arena); a string the pool does not hold yet (the first frames, a full pool)
  // is made and interned the old way. Without a pool (the switch off, a tool's own table) every string is made and interned as before.
  if (pool && pooled_for_ != pool->id()) {
    slot_fields_.clear();
    pac_.fill(PooledField{});
    motion_ = when_ = vis_unknown_ = PooledField{};
    screen_name_.fill(PooledField{});
    screen_entry_.fill(PooledField{});
    pooled_for_ = pool->id();
  }
  uint64_t pooled_hits = 0, pooled_misses = 0;
  const auto S = [&](PooledField* pf, auto&& make) -> uint32_t {   // make() builds the text
    if (pool && pf) {
      const uint32_t f = pf->field ? pf->field : PooledFieldOf(*pool, *pf, make);
      if (f) { ++pooled_hits; return f; }
      ++pooled_misses;
    }
    return t.AddString(make());
  };
  const auto SL = [&](PooledField* pf, const char* literal) -> uint32_t {   // a fixed name
    if (pool && pf) {
      const uint32_t f = pf->field ? pf->field : PooledFieldOf(*pool, *pf, [literal]() { return std::string(literal); });
      if (f) { ++pooled_hits; return f; }
      ++pooled_misses;
    }
    return t.AddString(literal);
  };
  int pac_index = s.arena_id;
#ifdef RDR_CROWD_IDS_PLANTS
  if (g_arena_strings_plant == 1) pac_index = 0;   // planted: the pac string's field kept without the arena id
#endif
  const uint32_t pac_s = S(pool && s.arena_id < kMaxArenas ? &pac_[size_t(pac_index)] : nullptr, [&]() {
    char pac[64];
    std::snprintf(pac, sizeof pac, "pac\\bg\\bg%02d.pac", s.arena_id);
    return std::string(pac);
  });
  const uint32_t none = kNone;

  // ---- light sets ----
  if (s.sets.empty()) {
    g_win.lights_unknown.fetch_add(1, std::memory_order_relaxed);
    ++c.unknown, ++tot.unknown;  // no evaluated light this frame (the hook did not run): lights stay unknown, never guessed
  }
  {  // S101 lane28 (review F6): light objects the hook could not read at all / read without their fifth light
    const uint32_t lost = s.dropped_sets + s.partial_sets;
    c.unknown += lost, tot.unknown += lost;
    g_win.lights_unknown.fetch_add(lost, std::memory_order_relaxed);
  }
  for (const SetSnap& ls : s.sets) {
    if (ls.set_id == kNone) {
      ++c.unknown, ++tot.unknown;
      g_win.lights_unknown.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    bool ok = Finite3(ls.block_a) && Finite3(ls.block_b);
    for (int k = 0; k < ls.n_lights && k < 5; ++k) ok = ok && Finite3(ls.dir[k]) && Finite3(ls.col[k]);
    if (!ok) {  // a non-finite value: the set is left out, counted
      ++c.unknown, ++tot.unknown;
      g_win.lights_unknown.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    for (int k = 0; k < ls.n_lights && k < 5; ++k) {
      Light l;
      l.set = ls.set_id;
      l.kind = uint8_t(LightKind::Directional);
      l.slot = uint8_t(k);
      std::memcpy(l.dir_or_pos, ls.dir[k], sizeof l.dir_or_pos);
      std::memcpy(l.color, ls.col[k], sizeof l.color);
      l.range = (ls.has_fall && k < 4) ? ls.fall[k] : kUnknownRange;  // review F1: unknown is -1, never 0 (0 is a real falloff)
      t.lights.push_back(l);
    }
    for (int a = 0; a < 2; ++a) {
      Light l;
      l.set = ls.set_id;
      l.kind = uint8_t(LightKind::Ambient);
      l.slot = uint8_t(5 + a);
      std::memcpy(l.color, a == 0 ? ls.block_a : ls.block_b, sizeof l.color);
      t.lights.push_back(l);
    }
    ++tot.sets;
  }

  // ---- slots ----
  if (s.slots.empty()) {
    g_win.slot_table_unknown.fetch_add(1, std::memory_order_relaxed);
    ++c.unknown, ++tot.unknown;  // the slot table of this arena is not known (the provider did not run): no slot objects, never a guessed list
  }
  for (const SlotSnap& sl : s.slots) {
    bool vis_unknown = false;
    if (sl.when) {
      if (s.look == Look::Unknown) {
        vis_unknown = true;  // review F2: which look is live is unknown: the slot is listed but NOT visible, marked "arena.visibility_unknown", counted
        ++c.unknown, ++tot.unknown;
        g_win.look_unknown_slots.fetch_add(1, std::memory_order_relaxed);
      } else if (s.look == Look::Match && (sl.when & 1) && !(sl.when & 2)) {
        continue;  // events-only slot in the match look
      } else if (s.look == Look::Entrance && (sl.when & 2) && !(sl.when & 1)) {
        continue;  // game-only slot in the entrance look
      }
    }
    SlotFields* sf = nullptr;
    if (pool && sl.slot < kMaxSlotFields) {
      if (slot_fields_.size() <= sl.slot) slot_fields_.resize(size_t(sl.slot) + 1);
      sf = &slot_fields_[sl.slot];
    }
    AssetRef a;
    a.kind = uint8_t(AssetKind::Model);
    a.pac = pac_s;
    {
      PooledField* entry_pf = sf ? &sf->entry : nullptr;
#ifdef RDR_CROWD_IDS_PLANTS
      if (sf && g_arena_strings_plant == 3) entry_pf = &slot_fields_[0].entry;   // planted: the entry's field kept without the slot number
#endif
      a.entry = S(entry_pf, [&]() { return "slot " + std::to_string(sl.slot); });
    }
    const uint32_t asset = uint32_t(t.assets.size());
    t.assets.push_back(a);
    const std::string base_id = sf ? std::string() : "arena:slot:" + std::to_string(sl.slot);   // (the pooled path makes the text only on a miss)
    const auto id_text = [&](const char* suffix) { return (sf ? "arena:slot:" + std::to_string(sl.slot) : base_id) + suffix; };
    const auto add = [&](uint32_t id_field, uint32_t flags, const Mat4& world) {
      Object o;
      o.asset = asset;
      o.klass = rdr_adapter::DrawClass::ModelPart;
      o.layer = sl.blend ? 1 : 0;
      o.flags = flags;
      o.id = id_field;
      o.world = world;
      o.light_set = sl.light_set;
      o.params_first = uint32_t(t.params.size());
      if (sl.motion >= 0) {
        ParamValue p;
        p.name = SL(pool ? &motion_ : nullptr, "arena.slot_motion");
        p.value[0] = float(sl.motion);
        t.params.push_back(p);
      }
      if (sl.when) {
        ParamValue p;
        p.name = SL(pool ? &when_ : nullptr, "arena.slot_when");
        p.value[0] = float(sl.when);
        t.params.push_back(p);
      }
      if (vis_unknown) {
        ParamValue p;
        p.name = SL(pool ? &vis_unknown_ : nullptr, "arena.visibility_unknown");
        p.value[0] = 1.f;
        t.params.push_back(p);
      }
      o.params_count = uint32_t(t.params.size()) - o.params_first;
      t.objects.push_back(o);
      ++c.objects, ++tot.objects;
    };
    const uint32_t vis = vis_unknown ? 0u : uint32_t(kObjVisible);
    add(S(sf ? &sf->base : nullptr, [&]() { return id_text(""); }), vis, IdentityMat4());
    if (sl.mirror & 1) {
      PooledField* r_pf = sf ? &sf->r : nullptr;
#ifdef RDR_CROWD_IDS_PLANTS
      if (sf && g_arena_strings_plant == 2) r_pf = &sf->base;   // planted: the mirror twin answers with the slot's own id field
#endif
      add(S(r_pf, [&]() { return id_text(":r"); }), vis | kObjMirrorX, Diag(-1.f, 1.f, 1.f)), ++tot.twins;
    }
    if (sl.mirror & 2) add(S(sf ? &sf->y : nullptr, [&]() { return id_text(":y"); }), vis | kObjTurned, Diag(-1.f, 1.f, -1.f)), ++tot.twins;
  }

  // ---- screens ----
  for (const ScreenSnap& sc : s.screens) {
    if (sc.key < 800 || sc.key > 809) continue;
    Target tg;
    tg.role = rdr_adapter::SurfaceRole::TronPicture;
    tg.index = uint8_t(sc.key - 800);
    PooledField* scr_name = pool ? &screen_name_[size_t(sc.key - 800)] : nullptr;
    PooledField* scr_entry = pool ? &screen_entry_[size_t(sc.key - 800)] : nullptr;
    tg.name = S(scr_name, [&]() { return "stage_screen:" + std::to_string(sc.key); });
    const uint32_t ti = uint32_t(t.targets.size());
    t.targets.push_back(tg);
    AssetRef a;
    a.kind = uint8_t(AssetKind::Movie);
    a.pac = 0;
    a.entry = S(scr_entry, [&]() { return "stage:" + std::to_string(sc.key); });  // which movie plays on it is not traced: named by the surface only
    const uint32_t ai = uint32_t(t.assets.size());
    t.assets.push_back(a);
    Movie m;
    std::memset(static_cast<void*>(&m), 0, sizeof m);   // S102 T11b: the 4 padding bytes before movie_time_s were stack bytes in every record with a screen (every field below is set)
    m.asset = ai;
    m.show = uint8_t(MovieShow::OnTarget);
    m.target = ti;
    bool known = true;
    for (int p = 0; p < 4; ++p) {
      m.planes[p] = sc.planes[p];
      known = known && sc.planes[p] != none;
    }
    if (!known) {
      ++c.unknown, ++tot.unknown;
      g_win.screen_planes_unknown.fetch_add(1, std::memory_order_relaxed);
    }
    t.movies.push_back(m);
    ++c.objects, ++tot.objects, ++tot.screens;
  }

  if (pool) pool->stats.arena_fields += pooled_hits, pool->stats.arena_fallback += pooled_misses;   // S102 T11b
  std::lock_guard<std::mutex> lk(g_lock);
  g_totals.emits += tot.emits, g_totals.objects += tot.objects, g_totals.unknown += tot.unknown, g_totals.sets += tot.sets,
      g_totals.twins += tot.twins, g_totals.screens += tot.screens, g_totals.stale += tot.stale;
}

ArenaContributor g_instance;

}  // namespace

std::vector<SlotSnap> ParseSlotText(std::string_view txt) {
  std::vector<SlotSnap> out;
  const auto find_slot = [&](int slot) -> SlotSnap& {
    for (SlotSnap& s : out)
      if (s.slot == slot) return s;
    SlotSnap n;
    n.slot = uint16_t(slot);
    n.light_set = 100;  // the loader's default for a row with no l(N)
    out.push_back(n);
    return out.back();
  };
  size_t pos = 0;
  while (pos < txt.size()) {
    size_t eol = pos;
    while (eol < txt.size() && txt[eol] != '\n') ++eol;
    const std::string line(txt.substr(pos, eol - pos));
    pos = eol + 1;
    size_t i = 0;
    while (i < line.size() && line[i] == ' ') ++i;
    if (i >= line.size() || line[i] < '0' || line[i] > '9') continue;
    int slot = 0;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') slot = std::min(slot * 10 + (line[i++] - '0'), 1000000);
    if (slot > 65535) continue;
    SlotSnap& s = find_slot(slot);
    {
      std::string tok;
      for (size_t c = i; c <= line.size(); ++c) {
        const char ch = c < line.size() ? line[c] : ' ';
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == ';') {
          if (tok == "r") s.mirror |= 1;
          if (tok == "y") s.mirror |= 2;
          if (tok == "a") s.blend |= 1;
          if (tok == "s") s.blend |= 2;
          if (tok == "e") s.when |= 1;
          if (tok == "g") s.when |= 2;
          tok.clear();
          if (ch == ';') break;
        } else {
          tok.push_back(ch);
        }
      }
    }
    {
      const size_t mp = line.find(" m(", i);
      if (mp != std::string::npos) {
        int mn = 0;
        size_t j = mp + 3;
        while (j < line.size() && line[j] >= '0' && line[j] <= '9') mn = std::min(mn * 10 + (line[j++] - '0'), 1000000);
        if (j < line.size() && line[j] == ')') s.motion = mn;
      }
    }
    const size_t lp = line.find("l(", i);
    if (lp == std::string::npos) continue;
    int ln = 0;
    size_t j = lp + 2;
    while (j < line.size() && line[j] >= '0' && line[j] <= '9') ln = std::min(ln * 10 + (line[j++] - '0'), 1000000);
    if (j < line.size() && line[j] == ')') s.light_set = uint32_t(ln);
  }
  std::sort(out.begin(), out.end(), [](const SlotSnap& a, const SlotSnap& b) { return a.slot < b.slot; });
  return out;
}

void PublishAll(const ArenaSnapshot& s) {
  std::lock_guard<std::mutex> lk(g_lock);
  g_snap = s;
  g_have_slots = !s.slots.empty();
  g_slots_arena = s.arena_id;
  ++g_seq;
}

void PublishSets(int32_t arena_id, float brightness, std::vector<SetSnap>&& sets, uint32_t dropped, uint32_t partial) {
  std::lock_guard<std::mutex> lk(g_lock);
  if (arena_id >= 0 && g_slots_arena >= 0 && arena_id != g_slots_arena) {  // another arena: the static part belongs to the old one (review F4: an unknown id (-1) keeps it)
    g_snap.slots.clear();
    g_snap.screens.clear();
    g_have_slots = false;
    g_slots_arena = -1;
  }
  g_snap.arena_id = arena_id;
  g_snap.brightness = brightness;
  g_snap.sets = std::move(sets);
  g_snap.dropped_sets = dropped;
  g_snap.partial_sets = partial;
  ++g_seq;
}

void PublishSlots(int32_t arena_id, Look look, std::vector<SlotSnap>&& slots, std::vector<ScreenSnap>&& screens) {
  std::lock_guard<std::mutex> lk(g_lock);
  if (g_snap.arena_id >= 0 && arena_id != g_snap.arena_id) return;  // a late load for an arena that is no longer the live one
  g_snap.look = look;
  g_snap.slots = std::move(slots);
  g_snap.screens = std::move(screens);
  g_have_slots = true;
  g_slots_arena = arena_id;
  ++g_seq;
}

void PublishLook(Look look) {
  std::lock_guard<std::mutex> lk(g_lock);
  g_snap.look = look;  // no ++g_seq: the same frame's PublishSets already marks the snapshot fresh; a look alone is not a new hook run
}

bool NeedsSlots(int32_t arena_id) {
  std::lock_guard<std::mutex> lk(g_lock);
  return !(g_have_slots && g_slots_arena == arena_id);
}

Contributor* Instance() { return &g_instance; }

Totals GetTotals() {
  std::lock_guard<std::mutex> lk(g_lock);
  return g_totals;
}

}  // namespace rdr_scene::live::arena
