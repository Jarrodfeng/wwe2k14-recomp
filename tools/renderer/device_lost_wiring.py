r"""S102 lane D1 (docs\renderer\DEVICE_LOST.md): the lost-device accounting is wired at every link, read as text; the bench reader names the frame. Offline, 0 runs.
  py -I tools\renderer\device_lost_wiring.py [--root DIR]
Links (each one removed in a text copy must make this lint FAIL; a plant whose text is gone FAILs as DID NOT APPLY):
  swapchain    Swapchain::Present accepts no present once rdr::DeviceLost() (window and offscreen paths), and names the call that saw VK_ERROR_DEVICE_LOST
  renderer     Render counts rdr::g_presented only for an accepted present, and calls OnDeviceLost from the fence wait, the submit and the swapchain's lost_at()
  live         live.cpp's NotePresent (frames csv `presented`, the "on screen" line, the hang guard's beat) returns at once after a loss (inside it: every path)
  csv          frame_stats.h writes device_lost after record_thread_ms and before the fs_ split columns, the row passes lost_now
  table        the three switches are in the S0-4 table (PerFrame, defaults 1 / 0 / 0), have cvars in reader.h, and live.cpp reads each every frame
Then bench_line.device_lost_row on synthetic csvs: a csv with device_lost = 1 from frame 3416 names 3416; an old csv without the column and a clean one name none.
Exit 1 on any failure."""
import argparse, csv, io, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
FAILS = []


def say(ok, text):
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    if not ok:
        FAILS.append(text)


def read(root, rel):
    with open(os.path.join(root, rel), encoding='utf-8', newline='') as f:
        return f.read().replace('\r\n', '\n')


