r"""S102 lane M1: the C++ unit test of counts.h (tools\budget\counts_test.cpp) plus a source lint of the counters in src\renderer\renderer.cpp.
  py -I tools\budget\counts_unit.py
Lint (review D / E): the image upload bytes counted are the requested bytes (ImageUpload::bytes), never the driver's allocation size (memory.size); every copy / blit
recorded in ApplyUploads and RecordCopy is followed by CountCopy(); every raw vkCmdPipelineBarrier in ApplyUploads is followed by CountBarrier(); a planted copy of
the source with one count removed must make the lint fail.
"""
import re, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parents[2]
CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'


def region(text, start_pat, end_pat):
    m = re.search(start_pat, text, re.M)
    if not m:
        raise SystemExit('FAIL lint: cannot find %s in renderer.cpp' % start_pat)
    e = re.search(end_pat, text[m.end():], re.M)
    return text[m.start(): m.end() + (e.start() if e else len(text))]


def lint(text):
    """Returns a list of problems."""
    bad = []
    if re.search(r'upload_bytes\s*\+=\s*upload\.memory\.size', text.replace('g_render_times.upload_bytes', '')):
        bad.append('counts_cur.upload_bytes follows upload.memory.size (the driver allocation), not the requested bytes')
    if 'counts_cur.upload_bytes += upload.bytes' not in text:
        bad.append('image upload bytes are not counted from upload.bytes')
    for name, start, end in (('ApplyUploads', r'^void Renderer::Impl::ApplyUploads', r'^// ---- caches'), ('RecordCopy', r'^void Renderer::Impl::RecordCopy', r'^bool Renderer::Impl::RecordPresent')):
        r = region(text, start, end).splitlines()
        for i, line in enumerate(r):
            if re.search(r'\bvkCmd(CopyBufferToImage|CopyImage|CopyBuffer|BlitImage)\(', line) and not any('CountCopy()' in x for x in r[i + 1:i + 3]):
                bad.append('%s: a copy / blit is not counted: %s' % (name, line.strip()[:70]))
            if name == 'ApplyUploads' and re.search(r'\bvkCmdPipelineBarrier\(', line) and not any('CountBarrier(' in x for x in r[i + 1:i + 3]):
                bad.append('%s: a raw barrier is not counted: %s' % (name, line.strip()[:70]))
    return bad


def main():
    exe = Path(tempfile.mkdtemp(prefix='counts_unit_')) / 'counts_test.exe'
    p = subprocess.run([CLANG, '-std=c++20', '-Wall', '-Wextra', str(HERE / 'tools' / 'budget' / 'counts_test.cpp'), '-o', str(exe)], capture_output=True, text=True, timeout=300)
    if p.returncode != 0:
        print('FAIL counts_test build (exit %d)\n%s' % (p.returncode, p.stderr[-600:]))
        return 1
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    print(r.stdout.strip())
    if r.returncode != 0:
        return 1
    sys.path.insert(0, str(HERE / 'tools' / 'split'))
    from read_unit import read_path  # renderer.cpp is split: read it with its parts expanded (tools/split/read_unit.py)
    text = read_path(HERE / 'src' / 'renderer' / 'renderer.cpp')
    bad = lint(text)
    for b in bad:
        print('FAIL lint: ' + b)
    if bad:
        return 1
    print('lint: the counters in renderer.cpp are placed as the review asked')
    # planted faults: each must be caught
    faults = [('upload bytes from the allocation size', text.replace('counts_cur.upload_bytes += upload.bytes', 'counts_cur.upload_bytes += upload.memory.size', 1)),
              ('a blit not counted', re.sub(r'(vkCmdBlitImage\(cmd, temp\.image[^\n]*\n)\s*CountCopy\(\);[^\n]*\n', r'\1', text, count=1)),
              # S102 main (04:20): G5 made the line `CountBarrier(1), ++out_up_explicit;` and the old pattern (`CountBarrier\(1\);`) stopped
              # applying, so the plant silently became a no-op. Match the call, whatever follows it on the line.
              ('a raw barrier not counted', re.sub(r'(&to_dst\);\r?\n)\s*CountBarrier\(1\)[^\n]*\n', r'\1', text, count=1))]
    ok = True
    for name, t in faults:
        if t == text:   # a plant that no longer applies is a broken check, not a missed fault: say which
            print('FAIL  planted fault "%s" DID NOT APPLY (its pattern no longer matches renderer.cpp: update the plant)' % name)
            ok = False
            continue
        caught = bool(lint(t))
        print('%s  planted fault "%s" is %s' % ('PASS' if caught else 'FAIL', name, 'caught' if caught else 'NOT caught'))
        ok &= caught
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
