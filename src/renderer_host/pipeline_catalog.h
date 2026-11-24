// S105 B3: the shader catalog for the stream-free host. The pipeline store (src\renderer\pipeline_store.h) lists pipelines by the hash of their SPIR-V; to build a
// stored pipeline before a scene draws it, the renderer needs the program behind each hash. The only registration was the stream build's Binder constructor
// (renderer_adapter\binder.h RegisterPipelineCatalog), so the player build loaded its list and built only the UI pipelines it had drawn this session
// (008: "182 unresolved", precompile "0 of 0"). SceneHost::Start calls RegisterShaderCatalogFrom once after Renderer::Create (the store is open by then).
// The catalog is our own compiled-in kVariants arrays (spirv and the alpha twin) and kOwnShaders: no game data. Header-only, templated on the renderer so a unit can drive it with a fake.
// It takes ranges, so a unit that includes it does not odr-use the SPIR-V data by that alone (tools\renderer\spv_split_lint.py reads code, not these comments).
#pragma once

#include <cstddef>
#include <vector>

namespace wwe2k14::scene_host {

// S110 RDRPREP: the ONE registrar. The catalog holds our own programs' blobs (rdr::fx::kOwnShaders: the crowd flash, post depth, afterimage, plane mask / blur, the in-scene 2D programs, ...)
// beside the game-derived kVariants: a stored record of an own program resolves at boot and is built in the boot / menu slices, never on its first draw mid-scene. Every site that fills the
// catalog (scene_host.cpp Start, binder.h RegisterPipelineCatalog, tools\scene\frame_rig.cpp) calls this and nothing else (tools\renderer\own_catalog_lint.py).
// R = rdr::Renderer (or a fake with ShaderBlobDesc, pipeline_store_active(), RegisterShaderCatalog()); V = a variant range with .name .spirv .bytes .spirv_alpha .bytes_alpha;
// O = an own-shader range with .name .words .bytes. One RegisterShaderCatalog call for both ranges.
// Returns the number of blobs handed over (0 when the store is off: nothing is registered then, as Binder did).
template <class R, class It, class Ot>
inline size_t RegisterShaderCatalogFrom(R& renderer, It first, It last, Ot own_first, Ot own_last) {
  if (!renderer.pipeline_store_active()) return 0;
  std::vector<typename R::ShaderBlobDesc> blobs;
  for (It v = first; v != last; ++v) {
    blobs.push_back({v->name, v->spirv, v->bytes});
    if (v->spirv_alpha) blobs.push_back({v->name, v->spirv_alpha, v->bytes_alpha});
  }
  for (Ot o = own_first; o != own_last; ++o) blobs.push_back({o->name, o->words, o->bytes});
  renderer.RegisterShaderCatalog(blobs.data(), blobs.size());
  return blobs.size();
}

}  // namespace wwe2k14::scene_host
