#!/usr/bin/env python3
"""S102 TS1 offline test of the GPU-watchdog report reader. 0 game runs, 0 Surface runs, no ssh.

  py -I tools\\surface\\wer_reports_selftest.py [--root .]

1. wer_scan.ps1 (the script the Surface runs) against a FAKE WER layout on this PC: a Kernel_141 report whose folder write time is NEWER than its EventTime (the real
   boot-time requeue), an archive report, a stale one (older than -Since), a report folder without a dump, a Report.wer-less folder, a non-kernel folder (ignored), a dump in a
   fake LiveKernelReports (listed) and the same dump also in a report folder (listed once), and -Stage copying with -Max.
2. wer_report.py on fake job folders (logs with / without the `[rdr][DEVICE_LOST] =====` line): matched, a loss without a report, a report without a loss, nothing at all,
   a folder from before the listing existed, and a malformed reports.txt (must be refused, never read as "0 reports").
3. The planted faults: each wrong layout above must change the output (the checks fail when the reader is made blind).
"""
import argparse, os, shutil, subprocess, sys, tempfile
from datetime import datetime, timedelta
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import wer_report

FAILS = []


def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what)
    if not cond:
        FAILS.append(what)


def filetime(dt):          # naive local datetime -> FILETIME (the same conversion [datetime]::FromFileTime undoes)
    return int((dt.astimezone(__import__('datetime').timezone.utc) - datetime(1601, 1, 1, tzinfo=__import__('datetime').timezone.utc)).total_seconds() * 10_000_000)


def make_report(root, name, code, event, dump=None, wer=True, mtime=None):
    d = Path(root) / name
    d.mkdir(parents=True)
    if wer:
        (d / 'Report.wer').write_text(f'Version=1\nEventType=LiveKernelEvent\nEventTime={filetime(event)}\nSig[0].Name=Code\nSig[0].Value={code}\n', encoding='utf-8')
    if dump:
        (d / dump).write_bytes(b'DMP' * 100)
    if mtime:
        os.utime(d, (mtime.timestamp(), mtime.timestamp()))
    return d


