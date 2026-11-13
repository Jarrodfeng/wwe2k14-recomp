// wrestler_ours_check: the offline proof of the wrestlers' in-place replacement, CPU half (Session 101, lane 45 = W2; docs\renderer\WRESTLER_PROMOTION_MAP.md section 11). 0 game runs.
// It drives the SHIPPED decision code (src\renderer_scene\wrestler_core.h, the same class wrestler_ours.cpp's hook uses) over a capture, frame by frame, the way the binder
// would: stream frame j with the scene record of frame j-1 (the +1 pairing lane 42 measured), every main-pass model-part draw offered to Decide. For each REPLACED draw it
// then proves, independently of Decide's own checks, that what would be drawn is the stream's: our vertex bytes equal the stream draw's captured vertex bytes (positions
// within float ulps, everything else bit-equal), our index buffer equals the stream's, and the palette rows we would send equal the stream draw's rows c76 .. bit for bit.
//   wrestler_ours_check --capture DIR --game DIR [--frames A-B] [--step N] [--fault LIST] [--lines] [--live] [--corrupt-at F]
// S101 lane 88: --live builds the core the way the live hook does (verify_all off: pinned parts, proof frames). --corrupt-at F (with --live): from frame F + 1 on, the vertex blob of the first
// replaced draw of frame F reads back with its first vertex inverted (a blob corrupted after the pin); the output line CORRUPT says in which frame the core first refused a part's bytes.
// Faults (comma list, the hook's --replace-fault names): bind_pose, record_off_by_one, part_off_by_one, wrong_attire, scale_one, lent_constants, uv_swap,
// accept_record_palette, checks_off. With a fault and the checks ON, every wrong draw must be refused (the "wrong" counters stay 0); with checks_off / accept_record_palette
// the independent proof must SEE the wrongness (wrong > 0): a positive control for the proof itself.
// Exit 0 = measured, 2 = a loud failure (no scene records, no game files, nothing claimed), 3 = bad usage. Machine-readable lines: ARM, SLOT, RESULT.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../../src/renderer_adapter/capture_reader.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/wrestler_core.h"

using namespace rdr_adapter;
using rdr_scene::SceneTables;
using rdr_scene::WrestlerCore;
using rdr_scene::WrestlerDecision;
using rdr_scene::WrestlerFaults;
namespace wg = assets::wrestler_geom;

// lane 207: the want hook the live layer gets (WrestlerContributor::WantSkeleton), collected here for --wants and --inject-own-skeletons.
static std::map<uint32_t, uint32_t> g_wants, wants_ever;   // wants_ever: every (object, count) wanted at any point (an unwant must not hide it from --wants)

