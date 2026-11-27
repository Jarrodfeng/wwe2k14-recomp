
// ---- GPU pictures the CPU reads (S100, owner mode) ----------------------------------------------------------------------------------
// The game reads some pictures it made on the GPU with the CPU: the create modes' pictures that go into the save (Create a Superstar's
// render, Create an Arena's menu screenshot, Create a Title's export; Sessions 57 / 61) and the attire builds (packed on our GPU instead:
// OnAttirePacked). In owner mode the old path draws and reads nothing, so those bytes would be stale. The CPU reaches such a picture
// through the library's lock (0x82CA7EB8, reached by branch from LockRect 0x82CA8628 and the surface lock 0x82CA8AB0, so lr is the
// locker): when it locks a texture our renderer made (a copy destination whose content is newer than memory), the reader has the frame so
// far drawn (LiveSink::DrawAndRead), reads the level back and writes it into guest memory as the copy would have (WriteCopyBack), then the
// lock goes on. Nothing the CPU does not lock is read back: no windows, no rule about which copies matter. It replaces the same day's
// first bridge, which read back every copy inside the old path's read-back windows (docs\systems\readback_windows.md).
struct ReadbackCounts {
  uint64_t read = 0, not_read = 0, not_written = 0;
  double ms = 0;
};
inline ReadbackCounts g_readback_counts;
inline uint32_t g_last_present_interval = 1;  // OnPresent: what a frame part read back mid-frame carries in its header

// Writes `rgba` (the destination's mip as our renderer drew it: RGBA8 in the image's channel order, `rw` x `rh`) into guest memory over
// `rect` of the destination texture's `level` / `slice`: per texel the fetch channels a read of the copy gives, encoded as DecodeTexture
// decodes that format (its exact inverse; the endian swaps are their own inverses).
inline bool WriteCopyBack(const uint32_t w[6], bool swap, uint32_t level, uint32_t slice, const rdr::Rect& rect, const std::vector<uint8_t>& rgba, uint32_t rw,
                          uint32_t rh) {
  const TexShape s = ShapeOf(w);
  const uint32_t bf = BaseFormat(s.format);
  const Fmt f = FormatOf(s.format);
  if (bf != 6 && bf != 2 && bf != 15) {
    Note("readback_dest_format_other", bf);
    return false;
  }
  if (s.dim == 2) {
    Note("readback_dest_3d");
    return false;
  }
  // `w` are the texture object's header words: CPU virtual addresses. The GPU form (what DecodeTexture reads, RememberHeaderWords) adds the
  // 4 KB offset of the 0xE0000000 view (S100: the first version wrote 4 KB too early, one 32x32 tile of a 2048-wide atlas).
  const uint32_t w1 = wwe2k14::guest_view::ToPhysical(w[1]);
  const uint32_t w5 = wwe2k14::guest_view::ViewOffset(w[5]) + (w[5] & 0x1FFFFE00u);
  const uint32_t base_addr = w1 & 0xFFFFF000u, mip_addr = w5 & 0xFFFFF000u;
  const uint32_t endian = (w[1] >> 6) & 3;
  const uint32_t pitch_div32 = (w[0] >> 22) & 0x1FF;
  const bool packed = (w[5] >> 11) & 1;
  const uint32_t min_log2 = Log2Ceil(std::min(s.width, s.height));
  const uint32_t packed_level = packed ? (min_log2 > 4 ? min_log2 - 4 : 0) : 0xFFFFFFFFu;
  if (level > 0 && level >= packed_level) {
    Note("readback_dest_packed_tail");
    return false;
  }
  LevelLayout l;
  uint32_t region = 0;
  if (level == 0) {
    region = base_addr;
    GuestLevelLayout(f, s, pitch_div32, true, 0, s.tiled, l);
  } else {
    uint32_t offset = 0;
    for (uint32_t k = 1; k < level; ++k) {
      LevelLayout lk;
      GuestLevelLayout(f, s, pitch_div32, false, k, s.tiled, lk);
      offset += lk.slice_stride_bytes * s.layers;
    }
    region = mip_addr ? mip_addr + offset : 0;
    GuestLevelLayout(f, s, pitch_div32, false, level, s.tiled, l);
  }
  if (!region || rgba.size() < size_t(rw) * rh * 4 || !rw || !rh) {
    Note("readback_dest_no_address");
    return false;
  }
  region += slice * l.slice_stride_bytes;
  uint8_t* mem = REX_KERNEL_MEMORY()->TranslatePhysical<uint8_t*>(region);
  const int32_t lw = int32_t(std::max(s.width >> level, 1u)), lh = int32_t(std::max(s.height >> level, 1u));
  const int32_t x0 = std::max(rect.x, 0), y0 = std::max(rect.y, 0);
  const int32_t x1 = std::min(rect.x + int32_t(rect.width), lw), y1 = std::min(rect.y + int32_t(rect.height), lh);
  const uint32_t log2_bpb = Log2Floor(f.bpb);
  const auto q4 = [](uint8_t v) { return uint32_t((v * 15 + 127) / 255); };
  for (int32_t y = y0; y < y1; ++y) {
    const uint32_t sy = std::min(rh - 1, uint32_t(uint64_t(y) * rh / uint32_t(lh)));  // our image may be drawn larger (render scale)
    for (int32_t x = x0; x < x1; ++x) {
      const uint32_t sx = std::min(rw - 1, uint32_t(uint64_t(x) * rw / uint32_t(lw)));
      const uint8_t* p = &rgba[(size_t(sy) * rw + sx) * 4];
      uint8_t c[4] = {p[0], p[1], p[2], p[3]};
      if (swap) std::swap(c[0], c[2]);
      uint8_t out[4] = {};
      if (bf == 6) {  // T2: host RGBA8, the channels in order
        std::memcpy(out, c, 4);
      } else if (bf == 2) {  // T1: one byte, the first channel
        out[0] = c[0];
      } else {  // T3 (15): component k in nibble k (bits 4k..4k+3), the inverse of the load's rotate
        const uint16_t v = uint16_t(q4(c[0]) | (q4(c[1]) << 4) | (q4(c[2]) << 8) | (q4(c[3]) << 12));
        std::memcpy(out, &v, 2);
      }
      SwapBlock(out, f.bpb, endian);
      const uint32_t at = s.tiled ? TiledOffset2D(x, y, l.pitch_blocks, log2_bpb) : uint32_t(y) * l.row_pitch_bytes + uint32_t(x) * f.bpb;
      std::memcpy(mem + at, out, f.bpb);
    }
  }
  return true;
}

