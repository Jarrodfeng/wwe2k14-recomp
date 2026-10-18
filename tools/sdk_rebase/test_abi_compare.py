"""Offline check of sdk_rebase.abi_compare (Session 8, task 17): no build, no game.

    py tools\\sdk_rebase\\test_abi_compare.py

1. The installed SDK against itself: abi_same must be True.
2. The prebuilt upstream v0.10.0 zip install (thirdparty\\rexglue-sdk, no patches) against the installed patched SDK:
   our patches change SDK headers, so abi_same must be False with changed headers listed.
3. Synthetic trees whose files differ only in configure-time stamps (version tweak, timestamp, git hash) in the
   STAMP_FILES: abi_same True. The same plus one real #define change: False.
4. Copies of the real installed files with one edit each (the 2026-10-09 review's probes): every one must be a change
   (exported compile definitions / link libraries in rexglueTargets.cmake, share/rexglue/rex_app.cpp, a new or removed
   CMake config file, SDL3Config.cmake in the root cmake\\, a share\\cmake config, a non-stamp header with a VERSION
   number or a hex string, an upstream version bump in version.h). Edits that must NOT count: licenses\\ (not used by
   the game build), bin\\ (tested by regress/codegen), and an install-root path written into a config file.
"""
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sdk_rebase as s  # noqa: E402


class Quiet:
    def say(self, x=''):
        print('   ', x[:400])


def write(root, rel, text):
    p = os.path.join(root, *rel.split('/'))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def read(rel):
    with open(os.path.join(GAME, *rel.split('/')), encoding='utf-8', errors='replace') as f:
        return f.read()


GAME = s.game_sdk_root()


