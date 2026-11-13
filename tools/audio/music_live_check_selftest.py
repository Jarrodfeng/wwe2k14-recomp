"""Offline tests of tools\\audio\\music_live_check.py (lane 303): a passing synthetic log, failing variants (each plants one known fault and must be
caught by the item that owns it), synthetic audio captures through ffmpeg's ebur128, and a lint of the route files. No game, no game files.
  py -I tools\\audio\\music_live_check.py --selftest
Prints one line per case; exit code 0 when every case behaved."""
import array, csv, json, math, os, random, re, shutil, struct, sys, tempfile, wave

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import music_live_check as C

FAILS = []


def ok(name, cond, extra=''):
    print('%s  %s%s' % ('ok  ' if cond else 'FAIL', name, ('  ' + extra) if (extra and not cond) else ''))
    if not cond:
        FAILS.append(name)


def stamp(n, msg, ms=0):
    return '[2026-11-13 20:%02d:%02d.%03d] [info] [core] [t1234] %s' % (n // 60 % 60, n % 60, ms, msg)


CACHE = lambda k: '%016x_tm175_v2.wav' % k


def good_log(mods, capture_frames=(600, 1800)):
    """The lines a healthy run prints, in the order the code prints them (formats copied from src\\*.h, lane 303 reading)."""
    L = []
    t = [0]

    def add(msg):
        t[0] += 1
        L.append(stamp(t[0], msg))
    add('[wwe2k14] modified: audio_mute = \'true\' (source: command line)')
    add('[wwe2k14] modified: window_activate_when_shown = \'false\' (source: command line)')
    add('[wwe2k14] modified: window_show_behind = \'true\' (source: command line)')
    add("[wwe2k14] modified: user_data_root = 'C:\\Users\\Jarrod\\wwe2k14-recomp\\out\\bench\\userdata_work_s1' (source: command line)")
    add("[wwe2k14] modified: wwe2k14_mods_dir = '%s' (source: command line)" % mods)
    add("[wwe2k14] modified: wwe2k14_settings_file = 'C:\\Users\\Jarrod\\wwe2k14-recomp\\out\\owned_check\\settings_native720.json' (source: command line)")
    add('[wwe2k14][audio_capture] armed: C:\\x\\out\\l303\\run\\a.wav (until guest frame 0)')
    add('[wwe2k14][input] line 3: l303: start music_live_check (frame 10)')
    add('[wwe2k14][music_slots] 2 song(s) in %s\\music\\new (2 id(s) in slots.txt)' % mods)
    add('[wwe2k14][music_slots] MUSIC list: 123 stock + 2 new (object 0x83A00000, records 0x40000000)')
    add('[wwe2k14][music] player registered with the SDK audio inject hook (48 kHz stereo, added to the game\'s FL / FR)')
    add("[wwe2k14][music] arena fx ready: chain from the sound bank (RoomVerb decay 2.30 s, pre-delay 36 ms, dry -3.3 dB, reverb -17.0 dB); gain: undo bus+master +11.00 dB, "
        "trim +0.0 dB => output +11.00 dB (the chain's own static net is -10.30 dB; a song goes through the same transform as a stock theme); tail 2.44 s; prepared in 63 ms off the guest threads")
    add('[wwe2k14][music] arena fx ON (entrance and preview songs get the stock room; the playlist stays dry)')
    add('[wwe2k14][input] line 40: l303: editor open (EASY CREATION list) (frame 900)')
    add("[wwe2k14][music] preview: song 6001 -> %s instead of the game's" % CACHE(0xAAAA))
    add('[wwe2k14][music] preview song 6001 started')
    add('[wwe2k14][music] preview: ours stops (another song)')
    add('[wwe2k14][music] preview song 6001 stopped')
    add("[wwe2k14][music_picker] SetSong(9998): M=40000000 name_ptr=40000010 'x' seq=25")
    add('[wwe2k14][user_playlist] 3 song(s) from %s\\music\\library' % mods)
    add('[wwe2k14][music_picker] MUSIC row selected: this=40001000 record=40002000 seq=25')
    add('[wwe2k14][music_picker] opened: 3 song(s), key \'*\', current \'\', sequence 25, caller lr 83353F34')
    add('[wwe2k14][music] playlist song 1 started')
    add('[wwe2k14][music] playlist song 2 started')
    add("[wwe2k14][music_picker] chose l303_lib_2.mp3 for key '*' (saved); keystroke reads while open: 7")
    add('[wwe2k14][input] line 90: l303: picker exit begin (frame 5000)')
    add("[wwe2k14][input] line 91: 'CREATE-AN-ENTRANCE' on screen 12 frames after press 1 ('CREATE-AN-ENTRANCE')")
    add('[wwe2k14][scenario] launch \'lesnar_bryan\': in-game sequence started frame=6000')
    add("[wwe2k14][music] event 'Play_MUS_0324_0_0' (play from 0x82AF1C84) frame %d" % capture_frames[0])
    add("[wwe2k14][music] event 'Play_MUS_0343_0_0' (play from 0x82AF1C84) frame %d" % capture_frames[1])
    add("[wwe2k14][music] theme 343 ('Play_MUS_0343_0_0') muted: %s instead" % CACHE(0xBBBB))
    add('[wwe2k14][music] entrance song 343 started')
    add('[wwe2k14][music] the match started: custom theme stops')
    add('[wwe2k14][music] entrance song 343 stopped')
    add('[wwe2k14][audio_capture] wrote 11250 audio frame(s), 2880000 samples (60.00 s) at guest frame 4000 -> C:\\x\\out\\l303\\run\\a.wav')
    return L


def make_capture(path, db_diff, secs=60, rate=48000, stock_at=(8, 28), custom_at=(30, 50)):
    """Stereo 48 kHz noise: a stock-level segment and a custom segment `db_diff` dB louder; stamps like src\\audio_capture.h (256-sample audio frames, guest
    frame = sample // 800 = 60 fps)."""
    rnd = random.Random(7)
    n = secs * rate
    base = 10 ** (-24.0 / 20)
    a_s, a_c = base, base * 10 ** (db_diff / 20)
    pcm = array.array('h')
    for i in range(n):
        t = i / rate
        a = a_s if stock_at[0] <= t < stock_at[1] else a_c if custom_at[0] <= t < custom_at[1] else base * 0.05
        v = int(max(-1, min(1, (rnd.random() - 0.5) * 2 * a * 1.7)) * 32767)
        pcm.append(v), pcm.append(v)
    with wave.open(path, 'wb') as w:
        w.setnchannels(2), w.setsampwidth(2), w.setframerate(rate)
        w.writeframes(pcm.tobytes())
    with open(path[:-4] + '.stamps.csv', 'w', newline='') as f:
        f.write('audio_frame,first_sample,guest_frame\n')
        for k in range(0, n // 256):
            f.write('%d,%d,%d\n' % (k, k * 256, (k * 256) // 800))


def status_of(res, item):
    for r in res:
        if r.item == item:
            return r.status
    return None


def run_text(lines, man, capture, ffmpeg):
    return C.run_all('\n'.join(lines) + '\n', man, capture, ffmpeg)


def mutate(lines, drop=None, sub=None):
    out = []
    for l in lines:
        if drop and re.search(drop, l):
            continue
        if sub:
            l = re.sub(sub[0], lambda m: sub[1], l)
        out.append(l)
    return out


def route_lint():
    """Every command of the routes is one src\\input_scripts.h knows, repeats are balanced, includes resolve, cvars and states exist."""
    known = set('wait_frames wait_file press press_until_file press_until_state press_until_text press_until_preview set screenshot screenshot_every clip_start clip_stop log '
                'renderdoc_capture mark cvar fail_if_file clear_fail_if repeat end_repeat log_text wait_text assert_text plugin dump_struct include watch_zero '
                'watch unwatch snap unsnap wait_state wait_event assert_state log_state label jump_if_preview_not on_fail quit check'.split())
    srcs = ''
    for fn in os.listdir(os.path.join(ROOT, 'src')):
        if fn.endswith(('.h', '.cpp')):
            srcs += open(os.path.join(ROOT, 'src', fn), encoding='utf-8', errors='replace').read()
    states = open(os.path.join(ROOT, 'tools', 'input_scripts', 'states.toml'), encoding='utf-8').read()
    scenes = os.path.join(ROOT, 'tools', 'input_scripts', 'scenes')
    nav = os.path.join(ROOT, 'tools', 'input_scripts', 'nav')
    probs = []
    files = ['music_live_check.txt', 'music_live_cae.txt', 'music_live_match.txt', 'music_live_cae_part.txt', 'music_live_match_part.txt']

    def resolve(name, base):
        for d in (base, nav, os.path.join(ROOT, 'tools', 'input_scripts')):
            for n in (name, name + '.txt'):
                if os.path.isfile(os.path.join(d, n)):
                    return os.path.join(d, n)
        return None

    def expand(path, depth=0):
        out = []
        for i, raw in enumerate(open(path, encoding='utf-8'), 1):
            line = raw.strip()
            if not line or line.startswith('#'):
                continue
            w = line.split()
            if w[0] == 'include':
                p = resolve(w[1], os.path.dirname(path))
                if not p:
                    probs.append('%s:%d include %s not found' % (os.path.basename(path), i, w[1]))
                elif depth > 6:
                    probs.append('include too deep')
                else:
                    out += expand(p, depth + 1)
                continue
            out.append((os.path.basename(path), i, w))
        return out
    for f in files:
        p = os.path.join(scenes, f)
        if not os.path.isfile(p):
            probs.append('missing ' + f)
            continue
        steps = expand(p)
        depth = 0
        for fn, i, w in steps:
            c = w[0]
            if c not in known:
                probs.append('%s:%d unknown command %s' % (fn, i, c))
            if c == 'repeat':
                depth += 1
            if c == 'end_repeat':
                depth -= 1
            if c == 'cvar':
                if not re.search(r'REXCVAR_DEFINE_\w+\(%s\b' % re.escape(w[1]), srcs):
                    probs.append('%s:%d cvar %s is not defined in src' % (fn, i, w[1]))
            if c in ('press_until_state', 'wait_state', 'assert_state', 'log_state'):
                name = w[2] if c == 'press_until_state' else w[1]
                if '[%s]' % name not in states:
                    probs.append('%s:%d state %s not in states.toml' % (fn, i, name))
            if c == 'press_until_state':
                nums = [x for x in w[4:] if x.isdigit()]
                if len(nums) >= 3 and not (0 < int(nums[0]) <= 100 and int(nums[1]) > int(nums[2]) > 0):
                    probs.append('%s:%d press_until_state numbers %s break tries<=100, interval>hold>0' % (fn, i, nums))
            if c == 'press_until_preview':
                ok_what = len(w) >= 6 and (w[2] in ('custom', 'any') or w[2].isdigit())
                if not (ok_what and w[3].isdigit() and w[4].isdigit() and w[5].isdigit() and 0 < int(w[3]) <= 400 and int(w[4]) > int(w[5]) > 0):
                    probs.append('%s:%d press_until_preview needs TARGETS WHAT TRIES(<=400) INTERVAL>HOLD HOLD' % (fn, i))
            if c == 'press_until_text' and not (len(w) >= 6 and w[2].isdigit() and w[3].isdigit() and w[4].isdigit() and int(w[3]) > int(w[4])):
                probs.append('%s:%d press_until_text needs TARGETS TRIES INTERVAL HOLD TEXT' % (fn, i))
        if depth != 0:
            probs.append('%s: repeat/end_repeat unbalanced' % f)
        head = open(p, encoding='utf-8').read().splitlines()[:40]
        if f in ('music_live_check.txt', 'music_live_cae.txt', 'music_live_match.txt'):
            fl = [l for l in head if l.startswith('# harness: flags')]
            if not fl:
                probs.append(f + ': no "# harness: flags" line')
            elif '--wwe2k14_launch=' in fl[0] or '--wwe2k14_launch ' in fl[0]:
                probs.append(f + ': the header must not pass --wwe2k14_launch (the route arms it after the editor)')
            if not any(l.startswith('# harness: realtime') for l in head):
                probs.append(f + ': not marked realtime')
            for flag in re.findall(r'--(\w+)=', fl[0] if fl else ''):
                if flag not in srcs and flag not in ('vulkan_stored_pipelines_at_boot',):
                    probs.append(f + ': flag --%s is not defined in src' % flag)
    return probs


def main():
    ffmpeg = shutil.which('ffmpeg')
    tmp = tempfile.mkdtemp(prefix='l303_selftest_')
    try:
        mods = os.path.join(tmp, 'out', 'l303', 'mods')
        os.makedirs(os.path.join(mods, 'music', 'library'))
        choices = os.path.join(mods, 'music', 'library', 'choices.txt')
        open(choices, 'w').write('*\tl303_lib_2.mp3\n')
        man = dict(theme_id=343, stock_control_id=324, new_ids=[6001, 6002], library_count=3, choices_file=choices)
        cap_ok = os.path.join(tmp, 'ok.wav')
        cap_loud = os.path.join(tmp, 'loud.wav')
        if ffmpeg:
            make_capture(cap_ok, 0.6)
            make_capture(cap_loud, 6.0)
        good = good_log(mods)

        # ---- the passing log
        r = run_text(good, man, cap_ok if ffmpeg else None, ffmpeg)
        bad = [x for x in r if x.status == 'FAIL']
        ok('good log: no FAIL (%d items)' % len(r), not bad and (ffmpeg is None or status_of(r, '3.3') == 'PASS'), '; '.join('%s %s' % (x.item, x.text[:80]) for x in bad))
        if ffmpeg:
            ok('good log: loudness difference measured and PASS', status_of(r, '3.3') == 'PASS', str([str(x) for x in r if x.item.startswith('3')]))
        for item in ('1.1', '1.2', '1.3', '1.5', '1.8', '2.1', '2.4', '2.5', '2.6', '2.8', '4.1', '4.3', '4.4'):
            ok('good log: %s PASS' % item, status_of(r, item) == 'PASS', str(status_of(r, item)))

        # ---- one planted fault each: the item that owns it must flag it (the known bugs of lanes 283 / 288 / 291 / 292 / 298 among them)
        def expect(name, lines, item, want, man2=man, cap=None):
            res = run_text(lines, man2, cap, ffmpeg)
            ok('%s -> %s %s' % (name, item, want), status_of(res, item) == want, 'got %s' % status_of(res, item))
        expect('mix-in player off (the old MFPlay path: no inject-hook line)', mutate(good, drop='player registered'), '1.1', 'FAIL')
        expect('lane 283 bug: the source plays as it is, not converted yet', mutate(good, sub=(r'muted: \w+_tm175_v2.wav', 'muted: 343.wav')), '1.2', 'FAIL')
        expect('no takeover line at all', mutate(good, drop=r'theme 343 \('), '1.2', 'FAIL')
        expect('player never opened the entrance file', mutate(good, drop=r'entrance song 343 started'), '1.3', 'FAIL')
        expect('"not converted yet" warning', good + [stamp(99, '[wwe2k14][music_slots] 343.wav is not converted yet - open the launcher to convert it (it plays as it is)')], '1.4', 'FAIL')
        expect('lane 298 bug: bus + master applied twice / not undone (output 0 dB)', mutate(good, sub=(r'=> output \+11\.00 dB', '=> output +0.00 dB')), '1.5', 'FAIL')
        expect('arena fx chain from built-in numbers', mutate(good, sub=(r'chain from the sound bank', 'chain from built-in numbers')), '1.5', 'WARN')
        expect('the stock control got replaced too', good + [stamp(98, "[wwe2k14][music] theme 324 ('Play_MUS_0324_0_0') muted: x instead")], '1.8', 'FAIL')
        expect('lane 281: SetSong is not the door (no preview line)', mutate(good, drop=r'preview: song'), '2.1', 'FAIL')
        expect('preview never stops', mutate(good, drop=r'preview: ours stops|preview song \d+ stopped'), '2.3', 'FAIL')
        expect('picker never opens', mutate(good, drop=r'music_picker\] opened'), '2.4', 'FAIL')
        expect('picker lists 2 of 3 library songs', mutate(good, sub=(r'opened: 3 song', 'opened: 2 song')), '2.4', 'FAIL')
        expect('PLAY did not preview', mutate(good, drop=r'playlist song'), '2.5', 'FAIL')
        expect('nothing chosen (closed, nothing changed)', mutate(good, sub=(r"chose .* for key.*", 'closed, nothing changed; keystroke reads while open: 3')), '2.6', 'FAIL')
        expect('save failed', mutate(good, sub=(r'\(saved\)', '(SAVE FAILED)')), '2.6', 'FAIL')
        open(choices, 'w').write('*\tother.mp3\n')
        expect('choices.txt does not hold the chosen file', good, '2.6', 'FAIL')
        open(choices, 'w').write('*\tl303_lib_2.mp3\n')
        expect('picker drew nothing for 2 s', good + [stamp(97, '[wwe2k14][music_picker] the picker never drew a frame in 2 s: the pad goes back to the game')], '2.7', 'FAIL')
        expect('lane 292 SF2: latch held the first B (press 3)', mutate(good, sub=(r'frames after press 1', 'frames after press 3')), '2.8', 'WARN')
        expect('B never left the editor', mutate(good, drop=r"'CREATE-AN-ENTRANCE' on screen"), '2.8', 'FAIL')
        expect('audible run', mutate(good, sub=(r"audio_mute = 'true'", "audio_mute = 'false'")), '4.1', 'FAIL')
        expect("Jarrod's real mods folder", mutate(good, sub=(r"wwe2k14_mods_dir = '[^']*'", "wwe2k14_mods_dir = 'C:\\Users\\Jarrod\\wwe2k14-recomp\\game_data\\mods'")), '4.3', 'FAIL')
        expect('real save', mutate(good, sub=(r"userdata_work_s1", 'AppData\\Roaming\\x')), '4.4', 'FAIL')

        # ---- loudness (needs ffmpeg)
        if ffmpeg:
            expect('custom song 6 dB louder than the stock theme', good, '3.3', 'FAIL', cap=cap_loud)
            expect('no capture', good, '3.1', 'FAIL', cap=os.path.join(tmp, 'missing.wav'))
            expect('no probe event for the custom theme', mutate(good, drop=r"event 'Play_MUS_0343"), '3.1', 'FAIL', cap=cap_ok)
            res = C.run_all('\n'.join(good) + '\n', man, cap_ok, None)
            ok('no ffmpeg -> SKIP, not a crash', status_of(res, '3.1') == 'SKIP', str(status_of(res, '3.1')))
            res = run_text(mutate(good, sub=(r'frame 1800', 'frame 90000')), man, cap_ok, ffmpeg)
            ok('a play frame past the capture end -> FAIL', status_of(res, '3.2') == 'FAIL', str(status_of(res, '3.2')))
        else:
            print('skip  loudness cases (no ffmpeg on PATH)')

        # ---- the driver: exit code through the real entry point on a log file
        lp = os.path.join(tmp, 'wwe2k14_1.log')
        open(lp, 'w').write('\n'.join(good) + '\n')
        mp = os.path.join(tmp, 'm.json')
        json.dump(dict(man, capture=cap_ok if ffmpeg else None), open(mp, 'w'))
        import subprocess
        pr = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'music_live_check.py'), '--log', lp, '--manifest', mp], capture_output=True, text=True)
        ok('CLI on the good log exits 0', pr.returncode == 0, pr.stdout[-600:] + pr.stderr[-300:])
        open(lp, 'w').write('\n'.join(mutate(good, drop='player registered')) + '\n')
        pr = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'music_live_check.py'), '--log', lp, '--manifest', mp], capture_output=True, text=True)
        ok('CLI on a failing log exits 1', pr.returncode == 1)

        # ---- the routes
        probs = route_lint()
        ok('route lint (%d problem(s))' % len(probs), not probs, '; '.join(probs))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print('\n%s' % ('ALL OK' if not FAILS else 'FAILED: ' + ', '.join(FAILS)))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
