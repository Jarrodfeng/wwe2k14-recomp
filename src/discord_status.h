// wwe2k14 - Discord status (Session 33 item 3). Default off.
//
// With --wwe2k14_discord=true and --wwe2k14_discord_app_id=ID (a Discord application the player or the project made
// on Discord's developer site; without an id nothing is sent), the game tells the Discord client running on this PC
// what is going on, over the client's local pipe (\\.\pipe\discord-ipc-0..9; Discord's documented RPC framing: u32
// opcode, u32 length, JSON; no Discord SDK, no network of our own):
//   in a match:  details "John Cena vs CM Punk", state "One on One - Hell in a Cell", the time since the match loaded;
//   otherwise:   details "In the menus".
// Nothing else is sent: only those names, the match type and the timer.
//
// Where the facts come from: the sequence id (states.toml seq: 9 = in game), the match description object M
// ([0x836F2D88]; M+8 match type, M+724 + 3580 i + 56 = participant i's character id, 500 = empty;
// docs\systems\match_setup.md), the wrestlers' names from the game's own string table (pac\string.pac: text id
// 60000 + character id, Session 32; DLC wrestlers fall back to "Superstar #ID") and the match type names from our id
// table (src\match_type_names.h, db\ids\match_types.json). Guest memory is read on the game's frame (VdSwap, every 30
// frames, readable-page checks first); a worker thread does the file read, the pipe and the logging.
#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "platform/compat.h"  // <windows.h> on Windows (Session 63)

#include "game_files.h"
#include "guest_read.h"
#include "match_type_names.h"
#include "native_zlib.h"  // stb_image's zlib decoder (stbi_zlib_decode_malloc_guesssize_headerflag)

REXCVAR_DEFINE_BOOL(wwe2k14_discord, false, "wwe2k14",
                    "Session 33: show what you are playing in your Discord status (the wrestlers, the match type and "
                    "the match time; \"In the menus\" otherwise), through the Discord app on this PC. Needs "
                    "--wwe2k14_discord_app_id. Default off");
REXCVAR_DEFINE_STRING(wwe2k14_discord_app_id, "", "wwe2k14",
                      "Session 33: the Discord application id the status is shown under (its name is what Discord "
                      "shows as the game). Empty = nothing is sent");

namespace wwe2k14::discord {

struct Snapshot {
  bool in_match = false;
  uint32_t type = 0;
  std::vector<uint32_t> ids;
  bool operator==(const Snapshot& o) const { return in_match == o.in_match && type == o.type && ids == o.ids; }
};

inline std::mutex g_mutex;
inline std::condition_variable g_cv;
inline Snapshot g_pending;
inline bool g_have_pending = false;
inline Snapshot g_last_posted;
inline bool g_started = false;

inline bool ReadBe32(uint8_t* base, uint32_t addr, uint32_t* out) {
  if (addr < 0x1000 || (addr & 3) || !GuestWordReadableFast(addr)) return false;
  uint32_t v;
  std::memcpy(&v, rex::memory::GuestPtr(base, addr), 4);
  *out = _byteswap_ulong(v);
  return true;
}

// ---- names (worker thread) ----
inline std::map<int, std::string> g_names;
inline bool g_names_loaded = false;

inline uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// The same reading as the launcher's DLC names (src\launcher\dlc_names.h): zlib streams, each a string table
// {u32 0, u32 count, count x (offset, length, text id), strings}; the first one with a name list.
inline void LoadNames() {
  g_names_loaded = true;
  std::vector<uint8_t> data(24u << 20);
  const size_t n = game_files::Read("game:\\pac\\string.pac", data);
  data.resize(n);
  for (size_t s = 0; s + 2 < data.size(); ++s) {
    if (data[s] != 0x78 || (data[s + 1] != 0x01 && data[s + 1] != 0x5E && data[s + 1] != 0x9C && data[s + 1] != 0xDA))
      continue;
    int out_len = 0;
    const int in_len = int(std::min<size_t>(data.size() - s, size_t(32) << 20));
    char* out = stbi_zlib_decode_malloc_guesssize_headerflag(reinterpret_cast<const char*>(&data[s]), in_len, 1 << 16,
                                                             &out_len, 1);
    if (!out) continue;
    std::map<int, std::string> names;
    const auto* o = reinterpret_cast<const uint8_t*>(out);
    const size_t len_all = size_t(out_len);
    if (len_all >= 8 && Le32(o) == 0) {
      const size_t count = Le32(o + 4);
      if (count < 200000 && 8 + 12 * count <= len_all) {
        for (size_t i = 0; i < count; ++i) {
          const uint8_t* r = o + 8 + 12 * i;
          const uint32_t off = Le32(r), len = Le32(r + 4), tid = Le32(r + 8);
          if (tid < 60100 || tid >= 61000 || off > len_all || len > len_all - off) continue;
          const char* str = reinterpret_cast<const char*>(o + off);
          names[int(tid - 60000)] = std::string(str, strnlen(str, len));
        }
      }
    }
    STBI_FREE(out);
    if (names.size() >= 50) {
      g_names = std::move(names);
      break;
    }
  }
  REXLOG_INFO("[wwe2k14][discord] {} wrestler names from the game's string table ({} bytes read)", g_names.size(), n);
}

// "JOHN CENA" or "John Cena": as the game writes it, but a name in capitals gets title case (Discord shows it small).
inline std::string NameOf(uint32_t id) {
  auto it = g_names.find(int(id));
  if (it == g_names.end() || it->second.empty()) return "Superstar #" + std::to_string(id);
  std::string s = it->second;
  bool lower_seen = false;
  for (char c : s) lower_seen |= (c >= 'a' && c <= 'z');
  if (!lower_seen) {
    bool start = true;
    for (char& c : s) {
      if (c >= 'A' && c <= 'Z' && !start) c = char(c - 'A' + 'a');
      start = !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '\'');
    }
  }
  return s;
}

