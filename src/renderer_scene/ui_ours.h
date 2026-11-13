// Our UI textures drawn in place inside the game's frame (Session 101, lane 66; docs\renderer\UI_OURS.md, the design: docs\renderer\UI_PROMOTION_MAP.md; the seam:
// src\renderer_adapter\draw_replace_hook.h). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// UiOurs is a DrawReplaceHook for the 2D sprite / glyph draws (op 19 at 0x82BC5688 / 0x82BC3784 / 0x82BC8BEC). The stream keeps the quad, colour, blend, order and
// target (ReplaceOut::keep_stream_geometry); this hook swaps in OUR upload of the picture from the player's own menu pac for slot 0. Which file picture a draw is, and
// every per-draw check, is UiCore (ui_ours_core.h); this class adds the renderer side: the index and the loads on workers (live) or on the calling thread (fview /
// rplay), the upload, the release of pictures nobody draws any more.
//
// No game or SDK includes. Threads: SetGameRoot from any thread; every other call on the render thread (the one that binds frames); the workers only read files, hash
// and upload (rdr::Renderer::CreateImage / UpdateImage are callable from any thread).
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <set>
#include <unordered_map>

#include "../renderer/renderer.h"
#include "../renderer_adapter/draw_replace_hook.h"
#include "record_io.h"   // lane 126: SceneTables (the arena of the record)
#include "ui_ours_core.h"
#include "ui_quads.h"   // lane 148

namespace rdr_scene {

struct UiOursOptions {
  std::string game_root;
  std::string cache_dir;                 // the hash cache of the index (offline: the check script's folder under out\; live: none)
  UiFaults faults;
  UiCore::ContentFn image_content;       // the stream image -> size / format / mips / hash supplier (ImageContentTable::Supplier)
  bool skip_bg_etc = false;              // lane 126 (review F1.4): --wwe2k14_low_ram: bgEtc (42 MB packed) is not indexed
  bool live = false;                     // true: the index and the loads run on workers and a draw waits (stays the stream's); false: on the calling thread
  bool quads = false;                    // lane 148 (UI_QUADS_OURS.md): the sprites and the T_Rect rectangles are drawn with our own quad and program (UiQuads); off: the stream's quads
  UiQuadFaults quad_faults;              // lane 148: the quads' planted faults
};

class UiOurs final : public rdr_adapter::DrawReplaceHook, private UiTextureProvider {
 public:
  UiOurs(rdr::Renderer& renderer, const UiOursOptions& options);
  ~UiOurs() override;
  UiOurs(const UiOurs&) = delete;
  UiOurs& operator=(const UiOurs&) = delete;

  void SetGameRoot(const std::string& root);
  void SetRecord(std::shared_ptr<const SceneTables> record);   // lane 126: the arena for the second tier

  // ---- DrawReplaceHook ----
  const char* Name() const override { return "ui"; }
  bool Claims(const rdr_adapter::DrawOrigin& origin) const override { return core_.Claims(origin) || (options_.quads && origin.op == 19 && origin.call_site == kUiQuadRectSite); }   // lane 148: the rectangles (no picture) when the quads are ours
  bool FrameReady(const rdr_adapter::LoadedFrame& frame) override;
  bool Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, rdr_adapter::KeepWhy& why) override;

  const UiCore& core() const { return core_; }
  const UiFileIndex& index() const { return index_; }
  std::string Report() const;
  std::string WindowDetail() const override;   // lane 156: the quads counters in the 600-frame window line (Report() is logged at the renderer stop only, after the case zip)

 private:
  struct Slot {
    int state = 0;                       // 1 loading, 2 ready, 3 failed
    UiTexture texture;
  };
  const UiTexture* Texture(uint32_t entry, const UiFileEntry& e, uint32_t mips, bool& pending) override;
  void StartIndex();
  void IndexAll();                       // the first tier of kUiPacs into index_ (the worker's body, or the calling thread's)
  bool IndexOne(uint8_t pac, uint64_t* entry_count);   // lane 126: one pac into the index; false: it could not be read (logged)
  void RequestDeep(uint8_t pac);         // lane 126: the second tier (bgEtc, the current arena's bgNN): queue one pac, once
  bool DeepStep();
  void DeepWorker();
  bool MaybeStartDeep(uint64_t unjoined_before);
  bool LoadAndUpload(const UiFileEntry& e, uint32_t mips, UiTexture& out, std::string& err);
  void LoadWorker();
  void Sweep();
  uint32_t Epoch() const override { return epoch_.load(); }

  rdr::Renderer& renderer_;
  UiOursOptions options_;
  UiFileIndex index_;                    // (declared before core_: core_ points at it)
  UiCore core_;
  std::unique_ptr<UiFileLoader> loader_owner_;
  std::unique_ptr<UiQuads> quads_;       // lane 148 (null unless options.quads)
  mutable std::mutex mu_;                // guards root_, slots_, queue_, index_error_ and the upload counters
  std::string root_;
  std::unordered_map<uint64_t, Slot> slots_;           // by (entry, mips uploaded): entry | mips << 32
  std::deque<uint64_t> queue_;
  std::condition_variable cv_;
  std::thread index_thread_, load_thread_, deep_thread_;   // (lane 126: deep_thread_)
  std::atomic<bool> stopping_{false}, index_started_{false}, index_done_{false};
  std::atomic<bool> deep_started_{false};   // lane 126
  std::atomic<int> arena_{-1};              // lane 126: the arena of the last scene record (ArenaCore::ArenaOfRecord), -1 unknown
  mutable std::mutex deep_mu_;                      // lane 126: guards the queue and the deep counters
  std::condition_variable deep_cv_;
  std::deque<uint8_t> deep_queue_;
  std::vector<int> record_arenas_;          // lane 126: the bgNN pacs the last scene record names (first = the arena), guarded by deep_mu_
  std::set<uint8_t> deep_requested_;
  unsigned deep_pending_ = 0, deep_walked_ = 0, deep_failed_ = 0;
  uint64_t deep_entries_ = 0;
  double deep_ms_ = 0;
  std::string index_error_;              // pacs the index could not read (mutex-guarded)
  uint64_t frames_ = 0, ours_frames_ = 0, not_ready_frames_ = 0;
  uint64_t uploads_ = 0, upload_bytes_ = 0, released_ = 0, load_failures_ = 0;
  std::atomic<double> index_ms_{0};
  std::atomic<uint32_t> epoch_{0};
};

}  // namespace rdr_scene