// 0x82CA7EB8 the library's texture lock (before it): r3 texture, r5 face / slice, r6 level, r9 flags; lr = the caller of LockRect / the
// surface lock (both reach it by a branch).
inline constexpr uint32_t kPackAtlasLockReturn = 0x83418024;  // sub_83417E20 locks an attire atlas: packed on our GPU instead
// Lane 232: the callers of the library's lock whose read-back is answered asynchronously (the lr at the lock entry; evidence: docs\systems\gpu_readback.md "Async read-back").
inline constexpr uint32_t kAsyncReadbackSites[] = {0x8274FADC};  // sub_8274FA70's lock of its 128x128 picture (runs 7102-7146: the only reader seen in tonight's runs)
inline bool IsAsyncReadbackSite(uint32_t lr) {
  for (uint32_t site : kAsyncReadbackSites)
    if (site == lr) return true;
  return false;
}
inline thread_local bool t_thumb_replay = false;  // lane 232: the deferred thumbnail job is being run for its delivered picture: its lock reads from memory, no read-back
inline void OnTextureLock(PPCContext& ctx, uint8_t* base) {
  if (t_thumb_replay) return;
  if (!REXCVAR_GET(wwe2k14_rdr_owner) || !REXCVAR_GET(wwe2k14_rdr_readback)) return;
  const uint32_t lr = uint32_t(ctx.lr), obj = ctx.r3.u32, layer = ctx.r5.u32, level = ctx.r6.u32;
  rdr_adapter::tail::The().FlushIfPending(rdr_adapter::tail::kAtLock);   // S102 U1 review F1: frame G (the copy this lock reads) is handed over before a part is drawn
  std::unique_lock lock = LockReaderCounted();
  if (!g_live.active()) return;
  const auto it = g_reg.tex_by_object.find(obj);
  if (it == g_reg.tex_by_object.end()) return;
  const uint32_t id = it->second;
  {
    const Image& img = ById(id);
    if (!img.copy_dest || img.packed || !img.gpu_newer) return;
  }
  rdr_adapter::copy_alias::NoteReadback(id);  // S101 lane 190 (S2): a destination the CPU reads back is never elided (copy_alias.h)
  if (lr == kPackAtlasLockReturn && REXCVAR_GET(wwe2k14_rdr_pack_on_gpu)) return;
  const uint64_t guest_frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  uint32_t w[6];
  for (uint32_t k = 0; k < 6; ++k) w[k] = db::Ld32(base, obj + 28 + 4 * k);
  if (ById(id).rec.format != uint8_t(rdr::Format::RGBA8) && ById(id).rec.format != uint8_t(rdr::Format::BGRA8)) {
    Note("lock_readback_storage_not_8bit", ById(id).rec.format);  // 16-bit targets and depth: no RGBA8 reading (ReadImage), not written
    ++g_readback_counts.not_written;
    ById(id).gpu_newer = false;
    return;
  }
  const bool swap = ById(id).swap;
  std::vector<uint8_t> rgba;
  uint32_t rw = 0, rh = 0;
  // Lane 232 (3d): a NAMED site's picture is answered from the render thread's newest finished copy, without drawing the frame so far or waiting. Anything else (a first lock, a copy
  // older than the bounds, an unnamed site) takes the blocking read below, which then teaches the mailbox the picture.
  const bool named_site = IsAsyncReadbackSite(lr);
  const bool async_on = named_site && REXCVAR_GET(wwe2k14_rdr_readback_async);
  const rdr_adapter::AsyncReadback::Key akey{id, level, layer};
  bool served_async = false;
  uint32_t async_lag = 0;
  if (async_on) {
    rdr_adapter::AsyncReadback& ar = g_live.Async();
    ar.SetParams({uint32_t(std::max(REXCVAR_GET(wwe2k14_rdr_readback_async_lag), 0)), uint32_t(std::max(REXCVAR_GET(wwe2k14_rdr_readback_async_age), 0)),
                  uint32_t(std::max(REXCVAR_GET(wwe2k14_rdr_readback_async_every), 1))});
    uint64_t served_frame = 0;
    uint32_t lag = 0;
    const auto r = ar.Take(akey, guest_frame, rgba, rw, rh, &served_frame, &lag);
    served_async = r == rdr_adapter::AsyncReadback::Result::Served;
    async_lag = lag;
    const auto c = ar.GetCounts();
    const uint64_t total = c.served + c.not_watched + c.nothing_yet + c.too_lag + c.too_old + c.busy_denied;
    if (total <= 32 || (total & (total - 1)) == 0)
      REXLOG_INFO("[rdr][readback] async lock {}: texture {:08X} locked from {:08X}, frame {}: {} (copy of frame {}, {} frames late; since start: served {}, first lock {}, nothing yet {}, too late {}, too old {}, too busy {}, published {} of {} frames, render thread {:.2f} ms in all)",
                  total, w[1] & 0xFFFFF000u, lr, guest_frame, served_async ? "served without waiting" : "falls back to the blocking read", served_async ? served_frame : 0, lag, c.served,
                  c.not_watched, c.nothing_yet, c.too_lag, c.too_old, c.busy_denied, c.published, guest_frame - std::min<uint64_t>(guest_frame, ar.FirstFrame()), c.read_ms);
  }
  // The frame so far, drawn now (the rest of the frame follows as a new frame: the per-frame caches start again).
  if (!served_async) LiveSendImages();
  const auto t0 = std::chrono::steady_clock::now();
  cap::LiveSink::ReadTimes times;
  const bool read = served_async || g_live.DrawAndRead(FrameBytes(guest_frame, g_last_present_interval, uint32_t(g_record_frame)), guest_frame, id, level, layer, rgba, rw, rh, 5000, &times);
  if (!served_async) {
    g_frame.Reset();
    ++g_record_frame;
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  g_readback_counts.ms += ms;
  if (named_site && !served_async && read && REXCVAR_GET(wwe2k14_rdr_readback_async)) g_live.Async().Watch(akey, guest_frame, rdr_adapter::SiteLagAllowance(lr, uint32_t(std::max(REXCVAR_GET(wwe2k14_rdr_readback_async_lag_thumb), 0)), 0), ById(id).copy_source);  // lane 232: this picture's next copies are read back by the render thread
  if (!read) {
    Note("lock_readback_not_read");
    ++g_readback_counts.not_read;
    return;
  }
  Image& img = ById(id);
  const uint32_t lw = std::max<uint32_t>(img.rec.width >> level, 1u), lh = std::max<uint32_t>(img.rec.height >> level, 1u);
  const bool written = WriteCopyBack(w, swap, level, layer, rdr::Rect{0, 0, lw, lh}, rgba, rw, rh);
  img.gpu_newer = served_async && async_lag > 0;  // SF1: a frame-late answer leaves memory behind the newest copy
  if (written && img.dest_watched) {  // S100: our own write-back is not the CPU's (WatchCopyDest)
    for (uint32_t r = 0; r < 2; ++r)
      if (img.dest_pages[r][0] <= img.dest_pages[r][1]) ClearWritten(img.dest_pages[r][0], img.dest_pages[r][1]);
  }
  if (!written) ++g_readback_counts.not_written;
  const uint64_t n = ++g_readback_counts.read;
  if (n <= 32 || (n & (n - 1)) == 0) {
    REXLOG_INFO("[rdr][readback] lock {}: texture {:08X} (format {}, {}x{}, level {} layer {}) read back for the CPU in {:.2f} ms, {}; locked from "
                "{:08X}, frame {} ({}; the picture was copied into {} times, {:.1f} per 100 frames since the first; since start: read {}, not read {}, not written {}, {:.0f} ms)",
                n, w[1] & 0xFFFFF000u, BaseFormat(w[1] & 0x3F), lw, lh, level, layer, ms, written ? "written to guest memory" : "NOT written", lr, guest_frame,
                served_async ? std::string("served by the async mailbox") : fmt::format("blocking: mailbox wait {:.2f} ms, part draw {:.2f} ms, ReadImage {:.2f} ms", times.wait_ms, times.draw_ms, times.read_ms),
                img.copy_count, double(img.copy_count) * 100.0 / double(std::max<uint64_t>(guest_frame - std::min<uint64_t>(guest_frame, img.copy_first), 1)),
                g_readback_counts.read, g_readback_counts.not_read, g_readback_counts.not_written, g_readback_counts.ms);
  }
  if (REXCVAR_GET(wwe2k14_rdr_debug_readback_png)) {
    std::vector<uint8_t> colour(rgba), alpha(rgba.size());
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
      colour[i + 3] = 255;
      alpha[i] = alpha[i + 1] = alpha[i + 2] = rgba[i + 3];
      alpha[i + 3] = 255;
    }
    std::error_code ec;
    const std::filesystem::path dir = CapturesRoot() / "live";
    std::filesystem::create_directories(dir, ec);
    char name[96];
    std::snprintf(name, sizeof(name), "readback_%04llu_%08X", static_cast<unsigned long long>(n), w[1] & 0xFFFFF000u);
    rdr::WritePng((dir / (std::string(name) + ".png")).string().c_str(), colour.data(), rw, rh);
    rdr::WritePng((dir / (std::string(name) + "_alpha.png")).string().c_str(), alpha.data(), rw, rh);
  }
}

