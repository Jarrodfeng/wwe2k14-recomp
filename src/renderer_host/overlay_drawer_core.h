// S103 lane AIM (review fix 4): the ONE body of the app's ImGui immediate drawer, shared by both builds. It records ImGui's draw lists on the UI thread into an rdr::OverlayList and hands each finished
// list (and each texture) to a Sink: the stream build's LiveSink (src\renderer_adapter\overlay_drawer.h) or the stream-free build's static mailbox (overlay_off.h -> overlay_mailbox.h, drained by the
// scene host). A Sink provides: SetTexture(id, w, h, std::vector<uint8_t>&&), SetList(std::shared_ptr<const rdr::OverlayList>), ReleaseTexture(id). Sinks are cheap to copy (a reference / empty).
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>

#include "../renderer/overlay.h"

namespace wwe2k14::rdr_overlay_core {

template <class Sink>
class Texture final : public rex::ui::ImmediateTexture {
 public:
  Texture(uint32_t width, uint32_t height, uint32_t id, Sink sink) : rex::ui::ImmediateTexture(width, height), id(id), sink_(sink) {}
  ~Texture() override { sink_.ReleaseTexture(id); }   // the renderer's image of this id is freed (review fix 2)
  const uint32_t id;

 private:
  Sink sink_;
};

template <class Sink>
class BasicDrawer final : public rex::ui::ImmediateDrawer {
 public:
  explicit BasicDrawer(Sink sink = Sink()) : sink_(sink) {}

  std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(uint32_t width, uint32_t height, rex::ui::ImmediateTextureFilter, bool,
                                                           const uint8_t* data) override {
    const uint32_t id = next_id_++;
    std::vector<uint8_t> rgba(size_t(width) * height * 4, 0xFF);
    if (data) std::copy(data, data + rgba.size(), rgba.begin());
    sink_.SetTexture(id, width, height, std::move(rgba));
    return std::make_unique<Texture<Sink>>(width, height, id, sink_);
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
    out.texture = draw.texture ? static_cast<const Texture<Sink>*>(draw.texture)->id : 0;
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
    if (list_) sink_.SetList(std::move(list_));
    list_.reset();
    shown_.store(true, std::memory_order_release);
  }

  // No dialog open: the last list is taken off the screen (once).
  void Clear() {
    if (!shown_.load(std::memory_order_acquire)) return;
    sink_.SetList(nullptr);
    shown_.store(false, std::memory_order_release);
  }

  // S102 SW1: an overlay list is on the screen (written on the UI thread, read by RequestPaint on the game's swap thread).
  bool Shown() const { return shown_.load(std::memory_order_acquire); }

 private:
  Sink sink_;
  std::shared_ptr<rdr::OverlayList> list_;
  uint32_t next_id_ = 1;  // 0 is the renderer's plain white
  uint32_t vertex_base_ = 0, index_base_ = 0;
  bool indexed_ = false;
  std::atomic<bool> shown_{false};   // (S102 SW1: atomic, read by the swap thread)
};

// ImGuiDrawer::Draw wants a draw context; the app-driven one has no presenter.
class Context final : public rex::ui::UIDrawContext {
 public:
  Context(uint32_t width, uint32_t height) : rex::ui::UIDrawContext(width, height) {}
};

}  // namespace wwe2k14::rdr_overlay_core
