// The arena contributor's game side (Session 101, lane 28, class C3): one post-hook on sub_827F8730, the per-frame loop that copies every light
// set into its light object. Read only: the original runs first, the hook then reads what it wrote. Off and free when no scene sink is attached
// (one relaxed load, rdr_scene::live::Enabled()).
//
// Verified from the listings (generated\default\wwe2k14_recomp.121.cpp:9962 sub_827F8730, .184.cpp:7641 sub_827F70D0, .39.cpp sub_827F7BA0, .302.cpp:9525 sub_827D7750):
//   - sub_827F8730(r3 = light manager): count = word at +124; per i the light object = (*(r3+112))[i] and its source = (*(r3+104))[i] go to
//     sub_827F70D0; afterwards, unless the float at r3+452 equals the constant 1.0, the three floats at object+400 and at object+416 are each
//     multiplied by it. [C listing]
//   - sub_827F70D0 writes: the two 16-byte blocks at object+400 / +416 (from source +4848 / +4928); per light k = 0..4 the direction vec4 at
//     object+64+16k (also +960+16k) from source+3488+16k, and the colour (3 floats) at object+144+16k (from source+5008+80k). [C listing]
//   - the set id of light object i is (*(r3+116))[i] (sub_827F7BA0 stores a stack-array word there, .39.cpp lines after "lwz r9,116(r24)"; the low
//     16 bits are used by src\lighting_mod.h). [C listing; the id's meaning as the l(N) number: C by lighting_mod.h's use + runs 6816/6822]
//   - the arena in use: the word at G+64 = 0x8376AB34, stored by sub_827D7750 (r3 -> 64(0x8376AAF4)) from the match description's arena id.
//     [C store in the listing; that it equals the pac\bg\bgNN.pac number: documented medium, docs\systems\match_setup.md; a value outside
//     0..399 is left unknown and counted by the contributor]
//   - NOT verified [U]: which of +400 / +416 is the sky; the fifth light's role; the entrance remap of sets to slots; the camera key at +300.
//
// The static part (the slot table: pac entry 50001) and the screens are NOT read from guest memory: they come from the player's files. This hook only
// asks for them (arena::NeedsSlots) and leaves the loading to the provider the main session installs with SetSlotProvider (one call at startup;
// without a provider the contributor counts the slot table as unknown and emits only the lights). THE ONE STARTUP CALL (review F5):
//   wwe2k14::scene_live::InstallArena(game_data_root());   // once, after the live writer exists; it installs the provider and registers the contributor
#pragma once

#include <rex/hook.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "frame_stats.h"  // lane 130: g_guest_frame (the stage clock origin)
#include "guest_read.h"
#include "native_zlib.h"  // stb_image's zlib decoder, which assets/pac.h needs declared
#include "scene_live/arena_contrib.h"
#include "scene_live/arena_world_glue.h"  // lane 130: the stage clock origin (StageSetupHook)
#include "scene_live/arena_provider.h"
#include "scene_live/crowd_contrib.h"  // S101 lane 1: the crowd's arena comes from this hook's read of G+64

REX_EXTERN(__imp__sub_827F8730);

