r"""S102 lane D1 round 7 (docs\renderer\DEVICE_LOST.md section 14): read a Windows WATCHDOG live-kernel minidump (LiveKernelEvent 0x141 / 0x117, the GPU
watchdog's engine-timeout dump) without a debugger. Offline; reads the Surface's dumps copied to out\s102\dumps (local only, never committed).
  py -I tools\renderer\watchdog_dump.py DUMP.dmp [DUMP.dmp ...] [--events N] [--selftest]
What it reads (all from the file, no symbols):
  - the kernel dump header (DUMP_HEADER64: 'PAGE' 'DU64', the bug check code and its four parameters, the dump type, the system time);
  - the triage block (TRIAGE_DUMP64 at 0x2000: the driver list in its string pool) for the modules named;
  - the secondary data ('DumpBlob' at the end of the triage dump): blob 270a33fd-3da6-460d-ba89-3c1bae21e39b is dxgkrnl's live-dump data; its section 0x01 is the
    display driver's own dump (on the Surface: the Intel KMD), which carries a text event log ("N:<seq>: TimeStamp=0x..: (dt) <event>") and per-engine ring
    states ("Ring_Gfxbase=..., CpuHead=..., Tailoffset=...", "BBLinAddr=..., ContextLinAddr=..., GpuCtxt=..."). The parser pulls the event log in sequence order
    and summarises: the last submissions (Submit3D: BB_Type OGL = our Vulkan / OpenGL UMD, DX* = D3D: DWM and the like) with their fence ids and process,
    the last completed fence, the preemption requests (ExecList_Preempt_Engine ... Ipehr), the semaphore waits, and the events after the last completion.
Bug check 0x141 VIDEO_ENGINE_TIMEOUT_DETECTED: P1 = the TDR recovery context, P2 = a pointer into the responsible driver module, P3 = an optional error code
(NTSTATUS of the last failed operation), P4 = optional context data. Exit 1 when a file cannot be read or the selftest fails."""
import argparse, datetime, re, struct, sys

BLOB_DXGK = bytes.fromhex('fd330a27a63d0d46ba893c1bae21e39b')   # 270a33fd-3da6-460d-ba89-3c1bae21e39b (little-endian GUID bytes)
EVENT = re.compile(rb'N:(\d+): TimeStamp=0x([0-9A-Fa-f]+) : \( *(\d+)\) ([\x20-\x7e]+)')