// ---- lane 232: the deferred replay-thumbnail job (thumb_defer.h hooks sub_8274FA70 / sub_8274FC60 / sub_82750580 and calls these) ----
inline rdr_adapter::ThumbBoard g_thumb_board;  // under g_mutex
inline rdr_adapter::ThumbTimesBook g_thumb_times;  // lane 278: the per-job step timers (own mutex)
// Lane 278: the job's line, once: write_ms < 0 = the file writer never ran for it.
inline void ThumbTimesPrint(uint32_t obj, double write_ms) {
  rdr_adapter::ThumbTimes t;
  if (!g_thumb_times.Take(obj, &t)) return;
  REXLOG_INFO("[rdr][thumb] {}", rdr_adapter::FormatThumbTimes(t, write_ms));
}
inline uint64_t g_thumb_deferred = 0, g_thumb_delivered = 0, g_thumb_timed_out = 0, g_thumb_refused = 0, g_thumb_dropped = 0, g_thumb_before_write = 0;
inline thread_local bool t_thumb_nodefer = false;  // the job is being run by a hook that wants the answer now (a timeout, a Write that found it deferred): the entry hook does not defer it again

// A job to run on the game's thread after a delivery step. replay: the picture was written into the texture's memory, the job's lock reads it (no read-back); else the job runs the normal blocking read.
struct ThumbRun {
  uint32_t obj = 0;
  bool replay = false;
};

