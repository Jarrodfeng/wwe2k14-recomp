// The D3D11 backend (renderer_d3d11.h). Session 98 phone port, stage A: the former renderer.cpp, its passes unchanged; the
// constants, draw lists and post plan come from the shared Renderer (renderer.cpp / render_shared.cpp), the HLSL from viewer_hlsl.h.
#include "renderer_d3d11.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "viewer_hlsl.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using assets::TexFormat;

namespace view {

namespace {

// The model's D3D11 objects: textures by slot, one vertex / index buffer pair per mesh (GpuMesh::buf), one instance buffer per
// placement (null when it has no instances), the bone palette.
struct D3DModel : GpuModelData {
  std::vector<ComPtr<ID3D11ShaderResourceView>> textures;  // null where the upload failed
  struct Buf { ComPtr<ID3D11Buffer> vb, ib; };
  std::vector<Buf> bufs;
  std::vector<ComPtr<ID3D11Buffer>> placements;
  ComPtr<ID3D11Buffer> bones;  // the skinning palette (dynamic); null = identity
  ComPtr<ID3D11ShaderResourceView> bones_srv;
};
D3DModel& DM(const GpuModel& g) { return *static_cast<D3DModel*>(g.gpu.get()); }

std::string HrStr(const char* what, HRESULT hr) {
  char b[160];
  snprintf(b, sizeof b, "%s failed 0x%08lx", what, (unsigned long)hr);
  return b;
}

}  // namespace

std::unique_ptr<Renderer> MakeRendererD3D11() { return std::make_unique<RendererD3D11>(); }

bool RendererD3D11::Init(const Surface& surf, int w, int h, std::string& err) {
  void* hwnd = surf.hwnd;
  w_ = w; h_ = h;
  D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fls, 2, D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
  if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fls + 1, 1, D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
  if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, fls + 1, 1, D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
  if (FAILED(hr)) { err = HrStr("D3D11CreateDevice", hr); return false; }

  ComPtr<IDXGIDevice> dxdev;
  ComPtr<IDXGIAdapter> adapter;
  dev_.As(&dxdev);
  if (dxdev && SUCCEEDED(dxdev->GetAdapter(&adapter))) {
    DXGI_ADAPTER_DESC ad;
    if (SUCCEEDED(adapter->GetDesc(&ad))) {
      char nm[256];
      WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, nm, sizeof nm, nullptr, nullptr);
      adapter_ = nm;
    }
  }
  UINT fs = 0;
  if (SUCCEEDED(dev_->CheckFormatSupport(DXGI_FORMAT_B4G4R4A4_UNORM, &fs)) && (fs & D3D11_FORMAT_SUPPORT_TEXTURE2D))
    b4g4r4a4_ok_ = true;