def main():
    ok = True
    r = s.abi_compare(GAME, GAME, Quiet())
    print('1 self-compare %s: abi_same=%s' % (GAME, r['abi_same']))
    ok &= r['abi_same'] is True
    pre = os.path.join(s.REPO, 'thirdparty', 'rexglue-sdk')
    if os.path.isdir(os.path.join(pre, 'include')):
        r = s.abi_compare(pre, GAME, Quiet())
        print('2 prebuilt upstream vs installed: abi_same=%s, changed headers %d (e.g. %s), added %d, removed %d' % (
            r['abi_same'], len(r['header_changed']), r['header_changed'][:3], len(r['header_added']),
            len(r['header_removed'])))
        ok &= r['abi_same'] is False and bool(r['header_changed'] or r['header_added'])
    # 3. Stamp-only differences (the lines the 2026-10-09 dry runs saw) vs a real change.
    old_h = ['#define REXGLUE_VERSION_MAJOR 0', '#define REXGLUE_VERSION_TWEAK 82',
             '#define REXGLUE_VERSION_STRING "0.10.0.82-dev.g728667b"', '#define REXGLUE_VERSION_NUMERIC "0.10.0.82"',
             '#define REXGLUE_BUILD_TIMESTAMP "20261009_0826"', '#define REXGLUE_MAX_THING 8']
    new_h = ['#define REXGLUE_VERSION_MAJOR 0', '#define REXGLUE_VERSION_TWEAK 36',
             '#define REXGLUE_VERSION_STRING "0.10.0.36-dev.g0b9a3f6"', '#define REXGLUE_VERSION_NUMERIC "0.10.0.36"',
             '#define REXGLUE_BUILD_TIMESTAMP "20261009_1028"', '#define REXGLUE_MAX_THING 8']
    old_sdl, new_sdl = '#define SDL_REVISION "SDL-3.5.0-8bf3b72"\n', '#define SDL_REVISION "SDL-3.5.0-HEAD-HASH-NOTFOUND"\n'
    old_c = 'set(PACKAGE_VERSION "0.10.0.82")\n  if("0.10.0.82" MATCHES "^([0-9]+)\\\\.")\n'
    new_c = 'set(PACKAGE_VERSION "0.10.0.36")\n  if("0.10.0.36" MATCHES "^([0-9]+)\\\\.")\n'
    for case, real_change in (('3a stamps only', False), ('3b stamps + a real #define change', True)):
        with tempfile.TemporaryDirectory() as td:
            for side, h, sdl, c in (('old', old_h, old_sdl, old_c), ('new', new_h, new_sdl, new_c)):
                if real_change and side == 'new':
                    h = h[:-1] + ['#define REXGLUE_MAX_THING 16']
                write(os.path.join(td, side), 'include/rex/version.h', '\n'.join(h) + '\n')
                write(os.path.join(td, side), 'include/SDL3/SDL_revision.h', sdl)
                write(os.path.join(td, side), 'lib/cmake/rexglue/rexglueConfigVersion.cmake', c)
            r = s.abi_compare(os.path.join(td, 'new'), os.path.join(td, 'old'), Quiet())
            want = not real_change
            print('%s: abi_same=%s (want %s), version-only %s, changed %s' % (
                case, r['abi_same'], want, r['header_version_only'] + r['config_version_only'], r['header_changed']))
            ok &= r['abi_same'] is want
    # 4. One edit each on copies of the real installed files.
    tg = 'lib/cmake/rexglue/rexglueTargets.cmake'
    targets = read(tg)
    app = read('share/rexglue/rex_app.cpp')
    sdl3 = read('cmake/SDL3Config.cmake')
    vma = 'share/cmake/VulkanMemoryAllocator/VulkanMemoryAllocatorConfig.cmake'
    vmac = read(vma)
    assert 'REX_HAS_VULKAN=1"' in targets and 'rex::spirv-tools-headers"' in targets
    base = {tg: targets, 'share/rexglue/rex_app.cpp': app, 'cmake/SDL3Config.cmake': sdl3, vma: vmac,
            'include/rex/a.h': '#define REX_CACHE_LAYOUT_VERSION 2\nconst char* k = "deadbeefcafe";\n',
            'include/rex/version.h': '\n'.join(old_h) + '\n', 'licenses/x/LICENSE.txt': 'MIT\n',
            'bin/rexglue.exe': 'binary'}
    cases = [  # (name, {rel: new text or None to delete}, want abi_same)
        ('4a INTERFACE_COMPILE_DEFINITIONS gains a define', {tg: targets.replace('REX_HAS_VULKAN=1"', 'REX_HAS_VULKAN=1;REX_NEW_LAYOUT=1"')}, False),
        ('4b INTERFACE_LINK_LIBRARIES gains a dependency', {tg: targets.replace('rex::spirv-tools-headers"', 'rex::spirv-tools-headers;rex::newdep"')}, False),
        ('4c share/rexglue/rex_app.cpp edited (compiled into the exe)', {'share/rexglue/rex_app.cpp': app + '\nint x = 1;\n'}, False),
        ('4d a new lib/cmake config file', {'lib/cmake/rexglue/rexglueExtra.cmake': 'set(X 1)\n'}, False),
        ('4e a lib/cmake config file removed', {tg: None}, False),
        ('4f root cmake/SDL3Config.cmake edited', {'cmake/SDL3Config.cmake': sdl3 + '\nset(SDL3_X 1)\n'}, False),
        ('4g share/cmake VMA config edited', {vma: vmac + '\nset(VMA_X 1)\n'}, False),
        ('4h non-stamp header: X_LAYOUT_VERSION 2 -> 3', {'include/rex/a.h': base['include/rex/a.h'].replace('VERSION 2', 'VERSION 3')}, False),
        ('4i non-stamp header: a hex string changes', {'include/rex/a.h': base['include/rex/a.h'].replace('deadbeefcafe', 'abcdef0123')}, False),
        ('4j version.h: upstream version bump 0.10.0 -> 0.11.0', {'include/rex/version.h': '\n'.join(new_h).replace('0.10.0', '0.11.0') + '\n'}, False),
        ('4k a share/rexglue source added', {'share/rexglue/new_main.cpp': 'int main();\n'}, False),
        ('4l licenses/ edited (not used by the game build)', {'licenses/x/LICENSE.txt': 'MIT (2027)\n'}, True),
        ('4m bin/ differs (tested by regress / codegen)', {'bin/rexglue.exe': 'other binary'}, True),
        ('4n config carries the install root path (path only)', {}, True),
    ]
    for name, edits, want in cases:
        with tempfile.TemporaryDirectory() as td:
            old, new = os.path.join(td, 'old'), os.path.join(td, 'sdk-wt', 'rebase-x', 'out', 'install', 'win-amd64')
            for rel, text in base.items():
                write(old, rel, text)
                write(new, rel, text)
            if name.startswith('4n'):
                write(old, 'lib/cmake/rexglue/paths.cmake', 'set(P "%s/lib")\n' % old.replace('\\', '/'))
                write(new, 'lib/cmake/rexglue/paths.cmake', 'set(P "%s/lib")\n' % new.replace('\\', '/'))
            for rel, text in edits.items():
                if text is None:
                    os.remove(os.path.join(new, *rel.split('/')))
                else:
                    write(new, rel, text)
            r = s.abi_compare(new, old, Quiet())
            good = r['abi_same'] is want
            ok &= good
            print('%-62s abi_same=%-5s want %-5s %s  %s' % (name, r['abi_same'], want, 'ok' if good else 'WRONG',
                                                             '; '.join(r['rebuild_reasons'])[:150]))
    print('TEST %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