// The job's hook, at its entry (r3 = the object). true: the job is deferred, do not run it (its ready word obj+44 stays 4).
inline bool OnThumbJobEnter(PPCContext& ctx, uint8_t* base) {
  if (t_thumb_nodefer || !REXCVAR_GET(wwe2k14_rdr_readback_defer) || !REXCVAR_GET(wwe2k14_rdr_owner) || !REXCVAR_GET(wwe2k14_rdr_readback)) return false;
  const uint32_t obj = ctx.r3.u32;
  if (!obj || db::Ld32(base, obj + 44) != 4) return false;  // the job would do nothing: let it
  std::lock_guard lock(g_mutex);
  if (!g_live.active()) return false;
  const uint32_t texture = db::Ld32(base, obj + 52);
  const auto it = g_reg.tex_by_object.find(texture);
  if (it == g_reg.tex_by_object.end()) return false;
  const uint32_t id = it->second;
  const Image& img = ById(id);
  if (!img.copy_dest || img.packed || !img.gpu_newer) return false;  // memory is current: the job's own lock is cheap
  if (img.rec.format != uint8_t(rdr::Format::RGBA8) && img.rec.format != uint8_t(rdr::Format::BGRA8)) return false;
  const uint64_t guest_frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  if (!g_thumb_board.Add({obj, texture, id, guest_frame, ctx.r13.u64})) {
    ++g_thumb_refused;
    return false;
  }
  rdr_adapter::copy_alias::NoteReadback(id);  // parity with the lock path: a destination the CPU reads back is never elided (copy_alias.h)
  g_live.Async().Demand({id, 0, 0}, g_live.SubmittedCount(), guest_frame);
  const uint64_t n = ++g_thumb_deferred;
  g_thumb_times.Begin(obj, n);
  if (n <= 8 || (n & (n - 1)) == 0)
    REXLOG_INFO("[rdr][thumb] job {} deferred: object {:08X}, texture {:08X} (image {}), frame {}; the job returns at once with its ready word at 4", n, obj, texture, id, guest_frame);
  return true;
}

