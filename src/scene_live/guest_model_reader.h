// The guest model reader (Session 103, lanes CW-2 and CW-2b; docs\research\s103\PCW_adversary.md "The readable path" and CW2_review.md, the SOURCE OF TRUTH, which correct PCW_caw_wrestler.md).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// Pure: no game, SDK or renderer includes (same-folder headers only). What it does: given a model object `m` (r3 at the model-entry hook 0x82C04E1C in sub_82C04DF0, the same object the part draw
// sub_82BDED40 reads) it follows the game's own finished geometry to the bytes: per part, the vertex flag word F, the vertex bytes of stream 0, the stride, the strip index buffer and the draw
// ranges, the part's palette (slot -> skeleton node), its effect name and its sampler textures' keys. The game build supplies the memory as `M` (u32(addr, out): the big-endian guest word as a host
// value; bytes(addr, n, dst): raw guest bytes). Hard rule 1: what this reads lives only in the running process and in the local scene records under out\ (never committed); the unit forges its memory.
//
// The path [C: PCW_adversary.md section 1, listings 0x82BDED40-0x82BDEF38 / sub_82BD7790 / sub_82BD5538 / sub_82CACD88 / sub_82CACE60; CW2_review.md]:
//   part count  = [m+372]   [C by listing: sub_82BDB460 stores [descobj+16] at [m+372] (0x82BDB4D8..)]
//   desc        = [m+824] + p*184   (the part descriptor IS the jboy mesh record: palette count [desc+8], palette [desc+12+4k] (1-based node numbers), vertices [desc+0], effect name [desc+124] (16),
//                 parameter count [desc+148], parameter pointer array [desc+152] [C: src\assets\jboy.h 398-406, 625-665 read the same record from the file]; when [m+824] == 0 the model is unreadable here and counted)
//   F, stride   = [[m+360]+8p] and [[m+360]+8p+4]   (stores 0x82BDC2AC-0x82BDC2C0; F is the packer sub_82BD4BE0's switch: the vertex is decoded from F, never from the stride)
//   vertices    = [desc+0];   stream 0 hdr = [[[m+336]+4p]+0]
//   data        = [hdr+24] & ~3 (low bits = fetch type)     bytes = [hdr+28] & 0x03FFFFFC       endian 8-in-32: every dword is byte-swapped (the guest holds big-endian dwords)
//   index hdr   = [[m+344]+4p]      data = [ihdr+24]      bytes = [ihdr+28]       16-bit big-endian, triangle strip (primitive type 6), 0xFFFF restart
//   ranges      = n0 = u16[[m+376]+2p]; when [m+548] != 0 also n1 = u16[[m+380]+2p] from index n0 (the part is drawn twice)
//   samplers    = every parameter of kind 15 (u16 at +16): value t = [param+20] (-1 none, < [m+256]); key = the 8 bytes at [m+260]+8t, word 0 = the registry handle (sub_82294F20)
//   override    = [m+832] -> [obj+1132] entries (20 bytes: value ptr +0, name ptr +8), count [obj+1136]: [L] where CEditEx composite textures enter; the value is read as a pointer to a key [U] (a handle that does
//                 not resolve in the registry changes nothing: it falls back to the model's own key)
//   extra streams: only when [m+232] != 0 and [[m+232] + [desc+96]*20 + 16] != 0 (an owned array of 20-byte objects, the remap of the extra 12-byte streams): a HOLE, counted (CW2_review item 8)
//   alt arrays  : [m+340]/[m+364] replace [m+336]/[m+360] for stream s when [m+348] != 0 and bit s of [m+352] is set: no code sets +348 (adversary 1b [L]): a part that asks for it is unreadable, counted
// Memory is reused after a model is destroyed (src\vb_unlock_report.h): the bytes are keyed by CONTENT hash, never by address (a cache by address is the planted twin); a model's cached read is reused only while
// its layout words are unchanged AND none of its buffers was unlocked since (buffer_unlock_log.h) AND the forced re-read period has not come (guest_model_contrib.h).
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "buffer_unlock_log.h"
#include "safe_guest_pages.h"   // S108 RDS: LineCopy (the layout pass's line copy)
#include "skin_data.h"   // S103 WC R3: kSkinData* (the +220 data object, shared)

