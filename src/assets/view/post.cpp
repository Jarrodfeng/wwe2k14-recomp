#include "post.h"

#include <d3dcompiler.h>

#include "viewer_hlsl.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace view {

bool CompileHlsl(const char* src, const char* entry, const char* profile, ComPtr<ID3DBlob>& out, std::string& err) {
  ComPtr<ID3DBlob> msg;
  HRESULT hr = D3DCompile(src, strlen(src), "post.hlsl", nullptr, nullptr, entry, profile, 0, 0, &out, &msg);
  if (FAILED(hr)) {
    err = std::string(entry) + ": " + (msg ? (const char*)msg->GetBufferPointer() : "compile failed");
    return false;
  }
  return true;
}

bool MakeRT(ID3D11Device* dev, Tex& o, int w, int h, DXGI_FORMAT fmt) {
  o = Tex();
  w = std::max(w, 1); h = std::max(h, 1);
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1; td.Format = fmt; td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(dev->CreateTexture2D(&td, nullptr, &o.t))) return false;
  if (FAILED(dev->CreateRenderTargetView(o.t.Get(), nullptr, &o.rtv))) return false;
  if (FAILED(dev->CreateShaderResourceView(o.t.Get(), nullptr, &o.srv))) return false;
  o.w = w; o.h = h;
  return true;
}

bool MakeDepth(ID3D11Device* dev, Tex& o, int w, int h) {
  o = Tex();
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R32_TYPELESS; td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(dev->CreateTexture2D(&td, nullptr, &o.t))) return false;
  D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
  dv.Format = DXGI_FORMAT_D32_FLOAT; dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
  if (FAILED(dev->CreateDepthStencilView(o.t.Get(), &dv, &o.dsv))) return false;
  D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
  sv.Format = DXGI_FORMAT_R32_FLOAT; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
  if (FAILED(dev->CreateShaderResourceView(o.t.Get(), &sv, &o.srv))) return false;
  o.w = w; o.h = h;
  return true;
}

// ---- GPU timer -------------------------------------------------------------------------------------------------------

void GpuTimer::Init(ID3D11Device* dev) {
  dev_ = dev;
  D3D11_QUERY_DESC q = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
  dev->CreateQuery(&q, &disjoint_);
  q.Query = D3D11_QUERY_TIMESTAMP;
  dev->CreateQuery(&q, &t0_);
}
void GpuTimer::Begin(ID3D11DeviceContext* ctx) {
  if (!disjoint_ || active_) return;
  ctx->Begin(disjoint_.Get());
  ctx->End(t0_.Get());
  used_ = 0; names_.clear(); active_ = true;
}
void GpuTimer::Mark(ID3D11DeviceContext* ctx, const char* name) {
  if (!active_) return;
  if (marks_.size() <= used_) {
    D3D11_QUERY_DESC q = {D3D11_QUERY_TIMESTAMP, 0};
    ComPtr<ID3D11Query> n;
    dev_->CreateQuery(&q, &n);
    marks_.push_back(n);
  }
  ctx->End(marks_[used_].Get());
  names_.push_back(name);
  used_++;
}
void GpuTimer::End(ID3D11DeviceContext* ctx) {
  if (!active_) return;
  ctx->End(disjoint_.Get());
  active_ = false; pending_ = true;
}
bool GpuTimer::Collect(ID3D11DeviceContext* ctx) {
  last_.clear(); total_ = 0;  // (a failed collect must not leave the previous frame's numbers behind)
  if (!pending_) return false;
  pending_ = false;
  D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
  bool got = false;
  for (int spin = 0; spin < 2000000 && !got; spin++) {
    HRESULT hr = ctx->GetData(disjoint_.Get(), &dj, sizeof dj, 0);
    if (hr == S_OK) got = true;
    else if (FAILED(hr)) return false;
  }
  if (!got || dj.Disjoint || dj.Frequency == 0) return false;
  UINT64 prev = 0;
  if (ctx->GetData(t0_.Get(), &prev, sizeof prev, 0) != S_OK) return false;
  for (size_t i = 0; i < used_; i++) {
    UINT64 t = 0;
    if (ctx->GetData(marks_[i].Get(), &t, sizeof t, 0) != S_OK) { last_.clear(); total_ = 0; return false; }
    double ms = double(t - prev) * 1000.0 / double(dj.Frequency);
    last_.push_back({names_[i], ms});
    total_ += ms;
    prev = t;
  }
  return true;
}

// ---- the chain -------------------------------------------------------------------------------------------------------

bool PostChain::Init(ID3D11Device* dev, std::string& err) {
  dev_ = dev;
  ComPtr<ID3DBlob> b;
  if (!CompileHlsl(hlsl::kPost, "vsFull", "vs_5_0", b, err)) return false;
  if (FAILED(dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs_))) { err = "post VS"; return false; }
  for (int i = 0; i < (int)PostShader::kCount; i++) {
    const char* n = PostShaderEntry((PostShader)i);
    ComPtr<ID3DBlob> pb;
    if (!CompileHlsl(hlsl::kPost, n, "ps_5_0", pb, err)) return false;
    if (FAILED(dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, ps_[i].GetAddressOf()))) { err = std::string("post PS ") + n; return false; }
  }
  D3D11_BUFFER_DESC bd = {};
  bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; bd.ByteWidth = sizeof(PostCB);
  if (FAILED(dev->CreateBuffer(&bd, nullptr, &cb_))) { err = "post cb"; return false; }
  D3D11_SAMPLER_DESC sm = {};
  sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sm.MaxLOD = D3D11_FLOAT32_MAX;
  dev->CreateSamplerState(&sm, &lin_);
  D3D11_RASTERIZER_DESC rs = {};
  rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE; rs.DepthClipEnable = TRUE;
  dev->CreateRasterizerState(&rs, &rs_);
  D3D11_BLEND_DESC bl = {};
  bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  dev->CreateBlendState(&bl, &bs_);
  D3D11_DEPTH_STENCIL_DESC dd = {};
  dd.DepthEnable = FALSE;
  dev->CreateDepthStencilState(&dd, &ds_);
  return true;
}