// B1: the object's destructor (sub_82750580) is about to free obj+84, release the texture and free the object without touching +44 / +52: its deferral goes first.
inline void OnThumbObjectDestroyed(uint32_t obj) {
  std::lock_guard lock(g_mutex);
  rdr_adapter::ThumbEntry e;
  if (!g_thumb_board.Remove(obj, &e)) return;
  g_live.Async().CancelDemand({e.image, 0, 0});
  ThumbTimesPrint(obj, -1.0);
  ++g_thumb_dropped;
  REXLOG_INFO("[rdr][thumb] object {:08X} destroyed while its job was deferred (frame {} -> {}): the deferral is dropped, nothing is delivered", obj, e.lock_frame,
              wwe2k14::g_guest_frame.load(std::memory_order_relaxed));
}

// Writes the landed picture into the texture's memory (as the blocking lock does) and, with --wwe2k14_rdr_debug_readback_png, dumps it. Under g_mutex.
inline bool ThumbWritePicture(uint8_t* base, const rdr_adapter::ThumbEntry& e, const std::vector<uint8_t>& rgba, uint32_t rw, uint32_t rh, uint64_t now) {
  uint32_t w[6];
  for (uint32_t k = 0; k < 6; ++k) w[k] = db::Ld32(base, e.texture + 28 + 4 * k);
  Image& img = ById(e.image);
  const uint32_t lw = std::max<uint32_t>(img.rec.width, 1u), lh = std::max<uint32_t>(img.rec.height, 1u);
  const auto tc = std::chrono::steady_clock::now();
  const bool written = WriteCopyBack(w, img.swap, 0, 0, rdr::Rect{0, 0, lw, lh}, rgba, rw, rh);
  const double convert_ms = rdr_adapter::MsSince(tc);
  g_thumb_times.Update(e.obj, [&](rdr_adapter::ThumbTimes& t) { t.convert_ms = convert_ms; });
  img.gpu_newer = false;
  if (written && img.dest_watched) {
    for (uint32_t k = 0; k < 2; ++k)
      if (img.dest_pages[k][0] <= img.dest_pages[k][1]) ClearWritten(img.dest_pages[k][0], img.dest_pages[k][1]);
  }
  const uint64_t n = ++g_thumb_delivered;
  if (n <= 8 || (n & (n - 1)) == 0)
    REXLOG_INFO("[rdr][thumb] picture {} delivered to object {:08X} {} guest frames after the job (texture memory {}); running the job now", n, e.obj, now - std::min(now, e.lock_frame),
                written ? "written" : "NOT written");
  if (REXCVAR_GET(wwe2k14_rdr_debug_readback_png)) {  // the check plan compares these with the blocking PNGs (OnTextureLock is skipped on a replay)
    std::vector<uint8_t> colour(rgba), alpha(rgba.size());
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
      colour[i + 3] = 255;
      alpha[i] = alpha[i + 1] = alpha[i + 2] = rgba[i + 3];
      alpha[i + 3] = 255;
    }
    std::error_code ec;
    const std::filesystem::path dir = CapturesRoot() / "live";
    std::filesystem::create_directories(dir, ec);
    char name[96];
    std::snprintf(name, sizeof(name), "readback_defer_%04llu_%08X", static_cast<unsigned long long>(n), w[1] & 0xFFFFF000u);
    rdr::WritePng((dir / (std::string(name) + ".png")).string().c_str(), colour.data(), rw, rh);
    rdr::WritePng((dir / (std::string(name) + "_alpha.png")).string().c_str(), alpha.data(), rw, rh);
  }
  return written;
}

// The guard for a ready entry: the object still waits and owns its buffer, the texture is unchanged and still registered to the same image.
inline bool ThumbGuard(uint8_t* base, const rdr_adapter::ThumbEntry& e) {
  const auto ld = [&](uint32_t a) { return db::Ld32(base, a); };
  const auto it = g_reg.tex_by_object.find(e.texture);
  const bool texture_live = it != g_reg.tex_by_object.end() && it->second == e.image && ById(e.image).live;
  if (rdr_adapter::DeliveryAllowed(e, ld, texture_live)) return true;
  g_live.Async().CancelDemand({e.image, 0, 0});
  ++g_thumb_dropped;
  REXLOG_INFO("[rdr][thumb] object {:08X} is not delivered to (ready word {}, texture {:08X} vs {:08X}, buffer {:08X}, texture {}): the deferral is dropped", e.obj, ld(e.obj + 44), ld(e.obj + 52),
              e.texture, ld(e.obj + 84), texture_live ? "live" : "gone");
  return false;
}

