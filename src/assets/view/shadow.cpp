#include "shadow.h"

#include "viewer_hlsl.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace view {

bool ShadowMaps::Init(ID3D11Device* dev, std::string& err) {
  dev_ = dev;
  if (!MakeRT(dev, props_, 512, 512, DXGI_FORMAT_R32_FLOAT) || !MakeDepth(dev, props_depth_, 512, 512) ||
      !MakeDepth(dev, wr_depth_, 1024, 1024) || !MakeRT(dev, plane_a_, 512, 512, DXGI_FORMAT_R8_UNORM) ||
      !MakeRT(dev, plane_b_, 256, 256, DXGI_FORMAT_R8_UNORM) || !MakeRT(dev, plane_c_, 256, 256, DXGI_FORMAT_R8_UNORM) ||
      !MakeRT(dev, rim_a_, 512, 256, DXGI_FORMAT_R8G8B8A8_UNORM) || !MakeRT(dev, rim_b_, 512, 256, DXGI_FORMAT_R8G8B8A8_UNORM) ||
      !MakeDepth(dev, rim_depth_, 512, 256)) {
    err = "shadow targets failed";
    return false;
  }
  D3D11_BUFFER_DESC bd = {};
  bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  bd.ByteWidth = (sizeof(ShadowCB) + 15) & ~15u;
  if (FAILED(dev->CreateBuffer(&bd, nullptr, &cb_))) { err = "shadow cb"; return false; }
  bd.ByteWidth = 16;
  if (FAILED(dev->CreateBuffer(&bd, nullptr, &cb_blur_))) { err = "shadow blur cb"; return false; }
  ComPtr<ID3DBlob> b;
  if (!CompileHlsl(hlsl::kBlur, "vsFull", "vs_5_0", b, err)) return false;
  dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs_full_);
  if (!CompileHlsl(hlsl::kBlur, "psDown", "ps_5_0", b, err)) return false;
  dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps_down_);
  if (!CompileHlsl(hlsl::kBlur, "psBlur", "ps_5_0", b, err)) return false;
  dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps_blur_);
  if (!CompileHlsl(hlsl::kBlur, "psRim13", "ps_5_0", b, err)) return false;
  dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps_rim13_);
  D3D11_BLEND_DESC bl = {};
  bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  dev->CreateBlendState(&bl, &bs_off_);
  bl.RenderTarget[0].BlendEnable = TRUE;
  bl.RenderTarget[0].SrcBlend = bl.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
  bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_MAX;
  bl.RenderTarget[0].SrcBlendAlpha = bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
  bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_MAX;
  dev->CreateBlendState(&bl, &bs_max_);
  D3D11_DEPTH_STENCIL_DESC dd = {};
  dd.DepthEnable = FALSE;
  dev->CreateDepthStencilState(&dd, &ds_off_);
  D3D11_SAMPLER_DESC sm = {};
  sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sm.MaxLOD = D3D11_FLOAT32_MAX;
  dev->CreateSamplerState(&sm, &lin_);
  D3D11_RASTERIZER_DESC rs = {};
  rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE; rs.DepthClipEnable = TRUE;
  dev->CreateRasterizerState(&rs, &rs_);
  return true;
}

