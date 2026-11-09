// Session 98 (plan step 16): the GPU side of "click to inspect". The Renderer owns one PickGpu; the passes themselves are Renderer
// member functions written in pick.cpp (IdPass, HighlightPass, PickAt, ...) so they can use DrawMesh and the frame constants.
//   ID pass    : the scene's meshes again into an R32G32_UINT target (x = model << 20 | mesh, plus 1; y = instance + 1), same depth test,
//                trivial shaders. Only the pixel under the cursor is read back (a 1x1 staging copy).
//   highlight  : the hovered / selected mesh instance drawn into a two-channel mask (depth-tested against the scene's depth), then an
//                edge outline + a blurred additive glow composited onto the finished picture, before the UI.
#pragma once
#include <d3d11.h>
#include <wrl/client.h>

#include <string>

#include "post.h"
#include "render_shared.h"

namespace view {

class PickGpu {
 public:
  bool Init(ID3D11Device* dev, const std::string& scene_src, std::string& err);

  Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_id, vs_mask, vs_fs;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_id, ps_mask, ps_comp;
  Microsoft::WRL::ComPtr<ID3D11Buffer> cb_pick, cb_comp;
  Microsoft::WRL::ComPtr<ID3D11BlendState> bs_mask_r, bs_mask_g, bs_comp, bs_none;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> ds_test, ds_write;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> rs_, rs_mask, rs_scissor;

  // ID target + its depth (size of the last ID pass)
  Microsoft::WRL::ComPtr<ID3D11Texture2D> id_tex, depth_tex;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> id_rtv;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth_dsv;
  int id_w = 0, id_h = 0;
  bool EnsureId(ID3D11Device* dev, int w, int h);

  // the highlight mask (R = selected, G = hovered)
  Tex mask;
  bool EnsureMask(ID3D11Device* dev, int w, int h);

  // the 1x1 readback ring
  static constexpr int kStage = 3;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> stage[kStage];
  bool pending[kStage] = {};
  int pend_x[kStage] = {}, pend_w[kStage] = {}, pend_h[kStage] = {};
  unsigned pend_seq[kStage] = {};
  uint32_t pend_gen[kStage] = {};  // Renderer::ModelsGen() when the readback was queued: a different set of models = stale
  unsigned seq = 0;
  bool EnsureStage(ID3D11Device* dev);
};

}  // namespace view