inline std::string JsonEscape(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += char(c);
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
    else o += char(c);
  }
  return o;
}

// ---- the pipe (worker thread) ----
inline HANDLE g_pipe = INVALID_HANDLE_VALUE;
inline uint64_t g_nonce = 0;

#if !defined(_WIN32)
// Session 63 (Android port): no Discord app pipe on the phone; the activity is only logged.
inline bool WriteFrame(uint32_t, const std::string&) { return false; }
inline bool ReadFrame(uint32_t*, std::string*) { return false; }
inline void Disconnect() {}
inline bool Connect(const std::string&) { return false; }
#else

inline bool WriteFrame(uint32_t op, const std::string& json) {
  std::vector<uint8_t> buf(8 + json.size());
  const uint32_t len = uint32_t(json.size());
  std::memcpy(buf.data(), &op, 4), std::memcpy(buf.data() + 4, &len, 4);
  std::memcpy(buf.data() + 8, json.data(), json.size());
  DWORD written = 0;
  return WriteFile(g_pipe, buf.data(), DWORD(buf.size()), &written, nullptr) && written == buf.size();
}

// Reads one frame (Discord answers every command); false when the pipe broke.
inline bool ReadFrame(uint32_t* op, std::string* json) {
  uint8_t h[8];
  DWORD got = 0, total = 0;
  while (total < 8) {
    if (!ReadFile(g_pipe, h + total, 8 - total, &got, nullptr) || !got) return false;
    total += got;
  }
  uint32_t len;
  std::memcpy(op, h, 4), std::memcpy(&len, h + 4, 4);
  if (len > (1u << 20)) return false;
  json->assign(len, '\0');
  total = 0;
  while (total < len) {
    if (!ReadFile(g_pipe, json->data() + total, len - total, &got, nullptr) || !got) return false;
    total += got;
  }
  return true;
}

inline void Disconnect() {
  if (g_pipe != INVALID_HANDLE_VALUE) CloseHandle(g_pipe);
  g_pipe = INVALID_HANDLE_VALUE;
}

inline bool Connect(const std::string& app_id) {
  if (g_pipe != INVALID_HANDLE_VALUE) return true;
  for (int i = 0; i < 10; ++i) {
    const std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(i);
    HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) continue;
    g_pipe = h;
    uint32_t op = 0;
    std::string reply;
    if (WriteFrame(0, "{\"v\":1,\"client_id\":\"" + JsonEscape(app_id) + "\"}") && ReadFrame(&op, &reply) && op == 1) {
      REXLOG_INFO("[wwe2k14][discord] connected to the Discord app (discord-ipc-{})", i);
      return true;
    }
    REXLOG_WARN("[wwe2k14][discord] discord-ipc-{}: the handshake failed (op {}, '{}')", i, op, reply.substr(0, 200));
    Disconnect();
  }
  return false;
}
#endif  // _WIN32