namespace rdr_scene::live::guest_model {

using skin::kSkinDataBlockGetterFn;
using skin::kSkinDataBlockGetterOff;
using skin::kSkinDataBlockOff;
using skin::kSkinDataPartCountOff;
using skin::kSkinDataPartsOff;
using skin::kSkinForwardOff;

inline constexpr uint32_t kOffExtra = 232, kOffTexCount = 256, kOffTexKeys = 260, kOffVB = 336, kOffAltOn = 348, kOffAltMask = 352, kOffIB = 344, kOffStride = 360, kOffPartCount = 372, kOffN0 = 376, kOffN1 = 380,
                          kOffRangeTwo = 548, kOffDesc = 824, kOffOverride = 832, kOvArray = 1132, kOvCount = 1136;
// S105 A5c (docs\research\s105\A5c.md items 1-2 [C]): the CAW model's logo callback. sub_822E6DE8 stores the callback sub_822E6CC8 at [model+324] and the editor object E at [model+328]; the model draw sub_82C04DF0 calls it at
// 0x82C09810; it reads composer = [E+2608], buf = [composer+100], set = [composer+116+4*buf], arr = [set+4] and binds the picture of arr+0 at fetch slot 7 and of arr+8 at slot 4 (each entry: word 0 = the registry handle, word 1).
inline constexpr uint32_t kOffLogoCb = 324, kOffLogoArg = 328, kEditComposer = 2608, kCompBuf = 100, kCompLogoSet = 116, kLogoCb = 0x822E6CC8u, kLogoSetArr = 4, kLogoEntry = 8;
inline constexpr uint32_t kDescStride = 184, kDescVertices = 0, kDescPalCount = 8, kDescPal = 12, kDescExtraIndex = 96, kDescEffect = 124, kDescParamCount = 148, kDescParams = 152;
inline constexpr uint32_t kMaxParts = 64, kMaxVertices = 1u << 20, kMaxStride = 256, kMaxVertexBytes = 32u << 20, kMaxIndices = 1u << 20, kMaxNodes = 20, kMaxParams = 64, kMaxSamplers = 6, kMaxOverrides = 64, kNameBytes = 16, kOvDump = 12;
inline constexpr uint32_t kParamSampler = 15;

// Test only (tools\scene\guest_model_unit.cpp): each plant puts back one fault a forged-memory check must catch. 0 = none.
//   1 stride from [..+8p] (the wrong word)   2 data address not masked (the fetch-type bits stay)   3 the byte count not masked (bit 28 stays)
//   4 the strip's restart is not handled (StripsToList: 0xFFFF is a vertex)   5 n1 ignored (the second range dropped)   6 a cache by guest address (AddressCachedReader::Read returns the last result of an address)
//   7 no endian swap of the vertex dwords   8 the index is read little-endian   9 the palette is not read (every slot is its own node)   10 F is not read (a stride-only decode: Why::Ok whatever F says)
//   11 the override block is never matched (the model's own key always wins)   12 the Planner ignores the unlock log (a changed buffer is not re-read)   13 the Planner reads a named model too (the filter)
// S105 A5c, test only: 1 = the callback word is not checked (any model's [m+328] is followed); 2 = the composer's buffer index is not bounded (buf >= 2 is read); 3 = a logo key whose word 0 is 0 is accepted.
inline int g_gm_logo_plant = 0;
inline int g_gm_plant = 0;   // 14 (WC R3): the [m+824] == 0 path is not taken (a model whose descriptors live in the data object is NoDesc again)

enum class Why : uint8_t { Ok = 0, NoModel, NoDesc, NoParts, Unreadable, BadSize, BadIndexBuffer, AltStream, BadFlags, Count };
inline const char* WhyText(Why w) {
  static const char* t[] = {"ok", "no model", "no part descriptor ([m+824] is 0)", "no parts", "guest memory unreadable", "size out of range", "index buffer unusable", "alternate stream arrays (unsupported)",
                            "vertex flag word F unknown or not matching the stride"};
  return uint32_t(w) < uint32_t(Why::Count) ? t[uint32_t(w)] : "?";
}

enum PartFlags : uint32_t { kPartN1 = 1, kPartExtraStreams = 2 };

// A word-wide hash (8 bytes a step; the byte-serial FNV was ~2 ms a frame for a whole cast, CW2_review bug 1).
inline uint64_t Hash64(const void* p, size_t n, uint64_t h = 0x9E3779B97F4A7C15ull) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  while (n >= 8) {
    uint64_t v;
    std::memcpy(&v, b, 8);
    h = (h ^ v) * 0xFF51AFD7ED558CCDull;
    h ^= h >> 32;
    b += 8, n -= 8;
  }
  if (n) {
    uint64_t v = 0;
    std::memcpy(&v, b, n);
    h = (h ^ v ^ (uint64_t(n) << 56)) * 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 29;
  }
  return h;
}
inline uint64_t HashU32(uint32_t v, uint64_t h) { return Hash64(&v, 4, h); }

// ---- the vertex declaration from F [C: sub_82BD4BE0 listing, every F test: 0xC, 0xA, 8, 6, 0x10, 0x10000, 0x40, 0xF00; CW2_review bug 5] -----------------------------------------------------------
// Dword order of one vertex: pos x y z 1.0 | skin | normal | tangent | colour | uv sets (half2 each). The skin block: A (F&0xC == 0xC) w0 w1 w2 + slots(s0..s3); B (F&0xA == 0xA) w0 w1 0 + (s0 s1 s2 255);
// C (F&8) w0 0 0 + (s0 s1 255 255); D (F&6 == 6) ONE dword (255 255 255 s0 in byte order: slot k = byte k of the host dword); none otherwise.
// Bit 12 (0x1000) is in the game's F build (`| 0x1050`, 0x82BDC1B8) but no packer branch reads it: it adds no vertex data (the stride cross-check below refuses a part where it, or any other bit, does).
enum class SkinKind : uint8_t { None = 0, Pairs4, Pairs3, Pairs2, Rigid };
struct VLayout {
  bool ok = false;
  const char* why = "";
  SkinKind skin = SkinKind::None;
  uint32_t dwords = 4;                 // pos x y z w
  uint32_t weights_at = 0, slots_at = 0;
  int normal_at = -1, tangent_at = -1, colour_at = -1, uv_at = -1;
  uint32_t uv_sets = 0;
  uint32_t Stride() const { return dwords * 4; }
};
inline constexpr uint32_t kFKnown = 0xEu | 0x10u | 0x40u | 0xF00u | 0x10000u, kFDeclOnly = 0x1000u;
inline VLayout LayoutOfFlags(uint32_t F, bool refuse_unknown = true) {
  VLayout l;
  if (refuse_unknown && (F & ~(kFKnown | kFDeclOnly))) return l.why = "unknown vertex flag bits", l;
  if ((F & 0xC) == 0xC) l.skin = SkinKind::Pairs4;
  else if ((F & 0xA) == 0xA) l.skin = SkinKind::Pairs3;
  else if (F & 8) l.skin = SkinKind::Pairs2;
  else if ((F & 6) == 6) l.skin = SkinKind::Rigid;
  if (l.skin == SkinKind::Rigid) {
    l.slots_at = l.dwords;
    l.dwords += 1;
  } else if (l.skin != SkinKind::None) {
    l.weights_at = l.dwords;
    l.slots_at = l.dwords + 3;
    l.dwords += 4;
  }
  if (F & 0x10) l.normal_at = int(l.dwords++);
  if (F & 0x10000) l.tangent_at = int(l.dwords++);   // the space is reserved even when [desc+116] == 0 (the dword is then not written)
  if (F & 0x40) l.colour_at = int(l.dwords++);
  const uint32_t n = (F >> 8) & 0xFu;
  if (n > 4 && refuse_unknown) return l.why = "uv set count above 4", l;
  if (n >= 1 && n <= 4) l.uv_at = int(l.dwords), l.uv_sets = n, l.dwords += n;
  l.ok = true;
  return l;
}

