#!/usr/bin/env python3
r"""Lane 17 offline acceptance (Session 101): texture provenance accuracy (docs\renderer\NAMES.md "Texture provenance"), no game.
  py -I tools\prov\selftest.py [--keep]
 1. builds tools\prov\prov_test.cpp (clang++, the real src\renderer_adapter\prov_map.h, host_write.h, content_fit.h) and runs it: every case PASS
    (one per open item I1-I6, the Session 100 "unannounced writes" sequence R1, case 7041 R2, a 1000-run concurrency case).
 2. planted faults: for each fix, a mutated copy of the headers with that fix undone (the text replaced; a replacement that finds nothing FAILS
    the check) is built and run: the matching case(s) must FAIL. This is the "fails on the code before the change" proof, re-runnable.
 3. source pins (the glue the harness cannot include): StartLive and StartCapture install the writer hook, ProvenanceOf uses prov::Classify, the
    file reads use BytesRead, the binder uses ContentIsStale, native_fn.h's Dispatch goes through CallReporting. These are TEXT checks only (the
    glue is not compiled by the harness); the reader and the game build are covered by the syntax check, not by this file.
 4. the frame format is unchanged: src\renderer_adapter\names.h is byte-identical to the base branch (s101-renderer-arc).
Exit 0 = all checks passed.
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / 'out' / 'prov'
CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
NEEDS = ['src/host_write.h', 'src/guest_view.h', 'src/renderer_adapter/prov_map.h', 'src/renderer_adapter/names.h',
         'src/renderer_adapter/content_fit.h']

# (id, file, old text, new text, cases that must FAIL)
MUTATIONS = [
    ('M1 file read labels the requested length', 'src/host_write.h', 'return information < requested ? information : requested;', 'return requested;', ['I1.bytes_read']),
    ('M2a memcpy takes only the first source page', 'src/renderer_adapter/prov_map.h',
     'const uint32_t off = p == first ? 0u : ((addr & ~0xFFFu) + ((p - first) << 12)) - addr;', 'const uint32_t off = 0u;', ['I2a.memcpy_page_by_page']),
    ('M2b a heap memcpy is not tracked (staging chain)', 'src/renderer_adapter/prov_map.h', '&& !(writer == W::Memcpy && source)) { Clear', ') { Clear',
     ['I2b.heap_staging_chain']),
    ('M2c a rewritten heap page keeps its FILE owner', 'src/renderer_adapter/prov_map.h', 'Clear(first, last); return 0; }', 'return 0; }', ['I2c.heap_overwritten_not_file']),
    ('M3a a flushed FILE texture stays FILE', 'src/renderer_adapter/prov_map.h',
     'f.flush_seen && f.first_read_seq && f.first_read_seq < f.flush_seq && f.flush_seq > e.seq', 'false',
     ['I3.file_rewritten_then_flushed', 'R1.unannounced_writes_s100']),
    ('M3b the flush rule compares frames (a worker thread vs VdSwap), not events', 'src/renderer_adapter/prov_map.h',
     'f.flush_seen && f.first_read_seq && f.first_read_seq < f.flush_seq && f.flush_seq > e.seq', 'f.flush_seen && f.flush_frame > e.frame',
     ['I3.file_rewritten_then_flushed']),
    ('M3c a loader\'s own flush (before the first bind) counts as a rewrite', 'src/renderer_adapter/prov_map.h',
     'f.first_read_seq && f.first_read_seq < f.flush_seq && ', '', ['I3.file_rewritten_then_flushed']),
    ('M8 Dispatch reads the memcpy source after the call', 'src/host_write.h',
     '  if constexpr (requires { Spec::Source(ctx); }) source = Spec::Source(ctx);\n  run();',
     '  run();\n  if constexpr (requires { Spec::Source(ctx); }) source = Spec::Source(ctx);', ['I2e.dispatch_reads_source_before_call']),
    ('M10 a decoder\'s output does not take its input block\'s owner (the menu path)', 'src/renderer_adapter/prov_map.h',
     'if (s.valid && IsLoader(s.writer)) { decoded_from_file = true; decoded_file = s.file; }\n      else if (s.valid) {',
     'writer = W::Memcpy; if (false) {', ['I7.decoded_block_keeps_file_owner']),
    ('M11 a decoded block takes the newest read\'s file, not its source\'s', 'src/renderer_adapter/prov_map.h', 'file_id = decoded_file;',
     'file_id = last_file_.load(std::memory_order_relaxed);', ['I7.decoded_block_keeps_file_owner']),
    ('M12 a decode of an input with no entry is declared CPU', 'src/renderer_adapter/prov_map.h', 'else if (s.valid) { writer = W::Memcpy;', 'else { writer = W::Memcpy;',
     ['I7.decoded_block_keeps_file_owner']),
    ('M13 a memcpy from a source with no entry is counted as CPU bytes', 'src/renderer_adapter/prov_map.h', 'extra = !s.valid ? kIdSrcNone :', 'extra = !s.valid ? kIdSrcCpu :',
     ['I8.census_memcpy_sources', 'I8b.read_before_the_hook_is_the_menu_miss']),
    ('M14 no way of asking for provenance installs the hook at app start', 'src/renderer_adapter/prov_map.h', 'return cvar || case_ring || dev_owner_build || capture_frames_at_start;', 'return false;',
     ['I8b.read_before_the_hook_is_the_menu_miss']),
    ('M15 the hook records only while a capture / the live mirror runs (the frame-0 read is dropped)', 'src/renderer_adapter/prov_map.h', 'return provenance_on || recording;', 'return recording;',
     ['I8b.read_before_the_hook_is_the_menu_miss']),
    ('M16 a heap page overwritten after its load is called no-entry, not CPU', 'src/renderer_adapter/prov_map.h', 'heap_[p].store(kCpuMarker, std::memory_order_release);', 'heap_[p].store(0, std::memory_order_release);',
     ['I2c.heap_overwritten_not_file']),
    ('M9 an overlapped read (PENDING return, filled block) labels the request', 'src/host_write.h',
     'if (status == 0x103 && block_status == 0x103) return requested;', 'if (status == 0x103) return requested;', ['I1.bytes_read']),
    ('M4a a copy destination has no source_image', 'src/renderer_adapter/prov_map.h',
     'if (f.copy_dest) { l.source = P::Gpu; l.source_image = f.copy_source; return l; }', 'if (f.copy_dest) { l.source = P::Gpu; return l; }', ['I4.copy_source_image']),
    ('M4b a packed attire has no source_image', 'src/renderer_adapter/prov_map.h', 'l.writer = "attire_pack"; l.source_image = f.copy_source;', 'l.writer = "attire_pack";',
     ['I4b.packed_source_image']),
    ('M5 the writer hook is never installed', 'src/renderer_adapter/prov_map.h', 'g_writer_hook.store(fn, std::memory_order_relaxed)', 'g_writer_hook.store(nullptr, std::memory_order_relaxed)',
     ['I5.hook_installed']),
    ('M6 an entry is written in two halves (torn)', 'src/renderer_adapter/prov_map.h',
     'space[p].store(Pack(writer, decoded_from_file, file_id, seq, extra), std::memory_order_release);',
     '{ auto* h = reinterpret_cast<std::atomic<uint32_t>*>(&space[p]); const uint64_t v = Pack(writer, decoded_from_file, file_id, seq, extra); '
     'h[1].store(uint32_t(v >> 32), std::memory_order_relaxed); h[0].store(uint32_t(v), std::memory_order_relaxed); }', ['I6b.lock_free_no_torn_entry']),
    ('M7 case 7041: a stale content is uploaded', 'src/renderer_adapter/content_fit.h', '&& content_format != image_formats[image]', '&& false', ['R2.case7041_stale_content']),
]

# (id, file, must contain, region start marker or None)
PINS = [
    ('StartCapture installs the writer hook', 'src/renderer_adapter/reader.h', 'wwe2k14::prov::InstallWriterHook(&OnHostWriteBy)', 'inline void StartCapture(uint32_t frames) {'),
    ('ProvenanceOf uses prov::Classify', 'src/renderer_adapter/reader.h', 'wwe2k14::prov::Classify(f, e)', 'inline cap::TextureProvenance ProvenanceOf('),
    ('OnHostWriteBy writes the flat map', 'src/renderer_adapter/reader.h', 'wwe2k14::prov::Map().Note(', 'inline void OnHostWriteBy(uint32_t addr, uint32_t length, wwe2k14::host_write::Writer writer, const char* file, uint32_t source) {'),
    ('NtReadFile reports the bytes read', 'src/file_trace.h', 'host_write::NoteBy(buffer.guest_address(), got,', None),
    ('NtReadFileScatter reports the bytes read', 'src/file_trace.h', 'std::min<uint32_t>(4096, got - done)', None),
    ('the binder uses ContentIsStale (case 7041)', 'src/renderer_adapter/binder.h', 'ContentIsStale(image_formats_, ref.image, texture.header.format)', None),
    ('a copy records its source image', 'src/renderer_adapter/reader.h', 'dest.copy_source = source_id;', None),
    ('Dispatch goes through CallReporting (Source read before the call)', 'src/native_fn.h', 'host_write::CallReporting<Spec, Ranges>(', None),
    ('a pending read passes the block status', 'src/file_trace.h', 'int32_t(uint32_t(io_status_block->status))', None),
    ('the first read of an image is stamped', 'src/renderer_adapter/reader.h', 'img.first_read_seq = wwe2k14::prov::Map().Stamp(', None),
    ('the BPE decoder reports its output', 'src/decode_probe.h', 'host_write::NoteDecoded(dst, expanded, block)', None),
    ('the zlib decoder reports its output', 'src/decode_probe.h', 'host_write::NoteDecoded(dst, dst_len, src)', None),
    ('the hook is installed at app start, only when wanted', 'src/renderer_adapter/reader.h', 'wwe2k14::prov::WantedAtStart(', None),
    ('OnPreSetup installs it', 'src/wwe2k14_app.h', 'InstallProvenanceAtStart();', None),
    ('the hook guard is ShouldRecord', 'src/renderer_adapter/reader.h', 'wwe2k14::prov::ShouldRecord(g_prov_on', None),
    ('FrameProvenance is gated', 'src/renderer_adapter/reader.h', 'if (!g_prov_on.load(std::memory_order_relaxed) && !g_cap.active) return;', None),
    ('the census is logged', 'src/renderer_adapter/reader.h', 'memcpys into physical pages', None),
    ('an unlock flush stamps the image', 'src/renderer_adapter/reader.h', 'img.flush_frame = uint32_t(', None),
]


def run(cmd, timeout):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


def build(src_root, exe):
    r = run([CLANG, '-std=c++20', '-Wall', '-Wextra', '-O1', f'-I{src_root}', str(REPO / 'tools' / 'prov' / 'prov_test.cpp'), '-o', str(exe)], 300)
    if r.returncode != 0:
        print(r.stdout + r.stderr)
        return False
    return True


def run_cases(exe):
    r = run([str(exe)], 300)
    cases = {}
    for line in r.stdout.splitlines():
        if line.startswith('CASE '):
            parts = line.split(' ', 3)
            cases[parts[1]] = parts[2]
    return r.returncode, cases, r.stdout


def mutated_tree(index, mut):
    root = OUT / f'mut{index}'
    if root.exists():
        shutil.rmtree(root)
    for rel in NEEDS:
        dst = root / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(REPO / rel, dst)
    _, rel, old, new, _ = mut
    path = root / rel
    text = path.read_bytes().decode('utf-8').replace('\r\n', '\n')  # (the repo's headers are CRLF)
    if text.count(old) != 1:
        return None, f'the text to undo was found {text.count(old)} times (expected 1): {old[:70]}'
    path.write_bytes(text.replace(old, new).encode('utf-8'))
    return root / 'src', ''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--keep', action='store_true')
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    fails = 0
    # 1
    exe = OUT / 'prov_test.exe'
    if not build(REPO / 'src', exe):
        print('FAIL build of the real code')
        return 1
    code, cases, out = run_cases(exe)
    if code != 0 or not cases or any(v != 'PASS' for v in cases.values()):
        print(out)
        print('FAIL the cases on the real code')
        return 1
    print(f'PASS 1: {len(cases)} cases on the real code: ' + ', '.join(sorted(cases)))
    # 2
    for i, mut in enumerate(MUTATIONS):
        tree, why = mutated_tree(i, mut)
        if tree is None:
            print(f'FAIL 2 {mut[0]}: {why}')
            fails += 1
            continue
        mexe = OUT / f'mut{i}.exe'
        if not build(tree, mexe):
            print(f'FAIL 2 {mut[0]}: the mutated copy did not build')
            fails += 1
            continue
        _, mcases, _ = run_cases(mexe)
        missing = [c for c in mut[4] if mcases.get(c) != 'FAIL']
        if missing:
            print(f'FAIL 2 {mut[0]}: the planted fault was NOT caught by {missing} (got {mcases})')
            fails += 1
        else:
            print(f'PASS 2 {mut[0]}: before-the-fix code FAILS {mut[4]}')
        if not args.keep:
            shutil.rmtree(tree.parent, ignore_errors=True)
            mexe.unlink(missing_ok=True)
    # 3
    for name, rel, needle, marker in PINS:
        text = (REPO / rel).read_bytes().decode('utf-8')
        region = text
        if marker:
            at = text.find(marker)
            if at < 0:
                print(f'FAIL 3 {name}: marker not found in {rel}')
                fails += 1
                continue
            end = text.find('\ninline ', at + len(marker))
            region = text[at:end if end > 0 else len(text)]
        ok = needle in region
        if ok:
            print(f'PASS 3 (text pin) {name}')
        else:
            print(f'FAIL 3 (text pin) {name}: missing in {rel}')
            fails += 1
    # 4
    r = run(['git', '-C', str(REPO), 'diff', '--quiet', 's101-renderer-arc', '--', 'src/renderer_adapter/names.h'], 60)
    if r.returncode == 0:
        print('PASS 4 names.h (the frame format) is unchanged from s101-renderer-arc')
    else:
        print('FAIL 4 names.h differs from s101-renderer-arc (a format change needs a version bump written into NAMES.md)')
        fails += 1
    print('PASS' if not fails else f'FAIL: {fails} check(s)')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