  if (hwnd) {
    ComPtr<IDXGIFactory> fac;
    if (!adapter || FAILED(adapter->GetParent(IID_PPV_ARGS(&fac)))) { err = "no DXGI factory"; return false; }
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width = (UINT)w; sd.BufferDesc.Height = (UINT)h;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = (HWND)hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = fac->CreateSwapChain(dev_.Get(), &sd, &sc_);
    if (FAILED(hr)) { err = HrStr("CreateSwapChain", hr); return false; }
    fac->MakeWindowAssociation((HWND)hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (!CreateBackBufferViews()) { err = "back buffer views failed"; return false; }
  }
  if (!CreateShaders(err)) return false;

  D3D11_BUFFER_DESC bd = {};
  bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  bd.ByteWidth = sizeof(FrameCB);
  if (FAILED(hr = dev_->CreateBuffer(&bd, nullptr, &cb_frame_))) { err = HrStr("CreateBuffer cb", hr); return false; }
  bd.ByteWidth = sizeof(DrawCB);
  if (FAILED(hr = dev_->CreateBuffer(&bd, nullptr, &cb_draw_))) { err = HrStr("CreateBuffer cb", hr); return false; }

  D3D11_SAMPLER_DESC sm = {};
  sm.Filter = D3D11_FILTER_ANISOTROPIC; sm.MaxAnisotropy = 8;
  sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
  sm.MaxLOD = D3D11_FLOAT32_MAX;
  dev_->CreateSamplerState(&sm, &sampler_);
  sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;  // the stage lightmap (outside its span: edge texel)
  dev_->CreateSamplerState(&sm, &sampler_clamp_);
  D3D11_RASTERIZER_DESC rs = {};
  rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE; rs.DepthClipEnable = TRUE;
  dev_->CreateRasterizerState(&rs, &raster_);
  // Session 98 (stage fix): the materials the game draws with back faces culled (Material::cull_back). The side: D3D11_CULL_BACK keeps
  // the big screen / banners (A/B out\s98\stage_fix\cull1: CULL_FRONT removed them, backdrop x0.36), i.e. after the loader's Y mirror
  // the files' front faces are the viewer's clockwise ones. `raster_cull_other_` = the other side (A/B only).
  rs.CullMode = D3D11_CULL_BACK;
  dev_->CreateRasterizerState(&rs, &raster_cull_);
  rs.CullMode = D3D11_CULL_FRONT;
  dev_->CreateRasterizerState(&rs, &raster_cull_other_);
  D3D11_DEPTH_STENCIL_DESC dd = {};
  dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
  dev_->CreateDepthStencilState(&dd, &ds_opaque_);
  dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  dev_->CreateDepthStencilState(&dd, &ds_blend_);
  D3D11_BLEND_DESC bl = {};
  bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  dev_->CreateBlendState(&bl, &bs_opaque_);
  bl.RenderTarget[0].BlendEnable = TRUE;
  bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  dev_->CreateBlendState(&bl, &bs_blend_);
  bl.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;  // (S97) additive: glows, beams, fog ( slots)
  bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
  dev_->CreateBlendState(&bl, &bs_add_);
  if (!post_.Init(dev_.Get(), err) || !shadow_.Init(dev_.Get(), err)) return false;  // Session 98
  timer_.Init(dev_.Get());
  return true;
}

bool RendererD3D11::CreateShaders(std::string& err) {
  ComPtr<ID3DBlob> vsb, psb, msg;
  // Session 98: the shadow receivers and caster entry points (shadow) and the ID / mask entry points (pick) spliced in at the marker.
  const std::string src = hlsl::SceneSource();
  if (src.empty()) { err = "shader marker missing"; return false; }
  HRESULT hr = S_OK;
  if (!CompileHlsl(src.c_str(), "vs", "vs_5_0", vsb, err)) return false;
  if (!CompileHlsl(src.c_str(), "ps", "ps_5_0", psb, err)) return false;
  {
    ComPtr<ID3DBlob> b;
    if (!CompileHlsl(src.c_str(), "ps_caster", "ps_5_0", b, err)) return false;
    if (FAILED(hr = dev_->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps_caster_))) { err = HrStr("CreatePixelShader", hr); return false; }
    if (!CompileHlsl(src.c_str(), "ps_rim", "ps_5_0", b, err)) return false;
    if (FAILED(hr = dev_->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps_rim_))) { err = HrStr("CreatePixelShader", hr); return false; }
    if (!CompileHlsl(src.c_str(), "ps_plane", "ps_5_0", b, err)) return false;
    if (FAILED(hr = dev_->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps_plane_))) { err = HrStr("CreatePixelShader", hr); return false; }
    ComPtr<ID3DBlob> vp;
    if (!CompileHlsl(src.c_str(), "vs_plane", "vs_5_0", vp, err)) return false;
    if (FAILED(hr = dev_->CreateVertexShader(vp->GetBufferPointer(), vp->GetBufferSize(), nullptr, &vs_plane_))) { err = HrStr("CreateVertexShader", hr); return false; }
  }
  if (!pick_.Init(dev_.Get(), src, err)) return false;  // Session 98 (step 16)
  if (FAILED(hr = dev_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_))) { err = HrStr("CreateVertexShader", hr); return false; }
  if (FAILED(hr = dev_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_))) { err = HrStr("CreatePixelShader", hr); return false; }
  D3D11_INPUT_ELEMENT_DESC el[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(assets::Vertex, pos), D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(assets::Vertex, normal), D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(assets::Vertex, uv0), D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(assets::Vertex, uv1), D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(assets::Vertex, color), D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT, 0, offsetof(assets::Vertex, bones), D3D11_INPUT_PER_VERTEX_DATA, 0},  // (S99: 16-bit)
      {"BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(assets::Vertex, weights), D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(assets::Vertex, tangent), D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"INST", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
      {"INST", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
      {"INST", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
      {"INST", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1},
  };
  if (FAILED(hr = dev_->CreateInputLayout(el, UINT(sizeof el / sizeof el[0]), vsb->GetBufferPointer(), vsb->GetBufferSize(), &layout_))) { err = HrStr("CreateInputLayout", hr); return false; }
  // One identity instance and one identity bone, for meshes with no placement / models with no palette.
  const assets::Matrix ident = assets::Identity();
  if (!MakeMatrixBuffer(&ident, 1, false, ident_inst_, nullptr, err)) return false;
  if (!MakeMatrixBuffer(&ident, 1, true, ident_bones_, &ident_bones_srv_, err)) return false;
  {  // Session 98 (flash fix): the crowd camera-flash sprite (FlashPass)
    ComPtr<ID3DBlob> fv, fp;
    if (!CompileHlsl(hlsl::kFlash, "vs_flash", "vs_5_0", fv, err)) return false;
    if (!CompileHlsl(hlsl::kFlash, "ps_flash", "ps_5_0", fp, err)) return false;
    if (FAILED(hr = dev_->CreateVertexShader(fv->GetBufferPointer(), fv->GetBufferSize(), nullptr, &vs_flash_))) { err = HrStr("CreateVertexShader flash", hr); return false; }
    if (FAILED(hr = dev_->CreatePixelShader(fp->GetBufferPointer(), fp->GetBufferSize(), nullptr, &ps_flash_))) { err = HrStr("CreatePixelShader flash", hr); return false; }
    D3D11_INPUT_ELEMENT_DESC fe[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(assets::Vertex, pos), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(assets::Vertex, uv0), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(assets::Vertex, color), D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(hr = dev_->CreateInputLayout(fe, UINT(sizeof fe / sizeof fe[0]), fv->GetBufferPointer(), fv->GetBufferSize(), &layout_flash_))) { err = HrStr("CreateInputLayout flash", hr); return false; }
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = sizeof(FlashCB);
    if (FAILED(hr = dev_->CreateBuffer(&bd, nullptr, &cb_flash_))) { err = HrStr("CreateBuffer flash", hr); return false; }
    D3D11_SAMPLER_DESC sm = {};
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sm.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(hr = dev_->CreateSamplerState(&sm, &sampler_point_))) { err = HrStr("CreateSamplerState flash", hr); return false; }
  }
  {  // Session 98 (cap / particles): the entrance particle sprites (SpritePass)
    ComPtr<ID3DBlob> sv, sp;
    if (!CompileHlsl(hlsl::kSprite, "vs_sprite", "vs_5_0", sv, err)) return false;
    if (!CompileHlsl(hlsl::kSprite, "ps_sprite", "ps_5_0", sp, err)) return false;
    if (FAILED(hr = dev_->CreateVertexShader(sv->GetBufferPointer(), sv->GetBufferSize(), nullptr, &vs_sprite_))) { err = HrStr("CreateVertexShader sprite", hr); return false; }
    if (FAILED(hr = dev_->CreatePixelShader(sp->GetBufferPointer(), sp->GetBufferSize(), nullptr, &ps_sprite_))) { err = HrStr("CreatePixelShader sprite", hr); return false; }
    D3D11_INPUT_ELEMENT_DESC se[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(hr = dev_->CreateInputLayout(se, 3, sv->GetBufferPointer(), sv->GetBufferSize(), &layout_sprite_))) { err = HrStr("CreateInputLayout sprite", hr); return false; }
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = 80;  // vpc[4] + soft (Session 99, particles 4)
    if (FAILED(hr = dev_->CreateBuffer(&bd, nullptr, &cb_sprite_))) { err = HrStr("CreateBuffer sprite", hr); return false; }
  }
  return true;
}

// Session 98 (cap / particles): an entrance particle sprite texture (a DDS of the effect's own texture directory) -> its slot.
int RendererD3D11::AddSpriteTexture(const assets::Texture& t) {
  std::vector<std::string> warn;
  ComPtr<ID3D11ShaderResourceView> srv = MakeTexture(t, warn);
  if (!srv) return -1;
  sprite_tex_.push_back(srv);
  return int(sprite_tex_.size()) - 1;
}

