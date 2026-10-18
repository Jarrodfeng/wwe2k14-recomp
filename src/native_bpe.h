// wwe2k14 - native decoder for the game's byte-pair-encoded data blocks (Session 11). Standalone: no SDK headers, so
// the offline corpus test (tools/bpe/bpe_test.cpp) compiles this exact file.
//
// The guest decoder is sub_82C41918 (CData::Copy sub_82C35798 calls it for blocks tagged 'BPE '; analysis in
// docs/research/decoder_native.md). Block layout: tag at +0, two words the decoder does not read at +4 / +8, the
// expanded size as a little-endian 32-bit word at +12, the stream from +16. The stream is a sequence of BPE blocks
// (Gage 1994): a pair table (count byte: top bit set -> skip count-127 codes, then one pair; else count+1 pairs; a
// pair is a left byte, plus a right byte only when left != code), a 16-bit little-endian packed length, the packed
// bytes. Each packed byte expands through the tables with a stack (right child pushed, left expanded first). Blocks
// repeat until the output reaches the expanded size; like the guest, the check is only between blocks.
//
// Bounds the guest does not have (a caller falls back to the guest body when one trips): pair index past 256 (error
// 1), expansion stack deeper than the guest's frame holds (error 2: 3648 bytes, frame +544 .. +4192), output past
// out_cap (error 3), stream past in_cap (error 4).

#pragma once

#include <cstdint>
#include <cstring>

