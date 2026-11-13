// The arena contributor (Session 101, lane 28). See arena_contrib.h. Host code only: no game memory, no SDK.
#include "arena_contrib.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../renderer_scene/record_io.h"

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
  void Census(std::string& out) override {  // S101 lane 264: the pieces of fields_unknown, since the last census
    out += "[look_unknown_slots " + std::to_string(g_win.look_unknown_slots.exchange(0)) + ", screen_planes_unknown " + std::to_string(g_win.screen_planes_unknown.exchange(0)) +
           ", slot_table_unknown " + std::to_string(g_win.slot_table_unknown.exchange(0)) + ", lights_unknown " + std::to_string(g_win.lights_unknown.exchange(0)) +
           ", arena_unknown " + std::to_string(g_win.arena_unknown.exchange(0)) + "]";
  }

 private:
  uint64_t last_seq_ = ~0ull;  // Emit runs on the writer's thread only
};

void ArenaContributor::Emit(const FrameInfo&, SceneTables& t, Counters& c) {
  ArenaSnapshot s;
  uint64_t seq;
  {
    std::lock_guard<std::mutex> lk(g_lock);
    s = g_snap;
    seq = g_seq;
  }
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
  char pac[64];
  std::snprintf(pac, sizeof pac, "pac\\bg\\bg%02d.pac", s.arena_id);
  const uint32_t pac_s = t.AddString(pac);
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
    AssetRef a;
    a.kind = uint8_t(AssetKind::Model);
    a.pac = pac_s;
    a.entry = t.AddString("slot " + std::to_string(sl.slot));
    const uint32_t asset = uint32_t(t.assets.size());
    t.assets.push_back(a);
    const std::string base_id = "arena:slot:" + std::to_string(sl.slot);
    const auto add = [&](const std::string& id, uint32_t flags, const Mat4& world) {
      Object o;
      o.asset = asset;
      o.klass = rdr_adapter::DrawClass::ModelPart;
      o.layer = sl.blend ? 1 : 0;
      o.flags = flags;
      o.id = t.AddString(id);
      o.world = world;
      o.light_set = sl.light_set;
      o.params_first = uint32_t(t.params.size());
      if (sl.motion >= 0) {
        ParamValue p;
        p.name = t.AddString("arena.slot_motion");
        p.value[0] = float(sl.motion);
        t.params.push_back(p);
      }
      if (sl.when) {
        ParamValue p;
        p.name = t.AddString("arena.slot_when");
        p.value[0] = float(sl.when);
        t.params.push_back(p);
      }
      if (vis_unknown) {
        ParamValue p;
        p.name = t.AddString("arena.visibility_unknown");
        p.value[0] = 1.f;
        t.params.push_back(p);
      }
      o.params_count = uint32_t(t.params.size()) - o.params_first;
      t.objects.push_back(o);
      ++c.objects, ++tot.objects;
    };
    const uint32_t vis = vis_unknown ? 0u : uint32_t(kObjVisible);
    add(base_id, vis, IdentityMat4());
    if (sl.mirror & 1) add(base_id + ":r", vis | kObjMirrorX, Diag(-1.f, 1.f, 1.f)), ++tot.twins;
    if (sl.mirror & 2) add(base_id + ":y", vis | kObjTurned, Diag(-1.f, 1.f, -1.f)), ++tot.twins;
  }

  // ---- screens ----
  for (const ScreenSnap& sc : s.screens) {
    if (sc.key < 800 || sc.key > 809) continue;
    Target tg;
    tg.role = rdr_adapter::SurfaceRole::TronPicture;
    tg.index = uint8_t(sc.key - 800);
    tg.name = t.AddString("stage_screen:" + std::to_string(sc.key));
    const uint32_t ti = uint32_t(t.targets.size());
    t.targets.push_back(tg);
    AssetRef a;
    a.kind = uint8_t(AssetKind::Movie);
    a.pac = 0;
    a.entry = t.AddString("stage:" + std::to_string(sc.key));  // which movie plays on it is not traced: named by the surface only
    const uint32_t ai = uint32_t(t.assets.size());
    t.assets.push_back(a);
    Movie m;
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