int main(int argc, char** argv) {
  std::string capture, game, fault_list;
  int first = 0, last = -1, step = 1;
  bool lines = false, live = false, refusals = false, dump_kept = false;   // lane 134: dump_kept
  bool link_rule_test = false, link_rule_fault = false;   // lane 206
  bool show_wants = false, inject = false, inject_plant = false;   // lane 207
  bool trim_test = false, trim_fault = false, loader_test = false, loader_fault = false;   // lane 142
  std::string dump_dir;
  int corrupt_at = -1;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--capture") capture = next();
    else if (a == "--game") game = next();
    else if (a == "--fault") fault_list = next();
    else if (a == "--frames") { if (std::sscanf(next(), "%d-%d", &first, &last) != 2) { std::fprintf(stderr, "bad --frames\n"); return 3; } }
    else if (a == "--step") step = std::max(1, std::atoi(next()));
    else if (a == "--lines") lines = true;
    else if (a == "--dump-kept") dump_kept = true;   // lane 134: print the kept (not replaced) draws: object, part, counts, why; --dump-dir DIR also writes their vertex / index bytes
    else if (a == "--dump-dir") dump_dir = next();
    else if (a == "--refusals") refusals = true;
    else if (a == "--live") live = true;
    else if (a == "--wants") show_wants = true;   // lane 207: print every (object, node count) the core asked the live hook to keep
    else if (a == "--inject-own-skeletons") inject = true;   // lane 207: from the frame after a want, the record gets a skeleton of the wanted object whose bones are the draws' own palette rows (what the live hook now records)
    else if (a == "--inject-own-skeletons-plant") inject = inject_plant = true;   // plant: every row one palette slot off (the skeleton must be refused, nothing drawn wrong)
    else if (a == "--link-rule-test") link_rule_test = true;
    else if (a == "--link-rule-test-fault") link_rule_test = link_rule_fault = true;
    else if (a == "--trim-test") trim_test = true;           // lane 142: Roster::TrimIdle (keep_pac load, trim while the load mutex is held, trim after)
    else if (a == "--loader-test") loader_test = true;        // lane 142: the single FIFO loader reads queued entries pac by pac (2 pac opens for A B A B A, not 5)
    else if (a == "--loader-test-fault") { loader_test = true; loader_fault = true; }   // lane 142 plant: arrival order
    else if (a == "--trim-test-fault") trim_fault = true;     // lane 142 plant: the final TrimIdle is skipped, the test must FAIL
    else if (a == "--corrupt-at") corrupt_at = std::atoi(next());
    else { std::fprintf(stderr, "wrestler_ours_check: unknown argument %s\n", a.c_str()); return 3; }
  }
  if (loader_test && !game.empty()) {
    assets::wrestler_roster::Roster roster;
    std::string err;
    if (!roster.Build(game, err)) { std::fprintf(stderr, "loader-test: FAIL roster: %s\n", err.c_str()); return 2; }
    // two different character pacs, three entries each (the first entries of each)
    std::vector<int> a, b;
    const int pa = roster.entries()[0].pac;
    for (size_t i = 0; i < roster.entries().size() && (a.size() < 3 || b.size() < 3); ++i) {
      const int p = roster.entries()[i].pac;
      if (p == pa && a.size() < 3) a.push_back(int(i));
      else if (p != pa && (b.empty() || roster.entries()[size_t(b[0])].pac == p) && b.size() < 3) b.push_back(int(i));
    }
    if (a.size() < 3 || b.size() < 3) { std::fprintf(stderr, "loader-test: FAIL need two pacs with 3 entries\n"); return 2; }
    roster.SetArrivalOrderForTest(loader_fault);
    std::vector<std::shared_future<std::shared_ptr<assets::wrestler_roster::Model>>> futs;
    roster.LockedForTest([&] {   // the first job is taken and waits on the mutex; the rest queue behind it
      futs.push_back(roster.LoadModelAsync(a[0], true));
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      const int order[4] = {b[0], a[1], b[1], a[2]};
      for (int i : order) futs.push_back(roster.LoadModelAsync(i, true));
      futs.push_back(roster.LoadModelAsync(b[2], true));
    });
    bool all = true;
    for (auto& f : futs) all = all && f.get() != nullptr;
    const int opens = roster.PacOpens();
    const bool ok = all && (loader_fault ? opens > 2 : opens == 2);
    std::printf("loader-test: %zu reads of 2 pacs queued behind a held mutex, all read %d, pac opens %d (FIFO-by-pac: 2; arrival order: more): %s\n", futs.size(), int(all), opens, ok ? "ok" : "FAIL");
    roster.TrimIdle(0);
    return loader_fault ? (opens > 2 ? 1 : 0) : (ok ? 0 : 1);   // the planted run exits 1 when the fault shows (the gate wants exit 1 from it)
  }
  if (link_rule_test) {   // lane 206 (review B1): a small model's declared link needs its rows; its same-count miss leaves the piece route open. --link-rule-test-fault = the old rules (must FAIL)
    const bool old = link_rule_fault;
    bool ok = true;
    auto chk = [&](bool c, const char* what) { std::printf("%s  %s\n", c ? "ok  " : "FAIL", what); ok = ok && c; };
    auto declared = [&](bool small, double s, bool off) { return old ? true : WrestlerCore::DeclaredLinkOk(small, s, off); };
    auto closes = [&](bool same, bool small) { return old ? same : WrestlerCore::MissClosesPieceRoute(same, small); };
    chk(declared(false, 0.0, false), "a body-sized model keeps its declared link whatever the score (unchanged)");
    chk(!declared(true, 0.1, false), "a small model's declared skeleton with rows that are not the draws' is refused (the piece route stays open)");
    chk(declared(true, 1.0, false), "a small model's declared skeleton whose rows are the draws' (a hair's own) links");
    chk(declared(true, 0.0, true), "checks_off (a plant's picture) takes it as before");
    chk(closes(true, false), "a body-sized model with a same-count miss closes the route (unchanged: Palette refusal)");
    chk(!closes(true, true), "a small model with a same-count miss does NOT close the piece route (an 11-node alpha piece beside a recorded hair skeleton)");
    chk(!closes(false, true) && !closes(false, false), "no same-count skeleton: never closed");
    std::printf("link-rule-test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
  }
  if (trim_test && !game.empty()) {
    assets::wrestler_roster::Roster roster;
    std::string err;
    if (!roster.Build(game, err)) { std::fprintf(stderr, "trim-test: FAIL roster: %s\n", err.c_str()); return 2; }
    std::shared_ptr<assets::wrestler_roster::Model> m = roster.LoadModel(0, err, true);
    const int want = roster.entries()[0].pac;
    bool ok = m && roster.OpenPacId() == want;
    std::printf("trim-test: after a keep_pac load the open pac is %d (want %d): %s\n", roster.OpenPacId(), want, ok ? "ok" : "FAIL");
    bool held_ok = false;
    roster.LockedForTest([&] {
      std::thread other([&] { roster.TrimIdle(0); });   // another thread trims while the mutex is held: it must not block and the pac must stay
      other.join();
      held_ok = roster.OpenPacId() == want;
    });
    std::printf("trim-test: TrimIdle(0) while the mutex is held leaves the pac (%d): %s\n", roster.OpenPacId(), held_ok ? "ok" : "FAIL");
    if (!trim_fault) roster.TrimIdle(0);
    const bool gone = roster.OpenPacId() == -1;
    std::printf("trim-test: TrimIdle(0) afterwards: open pac %d (want -1): %s\n", roster.OpenPacId(), gone ? "ok" : "FAIL");
    std::printf("trim-test: %s\n", ok && held_ok && gone ? "PASS" : "FAIL");
    return ok && held_ok && gone ? 0 : 1;
  }
  if (capture.empty() || game.empty()) {
    std::fprintf(stderr, "usage: wrestler_ours_check --capture DIR --game DIR [--frames A-B] [--step N] [--fault LIST] [--lines]\n");
    return 3;
  }
  WrestlerFaults faults;
  {
    std::string err;
    if (!WrestlerFaults::Parse(fault_list, faults, err)) { std::fprintf(stderr, "wrestler_ours_check: %s\n", err.c_str()); return 3; }
  }
  const auto t0 = std::chrono::steady_clock::now();
  CaptureReader cap;
  if (!cap.Open(capture)) { std::fprintf(stderr, "wrestler_ours_check: FAIL cannot open the capture at %s\n", capture.c_str()); return 2; }
  if (cap.scene_record_count() == 0) { std::fprintf(stderr, "wrestler_ours_check: FAIL %s has no scene records (take the capture with --wwe2k14_scene_records)\n", capture.c_str()); return 2; }
  assets::wrestler_roster::Roster roster;
  {
    std::string err;
    if (!roster.Build(game, err)) { std::fprintf(stderr, "wrestler_ours_check: FAIL roster: %s\n", err.c_str()); return 2; }
  }
  std::fprintf(stderr, "wrestler_ours_check: roster %zu pacs, %zu model entries, %.1f s\n", roster.pacs(), roster.entries().size(), roster.seconds());
  const int nframes = int(cap.frame_count());
  if (last < 0 || last >= nframes) last = nframes - 1;

  if (show_wants || inject) {
    rdr_scene::g_skeleton_want_hooks.want = [](uint32_t m, uint32_t n) { g_wants[m] = n, wants_ever[m] = n; };
    rdr_scene::g_skeleton_want_hooks.unwant = [](uint32_t m) { g_wants.erase(m); };
  }
  uint64_t injected_skeletons = 0;
  WrestlerCore core(roster, faults, /*sync=*/true, /*verify_all=*/!live);
  core.diag_cands_ = refusals;
  core.SetDropsWanted(true);   // lane 206: as fview's thin binder asks: a lite draw takes its (object, part)'s last full counts (thin captures)
  uint64_t lite_draws = 0;   // lane 206: thin (kDrawThin) draws: no geometry in the capture, drawn from the key's template live; not decided here, not a layout refusal
  uint64_t frames = 0, frames_ready = 0, claimed = 0, replaced = 0, kept[size_t(KeepWhy::Count)] = {};
  // the independent proof of every replaced draw
  uint64_t proof_draws = 0, wrong_vertex = 0, wrong_index = 0, wrong_rows = 0, wrong_count = 0;
  uint64_t piece_proof = 0, piece_wrong_vertex = 0, piece_wrong_rows = 0;   // S101 lane 48: the same proof, counted on piece draws only
  struct PieceTally {   // adds this draw's wrong-counter deltas to the piece counts when the draw is a piece (runs on every exit of the draw's block)
    bool piece; const uint64_t &wv, &wr; uint64_t &pp, &pwv, &pwr; uint64_t wv0, wr0;
    ~PieceTally() { if (piece) ++pp, pwv += wv - wv0, pwr += wr - wr0; }
  };
  uint64_t per_slot_replaced[16] = {}, per_slot_palette[16] = {}, per_slot_claimed[16] = {};
  std::map<std::string, std::pair<uint64_t, uint64_t>> by_part;  // "slot:part" -> replaced, palette-refused
  std::map<int, int> no_record;
  LoadedFrame fr;
  std::vector<uint8_t> vb, ib;
  uint64_t corrupt_offset = 0, corrupt_stride = 0, refused_before = 0, decide_ns = 0, decide_calls = 0;
  bool corrupt_set = false;
  int corrupt_detected = -1;
  for (int f = first; f <= last; f += step) {
    if (!cap.LoadFrame(uint32_t(f), fr)) { std::fprintf(stderr, "wrestler_ours_check: FAIL cannot load frame %d\n", f); return 2; }
    if (frames++ == 0) std::fprintf(stderr, "wrestler_ours_check: first frame %d flags 0x%X (bit 2 = 16-bit pairs high half first)\n", f, unsigned(fr.header.flags));
    // stream frame j with the record of frame j-1 (the +1 pairing)
    std::shared_ptr<SceneTables> rec;
    if (f > 0 && cap.HasSceneRecord(uint32_t(f - 1))) {
      rec = std::make_shared<SceneTables>();
      std::string err;
      if (!rdr_scene::ReadRecord(cap.SceneRecordPath(uint32_t(f - 1)).string(), *rec, err)) { std::fprintf(stderr, "wrestler_ours_check: FAIL scene record %d: %s\n", f - 1, err.c_str()); return 2; }
    }
    if (faults.record_off_by_one && f > 1 && cap.HasSceneRecord(uint32_t(f - 2))) {
      // (the Core keeps the previous record itself: feed it the one before first)
      auto before = std::make_shared<SceneTables>();
      std::string err;
      if (rdr_scene::ReadRecord(cap.SceneRecordPath(uint32_t(f - 2)).string(), *before, err)) core.SetRecord(before);
    }
    if (inject && rec) {   // lane 207
      for (const auto& w : g_wants) {
        const std::shared_ptr<const assets::wrestler_roster::Model> model = core.ModelOfObject(w.first);
        if (!model || model->parts.node_count != w.second) continue;
        std::vector<rdr_scene::Mat4> bones(w.second);
        for (auto& b : bones) for (int i = 0; i < 16; ++i) b.m[i] = (i % 5 == 0) ? 1.f : 0.f;
        bool any = false;
        for (uint32_t d = 0; d < fr.draws.count; ++d) {
          const DrawOrigin* o = fr.OriginOf(d);
          if (!o || o->object != w.first || (o->pass_site != kModelPartColourSite && o->pass_site != kModelPartMirrorSite) || (fr.draws[d].flags & kDrawThin)) continue;
          const Draw& dr = fr.draws[d];
          if (o->part >= model->parts.parts.size() || dr.vs_bank >= fr.constant_banks.count) continue;
          const assets::jboy_parts::Part& part = model->parts.parts[o->part];
          const ConstantBank& bank = fr.constant_banks[dr.vs_bank];
          for (uint32_t k = 0; k < part.palette_count; ++k) {
            uint32_t node = part.palette[k];   // 1-based
            if (node < 1 || node > w.second) continue;
            if (inject_plant) node = node % w.second + 1;   // one slot off
            for (int j = 0; j < 3; ++j) {
              const float* g = reinterpret_cast<const float*>(bank.dwords) + 4 * (76 + 3 * int(k) + j);
              for (int c = 0; c < 4; ++c) bones[node - 1].m[c * 4 + j] = g[c];
            }
            any = true;
          }
        }
        if (!any) continue;
        rdr_scene::Object ob;
        char id[32];
        std::snprintf(id, sizeof id, "skel:%08X", w.first);
        ob.id = rec->AddString(id);
        ob.flags = rdr_scene::kObjVisible | rdr_scene::kObjSkinned;
        ob.bones_first = uint32_t(rec->bones.size()), ob.bones_count = w.second;
        rec->bones.insert(rec->bones.end(), bones.begin(), bones.end());
        rec->engine_objects.resize(rec->objects.size(), 0u);
        rec->objects.push_back(ob);
        rec->engine_objects.push_back(w.first);
        ++injected_skeletons;
      }
    }
    core.SetRecord(rec);
    auto read = [&](uint64_t offset, uint32_t bytes, std::vector<uint8_t>& out) {
      if (!cap.ReadGeometry(offset, bytes, out)) return false;
      if (corrupt_set && f > corrupt_at && offset == corrupt_offset && out.size() >= corrupt_stride)
        for (uint64_t i = 0; i < corrupt_stride; ++i) out[size_t(i)] = uint8_t(~out[size_t(i)]);
      return true;
    };
    if (!core.BeginFrame(fr)) { if (!rec) ++no_record[f]; continue; }
    ++frames_ready;
    { std::vector<uint32_t> drops_unused; core.TakeDrops(drops_unused); }   // lane 206 (note): the checker never reads the drops; do not let them grow
    for (uint32_t d = 0; d < fr.draws.count; ++d) {
      const DrawOrigin* o = fr.OriginOf(d);
      if (!o || (o->pass_site != kModelPartColourSite && o->pass_site != kModelPartMirrorSite) || o->object == 0) continue;   // = the hook's Claims
      if (fr.draws[d].flags & kDrawThin) { ++lite_draws; continue; }
      ++claimed;
      WrestlerDecision dec;
      KeepWhy why = KeepWhy::NotOurs;
      const auto t_decide = std::chrono::steady_clock::now();
      const bool decided = core.Decide(fr, d, read, dec, why);
      decide_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t_decide).count()), ++decide_calls;
      if (corrupt_set && corrupt_detected < 0 && f > corrupt_at && core.stats().refused_bytes > refused_before) corrupt_detected = f;
      if (!decided) {
        ++kept[size_t(why)];
        if (dump_kept && why != KeepWhy::NotOurs) {   // lane 134
          const Draw& kd = fr.draws[d];
          std::printf("KEPT frame %d draw %u object 0x%08llX part %u why %d vertices %u indices %u stride %u\n", f, d, (unsigned long long)o->object, o->part, int(why), kd.vertex_count, kd.index_count, kd.layout < fr.layouts.count ? fr.layouts[kd.layout].host_stride : 0u);
          if (!dump_dir.empty() && cap.ReadGeometry(kd.vertex_offset, kd.vertex_bytes, vb)) {
            char nm[256]; std::snprintf(nm, sizeof nm, "%s/kept_f%d_d%u.vb", dump_dir.c_str(), f, d);
            if (FILE* fo = std::fopen(nm, "wb")) { std::fwrite(vb.data(), 1, vb.size(), fo); std::fclose(fo); }
            const ConstantBank& kb = fr.constant_banks[kd.vs_bank];
            std::snprintf(nm, sizeof nm, "%s/kept_f%d_d%u.c76", dump_dir.c_str(), f, d);
            if (FILE* fo = std::fopen(nm, "wb")) { std::fwrite(reinterpret_cast<const float*>(kb.dwords) + 4 * 76, 4, 4 * 60, fo); std::fclose(fo); }
          }
        }
        continue;
      }
      ++replaced;
      const Draw& dr = fr.draws[d];
      if (corrupt_at >= 0 && !corrupt_set && f == corrupt_at) {
        corrupt_set = true, corrupt_offset = dr.vertex_offset, corrupt_stride = dec.prepared->stride, refused_before = core.stats().refused_bytes;
      }
      const int slot_i = dec.slot >= 0 && dec.slot < 15 ? dec.slot : 15;
      ++per_slot_replaced[slot_i];
      ++proof_draws;
      PieceTally piece_tally{dec.is_piece, wrong_vertex, wrong_rows, piece_proof, piece_wrong_vertex, piece_wrong_rows, wrong_vertex, wrong_rows};
      // ---- the independent proof ----
      if (dec.prepared->vertex_count != dr.vertex_count || dec.prepared->index.size() != dr.index_count) { ++wrong_count; continue; }
      const VertexLayout& lay = fr.layouts[dr.layout];
      if (dr.vertex_bytes != size_t(dec.prepared->stride) * dr.vertex_count || dec.prepared->stride != lay.host_stride) { ++wrong_vertex; continue; }
      if (!cap.ReadGeometry(dr.vertex_offset, dr.vertex_bytes, vb) || vb.size() < dr.vertex_bytes) { ++wrong_vertex; continue; }
      // every byte except the position floats (a few ulps) and the normal / tangent fields (one field step): the same comparison the product makes, rebuilt here from the layout
      {
        // roles are not exposed by the Core: compare the whole vertex bytewise and count the draw wrong only when a differing byte is not inside a position or a packed vector
        // (the layout's elements tell which bytes those are: vk 109 = position, vk 92 = packed normal / tangent)
        bool bad = false;
        int32_t tangent_dword = -1, n17 = 0;   // lane 206: the second 10:11:11 element (the higher dword) is the tangent
        for (uint32_t e = 0; e < lay.element_count; ++e) {
          const VertexElement& el = fr.elements[lay.element_first + e];
          if (el.source_format == 17) tangent_dword = std::max<int32_t>(tangent_dword, el.guest_offset), ++n17;
        }
        if (n17 < 2) tangent_dword = -1;   // review S3a: a lone 10:11:11 element is the normal
        for (uint32_t v = 0; v < dr.vertex_count && !bad; ++v) {
          const uint8_t* a = dec.prepared->vertex.data() + size_t(v) * lay.host_stride;
          const uint8_t* b = vb.data() + size_t(v) * lay.host_stride;
          if (!std::memcmp(a, b, lay.host_stride)) continue;
          for (uint32_t e = 0; e < lay.element_count && !bad; ++e) {
            const VertexElement& el = fr.elements[lay.element_first + e];
            const size_t n = el.vk_format == 109 ? 16 : el.vk_format == 106 ? 12 : el.vk_format == 92 ? 8 : 4;
            if (!std::memcmp(a + el.host_offset, b + el.host_offset, n)) continue;
            if (dec.prepared->tangent_not_in_file && el.source_format == 17 && el.guest_offset == tangent_dword) continue;   // lane 206: the file has no tangent (the long map): the game's bytes are leftovers
            if (el.vk_format == 109) {
              for (int c = 0; c < 3; ++c) {
                float fa, fb;
                std::memcpy(&fa, a + el.host_offset + 4 * c, 4), std::memcpy(&fb, b + el.host_offset + 4 * c, 4);
                if (!(std::fabs(double(fa) - double(fb)) <= 2e-6 * std::max(1.0, std::fabs(double(fb))))) bad = true;
              }
              if (std::memcmp(a + el.host_offset + 12, b + el.host_offset + 12, 4)) bad = true;
            } else if (el.vk_format == 92) {
              for (int c = 0; c < 4; ++c) {
                int16_t qa, qb;
                std::memcpy(&qa, a + el.host_offset + 2 * c, 2), std::memcpy(&qb, b + el.host_offset + 2 * c, 2);
                if (std::abs(int(qa) - int(qb)) > 70) bad = true;
              }
            } else bad = true;
          }
        }
        if (bad) ++wrong_vertex;
      }
      if (!cap.ReadGeometry(dr.index_offset, dr.index_bytes, ib) || ib.size() != dec.prepared->index.size() * 2 || std::memcmp(ib.data(), dec.prepared->index.data(), ib.size())) ++wrong_index;
      {
        const ConstantBank& bank = fr.constant_banks[dr.vs_bank];
        bool same = true;
        for (uint32_t r = 0; r < dec.rows; ++r)
          for (int c = 0; c < 4; ++c) {
            uint32_t a, b;
            std::memcpy(&a, &dec.row_data[size_t(r) * 4 + size_t(c)], 4);
            std::memcpy(&b, reinterpret_cast<const float*>(bank.dwords) + 4 * (dec.first_register + r) + c, 4);
            same = same && a == b;
          }
        if (!same) ++wrong_rows;
      }
      if (lines) std::printf("DRAW frame %d draw %u slot %d part %u rows %u\n", f, d, dec.slot, o->part, dec.rows);
    }
  }
  if (show_wants || inject) {
    bool body_sized = false;
    for (const auto& w : wants_ever) { if (show_wants) std::printf("WANT 0x%08X nodes %u\n", w.first, w.second); body_sized = body_sized || w.second >= 60; }
    std::printf("WANTS distinct=%zu body_sized=%d injected_skeleton_frames=%llu%s\n", wants_ever.size(), int(body_sized), (unsigned long long)injected_skeletons, body_sized ? " FAIL (a 60+ node want)" : "");
  }
  std::printf("DECIDE calls=%llu ms_total=%.3f ms_per_ready_frame=%.4f\n", (unsigned long long)decide_calls, double(decide_ns) / 1e6, frames_ready ? double(decide_ns) / 1e6 / double(frames_ready) : 0.0);
  if (corrupt_at >= 0) {
    std::printf("CORRUPT set=%d frame=%d detected_frame=%d\n", corrupt_set ? 1 : 0, corrupt_at, corrupt_detected);
  }
  if (lite_draws) std::fprintf(stderr, "wrestler_ours_check: %llu lite (thin) draws skipped: not decided here (lane 206)\n", (unsigned long long)lite_draws);
  if (claimed == 0) { std::fprintf(stderr, "wrestler_ours_check: FAIL no main-pass model draw was claimed in frames %d-%d (nothing to measure)\n", first, last); return 2; }
  const rdr_scene::WrestlerStats& s = core.stats();
  std::printf("ARM faults=%s frames=%llu ready=%llu\n", fault_list.empty() ? "none" : fault_list.c_str(), (unsigned long long)frames, (unsigned long long)frames_ready);
  std::printf("%s\n", core.Report().c_str());
  for (const WrestlerCore::JoinedBody& j : core.joined())
    std::printf("BODY ch%03d attire %d entry %u nodes %u scale %.9g\n", j.pac, j.attire, j.entry, j.nodes, double(j.scale));
  for (int i = 0; i < 16; ++i)
    if (s.replaced_by_slot[i] || s.palette_refused_by_slot[i])
      std::printf("SLOT %s replaced=%llu palette_refused=%llu\n", i < 15 ? std::to_string(i).c_str() : "other", (unsigned long long)s.replaced_by_slot[i], (unsigned long long)s.palette_refused_by_slot[i]);
  std::printf("RESULT claimed=%llu replaced=%llu kept_not_ready=%llu kept_unjoined=%llu kept_mismatch=%llu kept_variant=%llu kept_texture=%llu kept_palette=%llu kept_not_ours=%llu "
              "proof_draws=%llu wrong_vertex=%llu wrong_index=%llu wrong_rows=%llu wrong_count=%llu objects_joined=%llu scale_measured=%llu\n",
              (unsigned long long)claimed, (unsigned long long)replaced, (unsigned long long)kept[size_t(KeepWhy::NotReady)], (unsigned long long)kept[size_t(KeepWhy::Unjoined)],
              (unsigned long long)kept[size_t(KeepWhy::Mismatch)], (unsigned long long)kept[size_t(KeepWhy::Variant)], (unsigned long long)kept[size_t(KeepWhy::Texture)],
              (unsigned long long)kept[size_t(KeepWhy::Palette)], (unsigned long long)kept[size_t(KeepWhy::NotOurs)], (unsigned long long)proof_draws, (unsigned long long)wrong_vertex,
              (unsigned long long)wrong_index, (unsigned long long)wrong_rows, (unsigned long long)wrong_count, (unsigned long long)s.objects_joined, (unsigned long long)s.scale_measured);
  // S101 lane 48: the pieces (own node table; rows from the owner body's node of the same name)
  // (units: linked = object-frames; every other number = DRAWS; proof_* = the independent proof's wrong counts on piece draws only)
  if (refusals) {   // S101 lane 100: the Mismatch refusals, grouped
    std::map<std::string, uint64_t> g;
    for (const auto& n : core.refusal_notes()) {
      char b[256];
      std::snprintf(b, sizeof b, "obj %08X entry %u slot %d part %u verts %u/%u %s role %s first_bad_vertex %u max_err %.6g bad_values %llu ours %08X stream %08X", n.object, n.entry, n.slot, n.part, n.vertices, n.stream_vertices, n.what, n.role, n.vertex, n.max_err, (unsigned long long)n.bad, n.ours_word, n.theirs_word);
      ++g[b];
    }
    for (const std::string& c : core.candidate_notes()) std::printf("CAND %s\n", c.c_str());
    for (const auto& kv : g) std::printf("REFUSAL %llux %s\n", (unsigned long long)kv.second, kv.first.c_str());
  }
  std::printf("PIECES linked_object_frames=%llu replaced=%llu palette_refused=%llu no_owner=%llu proof_draws=%llu proof_wrong_rows=%llu proof_wrong_vertex=%llu\n",
              (unsigned long long)s.piece_objects_linked, (unsigned long long)s.piece_replaced, (unsigned long long)s.piece_palette_refused, (unsigned long long)s.piece_no_owner,
              (unsigned long long)piece_proof, (unsigned long long)piece_wrong_rows, (unsigned long long)piece_wrong_vertex);
  std::printf("run time %.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  (void)per_slot_replaced;
  (void)per_slot_palette;
  (void)per_slot_claimed;
  (void)by_part;
  return 0;
}
