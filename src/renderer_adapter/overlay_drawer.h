// Settings parity S11 (Session 100): the app's Dear ImGui dialogs (the performance overlay, the CineCam panel, the check banner, the
// inspector, the upscale notice) drawn by the new renderer when it owns the window. With presentation detached the SDK asks the app for its
// own immediate drawer (SDK 88e4077, rex_app.cpp): this one records ImGui's draw lists on the UI thread into an rdr::OverlayList and hands
// each finished list to the live sink, whose render thread draws the newest one over every picture (renderer.cpp RecordOverlay). The UI
// thread paints once per game present (RequestPaint, from the reader's OnPresent): the SDK's ImGuiDrawer::Draw runs the dialogs there, as
// the SDK presenter's UI paint did.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>

#include "../renderer/overlay.h"
#include "live.h"

namespace wwe2k14::rdr_overlay {

class Texture final : public rex::ui::ImmediateTexture {
 public:
  Texture(uint32_t width, uint32_t height, uint32_t id) : rex::ui::ImmediateTexture(width, height), id(id) {}
  const uint32_t id;
};

class Drawer final : public rex::ui::ImmediateDrawer {
 public:
  explicit Drawer(rdr_adapter::LiveSink& sink) : sink_(sink) {}

  std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(uint32_t width, uint32_t height, rex::ui::ImmediateTextureFilter, bool,
                                                           const uint8_t* data) override {
    const uint32_t id = next_id_++;
    std::vector<uint8_t> rgba(size_t(width) * height * 4, 0xFF);
    if (data) std::copy(data, data + rgba.size(), rgba.begin());
    sink_.SetOverlayTexture(id, width, height, std::move(rgba));
    return std::make_unique<Texture>(width, height, id);
  }

  void Begin(rex::ui::UIDrawContext& context, float width, float height) override {
    rex::ui::ImmediateDrawer::Begin(context, width, height);
    list_ = std::make_shared<rdr::OverlayList>();
    list_->width = width;
    list_->height = height;
  }

  void BeginDrawBatch(const rex::ui::ImmediateDrawBatch& batch) override {
    if (!list_) return;
    vertex_base_ = uint32_t(list_->vertices.size());
    index_base_ = uint32_t(list_->indices.size());
    indexed_ = batch.indices != nullptr;
    static_assert(sizeof(rdr::OverlayVertex) == sizeof(rex::ui::ImmediateVertex), "the overlay vertex is the SDK's");
    const auto* vertices = reinterpret_cast<const rdr::OverlayVertex*>(batch.vertices);
    list_->vertices.insert(list_->vertices.end(), vertices, vertices + std::max(0, batch.vertex_count));
    if (indexed_) list_->indices.insert(list_->indices.end(), batch.indices, batch.indices + std::max(0, batch.index_count));
  }

  void Draw(const rex::ui::ImmediateDraw& draw) override {
    // Dear ImGui draws indexed triangles only; other primitives (the SDK's own debug lines) are not drawn here.
    if (!list_ || !indexed_ || draw.primitive_type != rex::ui::ImmediatePrimitiveType::kTriangles || draw.count <= 0) return;
    rdr::OverlayDraw out;
    out.texture = draw.texture ? static_cast<const Texture*>(draw.texture)->id : 0;
    out.first_index = index_base_ + uint32_t(draw.index_offset);
    out.count = uint32_t(draw.count);
    out.base_vertex = int32_t(vertex_base_) + draw.base_vertex;
    out.clipped = draw.scissor;
    out.clip[0] = draw.scissor_left, out.clip[1] = draw.scissor_top, out.clip[2] = draw.scissor_right, out.clip[3] = draw.scissor_bottom;
    list_->draws.push_back(out);
  }

  void EndDrawBatch() override {}

  void End() override {
    rex::ui::ImmediateDrawer::End();
    if (list_) sink_.SetOverlay(std::move(list_));
    list_.reset();
    shown_ = true;
  }

  // No dialog open: the last list is taken off the screen (once).
  void Clear() {
    if (!shown_) return;
    sink_.SetOverlay(nullptr);
    shown_ = false;
  }

 private:
  rdr_adapter::LiveSink& sink_;
  std::shared_ptr<rdr::OverlayList> list_;
  uint32_t next_id_ = 1;  // 0 is the renderer's plain white
  uint32_t vertex_base_ = 0, index_base_ = 0;
  bool indexed_ = false;
  bool shown_ = false;
};

// ImGuiDrawer::Draw wants a draw context; the app-driven one has no presenter.
class Context final : public rex::ui::UIDrawContext {
 public:
  Context(uint32_t width, uint32_t height) : rex::ui::UIDrawContext(width, height) {}
};

inline Drawer* g_drawer = nullptr;                // set by the app's OnCreateImmediateDrawer (owner mode)
inline std::atomic<bool> g_paint_pending{false};  // one UI paint queued at a time

// From the game's present (any thread): the UI thread runs the dialogs once and records them (`post` = the app context's CallInUIThread).
inline void RequestPaint(const std::function<void(std::function<void()>)>& post, rex::ui::Window* window, rex::ui::ImGuiDrawer* imgui) {
  if (!g_drawer || !window || !imgui || g_paint_pending.exchange(true)) return;
  post([window, imgui] {
    g_paint_pending.store(false);
    if (!imgui->HasDialogs()) {
      g_drawer->Clear();
      return;
    }
    Context context(window->GetActualPhysicalWidth(), window->GetActualPhysicalHeight());
    imgui->Draw(context);
  });
}

}  // namespace wwe2k14::rdr_overlay
