// See ui_ours_core.h. Session 101, lane 66.
#include "ui_ours_core.h"

#include "stream_hash_tap.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rdr_scene {


// ---- faults ---------------------------------------------------------------------------------------------------------------------------------------------------
bool UiFaults::Parse(const std::string& name, UiFaults& out) {
  out = {};
  if (name.empty()) return true;
  if (name.rfind("unchecked_", 0) == 0) {
    if (!Parse(name.substr(10), out) || name == "unchecked_stale_after_join" || name == "unchecked_stale_stream") return out = {}, false;
    out.no_checks = true;
    return true;
  }
  if (name == "stale_stream") out.names_stripped = out.stale_after_join = true;
  else if (name == "first_texture" || name == "wrong_texture") out.first_texture = true;   // lane 126: wrong_texture = the first joined texture for every sprite
  else if (name == "no_view_check") out.no_view_check = true;   // lane 126
  else if (name == "no_deep") out.no_deep = true;               // lane 126
  else if (name == "deep_wrong_arena") out.deep_wrong_arena = true;   // lane 126
  else if (name == "tier_boundary") out.tier_boundary = true;         // lane 126
  else if (name == "icon_as_font") out.icon_as_font = true;
  else if (name == "rb_swap") out.rb_swap = true;
  else if (name == "key_shift") out.key_shift = true;
  else if (name == "short_mips") out.short_mips = true;
  else if (name == "names_stripped") out.names_stripped = true;
  else if (name == "stale_after_join") out.stale_after_join = true;
  else if (name == "gpu_copy_after_join") out.gpu_copy_after_join = true;
  else return false;
  return true;
}

// ---- the decisions --------------------------------------------------------------------------------------------------------------------------------------------
bool UiCore::JoinByContent(const rdr_adapter::ImageContent& c, const uint8_t* swizzle, uint32_t& entry) {
  if (!index_) return false;
  std::vector<uint32_t> ids;
  index_->FindAll(c.width, c.height, rdr::Format(uint8_t(c.format)), c.mips, c.hash, ids);
  if (ids.empty()) return false;
  entry = ids.front();
  for (const uint32_t id : ids) {   // review F4: the first whose colours come out right through the stream slot's swizzle (ids are in index order)
    UiFileEntry fe;
    if (index_->Get(id, fe) && UiSwizzleMatches(fe.format, rdr::Format(uint8_t(c.format)), swizzle)) return entry = id, true;
  }
  return true;   // none matches: the first, and the per-draw swizzle check refuses it (counted)
}

