// Session 98 (plan step 16, "click to inspect"), D3D11 side: the ID pass, the 1x1 readback, the highlight (mask -> outline + glow).
// The RendererD3D11 members here use DrawMesh and the frame constants; the shaders are viewer_hlsl.h's (kPick spliced into the
// scene source, kComp); what to draw and the constants come from the shared Renderer (MakePickCB, MakeCompCB, HighlightWanted).
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "pick.h"
#include "pick_gpu.h"
#include "renderer_d3d11.h"
#include "viewer_hlsl.h"

using Microsoft::WRL::ComPtr;

namespace view {

namespace {

std::string HrStr(const char* what, HRESULT hr) {
  char b[160];
  snprintf(b, sizeof b, "%s failed 0x%08lx", what, (unsigned long)hr);
  return b;
}

}  // namespace

// ---- GPU objects ----------------------------------------------------------------------------------------------------------

bool PickGpu::Init(ID3D11Device* dev, const std::string& scene_src, std::string& err) {
  HRESULT hr = S_OK;
  auto make_vs = [&](const std::string& src, const char* entry, ComPtr<ID3D11VertexShader>& o) {
    ComPtr<ID3DBlob> b;
    if (!CompileHlsl(src.c_str(), entry, "vs_5_0", b, err)) return false;
    if (FAILED(hr = dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &o))) { err = HrStr("CreateVertexShader", hr); return false; }
    return true;
  };
  auto make_ps = [&](const std::string& src, const char* entry, ComPtr<ID3D11PixelShader>& o) {
    ComPtr<ID3DBlob> b;
    if (!CompileHlsl(src.c_str(), entry, "ps_5_0", b, err)) return false;
    if (FAILED(hr = dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &o))) { err = HrStr("CreatePixelShader", hr); return false; }
    return true;
  };
  if (!make_vs(scene_src, "vs_id", vs_id) || !make_vs(scene_src, "vs_mask", vs_mask) || !make_ps(scene_src, "ps_id", ps_id) ||
      !make_ps(scene_src, "ps_mask", ps_mask))
    return false;
  const std::string comp = hlsl::kComp;
  if (!make_vs(comp, "vs_fs", vs_fs) || !make_ps(comp, "ps_comp", ps_comp)) return false;

  D3D11_BUFFER_DESC bd = {};
  bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  bd.ByteWidth = sizeof(PickCB);
  if (FAILED(hr = dev->CreateBuffer(&bd, nullptr, &cb_pick))) { err = HrStr("CreateBuffer pick cb", hr); return false; }
  bd.ByteWidth = sizeof(CompCB);
  if (FAILED(hr = dev->CreateBuffer(&bd, nullptr, &cb_comp))) { err = HrStr("CreateBuffer comp cb", hr); return false; }

  D3D11_BLEND_DESC bl = {};
  bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  dev->CreateBlendState(&bl, &bs_none);
  bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
  dev->CreateBlendState(&bl, &bs_mask_r);
  bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_GREEN;
  dev->CreateBlendState(&bl, &bs_mask_g);
  bl = {};
  bl.RenderTarget[0].BlendEnable = TRUE;
  bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO; bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;  // (the back buffer's alpha stays)
  dev->CreateBlendState(&bl, &bs_comp);

  D3D11_DEPTH_STENCIL_DESC dd = {};
  dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
  dev->CreateDepthStencilState(&dd, &ds_write);
  dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  dev->CreateDepthStencilState(&dd, &ds_test);
  D3D11_RASTERIZER_DESC rs = {};
  rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE; rs.DepthClipEnable = TRUE;
  dev->CreateRasterizerState(&rs, &rs_);
  rs.ScissorEnable = TRUE;
  dev->CreateRasterizerState(&rs, &rs_scissor);  // (the hover ID pass: one pixel)
  rs.ScissorEnable = FALSE;
  rs.DepthBias = -4; rs.SlopeScaledDepthBias = -2.0f;  // the highlight mask: a hair nearer than the scene's own depth (slope scaled: no bleed through thin barricades)
  dev->CreateRasterizerState(&rs, &rs_mask);
  return true;
}

