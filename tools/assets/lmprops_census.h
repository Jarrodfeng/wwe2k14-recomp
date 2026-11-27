// S111 LMPROPS: which non-arena models (m2.pac weapons, evtobj.pac prop entries) that name `sampLightMapPrm` draw with a light-mapped technique variant, and whether that variant reads c184 (LightMapParam).
// Included by technique_census.cpp inside its anonymous namespace, after Pick(). Offline, 0 game runs.
//   technique_census --game DIR --lmprops [--pach N-M] [--plant-no-page]
// The live rule (FrontEnd::BindMaterial): w.lightmap = (own light map uploaded || has_lm_slot) && a page image exists; a prop has no stage page, so w.lightmap needs the mesh's OWN light-map texture. A mesh whose slot names
// a texture the model does not hold keeps w.lightmap = false (the plain variant). --plant-no-page = the census without that page rule (the sampler-presence reading): its LM count must be >= the live one.
// Exit 0 = printed; the totals line is "LMPROPS meshes N: LM variant M (reads c184 R), plain P, unbound U".
struct LmPropsTotals { size_t models = 0, lm_models = 0, meshes = 0, lm_meshes = 0, reads_c184 = 0, plain = 0, unbound = 0, no_page = 0, named_materials = 0, unused_materials = 0; };

inline bool LmPropsReadsC184(int variant) {
  if (variant < 0 || !rdr_scene::T().by_index[size_t(variant)]) return false;
  const auto& t = *rdr_scene::T().by_index[size_t(variant)];
  for (uint32_t k = 0; k < t.field_count; ++k)
    if (std::strcmp(t.fields[k].name, "LightMapParam") == 0) return true;
  return false;
}

inline bool LmPropsSamplesPage(const rdr::fx::Variant& v) {
  for (uint32_t s = 0; s < v.sampler_count; ++s)
    if (v.samplers[s].slot == 13 || v.samplers[s].slot == 16) return true;
  return false;
}

// One model: prints one line per mesh that names sampLightMapPrm; returns true when the model had one.
inline bool LmPropsModel(const std::string& label, const Model& m, bool live_page_rule, LmPropsTotals& tot) {
  ++tot.models;
  bool any = false;
  for (const Mesh& me : m.meshes) {
    if (me.material < 0 || size_t(me.material) >= m.materials.size()) continue;
    const Material& mat = m.materials[size_t(me.material)];
    ++tot.meshes;
    if (!mat.has_lm_slot && mat.lightmap < 0) continue;
    any = true;
    const bool page = mat.lightmap >= 0 && size_t(mat.lightmap) < m.textures.size();   // the model's own light-map texture (a prop has no stage page)
    Material twin = mat;
    if (live_page_rule && !page) twin.has_lm_slot = false, twin.lightmap = -1;   // BindMaterial: w.lightmap needs an image
    const Probe p = Pick(twin, &m);
    const rdr_scene::FxChoice& c = p.new_pick.Ok() ? p.new_pick : p.old_pick;
    if (!c.Ok()) { ++tot.unbound; std::printf("  %-22s mesh %-3d %-16s tech %-3u page %d  -> UNBOUND\n", label.c_str(), int(&me - m.meshes.data()), mat.shader.c_str(), unsigned(mat.technique), int(page)); continue; }
    const rdr::fx::Variant& ps = rdr::fx::kVariants[c.ps];
    const bool lm = LmPropsSamplesPage(ps) || LmPropsSamplesPage(rdr::fx::kVariants[c.vs]);
    const bool c184 = LmPropsReadsC184(c.vs) || LmPropsReadsC184(c.ps);
    tot.no_page += !page;
    if (lm) ++tot.lm_meshes, tot.reads_c184 += c184; else ++tot.plain;
    std::printf("  %-22s mesh %-3d %-16s tech %-3u page %d  -> %-44s %s%s\n", label.c_str(), int(&me - m.meshes.data()), mat.shader.c_str(), unsigned(mat.technique), int(page), ps.name,
                lm ? "LIGHT-MAPPED" : "plain", c184 ? " reads c184" : "");
  }
  {   // materials that name the slot but no mesh of this model draws with (a material table entry the stream never uses): counted, never drawn
    size_t named = 0, unused = 0;
    for (size_t i = 0; i < m.materials.size(); ++i) {
      if (!m.materials[i].has_lm_slot && m.materials[i].lightmap < 0) continue;
      ++named;
      bool used = false;
      for (const Mesh& me : m.meshes) used = used || me.material == int(i);
      unused += !used;
    }
    tot.named_materials += named, tot.unused_materials += unused;
    if (named && !any) std::printf("  %-22s names the slot in %zu material(s), no mesh uses them\n", label.c_str(), named);
  }
  tot.lm_models += any;
  return any;
}

inline int RunLmProps(launcher::GameFiles& files, const std::string& pachs, bool live_page_rule) {
  LmPropsTotals tot;
  std::printf("== m2.pac weapons (p0 / p1 / 1000 / id, model entries 0..2) ==\n");
  for (int id = 0; id < 128; ++id)
    for (uint32_t e = 0; e < 3; ++e) {
      Model m;
      std::string err;
      if (!LoadM2Weapon(files, id, m, err, 10, e, id == 14 ? 0 : -1)) continue;
      LmPropsModel("m2 id " + std::to_string(id) + (e ? "/" + std::to_string(e) : std::string()), m, live_page_rule, tot);
    }
  std::printf("== evtobj.pac prop entries ==\n");
  for (int pach : Range(pachs, 0, 399))
    for (uint32_t k = 0; k < 10; ++k) {
      Model m;
      std::string err;
      if (!LoadPropEntry(files, pach, k, m, err)) continue;
      LmPropsModel("evt PACH " + std::to_string(pach) + "/" + std::to_string(k), m, live_page_rule, tot);
    }
  std::printf("LMPROPS materials naming the slot %zu, of them used by no mesh %zu\n", tot.named_materials, tot.unused_materials);
  std::printf("LMPROPS models %zu (naming sampLightMapPrm: %zu), meshes %zu; of the naming meshes: LIGHT-MAPPED %zu (reads c184 %zu), plain %zu, unbound %zu; without an own page texture %zu%s\n", tot.models, tot.lm_models,
              tot.meshes, tot.lm_meshes, tot.reads_c184, tot.plain, tot.unbound, tot.no_page, live_page_rule ? "" : "  [--plant-no-page]");
  // The check: no non-arena draw reads c184 (so the loader default in Model::lm_scale / lm_gain is never seen by a draw). A draw that did would need the arena's value (arena_lm::Compute / Held), not the default.
  const bool ok = tot.lm_models > 0 && tot.reads_c184 == 0;   // (lm_models > 0: the census saw the naming models at all)
  std::printf("LMPROPS check (no prop / weapon draw reads the loader-default c184)%s -> %s\n", live_page_rule ? "" : "  [PLANTED TWIN: the page rule off]", ok ? "PASS" : "FAIL");
  if (!live_page_rule) return std::printf(ok ? "planted twin NOT caught: FAIL\n" : "planted twin CAUGHT: PASS\n"), ok ? 1 : 0;
  return ok ? 0 : 1;
}