UiCore::Verdict UiCore::Decide(uint32_t key, uint32_t stream_image, const uint8_t* swizzle, uint32_t view_format, uint32_t min_level, uint32_t max_level, UiTextureProvider& provider) {
  using rdr_adapter::KeepWhy;
  Verdict v;
  ++census_.claimed;
  if (!stream_image) return ++census_.untextured, v;
  rdr_adapter::ImageContent c;
  if (!content_ || !content_(stream_image, c)) return ++census_.gpu, v;   // no content was ever told: a surface (a GPU picture) or a texture not read yet
  if (c.hash == 0) return ++census_.movie, v;
  if (!key) return ++census_.nokey, v;
  if (const StreamHashTapFn tap = g_stream_hash_tap.load(std::memory_order_relaxed)) tap(key, c.hash, c.mips);   // S103 PN23 dev parity (null unless --wwe2k14_rdr_picture_source): the stream's content hash of this key
  const auto frame_key = frame_keys_.emplace(key, stream_image);
  if (!frame_key.second && frame_key.first->second != stream_image) {   // the key names two images in one frame: not 1:1, the draw and the key's join are dropped
    joins_.erase(key);
    return ++census_.mismatch_key, ++census_.mismatch, v.why = KeepWhy::Mismatch, v;
  }
  Join& j = joins_[key];
  const uint32_t index_gen = index_ ? index_->generation() : 0;
  const bool known = j.image != 0;
  const bool content_changed = known && (j.image != stream_image || j.generation != c.generation);
  if (!known || content_changed || (j.entry < 0 && j.index_gen != index_gen)) {
    const bool was_joined = known && j.entry >= 0;
    if (content_changed && was_joined) ++census_.changed_after_join;
    uint32_t e = 0;
    bool declared = false;
    bool found = false;
    if (const auto d = declared_.find(key); d != declared_.end()) e = d->second, declared = found = true;
    else found = JoinByContent(c, swizzle, e);
    j.image = stream_image, j.generation = c.generation, j.index_gen = index_gen, j.tex = nullptr;
    if (!found) {
      j.entry = -1;
      return ++census_.unjoined, v;
    }
    j.entry = e, j.declared = declared;
    if (!was_joined) {
      join_order_.push_back(key);
      ++(declared ? census_.joined_declared : census_.joined_content);
    } else if (content_changed) {
      ++census_.rejoined;
    }
  }
  if (j.entry < 0) return ++census_.unjoined, v;
  uint32_t entry = uint32_t(j.entry);
  // The join-side plants (the entry used is not the one the content named).
  if (faults_.first_texture) {
    if (first_entry_ < 0) first_entry_ = entry;
    entry = uint32_t(first_entry_);
  }
  if (faults_.key_shift && join_order_.size() > 1) {
    const auto at = std::find(join_order_.begin(), join_order_.end(), key);
    const uint32_t other = at == join_order_.end() || at == join_order_.begin() ? join_order_.back() : *(at - 1);
    if (const auto o = joins_.find(other); o != joins_.end() && o->second.entry >= 0) entry = uint32_t(o->second.entry);
  }
  if (faults_.icon_as_font && index_) {
    uint32_t sheet = 0, icons = 0;
    if (index_->FindPath(0, "p0/0/0", sheet) && index_->FindPath(0, "p0/0/1", icons) && entry == sheet) entry = icons;
  }
  if (min_level != 0) return ++census_.mismatch_mips, ++census_.mismatch, v.why = KeepWhy::Mismatch, v;   // a slot that starts above mip 0 stays the stream's (the join is kept)
  // A view of another class than the storage (a signed / biased reinterpretation of the stream's image): ours is the plain picture, so the stream's draw stays (review F3).
  if (!faults_.no_view_check && view_format != 0 && UiByteClass(rdr::Format(uint8_t(view_format))) != UiByteClass(rdr::Format(uint8_t(c.format)))) return ++census_.mismatch_view, ++census_.mismatch, v.why = KeepWhy::Mismatch, v;
  // The hot path (review C2): the entry's chain length and the provider's texture are cached per key while the provider's pointers stay valid.
  const uint32_t epoch = provider.Epoch();
  const bool cached = j.tex && j.tex_entry == entry && j.tex_epoch == epoch;
  uint32_t chain_mips = j.chain_mips;
  UiFileEntry fe;
  if (!cached) {
    if (!index_ || !index_->Get(entry, fe)) return ++census_.unjoined, v;
    chain_mips = fe.mips;
  }
  const uint32_t want = std::min<uint32_t>({chain_mips, max_level + 1, c.mips});   // the stream holds c.mips; its fetch reads up to max_level
  const UiTexture* t = nullptr;
  if (cached && j.tex_want == want) {
    t = j.tex;
  } else {
    if (fe.prefix.empty() && (!index_ || !index_->Get(entry, fe))) return ++census_.unjoined, v;
    bool pending = false;
    t = provider.Texture(entry, fe, want, pending);
    if (!t) {
      j.tex = nullptr;
      if (pending) return ++census_.pending, v.why = KeepWhy::NotReady, v;
      return ++census_.load_failed, ++census_.unjoined, v;
    }
    j.tex = t, j.tex_entry = entry, j.tex_want = want, j.chain_mips = chain_mips, j.tex_epoch = epoch;
  }
  t->last_used = frame_;
  // The per-draw checks: ours must be the stream's picture in size, format, mip count and bytes (OUR upload's hash against the stream content's), else the stream's draw
  // stays and the key's join is dropped until its content changes.
  const auto drop = [&](uint64_t& counter) -> Verdict {
    ++counter, ++census_.mismatch;
    j.entry = -1, j.tex = nullptr;
    v.why = KeepWhy::Mismatch;
    return v;
  };
  uint64_t* failed = nullptr;
  if (t->width != c.width || t->height != c.height || UiByteClass(t->format) != UiByteClass(rdr::Format(uint8_t(c.format)))) failed = &census_.mismatch_size;
  else if (!UiSwizzleMatches(t->format, rdr::Format(uint8_t(c.format)), swizzle)) failed = &census_.mismatch_swizzle;
  else if (c.mips == 0 || t->mips != want) failed = &census_.mismatch_mips;   // (ours holds exactly the mips the slot reads)
  else if (t->prefix.size() < c.mips || t->prefix[c.mips - 1] != c.hash) failed = &census_.mismatch_bytes;
  if (failed) {
    if (!faults_.no_checks) return drop(*failed);
    ++*failed, ++census_.unchecked;   // (the "unchecked_" plants: the check fired but the draw is replaced anyway, so the planted picture shows what the check protects)
  }
  v.ours = true, v.texture = t, v.entry = entry, v.declared = j.declared;
  ++census_.replaced;
  return v;
}

std::string UiCore::Report(const UiFileIndex& index) const {
  const UiCensus& c = census_;
  char line[900];
  std::snprintf(line, sizeof line,
                "replaced %llu, kept %llu (unjoined %llu, mismatch %llu, movie %llu, gpu %llu, untextured %llu, pending %llu, nokey %llu), keys joined %llu (declared %llu, by content %llu), "
                "changed after join %llu (rejoined %llu), mismatch size %llu view %llu swizzle %llu mips %llu bytes %llu key %llu, load failed %llu, unchecked %llu, %s",
                (unsigned long long)c.replaced, (unsigned long long)(c.claimed - c.replaced), (unsigned long long)c.unjoined, (unsigned long long)c.mismatch, (unsigned long long)c.movie,
                (unsigned long long)c.gpu, (unsigned long long)c.untextured, (unsigned long long)c.pending, (unsigned long long)c.nokey,
                (unsigned long long)(c.joined_declared + c.joined_content), (unsigned long long)c.joined_declared, (unsigned long long)c.joined_content,
                (unsigned long long)c.changed_after_join, (unsigned long long)c.rejoined, (unsigned long long)c.mismatch_size, (unsigned long long)c.mismatch_view, (unsigned long long)c.mismatch_swizzle, (unsigned long long)c.mismatch_mips,
                (unsigned long long)c.mismatch_bytes, (unsigned long long)c.mismatch_key, (unsigned long long)c.load_failed, (unsigned long long)c.unchecked, index.Summary().c_str());
  return line;
}

}  // namespace rdr_scene