// Once a present on the deferral's thread, phase 1 (under g_mutex): the deferrals whose picture has landed are written into the texture's memory; the ones that waited too long are run by the normal blocking
// read. Returns the jobs to run (phase 2, unlocked: the job locks the texture through OnTextureLock, which takes g_mutex). Runs also while the sink is stopped (timeouts still drain).
inline void ThumbCollect(uint8_t* base, uint64_t thread, std::vector<ThumbRun>& run) {
  run.clear();
  std::lock_guard lock(g_mutex);
  if (!g_thumb_board.Pending()) return;
  const uint64_t now = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  std::unordered_map<uint32_t, std::vector<uint8_t>> pictures;  // by object
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> sizes;
  std::unordered_map<uint32_t, double> read_ms;  // lane 278
  std::vector<rdr_adapter::ThumbBoard::Ready> ready;
  g_thumb_board.Step(now, uint64_t(std::max(REXCVAR_GET(wwe2k14_rdr_readback_defer_timeout), 1)), thread, [&](const rdr_adapter::ThumbEntry& e) {
    if (!g_live.active()) return false;
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    const auto tr = std::chrono::steady_clock::now();
    if (!g_live.Async().TakeFresh({e.image, 0, 0}, rgba, w, h)) return false;
    read_ms[e.obj] = rdr_adapter::MsSince(tr);
    pictures[e.obj] = std::move(rgba);
    sizes[e.obj] = {w, h};
    return true;
  }, ready);
  for (const rdr_adapter::ThumbBoard::Ready& r : ready) {
    const rdr_adapter::ThumbEntry& e = r.entry;
    if (!ThumbGuard(base, e)) continue;
    g_thumb_times.Update(e.obj, [&](rdr_adapter::ThumbTimes& t) {
      t.wait_ms = rdr_adapter::MsSince(t.enter);
      t.replay = r.why == rdr_adapter::ThumbBoard::Why::Picture;
      t.read_ms = read_ms.count(e.obj) ? read_ms[e.obj] : 0.0;
    });
    if (r.why == rdr_adapter::ThumbBoard::Why::Picture) {
      ThumbWritePicture(base, e, pictures[e.obj], sizes[e.obj].first, sizes[e.obj].second, now);
      run.push_back({e.obj, true});
    } else {
      g_live.Async().CancelDemand({e.image, 0, 0});
      ++g_thumb_timed_out;
      REXLOG_INFO("[rdr][thumb] no picture for object {:08X} after {} guest frames: the job runs the normal blocking read (timed out {} so far)", e.obj, now - std::min(now, e.lock_frame), g_thumb_timed_out);
      run.push_back({e.obj, false});
    }
  }
}

// S2: the file writer (sub_8274FC60) is entered for an object whose job is deferred: it is taken off the board and its job runs first (the landed picture, else the blocking read), so Write finds +44 == 0.
inline void ThumbBeforeWrite(uint8_t* base, uint32_t obj, std::vector<ThumbRun>& run) {
  run.clear();
  std::lock_guard lock(g_mutex);
  rdr_adapter::ThumbEntry e;
  if (!g_thumb_board.Remove(obj, &e)) return;
  ++g_thumb_before_write;
  if (!ThumbGuard(base, e)) return;
  std::vector<uint8_t> rgba;
  uint32_t w = 0, h = 0;
  const auto tr = std::chrono::steady_clock::now();
  const bool got = g_live.active() && g_live.Async().TakeFresh({e.image, 0, 0}, rgba, w, h);
  const double rms = rdr_adapter::MsSince(tr);
  g_thumb_times.Update(obj, [&](rdr_adapter::ThumbTimes& t) {
    t.wait_ms = rdr_adapter::MsSince(t.enter);
    t.read_ms = rms;
    t.replay = got;
  });
  if (got) {
    ThumbWritePicture(base, e, rgba, w, h, wwe2k14::g_guest_frame.load(std::memory_order_relaxed));
    run.push_back({obj, true});
  } else {
    g_live.Async().CancelDemand({e.image, 0, 0});
    run.push_back({obj, false});
  }
  REXLOG_INFO("[rdr][thumb] the file writer reached object {:08X} while its job was deferred ({} so far): its job runs first ({})", obj, g_thumb_before_write, run.back().replay ? "landed picture" : "blocking read");
}

