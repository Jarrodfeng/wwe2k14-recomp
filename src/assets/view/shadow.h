// Session 98 (plan step 10): the game's shadow set in the viewer, in our own simpler structure (CLAUDE.md rule 6):
//  - props map: a top-down 512^2 height map of the ring pieces (the game: fit +-262 x +-212), read by the floor; plus a circle blob
//    under each wrestler (the game's circle_shadow);
//  - wrestler map: ONE 1024^2 depth map from a directional light at 31 degrees elevation (match direction (0.857, 0.515, 0)),
//    read with a soft (PCSS-like) lookup by the canvas, the floor and the wrestlers;
//  - ring-mat plane shadow: the wrestlers projected from the point light (-450, -1400, 0) onto the canvas plane, flat, then
//    downscaled and blurred, read by whatever sits at the canvas height.
// Numbers: out\runtime\s98\passes.md section 4(b). The pixel constants (strengths, biases) are not in any log: ours, marked U.
// Session 98 phone port: the fit maths moved to render_shared.h (ShadowSetup); the HLSL to viewer_hlsl.h (kShadow, kBlur). This
// file is the D3D11 side: the targets, the blurs.
#pragma once
#include <d3d11.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#include "camera.h"
#include "post.h"
#include "render_shared.h"

namespace view {

class ShadowMaps {
 public:
  bool Init(ID3D11Device* dev, std::string& err);
  void Upload(ID3D11DeviceContext* ctx, const ShadowCB& cb);
  ID3D11Buffer* CbBuffer() const { return cb_.Get(); }
  // passes: bind the target, clear it; the caller draws the casters with the matching matrix
  void BeginProps(ID3D11DeviceContext* ctx);   // height map + its depth
  void BeginWrestler(ID3D11DeviceContext* ctx);  // depth only
  void BeginPlane(ID3D11DeviceContext* ctx);   // flat mask, max blend
  void BlurPlane(ID3D11DeviceContext* ctx);    // 512 -> 256 -> two blurs
  // Session 98 (wrestler fix 2): the game's rim light map (yRimLightMap): the wrestlers seen from the MAIN camera into 512x256,
  // red = 0 on a wrestler (clear 1), green = the view depth across the wrestlers' range; one 13-tap blur. The skin shaders read it
  // at their screen position: rim = blurred red (background around the silhouette) + max(blurred green - own depth, 0) x gain.
  void BeginRim(ID3D11DeviceContext* ctx);
  void BlurRim(ID3D11DeviceContext* ctx);
  ID3D11ShaderResourceView* RimSrv() const { return rim_b_.srv.Get(); }
  ID3D11ShaderResourceView* PropsSrv() const { return props_.srv.Get(); }
  ID3D11ShaderResourceView* WrestlerSrv() const { return wr_depth_.srv.Get(); }
  ID3D11ShaderResourceView* PlaneSrv() const { return plane_b_.srv.Get(); }
  ID3D11BlendState* MaxBlend() const { return bs_max_.Get(); }
  ID3D11DepthStencilState* NoDepth() const { return ds_off_.Get(); }

 private:
  Microsoft::WRL::ComPtr<ID3D11Device> dev_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_full_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_down_, ps_blur_, ps_rim13_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> cb_blur_;
  Microsoft::WRL::ComPtr<ID3D11BlendState> bs_max_, bs_off_;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> ds_off_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> lin_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> rs_;
  Tex props_, props_depth_, wr_depth_, plane_a_, plane_b_, plane_c_, rim_a_, rim_b_, rim_depth_;
  void Blur1(ID3D11DeviceContext* ctx, Tex& src, Tex& dst, float dx, float dy, ID3D11PixelShader* ps);
};

}  // namespace view