# (name, file, list of regexes that must all match)
LINKS = [
    ('swapchain present (window)', 'src/renderer/swapchain.cpp', [r'return \(result == VK_SUCCESS \|\| result == VK_SUBOPTIMAL_KHR\) && !DeviceLost\(\);']),
    ('swapchain present (offscreen)', 'src/renderer/swapchain.cpp', [r'if \(offscreen_\) return !DeviceLost\(\);']),
    ('swapchain names the lost call', 'src/renderer/swapchain.cpp', [r'if \(result == VK_ERROR_DEVICE_LOST\) lost_at_ = "vkAcquireNextImageKHR";', r'if \(result == VK_ERROR_DEVICE_LOST\) lost_at_ = "vkQueuePresentKHR";']),
    ('renderer counts accepted presents only', 'src/renderer/renderer.cpp', [r'if \(accepted \|\| \(crumb_plant == 3 && DeviceLost\(\)\)\) \{']),
    ('renderer: fence wait -> OnDeviceLost', 'src/renderer/renderer.cpp', [r'if \(waited == VK_ERROR_DEVICE_LOST\) OnDeviceLost\("vkWaitForFences"']),
    ('renderer: submit -> OnDeviceLost', 'src/renderer/renderer.cpp', [r'if \(submitted == VK_ERROR_DEVICE_LOST\) OnDeviceLost\("vkQueueSubmit"']),
    ('renderer: acquire / present -> OnDeviceLost', 'src/renderer/renderer.cpp', [r'if \(const char\* at = swapchain\.lost_at\(\); at && !lost_serial\) OnDeviceLost\(at']),
    ('renderer: the loss sets rdr::g_device_lost', 'src/renderer/renderer.cpp', [r'g_device_lost\.store\(serial \+ 1']),
    ('live: NotePresent itself refuses after a loss (every path: sync and R21-B split)', 'src/renderer_adapter/live.cpp', [r'void NotePresent\(\) \{\n\s*if \(rdr::DeviceLost\(\)\) return;']),
    ('renderer: no submit / acquire / present on a really lost device', 'src/renderer/renderer.cpp', [r'if \(lost_serial && !lost_simulated\) \{\n\s*if \(crumb\) crumb->submitted = false;', r'if \(lost_serial && !lost_simulated\) records = false;']),
    ('renderer: the dump comes before the Problem (RDR_DEV stops on it)', 'src/renderer/renderer.cpp', [r'if \(submitted == VK_ERROR_DEVICE_LOST\) OnDeviceLost\("vkQueueSubmit", int\(submitted\), false\);[^\n]*\n\s*Problem\("frame_submit_failed"',
                                                                                                    r'DumpCrumbs\(simulated\);\n(?:\s*//[^\n]*\n)*\s*Problem\("device_lost"']),
    ('markers: END words behind barriers', 'src/renderer/renderer.cpp', [r'if \(!bare\) vkCmdPipelineBarrier\(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT', r'vkCmdPipelineBarrier\(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &flush']),
    ('live (R21-B split frames): the switches travel with the frame', 'src/renderer_adapter/live.cpp', [r'rdr::crumbs::g_on\.store\(p\.crumbs, std::memory_order_relaxed\), rdr::crumbs::g_marker\.store\(p\.gpu_marker']),
    ('csv: header', 'src/frame_stats.h', [r'present_call_ms,record_thread_ms,"\s*\n\s*"device_lost[,"]']),
    ('csv: row', 'src/frame_stats.h', [r'record_text, lost_now \? 1 : 0, ', r'const bool lost_now = rdr::DeviceLost\(\);']),
    ('table: breadcrumbs', 'src/renderer_adapter/rdr_switches.h', [r'X\(Breadcrumbs, "wwe2k14_rdr_breadcrumbs", PerFrame, 1\)']),
    ('table: gpu marker', 'src/renderer_adapter/rdr_switches.h', [r'X\(GpuMarker, "wwe2k14_rdr_gpu_marker", PerFrame, 0\)']),
    ('table: message', 'src/renderer_adapter/rdr_switches.h', [r'X\(DeviceLostMessage, "wwe2k14_rdr_device_lost_message", PerFrame, 0\)']),
    ('cvars', 'src/renderer_adapter/reader.h', [r'REXCVAR_DEFINE_BOOL\(wwe2k14_rdr_breadcrumbs, true,', r'REXCVAR_DEFINE_INT32\(wwe2k14_rdr_gpu_marker, 0,',
                                                 r'REXCVAR_DEFINE_BOOL\(wwe2k14_rdr_device_lost_message, false,']),
    ('live reads the switches every frame', 'src/renderer_adapter/live.cpp', [r'render_globals\.crumbs = rdr_sw::BoolSeen\(rdr_sw::kBreadcrumbs,', r'render_globals\.gpu_marker = int\(rdr_sw::IntSeen\(rdr_sw::kGpuMarker,',
                                                                              r'if \(!split_frame\) rdr::crumbs::g_on\.store\(render_globals\.crumbs',
                                                                              r'rdr::crumbs::g_message\.store\(rdr_sw::BoolSeen\(rdr_sw::kDeviceLostMessage,']),
    ('frame-top barrier: table, cvar, read every frame, carried in the R21-B packet', 'src/renderer_adapter/rdr_switches.h', [r'X\(FrameTopBarrier, "wwe2k14_rdr_frame_top_barrier", PerFrame, 0\)']),
    ('frame-top barrier: cvar default 0', 'src/renderer_adapter/reader.h', [r'REXCVAR_DEFINE_INT32\(wwe2k14_rdr_frame_top_barrier, 0,']),
    ('frame-top barrier: live reads it every frame', 'src/renderer_adapter/live.cpp', [r'render_globals\.frame_top_barrier = int\(rdr_sw::IntSeen\(rdr_sw::kFrameTopBarrier,', r'rdr::crumbs::g_frame_top_barrier\.store\(p\.frame_top_barrier']),
    ('frame-top barrier: recorded at the top of the buffer', 'src/renderer/renderer.cpp', [r'frame_barrier_mode = crumbs::g_frame_top_barrier\.load\(std::memory_order_relaxed\);', r'RecordFrameBarrier\(slot\.cmd, "top"\)']),
    ('robust buffers (job C) reach the device', 'src/renderer/device.cpp', [r'enabled\.robustBufferAccess = options\.robust_buffers && available\.robustBufferAccess;']),
    ('robust buffers: cvar, table and start read', 'src/renderer_adapter/reader.h', [r'options\.robust_buffers = REXCVAR_GET\(wwe2k14_rdr_robust_buffers\);', r'REXCVAR_DEFINE_BOOL\(wwe2k14_rdr_robust_buffers, false,']),
    ('robust buffers: live -> renderer -> device', 'src/renderer_adapter/live.cpp', [r'ro\.robust_buffers = options\.robust_buffers;']),
    ('robust buffers: renderer -> device options', 'src/renderer/renderer.cpp', [r'device_options\.robust_buffers = opts\.robust_buffers;']),
    ('robust buffers: table', 'src/renderer_adapter/rdr_switches.h', [r'X\(RobustBuffers, "wwe2k14_rdr_robust_buffers", StartOnly, 0\)']),
    ('the renderer defaults match the table', 'src/renderer/breadcrumbs.h', [r'inline std::atomic<bool> g_on\{true\};', r'inline std::atomic<int> g_marker\{0\};', r'inline std::atomic<bool> g_message\{false\};']),
]