bool PostChain::Resize(int w, int h, std::string& err) {
  for (auto& st : sets_)
    if (st.scene.t && st.w == w && st.h == h) { s_ = &st; st.stamp = ++tick_; w_ = w; h_ = h; return true; }
  s_ = sets_[0].stamp <= sets_[1].stamp ? &sets_[0] : &sets_[1];  // the least recently used
  *s_ = Set();
  s_->w = w; s_->h = h; s_->stamp = ++tick_;
  w_ = w; h_ = h;
  const int qw = std::max(w / 4, 1), qh = std::max(h / 4, 1), ew = std::max(w / 8, 1), eh = std::max(h / 8, 1);
  const DXGI_FORMAT f = DXGI_FORMAT_R16G16B16A16_FLOAT;
  bool ok = MakeRT(dev_.Get(), s_->scene, w, h, f) && MakeDepth(dev_.Get(), s_->depth, w, h) && MakeRT(dev_.Get(), s_->near_a, qw, qh, f) &&
            MakeRT(dev_.Get(), s_->near_b, qw, qh, f) && MakeRT(dev_.Get(), s_->far_a, qw, qh, f) && MakeRT(dev_.Get(), s_->far_b, qw, qh, f) &&
            MakeRT(dev_.Get(), s_->bright, ew, eh, f) && MakeRT(dev_.Get(), s_->bl_a, ew, eh, f) && MakeRT(dev_.Get(), s_->bl_b, ew, eh, f) &&
            MakeRT(dev_.Get(), s_->glare, ew, eh, f);
  if (!ok) { err = "post targets failed"; *s_ = Set(); w_ = h_ = 0; return false; }
  return true;
}

void PostChain::SetCb(ID3D11DeviceContext* ctx, const float* v, size_t floats) {
  D3D11_MAPPED_SUBRESOURCE ms;
  if (SUCCEEDED(ctx->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) { memcpy(ms.pData, v, floats * 4); ctx->Unmap(cb_.Get(), 0); }
}

void PostChain::Pass(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, int w, int h, ID3D11PixelShader* ps,
                     ID3D11ShaderResourceView* const* srvs, int n) {
  ID3D11ShaderResourceView* nul[6] = {};
  ctx->PSSetShaderResources(0, 6, nul);
  ctx->OMSetRenderTargets(1, &rtv, nullptr);
  D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
  ctx->RSSetViewports(1, &vp);
  ctx->PSSetShader(ps, nullptr, 0);
  ctx->PSSetShaderResources(0, (UINT)n, srvs);
  ctx->Draw(3, 0);
  ctx->PSSetShaderResources(0, 6, nul);
}

// The plan (render_shared.cpp BuildPostPlan: the game's constants and pass order) on this backend's targets.
void PostChain::Run(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* out, const std::vector<PostStep>& plan, GpuTimer* timer) {
  ctx->ClearState();
  ctx->IASetInputLayout(nullptr);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(vs_.Get(), nullptr, 0);
  ctx->RSSetState(rs_.Get());
  ctx->OMSetBlendState(bs_.Get(), nullptr, 0xFFFFFFFF);
  ctx->OMSetDepthStencilState(ds_.Get(), 0);
  ID3D11Buffer* cb = cb_.Get();
  ctx->PSSetConstantBuffers(0, 1, &cb);
  ID3D11SamplerState* sm = lin_.Get();
  ctx->PSSetSamplers(0, 1, &sm);
  auto tex = [&](PostTarget t) -> Tex* {
    switch (t) {
      case PostTarget::kScene: return &s_->scene;
      case PostTarget::kDepth: return &s_->depth;
      case PostTarget::kNearA: return &s_->near_a;
      case PostTarget::kNearB: return &s_->near_b;
      case PostTarget::kFarA: return &s_->far_a;
      case PostTarget::kFarB: return &s_->far_b;
      case PostTarget::kBright: return &s_->bright;
      case PostTarget::kBlA: return &s_->bl_a;
      case PostTarget::kBlB: return &s_->bl_b;
      case PostTarget::kGlare: return &s_->glare;
      default: return nullptr;
    }
  };
  for (const PostStep& st : plan) {
    if (st.mark) { if (timer) timer->Mark(ctx, st.mark); continue; }
    SetCb(ctx, (const float*)&st.cb, sizeof st.cb / 4);
    ID3D11ShaderResourceView* s[6] = {};
    for (int i = 0; i < st.nsrc; i++) if (Tex* t = tex(st.src[i])) s[i] = t->srv.Get();
    ID3D11RenderTargetView* rtv = out;
    int w = w_, h = h_;
    if (Tex* d = tex(st.dst)) rtv = d->rtv.Get(), w = d->w, h = d->h;
    Pass(ctx, rtv, w, h, ps_[(int)st.ps].Get(), s, st.nsrc);
  }
}

}  // namespace view