struct SamplerRead {
  char name[kNameBytes] = {};
  int32_t t = -1;
  uint32_t key = 0, key1 = 0;       // the model's own key (word 0 = the registry handle)
  uint32_t ov_key = 0;              // the override block's handle for this name, 0 = none
  bool has_override = false;
};

struct PartRead {
  uint32_t part = 0;
  Why why = Why::Ok;
  uint32_t vertex_count = 0, stride = 0;   // stride in BYTES
  uint32_t vflags = 0;                     // F
  uint32_t n0 = 0, n1 = 0;                 // the draw's index ranges: [0, n0) and, when kPartN1, [n0, n0 + n1)
  uint32_t flags = 0;
  uint32_t desc_extra = 0;                 // [desc+96]: the extra-stream remap object index (census)
  uint32_t node_count = 0;
  uint16_t nodes[kMaxNodes] = {};          // slot -> node ([desc+12+4k] - 1), 0xFFFF = none
  char effect[kNameBytes] = {};            // [desc+124]
  std::vector<SamplerRead> samplers;       // every kind-15 parameter with a usable key (up to kMaxSamplers)
  uint32_t sampler_overflow = 0;           // kind-15 parameters beyond kMaxSamplers
  uint32_t vdata = 0, idata = 0;           // the guest data addresses (the unlock log's keys)
  std::shared_ptr<const std::vector<uint8_t>> vtx;   // vertex_count * stride bytes, dwords in HOST order (the 8-in-32 swap applied): little-endian values
  std::vector<uint16_t> idx;               // n0 + n1 host indices
  uint64_t vtx_hash = 0, idx_hash = 0;     // content hashes (vertex bytes + stride + count + F; index values + ranges)
  uint32_t max_index = 0;                  // the largest index that is not 0xFFFF (census; > vertex_count - 1 = a bad index: counted, the drawer drops that triangle)
  uint32_t restarts = 0;                   // 0xFFFF markers
  bool Ok() const { return why == Why::Ok; }
};

// S105 A5c: the two logo pictures of a CAW model (index 0 = entry 0 "logo", bound at fetch slot 7; index 1 = entry 1 "logo2", slot 4). `has` = the model's [m+324] is the callback and the words read.
struct LogoRead {
  bool has = false;
  uint32_t key[2] = {}, key1[2] = {};   // the registry handle (word 0) and word 1 of each entry
};

struct TexRead {   // one distinct picture some part names (filled by the contributor's resolver: guest_model_tex.h)
  uint64_t hash = 0;
  uint32_t width = 0, height = 0, format = 0;
  std::shared_ptr<const std::vector<uint8_t>> bytes;
};

// CTF (S103, docs\research\s103\CTEX.md site 4): what one refused sampler looked like, for the one live run that names the cause. Raw words as the game holds them (host order); 0 = not readable.
struct RefusedProbe {
  uint8_t part = 0, sampler = 0, why = 0;   // why: kWhyNoKey (9, guest_model_tex.h) or a guest_tex::Why
  bool ov = false;                          // the words are the OVERRIDE block's (composite) handle, not the model's own key
  uint32_t key0 = 0, key1 = 0, stored_id = 0, wrapper = 0, vtable = 0, kind = 0, header = 0, fetch[6] = {};
};
inline constexpr uint32_t kMaxRefusedProbes = 8;
struct ModelRead {
  uint32_t model = 0, parts = 0;
  Why why = Why::Ok;
  uint64_t hash = 0;                       // over the readable parts' hashes in part order (and the unreadable parts' numbers)
  uint64_t layout_key = 0;                 // over every word the layout pass read (no byte content): equal = same declaration
  uint32_t parts_ok = 0, parts_unreadable = 0, parts_extra_streams = 0, parts_alt = 0, parts_bad_flags = 0, bad_indices = 0;
  uint32_t samplers = 0, sampler_overflow = 0, ov_entries = 0, ov_matched = 0, ov_matched_ci = 0;   // ov_matched_ci: entries that equal a sampler name only when the case is ignored (CTX2: the lead for "0 of 11")
  uint32_t ov_array = 0, ov_count = 0;     // CTX2: the override block's entry array and count (0 = none); ReadModelBytes dumps up to kOvDump entries
  uint8_t ov_dump_n = 0;                   // CTX2: how many override entries ov_names / ov_handle0 hold (at most kOvDump)
  char ov_names[kOvDump][kNameBytes + 1] = {};
  uint32_t ov_handle0[kOvDump] = {};
  bool desc_via_d = false, parts_mismatch = false;   // S103 WC R3: the descriptors came from the data object (the [m+824] == 0 path); [H+16] != [m+372] (census)
  uint32_t d_vtable = 0;                              // S103 WC R3: the vtable of [m+220] when [m+824] == 0 (0 = not read); the class a live run names
  uint64_t vertices = 0, indices = 0, bytes_read = 0;
  std::vector<PartRead> p;
  // per part, per sampler: the resolved picture hash and its state (kGtMissing / kGtBound / kGtOverride), filled by the resolver; textures = the distinct pictures
  std::vector<std::vector<uint64_t>> tex_hash;
  std::vector<std::vector<uint8_t>> tex_state;
  std::vector<TexRead> textures;
  bool tex_8888 = false;                   // CTX2: some sampler was read from a k_8_8_8_8 page (the game may rewrite it in place: the resolver is asked again every kOverrideRefreshFrames, guarded by a sampled hash)
  bool tex_retry = false;                  // CTF: a sampler was refused for a reason a LATER call can cure (no handle yet, header / pixel memory not there yet): TexDue asks again about every kOverrideRefreshFrames
  uint8_t n_refused = 0;                   // CTF: the first kMaxRefusedProbes refused samplers' words (fixed array: no allocation)
  RefusedProbe refused[kMaxRefusedProbes];
  LogoRead logo;                           // S105 A5c: the model-level logo set (layout pass; its words are in layout_key)
  uint64_t logo_hash[2] = {};              // S105 A5c: the resolver's picture hash of each logo (0 = none); the pictures themselves are in `textures`
  uint8_t logo_state[2] = {};              // S105 A5c: kGtMissing / kGtBound
  bool tex_incomplete = false;             // some sampler has a key the resolver has not answered yet (deferred by its per-call budget): the next call asks again
};

