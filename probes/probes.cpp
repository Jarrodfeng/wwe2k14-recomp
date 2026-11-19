// wwe2k14 - the probe plugin (Session 30, #4): edit, rebuild (pwsh -File tools\probes\build_probes.ps1) and `plugin load` / `plugin reload` in the running
// coverage-build game. API: probes/probe_api.h.
//
// Current probe: Session 103 lane F3, "particle items II" (docs\formats\particle_items.md, "The run that settles 1-3 and 6"). Run it in Bryan's entrance on the coverage
// build; the same run's capture (case zip) gives the stream's blend and slot-0 image per particle draw. Every line starts "[wwe2k14][probe] pf <tag>". The S103 F probe
// ("pi" lines, run 8) is in git history.
//   pf item   sub_82A0D0E0 (sprites) / sub_82A0DAC8 (quads) / sub_82A0E3D8 (third): r3 = the item. Per sampled frame: the first 4 calls of EACH function. Logs the
//             class, the frame, a sequence number `seq` (calls of the three methods so far in this frame) and `rec_before` (op-19 particle recorder calls so far in this
//             frame): the draws of this item are the particle draws number rec_before .. rec_before + n - 1 of the frame in order, which pairs it with the capture's
//             particle draws (origin call sites 0x82A0DA58 / 0x82A0E338 / 0x82A0ED2C) of the same guest frame.
//   pf o48 / o0   the descriptor chain: o48 = [item+48] (0x200 bytes), o0 = [o48] (0x100 bytes): blend code at o0+196, pivot halfword at o0+226.
//   pf sel    the decoded selectors: idx = [o48+152]; blend = [o0+196]; pivot = halfword o0+226; the sampler byte at *[o48+28*idx+112]; the ZOffset int at *[o48+28*idx+108]
//             (and the 28-byte record at o48+28*idx+112-4 .. as words).
//   pf hdl    the two handle managers: handle ids at item+148 / item+156 and their cache words (+152 / +160), the global [0x8392E994] / [0x83929A98], the manager's
//             [+8] / [+16], the entry at the handle's index (id, ptr), and the count (end-begin)>>3.
//   pf tab    slot 33's table (64 bytes = 8 handles {id, cached pointer}) and the slot-33 pointer; for a quad also slot 29 (the first matrix, 64 bytes) and slot 23 (16 bytes).
//   pf size   sprites: slot 27's first record (16 bytes: w, texture index, angle, h) and slot 25 (colour ints); quads: slot 23's first record.
//   pf rec    sub_82C1B350 (the op-19 recorder) when the caller is 0x82A0DA5C / 0x82A0E33C / 0x82A0ED30: counted for the pairing; the first 2 per sampled frame are
//             logged with their lr and 40 bytes of the first vertex.
// Sampled frames: the first frame in which sub_82A0AED8 runs, and the 10th, 20th, 40th and 60th frame after it.
#include "probe_api.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace {

const Wwe2k14ProbeHost* g_host = nullptr;

constexpr uint32_t kEntry = 0x82A0AED8, kItemA = 0x82A0D0E0, kItemB = 0x82A0DAC8, kItemC = 0x82A0E3D8, kRec = 0x82C1B350;
constexpr uint32_t kMgr156 = 0x83929A98, kMgr148 = 0x8392E994;

std::mutex g_mutex;
uint64_t g_first_frame = 0;
bool g_have_first = false;
uint64_t g_last_frame = ~0ull;
int g_seq = 0, g_rec_total = 0, g_rec_logged = 0;
int g_per_fn[3] = {0, 0, 0};

void Log(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  g_host->log(buf);
}

uint32_t R32(uint32_t a) {
  uint32_t v = 0;
  g_host->read32(a, &v);
  return v;
}

