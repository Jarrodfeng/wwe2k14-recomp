// Session 98 (plan step 10): the game's frame recipe in the viewer. The scene renders into an offscreen colour + depth pair; this
// chain then does what the game's final passes do (docs\systems\post_effects.md, out\runtime\s98\passes.md): depth-aware quarter-size
// blurs for the depth of field, the combine (console-style FXAA, near / far DoF blend, depth haze), in entrances the bloom + star
// glare, and the display-gamma curve last. Our own HLSL, written from those notes.
#pragma once
#include <d3d11.h>
#include <wrl/client.h>

#include <string>
#include <utility>
#include <vector>

#include "render_shared.h"

namespace view {

bool CompileHlsl(const char* src, const char* entry, const char* profile, Microsoft::WRL::ComPtr<ID3DBlob>& out, std::string& err);

struct Tex {
  Microsoft::WRL::ComPtr<ID3D11Texture2D> t;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;
  int w = 0, h = 0;
};
bool MakeRT(ID3D11Device* dev, Tex& o, int w, int h, DXGI_FORMAT fmt);      // render target + SRV
bool MakeDepth(ID3D11Device* dev, Tex& o, int w, int h);                     // D32 depth + a float SRV of it

// GPU time per section (timestamp queries). Begin, then Mark("name") after each section, then End.
class GpuTimer {
 public:
  void Init(ID3D11Device* dev);
  void Begin(ID3D11DeviceContext* ctx);
  void Mark(ID3D11DeviceContext* ctx, const char* name);
  void End(ID3D11DeviceContext* ctx);
  // Read the finished frame's times (waits for it). Returns false when the GPU gave no usable timestamps.
  bool Collect(ID3D11DeviceContext* ctx);
  const std::vector<std::pair<std::string, double>>& Last() const { return last_; }
  double LastTotal() const { return total_; }

 private:
  Microsoft::WRL::ComPtr<ID3D11Device> dev_;
  Microsoft::WRL::ComPtr<ID3D11Query> disjoint_, t0_;
  std::vector<Microsoft::WRL::ComPtr<ID3D11Query>> marks_;
  std::vector<std::string> names_;
  size_t used_ = 0;
  bool active_ = false, pending_ = false;
  std::vector<std::pair<std::string, double>> last_;
  double total_ = 0;
};

class PostChain {
 public:
  bool Init(ID3D11Device* dev, std::string& err);
  bool Resize(int w, int h, std::string& err);  // (re)make the targets when the size changed
  ID3D11RenderTargetView* SceneRtv() const { return s_->scene.rtv.Get(); }
  ID3D11DepthStencilView* SceneDsv() const { return s_->depth.dsv.Get(); }
  ID3D11Texture2D* SceneTex() const { return s_->scene.t.Get(); }  // the scene colour (pre-post), for the live screen copy
  ID3D11ShaderResourceView* SceneDepthSrv() const { return s_->depth.srv.Get(); }  // Session 98 (flash fix): the flashes' centre test
  // Run every pass from the scene targets into `out` (w x h as given to Resize).
  void Run(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* out, const std::vector<PostStep>& plan, GpuTimer* timer);

 private:
  void Pass(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, int w, int h, ID3D11PixelShader* ps,
            ID3D11ShaderResourceView* const* srvs, int n);
  void SetCb(ID3D11DeviceContext* ctx, const float* four_by_k, size_t floats);
  Microsoft::WRL::ComPtr<ID3D11Device> dev_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_[(int)PostShader::kCount];  // by PostShader
  Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> lin_, point_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> rs_;
  Microsoft::WRL::ComPtr<ID3D11BlendState> bs_;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> ds_;
  // Two target sets keyed by size (the window's and a shot's / timing's), least recently used replaced.
  struct Set {
    Tex scene, depth;
    Tex near_a, near_b, far_a, far_b;  // quarter size: downsample, blurred (near = plain, far = depth-gated)
    Tex bright, bl_a, bl_b, glare;     // eighth size (160x90 at 720p)
    int w = 0, h = 0;
    unsigned stamp = 0;
  };
  Set sets_[2];
  Set* s_ = &sets_[0];
  unsigned tick_ = 0;
  int w_ = 0, h_ = 0;
};

}  // namespace view
