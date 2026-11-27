
// ---- compile: the whole frame is known before a single Vulkan call ----------------------------------------------------------------

void Renderer::Impl::Compile(const Frame& frame) {
  passes.clear();
  steps.clear();
  sampled.clear();
  sd_on = g_stencil_dead.load(std::memory_order_relaxed);   // S102 R24-C: read every frame
  depth_only_copies.clear();
  if (sd_on) g_sd_frames.fetch_add(1, std::memory_order_relaxed);
  clear_absorbed.assign(frame.clears.count, 0);
  const uint32_t image_limit = images.high_water();
  // A frame's first part (split at a read-back copy): the rest of the frame reads everything after it.
  next_use.assign(image_limit, frame.keep_contents ? kNextRead : kNextNone);
  stencil_needed.assign(image_limit, frame.keep_contents ? 1 : 0);
  state_needed.assign(image_limit, 0);
  if (image_stamp.size() < image_limit) {
    image_stamp.resize(image_limit, 0);
    sampled_index.resize(image_limit, 0);
    written_stamp.resize(image_limit, 0);
  }
  const uint32_t frame_stamp = ++write_stamp;

  const TargetsOp* current = nullptr;
  int open = -1;  // index into passes
  bool pass_has_work = false;
  trimmed_targets.clear();
  const auto close = [&](uint32_t end_op) {
    if (open >= 0) {
      PassPlan& plan = passes[size_t(open)];
      plan.end_op = end_op;
      // S100: a depth surface the game left bound but no draw or clear of this pass touches is not attached. The 360 never minds a
      // smaller one (EDRAM is addressed by pitch); a Vulkan pass covers only what every attachment covers, so Bobby Heenan's 2048x1024
      // attire build drawn with the scene's 1280x720 depth bound came out cut at 1280x720. Its contents stay as they were either way.
      if (plan.targets->depth.image != kNoImage && plan.targets->color_count && !plan.depth_used && !plan.depth_cleared) {
        trimmed_targets.push_back(*plan.targets);
        trimmed_targets.back().depth = ImageRef{};
        plan.targets = &trimmed_targets.back();
        plan.width = plan.height = 0xFFFFFFFFu;
        for (uint32_t c = 0; c < plan.targets->color_count; ++c) {
          const ImageRef& ref = plan.targets->color[c];
          const ImageEntry& entry = images[ref.image];
          plan.width = std::min(plan.width, std::max(1u, entry.width >> ref.mip));
          plan.height = std::min(plan.height, std::max(1u, entry.height >> ref.mip));
        }
      }
    }
    open = -1;
    pass_has_work = false;
  };
  const auto open_pass = [&](uint32_t op_index) {
    PassPlan plan;
    plan.targets = current;
    plan.first_op = op_index;
    plan.first_sampled = uint32_t(sampled.size());
    for (uint32_t i = 0; i < kAttachmentSlots; ++i) {
      plan.load[i] = VK_ATTACHMENT_LOAD_OP_LOAD;
      plan.store[i] = VK_ATTACHMENT_STORE_OP_STORE;
    }
    // S102 C1 (review of G14: "LOAD makes the cube carry state"; C1 review F3): a layered pass (the pre-light cube bake) loads CLEAR to 0: the cube then depends on no earlier content (no state to keep alive across
    // frames, no read of the old layers), and a texel the draw does not cover (an edge the top-left rule leaves out, a tiler's undefined tile contents) is 0 as the old strip's was, never garbage. The offline sentinel
    // (g_layered_clear_sentinel) clears magenta: a pass that covers every texel gives the same cube either way.
    for (uint32_t c = 0; c < current->color_count; ++c)
      if (ViewCount(current->color[c]) > 1) {
        plan.load[c] = VK_ATTACHMENT_LOAD_OP_CLEAR;
        const bool sentinel = g_layered_clear_sentinel.load(std::memory_order_relaxed);
        plan.clear[c].color.float32[0] = sentinel ? 1.0f : 0.0f, plan.clear[c].color.float32[1] = 0.0f, plan.clear[c].color.float32[2] = sentinel ? 1.0f : 0.0f, plan.clear[c].color.float32[3] = sentinel ? 1.0f : 0.0f;
      }
    // The render area is what every attachment can cover.
    plan.width = plan.height = 0xFFFFFFFFu;
    const auto limit = [&](const ImageRef& ref) {
      const ImageEntry& entry = images[ref.image];
      plan.width = std::min(plan.width, std::max(1u, entry.width >> ref.mip));
      plan.height = std::min(plan.height, std::max(1u, entry.height >> ref.mip));
    };
    for (uint32_t c = 0; c < current->color_count; ++c) limit(current->color[c]);
    if (current->depth.image != kNoImage) limit(current->depth);
    passes.push_back(plan);
    open = int(passes.size()) - 1;
    steps.push_back({StepKind::Pass, uint32_t(open), false});
    ++stamp;
  };

  for (uint32_t i = 0; i < frame.ops.count; ++i) {
    const Op& op = frame.ops[i];
    switch (op.kind) {
      case OpKind::Targets: {
        const TargetsOp* next = &frame.targets[op.index];
        if (const char* why = LayeredTargetsProblem(*next)) {   // S102 C1 review F8: a malformed layered target (layers past the image, beside a depth or another colour, pad garbage) is drawn as ONE view, counted
          Problem("layered_target_bad", "op %u: %s (drawn as an ordinary target)", i, why);
          trimmed_targets.push_back(*next);
          TargetsOp& fixed = trimmed_targets.back();
          for (uint32_t c = 0; c < kMaxColorTargets; ++c) fixed.color[c].pad = 0;
          fixed.depth.pad = 0;
          next = &fixed;
        }
        if (open >= 0 && std::memcmp(next, current, sizeof(TargetsOp)) != 0) close(i);
        current = next;
        break;
      }
      case OpKind::Clear: {
        if (!current) {
          Problem("clear_without_targets", "op %u", i);
          break;
        }
        if (open < 0) open_pass(i);
        const ClearOp& clear = frame.clears[op.index];
        PassPlan& plan = passes[size_t(open)];
        if (g_depth_census_on.load(std::memory_order_relaxed) && current->depth.image != kNoImage && (clear.flags & kClearStencil)) {   // S102 R24-C: the stencil census (fview / rplay only)
          DepthCensusSlot& cs = DepthCensusFor(current->depth.image);
          cs.stencil_clears.fetch_add(1, std::memory_order_relaxed);
          if (clear.stencil != 0) cs.stencil_clears_nonzero.fetch_add(1, std::memory_order_relaxed);
        }
        if (clear.full && !pass_has_work) {
          // A full clear before anything else in the pass becomes the attachment's load op.
          clear_absorbed[op.index] = 1;
          if (clear.flags & kClearColor) {
            for (uint32_t c = 0; c < current->color_count; ++c) {
              if (!(clear.color_mask & (1u << c))) continue;
              plan.load[c] = VK_ATTACHMENT_LOAD_OP_CLEAR;
              std::memcpy(plan.clear[c].color.float32, clear.color, sizeof(clear.color));
            }
          }
          if (current->depth.image != kNoImage) {
            if (clear.flags & kClearDepth) {
              plan.load[kMaxColorTargets] = VK_ATTACHMENT_LOAD_OP_CLEAR;
              plan.clear[kMaxColorTargets].depthStencil.depth = clear.depth;
              plan.depth_cleared = true;
            }
            if (clear.flags & kClearStencil) {
              plan.stencil_load = VK_ATTACHMENT_LOAD_OP_CLEAR;
              plan.clear[kMaxColorTargets].depthStencil.stencil = clear.stencil;
              plan.depth_cleared = true;
            }
          }
        } else {
          pass_has_work = true;
          if (clear.flags & kClearStencil) plan.stencil_clear_in = true;
          if ((clear.flags & kClearStencil) && !sd_on) plan.stencil_used = true;   // S102 R24-C: with stencil_dead a clear is a write, not a use (the pass's load / store and the clear itself die when nothing reads)
          if (clear.flags & (kClearDepth | kClearStencil)) plan.depth_used = true;
        }
        break;
      }
      case OpKind::Draw: {
        if (!current) {
          Problem("draw_without_targets", "op %u", i);
          break;
        }
        if (open < 0) open_pass(i);
        pass_has_work = true;
        const DrawOp& draw = frame.draws[op.index];
        const TextureSet& textures = frame.texture_sets[draw.textures];
        PassPlan& plan = passes[size_t(open)];
        const DrawState& draw_state = frame.states[draw.state];
        if (draw_state.stencil_test) plan.stencil_used = true, plan.stencil_tests = true;
        if (g_depth_census_on.load(std::memory_order_relaxed) && current->depth.image != kNoImage) {   // S102 R24-C: the stencil census (fview / rplay only)
          DepthCensusSlot& cs = DepthCensusFor(current->depth.image);
          const ImageEntry& de = images[current->depth.image];
          cs.width.store(de.desc.width, std::memory_order_relaxed), cs.height.store(de.desc.height, std::memory_order_relaxed);
          cs.draws.fetch_add(1, std::memory_order_relaxed);
          if (draw_state.stencil_test) {
            cs.stencil_test_draws.fetch_add(1, std::memory_order_relaxed);
            const StencilFaceView fv{draw_state.front.fail, draw_state.front.pass, draw_state.front.depth_fail, draw_state.front.compare};
            const StencilFaceView bv{draw_state.back.fail, draw_state.back.pass, draw_state.back.depth_fail, draw_state.back.compare};
            if (DrawNeedsStencil(true, fv, bv)) {
              cs.stencil_effect_draws.fetch_add(1, std::memory_order_relaxed);
              if (FaceWrites(fv) || FaceWrites(bv)) cs.stencil_write_draws.fetch_add(1, std::memory_order_relaxed);
              const uint8_t cls = current->color_count && current->color[0].image != kNoImage ? std::atomic_ref<uint8_t>(images[current->color[0].image].gpu_class).load(std::memory_order_relaxed) : uint8_t(31);
              cs.use_classes.fetch_or(1u << cls, std::memory_order_relaxed);
            }
          }
        }
        if (draw_state.depth_test || draw_state.depth_write || draw_state.stencil_test) plan.depth_used = true;
        for (const TextureSlot& texture : textures.slots) {
          if (texture.image == kNoImage) continue;
          ImageEntry& entry = images[texture.image];
          if (!(entry.desc.usage & kUsageTarget) && !entry.copy_dest) continue;
          if (image_stamp[texture.image] == stamp) {
            sampled[sampled_index[texture.image]].vertex |= texture.vertex != 0;
            continue;
          }
          image_stamp[texture.image] = stamp;
          sampled_index[texture.image] = uint32_t(sampled.size());
          sampled.push_back({texture.image, texture.vertex != 0});
          ++plan.sampled_count;
        }
        break;
      }
      case OpKind::Copy:
        if (const CopyOp& cp = frame.copies[op.index]; (cp.source.image != kNoImage && (cp.source.image >= image_limit || !images[cp.source.image].live)) ||
                                                        (cp.dest.image != kNoImage && (cp.dest.image >= image_limit || !images[cp.dest.image].live))) {
          // S102 R21-B: a copy naming a destroyed image (a release that did not wait for the frames that use it) is dropped whole and named, never recorded
          // with a null VkImage (a driver crash). It must never happen; the stage split's planted twin split_no_release_defer makes it happen.
          Problem("copy_dead_image", "op %u: copy img%u -> img%u names an image that is not there", i, unsigned(cp.source.image), unsigned(cp.dest.image));
          break;
        }
        close(i);
        images[frame.copies[op.index].dest.image].copy_dest = true;
        steps.push_back({StepKind::Copy, op.index, false});
        break;
      case OpKind::Present:
        close(i);
        steps.push_back({StepKind::Present, op.index, false});
        break;
      case OpKind::Dispatch: {   // S102 L1b: a compute dispatch is a step of its own: it ends the open pass (the next draw opens a new one), and the cubes it writes are checked for layout when sampled
        const DispatchOp& d = frame.dispatches[op.index];
        bool bad = d.cube_count == 0 || d.cube_count > uint32_t(kMaxLightCubes);   // review F5: a dispatch naming a missing image is dropped whole, never indexed
        for (uint32_t k = 0; k < d.cube_count && k < uint32_t(kMaxLightCubes) && !bad; ++k) bad = d.cube[k] == kNoImage || d.cube[k] >= image_limit || !images[d.cube[k]].live;
        if (bad) {
          Problem("light_cube_bad", "op %u: a dispatch of %u cubes names an image that is not there", i, d.cube_count);
          break;
        }
        close(i);
        for (uint32_t k = 0; k < d.cube_count; ++k) images[d.cube[k]].copy_dest = true;
        steps.push_back({StepKind::Dispatch, op.index, false});
        break;
      }
    }
  }
  close(frame.ops.count);

  // Walk backwards: what happens next to each image decides what must be kept.
  const auto single = [&](const ImageEntry& entry) { return entry.desc.mips == 1 && entry.layers == 1; };
  for (size_t s = steps.size(); s-- > 0;) {
    const Step& step = steps[s];
    if (step.kind == StepKind::Pass) {
      PassPlan& plan = passes[step.index];
      const auto decide = [&](ImageId id, uint32_t slot_index) {
        const ImageEntry& entry = images[id];
        const uint8_t next = next_use[id];
        const bool dead = (entry.desc.usage & kUsageTransient) || (next == kNextOverwrite && single(entry)) || (next == kNextNone && (entry.desc.usage & kUsageDeadAtFrameEnd));
        plan.store[slot_index] = dead ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
        if ((entry.desc.usage & kUsageTransient) && plan.load[slot_index] == VK_ATTACHMENT_LOAD_OP_LOAD) {
          Problem("transient_needs_contents", "a transient target is loaded instead of cleared (image %u)", unsigned(id));
        }
      };
      for (uint32_t c = 0; c < plan.targets->color_count; ++c) decide(plan.targets->color[c].image, c);
      const ImageId depth = plan.targets->depth.image;
      if (depth != kNoImage) {
        decide(depth, kMaxColorTargets);
        // Stencil is rare: it is kept only if a later pass really uses it.
        plan.stencil_after = stencil_needed[depth] != 0;   // S102 R24-C
        const bool had_abs_clear = plan.stencil_load == VK_ATTACHMENT_LOAD_OP_CLEAR;   // the game's own absorbed stencil clear (the old rule kept it as a CLEAR load, not a LOAD)
        const bool poison = sd_on && g_stencil_dead_poison.load(std::memory_order_relaxed) && (images[depth].aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
        if (sd_on && plan.stencil_clear_in) {   // (b): counted here, once per pass (not while recording: a Persistent + Display frame records a pass twice)
          plan.strip_stencil_clear = !plan.stencil_used && (!plan.stencil_after || g_stencil_dead_plant.load(std::memory_order_relaxed) == 3);
          (plan.strip_stencil_clear ? g_sd_in_stripped : g_sd_in_kept).fetch_add(1, std::memory_order_relaxed);
        }
        if (sd_on && plan.stencil_load == VK_ATTACHMENT_LOAD_OP_CLEAR) {   // S102 R24-C (a): an absorbed stencil clear nothing reads is not done
          const bool wrong = g_stencil_dead_plant.load(std::memory_order_relaxed) == 1;   // plant 1: dropped even before a stencil test
          if (wrong || (!plan.stencil_used && !plan.stencil_after)) {
            plan.stencil_load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            if (poison) plan.stencil_load = VK_ATTACHMENT_LOAD_OP_CLEAR, plan.clear[kMaxColorTargets].depthStencil.stencil = 0xA5, plan.sd_poisoned_load = true;   // poison: the stencil the clear would have made is garbage
            g_sd_abs_dropped.fetch_add(1, std::memory_order_relaxed);
            if (wrong) stencil_needed[depth] = 0;
          } else {
            g_sd_abs_kept.fetch_add(1, std::memory_order_relaxed);
          }
        }
        const bool cleared = plan.stencil_load == VK_ATTACHMENT_LOAD_OP_CLEAR;
        const bool needed_before = !cleared && (plan.stencil_used || stencil_needed[depth]);
        plan.stencil_store = stencil_needed[depth] ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        if (!cleared) plan.stencil_load = needed_before ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        if (sd_on && !had_abs_clear && !cleared && plan.stencil_clear_in && !needed_before) {   // the load the old rule kept (the in-pass clear counted as a use) and this one does not
          g_sd_loads_dead.fetch_add(1, std::memory_order_relaxed);
          if (poison) plan.stencil_load = VK_ATTACHMENT_LOAD_OP_CLEAR, plan.clear[kMaxColorTargets].depthStencil.stencil = 0xA5, plan.sd_poisoned_load = true;
        }
        stencil_needed[depth] = single(images[depth]) ? needed_before : (needed_before || stencil_needed[depth]);   // S102 R24-C review F5: a pass writes one mip / layer; only a 1-mip 1-layer image's need ends here
      }
      for (uint32_t c = 0; c < plan.targets->color_count; ++c) next_use[plan.targets->color[c].image] = plan.load[c] == VK_ATTACHMENT_LOAD_OP_CLEAR ? kNextOverwrite : kNextRead;
      if (depth != kNoImage) next_use[depth] = plan.load[kMaxColorTargets] == VK_ATTACHMENT_LOAD_OP_CLEAR ? kNextOverwrite : kNextRead;
      for (uint32_t k = 0; k < plan.sampled_count; ++k) next_use[sampled[plan.first_sampled + k].image] = kNextRead;
    } else if (step.kind == StepKind::Copy) {
      const CopyOp& copy = frame.copies[step.index];
      const ImageEntry& dest = images[copy.dest.image];
      const bool whole = copy.dest_rect.x == 0 && copy.dest_rect.y == 0 && copy.dest_rect.width >= dest.desc.width && copy.dest_rect.height >= dest.desc.height;
      if (dest.depth && images[copy.source.image].depth) {   // a depth copy (both aspects when the switch is off) carries the stencil: what reads the destination's stencil later needs the source's
        const bool keep = stencil_needed[copy.dest.image] != 0;
        if (sd_on && (g_stencil_dead_plant.load(std::memory_order_relaxed) == 2 || !keep)) {   // S102 R24-C (c): with stencil_dead it takes the depth aspect only when nothing reads the destination's stencil
          depth_only_copies.push_back(&copy);
          g_sd_copy_depth_only.fetch_add(1, std::memory_order_relaxed);
        } else {
          if (sd_on) g_sd_copy_whole.fetch_add(1, std::memory_order_relaxed);
          if (keep) {   // S102 R24-C review F2: also with the switch OFF (the pre-existing walk dropped the source's need: its store was DONT_CARE)
            if (whole && single(dest)) stencil_needed[copy.dest.image] = 0;   // the copy writes the destination's whole stencil: nothing before it matters there
            if (g_stencil_dead_plant.load(std::memory_order_relaxed) != 4) stencil_needed[copy.source.image] = 1;   // ... but the source's stencil is what it carries (plant 4: not marked)
          }
        }
      }
      next_use[copy.dest.image] = whole && single(dest) ? kNextOverwrite : kNextRead;
      next_use[copy.source.image] = kNextRead;
    } else if (step.kind == StepKind::Dispatch) {   // S102 L1b: the dispatch writes every texel of every cube it names
      const DispatchOp& d = frame.dispatches[step.index];
      for (uint32_t k = 0; k < d.cube_count && k < uint32_t(kMaxLightCubes); ++k) next_use[d.cube[k]] = kNextOverwrite;
    } else {
      next_use[frame.presents[step.index].source.image] = kNextRead;
    }
  }

  // S102 R24-C: the forward oracle (census runs only: fview / rplay --depth-census): replay the frame's steps with a "stencil is defined" bit per depth image and count the passes that TEST stencil
  // while it is not (the lever, or the walk before it, left it undefined). A defined stencil is: a CLEAR load op, a LOAD of a defined one, a clear inside the pass that is issued, or a copy
  // that carries both aspects from a defined source. The poison arm's CLEAR 0xA5 stands for "undefined" here (sd_poisoned_load).
  if (g_depth_census_on.load(std::memory_order_relaxed)) {
    std::vector<uint8_t> defined(image_limit, frame.keep_contents ? 1 : 0);
    for (const Step& step : steps) {
      if (step.kind == StepKind::Pass) {
        const PassPlan& p = passes[step.index];
        const ImageId d = p.targets ? p.targets->depth.image : kNoImage;
        if (d == kNoImage || !(images[d].aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) continue;
        const uint8_t load = p.sd_poisoned_load ? uint8_t(VK_ATTACHMENT_LOAD_OP_DONT_CARE) : p.stencil_load;
        bool def = load == VK_ATTACHMENT_LOAD_OP_CLEAR ? true : (load == VK_ATTACHMENT_LOAD_OP_LOAD ? defined[d] != 0 : false);
        if (p.stencil_clear_in && !p.strip_stencil_clear) def = true;
        if (p.stencil_tests) {
          g_sd_oracle_tests.fetch_add(1, std::memory_order_relaxed);
          if (!def) g_sd_oracle_violations.fetch_add(1, std::memory_order_relaxed);
        }
        defined[d] = p.stencil_store == VK_ATTACHMENT_STORE_OP_STORE ? (def ? 1 : 0) : 0;
      } else if (step.kind == StepKind::Copy) {
        const CopyOp& copy = frame.copies[step.index];
        const ImageEntry& dest = images[copy.dest.image];
        if (!dest.depth || !images[copy.source.image].depth) continue;
        const bool whole = copy.dest_rect.x == 0 && copy.dest_rect.y == 0 && copy.dest_rect.width >= dest.desc.width && copy.dest_rect.height >= dest.desc.height;
        const bool depth_only = std::find(depth_only_copies.begin(), depth_only_copies.end(), &copy) != depth_only_copies.end();
        if (depth_only) defined[copy.dest.image] = defined[copy.dest.image];   // (poison: the destination's stencil becomes garbage, but nothing reads it by the lever's rule)
        else if (whole && dest.desc.mips == 1 && dest.layers == 1) defined[copy.dest.image] = defined[copy.source.image];
        else defined[copy.dest.image] = (defined[copy.dest.image] && defined[copy.source.image]) ? 1 : 0;
      }
    }
  }

  // Which images carry state from frame to frame: read before anything in this frame wrote them. Once seen, always.
  const auto read = [&](ImageId id) {
    if (written_stamp[id] != frame_stamp) images[id].carries_state = true;
    images[id].ever_read = true;
  };
  for (const Step& step : steps) {
    if (step.kind == StepKind::Pass) {
      const PassPlan& plan = passes[step.index];
      for (uint32_t k = 0; k < plan.sampled_count; ++k) read(sampled[plan.first_sampled + k].image);
      for (uint32_t c = 0; c < plan.targets->color_count; ++c) {
        if (plan.load[c] == VK_ATTACHMENT_LOAD_OP_LOAD) read(plan.targets->color[c].image);
        written_stamp[plan.targets->color[c].image] = frame_stamp;
      }
      if (plan.targets->depth.image != kNoImage) {
        if (plan.load[kMaxColorTargets] == VK_ATTACHMENT_LOAD_OP_LOAD) read(plan.targets->depth.image);
        written_stamp[plan.targets->depth.image] = frame_stamp;
      }
    } else if (step.kind == StepKind::Copy) {
      const CopyOp& copy = frame.copies[step.index];
      read(copy.source.image);
      written_stamp[copy.dest.image] = frame_stamp;
    } else if (step.kind == StepKind::Dispatch) {   // S102 L1b
      const DispatchOp& d = frame.dispatches[step.index];
      for (uint32_t k = 0; k < d.cube_count && k < uint32_t(kMaxLightCubes); ++k) written_stamp[d.cube[k]] = frame_stamp;
    }
  }

  // Persistent work runs exactly once per game frame, in order, even when the frame is never shown: anything writing an image that carries state, and
  // what that work reads this frame. Passes on the screen-sized images are display work: dropping them leaves at worst a one-frame-old copy of the picture.
  uint32_t screen_width = 0, screen_height = 0;
  if (frame.presents.count) {
    const ImageEntry& screen = images[frame.presents[frame.presents.count - 1].source.image];
    screen_width = screen.desc.width;
    screen_height = screen.desc.height;
  }
  const auto screen_sized = [&](const ImageEntry& entry) { return entry.desc.width == screen_width && entry.desc.height == screen_height; };
  uint32_t persistent_count = 0;
  for (size_t s = steps.size(); s-- > 0;) {
    Step& step = steps[s];
    if (step.kind == StepKind::Pass) {
      const PassPlan& plan = passes[step.index];
      const ImageId depth = plan.targets->depth.image;
      bool on_screen = false, carries = false;
      const auto look = [&](ImageId id) {
        const ImageEntry& entry = images[id];
        on_screen |= screen_sized(entry);
        carries |= entry.carries_state || state_needed[id];
      };
      for (uint32_t c = 0; c < plan.targets->color_count; ++c) look(plan.targets->color[c].image);
      if (depth != kNoImage) look(depth);
      step.persistent = carries && !on_screen;
      if (step.persistent) {
        for (uint32_t k = 0; k < plan.sampled_count; ++k) state_needed[sampled[plan.first_sampled + k].image] = 1;
        for (uint32_t c = 0; c < plan.targets->color_count; ++c) {
          if (plan.load[c] == VK_ATTACHMENT_LOAD_OP_LOAD) state_needed[plan.targets->color[c].image] = 1;
        }
        if (depth != kNoImage && plan.load[kMaxColorTargets] == VK_ATTACHMENT_LOAD_OP_LOAD) state_needed[depth] = 1;
      }
    } else if (step.kind == StepKind::Copy) {
      const CopyOp& copy = frame.copies[step.index];
      const ImageEntry& dest = images[copy.dest.image];
      // S100: also a copy into an image no frame has read yet (an attire packed during a load is first drawn many frames later; the
      // atlas copies of its build): until something reads it, whether it lasts is unknown, so it is kept.
      // S101 names: a destination whose role is declared lasting is kept, whatever has read it (the "ever read" guess stays for the rest).
      // Keeps the !screen_sized(source) guard (S101 review 6: a screen-sized source's pass is not persistent, so its copy would be stale).
      step.persistent = !screen_sized(images[copy.source.image]) &&
                        ((dest.desc.usage & kUsageLasting) || dest.carries_state || state_needed[copy.dest.image] || !dest.ever_read);
      if (step.persistent) state_needed[copy.source.image] = 1;
    } else if (step.kind == StepKind::Dispatch) {   // S102 L1b: as a pass into the same images: it must run once per game frame when a cube carries state from frame to frame (a frame without a bake still reads the last bake)
      const DispatchOp& d = frame.dispatches[step.index];
      bool carries = false;
      // review F1: the copies' own rule (a copy into a lasting destination, one that carries state, one no frame has read yet is persistent; the cubes inherit kUsageLasting from their surface role): the dispatch
      // writes the same destinations the copies did, so it runs once per game frame in a frame that is never shown, and not again in a redraw (DisplayOnly). Plant 4 = the first version's rule.
      const bool old_rule = g_light_cube_plant.load(std::memory_order_relaxed) == 4;
      for (uint32_t k = 0; k < d.cube_count && k < uint32_t(kMaxLightCubes); ++k) {
        const ImageEntry& c = images[d.cube[k]];
        carries |= c.carries_state || state_needed[d.cube[k]] || (!old_rule && ((c.desc.usage & kUsageLasting) || !c.ever_read));
      }
      step.persistent = carries;
    }
    if (step.persistent) ++persistent_count;
  }
#ifdef RDR_DEV
  if (persistent_count != last_persistent_count) {
    last_persistent_count = persistent_count;
    Log("[rdr] frame %llu: %zu steps, %u persistent", static_cast<unsigned long long>(frame.tick), steps.size(), persistent_count);
  }
#endif
}
