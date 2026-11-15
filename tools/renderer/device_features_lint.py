r"""S102 lane G11 (review F1): a Vulkan device feature is enabled in src\renderer\device.cpp only from a short allow-list, or guarded by a DeviceOptions field. Offline.
  py -I tools\renderer\device_features_lint.py [--root DIR]
A line `enabled.X = available.X` (the device's own feature, enabled whenever it exists) must name X in ALLOWED or carry `options.` on the same line (the caller asked). The frame trace's
occlusionQueryPrecise was once enabled for every game run; the planted twin (that line back, unguarded) must FAIL, and it fails as DID NOT APPLY if the text it edits is gone.
Exit 1 on any failure."""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ALLOWED = {'depthClamp', 'samplerAnisotropy', 'textureCompressionBC', 'textureCompressionASTC_LDR'}
LINE = re.compile(r'^\s*enabled\.(\w+)\s*=\s*(.*?);')
FAILS = 0


def say(ok, text):
    global FAILS
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    FAILS += 0 if ok else 1


def problems(text):
    out = []
    n = 0
    for i, l in enumerate(text.splitlines(), 1):
        m = LINE.match(l)
        if not m:
            continue
        n += 1
        name, rhs = m.group(1), m.group(2)
        if 'available.' in rhs and 'options.' not in rhs and name not in ALLOWED:
            out.append('line %d: enabled.%s = %s is neither on the allow-list nor guarded by options.' % (i, name, rhs))
    if n == 0:
        out.append('no `enabled.X = ...` line found: the lint reads nothing')
    return out


STRUCT_DECL = re.compile(r'^\s*(VkPhysicalDevice\w*Features\w*)\s+(\w+)\s*\{', re.M)


def struct_problems(text):
    """S102 C1 review F2: a feature STRUCT (VkPhysicalDevice*Features*, in the pNext chain) is held to the same rule as the plain features: every assignment to a member of a struct named *_enabled must
    name `options.` on the same line (the caller asked), whatever the member; and a *_enabled struct must exist for the chain to carry one. The query structs (*_available) are not enabled."""
    out = []
    names = [m.group(2) for m in STRUCT_DECL.finditer(text) if m.group(2).endswith('_enabled')]
    for name in names:
        pat = re.compile(r'^\s*' + re.escape(name) + r'\.(\w+)\s*=\s*(.*?);', re.M)
        found = 0
        for m in pat.finditer(text):
            found += 1
            if 'options.' not in m.group(2):
                out.append('%s.%s = %s is not guarded by options.' % (name, m.group(1), m.group(2)))
        if not found:
            out.append('%s is declared but never assigned: the lint reads nothing' % name)
    return out, len(names)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    a = ap.parse_args()
    path = os.path.join(a.root, 'src', 'renderer', 'device.cpp')
    if not os.path.exists(path):
        print('FAIL cannot read', path)
        return 1
    text = open(path, encoding='utf-8').read()
    p = problems(text)
    say(not p, 'device.cpp: every enabled.X = available.X is allow-listed or guarded by options. (%s)' % ('; '.join(p) or 'clean'))
    guarded = re.search(r'enabled\.occlusionQueryPrecise\s*=\s*options\.occlusion_precise', text)
    planted = re.sub(r'enabled\.occlusionQueryPrecise\s*=\s*options\.occlusion_precise && ', 'enabled.occlusionQueryPrecise = ', text)
    if planted == text:
        say(False, 'planted twin DID NOT APPLY (the guarded occlusion line is not in device.cpp)')
    else:
        pp = problems(planted)
        say(bool(pp) and bool(guarded), 'planted twin (the occlusionQueryPrecise line back, unguarded): the lint FAILS it (%s)' % (pp[0] if pp else 'not caught'))
    sp, n = struct_problems(text)
    say(n > 0 and not sp, 'device.cpp: every member of a feature struct in the pNext chain (%d struct(s)) is guarded by options. (%s)' % (n, '; '.join(sp) or 'clean'))
    planted2 = text.replace('options.multiview && multiview_available.multiview', 'multiview_available.multiview', 1)
    if planted2 == text:
        say(False, 'planted twin DID NOT APPLY (the guarded multiview line is not in device.cpp)')
    else:
        sp2, _ = struct_problems(planted2)
        say(bool(sp2), 'planted twin (the multiview line unguarded): the lint FAILS it (%s)' % (sp2[0] if sp2 else 'not caught'))
    print('device_features_lint:', 'FAIL' if FAILS else 'PASS')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
