// S110 CUTGREY: the crowd camera flashes, drawn AFTER the post chain (docs\research\s110\CUTGREY.md, CUTGREY_BUILD.md). NOT a translation unit of its own: front_end.cpp #includes this file once, inside namespace
// rdr_scene, after FrontEnd::Record, because it reaches FrontEnd::ModelData (private to front_end.cpp) and because 17 unit build scripts list front_end.cpp as one source (a second TU would have to be added to
// every one of them, and to the stub builds of tools\assets\build_*.ps1). It is a .h so that every script's "newest header" test sees an edit. Do not list it in CMakeLists.txt, do not include it elsewhere.
//
// What the game does [C, CUTGREY_BUILD.md point 1]: the flash manager's draw node is in render category 52 beside the post module's node and draws after the whole post chain (combine, sepia, ...), before the
// fade (57) and the vintage overlay (68): the sprite lands crisp on the finished picture, SrcAlpha + One, no depth write, its own colour (one of 4 colour blocks), and is hidden WHOLE when the scene depth at its
// centre is nearer (point 2). Ours: the frame's flash sprites (SceneFrame::flashes, filled by live_flashes.cpp from the record's kFxFlash rows) are drawn here onto the post output (or the scene colour of a
// no-post frame) with our own shader pair rdr_flash.vert / rdr_flash.frag, after post_pass_->Record and before Stage::AfterPost.

// The meshes of a model as the flash draw needs them (one vertex buffer + index buffer range each, its picture and the material's two colour factors). false = the model is gone / has no drawable mesh.
bool FrontEnd::FlashMeshesOf(int model, std::vector<FlashMesh>& out) {
  if (model < 0 || size_t(model) >= models_.size() || !models_[size_t(model)]->live) return false;
  ModelData& m = Mod(model);
  for (const ModelData::Mesh& me : m.meshes) {
    if (me.skinned || me.chunk || me.cloth || me.index_count == 0) continue;
    FlashMesh f;
    f.vb = PosedVertexBuffer(m), f.ib = m.ib;   // (S103 PF2: `vb` is read only through PosedVertexBuffer)
    f.vb_offset = me.vb_offset, f.ib_offset = me.ib_offset, f.index_count = me.index_count;
    f.map = white_;
    if (me.material >= 0 && size_t(me.material) < m.materials.size()) {
      const assets::Material& mat = m.materials[size_t(me.material)];
      if (mat.diffuse >= 0 && size_t(mat.diffuse) < m.images.size() && m.images[size_t(mat.diffuse)] != rdr::kNoImage) f.map = m.images[size_t(mat.diffuse)];
      std::memcpy(f.amb, mat.ambient, sizeof f.amb);
      std::memcpy(f.dif, mat.diffuse_color, sizeof f.dif);
    }
    out.push_back(f);
  }
  return !out.empty();
}