namespace detail {
inline uint32_t Bswap(uint32_t v) { return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24); }
// A reader over M that folds every word it returns into the layout key.
// S108 RDS phase 2: every field of ONE layout pass goes through a pass-scoped line copy (safe_guest_pages.h LineCopy): the 128-byte aligned line that holds a field is read once per pass (one page check),
// the other fields of that line come from the copy. The line lies in the field's own page, so the verdicts are the field reads' (a refused line is the field read). The copy dies with the pass (Rd is a local
// of ReadModelLayout): the next call reads the game's memory again, so the layout key still sees every change. Was one page check per word: ~7,000 a frame in Fandango's entrance (S108 RDS tally, thread 8B28).
// Test only: g_gm_plant 15 = the window starts AT the field (unaligned: it can ask a page no field read asks); 16 = no line copy (one read per field: the cost check must fail).
template <class M>
struct Rd {
  const M& m;
  uint64_t key = 0x517CC1B727220A95ull;
  wwe2k14::safe_pages::LineCopy<> lines{};
  wwe2k14::safe_pages::LineCopy<1, true> lines_unaligned{};   // plant 15 only (one 128-byte window)
  bool Get(uint32_t a, uint32_t n, uint8_t* d) {
    const auto rd = [this](uint32_t x, uint32_t k, void* o) { return m.bytes(x, k, static_cast<uint8_t*>(o)); };
    if (g_gm_plant == 16) return rd(a, n, d);
    if (g_gm_plant == 15) return lines_unaligned.Get(a, n, d, rd);
    return lines.Get(a, n, d, rd);
  }
  bool Word(uint32_t a, uint32_t& v) {
    uint8_t b[4];
    if (!Get(a, 4, b)) return false;
    v = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];   // the guest's big-endian word (M::u32's value)
    key = HashU32(v, key);
    return true;
  }
  bool Half(uint32_t a, uint32_t& v) {   // a big-endian u16
    uint8_t b[2];
    if (!Get(a, 2, b)) return false;
    v = (uint32_t(b[0]) << 8) | b[1];
    key = HashU32(v, key);
    return true;
  }
  bool Name(uint32_t a, char* out) {   // 16 bytes, NUL padded
    uint8_t b[kNameBytes];
    if (!Get(a, kNameBytes, b)) return false;
    bool end = false;
    for (uint32_t i = 0; i < kNameBytes; ++i) {
      if (b[i] == 0) end = true;
      out[i] = end ? 0 : char(b[i]);
    }
    key = Hash64(out, kNameBytes, key);
    return true;
  }
};
inline bool NameEq(const char* a, const char* b) { return std::strncmp(a, b, kNameBytes) == 0; }
}  // namespace detail

