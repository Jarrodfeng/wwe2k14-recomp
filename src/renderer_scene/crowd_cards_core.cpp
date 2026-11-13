// The crowd's far cards, drawn by us: the GPU-free half. See crowd_cards_core.h and docs\renderer\CROWD_CARDS.md.
#include "crowd_cards_core.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#include "crowd_fast.h"   // lane 270
#include "crowd_phase.h"
#include "../renderer_adapter/crowd_hook.h"  // kCrowdBlocksSite / kCrowdCardsSite

namespace rdr_scene::cards {
namespace {
enum BPh : size_t { kBRecord, kBStream, kBStateLoop, kBQuad, kBBakes, kBThin, kBPairing, kBJoin, kBOurs, kBInst, kBTail };
const char* const kBNames[] = {"1 record scan", "2 stream scan", "2b state loop", "2c quad read", "3 bake ops", "3b thin", "4 pairing", "4b stream join", "5 ours", "6-7 instances", "tail"};
PhaseClock g_bph;   // lane 270 (env RDR_CROWD_PHASES=1)
bool g_bph_init = false;

uint64_t HashWords(const uint32_t* w) {  // FNV-1a over the 12 words of c76-c78
  uint64_t h = 0xcbf29ce484222325ull;
  for (int k = 0; k < 12; ++k) h = (h ^ w[k]) * 0x100000001b3ull;
  return h;
}

uint64_t HashInstance(const Instance& i) {
  uint32_t w[12];
  std::memcpy(w, i.c, sizeof w);
  return HashWords(w);
}

struct RecordCard {
  Instance inst;
  uint64_t hash = 0;
  uint32_t type = 0;
  uint32_t pair = 0xFFFFFFFFu;  // index into the stream cards
};

const rdr_adapter::TextureSlot* SlotEntry(const rdr_adapter::LoadedFrame& fr, const rdr_adapter::Draw& d, uint32_t slot) {
  for (uint32_t k = 0; k < d.texture_count; ++k)
    if (fr.texture_slots[d.texture_first + k].slot == slot) return &fr.texture_slots[d.texture_first + k];
  return nullptr;
}

struct StreamCard {
  uint32_t draw = 0;
  uint64_t hash = 0;
  uint32_t image0 = 0, image13 = 0, image16 = 0;
  uint32_t seq = 0xFFFFFFFFu;   // position in the final instance list (drawn cards only)
};

uint32_t SlotImage(const rdr_adapter::LoadedFrame& fr, const rdr_adapter::Draw& d, uint32_t slot) {
  for (uint32_t k = 0; k < d.texture_count; ++k) {
    const rdr_adapter::TextureSlot& ts = fr.texture_slots[d.texture_first + k];
    if (ts.slot == slot) return ts.image;
  }
  return 0;
}

// A record asset entry as a person type: "card:TT" (cards, returned as card = true) or a stands body "20000 + 40 * lod + type" (blocks).
bool EntryType(const SceneTables& t, const Object& o, bool& card, uint32_t& type) {
  if (o.asset >= t.assets.size()) return false;
  const std::string_view e = t.Str(t.assets[o.asset].entry);   // lane 270: read in place (it was a std::string copy + two sscanf calls per object, ~1400 objects a frame)
  uint32_t a = 0;
  if (e.size() > 5 && e.compare(0, 5, "card:") == 0 && fast::ParseUInt(e, 5, a)) {
    card = true, type = a;
    return a < kTypes;
  }
  if (fast::ParseUInt(e, 0, a) && a >= 20000 && a < 20000 + 3 * 40) {
    card = false, type = (a - 20000) % 40;
    return type < kTypes;
  }
  return false;
}

}  // namespace

void CardQuad(uint8_t out[64]) {
  struct V { float pos[3]; uint16_t u, v; };
  const V quad[4] = {{{+82.f, 0.f, 0.f}, 0x3C00, 0x3C00}, {{-82.f, 0.f, 0.f}, 0, 0x3C00}, {{+82.f, -250.f, 0.f}, 0x3C00, 0}, {{-82.f, -250.f, 0.f}, 0, 0}};
  static_assert(sizeof(V) == 16, "the blob's vertex is 16 bytes");
  std::memcpy(out, quad, 64);
}

int CardType(const SceneTables& t, const Object& o) {
  bool card = false;
  uint32_t type = 0;
  return EntryType(t, o, card, type) && card ? int(type) : -1;
}

std::string PhaseReport() { return g_bph.Report("cards BuildPlan", "calls"); }

void BuildPlan(const Input& in, Plan& plan) {
  if (!g_bph_init) g_bph_init = true, g_bph.Init(kBNames, sizeof kBNames / sizeof kBNames[0]);
  g_bph.Begin();
  struct PhEnd { ~PhEnd() { g_bph.Count(1); g_bph.End(kBTail); } } ph_end;
  {  // lane 270: the plan's two big arrays keep their memory from frame to frame
    std::vector<Instance> keep_instances;
    std::vector<Run> keep_runs;
    keep_instances.swap(plan.instances), keep_runs.swap(plan.runs);
    plan = Plan{};
    plan.instances.swap(keep_instances), plan.runs.swap(keep_runs);
    plan.instances.clear(), plan.runs.clear();
  }
  if (!in.frame || !in.record) return void(plan.why = "no frame or record");
  const rdr_adapter::LoadedFrame& fr = *in.frame;
  const SceneTables& rec = *in.record;

  // 1. The record: the visible cards, in the record's order; and the world -> type of every other crowd body (the blocks' join).
  thread_local std::vector<RecordCard> cards;   // lane 270: scratch kept across frames (this function is pure: nothing here outlives a call)
  thread_local fast::FlatU64Map body_type;
  cards.clear();
  body_type.Reset(rec.objects.size() + 64);
  for (const Object& o : rec.objects) {
    if (o.klass != rdr_adapter::DrawClass::Crowd || !(o.flags & kObjVisible)) continue;
    bool card = false;
    uint32_t type = 0;
    if (!EntryType(rec, o, card, type)) continue;
    RecordCard rc;
    for (int r = 0; r < 3; ++r)
      for (int k = 0; k < 4; ++k) rc.inst.c[r * 4 + k] = o.world.m[k * 4 + r];
    rc.hash = HashInstance(rc.inst);
    rc.type = type;
    if (card) cards.push_back(rc);
    else body_type.emplace(rc.hash, type);
  }
  if (in.fault == Fault::MissingCard && !cards.empty()) cards.erase(cards.begin());
  if (in.fault == Fault::NoCards) cards.clear();
  plan.record_cards = uint32_t(cards.size());
  for (const RecordCard& c : cards) plan.record_world_sum += c.hash;

  g_bph.Lap(kBRecord);
  // 2. The stream: the card draws (op order), and the textures the type's stands blocks bind (image -> type).
  thread_local std::vector<StreamCard> stream;
  stream.clear();
  std::unordered_map<uint32_t, uint32_t> image_type;  // a stands block's slot-0 image -> its person type (kAmbiguous when two types claim it)
  thread_local std::vector<uint8_t> in_ops;
  in_ops.assign(fr.draws.count, 0);  // lane 175: the stream is what the op list DRAWS (a thin capture's cut draws keep their table rows; a live frame's table holds only recorded draws)
  for (uint32_t k = 0; k < fr.ops.count; ++k)
    if (fr.ops[k].kind == rdr_adapter::OpKind::Draw && fr.ops[k].index < fr.draws.count) in_ops[fr.ops[k].index] = 1;
  for (uint32_t d = 0; d < fr.draws.count; ++d) {
    if (d + 16 < fr.draws.count) {  // lane 270: the c76-c78 words of a crowd draw's bank sit in a cache line nobody has touched this frame (banks are 4 KB apart): ask for the one 16 draws ahead
      const rdr_adapter::DrawOrigin* ahead = fr.OriginOf(d + 16);
      if (ahead && (ahead->call_site == rdr_adapter::kCrowdBlocksSite || ahead->call_site == rdr_adapter::kCrowdCardsSite)) {
        const char* w = reinterpret_cast<const char*>(fr.constant_banks[fr.draws[d + 16].vs_bank].dwords + 76 * 4);
        __builtin_prefetch(w), __builtin_prefetch(w + 47);
      }
    }
    if (!in_ops[d]) continue;
    const rdr_adapter::DrawOrigin* og = fr.OriginOf(d);
    if (!og) continue;
    if (og->call_site == rdr_adapter::kCrowdCardsSite) {
      StreamCard s;
      s.draw = d;
      s.hash = HashWords(fr.constant_banks[fr.draws[d].vs_bank].dwords + 76 * 4);
      s.image0 = SlotImage(fr, fr.draws[d], 0);
      s.image13 = SlotImage(fr, fr.draws[d], 13);
      s.image16 = SlotImage(fr, fr.draws[d], 16);
      stream.push_back(s);
    } else if (og->call_site == rdr_adapter::kCrowdBlocksSite) {
      const auto bt = body_type.find(HashWords(fr.constant_banks[fr.draws[d].vs_bank].dwords + 76 * 4));
      const uint32_t img = SlotImage(fr, fr.draws[d], 0);
      if (bt == body_type.end() || !img) continue;
      const auto ins = image_type.emplace(img, bt->second);
      if (!ins.second && ins.first->second != bt->second) ins.first->second = kAmbiguous;
    }
  }
  g_bph.Lap(kBStream);
  plan.stream_cards = uint32_t(stream.size());
  if (cards.empty()) return void(plan.why = "no cards in the record");
  if (stream.empty()) return void(plan.why = "no card draws in the frame");
  plan.light_map_image = in.page_slot == 16 ? stream[0].image16 : stream[0].image13;   // the page by the program (lane 219): lm_vertex reads it in vertex slot 16, lm_uv in the pixel stage's slot 13

  // Review F2: every card draw must be the first one in everything but the world: programs, fixed and dynamic state, ps constants, the vertex and index blobs, the page, and the
  // slot 0 / 13 bindings other than the image; and the blob must be our quad.
  {
    const rdr_adapter::Draw& d0 = fr.draws[stream[0].draw];
    const auto same_bank = [&](uint32_t a, uint32_t b) { return a == b || std::memcmp(fr.constant_banks[a].dwords, fr.constant_banks[b].dwords, sizeof fr.constant_banks[a].dwords) == 0; };
    const auto same_slot = [&](const rdr_adapter::Draw& d, uint32_t slot) {
      const rdr_adapter::TextureSlot *a = SlotEntry(fr, d0, slot), *b = SlotEntry(fr, d, slot);
      if (!a || !b) return a == b;
      return a->view_format == b->view_format && std::memcmp(a->swizzle, b->swizzle, 4) == 0 && std::memcmp(&fr.samplers[a->sampler], &fr.samplers[b->sampler], sizeof(rdr::SamplerDesc)) == 0 &&
             ((slot != 13 && slot != 16) || a->image == b->image);
    };
    for (size_t i = 1; i < stream.size() && !*plan.state_why; ++i) {
      const rdr_adapter::Draw& d = fr.draws[stream[i].draw];
      bool bad = d.vs_hash != d0.vs_hash || d.ps_hash != d0.ps_hash || d.vertex_offset != d0.vertex_offset || d.index_offset != d0.index_offset || d.vertex_bytes != d0.vertex_bytes ||
                 d.index_bytes != d0.index_bytes || d.index_count != d0.index_count || d.source_prim != d0.source_prim || d.bool_loop != d0.bool_loop || !same_slot(d, 0) || !same_slot(d, 13) || !same_slot(d, 16);
      if (!bad && d.fixed != d0.fixed) bad = std::memcmp(&fr.fixeds[d.fixed], &fr.fixeds[d0.fixed], sizeof(rdr_adapter::DrawFixed)) != 0;
      if (!bad && d.dynamic != d0.dynamic) bad = std::memcmp(&fr.dynamics[d.dynamic], &fr.dynamics[d0.dynamic], sizeof(rdr_adapter::DrawDynamic)) != 0;
      if (!bad) bad = !same_bank(d.ps_bank, d0.ps_bank);
      if (in.fault == Fault::StateDrift && i + 1 == stream.size()) bad = true;  // planted
      if (bad) plan.state_why = "a card draw differs from the first in program / state / constants / bindings";
    }
    g_bph.Lap(kBStateLoop);
    if (!*plan.state_why) {
      uint8_t quad[64];
      CardQuad(quad);
      if (in.fault == Fault::BlobDrift) quad[8] ^= 0x01;  // planted: one coordinate of the expected quad (82 -> 82.000008 in the 3rd float's low byte)
      std::vector<uint8_t> vb, ib;
      if (!in.read_geometry || d0.vertex_bytes != 64 || d0.index_bytes != 8 || d0.index_count != 4 || d0.index_32bit) plan.state_why = "the card quad's draw shape is not 4 vertices / 4 16-bit indices";
      else if (!in.read_geometry(d0.vertex_offset, d0.vertex_bytes, vb) || !in.read_geometry(d0.index_offset, d0.index_bytes, ib) || vb.size() < 64 || ib.size() < 8) plan.state_why = "the card quad's blobs cannot be read";
      else if (std::memcmp(vb.data(), quad, 64) != 0) plan.state_why = "the card quad's vertex blob is not our quad";
      else {
        const uint16_t want[4] = {0, 1, 2, 3};
        if (std::memcmp(ib.data(), want, 8) != 0) plan.state_why = "the card quad's index blob is not 0 1 2 3";
      }
    }
  }

  g_bph.Lap(kBQuad);
  // 3. The bakes: a pass of one draw on the bake's vertex program with a 64 x 64 viewport and a copy of its target, whose slot 0 is the type's stands texture and whose copy
  // destination is the type's picture.
  uint32_t picture_of[kTypes] = {};
  struct Bake { uint32_t tex0, dest; };
  std::vector<Bake> bakes;  // in the frame's order
  uint32_t target = 0, pass_draws = 0, pass_draw = 0;
  std::vector<uint32_t> pass_copies;
  const auto close_pass = [&]() {
    if (pass_draws == 1 && !pass_copies.empty() && in.bake_vs_hashes && in.bake_vs_hashes->count(fr.draws[pass_draw].vs_hash)) {
      const rdr_adapter::Draw& d = fr.draws[pass_draw];
      const float* vp = fr.dynamics[d.dynamic].host.viewport;
      const rdr::CopyOp& cp = fr.copies[pass_copies[0]];
      if (vp[2] == 64.0f && vp[3] == 64.0f && cp.source.image == target) {
        bakes.push_back({SlotImage(fr, d, 0), cp.dest.image});
        const auto bt = image_type.find(SlotImage(fr, d, 0));
        if (bt != image_type.end() && bt->second != kAmbiguous) {
          uint32_t& p = picture_of[bt->second];
          p = p ? kAmbiguous : cp.dest.image;  // two bakes for one type: not joinable
        }
      }
    }
    pass_draws = 0;
    pass_copies.clear();
  };
  thread_local std::vector<uint8_t> is_card;
  is_card.assign(fr.draws.count, 0);
  for (const StreamCard& s : stream) is_card[s.draw] = 1;
  uint32_t first_card_pos = 0xFFFFFFFFu, last_card_pos = 0, card_ops = 0, page_copy_pos = 0xFFFFFFFFu;
  bool page_copy_seen = false, page_copy_later = false;
  for (uint32_t k = 0; k < fr.ops.count; ++k) {
    const rdr_adapter::Op& op = fr.ops[k];
    if (op.kind == rdr_adapter::OpKind::SetTargets) {
      close_pass();
      const rdr::TargetsOp& t = fr.targets[op.index];
      target = t.color_count ? t.color[0].image : 0;
    } else if (op.kind == rdr_adapter::OpKind::Draw) {
      ++pass_draws, pass_draw = op.index;
      if (is_card[op.index]) {
        first_card_pos = std::min(first_card_pos, k), last_card_pos = k, ++card_ops;
      }
    } else if (op.kind == rdr_adapter::OpKind::Copy) {
      pass_copies.push_back(op.index);
      if (fr.copies[op.index].dest.image == plan.light_map_image && first_card_pos != 0xFFFFFFFFu) page_copy_later = plan.light_map_image != 0;
      if (fr.copies[op.index].dest.image == plan.light_map_image && first_card_pos == 0xFFFFFFFFu) page_copy_pos = k, page_copy_seen = true;  // (the page's copy before the first card)
    }
  }
  close_pass();
  // The pass order and the contiguity (review F2): ours goes in at the first card op, so a light-map page copy the frame makes must precede it (lane 219: a page the frame does not write has no order to keep) and the card draws must be one run (lane 223: that also keeps every picture write out of the cards; no bake has to be in the frame).
  plan.bake_count = uint32_t(bakes.size()), plan.bakes_scanned = true;
  if (in.fault == Fault::CardGap) ++card_ops;   // planted: one op more than the run holds = a non-card op inside the cards (must be refused as not contiguous)
  if (first_card_pos == 0xFFFFFFFFu) plan.state_why = "no card op";
  else if (last_card_pos - first_card_pos + 1 != card_ops) plan.state_why = "card draws are not one contiguous run";
  // Lane 219: the page copy only has an ORDER to keep when the frame has one. A page no copy writes this frame (the lm_vertex arenas bind a page that is loaded once, image 370 in the 44 arena captures) is the same bytes at the
  // first card op and at ours; only a copy AFTER the first card is a real refusal.
  else if (page_copy_later && !page_copy_seen) plan.state_why = "the light-map page copy comes after the first card";
  // Lane 223: no rule about the bakes. A frame without a bake leaves the pictures as the last bake wrote them: the stream's cards and ours read the same bytes. A write to a picture INSIDE the cards (the only
  // write that could make ours differ: ours is drawn at the first card) is an op between two card draws, which the contiguity rule above refuses; one after the last card changes nothing for either.
  if (in.fault == Fault::InjectEarly && page_copy_seen) plan.state_why = "injection before the page copy";  // planted: the first card op read as sitting before the page copy
  // A type with no stands block this frame has no block to join through: its bake is the one at its own position (the k-th bake of a frame is type k, measured on every frame of four
  // captures), taken only when the frame has exactly one bake per type and that bake's texture is claimed by no type; every card is still checked against the picture it gets.
  if (bakes.size() == kTypes)
    for (uint32_t t = 0; t < kTypes; ++t)
      if (!picture_of[t] && !image_type.count(bakes[t].tex0)) picture_of[t] = bakes[t].dest, ++plan.joined_by_position;
  if (bakes.size() == kTypes && (in.fault == Fault::PositionAll || in.fault == Fault::PositionReversed))  // planted: every type by position (must stay byte-equal) / reversed (must fail)
    for (uint32_t t = 0; t < kTypes; ++t) picture_of[t] = bakes[in.fault == Fault::PositionAll ? t : kTypes - 1 - t].dest, ++plan.joined_by_position;

  if ((in.fault == Fault::DropUnjoined || in.fault == Fault::UnjoinType) && !cards.empty()) picture_of[cards[0].type] = 0;  // planted: a type whose join failed

  // The picture each type is drawn with (planted faults change what we bind, never what the join found). Re-run after the stream join of step 4b.
  uint32_t bind_of[kTypes] = {};
  const auto compute_bind = [&]() {
    for (uint32_t t = 0; t < kTypes; ++t) bind_of[t] = picture_of[t];
    if (in.fault == Fault::TypeShift)
      for (uint32_t t = 0; t < kTypes; ++t) bind_of[t] = picture_of[(t + 1) % kTypes];
    if (in.fault == Fault::OnePicture)
      for (uint32_t t = 0; t < kTypes; ++t)
        if (picture_of[t] && picture_of[t] != kAmbiguous) bind_of[t] = stream[0].image0;
    if (in.fault == Fault::AtlasPicture)
      for (uint32_t t = 0; t < kTypes; ++t)
        if (picture_of[t] && picture_of[t] != kAmbiguous) bind_of[t] = plan.light_map_image;
  };
  compute_bind();

  g_bph.Lap(kBBakes);
  // 3b. lane 175 (THIN, card_thin_core.h): the game recorded only the FIRST card of each person type's loop (the op-21 of every card is still recorded: the record is complete). The kept stream
  // draws are the trigger, the state source and the proof: each must be the record's first card of its type, bit for bit, carrying that type's own picture; every type with record cards must have
  // one; then the whole type is drawn from the record, in the record's order (full frames prove that order equals the stream's: record_order_differs / inversions stay 0 on them). Any doubt and the
  // frame is a HOLE (thin_hole): the dropped cards are gone and nothing replaces them, so it is counted and the gate unproves the cut. A frame with more stream cards than types is not thin.
  if (in.thin && (in.thin_dropped > 0 || in.fault == Fault::ThinTrustCount)) {  // (the control fault enters on a FULL frame too: the entry rule is what keeps a full frame a full frame)
    int first_of[kTypes];
    for (int& f : first_of) f = -1;
    for (uint32_t i = 0; i < cards.size(); ++i)
      if (first_of[cards[i].type] < 0) first_of[cards[i].type] = int(i);
    {
      // Review B1: the delivery check. The kept cards' worlds are fixed per seat, so a record one frame off still pairs them; the COUNT does not: the game made `thin_seen` card draws this frame
      // (every one is one card object of the record), and kept `seen - dropped` of them in the stream.
      bool ok = true;
      if (in.thin_seen != cards.size()) ok = false, plan.thin_why = "the game's card count of this frame differs from the record's (a record of another frame)";
      else if (stream.size() + in.thin_dropped != in.thin_seen) ok = false, plan.thin_why = "the stream's card draws are not the game's minus the dropped";
      else {
        uint64_t sum = 0;   // which cards: the sum of the record cards' hashes must be the sum the hook took from the card op-21s (equal counts of another frame's cards differ here)
        for (const RecordCard& c : cards) sum += c.hash;
        if (sum != in.thin_world) ok = false, plan.thin_why = "the game's card worlds of this frame differ from the record's (a record of another frame)";
      }
      if (!ok) {
        plan.thin_hole = true;
        plan.why = "bad thin";
        return;
      }
    }
    {
      bool ok = !*plan.state_why;
      if (!ok) plan.thin_why = plan.state_why;
      bool seen[kTypes] = {};
      std::vector<int> stream_type(stream.size(), -1);
      for (uint32_t i = 0; i < stream.size() && ok; ++i) {
        int t = -1;
        for (uint32_t u = 0; u < kTypes; ++u)
          if (picture_of[u] && picture_of[u] != kAmbiguous && picture_of[u] == stream[i].image0) t = int(u);
        if (t < 0) {
          ok = false, plan.thin_why = "a kept card's picture is no type's bake";
        } else if (seen[t]) {
          ok = false, plan.thin_why = "two kept cards of one type";
        } else {
          int k = first_of[t];
          if (in.fault == Fault::ThinFirstOfTypeShift) {  // planted: the SECOND record card of the type
            for (uint32_t j = uint32_t(k) + 1; k >= 0 && j < cards.size(); ++j)
              if (cards[j].type == uint32_t(t)) {
                k = int(j);
                break;
              }
          }
          if (k < 0 || cards[k].hash != stream[i].hash) ok = false, plan.thin_why = "a kept card is not the record's first card of its type";
          else if (stream[i].image0 != bind_of[t]) ok = false, ++plan.picture_mismatch, plan.thin_why = "a kept card shows another picture than the type's bake";
          seen[t] = true;
          stream_type[i] = t;
        }
      }
      if (ok) {
        for (uint32_t t = 0; t < kTypes; ++t)
          if (first_of[t] >= 0 && !seen[t]) {
            ok = false, plan.thin_why = "a type with record cards has no kept card";
            break;
          }
        if (ok && in.fault == Fault::ThinTypeMissing) ok = false, plan.thin_why = "planted: the record holds a type the stream did not keep";
      }
      if (!ok) {
        plan.thin_hole = true;
        plan.why = "bad thin";
        return;
      }
      // Draw the types in the order their kept cards appear in the stream; within a type the record's order.
      std::vector<std::vector<uint32_t>> by_type_t(kTypes);
      for (uint32_t i = 0; i < cards.size(); ++i) {
        const RecordCard& c = cards[i];
        if (in.density < 100 && in.rank) {
          uint32_t w[3];
          std::memcpy(w, &c.inst.c[3], 4), std::memcpy(w + 1, &c.inst.c[7], 4), std::memcpy(w + 2, &c.inst.c[11], 4);
          if (in.rank(w[0], w[1], w[2]) >= in.density) {
            ++plan.density_skipped;
            continue;
          }
        }
        by_type_t[c.type].push_back(i);
      }
      for (uint32_t i = 0; i < stream.size(); ++i) {
        const uint32_t t = uint32_t(stream_type[i]);
        if (by_type_t[t].empty()) continue;
        Run run;
        run.type = t;
        run.picture = bind_of[t];
        run.first = uint32_t(plan.instances.size());
        for (uint32_t j : by_type_t[t]) plan.instances.push_back(cards[j].inst);
        run.count = uint32_t(plan.instances.size()) - run.first;
        plan.runs.push_back(run);
      }
      plan.drawn = uint32_t(plan.instances.size());
      plan.thin = true;
      plan.thin_dropped = uint32_t(cards.size() - stream.size());
      if (plan.drawn == 0) return void(plan.why = "no card could be drawn");
      plan.usable = true;
      return;
    }
  }

  g_bph.Lap(kBThin);
  // 4. Pairing by world (bit for bit).
  thread_local fast::FlatU64Map by_hash;
  by_hash.Reset(stream.size() + 16);
  for (uint32_t i = 0; i < stream.size(); ++i) by_hash.emplace(stream[i].hash, i);
  thread_local std::vector<uint8_t> stream_paired;
  stream_paired.assign(stream.size(), 0);
  for (RecordCard& c : cards) {
    const auto f = by_hash.find(c.hash);
    if (f != by_hash.end() && !stream_paired[f->second]) c.pair = f->second, stream_paired[f->second] = 1;
  }

  g_bph.Lap(kBPairing);
  // 4b. Lane 223: the stream join. A type the bakes did not join (no bake this frame, or no bake of its own) takes the picture its own stream cards show (slot 0): the one image all of the type's paired
  // cards agree on, which is exactly what the game draws (written by the last bake). Valid in any frame, so a frame is never half ours and half the game's; the bake join stays only because the thin
  // path (step 3b, before the pairing) needs the types' pictures before any card is paired [known gap: the thin path has no stream join]. The planted unjoin faults keep their type unjoined.
  {
    const int skip_type = ((in.fault == Fault::DropUnjoined || in.fault == Fault::UnjoinType) && !cards.empty()) ? int(cards[0].type) : -1;
    for (uint32_t t = 0; t < kTypes; ++t) {
      if (picture_of[t] || int(t) == skip_type) continue;
      uint32_t img = 0;
      bool same = true;
      for (const RecordCard& c : cards)
        if (c.type == t && c.pair != 0xFFFFFFFFu) {
          const uint32_t i0 = stream[c.pair].image0;
          if (!i0 || (img && img != i0)) same = false;
          else img = i0;
        }
      if (same && img) picture_of[t] = img, ++plan.joined_by_stream;
    }
    compute_bind();
  }

  g_bph.Lap(kBJoin);
  // 5. The cards that are ours, per type in the record's order.
  std::vector<std::vector<uint32_t>> by_type(kTypes);  // record card indices
  bool unjoined_seen[kTypes] = {};
  for (uint32_t i = 0; i < cards.size(); ++i) {
    const RecordCard& c = cards[i];
    if (c.pair == 0xFFFFFFFFu) {
      ++plan.unpaired_record;  // no stream draw: not drawn (nothing to stay)
      continue;
    }
    if (in.density < 100 && in.rank) {
      uint32_t w[3];
      std::memcpy(w, &c.inst.c[3], 4), std::memcpy(w + 1, &c.inst.c[7], 4), std::memcpy(w + 2, &c.inst.c[11], 4);  // the member's position bits = c76.w c77.w c78.w
      if (in.rank(w[0], w[1], w[2]) >= in.density) {
        ++plan.density_skipped;
        stream_paired[c.pair] = 2;  // accounted, dropped on purpose
        continue;
      }
    }
    if (in.fault == Fault::KeepFirst && c.pair == 0) {  // planted: the first stream card stays the game's (the kept-first case)
      plan.keep.insert(stream[c.pair].draw);
      stream_paired[c.pair] = 2;
      continue;
    }
    const uint32_t joined = picture_of[c.type];
    if (!joined || joined == kAmbiguous) {  // no picture for this type: its cards stay the stream's
      ++plan.unjoined_cards;
      if (!unjoined_seen[c.type]) unjoined_seen[c.type] = true, ++plan.unjoined_types;
      if (in.fault != Fault::DropUnjoined) plan.keep.insert(stream[c.pair].draw);
      stream_paired[c.pair] = 2;
      continue;
    }
    if (stream[c.pair].image0 != bind_of[c.type]) {  // the stream's card shows another picture than the one we would bind
      ++plan.picture_mismatch;
      if (in.enforce) {
        plan.keep.insert(stream[c.pair].draw);
        stream_paired[c.pair] = 2;
        continue;
      }
    }
    by_type[c.type].push_back(i);
  }
  for (uint32_t i = 0; i < stream.size(); ++i)
    if (!stream_paired[i]) ++plan.unaccounted, plan.keep.insert(stream[i].draw);

  g_bph.Lap(kBOurs);
  // 6. The instances (type-major, the record's order within a type = the stream's), and the order proof.
  for (auto& v : by_type) {  // review F3: the stream's order within a type is the drawing order; the record's order is a census count only
    if (!std::is_sorted(v.begin(), v.end(), [&](uint32_t a, uint32_t b) { return cards[a].pair < cards[b].pair; })) ++plan.record_order_differs;
    std::stable_sort(v.begin(), v.end(), [&](uint32_t a, uint32_t b) { return cards[a].pair < cards[b].pair; });
  }
  if (in.fault == Fault::OrderReversed)
    for (auto& v : by_type) std::reverse(v.begin(), v.end());
  if (in.fault == Fault::OrderSwap)
    for (auto& v : by_type)
      if (v.size() >= 2) {
        std::swap(v[0], v[1]);
        break;
      }
  std::vector<uint32_t> type_order;  // review F3: runs in the order the types first appear in the stream
  for (uint32_t t = 0; t < kTypes; ++t)
    if (!by_type[t].empty()) type_order.push_back(t);
  std::stable_sort(type_order.begin(), type_order.end(), [&](uint32_t a, uint32_t b) {
    return cards[*std::min_element(by_type[a].begin(), by_type[a].end(), [&](uint32_t x, uint32_t y) { return cards[x].pair < cards[y].pair; })].pair <
           cards[*std::min_element(by_type[b].begin(), by_type[b].end(), [&](uint32_t x, uint32_t y) { return cards[x].pair < cards[y].pair; })].pair;
  });
  for (uint32_t t : type_order) {
    Run run;
    run.type = t;
    run.picture = bind_of[t];
    run.first = uint32_t(plan.instances.size());
    for (uint32_t i : by_type[t]) {
      stream[cards[i].pair].seq = uint32_t(plan.instances.size());
      plan.instances.push_back(cards[i].inst);
    }
    run.count = uint32_t(plan.instances.size()) - run.first;
    plan.runs.push_back(run);
  }
  plan.drawn = uint32_t(plan.instances.size());
  uint32_t last = 0;
  bool any = false;
  for (const StreamCard& s : stream) {
    if (s.seq == 0xFFFFFFFFu) continue;
    if (any && s.seq < last) ++plan.inversions;
    last = s.seq, any = true;
  }
  plan.holes = plan.stream_cards - plan.drawn - uint32_t(plan.keep.size()) - plan.density_skipped;

  // 7. The whole-frame proofs: a frame whose pairing, order or pictures are off is not drawn (the caller keeps every card of it the game's).
  plan.bad_pairing = plan.unpaired_record != 0 || plan.unaccounted != 0;
  plan.bad_order = plan.inversions != 0;
  plan.bad_picture = plan.picture_mismatch != 0;
  plan.bad_state = *plan.state_why != 0;
  g_bph.Lap(kBInst);
  const bool bad = plan.bad_pairing || plan.bad_order || plan.bad_picture || plan.bad_state;
  if (in.verify && in.enforce && bad) {
    plan.why = plan.bad_state ? "bad state" : plan.bad_pairing ? "bad pairing" : plan.bad_order ? "bad order" : "bad picture";
    return;
  }
  if (plan.drawn == 0) return void(plan.why = "no card could be drawn");
  plan.usable = true;
}

}  // namespace rdr_scene::cards