// Session 98 (cap / particles): the sprites into the scene colour, depth-tested against the scene, before the post chain.
void RendererD3D11::SpritePass(int w, int h, const FrameCB& fcb) {
  if (sprites_.empty() || !vs_sprite_ || !ps_sprite_) return;
  using V = SpriteVertex;
  std::vector<V> vb;
  std::vector<SpriteRun> runs;
  BuildSprites(fcb, sprite_tex_.size(), vb, runs);
  if (vb.empty()) return;
  if (!vb_sprite_ || vb_sprite_cap_ < vb.size()) {
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = UINT(sizeof(V) * (vb.size() + 1024));
    vb_sprite_.Reset();
    if (FAILED(dev_->CreateBuffer(&bd, nullptr, &vb_sprite_))) return;
    vb_sprite_cap_ = vb.size() + 1024;
  }
  D3D11_MAPPED_SUBRESOURCE ms;
  if (FAILED(ctx_->Map(vb_sprite_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
  memcpy(ms.pData, vb.data(), sizeof(V) * vb.size());
  ctx_->Unmap(vb_sprite_.Get(), 0);
  float cbv[20];
  SpriteConsts(fcb, cbv);
  if (SUCCEEDED(ctx_->Map(cb_sprite_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) { memcpy(ms.pData, cbv, sizeof cbv); ctx_->Unmap(cb_sprite_.Get(), 0); }
  // Session 99 (particles 4): no depth attachment; the pixel shader reads the scene depth (its own depth test + the soft fade)
  ID3D11RenderTargetView* rtv = post_.SceneRtv();
  ctx_->OMSetRenderTargets(1, &rtv, nullptr);
  ID3D11ShaderResourceView* dsrv = post_.SceneDepthSrv();
  ctx_->PSSetShaderResources(1, 1, &dsrv);
  D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
  ctx_->RSSetViewports(1, &vp);
  ctx_->RSSetState(raster_.Get());
  ctx_->OMSetBlendState(bs_blend_.Get(), nullptr, 0xFFFFFFFF);
  ctx_->OMSetDepthStencilState(nullptr, 0);
  ctx_->IASetInputLayout(layout_sprite_.Get());
  ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  UINT stride = sizeof(V), off = 0;
  ID3D11Buffer* b = vb_sprite_.Get();
  ctx_->IASetVertexBuffers(0, 1, &b, &stride, &off);
  ctx_->VSSetShader(vs_sprite_.Get(), nullptr, 0);
  ctx_->PSSetShader(ps_sprite_.Get(), nullptr, 0);
  ID3D11Buffer* cb = cb_sprite_.Get();
  ctx_->VSSetConstantBuffers(0, 1, &cb);
  ctx_->PSSetConstantBuffers(0, 1, &cb);  // (the pixel shader reads soft: q, nq, 1 / range)
  ID3D11SamplerState* sm = sampler_clamp_.Get();
  ctx_->PSSetSamplers(0, 1, &sm);
  for (const auto& r : runs) {
    ID3D11ShaderResourceView* t = sprite_tex_[size_t(r.tex)].Get();
    ctx_->PSSetShaderResources(0, 1, &t);
    ctx_->Draw(UINT(r.count), UINT(r.first));
  }
  ID3D11ShaderResourceView* nul[2] = {};
  ctx_->PSSetShaderResources(0, 2, nul);
  ctx_->OMSetRenderTargets(0, nullptr, nullptr);
}

bool RendererD3D11::CreateBackBufferViews() {
  bb_rtv_.Reset(); depth_dsv_.Reset(); depth_tex_.Reset();
  ComPtr<ID3D11Texture2D> bb;
  if (FAILED(sc_->GetBuffer(0, IID_PPV_ARGS(&bb)))) return false;
  if (FAILED(dev_->CreateRenderTargetView(bb.Get(), nullptr, &bb_rtv_))) return false;
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)w_; td.Height = (UINT)h_; td.MipLevels = 1; td.ArraySize = 1;
  td.Format = DXGI_FORMAT_D32_FLOAT; td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  if (FAILED(dev_->CreateTexture2D(&td, nullptr, &depth_tex_)) ||
      FAILED(dev_->CreateDepthStencilView(depth_tex_.Get(), nullptr, &depth_dsv_))) {
    bb_rtv_.Reset(); depth_dsv_.Reset(); depth_tex_.Reset();  // (never leave a render target without its depth view)
    return false;
  }
  return true;
}

void RendererD3D11::Resize(int w, int h) {
  if (!sc_ || w <= 0 || h <= 0) return;
  w_ = w; h_ = h;
  ctx_->OMSetRenderTargets(0, nullptr, nullptr);
  bb_rtv_.Reset(); depth_dsv_.Reset(); depth_tex_.Reset();
  if (SUCCEEDED(sc_->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0))) CreateBackBufferViews();
}

// ---- textures --------------------------------------------------------------------------------------------------------

static size_t MipBytes(TexFormat f, uint32_t w, uint32_t h) {
  switch (f) {
    case TexFormat::kBC1: return (size_t)((w + 3) / 4) * ((h + 3) / 4) * 8;
    case TexFormat::kBC2:
    case TexFormat::kBC3: return (size_t)((w + 3) / 4) * ((h + 3) / 4) * 16;
    case TexFormat::kB4G4R4A4: return (size_t)w * h * 2;
    case TexFormat::kB8G8R8A8:
    case TexFormat::kR8G8B8A8: return (size_t)w * h * 4;
    default: return 0;
  }
}
static UINT MipPitch(TexFormat f, uint32_t w) {
  switch (f) {
    case TexFormat::kBC1: return ((w + 3) / 4) * 8;
    case TexFormat::kBC2:
    case TexFormat::kBC3: return ((w + 3) / 4) * 16;
    case TexFormat::kB4G4R4A4: return w * 2;
    default: return w * 4;
  }
}

ComPtr<ID3D11ShaderResourceView> RendererD3D11::MakeTexture(const assets::Texture& t, std::vector<std::string>& warnings) {
  ComPtr<ID3D11ShaderResourceView> none;
  auto warn = [&](const std::string& m) { warnings.push_back("texture '" + t.name + "': " + m); };
  if (t.width == 0 || t.height == 0 || t.format == TexFormat::kUnknown) { warn("empty or unknown format"); return none; }
  bool bc = t.format == TexFormat::kBC1 || t.format == TexFormat::kBC2 || t.format == TexFormat::kBC3;
  if (bc && ((t.width % 4) || (t.height % 4))) { warn("block-compressed size not a multiple of 4"); return none; }
  // how many mips fit the data
  uint32_t mips = std::max(1u, t.mips);
  {  // (no more mips than the size has: a bad header must not reach CreateTexture2D with a huge count)
    uint32_t mx = std::max(t.width, t.height), maxmips = 1;
    while (mx > 1) { mx >>= 1; maxmips++; }
    mips = std::min(mips, maxmips);
  }
  size_t total = 0;
  uint32_t fit = 0;
  for (uint32_t i = 0; i < mips; i++) {
    uint32_t mw = std::max(1u, t.width >> i), mh = std::max(1u, t.height >> i);
    size_t sz = MipBytes(t.format, mw, mh);
    if (total + sz > t.data.size()) break;
    total += sz; fit++;
  }
  if (fit == 0) { warn("data shorter than mip 0"); return none; }
  if (fit < mips) warn("data holds only " + std::to_string(fit) + " of " + std::to_string(mips) + " mips");
  bool conv = t.format == TexFormat::kB4G4R4A4 && !b4g4r4a4_ok_;
  DXGI_FORMAT df = DXGI_FORMAT_UNKNOWN;
  switch (t.format) {
    case TexFormat::kBC1: df = DXGI_FORMAT_BC1_UNORM; break;
    case TexFormat::kBC2: df = DXGI_FORMAT_BC2_UNORM; break;
    case TexFormat::kBC3: df = DXGI_FORMAT_BC3_UNORM; break;
    case TexFormat::kB4G4R4A4: df = conv ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B4G4R4A4_UNORM; break;
    case TexFormat::kB8G8R8A8: df = DXGI_FORMAT_B8G8R8A8_UNORM; break;
    case TexFormat::kR8G8B8A8: df = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    default: break;
  }
  std::vector<uint8_t> cbuf;
  std::vector<D3D11_SUBRESOURCE_DATA> sub(fit);
  if (conv) {
    cbuf.resize(total * 2);
    size_t src = 0, dst = 0;
    for (uint32_t i = 0; i < fit; i++) {
      uint32_t mw = std::max(1u, t.width >> i), mh = std::max(1u, t.height >> i);
      sub[i].pSysMem = nullptr;  // fixed below once cbuf is final
      for (size_t k = 0; k < (size_t)mw * mh; k++) {
        uint16_t px = (uint16_t)(t.data[src] | (t.data[src + 1] << 8));
        src += 2;
        cbuf[dst++] = (uint8_t)(((px >> 8) & 15) * 17);  // R
        cbuf[dst++] = (uint8_t)(((px >> 4) & 15) * 17);  // G
        cbuf[dst++] = (uint8_t)((px & 15) * 17);         // B
        cbuf[dst++] = (uint8_t)(((px >> 12) & 15) * 17); // A
      }
    }
    size_t off = 0;
    for (uint32_t i = 0; i < fit; i++) {
      uint32_t mw = std::max(1u, t.width >> i), mh = std::max(1u, t.height >> i);
      sub[i].pSysMem = cbuf.data() + off;
      sub[i].SysMemPitch = mw * 4;
      off += (size_t)mw * mh * 4;
    }
  } else {
    size_t off = 0;
    for (uint32_t i = 0; i < fit; i++) {
      uint32_t mw = std::max(1u, t.width >> i), mh = std::max(1u, t.height >> i);
      sub[i].pSysMem = t.data.data() + off;
      sub[i].SysMemPitch = MipPitch(t.format, mw);
      off += MipBytes(t.format, mw, mh);
    }
  }
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = t.width; td.Height = t.height; td.MipLevels = fit; td.ArraySize = 1; td.Format = df;
  td.SampleDesc.Count = 1; td.Usage = t.live ? D3D11_USAGE_DEFAULT : D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  ComPtr<ID3D11Texture2D> tex;
  HRESULT hr = dev_->CreateTexture2D(&td, sub.data(), &tex);
  if (FAILED(hr)) { warn(HrStr("CreateTexture2D", hr)); return none; }
  ComPtr<ID3D11ShaderResourceView> srv;
  hr = dev_->CreateShaderResourceView(tex.Get(), nullptr, &srv);
  if (FAILED(hr)) { warn(HrStr("CreateShaderResourceView", hr)); return none; }
  return srv;
}

// ---- model objects (the shared AddModel's hooks) ---------------------------------------------------------------------

std::unique_ptr<GpuModelData> RendererD3D11::NewModelData() { return std::make_unique<D3DModel>(); }

bool RendererD3D11::UploadTexture(GpuModelData& d, const assets::Texture& t, std::vector<std::string>& warnings) {
  auto& dm = static_cast<D3DModel&>(d);
  dm.textures.push_back(MakeTexture(t, warnings));
  return dm.textures.back() != nullptr;
}

bool RendererD3D11::UploadPlacement(GpuModelData& d, const std::vector<assets::Matrix>& transforms, std::string& err) {
  auto& dm = static_cast<D3DModel&>(d);
  ComPtr<ID3D11Buffer> vb;
  if (!transforms.empty() && !MakeMatrixBuffer(transforms.data(), transforms.size(), false, vb, nullptr, err)) return false;
  dm.placements.push_back(vb);
  return true;
}

bool RendererD3D11::UploadMesh(GpuModelData& d, const assets::Mesh& me, int& buf, std::string& err) {
  auto& dm = static_cast<D3DModel&>(d);
  D3DModel::Buf b;
  D3D11_BUFFER_DESC bd = {};
  bd.Usage = D3D11_USAGE_IMMUTABLE;
  bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  bd.ByteWidth = (UINT)(me.vertices.size() * sizeof(assets::Vertex));
  D3D11_SUBRESOURCE_DATA sd = {me.vertices.data(), 0, 0};
  HRESULT hr = dev_->CreateBuffer(&bd, &sd, &b.vb);
  if (FAILED(hr)) { err = HrStr("vertex buffer", hr); return false; }
  bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
  bd.ByteWidth = (UINT)(me.indices.size() * 4);
  sd.pSysMem = me.indices.data();
  hr = dev_->CreateBuffer(&bd, &sd, &b.ib);
  if (FAILED(hr)) { err = HrStr("index buffer", hr); return false; }
  buf = (int)dm.bufs.size();
  dm.bufs.push_back(std::move(b));
  return true;
}

uint64_t RendererD3D11::UiTexture(size_t model, size_t tex) {
  if (model >= models_.size()) return 0;
  const D3DModel& d = DM(models_[model]);
  return tex < d.textures.size() ? (uint64_t)(uintptr_t)d.textures[tex].Get() : 0;
}

bool RendererD3D11::UpdateTexture(GpuModel& g, size_t slot, int w, int h, const uint8_t* rgba) {
  if (!ctx_) return false;
  D3DModel& d = DM(g);
  if (slot >= d.textures.size() || !d.textures[slot]) return false;
  ComPtr<ID3D11Resource> res;
  d.textures[slot]->GetResource(&res);
  if (!res) return false;
  ctx_->UpdateSubresource(res.Get(), 0, nullptr, rgba, (UINT)w * 4, 0);
  return true;
}

// ---- matrices (instances, bone palettes) -----------------------------------------------------------------------------

bool RendererD3D11::MakeMatrixBuffer(const assets::Matrix* m, size_t n, bool structured, ComPtr<ID3D11Buffer>& buf,
                                     ComPtr<ID3D11ShaderResourceView>* srv, std::string& err) {
  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = UINT(std::max<size_t>(n, 1) * sizeof(assets::Matrix));
  bd.Usage = structured ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_IMMUTABLE;
  bd.CPUAccessFlags = structured ? D3D11_CPU_ACCESS_WRITE : 0;
  bd.BindFlags = structured ? D3D11_BIND_SHADER_RESOURCE : D3D11_BIND_VERTEX_BUFFER;
  if (structured) bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, bd.StructureByteStride = 16;  // (rows: see the shader)
  D3D11_SUBRESOURCE_DATA sd = {m, 0, 0};
  HRESULT hr = dev_->CreateBuffer(&bd, &sd, &buf);
  if (FAILED(hr)) { err = HrStr(structured ? "bone buffer" : "instance buffer", hr); return false; }
  if (srv) {
    D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
    vd.Format = DXGI_FORMAT_UNKNOWN; vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    vd.Buffer.NumElements = UINT(std::max<size_t>(n, 1) * (structured ? 4 : 1));
    hr = dev_->CreateShaderResourceView(buf.Get(), &vd, srv->ReleaseAndGetAddressOf());
    if (FAILED(hr)) { err = HrStr("bone view", hr); return false; }
  }
  return true;
}

void RendererD3D11::DropBones(GpuModel& g) {
  D3DModel& d = DM(g);
  d.bones.Reset(); d.bones_srv.Reset();
}

bool RendererD3D11::UploadBones(GpuModel& g, const std::vector<assets::Matrix>& full, std::string& err) {
  D3DModel& d = DM(g);
  if (!d.bones || g.bone_count != full.size()) {
    if (!MakeMatrixBuffer(full.data(), full.size(), true, d.bones, &d.bones_srv, err)) {
      d.bones.Reset(); d.bones_srv.Reset(); g.has_bones = false; g.bone_count = 0;
      return false;
    }
    g.bone_count = full.size();
    g.has_bones = true;
    return true;
  }
  D3D11_MAPPED_SUBRESOURCE ms;
  if (FAILED(ctx_->Map(d.bones.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) { err = "Map bones failed"; return false; }
  memcpy(ms.pData, full.data(), full.size() * sizeof(assets::Matrix));
  ctx_->Unmap(d.bones.Get(), 0);
  return true;
}

// ---- drawing ---------------------------------------------------------------------------------------------------------

void RendererD3D11::UploadFrameCB(const FrameCB& fcb) {
  D3D11_MAPPED_SUBRESOURCE ms;
  if (SUCCEEDED(ctx_->Map(cb_frame_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) { memcpy(ms.pData, &fcb, sizeof fcb); ctx_->Unmap(cb_frame_.Get(), 0); }
}

void RendererD3D11::BindCommon(ID3D11VertexShader* vs, ID3D11PixelShader* ps) {
  ctx_->IASetInputLayout(layout_.Get());
  ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx_->VSSetShader(vs, nullptr, 0);
  ctx_->PSSetShader(ps, nullptr, 0);
  ID3D11Buffer* cbs[3] = {cb_frame_.Get(), cb_draw_.Get(), shadow_.CbBuffer()};
  ctx_->VSSetConstantBuffers(0, 3, cbs);
  ctx_->PSSetConstantBuffers(0, 3, cbs);
  ID3D11SamplerState* smp[2] = {sampler_.Get(), sampler_clamp_.Get()};
  ctx_->PSSetSamplers(0, 2, smp);
  ctx_->RSSetState(raster_.Get());
}

void RendererD3D11::DrawMesh(const GpuModel& gm, const GpuMesh& me, bool blended) {
  DrawCB dcb;
  DrawTex dt;
  if (!MakeDrawCB(gm, me, blended, live_.srv != nullptr, dcb, dt)) return;
  D3D11_MAPPED_SUBRESOURCE m2;
  if (SUCCEEDED(ctx_->Map(cb_draw_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m2))) { memcpy(m2.pData, &dcb, sizeof dcb); ctx_->Unmap(cb_draw_.Get(), 0); }
  const D3DModel& d = DM(gm);
  auto tex = [&](int s) { return s >= 0 && size_t(s) < d.textures.size() ? d.textures[size_t(s)].Get() : nullptr; };
  ID3D11ShaderResourceView* srvs[4] = {dt.diffuse_live ? live_.srv.Get() : tex(dt.diffuse), tex(dt.lightmap), tex(dt.stage), tex(dt.normal)};
  ctx_->PSSetShaderResources(0, 4, srvs);
  ID3D11ShaderResourceView* dsrv[2] = {tex(dt.detail), tex(dt.detail_mask)};
  ctx_->PSSetShaderResources(8, 2, dsrv);
  ID3D11ShaderResourceView* bsrv = gm.has_bones && d.bones_srv ? d.bones_srv.Get() : ident_bones_srv_.Get();
  ctx_->VSSetShaderResources(0, 1, &bsrv);
  const GpuPlacement* pl = me.placement >= 0 ? &gm.placements[me.placement] : nullptr;
  if (pl && pl->count == 0) return;
  UINT strides[2] = {sizeof(assets::Vertex), sizeof(assets::Matrix)}, offs[2] = {0, 0};
  ID3D11Buffer* vbs[2] = {d.bufs[(size_t)me.buf].vb.Get(), pl ? d.placements[(size_t)me.placement].Get() : ident_inst_.Get()};
  ctx_->IASetVertexBuffers(0, 2, vbs, strides, offs);
  ctx_->IASetIndexBuffer(d.bufs[(size_t)me.buf].ib.Get(), DXGI_FORMAT_R32_UINT, 0);
  if (draw_inst_ >= 0 && pl) {  // Session 98 (step 16): the highlight mask draws one copy of a placed mesh
    if ((UINT)draw_inst_ < pl->count) ctx_->DrawIndexedInstanced(me.index_count, 1, 0, 0, (UINT)draw_inst_);
    return;
  }
  ctx_->DrawIndexedInstanced(me.index_count, pl ? pl->count : 1, 0, 0, 0);
}

void RendererD3D11::Scene(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, int w, int h, const Camera& cam) {
  ctx_->OMSetRenderTargets(1, &rtv, dsv);
  D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
  ctx_->RSSetViewports(1, &vp);
  // The clear colour is the picture's background AFTER the display curve: pre-invert it so the post chain's gamma step restores it.
  auto pre = [&](float v) {
    if (!(gamma_ && look_lit_)) return v;
    const float l = v < 0.081f ? v / 4.5f : std::pow((v + 0.099f) / 1.099f, 1.f / 0.45f);  // inverse BT.709
    return l <= 0.0031308f ? 12.92f * l : 1.055f * std::pow(l, 1.f / 2.4f) - 0.055f;       // sRGB encode (the curve decodes sRGB)
  };
  const float clear[4] = {pre(clear_rgb_[0]), pre(clear_rgb_[1]), pre(clear_rgb_[2]), 1.0f};  // (Session 98 importer: SetClearColour)
  ctx_->ClearRenderTargetView(rtv, clear);
  ctx_->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
  const FrameCB fcb = MakeFrameCB(cam, w, h);
  UploadFrameCB(fcb);
  BindCommon(vs_.Get(), ps_.Get());
  ID3D11ShaderResourceView* sh[4] = {shadow_.PropsSrv(), shadow_.WrestlerSrv(), shadow_.PlaneSrv(), shadow_.RimSrv()};
  ctx_->PSSetShaderResources(4, 4, sh);
  std::vector<DrawItem> items;
  BuildSceneList(cam, items);
  for (const DrawItem& it : items) {
    ctx_->OMSetDepthStencilState(it.depth_write ? ds_opaque_.Get() : ds_blend_.Get(), 0);
    ctx_->OMSetBlendState(it.blend == 2 ? bs_add_.Get() : it.blend == 1 ? bs_blend_.Get() : bs_opaque_.Get(), nullptr, 0xFFFFFFFF);
    ctx_->RSSetState(Raster(it.raster));
    DrawMesh(*it.gm, *it.me, it.blended);
  }
  ctx_->RSSetState(raster_.Get());
  ID3D11ShaderResourceView* nul[8] = {};
  ctx_->PSSetShaderResources(0, 8, nul);
}

// The shadow passes (shadow.cpp has the maps; the fit is the shared PrepareShadows; the casters are drawn here with the viewer's own vertex stage).
void RendererD3D11::ShadowPasses(const Camera& cam, int w, int h, GpuTimer* tm) {
  std::vector<float> foot_y;
  PrepareShadows(cam, w, h, foot_y);
  FrameCB fcb = MakeFrameCB(cam, w, h);
  const FrameCB main_fcb = fcb;
  shadow_.Upload(ctx_.Get(), shadow_set_.Cb());
  ID3D11ShaderResourceView* nul[8] = {};
  ctx_->PSSetShaderResources(0, 8, nul);
  ctx_->OMSetBlendState(bs_opaque_.Get(), nullptr, 0xFFFFFFFF);
  ctx_->OMSetDepthStencilState(ds_opaque_.Get(), 0);
  auto casters = [&](int kind) {  // 0 props (ring pieces), 1 wrestlers (opaque + cut-outs)
    for (auto& g : models_)
      for (auto& me : g.meshes)
        if (IsCaster(g.materials[me.material], me, kind)) DrawMesh(g, me, false);
  };
  if (shadow_set_.PropsActive()) {
    memcpy(fcb.viewProj.m, shadow_set_.PropsVP().m, sizeof fcb.viewProj.m);
    UploadFrameCB(fcb);
    BindCommon(vs_.Get(), ps_caster_.Get());
    shadow_.BeginProps(ctx_.Get());
    casters(0);
  }
  if (tm) tm->Mark(ctx_.Get(), "shadow_props");
  if (shadow_set_.WrestlerMapActive()) {
    memcpy(fcb.viewProj.m, shadow_set_.WrestlerVP().m, sizeof fcb.viewProj.m);
    UploadFrameCB(fcb);
    BindCommon(vs_.Get(), ps_caster_.Get());
    shadow_.BeginWrestler(ctx_.Get());
    casters(1);
  }
  if (tm) tm->Mark(ctx_.Get(), "shadow_wrestler");
  if (shadow_set_.PlaneActive()) {
    BindCommon(vs_plane_.Get(), ps_plane_.Get());
    ctx_->OMSetBlendState(shadow_.MaxBlend(), nullptr, 0xFFFFFFFF);
    ctx_->OMSetDepthStencilState(shadow_.NoDepth(), 0);
    shadow_.BeginPlane(ctx_.Get());
    for (size_t mi = 0; mi < models_.size(); mi++) {  // one plane per standing height: each model projects onto its own
      if (!models_[mi].has_wrestler) continue;
      cur_plane_y_ = foot_y[mi];
      for (auto& me : models_[mi].meshes)
        if (IsCaster(models_[mi].materials[me.material], me, 1)) DrawMesh(models_[mi], me, false);
    }
    cur_plane_y_ = 0.f;
    shadow_.BlurPlane(ctx_.Get());
  }
  if (tm) tm->Mark(ctx_.Get(), "shadow_plane");
  if (shadow_set_.RimActive()) {  // Session 98 (wrestler fix 2): the rim light map, from the main camera
    UploadFrameCB(main_fcb);
    BindCommon(vs_.Get(), ps_rim_.Get());
    ctx_->OMSetBlendState(bs_opaque_.Get(), nullptr, 0xFFFFFFFF);
    ctx_->OMSetDepthStencilState(ds_opaque_.Get(), 0);
    shadow_.BeginRim(ctx_.Get());
    casters(1);
    shadow_.BlurRim(ctx_.Get());
  }
  if (tm) tm->Mark(ctx_.Get(), "rim_map");
  ctx_->OMSetRenderTargets(0, nullptr, nullptr);
  ctx_->PSSetShaderResources(0, 8, nul);
}

void RendererD3D11::Frame(ID3D11RenderTargetView* out, int w, int h, const Camera& cam) {
  std::string err;
  if (!post_.Resize(w, h, err)) { fprintf(stderr, "%s\n", err.c_str()); return; }
  GpuTimer* tm = timing_on_ ? &timer_ : nullptr;
  if (tm) tm->Begin(ctx_.Get());
  ShadowPasses(cam, w, h, tm);
  Scene(post_.SceneRtv(), post_.SceneDsv(), w, h, cam);
  if (!sprites_.empty() && debug_ == 0) SpritePass(w, h, MakeFrameCB(cam, w, h));  // Session 98 (cap / particles): after the scene, before post
  if (tm) tm->Mark(ctx_.Get(), "scene");
  PollReadback();  // Session 98 (step 16): a finished 1x1 readback of an earlier ID pass
  {
    const bool hover_now = hover_req_ && sc_ && out == bb_rtv_.Get() && w == w_ && h == h_;
    if (hover_now || (timing_pick_ && tm)) {
      IdPass(cam, w, h, hover_now ? hover_x_ : -1, hover_now ? hover_y_ : -1);
      if (hover_now) QueueReadback(hover_x_, hover_y_);
      hover_req_ = false;  // (one-shot: the viewer asks again at its own pace)
      if (tm) tm->Mark(ctx_.Get(), "pick_id");
    }
  }
  // the live screen: keep this frame's pre-post scene colour for the next frame's big-screen draws
  if (live_screen_ && HasScreen()) {
    if (!live_.t || live_.w != w || live_.h != h) {
      if (MakeRT(dev_.Get(), live_, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT)) {
        const float black[4] = {0, 0, 0, 1};
        ctx_->ClearRenderTargetView(live_.rtv.Get(), black);
        live_has_ = false;
      }
    }
    if (live_.t) { ctx_->CopyResource(live_.t.Get(), post_.SceneTex()); live_has_ = true; }
  }
  const FrameCB fcb = MakeFrameCB(cam, w, h);
  post_.Run(ctx_.Get(), out, BuildPostPlan(MakePostParams(fcb), w, h), tm);
  if (FlashesWanted()) FlashPass(out, w, h, fcb);  // Session 98 (flash fix): over the finished picture
  if ((hl_hover_.valid() || hl_sel_.valid()) && (out == bb_rtv_.Get() || (timing_pick_ && tm))) HighlightPass(out, w, h, fcb, tm);  // Session 98 (step 16): outline + glow, before the UI
  if (tm) { tm->End(ctx_.Get()); timer_ok_ = tm->Collect(ctx_.Get()); }
  ctx_->ClearState();
}

// Session 98 (flash fix): the crowd camera flashes of entrance frame flash_frame_ (crowd_flash.h) over the finished picture `out`.
void RendererD3D11::FlashPass(ID3D11RenderTargetView* out, int w, int h, const FrameCB& fcb) {
  std::vector<FlashDraw> draws;
  BuildFlashes(fcb, draws);
  if (draws.empty() || !vs_flash_ || !ps_flash_) return;
  ID3D11ShaderResourceView* dsrv = post_.SceneDepthSrv();
  ctx_->OMSetRenderTargets(1, &out, nullptr);
  D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
  ctx_->RSSetViewports(1, &vp);
  ctx_->RSSetState(raster_.Get());
  ctx_->OMSetBlendState(bs_add_.Get(), nullptr, 0xFFFFFFFF);  // SrcAlpha + One (the game's flash blend)
  ctx_->OMSetDepthStencilState(nullptr, 0);
  ctx_->IASetInputLayout(layout_flash_.Get());
  ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx_->VSSetShader(vs_flash_.Get(), nullptr, 0);
  ctx_->PSSetShader(ps_flash_.Get(), nullptr, 0);
  ID3D11Buffer* cb = cb_flash_.Get();
  ctx_->VSSetConstantBuffers(0, 1, &cb);
  ID3D11SamplerState* sm[2] = {sampler_clamp_.Get(), sampler_point_.Get()};
  ctx_->PSSetSamplers(0, 2, sm);
  ctx_->VSSetSamplers(0, 2, sm);
  ctx_->VSSetShaderResources(1, 1, &dsrv);
  const GpuMesh* bound = nullptr;
  for (const FlashDraw& fd : draws) {
    if (fd.me != bound) {
      bound = fd.me;
      const D3DModel& d = DM(*fd.gm);
      const int ds = fd.gm->materials[fd.me->material].diffuse;
      ID3D11ShaderResourceView* tsrv = ds >= 0 ? d.textures[(size_t)ds].Get() : nullptr;
      ctx_->PSSetShaderResources(0, 1, &tsrv);
      UINT strides[2] = {sizeof(assets::Vertex), sizeof(assets::Matrix)}, offs[2] = {0, 0};
      ID3D11Buffer* vbs[2] = {d.bufs[(size_t)fd.me->buf].vb.Get(), ident_inst_.Get()};
      ctx_->IASetVertexBuffers(0, 2, vbs, strides, offs);
      ctx_->IASetIndexBuffer(d.bufs[(size_t)fd.me->buf].ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    }
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(ctx_->Map(cb_flash_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) { memcpy(ms.pData, &fd.cb, sizeof fd.cb); ctx_->Unmap(cb_flash_.Get(), 0); }
    ctx_->DrawIndexed(fd.me->index_count, 0, 0);
  }
  ID3D11ShaderResourceView* nul[2] = {};
  ctx_->VSSetShaderResources(0, 2, nul);
  ctx_->PSSetShaderResources(0, 2, nul);
}

void RendererD3D11::DrawFrame(const Camera& cam) {
  if (!sc_ || !bb_rtv_ || !depth_dsv_) return;
  Frame(bb_rtv_.Get(), w_, h_, cam);
  if (overlay_) {  // Session 98: the Arena Viewer UI, drawn over the finished frame (Frame() ends with ClearState)
    ID3D11RenderTargetView* rt = bb_rtv_.Get();
    ctx_->OMSetRenderTargets(1, &rt, nullptr);
    D3D11_VIEWPORT vp = {0, 0, (float)w_, (float)h_, 0, 1};
    ctx_->RSSetViewports(1, &vp);
    overlay_();
  }
  sc_->Present(1, 0);
}

bool RendererD3D11::GrabBackBuffer(std::vector<uint8_t>& bgra, int& w, int& h, std::string& err) {
  if (!sc_) { err = "no window"; return false; }
  ComPtr<ID3D11Texture2D> bb, stage;
  if (FAILED(sc_->GetBuffer(0, IID_PPV_ARGS(&bb)))) { err = "no back buffer"; return false; }
  D3D11_TEXTURE2D_DESC td;
  bb->GetDesc(&td);
  td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
  HRESULT hr = dev_->CreateTexture2D(&td, nullptr, &stage);
  if (FAILED(hr)) { err = HrStr("grab staging", hr); return false; }
  ctx_->CopyResource(stage.Get(), bb.Get());
  D3D11_MAPPED_SUBRESOURCE ms;
  hr = ctx_->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &ms);
  if (FAILED(hr)) { err = HrStr("grab map", hr); return false; }
  w = (int)td.Width; h = (int)td.Height;
  bgra.resize((size_t)w * h * 4);
  for (int y = 0; y < h; y++) memcpy(&bgra[(size_t)y * w * 4], (const uint8_t*)ms.pData + (size_t)y * ms.RowPitch, (size_t)w * 4);
  ctx_->Unmap(stage.Get(), 0);
  for (size_t i = 3; i < bgra.size(); i += 4) bgra[i] = 255;
  return true;
}

bool RendererD3D11::ShotPixels(const Camera& cam, int w, int h, std::vector<uint8_t>& px, std::string& err) {
  if (w < 16 || h < 16 || w > 8192 || h > 8192) { err = "shot size out of range (16..8192)"; return false; }
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_RENDER_TARGET;
  ComPtr<ID3D11Texture2D> rt, stage;
  ComPtr<ID3D11RenderTargetView> rtv;
  HRESULT hr = dev_->CreateTexture2D(&td, nullptr, &rt);
  if (SUCCEEDED(hr)) hr = dev_->CreateRenderTargetView(rt.Get(), nullptr, &rtv);
  D3D11_TEXTURE2D_DESC sd = td;
  sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  if (SUCCEEDED(hr)) hr = dev_->CreateTexture2D(&sd, nullptr, &stage);
  if (FAILED(hr)) { err = HrStr("shot targets", hr); return false; }
  if (live_screen_ && !entrance_ && HasScreen()) Frame(rtv.Get(), w, h, cam);  // the screen shows the previous frame: render once to seed it, so a still shows the screen's picture
  Frame(rtv.Get(), w, h, cam);
  ctx_->CopyResource(stage.Get(), rt.Get());
  D3D11_MAPPED_SUBRESOURCE ms;
  hr = ctx_->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &ms);
  if (FAILED(hr)) { ctx_->ClearState(); ctx_->Flush(); err = HrStr("Map staging", hr); return false; }
  px.resize((size_t)w * h * 4);
  for (int y = 0; y < h; y++) memcpy(&px[(size_t)y * w * 4], (const uint8_t*)ms.pData + (size_t)y * ms.RowPitch, (size_t)w * 4);
  ctx_->Unmap(stage.Get(), 0);
  ctx_->ClearState();  // (drop the references to this shot's targets; the scene pass rebinds everything)
  ctx_->Flush();
  for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
  return true;
}

// ---- timing ----------------------------------------------------------------------------------------------------------

std::string RendererD3D11::Timing(const Camera& cam, int w, int h, int frames, std::string& err, bool with_pick) {
  if (w < 16 || h < 16 || w > 8192 || h > 8192) { err = "size out of range (16..8192)"; return ""; }
  frames = std::max(2, std::min(frames, 200));
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_RENDER_TARGET;
  ComPtr<ID3D11Texture2D> rt;
  ComPtr<ID3D11RenderTargetView> rtv;
  HRESULT hr = dev_->CreateTexture2D(&td, nullptr, &rt);
  if (SUCCEEDED(hr)) hr = dev_->CreateRenderTargetView(rt.Get(), nullptr, &rtv);
  if (FAILED(hr)) { err = HrStr("timing target", hr); return ""; }
  timing_on_ = true;
  const PickHit save_hover = hl_hover_, save_sel = hl_sel_;  // Session 98 (step 16): "pick":true times the ID pass and a highlight too
  if (with_pick) {
    timing_pick_ = true;
    for (size_t mi = 0; mi < models_.size() && !hl_sel_.valid(); mi++)
      if (!models_[mi].meshes.empty()) hl_sel_ = PickHit{(int)mi, 0, 0};
  }
  std::vector<std::pair<std::string, double>> sum;
  double total = 0;
  int counted = 0;
  for (int f = 0; f < frames; f++) {
    Frame(rtv.Get(), w, h, cam);
    ctx_->Flush();
    if (f == 0) continue;  // (the first frame pays for the lazy GPU state)
    const auto& last = timer_.Last();
    if (!timer_ok_ || last.empty()) continue;  // (only frames whose timestamps were read count)
    if (sum.empty()) for (auto& p : last) sum.push_back({p.first, 0.0});
    for (size_t i = 0; i < last.size() && i < sum.size(); i++) sum[i].second += last[i].second;
    total += timer_.LastTotal();
    counted++;
  }
  timing_on_ = false;
  timing_pick_ = false;
  hl_hover_ = save_hover; hl_sel_ = save_sel;
  if (!counted) { err = "the GPU gave no timestamps"; return ""; }
  return TimingJson(w, h, counted, total, sum);
}

}  // namespace view