// Draws the frame's flash sprites onto `colour` (the post output, or the scene colour of a frame with no post). `depth` = the scene's depth, sampled for the whole-sprite centre test. Returns the sprites drawn.
// The depth must be a sampled image (OwnTargets / RenderToPixels ask for it whenever the frame carries flashes); if it is not, nothing is drawn and the first time says so.
uint32_t FrontEnd::DrawFlashesAfterPost(const SceneFrame& frame, rdr::FrameBuilder& fb, rdr::ImageId colour, rdr::ImageId depth, const StageCtx& sctx) {
  if (frame.flashes.empty() || colour == rdr::kNoImage || !sctx.view_proj || sctx.width == 0 || sctx.height == 0) return 0;
  if (depth == rdr::kNoImage || !(depth_sampled_ || depth_stored_)) {
    flash_nodepth_ += frame.flashes.size();   // S110 RDRPREP F3
    if (!flash_nodepth_logged_) flash_nodepth_logged_ = true, rdr::Log("[scene][flashes] %zu flash sprite(s) not drawn: the scene depth is not a sampled image (the caller must ask for it when a frame carries flashes)", frame.flashes.size());
    return 0;
  }
  if (!flash_made_) {
    constexpr uint16_t kRGB32F = 106, kRG32F = 103, kRGBA8Unorm = 37;
    rdr::ProgramDesc pd;
    pd.name = "flash: crowd camera flash after the post";
    pd.vertex_spirv = kSpv_rdr_flash_vert, pd.vertex_bytes = sizeof(kSpv_rdr_flash_vert);
    pd.fragment_spirv = kSpv_rdr_flash_frag, pd.fragment_bytes = sizeof(kSpv_rdr_flash_frag);
    flash_program_ = renderer_.AddProgram(pd);
    rdr::VertexLayoutDesc d{};
    d.strides[0] = uint32_t(sizeof(assets::Vertex));
    d.attributes[d.attribute_count++] = {0, 0, kRGB32F, uint32_t(offsetof(assets::Vertex, pos))};
    d.attributes[d.attribute_count++] = {1, 0, kRGB32F, uint32_t(offsetof(assets::Vertex, normal))};
    d.attributes[d.attribute_count++] = {2, 0, kRGBA8Unorm, uint32_t(offsetof(assets::Vertex, color))};
    d.attributes[d.attribute_count++] = {3, 0, kRG32F, uint32_t(offsetof(assets::Vertex, uv0))};
    flash_layout_ = renderer_.AddVertexLayout(d);
    flash_made_ = true;
  }
  const float* vp = sctx.view_proj;
  const bool plant_file_colour = g_plant_flash_file_colour.load(std::memory_order_relaxed);   // TEST ONLY: the row's colour ignored (the old behaviour)
  const bool plant_v_flip = g_plant_flash_v_flip.load(std::memory_order_relaxed);             // TEST ONLY: the centre's depth row read upside down
  const bool plant_row0 = g_plant_flash_fetch_row0.load(std::memory_order_relaxed);           // TEST ONLY (S110 FLASHOCC): the centre's depth read from row 0

  // S111 FLASHGAME (docs\research\s111\FLASHGAME.md): the game's depth rule and no other. The flash item turns the depth test OFF for this draw (sub_823B27F0 +354) and hides a sprite only by the vertex stage's
  // read of the scene depth at its centre (yFlashDefault.vert:85-90, :111; ours: rdr_flash.frag): a body over the centre hides the whole sprite, a body over its rays only has the rays drawn OVER it. S110
  // FLASHFRONT's per-pixel test (its margin) is gone: no depth attachment, the scene depth itself is sampled. TEST ONLY: flash_pixel_ztest puts that test back (cutgrey_unit [RAYS] must catch it).
  const bool plant_pixel_z = g_plant_flash_pixel_ztest.load(std::memory_order_relaxed);
  rdr::TargetsOp t;
  t.color[0].image = colour;
  t.color_count = 1;
  if (plant_pixel_z) t.depth.image = depth;   // TEST ONLY: FLASHFRONT's attached, tested scene depth
  fb.SetTargets(t);   // (no clear: the post output / the scene colour is loaded as it is)

  rdr::DrawState state{};
  state.program = flash_program_;
  state.vertex_layout = flash_layout_;
  state.topology = 3;
  state.cull = 0;       // two-sided: the sprite's winding is our matrix's
  state.front_face = 1;
  state.depth_test = plant_pixel_z ? 1 : 0, state.depth_write = 0;   // S111 FLASHGAME: the game's z test is OFF for this draw (sub_823B27F0 +354); the plant arm is FLASHFRONT's per-pixel test (TEST ONLY)
  state.depth_compare = plant_pixel_z ? 3 : 0;   // (read only under the plant: LESS_OR_EQUAL, our forward depth)
  state.blend[0].enable = 1;
  state.blend[0].src_color = 6;   // SRC_ALPHA
  state.blend[0].dst_color = 1;   // ONE
  state.blend[0].src_alpha = 0;   // ZERO
  state.blend[0].dst_alpha = 1;   // ONE: the picture's alpha stays
  state.blend[0].write_mask = 0xF;
  rdr::DynamicState dynamic{};
  dynamic.viewport[2] = float(sctx.width), dynamic.viewport[3] = float(sctx.height), dynamic.viewport[5] = 1.0f;
  dynamic.scissor.width = sctx.width, dynamic.scissor.height = sctx.height;
  dynamic.system[0] = 1.f, dynamic.system[1] = -1.f, dynamic.system[2] = 1.f;
  dynamic.system[9] = 7.f;
  rdr::SamplerDesc map_sd{};
  map_sd.mag_filter = map_sd.min_filter = 1;
  map_sd.mip_mode = 1;
  map_sd.address_u = map_sd.address_v = map_sd.address_w = 2;
  map_sd.max_anisotropy = 1;
  map_sd.max_lod = 1000.f;
  rdr::SamplerDesc depth_sd{};   // the game's depth fetch is unfiltered: the centre's own texel
  depth_sd.mag_filter = depth_sd.min_filter = 0;
  depth_sd.mip_mode = 0;
  depth_sd.address_u = depth_sd.address_v = depth_sd.address_w = 2;
  depth_sd.max_anisotropy = 1;
  depth_sd.max_lod = 1000.f;
  const uint32_t map_sampler = fb.Sampler(map_sd), depth_sampler = fb.Sampler(depth_sd);

  std::vector<std::pair<int, std::vector<FlashMesh>>> by_model;
  uint32_t drawn = 0;
  for (const FlashSprite& s : frame.flashes) {
    std::vector<FlashMesh>* meshes = nullptr;
    for (auto& e : by_model)
      if (e.first == s.model) meshes = &e.second;
    if (!meshes) {
      by_model.push_back({s.model, {}});
      meshes = &by_model.back().second;
      FlashMeshesOf(s.model, *meshes);
    }
    if (meshes->empty()) continue;
    // the centre in clip space: the same row-vector matrix the scene's depth was drawn with
    float c[4];
    for (int k = 0; k < 4; ++k) c[k] = s.centre[0] * vp[0 * 4 + k] + s.centre[1] * vp[1 * 4 + k] + s.centre[2] * vp[2 * 4 + k] + vp[3 * 4 + k];
    if (!(c[3] > 1e-6f)) continue;   // behind the camera: nothing of it is on screen
    const float ndc_x = c[0] / c[3], ndc_y = c[1] / c[3], ndc_z = c[2] / c[3];
    // S110 FLASHOCC (FLASHOCC.md): the census that tells a flash really in front of a body (b-live) from a wrong compare (x). c[3] = the centre's view z.
    if (flash_nearest_ == 0.f || c[3] < flash_nearest_) flash_nearest_ = c[3];
    if (c[3] < kFlashNearViewZ) {
      ++flash_near_;
      if (flash_near_lines_ < 32) {
        ++flash_near_lines_;
        const float* e = frame.camera.eye;   // viewer space: x and z are the game's, so the radii from the ring centre (the world origin) are the game's
        const float factor = std::sqrt(s.world[0] * s.world[0] + s.world[1] * s.world[1] + s.world[2] * s.world[2]);
        rdr::Log("[scene][flashes] near flash: frame %llu slot %u seq %u centre view-z %.1f ndc %.3f %.3f scale %.4f factor %.4f eye radius %.1f (height %.1f) flash radius %.1f (height %.1f) held %d (threshold %.0f)",
                 (unsigned long long)s.guest_frame, unsigned(s.slot), s.seq, double(c[3]), double(ndc_x), double(ndc_y), double(s.scale), double(factor), double(std::sqrt(e[0] * e[0] + e[2] * e[2])), double(e[1]),
                 double(std::sqrt(s.centre[0] * s.centre[0] + s.centre[2] * s.centre[2])), double(s.centre[1]), int(s.held), double(kFlashNearViewZ));
      }
    }
    // clip y reaches the picture flipped (dynamic.system[1] = -1): row 0 = ndc y +1. The game's fetch clamps at the picture's edge.
    const float u = std::min(std::max(0.5f * ndc_x + 0.5f, 0.f), 0.99999994f), v = plant_row0 ? 0.f : std::min(std::max(plant_v_flip ? 0.5f + 0.5f * ndc_y : 0.5f - 0.5f * ndc_y, 0.f), 0.99999994f);
    const float fcb[4] = {u, v, ndc_z, plant_pixel_z ? 0.f : 1.f};
    float vsb[44] = {};
    for (int k = 0; k < 4; ++k)
      for (int i = 0; i < 4; ++i) vsb[4 * k + i] = vp[i * 4 + k];
    for (int k = 0; k < 3; ++k)
      for (int i = 0; i < 4; ++i) vsb[16 + 4 * k + i] = s.world[i * 4 + k];
    for (int i = 0; i < 3; ++i) vsb[28 + i] = plant_file_colour ? 1.f : s.rgb[i];
    vsb[40] = s.alpha;
    for (const FlashMesh& me : *meshes) {
      std::memcpy(vsb + 32, me.amb, 16);
      std::memcpy(vsb + 36, me.dif, 16);
      rdr::TextureSet set{};
      set.slots[0].image = me.map, set.slots[0].sampler = map_sampler, set.slots[0].kind = 0;
      set.slots[3].image = plant_pixel_z ? white_ : depth, set.slots[3].sampler = depth_sampler, set.slots[3].kind = 0;   // the scene depth, read at the centre (the plant: attached, so not sampled)
      rdr::DrawOp op{};
      op.state = fb.State(state);
      op.dynamic = fb.Dynamic(dynamic);
      op.textures = fb.Textures(set);
      op.vs_constants = fb.PushConstants(vsb, sizeof vsb);
      op.fs_constants = fb.PushConstants(fcb, sizeof fcb);
      op.vertex[0] = {me.vb, me.vb_offset};
      op.index = {me.ib, me.ib_offset};
      op.index_32bit = 1;
      op.count = me.index_count;
      op.instances = 1;
      fb.Draw(op, rdr::kGrpOther);
    }
    ++drawn;
  }
  return drawn;
}
