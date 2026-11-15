r"""S102 R21-B lint (docs\renderer\STAGE_SPLIT.md): the shared state between the bind stage and the record stage stays removed.

  py -I tools\renderer\stage_split_lint.py [--root DIR]            -> exit 0 when every rule holds
  py -I tools\renderer\stage_split_lint.py --selftest [--root DIR] -> each planted twin (a copy of the tree's text with one guard taken out) must FAIL;
                                                                     a plant whose text no longer matches fails as DID NOT APPLY.
Rules (hazard numbers of STAGE_SPLIT.md section 2):
  1   programs / layouts are StableTable (append-only), never std::vector
  1,2 every writer of programs / layouts / the store's program maps takes tables_mutex before it touches them (AddProgram, AddProgramChecked,
      AddVertexLayout, RegisterShaderCatalog, StoreNoteBuilt); PrebuildOne / PrebuildSlice read the maps under it
  3   ImageEntry::gpu_class is touched only through std::atomic_ref
  5   serial is bumped through std::atomic_ref, and DestroyImage / DestroyBuffer read it through std::atomic_ref
  4,6 the stage seals the uploads at Submit and names the bind thread the producer; WaitForStagingRoom never waits on the producer
  7   live.cpp stores the renderer-read switches (g_gpu_stamps, g_gpu_outside, g_upload_batch, g_fence_friendly, g_copy_batch) only on the
      one-thread path (`if (!split_frame)`) or in the record stage's hook (from the packet)
  8   live.cpp calls ApplyOverlay only in the record stage's hook and on the one-thread path
"""
import argparse
import os
import re
import sys

FAILS = []


def say(ok, text):
    print(('PASS ' if ok else 'FAIL ') + text)
    if not ok:
        FAILS.append(text)
    return ok


def body(src, signature):
    """The text of a function from its signature to the first line that is a lone '}' at column 0."""
    at = src.find(signature)
    if at < 0:
        return None
    end = src.find('\n}\n', at)
    return src[at:end if end >= 0 else len(src)]


def strip_comments(text):
    return '\n'.join(line.split('//')[0] for line in text.split('\n'))


def check(files):
    rc = files['renderer.cpp']
    live = files['live.cpp']
    stage = files['stage_split.h']
    problems = []

    def need(ok, text):
        if not ok:
            problems.append(text)

    need('StableTable<Program> programs;' in rc and 'StableTable<VertexLayoutDesc> layouts;' in rc, '1: programs / layouts are not StableTable')
    need(not re.search(r'std::vector<\s*Program\s*>\s+programs', rc) and not re.search(r'std::vector<\s*VertexLayoutDesc\s*>\s+layouts', rc), '1: programs / layouts declared as std::vector')
    shared = re.compile(r'\b(programs|layouts|program_by_hash|program_info|layout_by_hash|store_catalog|store_retry|StoreFindProgram|StoreProgramAdded)\b')
    for sig in ('uint16_t Renderer::AddProgram(const ProgramDesc& desc) {', 'bool Renderer::AddProgramChecked(', 'uint16_t Renderer::AddVertexLayout(',
                'void Renderer::RegisterShaderCatalog(', 'void Renderer::Impl::StoreNoteBuilt(', 'bool Renderer::Impl::PrebuildOne(', 'void Renderer::Impl::PrebuildSlice('):
        b = body(rc, sig)
        if b is None:
            need(False, '1,2: %s not found' % sig)
            continue
        b = sig + strip_comments(b[len(sig):])
        lock = b.find('tables_mutex')
        first = shared.search(b[len(sig):])
        first_at = len(sig) + first.start() if first else -1
        need(lock >= 0 and (first_at < 0 or lock < first_at), '1,2: %s touches a shared table before (or without) tables_mutex' % sig.split('(')[0])
    # 3: the class steers Record, so it is written only by Render's ApplyUploads (the sealed change) and reset by CreateImage on an id no frame names yet
    writes = [m.group(0) for m in re.finditer(r'[^\n]*gpu_class\)?\s*(=[^=]|\.store\()[^\n]*', rc) if 'uint8_t gpu_class = 0;' not in m.group(0)]
    allowed = ('images[image].gpu_class = cls;', 'std::atomic_ref<uint8_t>(entry.gpu_class).store(0')
    bad_writes = [w for w in writes if not any(a in w for a in allowed)]
    need(not bad_writes, '3: gpu_class written outside ApplyUploads / CreateImage: %s' % (bad_writes[0].strip()[:120] if bad_writes else ''))
    setc = body(rc, 'void Renderer::SetImageClass(') or ''
    need('class_changes' in setc and 'gpu_class =' not in strip_comments(setc), '3: SetImageClass does not defer the change to the sealed batch')
    need(not re.search(r'(\+\+serial\b|\bserial\+\+|\bserial \+= )', rc), '5: serial bumped without atomic_ref')
    need(rc.count('std::atomic_ref<uint64_t>(im.serial).load') >= 2, '5: DestroyImage / DestroyBuffer do not read serial through atomic_ref')
    sub = body(stage, '  void Submit(const Packet& packet) {') or ''
    need('SealUploads()' in sub, '4: RecordStage::Submit does not seal the uploads')
    start = body(stage, '  void Start() {') or stage[stage.find('void Start()'):stage.find('void Run()')]
    need('SetProducerThread()' in start, '6: RecordStage::Start does not name the producer thread')
    wait = body(rc, 'void Renderer::Impl::WaitForStagingRoom() {') or ''
    need('producer_thread' in wait, '6: WaitForStagingRoom may wait on the producer (bind) thread')
    for g in ('g_gpu_stamps', 'g_gpu_pass_stamps', 'g_gpu_outside', 'g_upload_batch', 'g_fence_friendly', 'g_copy_batch', 'g_render_lean'):
        for line in live.splitlines():
            if g + '.store(' not in line:
                continue
            for part in line.split(g + '.store(')[1:]:
                pass
            ok = line.lstrip().startswith('if (!split_frame)') or ('p.' in line.split(g + '.store(')[1][:40])
            need(ok, '7: live.cpp stores rdr::%s outside the one-thread path / the stage hook: %s' % (g, line.strip()[:110]))
    calls = [l.strip() for l in live.splitlines() if re.search(r'(?<![\w.>])ApplyOverlay\(\);', l)]
    need(len(calls) == 1, '8: live.cpp calls ApplyOverlay %d times (want 1: the one-thread path; a split frame uses TakeOverlay + the hook)' % len(calls))
    hook_at = live.find('ss::RecordStage stage(')
    hook = strip_comments(live[hook_at:live.find('uint64_t stage_cpu_seen', hook_at)]) if hook_at >= 0 else ''
    need(bool(hook) and not re.search(r'ApplyOverlay|SetOverlayTexture|MakeOverlayTexture|UpdateImage|CreateImage', hook),
         '8 (review F1): the record stage hook makes or uploads an image (its upload would miss the frame\'s seal)')
    need(bool(re.search(r'TakeOverlay\(p\);[^\n]*\n\s*stage\.Submit\(p\);', live)), '8 (review F1): the drawn split frame does not take the overlay before Submit seals it')
    return problems