// 0x82CAFB88 Resolve (d3d_layer_copy.h; C1): r3 device, r4 flags, r5 rect, r6 dest texture, r7 point, r8 level, r9 slice, r10 clear colour, f1 depth,
// stack (r1 + 92) stencil, (r1 + 100) parameter block.
inline void OnResolve(PPCContext& ctx, uint8_t* base) {
  std::lock_guard lock(g_mutex);
  ++g_census.copies;
  const uint32_t dev = ctx.r3.u32, flags = ctx.r4.u32, rect_ptr = ctx.r5.u32, dt = ctx.r6.u32, point_ptr = ctx.r7.u32, level = ctx.r8.u32, slice = ctx.r9.u32, colour_ptr = ctx.r10.u32;
  if (db::Ld32(base, ctx.r1.u32 + 100)) Note("resolve_parameter_block");
  if (base[dev + 11068] & 0x31) Note("tiling_active");
  if (!dt) {
    Note("resolve_no_destination");
    return;
  }
  const uint32_t src = flags & 7;
  if (src > 4) {
    Note("resolve_source_unknown", src);
    return;
  }
  const bool from_depth = src == 4;
  const uint32_t surf = from_depth ? db::Ld32(base, dev + kDevDs) : db::Ld32(base, dev + kDevRt0 + 4 * src);
  if (!surf) {
    Note("resolve_source_unbound");
    return;
  }
  if (src != 0 && !from_depth) Note("resolve_source_not_colour0");
  const uint32_t source_id = SurfaceOfObject(base, surf, from_depth);
  if (from_depth && (g_wide.on() || g_jitter.on)) {  // settings parity S5 / S10b: the scene's depth copy ends the scene's draws (scene-size only, as the SDK)
    const Image& src_image = ById(source_id);
    if (src_image.rec.width >= 1000 && src_image.rec.height >= 500) g_wide.scene_done = g_jitter.scene_done = true;
  }
  uint32_t dest_id = TextureOfObject(base, dt);
  Image& dest = ById(dest_id);
  const Image& source = ById(source_id);
  if (TraceOn()) {
    uint32_t dw[6];
    for (uint32_t k = 0; k < 6; ++k) dw[k] = db::Ld32(base, dt + 28 + 4 * k);
    REXLOG_INFO("[rdr][trace] copy src {} image {} -> texture {:08X} image {} [{}] lr {:08X}", src, source_id, dt, dest_id, TraceWords(dw), uint32_t(ctx.lr));
  }

  // C4: same-size raw copies. The destination keeps the source's storage and is read through a swizzle (T1, T7, C8).
  const uint32_t w28 = db::Ld32(base, dt + 28), w32 = db::Ld32(base, dt + 32), w40 = db::Ld32(base, dt + 40), w48 = db::Ld32(base, dt + 48);
  const uint32_t dest_fmt = BaseFormat(w32 & 0x3F);
  if (w48 & 0x400 && ((w48 >> 9) & 3) == 2) Note("resolve_dest_3d");  // L20
  uint8_t storage = source.rec.format;
  if (from_depth) {
    if (dest_fmt != 22 && dest_fmt != 23) Note("resolve_depth_dest_format", dest_fmt);
    storage = uint8_t(rdr::Format::Depth);
  } else {
    const uint32_t sf = source.game_format;
    const bool ok8 = (sf == 0) && (dest_fmt == 2 || dest_fmt == 6 || dest_fmt == 15);
    const bool ok16 = (sf == 4 && dest_fmt == 25) || (sf == 5 && dest_fmt == 26);
    if (!ok8 && !ok16) Note("resolve_shape_outside_census");
  }
  if (dest.copy_dest && dest.rec.format != storage) Note("copy_dest_storage_changed");
  dest.copy_dest = true;
  dest.copy_source = source_id;  // S101 lane17: provenance source_image
  {  // S100 (L26 both ways): watch the destination's pages from this copy on (EnsureContent)
    uint32_t dw6[6];
    for (uint32_t k = 0; k < 6; ++k) dw6[k] = db::Ld32(base, dt + 28 + 4 * k);
    const uint32_t h5 = dw6[5];  // the slot form, as RememberHeaderWords makes it: words 1 and 5 physical
    dw6[1] = wwe2k14::guest_view::ToPhysical(dw6[1]);
    dw6[5] = wwe2k14::guest_view::ViewOffset(h5) + (h5 & 0x1FFFFE00u);
    WatchCopyDest(dest, dw6);
  }
  dest.swap = (w40 & 0xE) != 0;
  dest.target16 = !from_depth && (source.game_format == 4 || source.game_format == 5);
  g_images_dirty = true;
  dest.rec.format = storage;
  dest.rec.flags |= cap::kImageCopyDest;
  if (dest.swap) dest.rec.flags |= cap::kImageCopySwap;
  // S101 names: a copy of a surface whose declared role keeps the game's size never scales, whatever its shape (the sweat map, shadow and
  // atlas targets: the Cena-seams class); a copy of a scaling or undeclared source keeps S100's rule (downscale copies of the scene).
  // Sticky (review 4): once a declared non-scaling or lasting source copied into it, it stays so, so its record does not flip between
  // copies (a changed record is recreated and loses its contents).
  const bool source_declared = (source.rec.role_flags & cap::kRoleDeclared) != 0;
  const bool source_keeps_size = source_declared && !(source.rec.role_flags & cap::kRoleScales) && !g_scale_all;
  const bool source_lasting = source_declared && (source.rec.role_flags & cap::kRoleLasting);
  if (dest.copy_dest_seen && (source_keeps_size != dest.role_keeps_size || (source_lasting && !(dest.rec.usage & rdr::kUsageLasting))))
    Note("copy_dest_role_conflict");
  dest.role_keeps_size = dest.role_keeps_size || source_keeps_size;
  const bool copy_scales = dest.role_keeps_size ? false : ScreenShaped(dest.rec.width, dest.rec.height, "copy");
  const uint8_t lasting = uint8_t((source_lasting || (dest.copy_dest_seen && (dest.rec.usage & rdr::kUsageLasting))) ? rdr::kUsageLasting : 0);
  dest.copy_dest_seen = true;
  dest.rec.usage = uint8_t(rdr::kUsageSampled | (copy_scales ? rdr::kUsageScaled : 0) | lasting);
  dest.linear_static = false;
  if (Recording()) dest.written_in_capture = true;

  // Rectangles (C1-C2): the given one or the destination's size; right and bottom up to a multiple of 8; cut to both images.
  const uint32_t mip_w = std::max<uint32_t>(dest.rec.width >> level, 1u), mip_h = std::max<uint32_t>(dest.rec.height >> level, 1u);
  int32_t r[4] = {0, 0, int32_t(mip_w), int32_t(mip_h)};
  if (rect_ptr) {
    for (uint32_t i = 0; i < 4; ++i) r[i] = int32_t(db::Ld32(base, rect_ptr + 4 * i));
  }
  int32_t px = 0, py = 0;
  if (point_ptr) {
    px = int32_t(db::Ld32(base, point_ptr));
    py = int32_t(db::Ld32(base, point_ptr + 4));
  }
  int32_t right = (r[2] + 7) & ~7, bottom = (r[3] + 7) & ~7;
  const int32_t before_w = right - r[0], before_h = bottom - r[1];
  right = std::min(right, int32_t(source.rec.width));
  bottom = std::min(bottom, int32_t(source.rec.height));
  right = std::min(right, r[0] + int32_t(mip_w) - px);
  bottom = std::min(bottom, r[1] + int32_t(mip_h) - py);
  if (right - r[0] != before_w || bottom - r[1] != before_h) Note("copy_rect_cut_to_images");  // C2, C12
  if (right <= r[0] || bottom <= r[1]) {
    Note("copy_empty");
    return;
  }
  rdr::CopyOp op;
  op.source.image = rdr::ImageId(source_id);
  op.dest.image = rdr::ImageId(dest_id);
  op.dest.mip = uint8_t(level);
  op.dest.layer = uint8_t(slice);
  op.source_rect = {r[0], r[1], uint32_t(right - r[0]), uint32_t(bottom - r[1])};
  op.dest_rect = {px, py, uint32_t(right - r[0]), uint32_t(bottom - r[1])};
  g_frame.ops.push_back({cap::OpKind::Copy, uint32_t(g_frame.copies.size())});
  g_frame.copies.push_back(op);
  cap::CopyExtra extra;
  extra.target16 = dest.target16;
  extra.dest_sign = uint8_t((w28 >> 2) & 3);  // the channel the library's copy number format follows (listing 0x82CAEA30, d3d_layer_copy.h)
  extra.dest_format = uint8_t(dest_fmt);
  extra.swap = dest.swap;
  if (dest.target16) Note(extra.dest_sign == 1 ? "target16_copy_dest_signed" : "target16_copy_dest_unsigned");
  g_frame.copy_extras.push_back(extra);

  // Clear-on-copy (C25).
  if (flags & 0x300) {
    const int32_t source_width = int32_t(source.rec.width), source_height = int32_t(source.rec.height);  // EmitTargets can add images and move `source`
    EmitTargets(base, dev);
    rdr::ClearOp c;
    c.rect = op.source_rect;
    c.full = r[0] == 0 && r[1] == 0 && right == source_width && bottom == source_height;
    if (flags & 0x200) {
      c.flags |= rdr::kClearDepth | rdr::kClearStencil;
      c.depth = float(ctx.f1.f64);
      c.stencil = uint8_t(db::Ld32(base, ctx.r1.u32 + 92));
    }
    if (flags & 0x100) {
      c.flags |= rdr::kClearColor;
      c.color_mask = uint8_t(from_depth ? 1 : (1u << src));
      if (colour_ptr) {
        for (uint32_t i = 0; i < 4; ++i) c.color[i] = F(db::Ld32(base, colour_ptr + 4 * i));
      }
      if (c.color[0] != 0 || c.color[1] != 0 || c.color[2] != 0 || c.color[3] != 0) Note("resolve_clear_colour_nonzero");
    }
    Note("resolve_with_clear");
    g_frame.ops.push_back({cap::OpKind::Clear, uint32_t(g_frame.clears.size())});
    g_frame.clears.push_back(c);
  }
  ById(dest_id).gpu_newer = true;  // S100: memory is behind until the CPU locks it (OnTextureLock)
  {
    Image& di = ById(dest_id);
    // render-clock: lookup - diagnostic counter of the first copy frame, not game progress
    if (!di.copy_count++) di.copy_first = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  }
  if (g_live.active() && REXCVAR_GET(wwe2k14_rdr_readback_async)) g_live.Async().NoteCopy(dest_id, wwe2k14::g_guest_frame.load(std::memory_order_relaxed));  // lane 232: the lag's reference (a no-op for an image nobody watches)
}
