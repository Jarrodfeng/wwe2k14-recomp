// Session 101, lane PO: the GPU-free half of the post chain's promotion, played over every frame of the captures (src\renderer_scene\post_ours_core.cpp, the code PostOurs calls).
//   post_ours_core_test [--faults LIST] [--scale F] [--limits no_r32f_linear] [--map-missing] [-v] LABEL=CAPTURE_FOLDER...
// One line per capture, key=value pairs (tools\post\post_ours_check.py reads them):
//   CAP label frames=N nochain=N ours=N failed=N kept_<reason>=N shape_<name>=N draws=min..max copies=min..max clears=sum targets=sum dead_draws=sum dead_copies=sum bloom0=N glare0=N dof0=N
//   first_draw_ops=..., null_inputs_frames=N
// Exit 0 when every capture opened (the numbers are judged by the Python check); 2 usage.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "renderer_adapter/capture_reader.h"
#include "renderer/shaders/spv/fx_registry.h"
#include "renderer_scene/post_ours_core.h"

using namespace rdr_scene;

namespace {
std::pair<uint32_t, uint32_t> dr_slots(const rdr_adapter::LoadedFrame& f, uint32_t draw) { return {f.draws[draw].texture_first, f.draws[draw].texture_count}; }
}  // namespace

// Lane 224: --live K mirrors what the live sink (src/renderer_adapter/live.cpp: NoteCopies, PerFrame, NoteMissed, CatchUp) does to a frame before Bind: it keeps every K-th frame as the drawn one, the
// others are "skipped" (their per-frame copies are left out and owed), and the drawn frame is handed to the post decision with the catch-up ops inserted (frame.ops = catch_up_ops). Mirror, not shared code
// (live.cpp's versions are members of the render thread); the functions to keep in step are marked "mirror".
struct LiveMirror {
  struct CopySeen { uint64_t last = 0; uint32_t streak = 0, missed = 0; };
  static constexpr uint32_t kPerFrameStreak = 3, kMaxCatchUp = 16;
  static constexpr uint64_t kCatchUpTexels = 64 * 64;
  std::map<uint64_t, CopySeen> seen;
  std::vector<rdr_adapter::Op> ops;
  uint64_t handled = 0, steps = 0, copies = 0;
  bool owed = false;
  static uint64_t Key(const rdr::CopyOp& c) {   // mirror of live.cpp CopyKey
    uint64_t k = 0xcbf29ce484222325ull;
    const auto mix = [&](uint64_t v) { k = (k ^ v) * 0x100000001b3ull; };
    mix(c.dest.image);
    mix(uint64_t(c.dest.mip) << 8 | c.dest.layer);
    mix(uint64_t(uint32_t(c.dest_rect.x)) << 32 | uint32_t(c.dest_rect.y));
    mix(uint64_t(c.dest_rect.width) << 32 | c.dest_rect.height);
    return k;
  }
  void Note(const rdr_adapter::LoadedFrame& f) {   // mirror of NoteCopies
    ++handled;
    for (const rdr::CopyOp& c : f.copies) {
      CopySeen& s = seen[Key(c)];
      if (s.last == handled) continue;
      if (s.last + 1 != handled) s.missed = 0;
      s.streak = s.last + 1 == handled ? s.streak + 1 : 1;
      s.last = handled;
    }
  }
  void Skipped(const rdr_adapter::LoadedFrame& f) {   // mirror of the skipped branch's NoteMissed
    for (const rdr_adapter::Op& op : f.ops) {
      if (op.kind != rdr_adapter::OpKind::Copy) continue;
      const rdr::CopyOp& c = f.copies[op.index];
      auto it = seen.find(Key(c));
      if (it != seen.end() && it->second.streak >= kPerFrameStreak) {
        if (it->second.missed < kMaxCatchUp) ++it->second.missed;
        owed = true;
      }
    }
  }
  // mirror of the skipped branch of live.cpp: the ops that stay in a skipped frame (every op but the per-frame copies); true when a copy is left
  bool Lasting(const rdr_adapter::LoadedFrame& f) {
    lasting_ops.clear();
    bool lasting = false;
    for (const rdr_adapter::Op& op : f.ops) {
      if (op.kind == rdr_adapter::OpKind::Copy) {
        const rdr::CopyOp& c = f.copies[op.index];
        auto it = seen.find(Key(c));
        if (it != seen.end() && it->second.streak >= kPerFrameStreak) continue;
        lasting = true;
      }
      lasting_ops.push_back(op);
    }
    return lasting;
  }
  std::vector<rdr_adapter::Op> lasting_ops;
  bool CatchUp(const rdr_adapter::LoadedFrame& frame) {   // mirror of CatchUp
    using rdr_adapter::Op; using rdr_adapter::OpKind; using rdr_adapter::Draw;
    if (!owed) return false;
    owed = false;
    ops.clear();
    bool any = false;
    const uint32_t count = frame.ops.count;
    for (uint32_t i = 0; i < count; ++i) {
      const Op& op = frame.ops[i];
      ops.push_back(op);
      if (op.kind != OpKind::Copy) continue;
      const rdr::CopyOp& c = frame.copies[op.index];
      const auto it = seen.find(Key(c));
      if (it == seen.end() || !it->second.missed) continue;
      const uint32_t n = it->second.missed;
      it->second.missed = 0;
      if (uint64_t(c.dest_rect.width) * c.dest_rect.height > kCatchUpTexels) continue;
      uint32_t j = i;
      bool reads_dest = false, renders_source = false;
      while (j > 0) {
        const Op& p = frame.ops[j - 1];
        if (p.kind == OpKind::Draw) {
          const Draw& d = frame.draws[p.index];
          for (uint32_t s = 0; s < d.texture_count; ++s) reads_dest |= frame.texture_slots[d.texture_first + s].image == c.dest.image;
          --j;
          continue;
        }
        if (p.kind == OpKind::Clear) { --j; continue; }
        if (p.kind == OpKind::SetTargets) {
          const rdr::TargetsOp& t = frame.targets[p.index];
          renders_source = t.color_count && t.color[0].image == c.source.image;
          --j;
        }
        break;
      }
      if (!renders_source || !reads_dest) continue;
      for (uint32_t r = 0; r < n; ++r) ops.insert(ops.end(), frame.ops.data + j, frame.ops.data + i + 1);
      steps += n, ++copies;
      any = true;
    }
    return any;
  }
};