inline void Worker() {
  std::string app_id;
  Snapshot snap;
  bool dirty = false;
  long long match_start = 0;
  auto next_retry = std::chrono::steady_clock::now();
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(g_mutex);
      g_cv.wait_for(lock, std::chrono::seconds(5), [] { return g_have_pending; });
      if (g_have_pending) {
        if (snap.in_match != g_pending.in_match && g_pending.in_match) match_start = (long long)std::time(nullptr);
        snap = g_pending, g_have_pending = false, dirty = true;
      }
    }
    if (!dirty) continue;
    if (!g_names_loaded && snap.in_match) LoadNames();
    std::string details, state;
    if (snap.in_match) {
      for (size_t i = 0; i < snap.ids.size() && i < 4; ++i) details += (i ? " vs " : "") + NameOf(snap.ids[i]);
      if (snap.ids.size() > 4) details = std::to_string(snap.ids.size()) + " superstars";
      const char* t = match_types::NameOf(snap.type);
      state = t ? t : "Match type " + std::to_string(snap.type);
    } else {
      details = "In the menus";
    }
    std::string activity = "{\"details\":\"" + JsonEscape(details) + "\"";
    if (!state.empty()) activity += ",\"state\":\"" + JsonEscape(state) + "\"";
    if (snap.in_match) activity += ",\"timestamps\":{\"start\":" + std::to_string(match_start) + "}";
    activity += "}";
    const std::string msg = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(GetCurrentProcessId()) +
                            ",\"activity\":" + activity + "},\"nonce\":\"" + std::to_string(++g_nonce) + "\"}";
    app_id = REXCVAR_GET(wwe2k14_discord_app_id);
    std::string result;
    if (app_id.empty()) {
      result = "not sent (no --wwe2k14_discord_app_id)";
      dirty = false;
    } else if (std::chrono::steady_clock::now() < next_retry && g_pipe == INVALID_HANDLE_VALUE) {
      continue;  // Discord was not there a moment ago: try again later (the change stays pending)
    } else if (!Connect(app_id)) {
      result = "not sent (the Discord app is not running; trying again in 15 s)";
      next_retry = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    } else {
      uint32_t op = 0;
      std::string reply;
      if (WriteFrame(1, msg) && ReadFrame(&op, &reply)) {
        const bool error = reply.find("\"evt\":\"ERROR\"") != std::string::npos;
        result = error ? "Discord answered an error: " + reply.substr(0, 300) : "sent";
        dirty = false;
      } else {
        result = "the pipe broke (Discord closed?): will reconnect";
        Disconnect();
      }
    }
    REXLOG_INFO("[wwe2k14][discord] activity: {} -> {}", activity, result);
  }
}

// The game's frame (VdSwap): every 30 frames, what is going on; a change wakes the worker.
inline void OnFrame(uint8_t* base, uint64_t frame) {
  if (!REXCVAR_GET(wwe2k14_discord) || frame % 30 != 0) return;
  if (!g_started) {
    g_started = true;
    if (REXCVAR_GET(wwe2k14_discord_app_id).empty())
      REXLOG_WARN("[wwe2k14][discord] --wwe2k14_discord is on but --wwe2k14_discord_app_id is empty: the activity is "
                  "logged, nothing is sent");
    std::thread(Worker).detach();
  }
  Snapshot s;
  uint32_t a = 0, b = 0, seq = 0;
  if (ReadBe32(base, 0x839400F8, &a) && ReadBe32(base, a, &b) && ReadBe32(base, b + 48, &seq)) s.in_match = seq == 9;
  uint32_t m = 0;
  if (s.in_match && ReadBe32(base, 0x836F2D88, &m) && m) {
    ReadBe32(base, m + 8, &s.type);
    for (uint32_t i = 0; i < 40; ++i) {
      uint32_t id = 0;
      if (!ReadBe32(base, m + 724 + 3580 * i + 56, &id)) break;
      if (id > 0 && id < 500) s.ids.push_back(id);
    }
  }
  static bool posted_once = false;  // the first state ("In the menus") is posted too
  if (posted_once && s == g_last_posted) return;
  posted_once = true;
  g_last_posted = s;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pending = s, g_have_pending = true;
  }
  g_cv.notify_one();
}

}  // namespace wwe2k14::discord
