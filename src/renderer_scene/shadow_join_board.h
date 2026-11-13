// The joins the shadow casters read (Session 101, lane 99 = SO; docs\renderer\SHADOW_PROMOTION_MAP.md section 4 "Pairing key"). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The shadow passes draw BEFORE the colour pass, so the wrestlers' and the ring's hooks have not seen this frame's colour draws when the first shadow draw arrives. Each hook publishes here, read only,
// (1) an early frame start (the work its FrameReady does, done once for the frame by whoever asks first) and (2) a JoinOf: which file model / scale / ring arena an object is. The shadow hook asks;
// nothing here changes a hook's decisions. Header only; set on the render thread by the hooks' constructors, cleared by their destructors.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "../assets/ring_game_space.h"
#include "../assets/wrestler_roster.h"
#include "../renderer_adapter/capture_reader.h"

namespace rdr_scene {

enum class BodyStatus : uint8_t { NotWrestler, Pending, Unjoined, Joined };

struct ShadowBody {
  std::shared_ptr<assets::wrestler_roster::Model> model;   // the file model (kept alive by this pointer)
  float scale = 0;                                         // the body's one position scale
  int entry = -1;                                          // roster entry
  // Per part, the settled other-attire model of lane 100 (null entry / null vector: the object's own model). Index = DrawOrigin::part.
  std::shared_ptr<const std::vector<std::shared_ptr<assets::wrestler_roster::Model>>> part_models;
  int EntryOf(uint32_t part) const {   // the roster entry the part's bytes come from (identity for caches)
    if (part_models && part < part_models->size() && (*part_models)[part] && (*part_models)[part]->entry_index >= 0) return (*part_models)[part]->entry_index;
    return entry;
  }
  const assets::jboy_parts::Part* PartOf(uint32_t part) const {
    if (!model) return nullptr;
    if (part_models && part < part_models->size() && (*part_models)[part]) {
      const auto& m = *(*part_models)[part];
      return part < m.parts.parts.size() ? &m.parts.parts[part] : nullptr;
    }
    return part < model->parts.parts.size() ? &model->parts.parts[part] : nullptr;
  }
};

// lane 225: the Create a Superstar editor's wrestler is not a roster model: its 12 parts are joined part by part to edit-pac leaves by the caw hook (caw_core.h). The shadow passes and the rim map draw the
// same object (same object address, same part numbers, same counts); the caw hook publishes each part's confirmed join here (read only), the shadow side re-checks its own bytes against the stream.
struct CawShadowPart {
  std::shared_ptr<assets::wrestler_roster::Model> model;   // the edit leaf (kept alive by this pointer)
  uint32_t mesh = 0, group = 0;                            // the mesh in the leaf, the leaf's group id (identity for caches)
  float scale = 0;                                         // the part's fitted position scale
  const assets::jboy_parts::Part* Part() const { return model && mesh < model->parts.parts.size() ? &model->parts.parts[mesh] : nullptr; }
};

// lane 124 (review B1): the wrestlers' early start is deduped on the FRAME KEY alone (WrestlerOurs::FrameKey): the shadow casters' and the rim map's hooks both ask for it in one bind, whatever
// bind numbers they count (a frame split at a readback is bound twice, and a per-hook running number drifted apart there). WrestlerOurs::FrameReady clears the early start at the end of every bind,
// so a second bind of the same frame starts it again, once. Counters (read by the rim map's census): binds = wrestlers' FrameReady calls, starts = FrameReadyImpl runs; one start per bind is the rule.
struct WrestlersStartCounts {
  uint64_t binds = 0, starts = 0, early_hits = 0;
  bool serial_keyed_plant = false;   // plant serial_running: the OLD dedupe (key AND the caller's bind number) back on
};
inline WrestlersStartCounts g_wrestlers_starts;

using ShadowReadGeometry = std::function<bool(uint64_t offset, uint32_t bytes, std::vector<uint8_t>& out)>;

struct ShadowJoinBoard {
  // Wrestlers: start the frame early; `serial` is the caller's per-bind number (idempotent for one serial of one frame); false = not ready (no record / roster): every wrestler caster stays the stream's.
  std::function<bool(const rdr_adapter::LoadedFrame&, uint64_t serial)> wrestlers_begin;
  // Wrestlers: the model of a stream object, joined through ALL its colour draws of this frame (every one rebuilds with the one scale).
  std::function<BodyStatus(const rdr_adapter::LoadedFrame&, uint32_t object, const ShadowReadGeometry&, ShadowBody&)> wrestlers_join;
  // Ring: the arena's file meshes (entry 956 is the rope proxy); the ring loads it early when the record names an arena. Null = not ready.
  std::function<std::shared_ptr<const assets::ring_gs::RingArena>(const rdr_adapter::LoadedFrame&)> ring_arena;
  // Ring: the entry the ring hook joined an object to (-1 none): a ring piece other than 956 never casts.
  std::function<int(uint32_t object)> ring_entry_of;
  // lane 225: the editor's wrestler: the caw hook's byte-confirmed join of (object, part); false = not an editor part (or not joined yet: the first frame, the stream's).
  std::function<bool(uint32_t object, uint32_t part, CawShadowPart&)> caw_join;
  const void* caw_owner = nullptr;
  const void* wrestlers_owner = nullptr;
  const void* ring_owner = nullptr;
};
inline ShadowJoinBoard g_shadow_join_board;

}  // namespace rdr_scene