def scan(args):
    r = subprocess.run(['pwsh', '-NoProfile', '-File', str(HERE / 'wer_scan.ps1')] + args, capture_output=True, text=True, timeout=120)
    return [l for l in r.stdout.splitlines() if l.strip()], r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.parse_args()
    now = datetime.now().replace(microsecond=0)
    tmp = Path(tempfile.mkdtemp(prefix='wer_selftest_'))
    try:
        q, ar, lkr = tmp / 'queue', tmp / 'archive', tmp / 'lkr'
        for p in (q, ar, lkr / 'WATCHDOG'):
            p.mkdir(parents=True)
        t_job = now - timedelta(minutes=30)
        # the folder write time is the REQUEUE time (newer), the EventTime is the real one
        make_report(q, 'Kernel_141_aaaa_00000000_11111111-0000', '141', now - timedelta(minutes=10), 'WATCHDOG-A.dmp', mtime=now)
        make_report(ar, 'Kernel_1a8_bbbb_00000000_22222222-0000', '1a8', now - timedelta(minutes=5), 'WATCHDOG-B.dmp')
        make_report(q, 'Kernel_141_cccc_00000000_33333333-0000', '141', now - timedelta(hours=5), 'WATCHDOG-OLD.dmp', mtime=now)   # stale by EventTime although the folder is new
        make_report(q, 'Kernel_117_dddd_00000000_44444444-0000', '117', now - timedelta(minutes=3), None)                            # no dump
        make_report(q, 'Kernel_141_eeee_00000000_55555555-0000', '141', now, 'WATCHDOG-NOWER.dmp', wer=False, mtime=now - timedelta(minutes=2))   # no Report.wer: folder time
        make_report(q, 'AppCrash_wwe2k14.exe_ffff', '0', now - timedelta(minutes=2), 'x.dmp')                                         # not a kernel report: ignored
        (lkr / 'WATCHDOG' / 'WATCHDOG-LKR.dmp').write_bytes(b'x' * 50)
        shutil.copy2(lkr / 'WATCHDOG' / 'WATCHDOG-LKR.dmp', lkr / 'WATCHDOG' / 'WATCHDOG-A.dmp')   # the same dump name in LiveKernelReports (the name carries the minute): listed once
        os.utime(lkr / 'WATCHDOG' / 'WATCHDOG-A.dmp', ((now - timedelta(minutes=10)).timestamp(),) * 2)

        fmt = '%Y-%m-%dT%H:%M:%S'
        stage = tmp / 'stage'
        lines, r = scan(['-Since', t_job.strftime(fmt), '-PcNow', now.strftime(fmt), '-Roots', f'{q},{ar}', '-Lkr', str(lkr), '-Stage', str(stage), '-Max', '2'])
        reps = [l for l in lines if l.startswith('REP|')]
        dumps = sorted(l.split('|')[3] for l in reps)
        check(r.returncode == 0 and lines and lines[0].startswith('NOW|'), 'scan runs and starts with the NOW line')
        check('WATCHDOG-OLD.dmp' not in dumps, 'a report older than -Since is not listed even when its folder is new (EventTime, not folder time)')
        check({'WATCHDOG-A.dmp', 'WATCHDOG-B.dmp', 'WATCHDOG-NOWER.dmp', '-', 'WATCHDOG-LKR.dmp'} <= set(dumps), f'the new reports (queue, archive, no-wer, no-dump, LiveKernelReports) are listed: {dumps}')
        check(dumps.count('WATCHDOG-A.dmp') == 1, 'the same dump in a report folder and in LiveKernelReports at the same time is listed once')
        check(not any('AppCrash' in l for l in reps), 'a non-kernel report folder is ignored')
        a = [l for l in reps if 'WATCHDOG-A.dmp' in l][0].split('|')
        check(a[1] == '141' and a[2] == (now - timedelta(minutes=10)).strftime(fmt), f'code 141 and EventTime read from Report.wer: {a[1]} {a[2]}')
        b = [l for l in reps if 'WATCHDOG-B.dmp' in l][0].split('|')
        check(b[1] == '1a8', 'code of a Kernel_1a8 report read from Sig[0]')
        check(any(l.startswith('WARN|no Report.wer') for l in lines), 'a folder without Report.wer is a WARN, still listed')
        staged = sorted(p.name for p in stage.glob('*')) if stage.exists() else []
        check(len(staged) == 2, f'-Max 2 copies two dumps and lists the rest ({len(staged)} copied, {len(reps)} listed)')
        # the clock offset: PcNow 1 h behind the machine's own clock moves the cut 1 h later (the Surface clock is ahead): the 10-min-old report is then older than the cut
        lines2, _ = scan(['-Since', t_job.strftime(fmt), '-PcNow', (now - timedelta(hours=1)).strftime(fmt), '-Roots', f'{q},{ar}', '-Lkr', str(lkr)])
        check(not any('WATCHDOG-A.dmp' in l for l in lines2), 'the Surface-vs-PC clock offset moves the cut (planted: PC 1 h behind)')

        # 2. wer_report.py on fake job folders
        def job(name, rep_lines, logs):
            j = tmp / name
            (j / 'watchdog').mkdir(parents=True)
            (j / 'x' / 'ab_1_A').mkdir(parents=True)
            if rep_lines is not None:
                (j / 'watchdog' / 'reports.txt').write_text('# job\n' + '\n'.join(rep_lines) + '\n', encoding='utf-8')
            else:
                shutil.rmtree(j / 'watchdog')
            for i, lg in enumerate(logs):
                (j / 'x' / 'ab_1_A' / f'wwe2k14_{i + 1}.log').write_text(lg, encoding='utf-8')
            return j

        def rep(code, dt, dump='WATCHDOG-X.dmp'):
            return f'REP|{code}|{dt.strftime(fmt)}|{dump}|Kernel_{code}_x|-'

        def lost(dt):
            return f'[{dt.strftime("%Y-%m-%d %H:%M:%S")}.361] [info] [core] [t1] [rdr][DEVICE_LOST] ===== the Vulkan device is LOST (vkQueueSubmit, VkResult -4)\n'
        t = now - timedelta(minutes=20)
        ok_job = job('ok', [rep('141', t + timedelta(seconds=13)), rep('141', t + timedelta(seconds=14), 'WATCHDOG-Y.dmp')], [lost(t)])
        s = wer_report.summary_line(ok_job / 'x' / 'ab_1_A')
        check(s and '2 reports (1 event; 141 x 2)' in s and 'DEVICE_LOST: 1, each with a report' in s and 'NO REPORT' not in s and 'WITHOUT' not in s, f'matched loss: {s}')
        s = wer_report.summary_line(ok_job)
        check(s and 'each with a report' in s, 'the job folder gives the same reading as a folder below it')
        # planted: the loss without a report, the report without a loss, a report at the wrong time
        s = wer_report.summary_line(job('noreport', [], [lost(t)]))
        check(s and '0 reports (listed)' in s and 'NO REPORT FOR 1' in s, f'PLANT loss without a report is flagged: {s}')
        s = wer_report.summary_line(job('nolost', [rep('141', t)], ['[2026-11-15 05:00:00.000] [info] nothing\n']))
        check(s and 'DEVICE_LOST: 0' in s and '1 EVENT WITHOUT A LOG LOSS' in s, f'PLANT report without a loss is flagged: {s}')
        s = wer_report.summary_line(job('far', [rep('141', t + timedelta(minutes=10))], [lost(t)]))
        check(s and 'NO REPORT FOR 1' in s and 'WITHOUT A LOG LOSS' in s, f'PLANT a report 10 min from the loss does not match: {s}')
        check(wer_report.summary_line(job('clean', [], ['[2026-11-15 05:00:00.000] [info] fine\n'])) == 'GPU watchdog: 0 reports (listed) | log DEVICE_LOST: 0', 'a clean job prints 0 reports (listed)')
        check(wer_report.summary_line(job('old', None, ['[2026-11-15 05:00:00.000] [info] fine\n'])) is None, 'a folder from before the listing, with no loss, prints nothing')
        s = wer_report.summary_line(job('old2', None, [lost(t)]))
        check(s and 'no listing' in s and 'DEVICE_LOST: 1' in s, f'before the listing existed, with a loss: {s}')
        try:
            wer_report.summary_line(job('bad', ['REP|141|2026-11-15T00:00:00|only|four'], []))
            check(False, 'PLANT a malformed REP line is refused')
        except ValueError:
            check(True, 'PLANT a malformed REP line is refused')
        # the real thing: the Surface's own listing format round-trips (REP lines the scan printed above)
        parsed = wer_report.parse_reports('\n'.join(reps))
        check(len(parsed) == len(reps) and all(p['time'] for p in parsed), 'the scan output parses back with parse_reports')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print('\n%s' % ('ALL OK' if not FAILS else '%d FAILED' % len(FAILS)))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