bool PickGpu::EnsureId(ID3D11Device* dev, int w, int h) {
  if (id_tex && id_w == w && id_h == h) return true;
  id_tex.Reset(); id_rtv.Reset(); depth_tex.Reset(); depth_dsv.Reset();
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
  td.Format = DXGI_FORMAT_R32G32_UINT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
  if (FAILED(dev->CreateTexture2D(&td, nullptr, &id_tex)) || FAILED(dev->CreateRenderTargetView(id_tex.Get(), nullptr, &id_rtv))) { id_tex.Reset(); return false; }
  td.Format = DXGI_FORMAT_D32_FLOAT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  if (FAILED(dev->CreateTexture2D(&td, nullptr, &depth_tex)) || FAILED(dev->CreateDepthStencilView(depth_tex.Get(), nullptr, &depth_dsv))) { id_tex.Reset(); return false; }
  id_w = w; id_h = h;
  for (int i = 0; i < kStage; i++) pending[i] = false;  // (readbacks of the old size are dropped)
  return true;
}

bool PickGpu::EnsureMask(ID3D11Device* dev, int w, int h) {
  if (mask.t && mask.w == w && mask.h == h) return true;
  return MakeRT(dev, mask, w, h, DXGI_FORMAT_R8G8_UNORM);
}

bool PickGpu::EnsureStage(ID3D11Device* dev) {
  if (stage[0]) return true;
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = 1; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
  td.Format = DXGI_FORMAT_R32G32_UINT; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  for (int i = 0; i < kStage; i++)
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &stage[i]))) { for (auto& s : stage) s.Reset(); return false; }
  return true;
}

// ---- the ID pass + readback ----------------------------------------------------------------------------------------------