namespace wwe2k14::scene_live {

namespace rl = rdr_scene::live;
namespace ra = rdr_scene::live::arena;

inline constexpr uint32_t kArenaIdWord = 0x8376AB34;  // G+64, see the header
inline constexpr uint32_t kScenePhaseWordGuest = ra::kScenePhaseWord;  // S101 lane 264: 0x836F3940, the scene phase word the stage-slot draw tests (arena_contrib.h)

// The provider fills the static part for an arena id (reads pac\bg\bgNN.pac entry 50001 through our loaders): false = could not.
using SlotProvider = std::function<bool(int32_t arena, std::vector<ra::SlotSnap>&, std::vector<ra::ScreenSnap>&)>;
inline SlotProvider& Provider() {
  static SlotProvider p;
  return p;
}
inline void SetSlotProvider(SlotProvider p) { Provider() = std::move(p); }

inline std::mutex& TriedLock() {
  static std::mutex m;
  return m;
}
inline std::set<int32_t>& Tried() {  // arena ids with a load in flight or failed (a failed load is not retried)
  static std::set<int32_t> s;
  return s;
}

inline bool Readable(const uint8_t* base, uint32_t guest, uint32_t off) {
  const uint32_t a = (guest + off) & ~3u;
  return a >= 0x10000 && wwe2k14::GuestWordReadable(a, base + a);
}
inline uint32_t Rd32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return std::byteswap(v);
}
inline float RdF(const uint8_t* base, uint32_t a) { return std::bit_cast<float>(Rd32(base, a)); }
inline bool RdF3(const uint8_t* base, uint32_t a, float out[3]) {
  if (!Readable(base, a, 0) || !Readable(base, a, 8)) return false;
  for (int i = 0; i < 3; ++i) out[i] = RdF(base, a + 4u * uint32_t(i));
  return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

// Reads every light object of the manager after the per-frame loop and publishes the evaluated sets.
inline void OnLightSets(const uint8_t* base, uint32_t mgr) {
  static std::atomic<uint32_t> bad_reads{0};
  if (!Readable(base, mgr, 0) || !Readable(base, mgr, 452)) return;
  const uint32_t n = Rd32(base, mgr + 124), objs = Rd32(base, mgr + 112), ids = Rd32(base, mgr + 116);
  if (n == 0 || n > 64 || !Readable(base, objs, 4 * n - 4) || !Readable(base, ids, 4 * n - 4)) {
    bad_reads.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  std::vector<ra::SetSnap> sets;
  sets.reserve(n);
  uint32_t dropped = 0, partial = 0;  // review F6: counted into Counters.unknown by the contributor
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t obj = Rd32(base, objs + 4 * i);
    if (obj < 0x10000 || !Readable(base, obj, 0) || !Readable(base, obj, 960)) continue;
    ra::SetSnap s;
    s.set_id = Rd32(base, ids + 4 * i) & 0xFFFF;
    s.n_lights = 4;  // lights 0-3 and both blocks are required; the fifth is optional (review F6)
    bool ok = RdF3(base, obj + 400, s.block_a) && RdF3(base, obj + 416, s.block_b);
    for (uint32_t k = 0; ok && k < 4; ++k) ok = RdF3(base, obj + 64 + 16 * k, s.dir[k]) && RdF3(base, obj + 144 + 16 * k, s.col[k]);
    if (!ok) {
      bad_reads.fetch_add(1, std::memory_order_relaxed);
      ++dropped;
      continue;
    }
    if (RdF3(base, obj + 64 + 64, s.dir[4]) && RdF3(base, obj + 144 + 64, s.col[4])) s.n_lights = 5;
    else ++partial;
    sets.push_back(s);
  }
  int32_t arena = -1;
  if (Readable(base, kArenaIdWord, 0)) {
    const uint32_t id = Rd32(base, kArenaIdWord);
    if (id < 400) arena = int32_t(id);
  }
  const float bright = RdF(base, mgr + 452);
  ra::PublishSets(arena, std::isfinite(bright) ? bright : 1.f, std::move(sets), dropped, partial);
  // S101 lane 264: the look, the way the game's own stage-slot draw decides it every frame (arena_contrib.h LookFromScenePhase: [0x836F3940] == 1 is the match).
  const bool phase_readable = Readable(base, kScenePhaseWordGuest, 0);
  const ra::Look look = ra::LookFromScenePhase(phase_readable, phase_readable ? Rd32(base, kScenePhaseWordGuest) : 0);
  ra::PublishLook(look);
  // S101 lane 1 (integration): the crowd class takes its arena (the bg pac its bodies load from) from this read, not a second one;
  // only on a change, so the crowd drops the old arena's snapshot once (crowd_contrib.h SetArena).
  {
    static std::atomic<int32_t> last_arena{-2};
    if (arena >= 0 && last_arena.exchange(arena, std::memory_order_relaxed) != arena) rdr_scene::live::crowd::The().SetArena(arena);
  }
  // review F4: the slot table is loaded once per arena id, on a worker thread (never inside the game's frame); a failed load is not retried
  // (the contributor keeps counting the table as unknown).
  if (arena >= 0 && Provider() && ra::NeedsSlots(arena)) {
    {
      std::lock_guard<std::mutex> lk(TriedLock());
      if (!Tried().insert(arena).second) return;  // loading, or failed before
    }
    std::thread([arena, look] {
      std::vector<ra::SlotSnap> slots;
      std::vector<ra::ScreenSnap> screens;
      if (Provider()(arena, slots, screens)) {
        ra::PublishSlots(arena, look, std::move(slots), std::move(screens));  // lane 264: the look read in this frame; PublishLook corrects it every frame after
        std::lock_guard<std::mutex> lk(TriedLock());
        Tried().erase(arena);  // loaded: a later arena change may need it again
      }
    }).detach();
  }
}

// The one startup call: reads the slot table from the player's own files (a folder or a disc image) and registers the contributor.
inline void InstallArena(const std::filesystem::path& game_data_root) {
  auto files = std::make_shared<launcher::GameFiles>();
  files->SetRoot(game_data_root);
  auto lock = std::make_shared<std::mutex>();
  SetSlotProvider([files, lock](int32_t arena, std::vector<ra::SlotSnap>& slots, std::vector<ra::ScreenSnap>& screens) {
    std::lock_guard<std::mutex> lk(*lock);
    std::string err;
    return ra::SlotsFromFiles(*files, arena, slots, screens, err);
  });
  ra::RegisterArena();
}

}  // namespace wwe2k14::scene_live

// sub_827F8730 = the per-frame loop over the light sets (see the header). Post-hook: the original, then a read.
REX_HOOK_RAW(sub_827F8730) {
  const uint32_t mgr = ctx.r3.u32;
  __imp__sub_827F8730(ctx, base);
  if (rdr_scene::live::Enabled()) wwe2k14::scene_live::OnLightSets(base, mgr);
}

// lane 130: sub_824E5340 = CStage::MotionSetup(this, CData*) [C string at 0x824E53D8, db stage_motion_setup]: it builds the stage's motion players once per stage set-up.
// Post-hook: the original, then the guest frame is noted as the stage clock's origin (arena_world.h: clock = frame - origin; [U] until the J1 capture compares it
// with the stream's VS c186). Read only; one relaxed load when the arena-worlds contributor is not registered.
REX_EXTERN(__imp__sub_824E5340);
REX_HOOK_RAW(sub_824E5340) {
  const uint32_t self = ctx.r3.u32;  // saved before the call (r3 is the return value afterwards)
  __imp__sub_824E5340(ctx, base);
  wwe2k14::arenaworld::StageSetupHook(wwe2k14::g_guest_frame.load(std::memory_order_relaxed), self);  // lane 130 (logs the first 16 calls)
}
