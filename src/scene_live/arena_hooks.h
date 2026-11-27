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
//   - +400 is the sky and +416 the ground [C: sub_82BE06B8 uploads them as g_f3SkyCol / g_f3GroundCol, docs\research\s111\SKYGROUND.md].
//   - NOT verified [U]: the fifth light's role; the entrance remap of sets to slots; the camera key at +300.
//
// The static part (the slot table: pac entry 50001) and the screens are NOT read from guest memory: they come from the player's files. This hook only
// asks for them (arena::NeedsSlots) and leaves the loading to the provider the main session installs with SetSlotProvider (one call at startup;
// without a provider the contributor counts the slot table as unknown and emits only the lights). THE ONE STARTUP CALL (review F5):
//   wwe2k14::scene_live::InstallArena(game_data_root());   // once, after the live writer exists; it installs the provider and registers the contributor
#pragma once

#include "../thread_name.h"  // S102 H1
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
#include "scene_live/gsf_latch.h"   // BHF4
#include "native_zlib.h"  // stb_image's zlib decoder, which assets/pac.h needs declared
#include "scene_live/arena_contrib.h"
#include "scene_live/arena_world_glue.h"  // lane 130: the stage clock origin (StageSetupHook)
#include "scene_live/arena_id_core.h"  // S103 CAE: the arena id from the stage object
#include "scene_live/arena_provider.h"
#include "scene_live/page_skip_core.h"  // S103 EPS: the unused stage-page pass rule
#include "scene_live/latch_period.h"  // S111 CUTREACH review: the stage page's clock census (NodeClock)
#include "keep_stream.h"  // S103 EPS: WWE2K14_STREAM()
#include "scene_live/stage_fx_contrib.h"  // S103 CAE: StageFx().Stage()
#include "scene_live/crowd_contrib.h"  // S101 lane 1: the crowd's arena comes from this hook's read of G+64

REX_EXTERN(__imp__sub_827F8730);