def load(root):
    paths = {'renderer.cpp': 'src/renderer/renderer.cpp', 'live.cpp': 'src/renderer_adapter/live.cpp', 'stage_split.h': 'src/renderer_adapter/stage_split.h'}
    out = {}
    for k, rel in paths.items():
        p = os.path.join(root, rel)
        if not os.path.exists(p):
            print('FAIL cannot read %s' % p)
            sys.exit(1)
        out[k] = open(p, encoding='utf-8').read().replace('\r\n', '\n')
    return out


PLANTS = [
    ('vector_table', 'renderer.cpp', 'StableTable<Program> programs;', 'std::vector<Program> programs;'),
    ('layout_unlocked', 'renderer.cpp', 'uint16_t Renderer::AddVertexLayout(const VertexLayoutDesc& desc) {\n  std::lock_guard<std::mutex> tables(impl_->tables_mutex);', 'uint16_t Renderer::AddVertexLayout(const VertexLayoutDesc& desc) {\n'),
    ('store_note_unlocked', 'renderer.cpp', '  std::lock_guard<std::mutex> tables(tables_mutex);  // S102 R21-B (hazard 2): program_info is written by AddProgram on the bind stage\n', ''),
    ('gpu_class_now', 'renderer.cpp', 'im.class_changes.emplace_back(image, gpu_class < kGpuClassCount ? gpu_class : uint8_t(kGpuOther));', 'im.images[image].gpu_class = gpu_class < kGpuClassCount ? gpu_class : uint8_t(kGpuOther);'),
    ('serial_plain', 'renderer.cpp', 'std::atomic_ref<uint64_t>(serial).fetch_add(1, std::memory_order_relaxed);', '++serial;'),
    ('no_seal', 'stage_split.h', 'const uint64_t seal = renderer_.SealUploads();', 'const uint64_t seal = 0;'),
    ('no_producer', 'stage_split.h', 'renderer_.SetProducerThread();', ''),
    ('global_unguarded', 'live.cpp', 'if (!split_frame) rdr::g_copy_batch.store(render_globals.copy_batch, std::memory_order_relaxed);', 'rdr::g_copy_batch.store(render_globals.copy_batch, std::memory_order_relaxed);'),
    ('overlay_in_hook', 'live.cpp', '          if (p.overlay_changed) renderer->SetOverlay(p.overlay_list);', '          ApplyOverlay();'),
    ('overlay_after_seal', 'live.cpp', '          TakeOverlay(p);   // review F1: before Submit seals the frame\'s uploads\n', ''),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    files = load(a.root)
    problems = check(files)
    say(not problems, 'the tree: %d rule(s) broken%s' % (len(problems), (': ' + '; '.join(problems)) if problems else ''))
    if a.selftest:
        for name, key, old, new in PLANTS:
            planted = dict(files)
            planted[key] = files[key].replace(old, new, 1)
            if planted[key] == files[key]:
                say(False, 'twin %s: DID NOT APPLY (its text is no longer in %s)' % (name, key))
                continue
            caught = check(planted)
            say(bool(caught), 'twin %s: %s' % (name, ('CAUGHT (' + caught[0][:100] + ')') if caught else 'NOT caught'))
    print('[stage_split_lint] %s' % ('FAIL' if FAILS else 'PASS'))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
