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
    ('table: message', 'src/renderer_adapter/rdr_switches.h', [r'X\(DeviceLostMessage, "wwe2k14_rdr_device_lost_message", PerFrame, 1\)']),   # S105 A9: default ON
    ('cvars', 'src/renderer_adapter/reader.h', [r'REXCVAR_DEFINE_BOOL\(wwe2k14_rdr_breadcrumbs, true,', r'REXCVAR_DEFINE_INT32\(wwe2k14_rdr_gpu_marker, 0,',
                                                 r'REXCVAR_DEFINE_BOOL\(wwe2k14_rdr_device_lost_message, true,']),
    ('live reads the switches every frame', 'src/renderer_adapter/live.cpp', [r'render_globals\.crumbs = rdr_sw::BoolSeen\(rdr_sw::kBreadcrumbs,', r'render_globals\.gpu_marker = int\(rdr_sw::IntSeen\(rdr_sw::kGpuMarker,',
                                                                              r'if \(!split_frame\) rdr::crumbs::g_on\.store\(render_globals\.crumbs',
                                                                              r'rdr::crumbs::g_message\.store\(rdr_sw::BoolSeen\(rdr_sw::kDeviceLostMessage,']),
    ('frame-top barrier: table, cvar, read every frame, carried in the R21-B packet', 'src/renderer_adapter/rdr_switches.h', [r'X\(FrameTopBarrier, "wwe2k14_rdr_frame_top_barrier", PerFrame, 0\)']),
    ('frame-top barrier: cvar default 0', 'src/renderer_adapter/reader.h', [r'REXCVAR_DEFINE_INT32\(wwe2k14_rdr_frame_top_barrier, 0,']),
    ('frame-top barrier: live reads it every frame', 'src/renderer_adapter/live.cpp', [r'render_globals\.frame_top_barrier = int\(rdr_sw::IntSeen\(rdr_sw::kFrameTopBarrier,', r'rdr::crumbs::g_frame_top_barrier\.store\(p\.frame_top_barrier']),
    ('frame-top barrier: recorded at the top of the buffer', 'src/renderer/renderer.cpp', [r'frame_barrier_mode = crumbs::g_frame_top_barrier\.load\(std::memory_order_relaxed\);', r'RecordFrameBarrier\(slot\.cmd, "top"\)']),
    ('robust buffers (job C) reach the device', 'src/renderer/device.cpp', [r'enabled\.robustBufferAccess = options\.robust_buffers && available\.robustBufferAccess;']),
    ('robust buffers: cvar, table and start read', 'src/renderer_adapter/reader.h', [r'options\.robust_buffers = applied\.robust_buffers;', r'REXCVAR_DEFINE_BOOL\(wwe2k14_rdr_robust_buffers, false,']),
    ('robust buffers: the start reads the cvar into Applied', 'src/start_settings.cpp', [r'a\.robust_buffers = REXCVAR_GET\(wwe2k14_rdr_robust_buffers\);']),   # S103 SPB review L6
    ('robust buffers: live -> renderer -> device', 'src/renderer_adapter/live.cpp', [r'ro\.robust_buffers = options\.robust_buffers;']),
    ('robust buffers: renderer -> device options', 'src/renderer/renderer.cpp', [r'device_options\.robust_buffers = opts\.robust_buffers;']),
    ('robust buffers: table', 'src/renderer_adapter/rdr_switches.h', [r'X\(RobustBuffers, "wwe2k14_rdr_robust_buffers", StartOnly, 0\)']),
    # S105 A9 (docs/research/s105/A9.md): the player build is STREAM-FREE: reader.h / live.cpp are not compiled there, so the switch needs its own copy and OnSwap must read the rdr_sw table (both builds).
    ('A9: the stream-free build defines the switch (default true)', 'src/start_settings.cpp', [r'#if !WWE2K14_STREAM\(\)[\s\S]*REXCVAR_DEFINE_BOOL\(wwe2k14_rdr_device_lost_message, true,']),
    ('A9: OnSwap reads the table switch, not the stream-only mirror', 'src/frame_stats.h', [r'rdr_sw::Bool\(rdr_sw::kDeviceLostMessage\)', r'TellPlayerDeviceLost\(frame\);']),
    ('A9: the box text comes from device_lost_message.h; Android notes + asks for the launcher', 'src/frame_stats.h', [r'device_lost_message::kTextPc', r'WriteExitRequest\("the graphics driver stopped"\)', r'device_lost_message::BlackboxLine\(frame\)']),
    ('A9: the plant is stored only in a +dev build', 'src/start_settings.cpp', [r'if \(a\.dev\) rdr::crumbs::g_test_lost_at\.store\(REXCVAR_GET\(wwe2k14_rdr_test_device_lost\)']),
    ('A9: the plant takes the real lost path before the submit', 'src/renderer/renderer.cpp', [r'g_test_lost_at\.load\(std::memory_order_relaxed\); plant_at >= 0 && int64_t\(serial\) >= plant_at && !lost_serial\)\n\s*OnDeviceLost\("test plant"']),
    ('A9: no wait on a never-signalled slot fence after a real loss', 'src/renderer/renderer.cpp', [r'const bool lost_for_good = lost_serial && !lost_simulated;\n\s*if \(lost_for_good\) \{\n\s*if \(!idle_after_loss\) idle_after_loss = true, vkDeviceWaitIdle\(dev\);', r'waited = lost_for_good \? VK_SUCCESS : vkWaitForFences\(']),
    ('A9: the phone launcher turns the GPU LOST line into the phone words', 'src/launcher_mobile/phone_check.h', [r'line\.find\("GPU LOST: "\)', r'if \(gpu_lost\) return "The graphics driver stopped\.']),
    ('the renderer defaults match the table', 'src/renderer/breadcrumbs.h', [r'inline std::atomic<bool> g_on\{true\};', r'inline std::atomic<int> g_marker\{0\};', r'inline std::atomic<bool> g_message\{true\};']),
]