void RendererD3D11::IdPass(const Camera& cam, int w, int h, int sx, int sy) {
  if (!pick_.EnsureId(dev_.Get(), w, h)) return;
  ID3D11ShaderResourceView* nul[8] = {};
  ctx_->PSSetShaderResources(0, 8, nul);
  ID3D11RenderTargetView* rtv = pick_.id_rtv.Get();
  ctx_->OMSetRenderTargets(1, &rtv, pick_.depth_dsv.Get());
  D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
  ctx_->RSSetViewports(1, &vp);
  const float zero[4] = {0, 0, 0, 0};
  ctx_->ClearRenderTargetView(rtv, zero);
  ctx_->ClearDepthStencilView(pick_.depth_dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
  UploadFrameCB(MakeFrameCB(cam, w, h));
  BindCommon(pick_.vs_id.Get(), pick_.ps_id.Get());
  if (sx >= 0 && sy >= 0) {  // hover: only the pixel under the cursor needs shading
    const D3D11_RECT sc = {sx, sy, sx + 1, sy + 1};
    ctx_->RSSetScissorRects(1, &sc);
    ctx_->RSSetState(pick_.rs_scissor.Get());
  }
  ID3D11Buffer* pcb = pick_.cb_pick.Get();
  ctx_->VSSetConstantBuffers(3, 1, &pcb);
  ctx_->PSSetConstantBuffers(3, 1, &pcb);
  ctx_->OMSetDepthStencilState(pick_.ds_write.Get(), 0);
  ctx_->OMSetBlendState(pick_.bs_none.Get(), nullptr, 0xFFFFFFFF);
  draw_inst_ = -1;
  for (size_t mi = 0; mi < models_.size(); mi++) {
    const GpuModel& gm = models_[mi];
    for (size_t k = 0; k < gm.meshes.size(); k++) {
      if (PickSkip(gm, k)) continue;
      const PickCB pc = MakePickCB(mi, k, true);
      D3D11_MAPPED_SUBRESOURCE ms;
      if (SUCCEEDED(ctx_->Map(pick_.cb_pick.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) { memcpy(ms.pData, &pc, sizeof pc); ctx_->Unmap(pick_.cb_pick.Get(), 0); }
      DrawMesh(gm, gm.meshes[k], false);
    }
  }
  ID3D11ShaderResourceView* nul2[4] = {};
  ctx_->PSSetShaderResources(0, 4, nul2);
  ctx_->OMSetRenderTargets(0, nullptr, nullptr);
}

void RendererD3D11::QueueReadback(int x, int y) {
  if (!pick_.id_tex || !pick_.EnsureStage(dev_.Get())) return;
  if (x < 0 || y < 0 || x >= pick_.id_w || y >= pick_.id_h) return;
  int s = -1;
  for (int i = 0; i < PickGpu::kStage; i++)
    if (!pick_.pending[i]) { s = i; break; }
  if (s < 0) return;  // all three still in flight: this request is dropped (the viewer asks again)
  const D3D11_BOX box = {(UINT)x, (UINT)y, 0, (UINT)x + 1, (UINT)y + 1, 1};
  ctx_->CopySubresourceRegion(pick_.stage[s].Get(), 0, 0, 0, 0, pick_.id_tex.Get(), 0, &box);
  pick_.pending[s] = true;
  pick_.pend_seq[s] = ++pick_.seq;
  pick_.pend_gen[s] = models_gen_;
}

void RendererD3D11::PollReadback() {
  unsigned best_seq = 0;
  bool got = false;
  PickHit best;
  for (int i = 0; i < PickGpu::kStage; i++) {
    if (!pick_.pending[i]) continue;
    D3D11_MAPPED_SUBRESOURCE ms;
    const HRESULT hr = ctx_->Map(pick_.stage[i].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
    pick_.pending[i] = false;
    if (FAILED(hr)) continue;
    if (pick_.pend_gen[i] != models_gen_) { ctx_->Unmap(pick_.stage[i].Get(), 0); continue; }  // (queued before the models changed)
    const uint32_t* px = (const uint32_t*)ms.pData;
    const PickHit hit = DecodeId(px[0], px[1]);
    ctx_->Unmap(pick_.stage[i].Get(), 0);
    if (!got || pick_.pend_seq[i] > best_seq) best = hit, best_seq = pick_.pend_seq[i], got = true;
  }
  if (got) hover_hit_ = best;
}

PickHit RendererD3D11::PickRead(const Camera& cam, int w, int h, int x, int y, std::string& err) {
  IdPass(cam, w, h);
  if (!pick_.id_tex || !pick_.EnsureStage(dev_.Get())) { err = "the ID target could not be made"; return PickHit(); }
  const D3D11_BOX box = {(UINT)x, (UINT)y, 0, (UINT)x + 1, (UINT)y + 1, 1};
  const int s = PickGpu::kStage - 1;
  pick_.pending[s] = false;
  ctx_->CopySubresourceRegion(pick_.stage[s].Get(), 0, 0, 0, 0, pick_.id_tex.Get(), 0, &box);
  D3D11_MAPPED_SUBRESOURCE ms;
  const HRESULT hr = ctx_->Map(pick_.stage[s].Get(), 0, D3D11_MAP_READ, 0, &ms);
  PickHit hit;
  if (FAILED(hr)) err = HrStr("pick readback", hr);
  else {
    const uint32_t* px = (const uint32_t*)ms.pData;
    hit = DecodeId(px[0], px[1]);
    ctx_->Unmap(pick_.stage[s].Get(), 0);
  }
  for (int i = 0; i < PickGpu::kStage; i++) pick_.pending[i] = false;  // (an in-flight hover readback of another size is stale now)
  ctx_->ClearState();
  ctx_->Flush();
  return hit;
}

// ---- the highlight --------------------------------------------------------------------------------------------------------

void RendererD3D11::HighlightPass(ID3D11RenderTargetView* out, int w, int h, const FrameCB& fcb, GpuTimer* tm) {
  bool sel = false, hov = false;
  if (!HighlightWanted(sel, hov)) return;
  if (!pick_.EnsureMask(dev_.Get(), w, h) || !post_.SceneDsv()) return;
  ID3D11ShaderResourceView* nul[8] = {};
  ctx_->PSSetShaderResources(0, 8, nul);
  ctx_->VSSetShaderResources(0, 1, nul);
  const float zero[4] = {0, 0, 0, 0};
  ctx_->ClearRenderTargetView(pick_.mask.rtv.Get(), zero);
  ID3D11RenderTargetView* mrtv = pick_.mask.rtv.Get();
  ctx_->OMSetRenderTargets(1, &mrtv, post_.SceneDsv());  // the scene's own depth: a part behind something shows only its visible area
  D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
  ctx_->RSSetViewports(1, &vp);
  UploadFrameCB(fcb);
  BindCommon(pick_.vs_mask.Get(), pick_.ps_mask.Get());
  ctx_->RSSetState(pick_.rs_mask.Get());
  ID3D11Buffer* pcb = pick_.cb_pick.Get();
  ctx_->VSSetConstantBuffers(3, 1, &pcb);
  ctx_->PSSetConstantBuffers(3, 1, &pcb);
  ctx_->OMSetDepthStencilState(pick_.ds_test.Get(), 0);
  auto draw = [&](const PickHit& p, ID3D11BlendState* bs) {
    const GpuModel& gm = models_[(size_t)p.model];
    const GpuMesh& me = gm.meshes[(size_t)p.mesh];
    ctx_->OMSetBlendState(bs, nullptr, 0xFFFFFFFF);
    const PickCB pc = MakePickCB((size_t)p.model, (size_t)p.mesh, false);
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(ctx_->Map(pick_.cb_pick.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) { memcpy(ms.pData, &pc, sizeof pc); ctx_->Unmap(pick_.cb_pick.Get(), 0); }
    draw_inst_ = p.instance;
    DrawMesh(gm, me, false);
    draw_inst_ = -1;
  };
  if (sel) draw(hl_sel_, pick_.bs_mask_r.Get());
  if (hov) draw(hl_hover_, pick_.bs_mask_g.Get());
  ctx_->PSSetShaderResources(0, 8, nul);

  // composite onto the finished picture
  const CompCB cc = MakeCompCB(w, h, sel, hov);
  D3D11_MAPPED_SUBRESOURCE ms;
  if (SUCCEEDED(ctx_->Map(pick_.cb_comp.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) { memcpy(ms.pData, &cc, sizeof cc); ctx_->Unmap(pick_.cb_comp.Get(), 0); }
  ID3D11RenderTargetView* o = out;
  ctx_->OMSetRenderTargets(1, &o, nullptr);
  ctx_->IASetInputLayout(nullptr);
  ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx_->VSSetShader(pick_.vs_fs.Get(), nullptr, 0);
  ctx_->PSSetShader(pick_.ps_comp.Get(), nullptr, 0);
  ID3D11Buffer* ccb = pick_.cb_comp.Get();
  ctx_->PSSetConstantBuffers(0, 1, &ccb);
  ID3D11ShaderResourceView* msrv = pick_.mask.srv.Get();
  ctx_->PSSetShaderResources(0, 1, &msrv);
  ctx_->OMSetBlendState(pick_.bs_comp.Get(), nullptr, 0xFFFFFFFF);
  ctx_->OMSetDepthStencilState(pick_.ds_test.Get(), 0);
  ctx_->RSSetState(pick_.rs_.Get());
  ctx_->Draw(3, 0);
  ctx_->PSSetShaderResources(0, 1, nul);
  if (tm) tm->Mark(ctx_.Get(), "highlight");
}

}  // namespace view
