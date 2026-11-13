// wwe2k14 - Session 100, strip-360 E2 / E3 (STRIP360_PLAN.md part 2): host code that writes guest memory the new renderer's reader may hold
// contents of (textures, vertex and index buffers) reports the bytes here. Writers: the file reads (NtReadFile / NtReadFileScatter,
// src\file_trace.h), our native replacements that declare guest writes (src\native_fn.h `kNotifyWrites`: the crowd's CPU skinning, the two
// memcpys), the LZX decompressor hook (src\lzx_probe.h). The reader (src\renderer_adapter\reader.h OnHostWrite) turns them into its
// written-page marks; with `--wwe2k14_rdr_page_watch=false` (the default) they are the only marks: no page protection, no write faults.
// Census (runs 6973-6976): in owner mode every content change after a resource's first read came from these or from the game's own flush
// calls (texture unlock flush, movie plane flush).
#pragma once

#include <atomic>
#include <cstdint>

namespace wwe2k14::host_write {

using Fn = void (*)(uint32_t guest_address, uint32_t length);
inline std::atomic<Fn> g_hook{nullptr};

inline void Note(uint32_t guest_address, uint32_t length) {
  if (Fn f = g_hook.load(std::memory_order_relaxed)) f(guest_address, length);
}

// S101 names A1c (texture provenance, docs\renderer\NAMES.md): each writer says who it is, so a texture's newest bytes have a declared owner.
// A file read and a decompressor make FILE content; the others (our native memcpys, skinning, unlock flushes) are named CPU writers.
enum class Writer : uint8_t { Unknown = 0, FileRead, Decompress, Memcpy, CrowdSkin, VertexUnlock, Count };
inline const char* WriterName(Writer w) {
  static const char* const k[] = {"unknown", "file_read", "decompress", "memcpy", "crowd_skin", "vertex_unlock"};
  return uint8_t(w) < uint8_t(Writer::Count) ? k[uint8_t(w)] : "?";
}
// file: the path a FileRead read, else null. source: a copy's source address (Memcpy), else 0: the copy inherits the source's owner.
using WriterFn = void (*)(uint32_t guest_address, uint32_t length, Writer writer, const char* file, uint32_t source);
inline std::atomic<WriterFn> g_writer_hook{nullptr};

// The same report as Note, with its writer (and for a file read, the file). Calls Note too, so the reader's page marks stay as they were.
inline void NoteBy(uint32_t guest_address, uint32_t length, Writer writer, const char* file = nullptr, uint32_t source = 0) {
  Note(guest_address, length);
  if (WriterFn f = g_writer_hook.load(std::memory_order_relaxed)) f(guest_address, length, writer, file, source);
}

// S101 lane17 (A1c item 1): the bytes a file read really wrote, from the read's return, not the length it asked for. status: the NT status the
// SDK returned (>= 0 success or pending); information / block_status: the io status block's byte count and status. The SDK completes every read
// at once and fills the block (xboxkrnl_io.cpp:219-221), but on a non-synchronous handle (the game's ReadFileEx / overlapped pac reads) it
// returns STATUS_PENDING anyway (:244-245, scatter :338-339): the block, not the return, says what happened. Only a block that itself still says
// PENDING has no count yet (the request, an upper bound). A block that says failed wrote nothing. no_block: the caller passed no block.
inline uint32_t BytesRead(int32_t status, uint32_t requested, uint32_t information, bool no_block = false, int32_t block_status = 0) {
  if (status < 0) return 0;
  if (no_block) return requested;
  if (block_status < 0) return 0;
  if (status == 0x103 && block_status == 0x103) return requested;  // STATUS_PENDING all the way: no count yet
  return information < requested ? information : requested;
}

// S101 lane17 (menu textures labelled CPU): a block decoder's output (CData::Copy's BPE and zlib decoders, src/decode_probe.h) reported with its
// input block as `source`: the output keeps the input's FILE owner (the reader's page map), as a memcpy does.
// LABELS ONLY: it reaches the writer hook, not Note (the reader's written-page marks), so it changes no re-read decision (the marks would, on the
// default owner path; that is a rendering change nobody has checked: docs\renderer\NAMES.md, provenance item 7, follow-up).
inline void NoteDecoded(uint32_t dst, uint32_t length, uint32_t source) {
  if (WriterFn f = g_writer_hook.load(std::memory_order_relaxed)) f(dst, length, Writer::Decompress, nullptr, source);
}

// S101 lane17 review: native_fn.h's Dispatch for a Spec that declares its guest writes (`kNotifyWrites`), as one template so the harness runs
// the real code. Everything the report needs is read BEFORE `run` (the natives write leftover values back into the registers: memcpy_fwd leaves
// r4 advanced, memcpy_vmx sets r4 = 128 on its aligned path, so a Source read after the call names the wrong page).
//   Ranges: native_fn::Ranges (.n, .r[i].addr / .size); Spec::Writes(ctx, base, Ranges&); optional Spec::kWriter, Spec::Source(ctx).
template <class Spec, class Ranges, class Ctx, class Run>
inline void CallReporting(Ctx& ctx, uint8_t* base, Run&& run) {
  Ranges written;
  Spec::Writes(ctx, base, written);
  Writer writer = Writer::Unknown;
  if constexpr (requires { Spec::kWriter; }) writer = Spec::kWriter;
  uint32_t source = 0;
  if constexpr (requires { Spec::Source(ctx); }) source = Spec::Source(ctx);
  run();
  for (int i = 0; i < written.n; ++i) NoteBy(written.r[i].addr, written.r[i].size, writer, nullptr, i == 0 ? source : 0);
}

}  // namespace wwe2k14::host_write