// Reads ONE part's WORDS (everything but the vertex / index bytes). `p` < the model's part count, `desc` = the descriptor address of the part.
template <class M>
inline void ReadPartLayout(detail::Rd<M>& rd, uint32_t model, uint32_t p, uint32_t desc, bool two_ranges, uint32_t extra_array, uint32_t ov_entries, uint32_t ov_count, uint32_t tex_count, uint32_t tex_keys,
                           PartRead& r, ModelRead& mr) {
  using namespace detail;
  r.part = p;
  uint32_t vbs_arr = 0, hdr0 = 0, stride_arr = 0, vbs = 0, vcount = 0, stride = 0, bytes_raw = 0, F = 0;
  uint32_t alt_on = 0, alt_mask = 0;
  if (!rd.Word(model + kOffAltOn, alt_on) || !rd.Word(model + kOffAltMask, alt_mask)) { r.why = Why::Unreadable; return; }
  if (alt_on != 0 && (alt_mask & 1u)) { r.why = Why::AltStream; return; }   // stream 0 would use [m+340] / [m+364]: not read here
  if (!rd.Word(desc + kDescVertices, vcount) || !rd.Word(desc + kDescExtraIndex, r.desc_extra)) { r.why = Why::Unreadable; return; }
  if (!rd.Word(model + kOffVB, vbs_arr) || !vbs_arr || !rd.Word(vbs_arr + 4 * p, vbs) || !vbs || !rd.Word(vbs + 0, hdr0) || !hdr0) { r.why = Why::Unreadable; return; }
  if (!rd.Word(model + kOffStride, stride_arr) || !stride_arr) { r.why = Why::Unreadable; return; }
  if (!rd.Word(stride_arr + 8 * p + (g_gm_plant == 1 ? 0 : 4), stride) || !rd.Word(stride_arr + 8 * p + 0, F)) { r.why = Why::Unreadable; return; }
  if (!rd.Word(hdr0 + 24, r.vdata) || !rd.Word(hdr0 + 28, bytes_raw)) { r.why = Why::Unreadable; return; }
  if (g_gm_plant != 2) r.vdata &= ~3u;
  const uint32_t bytes = (g_gm_plant == 3) ? bytes_raw : (bytes_raw & 0x03FFFFFCu);
  if (vcount == 0 || vcount > kMaxVertices || stride < 4 || stride > kMaxStride || (stride & 3)) { r.why = Why::BadSize; return; }
  const uint64_t need = uint64_t(vcount) * stride;
  // the header's size is the buffer the game made for exactly vcount * stride (sub_82BD7790 -> sub_82CACD88(stride * count)); a slack over a page is another buffer: refuse (bit 28 left in the size is plant 3)
  if (need > kMaxVertexBytes || need > bytes || bytes - need > 4096 || !r.vdata) { r.why = Why::BadSize; return; }
  // the declaration: F decides every field; the stride must be exactly what F makes (an F the decoder does not know, or one that disagrees with the stride, is refused: never a guess)
  r.vflags = F;
  if (g_gm_plant != 10) {
    const VLayout lay = LayoutOfFlags(F);
    if (!lay.ok || lay.Stride() != stride) { r.why = Why::BadFlags; return; }
  }
  r.vertex_count = vcount, r.stride = stride;
  // extra streams (counted, not read)
  if (extra_array) {
    uint32_t gate = 0;
    if (rd.Word(extra_array + r.desc_extra * 20 + 16, gate) ? gate != 0 : true) r.flags |= kPartExtraStreams;   // unreadable gate = conservatively "has extra streams"
  }
  // the palette, the effect name and the samplers (the descriptor is the jboy mesh record)
  uint32_t npal = 0;
  if (!rd.Word(desc + kDescPalCount, npal)) { r.why = Why::Unreadable; return; }
  r.node_count = std::min(npal, kMaxNodes);
  for (uint32_t k = 0; k < r.node_count; ++k) {
    uint32_t pv = 0;
    if (!rd.Word(desc + kDescPal + 4 * k, pv)) { r.why = Why::Unreadable; return; }
    r.nodes[k] = (g_gm_plant == 9) ? uint16_t(k) : (pv == 0 || pv > 0xFFFE ? uint16_t(0xFFFF) : uint16_t(pv - 1));
  }
  if (!rd.Name(desc + kDescEffect, r.effect)) { r.why = Why::Unreadable; return; }
  uint32_t pc = 0, parr = 0;
  if (rd.Word(desc + kDescParamCount, pc) && rd.Word(desc + kDescParams, parr) && pc && pc <= kMaxParams && parr) {
    for (uint32_t i = 0; i < pc; ++i) {
      uint32_t po = 0, kind = 0, tv = 0;
      if (!rd.Word(parr + 4 * i, po) || !po || !rd.Half(po + 16, kind)) break;
      if (kind != kParamSampler) continue;
      SamplerRead s;
      if (!rd.Name(po, s.name) || !rd.Word(po + 20, tv)) break;
      s.t = int32_t(tv);
      if (s.t < 0) continue;   // -1: no texture set
      if (uint32_t(s.t) >= tex_count || !tex_keys) continue;   // beyond the model's texture table: not a picture
      if (!rd.Word(tex_keys + 8 * uint32_t(s.t), s.key) || !rd.Word(tex_keys + 8 * uint32_t(s.t) + 4, s.key1)) continue;
      // the override block, by the parameter's name
      if (ov_entries && g_gm_plant != 11)
        for (uint32_t j = 0; j < ov_count; ++j) {
          uint32_t vp = 0, np = 0;
          char nm[kNameBytes];
          if (!rd.Word(ov_entries + 20 * j, vp) || !rd.Word(ov_entries + 20 * j + 8, np) || !np || !rd.Name(np, nm)) continue;
          if (!NameEq(nm, s.name)) {
            bool ci = true;   // CTX2: equal when the case is ignored? (counted, no behaviour change)
            for (uint32_t i = 0; i < kNameBytes && ci; ++i) {
              const char a = nm[i] >= 'A' && nm[i] <= 'Z' ? char(nm[i] + 32) : nm[i], b = s.name[i] >= 'A' && s.name[i] <= 'Z' ? char(s.name[i] + 32) : s.name[i];
              if (a != b) ci = false;
              else if (!a) break;
            }
            if (ci) ++mr.ov_matched_ci;
            continue;
          }
          ++mr.ov_matched;
          uint32_t h = 0;
          if (vp && rd.Word(vp, h) && h) s.ov_key = h, s.has_override = true;
          break;
        }
      if (r.samplers.size() < kMaxSamplers) r.samplers.push_back(s);
      else ++r.sampler_overflow;
    }
  }
  // indices (words)
  uint32_t ib_arr = 0, ihdr = 0, ibytes = 0, n0_arr = 0, n1_arr = 0;
  if (!rd.Word(model + kOffIB, ib_arr) || !ib_arr || !rd.Word(ib_arr + 4 * p, ihdr) || !ihdr || !rd.Word(ihdr + 24, r.idata) || !rd.Word(ihdr + 28, ibytes)) { r.why = Why::BadIndexBuffer; return; }
  if (!rd.Word(model + kOffN0, n0_arr) || !n0_arr || !rd.Half(n0_arr + 2 * p, r.n0)) { r.why = Why::BadIndexBuffer; return; }
  if (two_ranges && g_gm_plant != 5) {
    if (!rd.Word(model + kOffN1, n1_arr) || !n1_arr || !rd.Half(n1_arr + 2 * p, r.n1)) { r.why = Why::BadIndexBuffer; return; }
    r.flags |= kPartN1;
  }
  const uint64_t total = uint64_t(r.n0) + r.n1;
  if (!r.idata || total == 0 || total > kMaxIndices || total * 2 > ibytes) { r.why = Why::BadIndexBuffer; return; }
}