def link_problems(texts):
    out = []
    # S105 A9: the phone launcher's words are the header's kTextPhone, and its prefix is the one the game writes.
    h = texts['src/device_lost_message.h']
    m = re.search(r'kTextPhone =\s*"([^"]+)"', h)
    if not m or ('return "' + m.group(1) + '";') not in texts['src/launcher_mobile/phone_check.h']:
        out.append('A9: phone_check.h LastRun does not return device_lost_message.h kTextPhone')
    if 'kBlackboxPrefix = "GPU LOST: "' not in h:
        out.append('A9: device_lost_message.h kBlackboxPrefix is not "GPU LOST: "')
    for name, rel, pats in LINKS:
        for p in pats:
            if not re.search(p, texts[rel]):
                out.append('%s: %s does not hold /%s/' % (name, rel, p))
    rc = texts['src/renderer/renderer.cpp']
    # S105 A9 review F1: the plant is set at the top of Render, before the present step's `records = false` test and before the wait / submit code (a present on a never-signalled semaphore must not happen)
    a, b, c = rc.find('plant_at >= 0 && int64_t(serial) >= plant_at'), rc.find('const bool lost_for_good'), rc.find('if (lost_serial && !lost_simulated) records = false;')
    if not (0 <= a < b < c):
        out.append('A9 F1: the test plant is not set before lost_for_good and the present step in renderer.cpp Render')
    # S105 A9 review F2: every vkWaitForFences on slot.fence inside Render carries the real-loss guard (the fence is reset every frame after a loss but never submitted)
    lo = rc.find('const bool lost_for_good')
    hi = rc.find('vkWaitForFences(im.device.device()')
    for m_ in re.finditer(r'[^\n]*vkWaitForFences\(dev, 1, &slot\.fence[^\n]*', rc[lo:hi if hi > lo else len(rc)]):
        if 'lost_for_good ?' not in m_.group(0) and '!(lost_serial && !lost_simulated)' not in m_.group(0):
            out.append('A9 F2: an unguarded vkWaitForFences on slot.fence after the loss test: ' + m_.group(0).strip()[:90])
    if 'rex::FlushLogging();' not in texts['src/frame_stats.h'].split('WriteExitRequest("the graphics driver stopped")')[0][-400:]:
        out.append('A9 F4: frame_stats.h does not flush the log before the Android exit request')
    hdr = texts['src/frame_stats.h']
    i, j, k = hdr.find('present_call_ms'), hdr.find('"device_lost,present'), hdr.find('kColumnNames[i]')
    if not (0 <= i < j < k):
        out.append('csv: device_lost is not between present_call_ms and the fs_ split columns in frame_stats.h')
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    root = os.path.abspath(ap.parse_args().root)
    files = sorted({rel for _, rel, _ in LINKS} | {'src/device_lost_message.h'})
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
    # S105 A9 review F1 / F2 / F4: the extra checks of link_problems must FAIL on the old code (planted twins).
    rc = texts['src/renderer/renderer.cpp']
    marker = 'plant_at >= 0 && int64_t(serial) >= plant_at'
    old_f1 = dict(texts)   # F1 old code: the plant after the present step (the first review's finding)
    old_f1['src/renderer/renderer.cpp'] = rc.replace(marker, 'plant_at >= 1', 1).replace('if (lost_serial && !lost_simulated) records = false;', 'if (lost_serial && !lost_simulated) records = false; ' + marker, 1)
    say(any('A9 F1' in x for x in link_problems(old_f1)), 'plant F1 (the plant set after the present step) is caught')
    old_f2 = dict(texts)   # F2 old code: an unguarded fence wait on the slot's fence after the loss test
    old_f2['src/renderer/renderer.cpp'] = rc.replace('const bool lost_for_good', 'const bool lost_for_good_x = (vkWaitForFences(dev, 1, &slot.fence, VK_TRUE, UINT64_MAX), false); const bool lost_for_good', 1)
    say(any('A9 F2' in x for x in link_problems(old_f2)), 'plant F2 (an unguarded vkWaitForFences on slot.fence) is caught')
    old_f4 = dict(texts)
    old_f4['src/frame_stats.h'] = texts['src/frame_stats.h'].replace('rex::FlushLogging();  // (review F4)', '// (flush removed) (review F4)', 1)
    say(any('A9 F4' in x for x in link_problems(old_f4)), 'plant F4 (no log flush before the Android exit request) is caught')
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