namespace wwe2k14::native_bpe {

struct Result {
  uint32_t produced = 0;  // bytes written
  uint32_t consumed = 0;  // stream bytes read after the 16-byte header
  uint32_t blocks = 0;
  int error = 0;          // 0 ok, 1 pair index, 2 stack, 3 output cap, 4 input cap
};

inline uint32_t ExpandedSize(const uint8_t* block) {
  return uint32_t(block[12]) | (uint32_t(block[13]) << 8) | (uint32_t(block[14]) << 16) | (uint32_t(block[15]) << 24);
}

constexpr uint32_t kGuestStackBytes = 3648;

// Reference decoder: a literal transcription of the guest loop (the Session 8 shadow decoder). out_cap / in_cap
// bound the output and the stream (in_cap counts stream bytes after the header).
inline Result DecodeReference(const uint8_t* block, uint8_t* out, uint32_t out_cap, uint32_t in_cap = 0xFFFFFFFFu) {
  Result r;
  const uint32_t expanded = ExpandedSize(block);
  const uint8_t* const p0 = block + 16;
  const uint8_t* p = p0;
  const uint8_t* const pend = (in_cap == 0xFFFFFFFFu) ? nullptr : p0 + in_cap;
  uint8_t* o = out;
  uint8_t* const end = out + expanded;
  uint8_t* const cap = out + out_cap;
  uint8_t left[256];
  uint8_t right[256];
  uint8_t stack[kGuestStackBytes];
#define WWE2K14_BPE_NEED(n)                      \
  do {                                           \
    if (pend && p + (n) > pend) {                \
      r.error = 4;                               \
      goto done;                                 \
    }                                            \
  } while (0)
  while (o < end) {
    for (int i = 0; i < 256; ++i) left[i] = uint8_t(i);
    int c = 0;
    for (;;) {
      WWE2K14_BPE_NEED(1);
      uint32_t count = *p++;
      if (count & 0x80) {
        c += int(count) - 127;
        if (c == 256) break;
        if (c > 255) { r.error = 1; goto done; }
        WWE2K14_BPE_NEED(1);
        left[c] = *p++;
        if (c != left[c]) { WWE2K14_BPE_NEED(1); right[c] = *p++; }
        ++c;
      } else {
        for (uint32_t n = count + 1; n; --n) {
          if (c > 255) { r.error = 1; goto done; }
          WWE2K14_BPE_NEED(1);
          left[c] = *p++;
          if (c != left[c]) { WWE2K14_BPE_NEED(1); right[c] = *p++; }
          ++c;
        }
      }
      if (c == 256) break;
    }
    {
      WWE2K14_BPE_NEED(2);
      uint32_t size = uint32_t(p[0]) | (uint32_t(p[1]) << 8);
      p += 2;
      WWE2K14_BPE_NEED(size);
      while (size--) {
        uint32_t sp = 0;
        uint8_t v = *p++;
        for (;;) {
          if (left[v] == v) {
            if (o >= cap) { r.error = 3; goto done; }
            *o++ = v;
            if (sp == 0) break;
            v = stack[--sp];
          } else {
            if (sp >= kGuestStackBytes) { r.error = 2; goto done; }
            stack[sp++] = right[v];
            v = left[v];
          }
        }
      }
    }
    ++r.blocks;
  }
#undef WWE2K14_BPE_NEED
done:
  r.produced = uint32_t(o - out);
  r.consumed = uint32_t(p - p0);
  return r;
}

// Faster decoder, same results: within a block, the first expansion of each top-level packed code is remembered
// (where it starts in the output and how long it is) and later occurrences copy those output bytes. A top-level
// expansion always starts with an empty stack, so a remembered code already expanded without tripping the stack
// bound and a copy cannot hide a stack error the reference would report. Short copies use two 8-byte moves when at
// least 16 bytes of room remain before the expanded end (the bytes past the copy are overwritten by the following
// output); never writes past min(expanded, out_cap).
inline Result DecodeMemo(const uint8_t* block, uint8_t* out, uint32_t out_cap, uint32_t in_cap = 0xFFFFFFFFu) {
  Result r;
  const uint32_t expanded = ExpandedSize(block);
  const uint8_t* const p0 = block + 16;
  const uint8_t* p = p0;
  const uint8_t* const pend = (in_cap == 0xFFFFFFFFu) ? nullptr : p0 + in_cap;
  uint8_t* o = out;
  uint8_t* const end = out + expanded;
  uint8_t* const cap = out + out_cap;
  uint8_t* const wend = (cap < end) ? cap : end;  // room for 16-byte moves
  uint8_t left[256];
  uint8_t right[256];
  uint32_t memo_pos[256];
  uint32_t memo_len[256];
  uint8_t stack[kGuestStackBytes];
#define WWE2K14_BPE_NEED(n)                      \
  do {                                           \
    if (pend && p + (n) > pend) {                \
      r.error = 4;                               \
      goto done;                                 \
    }                                            \
  } while (0)
  while (o < end) {
    for (int i = 0; i < 256; ++i) left[i] = uint8_t(i);
    std::memset(memo_len, 0, sizeof(memo_len));
    int c = 0;
    for (;;) {
      WWE2K14_BPE_NEED(1);
      uint32_t count = *p++;
      if (count & 0x80) {
        c += int(count) - 127;
        if (c == 256) break;
        if (c > 255) { r.error = 1; goto done; }
        WWE2K14_BPE_NEED(1);
        left[c] = *p++;
        if (c != left[c]) { WWE2K14_BPE_NEED(1); right[c] = *p++; }
        ++c;
      } else {
        for (uint32_t n = count + 1; n; --n) {
          if (c > 255) { r.error = 1; goto done; }
          WWE2K14_BPE_NEED(1);
          left[c] = *p++;
          if (c != left[c]) { WWE2K14_BPE_NEED(1); right[c] = *p++; }
          ++c;
        }
      }
      if (c == 256) break;
    }
    {
      WWE2K14_BPE_NEED(2);
      uint32_t size = uint32_t(p[0]) | (uint32_t(p[1]) << 8);
      p += 2;
      WWE2K14_BPE_NEED(size);
      const uint8_t* const pk_end = p + size;
      while (p < pk_end) {
        const uint8_t top = *p++;
        if (left[top] == top) {
          if (o >= cap) { r.error = 3; goto done; }
          *o++ = top;
          continue;
        }
        const uint32_t len = memo_len[top];
        if (len) {
          const uint8_t* s = out + memo_pos[top];
          if (len <= 16 && o + 16 <= wend) {
            uint64_t a, b;
            std::memcpy(&a, s, 8);
            std::memcpy(&b, s + 8, 8);
            std::memcpy(o, &a, 8);
            std::memcpy(o + 8, &b, 8);
          } else {
            if (uint64_t(cap - o) < len) { r.error = 3; goto done; }
            std::memcpy(o, s, len);  // s + len <= o: no overlap
          }
          o += len;
          continue;
        }
        uint8_t* const start = o;
        uint32_t sp = 0;
        uint8_t v = top;
        for (;;) {
          if (left[v] == v) {
            if (o >= cap) { r.error = 3; goto done; }
            *o++ = v;
            if (sp == 0) break;
            v = stack[--sp];
          } else {
            if (sp >= kGuestStackBytes) { r.error = 2; goto done; }
            stack[sp++] = right[v];
            v = left[v];
          }
        }
        memo_pos[top] = uint32_t(start - out);
        memo_len[top] = uint32_t(o - start);
      }
    }
    ++r.blocks;
  }
#undef WWE2K14_BPE_NEED
done:
  r.produced = uint32_t(o - out);
  r.consumed = uint32_t(p - p0);
  return r;
}

// The decoder the game hook uses. Same results as DecodeReference (the offline test checks both against the corpus).
inline Result Decode(const uint8_t* block, uint8_t* out, uint32_t out_cap, uint32_t in_cap = 0xFFFFFFFFu) {
  return DecodeMemo(block, out, out_cap, in_cap);
}

}  // namespace wwe2k14::native_bpe
