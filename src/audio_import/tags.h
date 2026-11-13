// wwe2k14 - lane 283: song metadata + embedded cover art, read from the source file's own tags (header-only, portable, no
// dependencies): ID3v2 (mp3: TIT2 / TPE1 / TALB, APIC), MP4 / M4A (moov.udta.meta.ilst: (c)nam / (c)ART / (c)alb, covr), FLAC
// (VORBIS_COMMENT, PICTURE), Ogg Vorbis / Opus (Vorbis comment: TITLE / ARTIST / ALBUM, METADATA_BLOCK_PICTURE = base64 FLAC
// picture), WMA / ASF (content description, WM/Picture). Everything is bounds-checked: a damaged tag yields what was read so
// far, never a crash. The picture comes back as the raw JPEG / PNG bytes; cover.h turns it into the thumbnail.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace audio_import {

struct Tags {
  std::string title, artist, album;  // UTF-8, control characters removed
  std::vector<uint8_t> picture;      // the embedded image file (JPEG / PNG / ...), empty = none
  int picture_type = -1;             // ID3 picture type of `picture` (3 = front cover), -1 = unknown
};

namespace tags_detail {

using Bytes = std::vector<uint8_t>;

inline uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint32_t Le32(const uint8_t* p) { return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; }
inline uint16_t Le16(const uint8_t* p) { return uint16_t(p[1] << 8 | p[0]); }

inline void AppendUtf8(std::string& s, uint32_t cp) {
  if (cp < 0x80) s += char(cp);
  else if (cp < 0x800) s += char(0xC0 | cp >> 6), s += char(0x80 | (cp & 0x3F));
  else if (cp < 0x10000) s += char(0xE0 | cp >> 12), s += char(0x80 | ((cp >> 6) & 0x3F)), s += char(0x80 | (cp & 0x3F));
  else s += char(0xF0 | cp >> 18), s += char(0x80 | ((cp >> 12) & 0x3F)), s += char(0x80 | ((cp >> 6) & 0x3F)), s += char(0x80 | (cp & 0x3F));
}

inline std::string Clean(std::string s) {
  std::string o;
  for (unsigned char c : s) {
    if (c == '\r' || c == '\n' || c == '\t') o += ' ';
    else if (c >= 32 && c != 127) o += char(c);
  }
  while (!o.empty() && o.back() == ' ') o.pop_back();
  size_t i = 0;
  while (i < o.size() && o[i] == ' ') ++i;
  o = o.substr(i);
  if (o.size() > 200) o.resize(200);
  return o;
}

// ID3 text encodings: 0 Latin-1, 1 UTF-16 with BOM, 2 UTF-16BE, 3 UTF-8. Decodes [p, p+n) (stops at a NUL terminator).
inline std::string DecodeText(int enc, const uint8_t* p, size_t n) {
  std::string out;
  if (enc == 0 || enc == 3) {
    for (size_t i = 0; i < n && p[i]; ++i) {
      if (enc == 3) out += char(p[i]);
      else AppendUtf8(out, p[i]);
    }
    return out;
  }
  bool be = enc == 2;
  size_t i = 0;
  if (enc == 1 && n >= 2) {
    if (p[0] == 0xFE && p[1] == 0xFF) be = true, i = 2;
    else if (p[0] == 0xFF && p[1] == 0xFE) be = false, i = 2;
  }
  for (; i + 1 < n; i += 2) {
    uint32_t u = be ? uint32_t(p[i] << 8 | p[i + 1]) : uint32_t(p[i + 1] << 8 | p[i]);
    if (!u) break;
    if (u >= 0xD800 && u < 0xDC00 && i + 3 < n) {
      uint32_t lo = be ? uint32_t(p[i + 2] << 8 | p[i + 3]) : uint32_t(p[i + 3] << 8 | p[i + 2]);
      if (lo >= 0xDC00 && lo < 0xE000) u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00), i += 2;
    }
    AppendUtf8(out, u);
  }
  return out;
}

// start of the text after a terminator (1 NUL for Latin-1 / UTF-8, 2 for UTF-16); npos-like n when missing
inline size_t SkipTerminated(int enc, const uint8_t* p, size_t n, size_t from) {
  if (enc == 1 || enc == 2) {
    for (size_t i = from; i + 1 < n; i += 2) {
      if (!p[i] && !p[i + 1]) return i + 2;
    }
    return n;
  }
  for (size_t i = from; i < n; ++i) {
    if (!p[i]) return i + 1;
  }
  return n;
}

