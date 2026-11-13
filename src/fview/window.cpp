// Session 101 lane 2 (fview): the window. Dear ImGui over Win32 + Direct3D 11 draws the tool; the renderer runs headless on Vulkan and each picture is
// read back (Renderer::ReadImage) into a D3D11 texture. Panels: op list | surface after the selected op | pixel history and draw description.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include "../renderer/player/png.h"
#include "app.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace fview {
namespace {

namespace fs = std::filesystem;

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
bool g_running = true;
UINT g_resize_w = 0, g_resize_h = 0;

void CreateTarget() {
  ID3D11Texture2D* back = nullptr;
  g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
  if (back) {
    g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
  }
}
void DropTarget() {
  if (g_rtv) g_rtv->Release();
  g_rtv = nullptr;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
  switch (msg) {
    case WM_SIZE:
      if (wp != SIZE_MINIMIZED) g_resize_w = LOWORD(lp), g_resize_h = HIWORD(lp);
      return 0;
    case WM_SYSCOMMAND:
      if ((wp & 0xfff0) == SC_KEYMENU) return 0;
      break;
    case WM_CLOSE:
    case WM_DESTROY:
      g_running = false;
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

struct Picture {
  ID3D11Texture2D* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  uint32_t w = 0, h = 0;
  void Set(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) {
    if (!tex || width != w || height != h) {
      Free();
      D3D11_TEXTURE2D_DESC d{};
      d.Width = width, d.Height = height, d.MipLevels = 1, d.ArraySize = 1;
      d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      d.SampleDesc.Count = 1;
      d.Usage = D3D11_USAGE_DEFAULT;
      d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      if (FAILED(g_device->CreateTexture2D(&d, nullptr, &tex))) return;
      g_device->CreateShaderResourceView(tex, nullptr, &srv);
      w = width, h = height;
    }
    g_context->UpdateSubresource(tex, 0, nullptr, rgba.data(), width * 4, 0);
  }
  void Free() {
    if (srv) srv->Release();
    if (tex) tex->Release();
    srv = nullptr, tex = nullptr, w = h = 0;
  }
};

const char* FormatLabel(uint8_t f) {
  static const char* names[] = {"?", "RGBA8", "RGBA8s", "BGRA8", "R8", "R8s", "RG8", "RGBA4", "R5G6B5", "A1RGB5", "RGB10A2", "RGBA16s", "RGBA16u", "RG16u", "RG16s", "R16u",
                                "RGBA16F", "RG16F", "R16F", "RGBA32F", "RG32F", "R32F", "R32U", "Depth"};
  return f < std::size(names) ? names[f] : "fmt";
}

bool Readable(uint8_t f) { return f == uint8_t(rdr::Format::RGBA8) || f == uint8_t(rdr::Format::BGRA8) || f == uint8_t(rdr::Format::R8) || f == uint8_t(rdr::Format::RG16F) || f == uint8_t(rdr::Format::RGBA16F); }

struct App {
  Session& session;
  EffectIndex* effects;
  const Options& options;
  fs::path root;
  fs::path fx_dir;

  uint32_t frame = 0;
  int op = -1;
  uint32_t surface = 0;
  std::vector<uint32_t> surfaces;
  bool have_pixel = false;
  int px = 0, py = 0;
  Picture picture;
  bool picture_ok = false;
  DrawInfo info;
  bool have_info = false;
  std::vector<std::string> lines;
  float zoom = 0;  // 0: fit
  std::string status = "ready";
  bool status_bad = false;
  std::map<std::string, fs::file_time_type> mtimes;
  double next_poll = 0;
  int history_selected = -1;
  bool only_wrote = false;
  int through = -1;  // the history is taken as of this op (-1: all)
  bool scroll_to_op = false;

  App(Session& s, EffectIndex* e, const Options& o, const fs::path& r) : session(s), effects(e), options(o), root(r) {}

  void ListSurfaces() {
    surfaces.clear();
    for (const rdr_adapter::ImageRecord& r : session.capture().images())
      if (r.id && r.source == rdr_adapter::kSourceSurface && (r.usage & rdr::kUsageTarget) && Readable(r.format)) surfaces.push_back(r.id);
  }

  void ShowOp(int new_op) {
    op = new_op;
    scroll_to_op = true;
    session.RenderThrough(op);
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    picture_ok = surface && session.ReadSurface(surface, rgba, w, h);
    if (picture_ok) picture.Set(rgba, w, h);
    have_info = false;
    lines.clear();
    if (op >= 0 && uint32_t(op) < session.frame().ops.count && session.frame().ops[uint32_t(op)].kind == rdr_adapter::OpKind::Draw) {
      std::string error;
      have_info = DescribeDraw(session, effects, uint32_t(op), info, error);
      if (have_info) lines = DrawLines(info);
    }
  }

  void LoadFrame(uint32_t f) {
    std::string error;
    if (!session.SelectFrame(f, error)) {
      status = error, status_bad = true;
      return;
    }
    frame = f;
    have_pixel = false;
    history_selected = -1;
    if (!surface || std::find(surfaces.begin(), surfaces.end(), surface) == surfaces.end()) surface = session.DefaultSurface();
    ShowOp(int(session.frame().ops.count) - 1);
  }

  void Click(int x, int y) {
    px = x, py = y, have_pixel = true;
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<HistoryEntry>& h = session.PixelHistory(surface, x, y, through);
    history_selected = -1;
    int top = -1;
    for (size_t i = 0; i < h.size(); ++i)
      if (h[i].wrote && h[i].kind == "draw") {
        top = int(i);
        break;
      }
    status = "history: " + std::to_string(h.size()) + " ops touch (" + std::to_string(x) + "," + std::to_string(y) + "), " +
             std::to_string(int(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count())) + " ms";
    status_bad = false;
    if (top >= 0) {
      history_selected = top;
      ShowOp(int(h[size_t(top)].op));
    } else {
      ShowOp(op);
    }
  }

  void Reload(const std::string& effect) {
    status = "reloading " + effect + " ...";
    ReloadResult r;
    std::string error;
    if (effects) effects->Load(fx_dir, error);  // (the description may have changed too)
    const bool ok = ReloadEffect(session, effect, fx_dir, root, options.python, r);
    if (!ok) {
      status = "reload of " + effect + " FAILED: " + (r.failed.empty() ? "?" : r.failed[0].substr(0, 300));
      status_bad = true;
      return;
    }
    int changed = 0;
    for (const ReloadVariant& v : r.variants) changed += v.changed;
    char text[200];
    std::snprintf(text, sizeof(text), "reloaded %s: %zu variants, %d changed, %zu not in the compiled registry, %.1f s", effect.c_str(), r.variants.size(), changed, r.skipped.size(), r.seconds);
    status = text, status_bad = false;
    ShowOp(op);
    if (have_pixel) session.PixelHistory(surface, px, py, through);
  }

  void Poll(double now) {
    if (now < next_poll) return;
    next_poll = now + 0.5;
    std::error_code ec;
    std::set<std::string> changed_effects;  // every effect edited since the last poll (two saved in one half second are both rebuilt)
    bool shared = false;
    for (const auto& entry : fs::directory_iterator(fx_dir, ec)) {
      const std::string name = entry.path().filename().string();
      const std::string ext = entry.path().extension().string();
      if (ext != ".glsl" && ext != ".frag" && ext != ".vert" && ext != ".json") continue;
      const auto time = entry.last_write_time(ec);
      const auto found = mtimes.find(name);
      if (found == mtimes.end()) {
        mtimes[name] = time;
        continue;
      }
      if (found->second == time) continue;
      found->second = time;
      if (ext == ".glsl") shared = true;
      else changed_effects.insert(name.substr(0, name.find('.')));
    }
    if (shared && have_info && !info.effect.empty()) changed_effects.insert(info.effect);  // an include changed: the selected draw's effect
    for (const std::string& effect : changed_effects) Reload(effect);
  }

  void SaveCaseNow() {
    std::string error;
    const std::string dir = SaveCase(session, effects, root, surface, op, have_pixel, px, py, error);
    status = dir.empty() ? "save case FAILED: " + error : "case saved: " + dir;
    status_bad = dir.empty();
    ShowOp(op);  // (the save rendered through the same op; the picture is the same)
  }

  void Draw() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("fview", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    // Top bar.
    int f = int(frame);
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt("frame", &f, 1, 1) && f >= 0 && uint32_t(f) < session.frame_count() && uint32_t(f) != frame) LoadFrame(uint32_t(f));
    ImGui::SameLine();
    char label[96];
    const rdr_adapter::ImageRecord* cur = session.capture().FindImage(surface);
    std::snprintf(label, sizeof(label), "%u  %ux%u %s", surface, cur ? cur->width : 0, cur ? cur->height : 0, cur ? FormatLabel(cur->format) : "?");
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("surface", label)) {
      for (uint32_t id : surfaces) {
        const rdr_adapter::ImageRecord* r = session.capture().FindImage(id);
        std::snprintf(label, sizeof(label), "%u  %ux%u %s  %s", id, r->width, r->height, FormatLabel(r->format), SurfaceRole(session.capture(), id).c_str());
        if (ImGui::Selectable(label, id == surface)) {
          surface = id;
          have_pixel = false;
          ShowOp(op);
        }
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::InputInt("history thru op", &through, 0, 0);
    ImGui::SameLine();
    if (ImGui::Button("Save case")) SaveCaseNow();
    ImGui::SameLine();
    if (ImGui::Button("Fit")) zoom = 0;
    ImGui::SameLine();
    if (ImGui::Button("1:1")) zoom = 1;
    ImGui::SameLine();
    if (have_info && !info.effect.empty() && ImGui::Button("Reload effect")) Reload(info.effect);
    ImGui::SameLine();
    if (session.capture().scene_record_count())  // S101 lane 26: the capture folder's scene records (the judge's scene arm will use them)
      ImGui::TextDisabled("scene records: %u of %u frames", session.capture().scene_record_count(), session.frame_count());
    else
      ImGui::TextDisabled("no scene records");
    ImGui::SameLine();
    ImGui::TextColored(status_bad ? ImVec4(1, 0.4f, 0.4f, 1) : ImVec4(0.7f, 0.9f, 0.7f, 1), "%s", status.c_str());
    ImGui::Separator();

    const float h = ImGui::GetContentRegionAvail().y;
    const float list_w = ImGui::GetContentRegionAvail().x * 0.22f, right_w = ImGui::GetContentRegionAvail().x * 0.38f;
    // Op list.
    ImGui::BeginChild("ops", ImVec2(list_w, h), ImGuiChildFlags_Borders);
    const rdr_adapter::LoadedFrame& fr = session.frame();
    ImGuiListClipper clip;
    clip.Begin(int(fr.ops.count));
    if (scroll_to_op && op >= 0) {
      ImGui::SetScrollY(std::max(0.0f, float(op) * ImGui::GetTextLineHeightWithSpacing() - ImGui::GetWindowHeight() * 0.5f));
      scroll_to_op = false;
    }
    int clicked = -1;
    while (clip.Step()) {
      for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
        const rdr_adapter::Op& o = fr.ops[uint32_t(i)];
        char text[200];
        if (o.kind == rdr_adapter::OpKind::Draw) {
          const OpTrace& t = session.trace()[size_t(i)];
          const char* ps = t.ps >= 0 ? session.VariantName(t.ps) : t.vs >= 0 ? session.VariantName(t.vs) : "(no shader)";
          std::snprintf(text, sizeof(text), "%5d draw %s%s", i, ps, OpDrawn(t) ? "" : "  [not drawn]");
        } else {
          std::snprintf(text, sizeof(text), "%5d %s", i, OpKindName(o.kind));
        }
        if (ImGui::Selectable(text, i == op)) clicked = i;
      }
    }
    ImGui::EndChild();
    if (clicked >= 0) ShowOp(clicked);
    ImGui::SameLine();

    // Surface after the selected op.
    ImGui::BeginChild("view", ImVec2(ImGui::GetContentRegionAvail().x - right_w - ImGui::GetStyle().ItemSpacing.x, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    if (picture_ok && picture.srv) {
      const ImVec2 avail = ImGui::GetContentRegionAvail();
      float z = zoom;
      if (z <= 0) z = std::min(avail.x / float(picture.w), avail.y / float(picture.h));
      const ImVec2 size(float(picture.w) * z, float(picture.h) * z);
      ImGui::Text("surface %u after op %d: %ux%u  zoom %.2f", surface, op, picture.w, picture.h, double(z));
      const ImVec2 origin = ImGui::GetCursorScreenPos();
      ImGui::Image((ImTextureID)(uintptr_t)picture.srv, size);
      if (ImGui::IsItemHovered()) {
        const ImVec2 m = ImGui::GetMousePos();
        const int x = int((m.x - origin.x) / z), y = int((m.y - origin.y) / z);
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0 && ImGui::GetIO().KeyCtrl) zoom = std::max(0.1f, z * (wheel > 0 ? 1.25f : 0.8f));
        ImGui::SetTooltip("%d, %d", x, y);
        if (ImGui::IsMouseClicked(0)) Click(x, y);
      }
      if (have_pixel) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c(origin.x + (float(px) + 0.5f) * z, origin.y + (float(py) + 0.5f) * z);
        dl->AddCircle(c, 8, IM_COL32(255, 255, 0, 255), 24, 2.0f);
        dl->AddLine(ImVec2(c.x - 14, c.y), ImVec2(c.x + 14, c.y), IM_COL32(255, 255, 0, 255));
        dl->AddLine(ImVec2(c.x, c.y - 14), ImVec2(c.x, c.y + 14), IM_COL32(255, 255, 0, 255));
      }
    } else {
      ImGui::TextWrapped("surface %u is not readable (format %s) or not bound in this frame", surface, cur ? FormatLabel(cur->format) : "?");
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // History and draw.
    ImGui::BeginChild("right", ImVec2(0, h), ImGuiChildFlags_Borders);
    if (ImGui::BeginTabBar("tabs")) {
      if (ImGui::BeginTabItem("Pixel history")) {
        if (!have_pixel) {
          ImGui::TextWrapped("Click a pixel in the picture: the draws, clears and copies that touch it are listed here, newest first.");
        } else {
          const std::vector<HistoryEntry>& hist = session.PixelHistory(surface, px, py, through);
          ImGui::Text("pixel %d,%d on surface %u: %zu ops", px, py, surface, hist.size());
          ImGui::SameLine();
          ImGui::Checkbox("only draws that wrote", &only_wrote);
          ImGui::BeginChild("hist_rows");
          for (size_t i = 0; i < hist.size(); ++i) {
            const HistoryEntry& e = hist[i];
            if (only_wrote && !e.wrote) continue;
            std::string eff;
            if (e.kind == "draw") {
              const OpTrace& t = session.trace()[e.op];
              eff = EffectOf(effects, t.ps >= 0 ? session.VariantName(t.ps) : t.vs >= 0 ? session.VariantName(t.vs) : "");
            }
            char text[320];
            if (e.value_known)
              std::snprintf(text, sizeof(text), "op %u %s %s  %s  (%u,%u,%u,%u) -> (%u,%u,%u,%u)", e.op, e.kind.c_str(), eff.c_str(), e.wrote ? "WROTE" : "unchanged", e.before[0], e.before[1], e.before[2],
                            e.before[3], e.after[0], e.after[1], e.after[2], e.after[3]);
            else
              std::snprintf(text, sizeof(text), "op %u %s %s  %s", e.op, e.kind.c_str(), eff.c_str(), e.note.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, e.wrote ? ImVec4(0.6f, 1, 0.6f, 1) : e.drawn ? ImVec4(0.75f, 0.75f, 0.75f, 1) : ImVec4(1, 0.6f, 0.5f, 1));
            if (ImGui::Selectable(text, int(i) == history_selected)) {
              history_selected = int(i);
              ShowOp(int(e.op));
            }
            ImGui::PopStyleColor();
          }
          ImGui::EndChild();
        }
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Draw")) {
        if (!have_info) ImGui::TextWrapped("Select a draw (op list or history).");
        for (const std::string& l : lines) {
          const bool bad = l.find("MISMATCH") != std::string::npos || l.find("program OFF") != std::string::npos || l.find("STALE TABLE") != std::string::npos;
          if (bad) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.45f, 0.4f, 1));
          ImGui::TextWrapped("%s", l.c_str());
          if (bad) ImGui::PopStyleColor();
        }
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    ImGui::EndChild();

    // Keys: Up / Down step the selected op while nothing is being typed.
    if (!ImGui::GetIO().WantTextInput) {
      if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && op + 1 < int(fr.ops.count)) ShowOp(op + 1);
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && op > 0) ShowOp(op - 1);
      if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) ShowOp(std::min(int(fr.ops.count) - 1, op + 20));
      if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) ShowOp(std::max(0, op - 20));
    }
    ImGui::End();
  }
};

