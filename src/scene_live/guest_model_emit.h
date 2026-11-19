// A model read -> GuestModel rows (Session 103, lane CW-2b; split out of guest_model_contrib.h so the offline unit builds the REAL row writer). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Pure: no game or SDK includes. The rows carry hashes always, the bytes (blob section) for a hash NEW to the record session and again every kGmResendFrames (odd) frames; a row whose bytes an earlier record
// carried names kGmNoBytes and the consumer asks the process byte store by hash (guest_model_store.h).
#pragma once
#include <cstdint>
#include <cstring>
#include <unordered_map>

#include "guest_model_reader.h"
#include "../renderer_scene/guest_model_rows.h"

namespace rdr_scene::live::guest_model {

// Which byte runs went into a record lately (content-addressed: never by address).
class BlobSender {
 public:
  // The blob offset of `hash`'s bytes in `g`, or kGmNoBytes when they went out within kGmResendFrames frames.
  uint32_t Send(GuestModelTables& g, uint64_t hash, const void* data, size_t n, uint64_t frame) {
    auto it = sent_.find(hash);
    if (it != sent_.end() && frame - it->second < kGmResendFrames) return kGmNoBytes;
    sent_[hash] = frame;
    while (g.blobs.size() & 7) g.blobs.push_back(0);
    const uint32_t off = uint32_t(g.blobs.size());
    g.blobs.insert(g.blobs.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + n);
    return off;
  }
  void Prune(uint64_t frame) {
    for (auto s = sent_.begin(); s != sent_.end();) s = (frame > s->second + 8 * kGmResendFrames) ? sent_.erase(s) : std::next(s);
  }
  size_t size() const { return sent_.size(); }
 private:
  std::unordered_map<uint64_t, uint64_t> sent_;
};

// The signature of a read's pictures (hash + state per sampler): a changed picture is a new model key.
inline uint64_t TexSignature(const ModelRead& m) {
  uint64_t h = 0x51;
  for (size_t i = 0; i < m.tex_hash.size(); ++i) {
    for (uint64_t v : m.tex_hash[i]) h = Hash64(&v, 8, h);
    for (uint8_t v : m.tex_state[i]) h = HashU32(v, h);
  }
  return h;
}

// Appends the rows of `m` (every readable part) and its pictures to `g`. Returns the number of part rows added.
inline uint32_t AppendModelRows(const ModelRead& m, GuestModelTables& g, BlobSender& blobs, uint64_t frame) {
  uint32_t added = 0;
  if (!m.parts_ok) return 0;
  const uint64_t sig = TexSignature(m);
  for (const TexRead& tx : m.textures) {
    if (g.FindTexture(tx.hash) || !tx.bytes) continue;
    GuestModelTexture row;
    row.hash = tx.hash, row.width = tx.width, row.height = tx.height, row.format = tx.format, row.nbytes = uint32_t(tx.bytes->size());
    row.off = blobs.Send(g, tx.hash, tx.bytes->data(), tx.bytes->size(), frame);
    g.textures.push_back(row);
  }
  for (size_t pi = 0; pi < m.p.size(); ++pi) {
    const PartRead& p = m.p[pi];
    if (!p.Ok() || !p.vtx) continue;
    GuestModelPart row;
    row.model = m.model, row.part = p.part, row.parts = m.parts;
    row.model_hash = Hash64(&sig, 8, m.hash);   // the key the consumer builds under: geometry AND pictures (a composite texture rewritten = a new model)
    row.vtx_hash = p.vtx_hash, row.idx_hash = p.idx_hash, row.vertex_count = p.vertex_count, row.stride = p.stride;
    row.index_count = p.n0 + p.n1, row.n0 = p.n0, row.n1 = p.n1;
    row.flags = (p.n1 ? kGmN1 : 0u) | ((p.flags & kPartExtraStreams) ? kGmExtraStreams : 0u);
    row.vflags = p.vflags, row.node_count = p.node_count;
    for (uint32_t k = 0; k < p.node_count && k < kGmMaxNodes; ++k) row.nodes[k] = p.nodes[k];
    std::memcpy(row.effect, p.effect, kGmNameBytes);
    row.tex_overflow = p.sampler_overflow;
    for (size_t si = 0; si < p.samplers.size() && row.tex_count < kGmMaxTex; ++si) {
      GuestModelTex& tx = row.tex[row.tex_count++];
      std::memcpy(tx.name, p.samplers[si].name, kGmNameBytes);
      const uint8_t state = pi < m.tex_state.size() && si < m.tex_state[pi].size() ? m.tex_state[pi][si] : uint8_t(kGtMissing);
      tx.key = (state == kGtOverride) ? p.samplers[si].ov_key : p.samplers[si].key;
      tx.state = state;
      tx.hash = pi < m.tex_hash.size() && si < m.tex_hash[pi].size() ? m.tex_hash[pi][si] : 0;
    }
    row.vtx_off = blobs.Send(g, p.vtx_hash, p.vtx->data(), p.vtx->size(), frame);
    row.idx_off = blobs.Send(g, p.idx_hash, p.idx.data(), p.idx.size() * 2, frame);
    g.parts.push_back(row);
    ++added;
  }
  return added;
}

}  // namespace rdr_scene::live::guest_model