// ---------------- ID3v2 ----------------
inline void ReadId3(const Bytes& b, Tags& t) {
  if (b.size() < 10 || std::memcmp(b.data(), "ID3", 3) != 0) return;
  const int ver = b[3];  // 2, 3 or 4
  if (ver < 2 || ver > 4) return;
  const size_t tag_size = size_t(b[6] & 0x7F) << 21 | size_t(b[7] & 0x7F) << 14 | size_t(b[8] & 0x7F) << 7 | (b[9] & 0x7F);
  size_t pos = 10;
  const size_t end = std::min(b.size(), 10 + tag_size);
  if (b[5] & 0x40 && ver >= 3 && pos + 4 <= end) {  // extended header: skip it
    const size_t ext = ver == 4 ? (size_t(b[10] & 0x7F) << 21 | size_t(b[11] & 0x7F) << 14 | size_t(b[12] & 0x7F) << 7 | (b[13] & 0x7F))
                                : Be32(&b[10]) + 4;
    pos += ext;
  }
  const size_t head = ver == 2 ? 6 : 10;
  while (pos + head <= end) {
    char id[5] = {};
    std::memcpy(id, &b[pos], ver == 2 ? 3 : 4);
    if (!id[0]) break;
    size_t size;
    if (ver == 2) size = size_t(b[pos + 3]) << 16 | size_t(b[pos + 4]) << 8 | b[pos + 5];
    else if (ver == 3) size = Be32(&b[pos + 4]);
    else size = size_t(b[pos + 4] & 0x7F) << 21 | size_t(b[pos + 5] & 0x7F) << 14 | size_t(b[pos + 6] & 0x7F) << 7 | (b[pos + 7] & 0x7F);
    const size_t body = pos + head;
    if (size > end - body) break;
    const uint8_t* p = &b[body];
    const std::string sid = id;
    auto text = [&]() { return size >= 1 ? Clean(DecodeText(p[0], p + 1, size - 1)) : std::string(); };
    if (sid == "TIT2" || sid == "TT2") { if (t.title.empty()) t.title = text(); }
    else if (sid == "TPE1" || sid == "TP1") { if (t.artist.empty()) t.artist = text(); }
    else if (sid == "TALB" || sid == "TAL") { if (t.album.empty()) t.album = text(); }
    else if ((sid == "APIC" || sid == "PIC") && size > 4 && (t.picture.empty() || t.picture_type != 3)) {
      const int enc = p[0];
      size_t q = 1;
      if (sid == "PIC") q += 3;  // 3-char image format
      else q = SkipTerminated(0, p, size, q);  // MIME type
      if (q < size) {
        const int type = p[q++];
        q = SkipTerminated(enc, p, size, q);  // description
        if (q < size) {
          // prefer the front cover (type 3); otherwise the first picture found
          if (type == 3 || t.picture.empty()) t.picture.assign(p + q, p + size), t.picture_type = type;
        }
      }
    }
    pos = body + size;
  }
}

// ---------------- Vorbis comments (FLAC block, Ogg) ----------------
inline const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
inline Bytes Base64(const std::string& s) {
  int8_t map[256];
  std::memset(map, -1, sizeof map);
  for (int i = 0; i < 64; ++i) map[uint8_t(kB64[i])] = int8_t(i);
  Bytes out;
  uint32_t acc = 0;
  int bits = 0;
  for (unsigned char c : s) {
    if (map[c] < 0) continue;
    acc = acc << 6 | uint32_t(map[c]);
    bits += 6;
    if (bits >= 8) bits -= 8, out.push_back(uint8_t(acc >> bits)), acc &= (1u << bits) - 1;
  }
  return out;
}