void ShadowMaps::Upload(ID3D11DeviceContext* ctx, const ShadowCB& cb) {
  D3D11_MAPPED_SUBRESOURCE ms;
  if (SUCCEEDED(ctx->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) { memcpy(ms.pData, &cb, sizeof cb); ctx->Unmap(cb_.Get(), 0); }
}

void ShadowMaps::BeginProps(ID3D11DeviceContext* ctx) {
  const float clear[4] = {-1e4f, 0, 0, 0};
  ID3D11RenderTargetView* r = props_.rtv.Get();
  ctx->OMSetRenderTargets(1, &r, props_depth_.dsv.Get());
  ctx->ClearRenderTargetView(r, clear);
  ctx->ClearDepthStencilView(props_depth_.dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
  D3D11_VIEWPORT vp = {0, 0, 512, 512, 0, 1};
  ctx->RSSetViewports(1, &vp);
}
void ShadowMaps::BeginWrestler(ID3D11DeviceContext* ctx) {
  ctx->OMSetRenderTargets(0, nullptr, wr_depth_.dsv.Get());
  ctx->ClearDepthStencilView(wr_depth_.dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
  D3D11_VIEWPORT vp = {0, 0, 1024, 1024, 0, 1};
  ctx->RSSetViewports(1, &vp);
}
void ShadowMaps::BeginPlane(ID3D11DeviceContext* ctx) {
  const float clear[4] = {0, 0, 0, 0};
  ID3D11RenderTargetView* r = plane_a_.rtv.Get();
  ctx->OMSetRenderTargets(1, &r, nullptr);
  ctx->ClearRenderTargetView(r, clear);
  D3D11_VIEWPORT vp = {0, 0, 512, 512, 0, 1};
  ctx->RSSetViewports(1, &vp);
}

void ShadowMaps::Blur1(ID3D11DeviceContext* ctx, Tex& src, Tex& dst, float dx, float dy, ID3D11PixelShader* ps) {
  ID3D11ShaderResourceView* nul[1] = {};
  ctx->PSSetShaderResources(0, 1, nul);
  ID3D11RenderTargetView* r = dst.rtv.Get();
  ctx->OMSetRenderTargets(1, &r, nullptr);
  D3D11_VIEWPORT vp = {0, 0, (float)dst.w, (float)dst.h, 0, 1};
  ctx->RSSetViewports(1, &vp);
  D3D11_MAPPED_SUBRESOURCE ms;
  if (SUCCEEDED(ctx->Map(cb_blur_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
    float d[4] = {dx, dy, 0, 0};
    memcpy(ms.pData, d, 16);
    ctx->Unmap(cb_blur_.Get(), 0);
  }
  ctx->PSSetShader(ps, nullptr, 0);
  ID3D11ShaderResourceView* s = src.srv.Get();
  ctx->PSSetShaderResources(0, 1, &s);
  ctx->Draw(3, 0);
  ctx->PSSetShaderResources(0, 1, nul);
}

void ShadowMaps::BlurPlane(ID3D11DeviceContext* ctx) {
  ctx->IASetInputLayout(nullptr);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(vs_full_.Get(), nullptr, 0);
  ctx->RSSetState(rs_.Get());
  ctx->OMSetBlendState(bs_off_.Get(), nullptr, 0xFFFFFFFF);
  ctx->OMSetDepthStencilState(ds_off_.Get(), 0);
  ID3D11Buffer* cb = cb_blur_.Get();
  ctx->PSSetConstantBuffers(0, 1, &cb);
  ID3D11SamplerState* sm = lin_.Get();
  ctx->PSSetSamplers(0, 1, &sm);
  Blur1(ctx, plane_a_, plane_b_, 1.f / 512.f, 1.f / 512.f, ps_down_.Get());  // 512 -> 256 (4 taps)
  Blur1(ctx, plane_b_, plane_c_, 1.f / 256.f, 0, ps_blur_.Get());
  Blur1(ctx, plane_c_, plane_b_, 0, 1.f / 256.f, ps_blur_.Get());             // the finished mask is plane_b_
}

void ShadowMaps::BeginRim(ID3D11DeviceContext* ctx) {
  // clear: red 1 (the game's mask test), green 0 [C: the captured yRimLightMap target (capture rp_ringin, run 6818, the skin draw's
  // fetch 15): every texel outside the wrestlers is (r 1, g 0); green 1 here gave a blurred green halo above the own depth at every
  // silhouette, i.e. a rim term ~10x the game's (oracle rim_strength 0.063 vs 0.006)]
  const float clear[4] = {1, 0, 0, 1};
  ID3D11RenderTargetView* r = rim_a_.rtv.Get();
  ctx->OMSetRenderTargets(1, &r, rim_depth_.dsv.Get());
  ctx->ClearRenderTargetView(r, clear);
  ctx->ClearDepthStencilView(rim_depth_.dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
  D3D11_VIEWPORT vp = {0, 0, 512, 256, 0, 1};
  ctx->RSSetViewports(1, &vp);
}

void ShadowMaps::BlurRim(ID3D11DeviceContext* ctx) {
  ctx->IASetInputLayout(nullptr);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(vs_full_.Get(), nullptr, 0);
  ctx->RSSetState(rs_.Get());
  ctx->OMSetBlendState(bs_off_.Get(), nullptr, 0xFFFFFFFF);
  ctx->OMSetDepthStencilState(ds_off_.Get(), 0);
  ID3D11Buffer* cb = cb_blur_.Get();
  ctx->PSSetConstantBuffers(0, 1, &cb);
  ID3D11SamplerState* sm = lin_.Get();
  ctx->PSSetSamplers(0, 1, &sm);
  Blur1(ctx, rim_a_, rim_b_, 1.f / 512.f, 1.f / 256.f, ps_rim13_.Get());
}

}  // namespace view