int main(int argc, char** argv) {
  PostFaults faults;
  PostLimits limits;
  bool verbose = false, map_missing = false, no_dof = false, no_film = false;
  std::vector<std::pair<std::string, std::string>> caps;
  int live_k = 0;        // lane 224: --live K
  bool detail = false;
  bool lasting_too = false;   // lane 224: --lasting: a skipped frame is decided too, as the persistent-only bind of live.cpp sees it (its per-frame copies left out)
  uint32_t lasting_frames = 0, lasting_ours = 0, lasting_nochain = 0;
  std::map<std::string, uint32_t> lasting_kept;
  std::map<std::string, uint32_t> lasting_detail;   // lane 224: --detail prints every refusal with its sequence
  std::vector<char> drawn_pattern;   // lane 224: --drawn timing.csv: frame i is drawn when row i has a render_ms (the real skip pattern of the run), with --live
  int dump_frame = -1;
  std::vector<uint32_t> ops_imgs;   // lane 224: --img N (repeatable): with --dump, list every op of the frame that targets, copies or samples image N
   // lane 140: --dump N prints frame N's chain draws (kind, target, slots)
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--faults" && i + 1 < argc) {
      std::string error;
      if (!PostFaults::Parse(argv[++i], faults, error)) { std::printf("usage: %s\n", error.c_str()); return 2; }
    } else if (a == "--scale" && i + 1 < argc) limits.render_scale = float(std::atof(argv[++i]));
    else if (a == "--limits" && i + 1 < argc) { if (std::string(argv[++i]) == "no_r32f_linear") limits.no_r32f_linear = true; }
    else if (a == "--map-missing") map_missing = true;
    else if (a == "--no-dof") no_dof = true;
    else if (a == "--no-film") no_film = true;
    else if (a == "-v") verbose = true;
    else if (a == "--live" && i + 1 < argc) live_k = std::atoi(argv[++i]);
    else if (a == "--detail") detail = true;
    else if (a == "--lasting") lasting_too = true;
    else if (a == "--drawn" && i + 1 < argc) {   // lane 224: the run's own drawn / skipped pattern (case zip timing.csv: guest_frame,step_ms,render_ms,...)
      std::FILE* tf = std::fopen(argv[++i], "r");
      if (!tf) { std::printf("FAIL  input: cannot open %s\n", argv[i]); return 1; }
      char line[512];
      bool first = true;
      while (std::fgets(line, sizeof line, tf)) {
        if (first) { first = false; continue; }
        int commas = 0; const char* q = line; const char* f3 = nullptr;
        for (; *q; ++q) if (*q == ',' && ++commas == 2) { f3 = q + 1; break; }
        drawn_pattern.push_back(f3 && *f3 != ',' && *f3 != '\n' && *f3 != '\r' ? 1 : 0);
      }
      std::fclose(tf);
    }
    else if (a == "--img" && i + 1 < argc) ops_imgs.push_back(uint32_t(std::atoi(argv[++i])));
    else if (a == "--dump" && i + 1 < argc) dump_frame = std::atoi(argv[++i]);   // lane 140
    else if (a.find('=') != std::string::npos) caps.emplace_back(a.substr(0, a.find('=')), a.substr(a.find('=') + 1));
    else { std::printf("usage: unknown argument %s\n", a.c_str()); return 2; }
  }
  if (caps.empty()) { std::printf("FAIL  input: no captures given (a check that reads nothing must fail)\n"); return 1; }
  int rc = 0;
  for (const auto& cp : caps) {
    rdr_adapter::CaptureReader reader;
    if (!reader.Open(cp.second)) { std::printf("FAIL  input: cannot open capture %s\n", cp.second.c_str()); rc = 1; continue; }
    uint32_t frames = 0, nochain = 0, ours = 0, failed = 0, bad = 0, null_frames = 0;
    uint32_t dmin = ~0u, dmax = 0, cmin = ~0u, cmax = 0, kmin = ~0u, kmax = 0;
    uint64_t depth_clears = 0, kept_copies = 0, repeat_slots = 0, clears = 0, targets = 0, dead_draws = 0, dead_copies = 0, bloom0 = 0, glare0 = 0, dof0 = 0, far_frames = 0;
    std::map<std::string, uint32_t> kept, shape, first_kept;
    uint32_t merge_point = 0, merge_point_first = ~0u, merge_point_mask = 0;   // lane 238: frames whose m1_m2 merge binds a slot POINT (run 7131: slot 3)
    std::map<std::string, uint32_t> seq_hist, seq_first;   // lane 238: SEQ lines: how many frames carry each chain sequence, and whether it is ours
    std::vector<uint32_t> l_t2, l_restore, l_repeat, l_change;   // lane 140: frame lists the pictures check shoots (t2 blur, a restore in front, REPEAT dof depth, a change of the chain sequence)
    std::string prev_seq = "?";
    LiveMirror live;
    for (uint32_t fi = 0; fi < reader.frame_count(); ++fi) {
      rdr_adapter::LoadedFrame f;
      if (!reader.LoadFrame(fi, f)) { ++bad; continue; }
      // the shipped path must not need the vertex-program hash that marked the chain's end
      for (uint32_t d = 0; d < f.draws.count; ++d) const_cast<rdr_adapter::Draw&>(f.draws[d]).vs_patched_hash = 0;
      if (live_k > 0) {   // lane 224: what the live sink does before Bind
        live.Note(f);
        const bool is_drawn = !drawn_pattern.empty() ? (fi < drawn_pattern.size() ? drawn_pattern[fi] != 0 : true) : fi % uint32_t(live_k) == 0;
        if (!is_drawn) {
          if (!lasting_too) { live.Skipped(f); continue; }
          const bool has_lasting = live.Lasting(f);
          live.Skipped(f);
          if (!has_lasting) continue;
          f.ops = rdr_adapter::Table<rdr_adapter::Op>{live.lasting_ops.data(), uint32_t(live.lasting_ops.size())};
          rdr_adapter::PostPlanIn lin;
          lin.frame = &f;
          lin.depth_of_field = !no_dof, lin.film_grain = !no_film;
          lin.info = [&reader](uint32_t id) { rdr_adapter::PostImageInfo i; const rdr_adapter::ImageRecord* r = reader.FindImage(id); if (!r) return i; i.width = r->width, i.height = r->height, i.scaled = (r->usage & rdr::kUsageScaled) != 0, i.known = true; return i; };
          lin.map = [&reader](uint32_t id) { return reader.FindImage(id) ? rdr::ImageId(id) : rdr::kNoImage; };
          const PostDecision ld = DecidePost(lin, faults, limits);
          ++lasting_frames;
          if (ld.no_chain) ++lasting_nochain;
          else if (ld.ok) ++lasting_ours;
          else { ++lasting_kept[PostKeepName(ld.keep)]; ++lasting_detail[std::string(PostKeepName(ld.keep)) + ": " + ld.detail.substr(0, 90) + " [" + ld.sequence + "]"]; }
          continue;
        }
        if (live.CatchUp(f)) f.ops = rdr_adapter::Table<rdr_adapter::Op>{live.ops.data(), uint32_t(live.ops.size())};
      }
      ++frames;
      rdr_adapter::PostPlanIn in;
      in.frame = &f;
      in.depth_of_field = !no_dof, in.film_grain = !no_film;
      in.info = [&reader](uint32_t id) {
        rdr_adapter::PostImageInfo i;
        const rdr_adapter::ImageRecord* r = reader.FindImage(id);
        if (!r) return i;
        i.width = r->width, i.height = r->height, i.scaled = (r->usage & rdr::kUsageScaled) != 0, i.known = true;
        return i;
      };
      in.map = [&reader, map_missing](uint32_t id) { return !map_missing && reader.FindImage(id) ? rdr::ImageId(id) : rdr::kNoImage; };
      const PostDecision d = DecidePost(in, faults, limits);
      if (int(fi) == dump_frame) {   // lane 140
        StreamChain sc; std::string err;
        ReadStreamChain(reader, f, sc, err);
        for (size_t k = 0; k < sc.draws.size(); ++k) {
          const ChainDraw& c = sc.draws[k];
          std::printf("  D%zu %s [%s] target=%u %ux%u slots:", k, ChainKindName(c.kind), c.ps_variant >= 0 ? rdr::fx::kVariants[c.ps_variant].name : "?", c.target, c.target_w, c.target_h);
          for (const ChainSlot& s : c.slots) std::printf(" s%d=img%u/f%u/a%u%u/w%d", s.slot, s.image, s.filter, s.addr_u, s.addr_v, s.writer);
          std::printf("  c20=%g c31=%g c32=%g", c.regs[20][0], c.regs[31][0], c.regs[32][0]);
          {   // lane 238: the draw's own pipeline state (blend of target 0, depth test) and draw index
            const rdr_adapter::Draw& dr = f.draws[c.draw];
            const rdr::DrawState& st = f.fixeds[dr.fixed].state;
            std::printf(" draw=%u op=%u blend=%u/%u/%u/%u mask=%02x depth_test=%u", c.draw, c.op, st.blend[0].enable, st.blend[0].src_color, st.blend[0].dst_color, st.blend[0].color_op, st.blend[0].write_mask, st.depth_test);
            const rdr::DynamicState& dy = f.dynamics[dr.dynamic].host;
            std::printf(" vp=%g,%g,%gx%g sc=%d,%d,%ux%u verts=%u idx=%u topo=%u", dy.viewport[0], dy.viewport[1], dy.viewport[2], dy.viewport[3], dy.scissor.x, dy.scissor.y, dy.scissor.width, dy.scissor.height, dr.vertex_count, dr.index_count, st.topology);
          }
          for (uint32_t s = 0; s < dr_slots(f, c.draw).second; ++s) {   // lane 238: the fetch facts the stream binds (swizzle, sign, view format ...): our pass samples the plain image
            const rdr_adapter::TextureSlot& ts = f.texture_slots[dr_slots(f, c.draw).first + s];
            if (ts.view_format || ts.swizzle[0] || ts.swizzle[1] || ts.swizzle[2] || ts.swizzle[3] || ts.sign[0] || ts.sign[1] || ts.sign[2] || ts.sign[3] || ts.target16_read || ts.fetch_lod_bias != 0.f || ts.t21_exp || ts.word3_exp)
              std::printf(" [s%d view=%u sw=%u%u%u%u sign=%u%u%u%u t16=%u lodbias=%g t21=%d w3=%d]", ts.slot, ts.view_format, ts.swizzle[0], ts.swizzle[1], ts.swizzle[2], ts.swizzle[3], ts.sign[0], ts.sign[1], ts.sign[2], ts.sign[3], ts.target16_read, ts.fetch_lod_bias, ts.t21_exp, ts.word3_exp);
          }
          std::printf("\n");
        }
        for (const ChainCopy& cc : sc.copies) std::printf("  copy op%u %u->%u\n", cc.op, cc.src, cc.dst);
        if (!ops_imgs.empty()) {   // lane 224: the ops that touch the given images, in stream order
          const auto want = [&](uint32_t id) { return std::find(ops_imgs.begin(), ops_imgs.end(), id) != ops_imgs.end(); };
          uint32_t cur = 0;
          for (uint32_t k = 0; k < f.ops.count; ++k) {
            const rdr_adapter::Op& op = f.ops[k];
            if (op.kind == rdr_adapter::OpKind::SetTargets) {
              const rdr::TargetsOp& t = f.targets[op.index];
              cur = t.color_count ? t.color[0].image : t.depth.image;
              if (want(cur)) std::printf("  op%u targets -> %u\n", k, cur);
            } else if (op.kind == rdr_adapter::OpKind::Copy) {
              const rdr::CopyOp& c = f.copies[op.index];
              if (want(c.source.image) || want(c.dest.image)) std::printf("  op%u copy %u -> %u\n", k, uint32_t(c.source.image), uint32_t(c.dest.image));
            } else if (op.kind == rdr_adapter::OpKind::Clear) {
              if (want(cur)) std::printf("  op%u clear (target %u)\n", k, cur);
            } else if (op.kind == rdr_adapter::OpKind::Draw) {
              const rdr_adapter::Draw& dr = f.draws[op.index];
              bool reads = false;
              for (uint32_t q = 0; q < dr.texture_count; ++q) reads |= want(f.texture_slots[dr.texture_first + q].image);
              if (reads || want(cur)) {
                std::printf("  op%u draw %u target %u reads", k, op.index, cur);
                for (uint32_t q = 0; q < dr.texture_count; ++q) std::printf(" %u", uint32_t(f.texture_slots[dr.texture_first + q].image));
                std::printf("\n");
              }
            }
          }
        }
        std::printf("  decision: %s keep=%s %s\n", d.ok ? "ok" : "refused", PostKeepName(d.keep), d.detail.c_str());
      }
      {   // lane 140
        const std::string seq = d.no_chain ? std::string() : d.sequence;
        if (seq != prev_seq && prev_seq != "?") l_change.push_back(fi);
        prev_seq = seq;
        if (!d.no_chain && d.plan.ok && d.plan.t2_blur) l_t2.push_back(fi);
        if (!d.no_chain && d.restores_before) l_restore.push_back(fi);
        if (!d.no_chain && d.dof_depth_repeat) l_repeat.push_back(fi);
      }
      if (d.no_chain) { ++nochain; continue; }
      if (d.plan.ok && d.plan.hdr_drawn) {
        uint32_t m = 0;
        for (int sl = 0; sl < 4; ++sl) if (!d.plan.params.glare_merge_linear[sl]) m |= 1u << sl;
        if (m) { if (!merge_point++) merge_point_first = fi; merge_point_mask |= m; }
      }
      {
        const std::string key = d.sequence + (d.ok ? std::string(" => ours") : std::string(" => ") + PostKeepName(d.keep) + ": " + d.detail.substr(0, 90));
        if (!seq_hist[key]++) seq_first[key] = fi;
      }
      shape[PostShapeName(d.shape)]++;
      if (d.failed) {
        ++failed;
        const std::string key = std::string(PostKeepName(d.keep));
        kept[key]++;
        if (!first_kept.count(key)) first_kept[key] = fi;
        if (verbose || detail) std::printf("  frame %u: %s (%s) [%s]\n", fi, PostKeepName(d.keep), d.detail.c_str(), d.sequence.c_str());
      }
      if (!d.ok) continue;
      ++ours;
      dmin = std::min(dmin, d.claim.draws), dmax = std::max(dmax, d.claim.draws);
      kmin = std::min(kmin, d.stream_chain_draws - d.claim.draws), kmax = std::max(kmax, d.stream_chain_draws - d.claim.draws);
      cmin = std::min(cmin, d.claim.copies), cmax = std::max(cmax, d.claim.copies);
      clears += d.claim.clears, targets += d.claim.targets, depth_clears += d.claim.depth_clears_dropped, kept_copies += d.claim.copies_kept, repeat_slots += d.slot_repeat;
      dead_draws += d.dead_draws, dead_copies += d.dead_copies, bloom0 += d.bloom0, glare0 += d.glare0, dof0 += d.dof0;
      far_frames += d.far_blur ? 1 : 0;
      null_frames += d.plan.null_inputs ? 1 : 0;
    }
    if (live_k > 0) std::printf("  live mirror K=%d: catch-up steps %llu in %llu copies\n", live_k, (unsigned long long)live.steps, (unsigned long long)live.copies);
    if (lasting_too) {
      std::printf("  lasting binds (skipped frames with lasting work, per-frame copies left out): %u, no chain %u, ours %u", lasting_frames, lasting_nochain, lasting_ours);
      for (const auto& k : lasting_kept) std::printf(", kept_%s %u", k.first.c_str(), k.second);
      std::printf("\n");
      for (const auto& k : lasting_detail) std::printf("    %u x %s\n", k.second, k.first.c_str());
    }
    if (!ours) dmin = cmin = kmin = 0;
    std::printf("CAP %s frames=%u nochain=%u ours=%u failed=%u bad_read=%u draws=%u..%u stays=%u..%u copies=%u..%u clears=%llu targets=%llu dead_draws=%llu dead_copies=%llu bloom0=%llu glare0=%llu dof0=%llu far_blur_frames=%llu",
                cp.first.c_str(), frames, nochain, ours, failed, bad, dmin, dmax, kmin, kmax, cmin, cmax, (unsigned long long)clears, (unsigned long long)targets, (unsigned long long)dead_draws,
                (unsigned long long)dead_copies, (unsigned long long)bloom0, (unsigned long long)glare0, (unsigned long long)dof0, (unsigned long long)far_frames);
    std::printf(" depth_clears_dropped=%llu copies_kept=%llu nonclamp_slots=%llu", (unsigned long long)depth_clears, (unsigned long long)kept_copies, (unsigned long long)repeat_slots);
    for (const auto& s : shape) std::printf(" shape_%s=%u", s.first.c_str(), s.second);
    for (const auto& k : kept) std::printf(" kept_%s=%u", k.first.c_str(), k.second);
    for (const auto& k : first_kept) std::printf(" first_%s=%u", k.first.c_str(), k.second);
    std::printf("\n");
    const auto list = [&](const char* name, const std::vector<uint32_t>& v) {   // lane 140: LIST label name idx,idx,...
      std::printf("LIST %s %s", cp.first.c_str(), name);
      for (size_t i = 0; i < v.size(); ++i) std::printf("%c%u", i ? ',' : ' ', v[i]);
      std::printf("\n");
    };
    if (merge_point) std::printf("MERGE %s frames=%u first=%u slots_mask=%u\n", cp.first.c_str(), merge_point, merge_point_first, merge_point_mask);
    for (const auto& q : seq_hist) std::printf("SEQ %s %u first=%u | %s\n", cp.first.c_str(), q.second, seq_first[q.first], q.first.c_str());   // lane 238
    list("t2", l_t2), list("restore", l_restore), list("repeat", l_repeat), list("change", l_change);
    if (bad) rc = 1;
  }
  return rc;
}
