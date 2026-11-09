// The D3D11 backend of the Arena Viewer renderer (Windows): the pre-Session-98-phone-port renderer, its passes unchanged; the
// constants, draw lists and plans come from the shared Renderer (renderer.h / render_shared.h), the HLSL from viewer_hlsl.h.
#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include "pick_gpu.h"
#include "post.h"
#include "renderer.h"
#include "shadow.h"

namespace view {

class RendererD3D11 : public Renderer {
 public:
  const char* BackendName() const override { return "d3d11"; }
  bool Init(const Surface& s, int w, int h, std::string& err) override;
  void Resize(int w, int h) override;
  void DrawFrame(const Camera& cam) override;
  bool ShotPixels(const Camera& cam, int w, int h, std::vector<uint8_t>& px, std::string& err) override;
  bool GrabBackBuffer(std::vector<uint8_t>& bgra, int& w, int& h, std::string& err) override;
  int AddSpriteTexture(const assets::Texture& t) override;
  std::string Timing(const Camera& cam, int w, int h, int frames, std::string& err, bool with_pick = false) override;
  bool DeviceLost() const override { return dev_ && dev_->GetDeviceRemovedReason() != S_OK; }
  uint64_t UiTexture(size_t model, size_t tex) override;
  ID3D11Device* Device() const { return dev_.Get(); }
  ID3D11DeviceContext* Context() const { return ctx_.Get(); }

 protected:
  std::unique_ptr<GpuModelData> NewModelData() override;
  bool UploadTexture(GpuModelData& d, const assets::Texture& t, std::vector<std::string>& warnings) override;
  bool UploadPlacement(GpuModelData& d, const std::vector<assets::Matrix>& transforms, std::string& err) override;
  bool UploadMesh(GpuModelData& d, const assets::Mesh& m, int& buf, std::string& err) override;
  bool UploadBones(GpuModel& g, const std::vector<assets::Matrix>& full, std::string& err) override;
  void DropBones(GpuModel& g) override;
  bool UpdateTexture(GpuModel& g, size_t slot, int w, int h, const uint8_t* rgba) override;
  void DropSpriteTextures() override { sprite_tex_.clear(); }
  void DropPendingReadbacks() override { for (bool& p : pick_.pending) p = false; }
  void AfterModelsDrop() override { if (ctx_) { ctx_->ClearState(); ctx_->Flush(); } }
  PickHit PickRead(const Camera& cam, int w, int h, int x, int y, std::string& err) override;
  bool LiveScreenReady() const override { return live_.srv != nullptr; }

 private:
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> MakeTexture(const assets::Texture& t, std::vector<std::string>& warnings);
  void UploadFrameCB(const FrameCB& fcb);
  void BindCommon(ID3D11VertexShader* vs, ID3D11PixelShader* ps);
  void DrawMesh(const GpuModel& gm, const GpuMesh& me, bool blended);
  void ShadowPasses(const Camera& cam, int w, int h, GpuTimer* tm);
  void Frame(ID3D11RenderTargetView* out, int w, int h, const Camera& cam);  // shadows -> scene -> post chain -> out
  void Scene(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, int w, int h, const Camera& cam);
  void SpritePass(int w, int h, const FrameCB& fcb);
  void FlashPass(ID3D11RenderTargetView* out, int w, int h, const FrameCB& fcb);
  ID3D11RasterizerState* Raster(uint8_t r) const { return r == 1 ? raster_cull_.Get() : r == 2 ? raster_cull_other_.Get() : raster_.Get(); }
  // Session 98 (step 16): pick.cpp
  void IdPass(const Camera& cam, int w, int h, int sx = -1, int sy = -1);  // sx, sy >= 0: scissor to that pixel (hover)
  void QueueReadback(int x, int y);
  void PollReadback();
  void HighlightPass(ID3D11RenderTargetView* out, int w, int h, const FrameCB& fcb, GpuTimer* tm);
  bool CreateShaders(std::string& err);
  bool CreateBackBufferViews();
  bool MakeMatrixBuffer(const assets::Matrix* m, size_t n, bool structured, Microsoft::WRL::ComPtr<ID3D11Buffer>& buf,
                        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>* srv, std::string& err);

  bool timing_on_ = false, timer_ok_ = false, live_has_ = false;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_flash_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_flash_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> cb_flash_;
  Microsoft::WRL::ComPtr<ID3D11InputLayout> layout_flash_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_point_;
  std::vector<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> sprite_tex_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_sprite_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_sprite_;
  Microsoft::WRL::ComPtr<ID3D11InputLayout> layout_sprite_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> vb_sprite_, cb_sprite_;
  size_t vb_sprite_cap_ = 0;
  Tex live_;                                   // last frame's scene colour (the big screen of the match look)
  PostChain post_;
  ShadowMaps shadow_;
  GpuTimer timer_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_caster_, ps_plane_, ps_rim_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_plane_;
  PickGpu pick_;
  int draw_inst_ = -1;  // >= 0: DrawMesh draws only that instance of a placed mesh (the highlight mask)

  Microsoft::WRL::ComPtr<ID3D11Device> dev_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
  Microsoft::WRL::ComPtr<IDXGISwapChain> sc_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> bb_rtv_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> depth_tex_;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth_dsv_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
  Microsoft::WRL::ComPtr<ID3D11InputLayout> layout_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> cb_frame_, cb_draw_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_, sampler_clamp_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> ident_inst_, ident_bones_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ident_bones_srv_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_, raster_cull_, raster_cull_other_;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> ds_opaque_, ds_blend_;
  Microsoft::WRL::ComPtr<ID3D11BlendState> bs_opaque_, bs_blend_, bs_add_;
  bool b4g4r4a4_ok_ = false;
};

}  // namespace view