inline std::string UpperAscii(std::string s) {
  for (auto& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// FLAC PICTURE block body -> image bytes
inline Bytes FlacPictureData(const uint8_t* p, size_t n, int* type_out = nullptr) {
  size_t q = 0;
  auto u32 = [&](uint32_t& v) { if (q + 4 > n) return false; v = Be32(p + q); q += 4; return true; };
  uint32_t type, mime, desc, w, h, d, c, len;
  if (!u32(type) || !u32(mime) || mime > n - q) return {};
  q += mime;
  if (!u32(desc) || desc > n - q) return {};
  q += desc;
  if (!u32(w) || !u32(h) || !u32(d) || !u32(c) || !u32(len) || len > n - q) return {};
  if (type_out) *type_out = int(type);
  return Bytes(p + q, p + q + len);
}

inline void ApplyVorbisComment(const uint8_t* p, size_t n, Tags& t) {
  if (n < 8) return;
  size_t q = 0;
  const uint32_t vendor = Le32(p);
  q = 4;
  if (vendor > n - q) return;
  q += vendor;
  if (q + 4 > n) return;
  const uint32_t count = Le32(p + q);
  q += 4;
  for (uint32_t i = 0; i < count && q + 4 <= n; ++i) {
    const uint32_t len = Le32(p + q);
    q += 4;
    if (len > n - q) break;
    const std::string kv(reinterpret_cast<const char*>(p + q), len);
    q += len;
    const size_t eq = kv.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = UpperAscii(kv.substr(0, eq)), v = kv.substr(eq + 1);
    if (k == "TITLE" && t.title.empty()) t.title = Clean(v);
    else if (k == "ARTIST" && t.artist.empty()) t.artist = Clean(v);
    else if (k == "ALBUM" && t.album.empty()) t.album = Clean(v);
    else if (k == "METADATA_BLOCK_PICTURE" && t.picture.empty()) {
      const Bytes raw = Base64(v);
      t.picture = FlacPictureData(raw.data(), raw.size());
    }
  }
}

inline void ReadFlac(const Bytes& b, Tags& t) {
  if (b.size() < 8 || std::memcmp(b.data(), "fLaC", 4) != 0) return;
  size_t pos = 4;
  for (;;) {
    if (pos + 4 > b.size()) return;
    const bool last = b[pos] & 0x80;
    const int type = b[pos] & 0x7F;
    const size_t len = size_t(b[pos + 1]) << 16 | size_t(b[pos + 2]) << 8 | b[pos + 3];
    pos += 4;
    if (len > b.size() - pos) return;
    if (type == 4) ApplyVorbisComment(&b[pos], len, t);
    else if (type == 6 && t.picture.empty()) t.picture = FlacPictureData(&b[pos], len);
    pos += len;
    if (last) return;
  }
}

// ---------------- Ogg (Vorbis / Opus comment packet, reassembled across pages) ----------------
inline void ReadOgg(const Bytes& b, Tags& t) {
  size_t pos = 0;
  Bytes packet;
  int packets_done = 0;
  const bool opus = b.size() > 40 && std::string(b.begin(), b.begin() + std::min<size_t>(b.size(), 200)).find("OpusHead") != std::string::npos;
  while (pos + 27 <= b.size() && std::memcmp(&b[pos], "OggS", 4) == 0 && packets_done < 3) {
    const int nseg = b[pos + 26];
    if (pos + 27 + size_t(nseg) > b.size()) return;
    size_t data = pos + 27 + size_t(nseg);
    for (int i = 0; i < nseg; ++i) {
      const size_t seg = b[pos + 27 + size_t(i)];
      if (data + seg > b.size()) return;
      packet.insert(packet.end(), b.begin() + long(data), b.begin() + long(data + seg));
      data += seg;
      if (seg < 255) {  // packet complete
        ++packets_done;
        if (packets_done == 2 || (opus && packets_done == 2)) {
          if (!opus && packet.size() > 7 && packet[0] == 3 && std::memcmp(&packet[1], "vorbis", 6) == 0) ApplyVorbisComment(&packet[7], packet.size() - 7, t);
          else if (opus && packet.size() > 8 && std::memcmp(packet.data(), "OpusTags", 8) == 0) ApplyVorbisComment(&packet[8], packet.size() - 8, t);
          return;
        }
        packet.clear();
      }
    }
    pos = data;
    if (packet.size() > 16u * 1024 * 1024) return;
  }
}

// ---------------- MP4 / M4A ----------------
struct Atom { size_t body, end; char type[5]; };
inline bool NextAtom(const Bytes& b, size_t pos, size_t limit, Atom& a) {
  if (pos + 8 > limit) return false;
  uint64_t size = Be32(&b[pos]);
  std::memcpy(a.type, &b[pos + 4], 4);
  a.type[4] = 0;
  a.body = pos + 8;
  if (size == 1) {
    if (pos + 16 > limit) return false;
    size = uint64_t(Be32(&b[pos + 8])) << 32 | Be32(&b[pos + 12]);
    a.body = pos + 16;
  } else if (size == 0) {
    size = limit - pos;
  }
  if (size < (a.body - pos) || size > limit - pos) return false;
  a.end = pos + size_t(size);
  return true;
}

inline void ReadMp4Ilst(const Bytes& b, size_t pos, size_t end, Tags& t) {
  Atom item;
  while (NextAtom(b, pos, end, item)) {
    std::string type(item.type, 4);
    Atom d;
    if (NextAtom(b, item.body, item.end, d) && std::string(d.type) == "data" && d.end - d.body >= 8) {
      const uint8_t* p = &b[d.body + 8];
      const size_t n = d.end - d.body - 8;
      if (type == "\xA9nam" && t.title.empty()) t.title = Clean(std::string(reinterpret_cast<const char*>(p), n));
      else if (type == "\xA9" "ART" && t.artist.empty()) t.artist = Clean(std::string(reinterpret_cast<const char*>(p), n));
      else if (type == "\xA9" "alb" && t.album.empty()) t.album = Clean(std::string(reinterpret_cast<const char*>(p), n));
      else if (type == "covr" && t.picture.empty()) t.picture.assign(p, p + n);
    }
    pos = item.end;
  }
}

inline void ReadMp4(const Bytes& b, size_t pos, size_t end, int depth, Tags& t) {
  Atom a;
  while (NextAtom(b, pos, end, a)) {
    const std::string type(a.type, 4);
    if (depth < 5 && (type == "moov" || type == "udta" || type == "trak" || type == "mdia")) ReadMp4(b, a.body, a.end, depth + 1, t);
    else if (type == "meta" && depth < 5) ReadMp4(b, a.body + 4, a.end, depth + 1, t);  // full box: 4 version / flags bytes
    else if (type == "ilst") ReadMp4Ilst(b, a.body, a.end, t);
    pos = a.end;
  }
}

// ---------------- WMA / ASF ----------------
inline std::string Utf16Le(const uint8_t* p, size_t n) {
  std::string out;
  for (size_t i = 0; i + 1 < n; i += 2) {
    uint32_t u = Le16(p + i);
    if (!u) break;
    if (u >= 0xD800 && u < 0xDC00 && i + 3 < n) {
      const uint32_t lo = Le16(p + i + 2);
      if (lo >= 0xDC00 && lo < 0xE000) u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00), i += 2;
    }
    AppendUtf8(out, u);
  }
  return out;
}

inline void ReadAsf(const Bytes& b, Tags& t) {
  static const uint8_t kHeader[16] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
  static const uint8_t kContent[16] = {0x33, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
  static const uint8_t kExtContent[16] = {0x40, 0xA4, 0xD0, 0xD2, 0x07, 0xE3, 0xD2, 0x11, 0x97, 0xF0, 0x00, 0xA0, 0xC9, 0x5E, 0xA8, 0x50};
  if (b.size() < 30 || std::memcmp(b.data(), kHeader, 16) != 0) return;
  size_t pos = 30;  // GUID 16 + size 8 + object count 4 + 2 reserved
  while (pos + 24 <= b.size()) {
    const uint64_t size = uint64_t(Le32(&b[pos + 20])) << 32 | Le32(&b[pos + 16]);
    if (size < 24 || size > b.size() - pos) return;
    const uint8_t* p = &b[pos + 24];
    const size_t n = size_t(size) - 24;
    if (std::memcmp(&b[pos], kContent, 16) == 0 && n >= 10) {
      const size_t tl = Le16(p), al = Le16(p + 2);
      if (10 + tl + al <= n) {
        if (t.title.empty()) t.title = Clean(Utf16Le(p + 10, tl));
        if (t.artist.empty()) t.artist = Clean(Utf16Le(p + 10 + tl, al));
      }
    } else if (std::memcmp(&b[pos], kExtContent, 16) == 0 && n >= 2) {
      size_t q = 2;
      for (unsigned i = 0, cnt = Le16(p); i < cnt && q + 2 <= n; ++i) {
        const size_t nl = Le16(p + q);
        q += 2;
        if (nl > n - q) break;
        const std::string name = Utf16Le(p + q, nl);
        q += nl;
        if (q + 4 > n) break;
        const unsigned type = Le16(p + q), vl = Le16(p + q + 2);
        q += 4;
        if (vl > n - q) break;
        const uint8_t* v = p + q;
        q += vl;
        if (name == "WM/AlbumTitle" && type == 0 && t.album.empty()) t.album = Clean(Utf16Le(v, vl));
        else if (name == "WM/Picture" && type == 1 && vl > 9 && t.picture.empty()) {
          // picture type 1, data length 4, MIME (UTF-16 NUL), description (UTF-16 NUL), data
          size_t r = 5;
          while (r + 1 < vl && (v[r] || v[r + 1])) r += 2;
          r += 2;
          while (r + 1 < vl && (v[r] || v[r + 1])) r += 2;
          r += 2;
          const size_t dl = Le32(v + 1);
          if (r <= vl && dl <= vl - r) t.picture.assign(v + r, v + r + dl);
        }
      }
    }
    pos += size_t(size);
  }
}

}  // namespace tags_detail

// Reads whatever tags the file's container carries. Never throws; unknown containers give empty Tags.
inline Tags ReadTags(const std::vector<uint8_t>& b) {
  Tags t;
  auto starts = [&](const char* s) { return b.size() >= std::strlen(s) && std::memcmp(b.data(), s, std::strlen(s)) == 0; };
  if (starts("ID3")) tags_detail::ReadId3(b, t);
  else if (starts("fLaC")) tags_detail::ReadFlac(b, t);
  else if (starts("OggS")) tags_detail::ReadOgg(b, t);
  else if (b.size() > 12 && std::memcmp(&b[4], "ftyp", 4) == 0) tags_detail::ReadMp4(b, 0, b.size(), 0, t);
  else if (b.size() > 16 && b[0] == 0x30 && b[1] == 0x26 && b[2] == 0xB2 && b[3] == 0x75) tags_detail::ReadAsf(b, t);
  return t;
}

}  // namespace audio_import