bool GrabBackBuffer(const std::string& path) {
  ID3D11Texture2D* back = nullptr;
  g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
  if (!back) return false;
  D3D11_TEXTURE2D_DESC d;
  back->GetDesc(&d);
  D3D11_TEXTURE2D_DESC sd = d;
  sd.Usage = D3D11_USAGE_STAGING, sd.BindFlags = 0, sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ, sd.MiscFlags = 0;
  ID3D11Texture2D* staging = nullptr;
  bool ok = false;
  if (SUCCEEDED(g_device->CreateTexture2D(&sd, nullptr, &staging))) {
    g_context->CopyResource(staging, back);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
      std::vector<uint8_t> rgba(size_t(d.Width) * d.Height * 4);
      for (UINT y = 0; y < d.Height; ++y) {
        const uint8_t* src = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
        uint8_t* dst = rgba.data() + size_t(y) * d.Width * 4;
        for (UINT x = 0; x < d.Width; ++x) {
          const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
          dst[x * 4 + 0] = src[x * 4 + (bgra ? 2 : 0)];
          dst[x * 4 + 1] = src[x * 4 + 1];
          dst[x * 4 + 2] = src[x * 4 + (bgra ? 0 : 2)];
          dst[x * 4 + 3] = 255;
        }
      }
      std::error_code ec;
      fs::create_directories(fs::path(path).parent_path(), ec);
      ok = rdr::WritePng(path.c_str(), rgba.data(), d.Width, d.Height);
      g_context->Unmap(staging, 0);
    }
    staging->Release();
  }
  back->Release();
  return ok;
}

}  // namespace

