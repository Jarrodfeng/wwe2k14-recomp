"""S108 lane MOB: build and run redact_unit.cpp. The real headers must pass; every planted copy (one rule removed or broken each) must FAIL, else the unit
cannot see the fault it exists for. Exit 0 = all as required.
    py -I tools\\report\\run_redact_unit.py [--root DIR] [--keep]
"""
import argparse, os, shutil, subprocess, sys, tempfile

ap = argparse.ArgumentParser()
ap.add_argument('--root', default=os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..')))
ap.add_argument('--keep', action='store_true')
a = ap.parse_args()
root = a.root
src = os.path.join(root, 'tools', 'report', 'redact_unit.cpp')
hdrs = {'report_redact.h': os.path.join(root, 'src', 'launcher'), 'cpu_block.h': os.path.join(root, 'src', 'launcher_mobile'),
        'cpu_report.h': os.path.join(root, 'src', 'launcher')}
for n, d in hdrs.items():
    if not os.path.isfile(os.path.join(d, n)):
        print('run_redact_unit: FAIL: missing', os.path.join(d, n))
        sys.exit(1)
clang = next((c for c in (r'C:\Program Files\LLVM\bin\clang++.exe', 'clang++') if c == 'clang++' or os.path.isfile(c)), 'clang++')
tmp = tempfile.mkdtemp(prefix='redact_unit_')


def build_run(tag, incs, big=True):
    exe = os.path.join(tmp, tag + '.exe')
    cmd = [clang, '-std=c++20', '-O2', '-Wall', '-Wextra', '-D_MSVC_STL_HARDENING=1'] + sum((['-I', i] for i in incs), []) + [src, '-o', exe]
    if sys.platform == 'win32':
        cmd += ['-lpsapi', '-D_ITERATOR_DEBUG_LEVEL=1']  # (S108 review: the checked STL: a read outside a string stops the unit)
    b = subprocess.run(cmd, capture_output=True, text=True)
    if b.returncode:
        print('run_redact_unit: FAIL: compile', tag, b.stderr[-1500:])
        sys.exit(1)
    if b.stderr.strip():
        print('run_redact_unit: warnings (' + tag + '):', b.stderr[-1500:])
    r = subprocess.run([exe, os.path.join(tmp, 'work_' + tag)] + ([] if big else ['nobig']), capture_output=True, text=True)
    return r.returncode, r.stdout


rc, out = build_run('real', [hdrs['report_redact.h'], hdrs['cpu_block.h']])
print(out.rstrip())
if rc != 0:
    print('run_redact_unit: FAIL: the real headers fail their own test')
    sys.exit(1)

PLANTS = {  # name -> (header, old, new)
    'no /data/user/ path rule': ('report_redact.h', '{"/data/user/", 4}, ', ''),
    'no /storage/ path rule': ('report_redact.h', '{"/storage/", 3},', ''),
    'no email pass': ('report_redact.h', 'if (r.email) t = detail::Emails(t);', ''),
    'no MAC pass': ('report_redact.h', 'if (r.macs) t = detail::Macs(t);', ''),
    'no IP pass': ('report_redact.h', 'if (r.ips) t = detail::Ips(t);', ''),
    'no URI pass': ('report_redact.h', 'if (r.uris) t = detail::Uris(t);', ''),
    'no UNC pass': ('report_redact.h', 'if (r.unc) t = detail::Unc(t);', ''),
    'no other-profile pass': ('report_redact.h', 'if (r.other_profiles) t = detail::OtherProfiles(t);', ''),
    'user name replaced by something else (PC old behaviour changed)': ('report_redact.h', 'append("USER");', 'append("U");'),
    'IPs also on the PC (driver versions eaten)': ('report_redact.h', 'r.other_profiles = r.unc = r.email = true;', 'r.other_profiles = r.unc = r.email = r.ips = true;'),
    'email pass eats file extensions': ('report_redact.h', 'FileExt(t.substr(last_dot + 1, e - last_dot - 1))', 'false'),
    'a folder path ending in / loses its folder name (review)': ('report_redact.h', "while (name_end > 1 && tok[name_end - 1] == '/') --name_end;", ''),
    'OtherProfiles reads before a text that starts with "users" (review)': ('report_redact.h', 'if (at < from || at == 0) continue;', 'if (at < from) continue;'),
    'binary pass writes the text replacement (the tombstone changes size; review D1)': (
        'report_redact.h', '  s.replace(x.b, len, f);  // f.size() == len', '  s.replace(x.b, len, x.rep);  // f.size() == len'),
    'binary pass without the protobuf walk (text runs only; review D1)': (
        'report_redact.h', '<= (size_t(8) << 20) && detail::PbFields(s, 0, s.size(), nullptr)) detail::PbMask', '<= (size_t(8) << 20) && false) detail::PbMask'),
    'binary runs use the strict path boundary (a path after a length byte stays; review of D1)': (
        'report_redact.h', 'MaskText(piece, r, true);', 'MaskText(piece, r, false);'),
    '.bin taken as a text type (review D1)': ('report_redact.h', '{".txt", ".json", ".jsonl"', '{".bin", ".txt", ".json", ".jsonl"'),
    'bare root keeps the user number': ('report_redact.h', '{"/mnt/user/", 3}', '{"/mnt/user/", 0}'),
    'whole-file-sized pieces (memory not bounded)': ('report_redact.h', 'constexpr size_t kBlock = size_t(1) << 20;', 'constexpr size_t kBlock = size_t(1) << 27;'),
    'a CPU serial line is read': ('cpu_block.h', 'key == "Hardware" && hardware.empty()', 'key == "Serial" && hardware.empty()'),
    'clusters not grouped': ('cpu_block.h', 'by[k].push_back(cpu);', 'by[k + unsigned(cpu)].push_back(cpu);'),
    'unreadable clock shown as 0.00 GHz': ('cpu_block.h', '(k ? ghz : "clock not readable")', 'ghz'),
    'no AVX2 in the missing list': ('cpu_report.h', 'need(c.avx2, "AVX2"), ', ''),
}
bad = 0
for name, (hdr, old, new) in PLANTS.items():
    d = os.path.join(tmp, 'plant')
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    for n, srcdir in hdrs.items():
        shutil.copy(os.path.join(srcdir, n), d)
    # cpu_report.h includes exe_pick.h from src\launcher
    shutil.copy(os.path.join(root, 'src', 'launcher', 'exe_pick.h'), d)
    p = os.path.join(d, hdr)
    text = open(p, encoding='utf-8', newline='').read()
    if old not in text:
        print('run_redact_unit: FAIL: plant "%s": text to replace not found in %s' % (name, hdr))
        bad += 1
        continue
    open(p, 'w', encoding='utf-8', newline='').write(text.replace(old, new, 1))
    rc2, out2 = build_run('plant', [d], big='memory' in name)
    if rc2 == 0:
        print('run_redact_unit: FAIL: planted fault NOT seen: ' + name)
        bad += 1
    else:
        print('run_redact_unit: planted fault seen: ' + name)
if not a.keep:
    shutil.rmtree(tmp, ignore_errors=True)
if bad:
    print('run_redact_unit: FAIL: %d plant(s) not seen' % bad)
    sys.exit(1)
print('run_redact_unit: OK (real headers pass, %d planted faults all fail)' % len(PLANTS))
