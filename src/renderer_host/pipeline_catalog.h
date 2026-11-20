// S105 B3: the shader catalog for the stream-free host. The pipeline store (src\renderer\pipeline_store.h) lists pipelines by the hash of their SPIR-V; to build a
// stored pipeline before a scene draws it, the renderer needs the program behind each hash. The only registration was the stream build's Binder constructor
// (renderer_adapter\binder.h RegisterPipelineCatalog), so the player build loaded its list and built only the UI pipelines it had drawn this session
// (008: "182 unresolved", precompile "0 of 0"). SceneHost::Start calls RegisterFxCatalog once after Renderer::Create (the store is open by then).
// The catalog is our own compiled-in kVariants arrays (spirv and the alpha twin): no game data. Header-only, templated on the renderer so a unit can drive it with a fake.
#pragma once

#include <cstddef>
#include <vector>

namespace wwe2k14::scene_host {

// R = rdr::Renderer (or a fake with ShaderBlobDesc, pipeline_store_active(), RegisterShaderCatalog()); V = a variant range with .name .spirv .bytes .spirv_alpha .bytes_alpha.
// Returns the number of blobs handed over (0 when the store is off: nothing is registered then, as Binder did).
template <class R, class It>
inline size_t RegisterFxCatalogFrom(R& renderer, It first, It last) {
  if (!renderer.pipeline_store_active()) return 0;
  std::vector<typename R::ShaderBlobDesc> blobs;
  for (It v = first; v != last; ++v) {
    blobs.push_back({v->name, v->spirv, v->bytes});
    if (v->spirv_alpha) blobs.push_back({v->name, v->spirv_alpha, v->bytes_alpha});
  }
  renderer.RegisterShaderCatalog(blobs.data(), blobs.size());
  return blobs.size();
}

}  // namespace wwe2k14::scene_host