// Reads the bytes of one part whose layout was read (the memory is read here, now: never cached by address across calls).
template <class M>
inline void ReadPartBytes(const M& m, PartRead& r, uint64_t& bytes_read) {
  using namespace detail;
  if (!r.Ok()) return;
  const uint64_t need = uint64_t(r.vertex_count) * r.stride, total = uint64_t(r.n0) + r.n1;
  auto vb = std::make_shared<std::vector<uint8_t>>(size_t(need));
  if (!m.bytes(r.vdata, uint32_t(need), vb->data())) { r.why = Why::Unreadable; return; }
  if (g_gm_plant != 7)
    for (size_t i = 0; i + 4 <= vb->size(); i += 4) {
      uint32_t v;
      std::memcpy(&v, vb->data() + i, 4);
      v = Bswap(v);
      std::memcpy(vb->data() + i, &v, 4);
    }
  std::vector<uint8_t> ib(size_t(total) * 2);
  if (!m.bytes(r.idata, uint32_t(ib.size()), ib.data())) { r.why = Why::Unreadable; return; }
  r.idx.resize(size_t(total));
  r.max_index = 0, r.restarts = 0;
  for (size_t i = 0; i < r.idx.size(); ++i) {
    const uint16_t v = (g_gm_plant == 8) ? uint16_t(ib[2 * i] | (ib[2 * i + 1] << 8)) : uint16_t((ib[2 * i] << 8) | ib[2 * i + 1]);
    r.idx[i] = v;
    if (v == 0xFFFF) ++r.restarts;
    else r.max_index = std::max<uint32_t>(r.max_index, v);
  }
  uint64_t h = HashU32(r.vertex_count, 0x1234), hi = HashU32(r.n0, 0x4321);
  h = HashU32(r.stride, h), h = HashU32(r.vflags, h), h = Hash64(vb->data(), vb->size(), h);
  hi = HashU32(r.n1, hi), hi = Hash64(r.idx.data(), r.idx.size() * 2, hi);
  r.vtx_hash = h, r.idx_hash = hi;
  r.vtx = std::move(vb);
  bytes_read += need + total * 2;
}

// Pass 1: the WORDS of a whole model (a few hundred guest reads, no byte run). `why != Ok` at model level = nothing to read.
template <class M>
inline ModelRead ReadModelLayout(const M& m, uint32_t model) {
  using namespace detail;
  ModelRead out;
  out.model = model;
  Rd<M> rd{m};
  uint32_t desc_base = 0, parts = 0, range_two = 0, extra = 0, ov = 0, ov_entries = 0, ov_count = 0, tex_count = 0, tex_keys = 0;
  if (!model) return out.why = Why::NoModel, out;
  if (!rd.Word(model + kOffDesc, desc_base)) return out.why = Why::Unreadable, out;
  uint32_t data_parts = 0;   // S103 WC R3: [H+16] when the descriptors come from the data object
  if (!desc_base && g_gm_plant != 14) {
    // S103 WC R3 (WNCC_adversary.md 4a-4c; the draw's own fallback, listing 0x82C05940-0x82C05978): [m+824] == 0 -> D = [m+220], vtable slot +60 = sub_827C17B8 (`return [D+12]`) = H; the descriptors are [H+20] + 184 p, p < [H+16].
    // D is read only when its class's +60 IS that getter (read, not assumed); anything else keeps NoDesc and its vtable goes to the census (d_vtable).
    uint32_t d = 0, dvt = 0, getter = 0, h = 0;
    if (rd.Word(model + kSkinForwardOff, d) && d) {
      if (rd.Word(d, dvt) && rd.Word(dvt + kSkinDataBlockGetterOff, getter)) {
        out.d_vtable = dvt;
        if (getter == kSkinDataBlockGetterFn && rd.Word(d + kSkinDataBlockOff, h) && h && rd.Word(h + kSkinDataPartsOff, desc_base) && rd.Word(h + kSkinDataPartCountOff, data_parts) && desc_base) out.desc_via_d = true;
        else desc_base = 0;
      }
    }
  }
  if (!desc_base) return out.why = Why::NoDesc, out;
  if (!rd.Word(model + kOffPartCount, parts)) return out.why = Why::Unreadable, out;
  if (out.desc_via_d && parts != data_parts) {   // [H+16] vs [m+372]: a census counter, not a refusal (the early-return path of sub_82BDB460 leaves [m+372] at the ctor's value); the descriptors only exist for p < [H+16]
    out.parts_mismatch = true;
    if (data_parts < parts) parts = data_parts;
  }
  if (parts == 0 || parts > kMaxParts) return out.why = Why::NoParts, out;
  if (!rd.Word(model + kOffRangeTwo, range_two) || !rd.Word(model + kOffExtra, extra)) return out.why = Why::Unreadable, out;
  rd.Word(model + kOffTexCount, tex_count), rd.Word(model + kOffTexKeys, tex_keys);   // (a model without a texture table has no pictures: not an error)
  {   // S105 A5c: the logo callback of a CAW model (item 1 hook check: [m+324] == the callback, buf < 2, readable words, key word 0 != 0, the game binds slot 4 only inside slot 7's resolve, sub_822E6CC8 +50: review F4)
    uint32_t cb = 0, arg = 0, comp = 0, buf = 0, set = 0, arr = 0;
    if (rd.Word(model + kOffLogoCb, cb) && (cb == kLogoCb || g_gm_logo_plant == 1) && rd.Word(model + kOffLogoArg, arg) && arg && rd.Word(arg + kEditComposer, comp) && comp && rd.Word(comp + kCompBuf, buf) &&
        (buf < 2 || g_gm_logo_plant == 2) && rd.Word(comp + kCompLogoSet + 4 * buf, set) && set && rd.Word(set + kLogoSetArr, arr) && arr) {
      uint32_t k[2], k1[2];
      if (rd.Word(arr, k[0]) && rd.Word(arr + 4, k1[0]) && rd.Word(arr + kLogoEntry, k[1]) && rd.Word(arr + kLogoEntry + 4, k1[1]) && (k[0] != 0 || g_gm_logo_plant == 3)) {
        out.logo.has = true;
        for (int i = 0; i < 2; ++i) out.logo.key[i] = k[i], out.logo.key1[i] = k1[i];
      }
    }
  }
  if (rd.Word(model + kOffOverride, ov) && ov) {
    rd.Word(ov + kOvArray, ov_entries), rd.Word(ov + kOvCount, ov_count);
    if (ov_count > kMaxOverrides) ov_entries = 0, ov_count = 0;
    out.ov_entries = ov_count;
    out.ov_array = ov_entries, out.ov_count = ov_count;   // CTX2: ReadModelBytes dumps the entries (the words-only pass stays as cheap as before)
  }
  out.parts = parts;
  out.p.resize(parts);
  for (uint32_t p = 0; p < parts; ++p) ReadPartLayout(rd, model, p, desc_base + p * kDescStride, range_two != 0, extra, ov_entries, ov_count, tex_count, tex_keys, out.p[p], out);
  for (const PartRead& r : out.p) {
    if (r.Ok()) {
      ++out.parts_ok;
      if (r.flags & kPartExtraStreams) ++out.parts_extra_streams;
      out.samplers += uint32_t(r.samplers.size()), out.sampler_overflow += r.sampler_overflow;
    } else {
      ++out.parts_unreadable;
      if (r.why == Why::AltStream) ++out.parts_alt;
      if (r.why == Why::BadFlags) ++out.parts_bad_flags;
    }
  }
  out.layout_key = rd.key;
  return out;
}

