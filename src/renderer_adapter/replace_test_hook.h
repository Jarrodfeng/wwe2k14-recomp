// S101 lane 41: the TEST hook of the in-place replace seam (draw_replace_hook.h). It plugs into the same Binder::AddReplaceHook the arena and wrestler
// classes use, with no class code: it replaces every claimed draw with ITS OWN STREAM geometry (the draw's captured vertex / index bytes, uploaded as
// our own buffers), so a correct seam gives a byte-identical picture. The acceptance test (tools\replace\replace_seam_check.py) drives it through
// fview (--replace-test [--replace-fault LIST]) and proves: transparent, refusals keep the stream draw, the row override changes only the named rows,
// the counts add up, it coexists with the crowd hook. Not shipped in any path a player turns on: only fview creates it.
//
// Faults (comma list, --replace-fault): all (claim every named draw, not just model parts), refuse_odd (every second claimed draw is refused: kept),
// not_ready (FrameReady false: every claimed draw kept, "not ready"), no_vertex (returns true with no vertex buffer: the binder must keep the
// stream draw), rows (override the first constant range's rows with the stream's own values: transparent), rows_off_by_one (the same rows one
// register late: must change the picture), half_count (index_count / 2), slot_swap (slot 0 set to another image of the frame), topology (a strip given as
// a list or the reverse), unset_layout (the layout left unset: refused as Mismatch), slot_same (slot 0 set to the stream's own image, view format and swizzle: transparent).
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "../renderer/renderer.h"
#include "../renderer/shaders/spv/fx_registry.h"
#include "draw_replace_hook.h"

namespace rdr_adapter {

struct ReplaceTestOptions {
  bool all = false, refuse_odd = false, not_ready = false, no_vertex = false, rows = false, rows_off_by_one = false;
  bool half_count = false, slot_swap = false, topology = false, unset_layout = false, slot_same = false;
  // Parses the comma list; returns false and names the unknown word.
  static bool Parse(const std::string& list, ReplaceTestOptions& o, std::string& error) {
    size_t at = 0;
    while (at <= list.size()) {
      size_t comma = list.find(',', at);
      if (comma == std::string::npos) comma = list.size();
      const std::string word = list.substr(at, comma - at);
      at = comma + 1;
      if (word.empty()) continue;
      if (word == "all") o.all = true;
      else if (word == "refuse_odd") o.refuse_odd = true;
      else if (word == "not_ready") o.not_ready = true;
      else if (word == "no_vertex") o.no_vertex = true;
      else if (word == "rows") o.rows = true;
      else if (word == "rows_off_by_one") o.rows = o.rows_off_by_one = true;
      else if (word == "half_count") o.half_count = true;
      else if (word == "slot_swap") o.slot_swap = true;
      else if (word == "topology") o.topology = true;
      else if (word == "unset_layout") o.unset_layout = true;
      else if (word == "slot_same") o.slot_same = true;
      else {
        error = "unknown --replace-fault '" + word + "' (all, refuse_odd, not_ready, no_vertex, rows, rows_off_by_one, half_count, slot_swap, topology, unset_layout, slot_same)";
        return false;
      }
    }
    return true;
  }
};

class ReplaceTestHook final : public DrawReplaceHook {
 public:
  ReplaceTestHook(rdr::Renderer& renderer, const ReplaceTestOptions& options) : renderer_(renderer), options_(options) {}
  ~ReplaceTestHook() override {
    for (auto& [offset, entry] : cache_) renderer_.DestroyBuffer(entry.buffer);
    for (const Pending& p : pending_) renderer_.DestroyBuffer(p.buffer);
  }
  const char* Name() const override { return "test"; }
  // The independent tally of the acceptance test: how many draws this hook was asked about and said yes to (the binder's replaced + kept must equal it).
  bool Claims(const DrawOrigin& origin) const override {
    const bool yes = options_.all ? origin.klass != DrawClass::Unnamed : origin.klass == DrawClass::ModelPart;
    if (yes) ++claims_seen_;
    return yes;
  }
  bool FrameReady(const LoadedFrame&) override {
    ++frame_;
    Sweep();
    return !options_.not_ready;
  }
  bool Replace(const ReplaceIn& in, ReplaceOut& out, KeepWhy& why) override {
    ++replace_calls_;
    if (options_.refuse_odd && (replace_calls_ & 1)) return why = KeepWhy::NotOurs, false;
    if (!in.frame || !in.draw || !in.read_geometry || !in.draw->vertex_bytes) return why = KeepWhy::Mismatch, false;
    if (!(in.frame->header.flags & kFrameSixteenPairsHighFirst)) return why = KeepWhy::Variant, false;  // an old capture: the binder swaps 16-bit pairs on upload, we do not
    if (options_.no_vertex) return true;  // a hook that says yes and gives nothing: the binder must treat it as a refusal and keep the stream draw
    const bool inline_geometry = (in.draw->flags & kDrawInline) != 0;  // per-frame bytes: the offset may name other bytes next frame, so never cached
    if (!Upload(in.vertex_offset, in.draw->vertex_bytes, inline_geometry, in, out.vertex)) return why = KeepWhy::Mismatch, false;
    if (in.draw->index_bytes) {
      if (!Upload(in.index_offset, in.draw->index_bytes, inline_geometry, in, out.index)) return why = KeepWhy::Mismatch, false;
      out.index_count = in.draw->index_count;
    } else {
      out.index = {};
      out.index_count = in.draw->vertex_count;
    }
    // The geometry description is the stream's own (copied from ReplaceIn::stream_*; the binder refuses a hook that leaves any of them unset).
    if (!options_.unset_layout) out.vertex_layout = in.stream_layout;
    out.topology = in.stream_topology;
    out.index_32bit = in.stream_index_32bit;
    if (options_.half_count) out.index_count /= 2;                                   // planted: half the elements -> a different picture
    if (options_.topology) out.topology = in.stream_topology == 3 ? 4 : 3;          // planted: a strip given as a list or the reverse
    if (in.textures && in.textures->slots[0].image != rdr::kNoImage) {
      const rdr::TextureSlot& stream0 = in.textures->slots[0];
      if (options_.slot_same) {  // transparent: slot 0 set to the stream's own image, with its own view format / swizzle
        out.slots[0] = {true, stream0.image, stream0.view_format, {stream0.swizzle[0], stream0.swizzle[1], stream0.swizzle[2], stream0.swizzle[3]}};
      }
      if (options_.slot_swap && alt_slot0_ != rdr::kNoImage && alt_slot0_ != stream0.image) out.slots[0] = {true, alt_slot0_, rdr::Format::Undefined, {0, 0, 0, 0}};  // planted: another image of the frame
      if (stream0.image != last_slot0_) alt_slot0_ = last_slot0_, last_slot0_ = stream0.image;
    }
    if (options_.rows && in.vs >= 0 && in.vs_block) {
      const rdr::fx::Variant& v = rdr::fx::kVariants[in.vs];
      if (v.constant_count) {
        const rdr::fx::ConstantRange& range = v.constants[0];
        rows_.assign(reinterpret_cast<const float*>(in.vs_block + range.offset), reinterpret_cast<const float*>(in.vs_block + range.offset) + size_t(range.count) * 4);
        out.first_register = range.reg + (options_.rows_off_by_one ? 1u : 0u);
        out.rows = range.count;
        out.row_data = rows_.data();
      }
    }
    return true;
  }
  uint64_t claims_seen() const { return claims_seen_; }