def link_problems(texts):
    out = []
    for name, rel, pats in LINKS:
        for p in pats:
            if not re.search(p, texts[rel]):
                out.append('%s: %s does not hold /%s/' % (name, rel, p))
    hdr = texts['src/frame_stats.h']
    i, j, k = hdr.find('present_call_ms'), hdr.find('"device_lost'), hdr.find('kColumnNames[i]')
    if not (0 <= i < j < k):
        out.append('csv: device_lost is not between present_call_ms and the fs_ split columns in frame_stats.h')
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    root = os.path.abspath(ap.parse_args().root)
    files = sorted({rel for _, rel, _ in LINKS})
    try:
        texts = {rel: read(root, rel) for rel in files}
    except OSError as e:
        say(False, 'cannot read: %s' % e)
        return 1
    probs = link_problems(texts)
    say(not probs, 'every link holds (%d links in %d files)%s' % (len(LINKS), len(files), ': ' + '; '.join(probs) if probs else ''))
    # Planted twins: each link's first pattern's text removed (its matched text replaced by a comment) must FAIL; a plant that matches nothing is DID NOT APPLY.
    for name, rel, pats in LINKS:
        m = re.search(pats[0], texts[rel])
        if not m:
            say(False, 'plant "%s": DID NOT APPLY (the text it edits is gone)' % name)
            continue
        planted = dict(texts)
        planted[rel] = texts[rel][:m.start()] + '/* planted: link removed */' + texts[rel][m.end():]
        if planted[rel] == texts[rel]:
            say(False, 'plant "%s": DID NOT APPLY' % name)
            continue
        say(bool(link_problems(planted)), 'plant "%s" (link removed) is caught' % name)
    # bench_line: the reader names the frame of the loss.
    sys.path.insert(0, os.path.join(root, 'tools', 'surface'))
    import bench_line
    cols = [c for c in bench_line.CSV_ORDER if not c.startswith('fs_')]
    say('device_lost' in cols and cols.index('device_lost') == cols.index('record_thread_ms') + 1, 'bench_line.CSV_ORDER has device_lost right after record_thread_ms (R21-B), before the fs_ columns')

    def synth(lost_from, with_col=True):
        use = cols if with_col else [c for c in cols if c != 'device_lost']
        buf = io.StringIO()
        w = csv.DictWriter(buf, fieldnames=use)
        w.writeheader()
        for f in range(3400, 3430):
            row = {c: '' for c in use}
            row.update(frame=str(f), t_seconds='%.3f' % (f / 60.0), frame_ms='16.7', presented='1')
            if with_col:
                row['device_lost'] = '1' if lost_from is not None and f >= lost_from else '0'
            w.writerow(row)
        return list(csv.DictReader(io.StringIO(buf.getvalue())))
    r = bench_line.device_lost_row(synth(3416))
    say(r is not None and r['frame'] == '3416', 'bench_line names the frame of the loss: %s' % (r['frame'] if r else None))
    say(bench_line.device_lost_row(synth(None)) is None, 'a clean run names no loss')
    say(bench_line.device_lost_row(synth(3416, with_col=False)) is None, 'an old csv without the column is read (no loss named, not refused)')
    src = read(root, 'tools/surface/bench_line.py')
    a, b = src.find('lost = device_lost_row(rows)'), src.find("if '[rdr][PROBLEM]' in line")
    say(0 <= a < b, 'read_step refuses on device_lost before the PROBLEM line (it names the frame)')
    print('device_lost_wiring: %s (%d failure%s)' % ('PASS' if not FAILS else 'FAIL', len(FAILS), '' if len(FAILS) == 1 else 's'), flush=True)
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