int RunWindow(Session& session, EffectIndex* effects, const Options& options, const fs::path& root) {
  // S101 2026-11-12 (Jarrod: "definitely wanna keep those hidden"): an agent (Claude Code sets CLAUDECODE=1 in its shells)
  // never gets the interactive window - it would sit on Jarrod's screen and never exit (lane 149's did). Agents use the
  // headless flags; --screenshot still renders, into a window that is never shown.
  const bool agent = std::getenv("CLAUDECODE") != nullptr;
  if (agent && options.screenshot.empty()) {
    std::fprintf(stderr, "[fview] window mode refused: run by an agent (CLAUDECODE is set). Use the headless flags "
                         "(--ops --json, --pixel, --image-png, --validation, --frames, --save-case) or --screenshot.\n");
    return 2;
  }
  const bool hidden = agent || !options.screenshot.empty();
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WindowProc, 0, 0, instance, nullptr, nullptr, nullptr, nullptr, L"fview", nullptr};
  RegisterClassExW(&wc);
  HWND hwnd = CreateWindowW(wc.lpszClassName, L"fview - frame viewer", WS_OVERLAPPEDWINDOW, 60, 40, 1760, 1000, nullptr, nullptr, instance, nullptr);
  if (!hwnd) {
    std::fprintf(stderr, "[fview] could not open a window\n");
    return 2;
  }
  DXGI_SWAP_CHAIN_DESC sd{};
  sd.BufferCount = 2;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hwnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
  D3D_FEATURE_LEVEL got;
  if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 1, D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got, &g_context))) {
    std::fprintf(stderr, "[fview] could not start Direct3D 11\n");
    return 2;
  }
  CreateTarget();
  if (!hidden) {  // S101: a hidden window still renders its back buffer (GrabBackBuffer reads it before Present)
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  ImGui::StyleColorsDark();
  const float dpi = std::max(1.0f, float(GetDpiForWindow(hwnd)) / 96.0f);
  ImGui::GetStyle().ScaleAllSizes(dpi);
  ImGui::GetStyle().FontScaleMain = dpi;
  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX11_Init(g_device, g_context);

  App app(session, effects, options, root);
  app.fx_dir = effects ? effects->dir() : root / "src" / "renderer" / "shaders" / "fx";
  app.ListSurfaces();
  app.surface = options.surface;
  app.through = options.through_op;
  app.LoadFrame(session.frame_index());
  if (options.have_pixel) app.Click(options.px, options.py);
  if (options.op >= 0) app.ShowOp(options.op);

  int frames_drawn = 0;
  const auto start = std::chrono::steady_clock::now();
  while (g_running) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (msg.message == WM_QUIT) g_running = false;
    }
    if (!g_running) break;
    if (g_resize_w && g_resize_h) {
      DropTarget();
      g_swap->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
      g_resize_w = g_resize_h = 0;
      CreateTarget();
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    app.Poll(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    app.Draw();
    ImGui::Render();
    const float clear[4] = {0.08f, 0.08f, 0.09f, 1};
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_context->ClearRenderTargetView(g_rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    ++frames_drawn;
    if (!options.screenshot.empty() && frames_drawn == 6) {
      const bool ok = GrabBackBuffer(options.screenshot);
      std::printf("[fview] window screenshot %s: %s\n", options.screenshot.c_str(), ok ? "written" : "FAILED");
      g_running = false;
    }
    g_swap->Present(1, 0);
  }
  app.picture.Free();
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  DropTarget();
  if (g_swap) g_swap->Release();
  if (g_context) g_context->Release();
  if (g_device) g_device->Release();
  DestroyWindow(hwnd);
  UnregisterClassW(wc.lpszClassName, instance);
  return 0;
}

}  // namespace fview