 private:
  struct Pending { rdr::BufferId buffer; uint64_t frame; };
  bool Upload(uint64_t offset, uint32_t bytes, bool per_frame, const ReplaceIn& in, rdr::BufferRange& range) {
    if (!per_frame) {
      auto it = cache_.find(offset);
      if (it != cache_.end() && it->second.size == bytes) {
        it->second.last_frame = frame_;
        range = {it->second.buffer, 0};
        return true;
      }
    }
    std::vector<uint8_t> data;
    if (!in.read_geometry(offset, bytes, data) || data.size() < bytes) return false;
    const rdr::BufferId id = renderer_.CreateBuffer(bytes, data.data());
    if (per_frame) pending_.push_back({id, frame_});
    else cache_[offset] = {id, frame_, bytes};
    range = {id, 0};
    return true;
  }
  void Sweep() {
    // Buffers the renderer destroys are deferred by its own 12 frames, so a destroy of one used last frame is safe.
    for (size_t i = 0; i < pending_.size();) {
      if (frame_ - pending_[i].frame > 2) renderer_.DestroyBuffer(pending_[i].buffer), pending_[i] = pending_.back(), pending_.pop_back();
      else ++i;
    }
    if (frame_ % 60) return;
    for (auto it = cache_.begin(); it != cache_.end();) {
      if (frame_ - it->second.last_frame > 120) renderer_.DestroyBuffer(it->second.buffer), it = cache_.erase(it);
      else ++it;
    }
  }
  struct CachedEntry { rdr::BufferId buffer; uint64_t last_frame; uint32_t size; };

  rdr::Renderer& renderer_;
  ReplaceTestOptions options_;
  std::unordered_map<uint64_t, CachedEntry> cache_;
  std::vector<Pending> pending_;
  std::vector<float> rows_;
  uint64_t frame_ = 0, replace_calls_ = 0;
  rdr::ImageId last_slot0_ = rdr::kNoImage, alt_slot0_ = rdr::kNoImage;  // slot_swap: the last two different slot-0 images seen
  mutable uint64_t claims_seen_ = 0;
};

}  // namespace rdr_adapter