def filetime(v):
    return datetime.datetime(1601, 1, 1) + datetime.timedelta(microseconds=v // 10)


def header(d):
    if d[:8] != b'PAGEDU64':
        raise ValueError('not a 64-bit kernel dump (no PAGEDU64)')
    code, = struct.unpack_from('<I', d, 0x38)
    params = struct.unpack_from('<4Q', d, 0x40)
    dtype, = struct.unpack_from('<I', d, 0xF98)
    systime, = struct.unpack_from('<Q', d, 0xFA8)
    return {'code': code, 'params': params, 'type': dtype, 'time_utc': filetime(systime)}


def drivers(d):
    if struct.unpack_from('<I', d, 0xF98)[0] != 4:
        return []
    t = 0x2000
    sp_off, sp_size = struct.unpack_from('<II', d, t + 0x38)
    names = []
    p = sp_off
    while p + 4 < sp_off + sp_size:
        n, = struct.unpack_from('<I', d, p)
        if n == 0 or n > 260:
            break
        names.append(d[p + 4:p + 4 + 2 * n].decode('utf-16le', 'replace'))
        p = (p + 4 + 2 * n + 2 + 7) & ~7
    return names


def blobs(d):
    base = d.find(b'DumpBlob')
    if base < 0:
        return []
    hs, = struct.unpack_from('<I', d, base + 8)
    p, out = base + hs, []
    while p + 0x20 <= len(d):
        h, = struct.unpack_from('<I', d, p)
        if h != 0x20:
            break
        size, pre, post = struct.unpack_from('<III', d, p + 20)
        out.append((d[p + 4:p + 20], d[p + h + pre:p + h + pre + size]))
        p += h + pre + size + post
    return out


def kmd_section(blob):
    """dxgkrnl's live-dump blob: a section table (offset, id) after a 16-byte head; section 0x01 is the KMD's own dump."""
    count, = struct.unpack_from('<I', blob, 0)
    table = []
    for i in range(count):
        o = 16 + 8 * i
        if o + 8 > len(blob):
            break
        off, sid = struct.unpack_from('<II', blob, o)
        if off == 0 and sid == 0:
            break
        table.append((off, sid))
    table.sort()
    for i, (off, sid) in enumerate(table):
        if sid == 1:
            end = table[i + 1][0] if i + 1 < len(table) else len(blob)
            return blob[off:end], table
    return b'', table


def events(kmd):
    ev = {}
    for m in EVENT.finditer(kmd):
        seq = int(m.group(1))
        ev[seq] = (seq, int(m.group(2), 16), int(m.group(3)), m.group(4).decode().strip())
    return [ev[k] for k in sorted(ev)]


def summarise(evs, tail):
    out = []
    subs = [e for e in evs if 'Submit3D' in e[3] or 'SubmitVirt' in e[3]]
    comp = [e for e in evs if e[3].startswith('LastCompletedFenceID') or '3DInterrupt' in e[3]]
    pre = [e for e in evs if 'Preempt' in e[3]]
    if subs:
        last = subs[-1]
        out.append('last submission: seq %d: %s' % (last[0], last[3][:220]))
    if comp:
        out.append('last completion: seq %d: %s' % (comp[-1][0], comp[-1][3][:200]))
    if pre:
        out.append('last preemption event: seq %d: %s' % (pre[-1][0], pre[-1][3][:200]))
    out.append('the last %d events:' % tail)
    for e in evs[-tail:]:
        out.append('  %d  ts %08x  +%-6d %s' % (e[0], e[1], e[2], e[3][:230]))
    return out


def fault_row(evs):
    """The page fault the KMD logged, its fence's submission (resubmitted after a preemption or fresh), and the preemption before it."""
    pf = [e for e in evs if 'Page_Fault_interrupt' in e[3]]
    if not pf:
        return None
    m = re.search(r'Faulted_FenceID=(0x[0-9A-Fa-f]+), Faulted_Engine=(0x[0-9A-Fa-f]+), Faulted_Address=(0x[0-9A-Fa-f]+)', pf[-1][3])
    if not m:
        return None
    fence, engine, addr = m.groups()
    sv = [e for e in evs if 'SubmitVirt' in e[3] and 'FenceID=%s,' % fence in e[3]]
    s3 = [e for e in evs if 'Submit3D' in e[3] and 'FenceID=%s,' % fence in e[3]]
    pre = [e for e in evs if 'ExecList_Preempt_Engine' in e[3] and e[0] < pf[-1][0]]
    get = lambda pat, e: (re.search(pat, e[3]).group(1) if e and re.search(pat, e[3]) else '?')
    return {'fence': fence, 'engine': engine, 'address': addr,
            'resubmission': (get(r'Resubmission=(\d)', sv[-1]) if sv else '0 (no SubmitVirt line: in these logs SubmitVirt lines appear for presents and resubmissions only)'),
            'bb_type': get(r'BB_Type=(\w+)', s3[-1] if s3 else None), 'process': get(r'pProcess=(0x[0-9A-Fa-f]+)', s3[-1] if s3 else None),
            'context': get(r'LRCA_CtxtID=(0x[0-9A-Fa-f]+)', s3[-1] if s3 else None),
            'last_preempt_ipehr': get(r'Ipehr_Register_Value:(0x[0-9A-Fa-f]+)', pre[-1] if pre else None),
            'preempts': len(pre), 'resubmits': sum(1 for e in evs if e[3].startswith('Resubmit:'))}


def selftest():
    d = bytearray(0x3000)
    d[:8] = b'PAGEDU64'
    struct.pack_into('<I', d, 0x38, 0x141)
    struct.pack_into('<4Q', d, 0x40, 1, 2, 3, 4)
    struct.pack_into('<I', d, 0xF98, 1)
    h = header(bytes(d))
    kmd = b'xx N:7: TimeStamp=0x10 : (  3) Submit3D: BB_Type=OGL, FenceID=0x5, pProcess=0x1 \x00 N:8: TimeStamp=0x20 : ( 4) LastCompletedFenceID 0x4 FenceIDRingHead 0x1 \x00'
    ev = events(kmd)
    ok = h['code'] == 0x141 and h['params'] == (1, 2, 3, 4) and len(ev) == 2 and ev[0][0] == 7 and 'FenceID=0x5' in ev[0][3]
    print(('PASS' if ok else 'FAIL') + ' selftest: header fields and the KMD event log read back from a planted dump')
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dumps', nargs='*')
    ap.add_argument('--events', type=int, default=40)
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    rc = 0
    for path in a.dumps:
        try:
            d = open(path, 'rb').read()
            h = header(d)
        except Exception as e:
            print('FAIL %s: %s' % (path, e))
            rc = 1
            continue
        print('=== %s: bug check 0x%x, P1 %016x P2 %016x P3 %016x P4 %016x, dump type %d, %s UTC' % ((path, h['code']) + tuple(h['params']) + (h['type'], h['time_utc'])))
        drv = drivers(d)
        gfx = [n for n in drv if n.lower().startswith(('igd', 'dxg', 'watchdog', 'basicdisplay'))]
        print('  drivers in the triage list: %d (graphics: %s)' % (len(drv), ', '.join(gfx)))
        for guid, blob in blobs(d):
            if guid != BLOB_DXGK:
                continue
            kmd, table = kmd_section(blob)
            print('  dxgkrnl blob: %d bytes, sections %s; KMD section %d bytes' % (len(blob), ' '.join('%x@%x' % (s, o) for o, s in table), len(kmd)))
            ev = events(kmd)
            print('  KMD event log: %d events (seq %s..%s)' % (len(ev), ev[0][0] if ev else '-', ev[-1][0] if ev else '-'))
            f = fault_row(ev)
            if f:
                print('  PAGE FAULT: fence %(fence)s on engine %(engine)s (0 = render) at GPU VA %(address)s; its batch: %(bb_type)s, process %(process)s, context %(context)s, '
                      'resubmission after a preemption: %(resubmission)s; preemption requests in the log %(preempts)d (last IPEHR %(last_preempt_ipehr)s), resubmits %(resubmits)d' % f)
            for line in summarise(ev, a.events):
                print('  ' + line)
    return rc


if __name__ == '__main__':
    sys.exit(main())