// Pass 2: the bytes of every readable part, the part and model hashes and the counts.
template <class M>
inline void ReadModelBytes(const M& m, ModelRead& out) {
  uint64_t h = HashU32(out.parts, 0xABCDEF);
  out.parts_ok = out.parts_unreadable = out.parts_extra_streams = out.parts_alt = out.parts_bad_flags = out.bad_indices = 0;
  out.vertices = out.indices = 0;
  // CTX2 (CAWD Q2 / run 012: "override block entries 11 matched 0"): the entries' names (+8) and first handle word (the word at the +0 pointer's target); the contributor prints them once, so ONE run says why none matched.
  // Plain reads, once per byte pass (a census, not part of the layout key).
  out.ov_dump_n = 0;
  for (uint32_t j = 0; out.ov_array && j < out.ov_count && j < kOvDump; ++j) {
    uint32_t vp = 0, np = 0, hw = 0;
    m.u32(out.ov_array + 20 * j, vp), m.u32(out.ov_array + 20 * j + 8, np);
    uint8_t nb[kNameBytes] = {};
    out.ov_names[j][0] = '\0';
    if (np && m.bytes(np, kNameBytes, nb))
      for (uint32_t i = 0; i < kNameBytes; ++i) out.ov_names[j][i] = (nb[i] >= 32 && nb[i] < 127) ? char(nb[i]) : (nb[i] ? '?' : '\0');
    if (vp) m.u32(vp, hw);
    out.ov_handle0[j] = hw, out.ov_dump_n = uint8_t(j + 1);
  }
  for (PartRead& r : out.p) {
    ReadPartBytes(m, r, out.bytes_read);
    if (r.Ok()) {
      ++out.parts_ok;
      out.vertices += r.vertex_count, out.indices += r.n0 + r.n1;
      if (r.flags & kPartExtraStreams) ++out.parts_extra_streams;
      if (r.vertex_count && r.max_index >= r.vertex_count) ++out.bad_indices;
      h = Hash64(&r.vtx_hash, 8, h), h = Hash64(&r.idx_hash, 8, h);
    } else {
      ++out.parts_unreadable;
      if (r.why == Why::AltStream) ++out.parts_alt;
      if (r.why == Why::BadFlags) ++out.parts_bad_flags;
      h = HashU32(r.part, h);
    }
  }
  out.hash = h;
}

// Reads a whole model: every part. Never throws; an unreadable part is counted and kept as a row with `why` (its number stays in the hash so a model whose readable set changed hashes differently).
template <class M>
inline ModelRead ReadModel(const M& m, uint32_t model) {
  ModelRead out = ReadModelLayout(m, model);
  if (out.why == Why::Ok) ReadModelBytes(m, out);
  return out;
}

// Plant 6 only: the cache by guest address a reader must never have. (The real path calls ReadModel directly.)
struct AddressCachedReader {
  std::vector<std::pair<uint32_t, ModelRead>> cache;
  template <class M>
  const ModelRead& Read(const M& m, uint32_t model) {
    if (g_gm_plant == 6)
      for (auto& kv : cache)
        if (kv.first == model) return kv.second;
    ModelRead r = ReadModel(m, model);
    for (auto& kv : cache)
      if (kv.first == model) return kv.second = std::move(r);
    cache.emplace_back(model, std::move(r));
    return cache.back().second;
  }
};