namespace wwe2k14::scene_live {

namespace rl = rdr_scene::live;
namespace ra = rdr_scene::live::arena;

inline constexpr uint32_t kArenaIdWord = 0x8376AB34;  // G+64, see the header
inline constexpr uint32_t kLoadedStageWord = 0x83763544;  // S111 CACBG: the game's stage holder (0 = the game holds no stage: arena_id_core.h GameHoldsNoStage)

// The census line (once per change of the (stage id, published word) pair, which is rare), with the guest frame: `[arena_id] frame F: stage id N, published word M, disagreements K ...`.
inline void LogArenaIdOnChange(bool stage_ok, uint32_t stage_id, bool word_ok, uint32_t word) {
  static std::atomic<uint64_t> last{~0ull};
  const uint64_t key = (uint64_t(stage_ok ? stage_id : 0xFFFFu) << 32) | (word_ok ? word : 0xFFFFu);
  if (last.exchange(key, std::memory_order_relaxed) == key) return;
  REXLOG_INFO("[arena_id] frame {}: {}", wwe2k14::g_guest_frame.load(std::memory_order_relaxed), ra::ArenaIdCensusOf().Text(stage_ok, stage_id, word_ok, word));
}
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
  // S111 CACBG: the record names an arena only while the game holds a stage (arena_id_core.h GameHoldsNoStage): read once per frame, used by both publishes below.
  const bool holder_ok = Readable(base, kLoadedStageWord, 0);
  const bool no_stage = ra::GameHoldsNoStage(holder_ok, holder_ok ? Rd32(base, kLoadedStageWord) : 0u);
  const uint32_t n = Rd32(base, mgr + 124), objs = Rd32(base, mgr + 112), ids = Rd32(base, mgr + 116);
  if (n == 0 || n > 64 || !Readable(base, objs, 4 * n - 4) || !Readable(base, ids, 4 * n - 4)) {
    bad_reads.fetch_add(1, std::memory_order_relaxed);
    if (no_stage) ra::PublishSets(-1, 1.f, {}, 0, 0);   // S111 CACBG: no sets and no stage: the last published arena must not stay named (the slot table is kept, PublishSets review F4)
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
    if (Readable(base, obj, 1056) && Readable(base, obj, 1056 + 12)) {   // S103 RL F2: the camera-attached words of lights 0-3 (== 1: view space), next to the +64 directions [C: sub_827F70D0 +AC / +B4 writes them at +1056 + 4k in the loop that copies the directions to +64 and +960]
      s.has_follow = true;
      for (uint32_t k = 0; k < 4; ++k) s.follow[k] = Rd32(base, obj + 1056 + 4 * k) == 1u;
    }
    if (RdF3(base, obj + 64 + 64, s.dir[4]) && RdF3(base, obj + 144 + 64, s.col[4])) s.n_lights = 5;
    else ++partial;
    sets.push_back(s);
  }
  // S103 CAE: the arena is the one the GAME's stage was built for: the stage constructor sub_824E55B0 stores the id at stage + 1216 (0x824E560C; sub_82669F38 only chains into the same constructor through
  // sub_82764160, so every stage object has it). The CAW editor builds its own stage BG66 while G+64 (the match settings' arena) stays at its stale 0. G+64 only while no stage is known (arena_id_core.h).
  int32_t arena = -1;
  {
    uint32_t stage_id = 0, word = 0;
    const bool stage_ok = rdr_scene::live::stagefx::StageFx().StageIdRead(stage_id);   // [stage + 1216], page-checked by the contributor's own reader
    scene_inputs::gsf::NoteStage(stage_ok, stage_id);   // BHF4 (BH2 G4): a new stage forgets the previous arena's gGsf row
    const bool word_ok = Readable(base, kArenaIdWord, 0);
    if (word_ok) word = Rd32(base, kArenaIdWord);
    arena = ra::ArenaWhileStageHeld(no_stage, ra::ResolveArenaId(stage_ok, stage_id, word_ok, word, ra::ArenaIdCensusOf()));   // S111 CACBG: no stage held = no arena
    if (ra::ArenaIdCensusOf().NoteWord(word_ok, word, rdr_scene::live::stagefx::StageFx().CleanTakes()))   // G+64 changed: a window opens until the next stage set-up
      REXLOG_INFO("[arena_id] frame {}: G+64 changed to {} (stage id {}), clean stage_fx takes so far {}", wwe2k14::g_guest_frame.load(std::memory_order_relaxed), word, stage_ok ? int64_t(stage_id) : -1, rdr_scene::live::stagefx::StageFx().CleanTakes());
    LogArenaIdOnChange(stage_ok, stage_id, word_ok, word);
  }
  const float bright = RdF(base, mgr + 452);
  ra::PublishSets(arena, std::isfinite(bright) ? bright : 1.f, std::move(sets), dropped, partial);
  // S101 lane 264: the look, the way the game's own stage-slot draw decides it every frame (arena_contrib.h LookFromScenePhase: [0x836F3940] == 1 is the match; the editor, seq 76, writes 4 at 0x83397CA0: the game's own slot look is the entrance one there, S103 PHS).
  const bool phase_readable = Readable(base, kScenePhaseWordGuest, 0);
  const ra::Look look = ra::LookFromScenePhase(phase_readable, phase_readable ? Rd32(base, kScenePhaseWordGuest) : 0);
  ra::PublishLook(look);
  {  // S103 EPS census: the stage-page passes the game asked for and the ones skipped (page_skip_core.h), one line every 600 guest frames while any was asked (never in a build without the hook's work: g_called stays 0)
    static std::atomic<uint64_t> last_log{0};
    const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed), called = wwe2k14::pageskip::g_called.load(std::memory_order_relaxed);
    if (called && frame >= last_log.load(std::memory_order_relaxed) + 600) {
      last_log.store(frame, std::memory_order_relaxed);
      REXLOG_INFO("[page_skip] frame {}: stage-page passes asked {}, skipped {} (arena {}, page flag {}, switch {})", frame, called, wwe2k14::pageskip::g_skipped.load(std::memory_order_relaxed), arena,
                  uint32_t(wwe2k14::pageskip::PageFlagFor(arena)), wwe2k14::pageskip::g_on.load(std::memory_order_relaxed) ? "on" : "off");
    }
  }
  // S101 lane 1 (integration): the crowd class takes its arena (the bg pac its bodies load from) from this read, not a second one;
  // only on a change, so the crowd drops the old arena's snapshot once (crowd_contrib.h SetArena).
  {
    static std::atomic<int32_t> last_arena{-2};
    if (arena >= 0 && last_arena.exchange(arena, std::memory_order_relaxed) != arena) rdr_scene::live::crowd::The().SetArena(arena);
  }
  // review F4: the slot table is loaded once per arena id, on a worker thread (never inside the game's frame); a failed load is not retried
  // (the contributor keeps counting the table as unknown).
  // S103 CAR fix (blocker a, b): a created arena (200-249) has no bg pac. Its table waits (no Tried entry) until the latch published a selection for THIS id (stage_fx_contrib.h Take), then loads once per selection.
  assets::car::Sel csel;
  uint32_t tried_key = uint32_t(arena);
  if (arena >= 0 && assets::car::IsCustomId(arena)) {
    csel = assets::car::Current();
    if (!csel.valid || csel.arena != arena) return;
    uint32_t hsh = 2166136261u;
    for (char ch : csel.Key()) hsh = (hsh ^ uint8_t(ch)) * 16777619u;
    tried_key = 0x40000000u | (hsh & 0x3FFFFFFFu);
  }
  if (arena >= 0 && Provider() && ra::NeedsSlots(arena)) {
    {
      std::lock_guard<std::mutex> lk(TriedLock());
      if (!Tried().insert(int32_t(tried_key)).second) return;  // loading, or failed before
    }
    std::thread([arena, look, csel, tried_key] { wwe2k14::NameThread(L"arena_hook");
      std::unique_ptr<assets::car::ScopedThreadSel> thread_sel;
      if (csel.valid) thread_sel = std::make_unique<assets::car::ScopedThreadSel>(csel);   // the provider and PublishSlots read this selection
      std::vector<ra::SlotSnap> slots;
      std::vector<ra::ScreenSnap> screens;
      if (Provider()(arena, slots, screens)) {
        ra::PublishSlots(arena, look, std::move(slots), std::move(screens));  // lane 264: the look read in this frame; PublishLook corrects it every frame after
        std::lock_guard<std::mutex> lk(TriedLock());
        Tried().erase(int32_t(tried_key));  // loaded: a later arena change may need it again
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
    if (assets::car::IsCustomId(arena)) return ra::SlotsFromCustom(*files, assets::car::Current(), slots, screens, err);   // S103 CAR fix (blocker b): the thread's selection (the hook's worker sets it)
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
// S103 CAE (review F1): the stage destructor body sub_824E9858 (its deleting wrapper sub_824EBE58 and the derived destructors sub_827641C8 / sub_82764580 all run it; [C q decomp: it resets the vtable words 0x82016560 /
// +1152 / +1168 / +1184 and frees the stage's members; data word 0x821E5C20]). Pre-hook: the stage_fx contributor drops the stage the moment the game frees it. The destructor writes neither +1216 nor 0x83763544.
REX_EXTERN(__imp__sub_824E9858);
REX_HOOK_RAW(sub_824E9858) {
  wwe2k14::arenaworld::StageFreedHook(ctx.r3.u32);
  __imp__sub_824E9858(ctx, base);
}
// S103 CAR (BCA G1 piece 1): sub_8342A8D8(loader, light) opens the created arena's light set /CARL/%04d (r4 & 255; the game retries /CARL/0000 when it is missing). Its one caller is the created-arena loader sub_8342AA30
// (0x8342ADFC). The stage keeps the CAEN / CASD indices itself ([stage + 21184 / 21188]) but not this one, so the stage_fx contributor takes it from this pre-hook (one atomic store; no guest read, no behaviour change).
REX_EXTERN(__imp__sub_8342A8D8);
REX_HOOK_RAW(sub_8342A8D8) {
  if (rdr_scene::live::stagefx::StageFxRegistered()) rdr_scene::live::stagefx::StageFx().NoteCarLight(ctx.r4.u32);
  __imp__sub_8342A8D8(ctx, base);
}
// S103 EPS (360 HABIT row 10; docs\research\s103\PHS.md "BG66"): sub_825BB0E0 = the stage-page pass object's draw (vtable 0x8201F28C slot 1; this = object + 16, 0 direct callers: data words 0x8201F290 / 0x821E9608).
// In the stream-free build it is skipped while the live stage has no page consumer (page_skip_core.h: the arena's published page flag, or BG66 + sequence 76 until the flag is known) AND the object's entrance
// word [this + 72] (obj + 88) is 0 (a set word runs sub_825BACE0, which writes obj + 416 + 16 i: such a frame is never skipped).
// THE SAVING IS GUEST-CPU WORK ONLY: the body records ~40 render-state / pass records into the guest's command buffer and replays them (owner_guest_gpu_only: the stream-free build runs NO GPU work for them, and our own
// BG66 page is empty). Nothing the owned renderer draws changes.
// What the skip replays (the EPS adversary, q decomp sub_825BB0E0 / sub_82BCC510 / sub_82274678, docs\research\s103\PHS.md): ONLY the write at 0x825BB354..0x825BB358, sub_82BCC510([0x8392DDA4], [this + 48]) = the page
// texture handle at word +1180 of the calling thread's 2208-byte element (read back by sub_827F22C8, 0x827F2434, the LightMapParam upload), under the body's own tests ([0x83763544] != 0, [0x8392DDA4] != 0). Everything else is
// identity on guest state: the device getters read at its start (+88..+D0) are the values its tail (+210..+258) sets back, [0x83929A68]+300 / +304 are restored from r26 / r25, the 0x8392DD80 record is saved + reset by
// sub_82274678 and copied back by the loop at 0x825BB37C. The one call the tail does not undo, sub_82BB7BD0(dev, 0, 2) (vtable slot 30, +138), is sub_82C23FE0 = recorder_set_sampler_filter, stage 0 filter code 2 [C, Opus
// review]: it only records a record; nothing in the stream-free build consumes it. Return value: the original leaves r3 = [0x8392DDA4] after the replayed call (sub_82BCC510 returns its r3) when [0x83763544] != 0, else
// r3 = this; a vtable task slot, callers ignore it [U: the dispatcher was not read]; the hook leaves the same r3 either way.
REX_EXTERN(__imp__sub_825BB0E0);
REX_EXTERN(sub_82BCC510);
// SKIP-ORIGINAL: the stream-free page skip (pageskip::ShouldSkip) answers like the original's tail without walking the page pass (the comment inside the branch); otherwise the original runs
REX_HOOK_RAW(sub_825BB0E0) {
  scene_inputs::period::g_page_node_clock.Note(wwe2k14::g_guest_frame.load(std::memory_order_relaxed));   // S111 CUTREACH review: the clock the page pass runs on (log only; both builds), noted before the skip test
#if !WWE2K14_STREAM()
  namespace ps = wwe2k14::pageskip;
  ps::g_called.fetch_add(1, std::memory_order_relaxed);
  if (ps::g_on.load(std::memory_order_relaxed)) {
    namespace sl = wwe2k14::scene_live;
    ps::Inputs in;
    in.stream_free = true, in.cvar_on = true;
    uint32_t stage_id = 0;
    in.stage_known = rdr_scene::live::stagefx::StageFx().StageIdRead(stage_id);
    in.stage_arena = in.stage_known && stage_id < 0x7FFFFFFFu ? int32_t(stage_id) : -1;
    in.flag = ps::PageFlagFor(in.stage_arena);
    in.entrance_word_known = sl::Readable(base, ctx.r3.u32, 72);
    in.entrance_word = in.entrance_word_known ? sl::Rd32(base, ctx.r3.u32 + 72) : 0;   // [this + 72] = obj + 88, the stage-page entrance word (PHS B7)
    if (in.flag == ps::PageFlag::Unknown && in.stage_arena == ps::kEditorArena) {   // the fallback's second test, read only when it can matter: [[[0x839400F8]] + 48] (step_rate.h OnFrameInner)
      const uint32_t p1 = sl::Readable(base, 0x839400F8u, 0) ? sl::Rd32(base, 0x839400F8u) : 0;
      const uint32_t p2 = p1 >= 0x40000000u && p1 < 0xE0000000u && sl::Readable(base, p1, 0) ? sl::Rd32(base, p1) : 0;
      if (p2 >= 0x40000000u && p2 < 0xE0000000u && sl::Readable(base, p2, 48)) in.seq_known = true, in.seq_id = sl::Rd32(base, p2 + 48);
    }
    if (ps::ShouldSkip(in)) {
      ps::g_skipped.fetch_add(1, std::memory_order_relaxed);
      const uint32_t self = ctx.r3.u32;
      const uint32_t global = sl::Readable(base, 0x8392DDA4u, 0) ? sl::Rd32(base, 0x8392DDA4u) : 0;
      const bool stage_global = sl::Readable(base, 0x83763544u, 0) && sl::Rd32(base, 0x83763544u) != 0;   // the body's own outer test (0x825BB0F4): nothing at all runs without it
      if (stage_global && global != 0 && sl::Readable(base, self, 48)) {
        ctx.r3.u64 = global, ctx.r4.u64 = sl::Rd32(base, self + 48);
        sub_82BCC510(ctx, base);   // a leaf (no stack, no calls): r1 / lr stay as they are; returns r3 = global, as the original's tail does
      } else if (stage_global) {
        ctx.r3.u64 = global;       // the original's r3 = [0x8392DDA4] (0 here) with no call made
      }
      return;
    }
  }
#endif
  __imp__sub_825BB0E0(ctx, base);
}
REX_HOOK_RAW(sub_824E5340) {
  const uint32_t self = ctx.r3.u32;  // saved before the call (r3 is the return value afterwards)
  __imp__sub_824E5340(ctx, base);
  wwe2k14::arenaworld::StageSetupHook(wwe2k14::g_guest_frame.load(std::memory_order_relaxed), self);  // lane 130 (logs the first 16 calls)
}