void Words(const char* tag, uint32_t base, uint32_t bytes) {
  for (uint32_t off = 0; off < bytes; off += 32) {
    uint32_t w[8];
    for (int i = 0; i < 8; ++i) w[i] = R32(base + off + 4u * uint32_t(i));
    Log("pf %s +%04X: %08X %08X %08X %08X %08X %08X %08X %08X", tag, off, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
  }
}

// True when the current frame is a sampled one; resets the per-frame counters on a frame change. Caller holds g_mutex.
bool Sampled(uint64_t frame) {
  if (!g_have_first) return false;
  if (frame != g_last_frame) {
    g_last_frame = frame;
    g_seq = g_rec_total = g_rec_logged = 0;
    g_per_fn[0] = g_per_fn[1] = g_per_fn[2] = 0;
  }
  const uint64_t d = frame - g_first_frame;
  return d == 0 || d == 10 || d == 20 || d == 40 || d == 60;
}

void HandleManager(const char* which, uint32_t handle_addr, uint32_t global) {
  const uint32_t id = R32(handle_addr), cache = R32(handle_addr + 4), mgr = R32(global);
  const uint32_t begin = mgr ? R32(mgr + 8) : 0, end = mgr ? R32(mgr + 16) : 0;
  const uint32_t count = uint32_t(int32_t(end - begin) >> 3), idx = id & 0xFFFF;
  const uint32_t e0 = idx < count ? R32(begin + 8 * idx) : 0, e1 = idx < count ? R32(begin + 8 * idx + 4) : 0;
  Log("pf hdl %s id=%08X cache=%08X global=%08X mgr=%08X begin=%08X end=%08X count=%u idx=%u entry_id=%08X entry_ptr=%08X match=%d", which, id, cache, global, mgr, begin, end, count, idx, e0, e1,
      e0 == id && e1 != 0);
}

void OnEnter(void* ctx, uint32_t addr) {
  const uint64_t frame = g_host->frame();
  std::lock_guard<std::mutex> lock(g_mutex);
  if (addr == kEntry && !g_have_first) g_have_first = true, g_first_frame = frame;
  if (!Sampled(frame)) return;
  const uint32_t lr = g_host->get_lr(ctx);
  if (addr == kItemA || addr == kItemB || addr == kItemC) {
    const int which = addr == kItemA ? 0 : addr == kItemB ? 1 : 2;
    ++g_seq;
    if (g_per_fn[which]++ >= 4) return;
    const uint32_t item = uint32_t(g_host->get_gpr(ctx, 3));
    const uint32_t base = R32(item + 68);
    Log("pf item frame %llu fn=%08X class=%s item=%08X vtable=%08X count[+40]=%u base[+68]=%u seq=%d rec_before=%d", (unsigned long long)frame, addr,
        which == 0 ? "sprite" : which == 1 ? "quad" : "third", item, R32(item), R32(item + 40), base, g_seq, g_rec_total);
    const uint32_t o48 = R32(item + 48), o0 = o48 ? R32(o48) : 0;
    if (o48) Words("o48", o48, 0x200);
    if (o0) Words("o0", o0, 0x100);
    const uint32_t idx = o48 ? R32(o48 + 152) : 0;
    const uint32_t bptr = o48 ? R32(o48 + idx * 28 + 112) : 0, zptr = o48 ? R32(o48 + idx * 28 + 108) : 0;
    const uint32_t bword = bptr ? R32(bptr & ~3u) : 0;
    const uint32_t sbyte = (bword >> (8 * (3 - (bptr & 3u)))) & 0xFF;
    const uint32_t pw = o0 ? R32(o0 + 224) : 0;
    Log("pf sel idx=%u blend=%u pivot=%u sampler_ptr=%08X sampler_byte=%u z_ptr=%08X z_int=%d rec28=%08X %08X %08X %08X %08X %08X %08X", idx, o0 ? R32(o0 + 196) : 0, pw & 0xFFFF, bptr, sbyte,
        zptr, int32_t(zptr ? R32(zptr) : 0), R32(o48 + idx * 28 + 96), R32(o48 + idx * 28 + 100), R32(o48 + idx * 28 + 104), R32(o48 + idx * 28 + 108), R32(o48 + idx * 28 + 112),
        R32(o48 + idx * 28 + 116), R32(o48 + idx * 28 + 120));
    HandleManager("156->160", item + 156, kMgr156);
    HandleManager("148->152", item + 148, kMgr148);
    const uint32_t s33 = R32(item + 4u * (base + 33)), s27 = R32(item + 4u * (base + 27)), s25 = R32(item + 4u * (base + 25)), s23 = R32(item + 4u * (base + 23)),
                   s29 = R32(item + 4u * (base + 29));
    Log("pf tab slot33=%08X slot27=%08X slot25=%08X slot23=%08X slot29=%08X", s33, s27, s25, s23, s29);
    if (s33) Words("tab", s33, 64);
    if (which == 1) {
      if (s29) Words("quadmat", s29, 64);
      if (s23) Words("quadprev", s23, 16);
    } else {
      if (s27) Words("size", s27, 32);
    }
    if (s25) Words("col", s25, 32);
    return;
  }
  if (addr == kRec) {
    if (lr != 0x82A0DA5C && lr != 0x82A0E33C && lr != 0x82A0ED30) return;
    ++g_rec_total;
    if (g_rec_logged++ >= 2) return;
    const uint32_t verts = uint32_t(g_host->get_gpr(ctx, 7));
    Log("pf rec lr=%08X n_in_frame=%d prim=%u count=%u stride=%u verts=%08X", lr, g_rec_total, uint32_t(g_host->get_gpr(ctx, 4)), uint32_t(g_host->get_gpr(ctx, 5)),
        uint32_t(g_host->get_gpr(ctx, 6)), verts);
    Words("rec", verts, 40);
    return;
  }
}

}  // namespace

extern "C" {

__declspec(dllexport) int wwe2k14_probe_init(const Wwe2k14ProbeHost* host) {
  if (!host || host->version < WWE2K14_PROBE_API_VERSION) return 0;
  g_host = host;
  Log("pf loaded: sampling the first frame with a particle entry and +10 / +20 / +40 / +60 (S103 F3 particle items II)");
  return WWE2K14_PROBE_API_VERSION;
}

__declspec(dllexport) int wwe2k14_probe_functions(uint32_t* out, int cap) {
  const uint32_t fns[] = {kEntry, kItemA, kItemB, kItemC, kRec};
  int n = 0;
  for (uint32_t f : fns)
    if (n < cap) out[n++] = f;
  return n;
}

__declspec(dllexport) void wwe2k14_probe_enter(void* ctx, uint32_t addr) { OnEnter(ctx, addr); }

__declspec(dllexport) void wwe2k14_probe_leave(void* ctx, uint32_t addr) {
  (void)ctx;
  (void)addr;
}

}  // extern "C"