// ---- the per-call planner (CW2_review bug 1: read only what the record cannot name, and only on a change) -----------------------------------------------------------------------------------------
// One instance per process (the contributor owns it). `Update` is what the model-entry hook runs for a model: nothing for a model the wrestler class did not size (`want` false) or the record can name
// (`named` true); otherwise a layout pass (words only); then the byte pass ONLY when the layout key changed, a buffer of the model was unlocked since the last read (buffer_unlock_log.h: VB and IB
// unlock hooks), or the forced re-read period came (kForceRereadCalls calls: a safety net for an unlock the log missed; a forced re-read that finds different bytes is counted `missed_unlock`).
inline constexpr uint32_t kForceRereadCalls = 241;   // odd on purpose (the frame clock's periods are even); about 4 s of a model drawn every frame
struct PlannerStats {
  uint64_t calls = 0, skipped_unwanted = 0, skipped_named = 0, layout_reads = 0, byte_reads = 0, reused = 0, forced = 0, missed_unlock = 0, unreadable = 0, bytes_read = 0, states_dropped = 0;
};
class Planner {
 public:
  struct State {
    std::shared_ptr<ModelRead> read;
    uint64_t unlock_seq = 0;      // the unlock log's sequence BEFORE the bytes were read
    uint32_t calls_since = 0;
    uint64_t last_call = 0;
    uint64_t tex_checked = 0;     // the frame the resolver last ran for this model (the contributor's override refresh)
    uint64_t dirty_seen = 0;      // S105 A5c: guest_tex_dirty::Seq() at the resolver's last run for this model
  };
  // Returns the model's current read (shared with the state; null = nothing to hand over). `fresh` = the byte pass ran this call.
  template <class M>
  std::shared_ptr<ModelRead> Update(const M& m, uint32_t model, bool want, bool named, uint64_t frame, bool* fresh = nullptr, const unlocklog::Ring& ring = unlocklog::TheRing()) {
    ++st_.calls;
    if (fresh) *fresh = false;
    if (!want) return ++st_.skipped_unwanted, nullptr;
    if (named && g_gm_plant != 13) return ++st_.skipped_named, nullptr;   // (plant 13: the filter reads a named model too)
    State& s = states_[model];
    s.last_call = frame;
    ModelRead lay = ReadModelLayout(m, model);
    ++st_.layout_reads;
    if (lay.why != Why::Ok) return ++st_.unreadable, s.read = std::make_shared<ModelRead>(std::move(lay)), s.read;
    bool need = !s.read || s.read->why != Why::Ok || s.read->layout_key != lay.layout_key;
    bool forced = false;
    if (!need) {
      if (g_gm_plant != 12) {
        std::vector<uint32_t> addrs;
        for (const PartRead& p : s.read->p)
          if (p.vdata) addrs.push_back(p.vdata), addrs.push_back(p.idata);
        if (unlocklog::Dirty(addrs.data(), addrs.size(), s.unlock_seq, ring)) need = true;
      }
      if (!need && ++s.calls_since >= kForceRereadCalls) need = forced = true;
    }
    if (!need) return ++st_.reused, s.read;
    s.unlock_seq = unlocklog::Now(ring);   // BEFORE the bytes (anything unlocked after this makes the next call re-read)
    s.calls_since = 0;
    ReadModelBytes(m, lay);
    ++st_.byte_reads, st_.bytes_read += lay.bytes_read;
    if (forced) {
      ++st_.forced;
      if (s.read && s.read->hash != lay.hash) ++st_.missed_unlock;
    }
    // the resolver's work (textures) is kept when the model's content is the same: the contributor re-runs it for a new content / incomplete read
    s.read = std::make_shared<ModelRead>(std::move(lay));
    if (fresh) *fresh = true;
    return s.read;
  }
  // The models not called for `kIdleCalls` frames are forgotten (a model gone: its address may hold another body later, which is then a fresh state).
  void Prune(uint64_t frame, uint64_t idle_frames = 600) {
    for (auto it = states_.begin(); it != states_.end();) {
      if (frame > it->second.last_call && frame - it->second.last_call > idle_frames) it = states_.erase(it), ++st_.states_dropped;
      else ++it;
    }
  }
  const PlannerStats& stats() const { return st_; }
  size_t size() const { return states_.size(); }
  State* Find(uint32_t model) { auto it = states_.find(model); return it == states_.end() ? nullptr : &it->second; }
 private:
  std::unordered_map<uint32_t, State> states_;
  PlannerStats st_;
};

// A triangle strip with 0xFFFF restarts -> a triangle list (what the front end draws). Alternate winding per strip triangle (D3D order, as jboy StripToList); degenerates and triangles that name a
// vertex >= vcount are dropped and counted. Each range of a part ([0, n0) and [n0, n0 + n1)) is its own strip run. `ranges` = {n0, n1} (n1 = 0 for one range).
struct StripStats { uint32_t triangles = 0, degenerate = 0, bad = 0; };
inline StripStats StripsToList(const std::vector<uint16_t>& idx, uint32_t vcount, uint32_t n0, uint32_t n1, std::vector<uint32_t>& out) {
  StripStats st;
  const uint32_t starts[2] = {0, n0}, lens[2] = {n0, n1};
  for (int r = 0; r < 2; ++r) {
    size_t run = 0;
    const size_t base = starts[r], n = lens[r];
    if (base + n > idx.size()) break;
    for (size_t i = 0; i < n; ++i) {
      const uint16_t cur = idx[base + i];
      if (cur == 0xFFFF && g_gm_plant != 4) {
        run = 0;
        continue;
      }
      ++run;
      if (run < 3) continue;
      uint32_t a = idx[base + i - 2], b = idx[base + i - 1], c = cur;
      if (a == 0xFFFF && g_gm_plant != 4) continue;   // (cannot happen: a restart resets `run`)
      if (a >= vcount || b >= vcount || c >= vcount) {
        ++st.bad;
        continue;
      }
      if (a == b || b == c || a == c) {
        ++st.degenerate;
        continue;
      }
      if ((run & 1) == 0) std::swap(a, b);
      out.push_back(a), out.push_back(b), out.push_back(c);
      ++st.triangles;
    }
  }
  return st;
}

}  // namespace rdr_scene::live::guest_model
