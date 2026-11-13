"""Reads the log (+ audio capture) of the ONE combined custom-music live check (lane 303) and prints PASS / FAIL / WARN / SKIP per item with the
log line it used. Nothing here starts the game.

  py tools\\audio\\music_live_check.py --run N [--manifest out\\l303\\fixtures.json] [--capture a.wav] [--logs DIR]
  py tools\\audio\\music_live_check.py --log PATH --manifest out\\l303\\fixtures.json
  py tools\\audio\\music_live_check.py --selftest          offline tests: a passing synthetic log + failing variants + synthetic captures + route lint

The run: tools\\input_scripts\\scenes\\music_live_check.txt (docs\\systems\\music.md "Combined live check (lane 303)"). Items:
  1 ENTRANCE  a custom entrance theme (music\\343.wav, converted) plays at the entrance through the shared player (cache file, arena fx +11.00 dB,
              no "not converted yet"); the stock control (Bryan 324) is untouched
  2 CAE       the Create-an-Entrance preview of a custom song (lane 281), the USER PLAYLIST picker opens (ctx.lr), PLAY previews by path, choosing writes
              choices.txt, the picker closes and the pad latch is released
  3 LOUDNESS  the entrance segment of our song vs a stock theme, measured on the audio capture with ffmpeg ebur128 (bar: within 1.5 LU)
  4 HIDDEN    muted, behind, unfocused, test mods folder under out\\, frozen user data
Exit code 0 when no item FAILs (WARN / SKIP are listed, not failures); 1 otherwise."""
import csv, json, os, re, shutil, subprocess, sys, tempfile, wave

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
LOG_PREFIX = re.compile(r'^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\] \[(\w+)\] \[[^\]]*\] \[t\d+\] (.*)$')
CACHE_NAME = re.compile(r'^[0-9a-f]{16}_tm\d+_v\d+\.wav$')
LOUD_BAR_LU = 1.5
WINDOW_S = 15.0
SKIP_S = 1.0  # after the theme's play frame: the player's start-up and fade-in


class R:
    def __init__(self, item, status, text, line=''):
        self.item, self.status, self.text, self.line = item, status, text, line

    def __str__(self):
        s = '%-4s %-5s %s' % (self.status, self.item, self.text)
        if self.line:
            s += '\n           line: ' + self.line[:230]
        return s


class Log:
    def __init__(self, text):
        self.rows = []  # (index, timestamp, message)
        for i, raw in enumerate(text.splitlines()):
            m = LOG_PREFIX.match(raw.rstrip())
            self.rows.append((i + 1, m.group(1), m.group(3)) if m else (i + 1, '', raw.rstrip()))

    def find(self, pat, after=0, before=None):
        rx = re.compile(pat)
        for i, ts, msg in self.rows:
            if i <= after or (before is not None and i >= before):
                continue
            m = rx.search(msg)
            if m:
                return i, msg, m
        return None

    def all(self, pat, after=0, before=None):
        rx = re.compile(pat)
        out = []
        for i, ts, msg in self.rows:
            if i <= after or (before is not None and i >= before):
                continue
            m = rx.search(msg)
            if m:
                out.append((i, msg, m))
        return out


def line_of(hit):
    return 'L%d: %s' % (hit[0], hit[1]) if hit else ''


# ------------------------------------------------------------------------------------------------------------------------------ item 1
def check_entrance(log, man):
    out = []
    tid = man.get('theme_id', 343)
    stock = man.get('stock_control_id', 324)
    h = log.find(r'\[wwe2k14\]\[music\] player registered with the SDK audio inject hook')
    out.append(R('1.1', 'PASS' if h else 'FAIL', 'the shared player is registered with the SDK audio inject hook (--wwe2k14_music_mix_into_game)' if h else
                 'no "player registered with the SDK audio inject hook": the mix-in player never started (flag off, or no custom song asked for)', line_of(h)))
    muted = log.find(r"\[wwe2k14\]\[music\] theme %d \('Play_MUS_%04d_\d+_\d+'\) muted: (\S+) instead" % (tid, tid))
    if not muted:
        out.append(R('1.2', 'FAIL', 'no "theme %d muted: ... instead": the entrance hook did not take the theme over (no 343.* found / entrance_music off / not converted)' % tid))
    else:
        name = muted[2].group(1)
        ok = bool(CACHE_NAME.match(name))
        out.append(R('1.2', 'PASS' if ok else 'FAIL',
                     'theme %d is replaced by the CACHE copy %s (the importer\'s normalised file, found through index.txt)' % (tid, name) if ok else
                     'theme %d is replaced by %s, which is not a cache file name (HASH_tmNNN_vN.wav): the source plays as it is' % (tid, name), line_of(muted)))
    started = log.find(r'\[wwe2k14\]\[music\] entrance song %d started' % tid, after=muted[0] if muted else 0)
    out.append(R('1.3', 'PASS' if started else 'FAIL', 'the shared player started the entrance song %d (music_app Start -> Started)' % tid if started else
                 'no "entrance song %d started" after the theme was taken over: the player never opened the file' % tid, line_of(started)))
    bad = log.find(r'is not converted yet|cannot be played:|could not play |\b(entrance|preview|playlist) song \d+ failed')
    out.append(R('1.4', 'FAIL' if bad else 'PASS', 'a song was not played: ' + bad[1][:120] if bad else 'no "not converted yet" / "cannot be played" / "failed" for any song', line_of(bad)))
    fx = log.find(r'arena fx ready: chain from (.*?) \(RoomVerb')
    if not fx:
        out.append(R('1.5', 'FAIL', 'no "arena fx ready" line: the stock room (lane 298) was never prepared'))
    else:
        msg = fx[1]
        gain = re.search(r'=> output ([+-][\d.]+) dB', msg)
        src = fx[2].group(1)
        okgain = gain and abs(float(gain.group(1)) - 11.0) < 0.01
        if not okgain:
            out.append(R('1.5', 'FAIL', 'arena fx output gain is %s dB, expected +11.00 dB (undo bus -9 + master -2)' % (gain.group(1) if gain else '?'), line_of(fx)))
        elif src != 'the sound bank':
            out.append(R('1.5', 'WARN', 'arena fx ready with output +11.00 dB, but the chain came from %s, not the sound bank' % src, line_of(fx)))
        else:
            out.append(R('1.5', 'PASS', 'arena fx ready: chain from the sound bank, output +11.00 dB', line_of(fx)))
    on = log.find(r'arena fx ON')
    out.append(R('1.6', 'PASS' if on else 'WARN', 'arena fx ON published to the player' if on else 'no "arena fx ON" line (the chain may be published after the song started)', line_of(on)))
    end = log.find(r'\[wwe2k14\]\[music\] entrance song %d (stopped|ended)' % tid, after=started[0] if started else 0)
    out.append(R('1.7', 'PASS' if end else 'WARN', 'the entrance song ended: ' + end[1][-40:] if end else
                 'no "entrance song %d stopped/ended": the theme never stopped at the match start (the START skip may not have reached the match scene)' % tid, line_of(end)))
    ev = log.find(r"\[wwe2k14\]\[music\] event 'Play_MUS_%04d_\d+_\d+' \(play" % stock)
    ctl_muted = log.find(r"\[wwe2k14\]\[music\] theme %d \('Play_MUS_%04d" % (stock, stock))
    if ctl_muted:
        out.append(R('1.8', 'FAIL', 'the stock control theme %d was muted/replaced (it must stay the game\'s own)' % stock, line_of(ctl_muted)))
    elif ev:
        out.append(R('1.8', 'PASS', 'the stock control theme %d played as the game\'s own (event seen, never replaced)' % stock, line_of(ev)))
    else:
        out.append(R('1.8', 'WARN', 'no Play_MUS_%04d event seen: the control entrance never started (loudness item has no stock side)' % stock))
    return out


# ------------------------------------------------------------------------------------------------------------------------------ item 2
def check_cae(log, man):
    out = []
    ids = man.get('new_ids', [6001, 6002])
    pv = log.find(r"\[wwe2k14\]\[music\] preview: song (%s) -> (\S+) instead of the game's" % '|'.join(map(str, ids)))
    if not pv:
        out.append(R('2.1', 'FAIL', 'no "preview: song 600x -> ... instead of the game\'s": SetSong never saw a custom song (the route did not reach the custom rows, or SetSong is not the door: music.md lane 281 failure reading)'))
    else:
        name = pv[2].group(2)
        out.append(R('2.1', 'PASS' if CACHE_NAME.match(name) else 'FAIL',
                     'the editor preview takes custom song %s over with the cache copy %s' % (pv[2].group(1), name) if CACHE_NAME.match(name) else
                     'the preview plays %s, which is not a cache file' % name, line_of(pv)))
    ps = log.find(r'\[wwe2k14\]\[music\] preview song (%s) started' % '|'.join(map(str, ids)), after=pv[0] if pv else 0)
    out.append(R('2.2', 'PASS' if ps else 'FAIL', 'the shared player started the preview song' if ps else 'no "preview song 600x started" after the takeover (the player never opened it)', line_of(ps)))
    st = log.find(r'\[wwe2k14\]\[music\] preview: ours stops \(', after=pv[0] if pv else 0)
    st2 = log.find(r'\[wwe2k14\]\[music\] preview song \d+ stopped', after=ps[0] if ps else 0)
    out.append(R('2.3', 'PASS' if (st or st2) else 'FAIL', 'the preview stops when the cursor leaves the custom song' if (st or st2) else
                 'the preview never stopped after the cursor moved on (no "preview: ours stops" / "preview song N stopped")', line_of(st or st2)))
    op = log.find(r"\[wwe2k14\]\[music_picker\] opened: (\d+) song\(s\), key '(.*?)', current '(.*?)', sequence (\d+), caller lr ([0-9A-Fa-f]{8})")
    if not op:
        out.append(R('2.4', 'FAIL', 'the picker never opened on the USER PLAYLIST row (no "music_picker opened"): the hook on sub_82268C10 / text 50303 did not fire, or the route was not on that row'))
    else:
        n = int(op[2].group(1))
        want = man.get('library_count')
        okn = want is None or n == want
        out.append(R('2.4', 'PASS' if okn else 'FAIL', 'the picker opened with %d song(s)%s, ctx.lr = %s (the caller that triggered it: sub_83353F30 or the unconfirmed sub_8335F530 loc_8335F634), key \'%s\', sequence %s' %
                     (n, '' if want is None else ' (library has %d)' % want, op[2].group(5), op[2].group(2), op[2].group(4)) if okn else
                     'the picker lists %d song(s), the fixtures library has %d' % (n, want), line_of(op)))
    ch = log.find(r"\[wwe2k14\]\[music_picker\] chose (.+?) for key '(.*?)' \((saved|SAVE FAILED)\)", after=op[0] if op else 0)
    cl = log.find(r'\[wwe2k14\]\[music_picker\] closed, nothing changed', after=op[0] if op else 0)
    # PLAY on a row previews by path: a Playlist-kind song started between "opened" and the close
    pl_after = op[0] if op else 0
    pl_before = (ch or cl)[0] if (ch or cl) else None
    pl = log.all(r'\[wwe2k14\]\[music\] playlist song \d+ started', after=pl_after, before=pl_before)
    if not op:
        out.append(R('2.5', 'SKIP', 'picker never opened'))
    else:
        out.append(R('2.5', 'PASS' if pl else 'FAIL', '%d playlist song(s) started while the picker was open (PLAY on a row previews by path)%s' % (len(pl), '' if len(pl) >= 2 else ' - only one: the DOWN + second PLAY did not switch songs (WARN-level)')
                     if pl else 'no "playlist song N started" while the picker was open: PLAY (the X key) did not preview', line_of(pl[0] if pl else None)))
    if not ch and not cl:
        out.append(R('2.6', 'FAIL' if op else 'SKIP', 'the picker never closed (no "chose" / "closed" line)'))
    elif ch and ch[2].group(3) != 'saved':
        out.append(R('2.6', 'FAIL', 'the choice could not be saved', line_of(ch)))
    elif cl and not ch:
        out.append(R('2.6', 'FAIL', 'the picker closed with nothing chosen: the route\'s A did not choose (the pad edge, or the cursor)', line_of(cl)))
    else:
        detail = ''
        cf = man.get('choices_file')
        if cf:
            try:
                txt = open(cf, encoding='utf-8').read()
                lines = [l for l in txt.splitlines() if '\t' in l]
                f = ch[2].group(1)
                if not any(l.split('\t', 1)[1].strip() == f for l in lines):
                    out.append(R('2.6', 'FAIL', 'choices.txt has no line for %s: %r' % (f, txt[:200]), line_of(ch)))
                    ch = None
                else:
                    detail = ' and choices.txt holds it (%d line(s))' % len(lines)
            except OSError as e:
                out.append(R('2.6', 'FAIL', 'choices.txt unreadable: %s' % e, line_of(ch)))
                ch = None
        if ch:
            out.append(R('2.6', 'PASS', 'chose %s for key \'%s\' (saved)%s' % (ch[2].group(1), ch[2].group(2), detail), line_of(ch)))
    ill = log.find(r'\[music_picker\] (the picker never drew a frame|the dialog could not be opened|could not open)')
    out.append(R('2.7', 'FAIL' if ill else 'PASS', ill[1][:120] if ill else 'no "never drew a frame" / "dialog could not be opened" / "could not open"', line_of(ill)))
    # the pad latch: the first B after the close must be seen at once (route log "picker exit begin", then the input script's "on screen N frames after press K")
    ex = log.find(r'\[wwe2k14\]\[input\] line \d+: l303: picker exit begin')
    if not ex:
        out.append(R('2.8', 'SKIP', 'the route never reached "picker exit begin"'))
    else:
        lat = log.find(r"\[wwe2k14\]\[input\] line \d+: 'CREATE-AN-ENTRANCE' on screen (\d+) frames after press (\d+)", after=ex[0])
        again = log.all(r"\[wwe2k14\]\[input\] line \d+: 'CREATE-AN-ENTRANCE' not on screen within", after=ex[0])
        if lat and int(lat[2].group(2)) == 1:
            out.append(R('2.8', 'PASS', 'the first B after the picker closed went through (press 1, %s frames): the pad latch was released' % lat[2].group(1), line_of(lat)))
        elif lat:
            out.append(R('2.8', 'WARN', 'the editor left only on B press %s (%d "pressing again" warnings): the picker\'s latch or a SAVE prompt ate the first B(s)' % (lat[2].group(2), len(again)), line_of(lat)))
        else:
            out.append(R('2.8', 'FAIL', 'B never left the editor after the picker closed (%d "pressing again" warnings): the pad is still taken, or a prompt is up (read the log_text lines after the close)' % len(again)))
    tr = [m for pat in (r'MUSIC row selected', r'SetSong\(9998\)', r'USER PLAYLIST track') for m in [log.find(r'\[music_picker\] ' + pat)]]
    out.append(R('2.9', 'INFO', 'KEY trace lines (music.md "KEY: the one-run trace"): MUSIC row selected %s, SetSong(9998) %s, USER PLAYLIST track %s - copy them into the music.md KEY section' %
                 tuple('yes' if x else 'NO' for x in tr)))
    return out


# ------------------------------------------------------------------------------------------------------------------------------ item 3
def ffmpeg_lufs(ffmpeg, wav, start_s, length_s):
    p = subprocess.run([ffmpeg, '-hide_banner', '-nostats', '-ss', '%.3f' % start_s, '-t', '%.3f' % length_s, '-i', wav,
                        '-af', 'ebur128=peak=true', '-f', 'null', '-'], capture_output=True, text=True)
    t = p.stderr[p.stderr.rfind('Summary:'):]
    i = re.search(r'I:\s+(-?[\d.]+) LUFS', t)
    pk = re.search(r'Peak:\s+(-?[\d.]+) dBFS', t)
    return (float(i.group(1)) if i else None, float(pk.group(1)) if pk else None)


def start_time_of_frame(stamps_csv, guest_frame, rate):
    """Seconds into the capture at the first audio frame submitted at or after `guest_frame` (stamps: audio_frame,first_sample,guest_frame)."""
    with open(stamps_csv, newline='') as f:
        for row in csv.DictReader(f):
            if int(row['guest_frame']) >= guest_frame:
                return int(row['first_sample']) / float(rate)
    return None


def check_loudness(log, man, capture, ffmpeg):
    tid, stock = man.get('theme_id', 343), man.get('stock_control_id', 324)
    out = []
    frames = {}
    for pid in (tid, stock):
        e = log.find(r"\[wwe2k14\]\[music\] event 'Play_MUS_%04d_\d+_\d+' \(play from 0x[0-9A-Fa-f]+\) frame (\d+)" % pid)
        frames[pid] = (int(e[2].group(1)), e) if e else None
    if not capture or not os.path.isfile(capture):
        return [R('3.1', 'FAIL', 'no audio capture file (%s): pass --wwe2k14_audio_capture=FILE.wav (run_list -Extra) - the loudness item cannot be measured' % capture)]
    stamps = capture[:-4] + '.stamps.csv'
    if not os.path.isfile(stamps):
        return [R('3.1', 'FAIL', 'capture has no .stamps.csv next to it (%s)' % stamps)]
    if not ffmpeg:
        return [R('3.1', 'SKIP', 'ffmpeg not found (PATH / --ffmpeg): the windows below would be measured with its ebur128 filter')]
    if not frames[tid] or not frames[stock]:
        return [R('3.1', 'FAIL', 'the log has no Play_MUS_%04d / Play_MUS_%04d event frame (--wwe2k14_music_probe): cannot cut the windows' % (tid, stock))]
    try:
        with wave.open(capture, 'rb') as w:
            rate, secs = w.getframerate(), w.getnframes() / float(w.getframerate())
    except (wave.Error, EOFError) as e:
        return [R('3.1', 'FAIL', 'capture unreadable: %s' % e)]
    res = {}
    for pid, label in ((stock, 'stock theme %d' % stock), (tid, 'our song (theme %d)' % tid)):
        f, ev = frames[pid]
        t0 = start_time_of_frame(stamps, f, rate)
        if t0 is None or t0 + SKIP_S + 1 > secs:
            out.append(R('3.%d' % (1 if pid == stock else 2), 'FAIL', '%s: its play frame %d is not inside the capture (%.1f s long)' % (label, f, secs), line_of(ev)))
            continue
        length = min(WINDOW_S, secs - t0 - SKIP_S)
        lufs, pk = ffmpeg_lufs(ffmpeg, capture, t0 + SKIP_S, length)
        res[pid] = lufs
        if lufs is None or lufs < -60:
            out.append(R('3.%d' % (1 if pid == stock else 2), 'FAIL', '%s: %.1f s window from %.1f s is silent or unmeasurable (I = %s)' % (label, length, t0 + SKIP_S, lufs), line_of(ev)))
        else:
            out.append(R('3.%d' % (1 if pid == stock else 2), 'INFO', '%s: integrated %.1f LUFS over %.1f s from %.1f s into the capture (peak %s dBFS)' % (label, lufs, length, t0 + SKIP_S, pk), line_of(ev)))
    if stock in res and tid in res and res[stock] is not None and res[tid] is not None and res[stock] > -60 and res[tid] > -60:
        d = res[tid] - res[stock]
        status = 'PASS' if abs(d) <= LOUD_BAR_LU else ('WARN' if abs(d) <= 3.0 else 'FAIL')
        out.append(R('3.3', status, 'our entrance segment is %+.2f LU against the stock theme (bar: within %.1f LU). CAVEAT: both windows are the whole game mix of an entrance '
                     '(crowd, pyro, voice are in both); a small residual is the entrances\' own differences, not the songs\'. The songs alone: ours = -17.5 LUFS cache + the stock '
                     'room (lane 298, selftest-proved equal to the stock transform), stock = -17.6 LUFS median as heard (music.md lane 283).' % (d, LOUD_BAR_LU)))
    else:
        out.append(R('3.3', 'FAIL', 'the loudness difference could not be computed'))
    return out


# ------------------------------------------------------------------------------------------------------------------------------ item 4
def check_hidden(log, man):
    out = []

    def mod(name):
        return log.find(r"\[wwe2k14\] modified: %s = '(.*?)'" % re.escape(name))
    m = mod('audio_mute')
    out.append(R('4.1', 'PASS' if m and m[2].group(1) == 'true' else 'FAIL', 'audio_mute = true (the speakers stay silent; the capture is taken before the mute)' if m and m[2].group(1) == 'true' else
                 'audio_mute is not true: the run was audible (run_list adds it unless the caller passed audio_mute=false)', line_of(m)))
    b, a = mod('window_show_behind'), mod('window_activate_when_shown')
    ok = b and b[2].group(1) == 'true' and a and a[2].group(1) == 'false'
    out.append(R('4.2', 'PASS' if ok else 'WARN', 'window behind other windows and not activated' if ok else
                 'window_show_behind / window_activate_when_shown not set (-Visible / -Foreground / -Exclusive used?): the game window may have been on Jarrod\'s screen', line_of(b or a)))
    md = mod('wwe2k14_mods_dir')
    if not md:
        out.append(R('4.3', 'FAIL', 'no wwe2k14_mods_dir flag: the run used the default mods folder (Jarrod\'s real one)'))
    else:
        p = md[2].group(1).lower().replace('/', '\\')
        okp = '\\out\\' in p and 'game_data' not in p
        out.append(R('4.3', 'PASS' if okp else 'FAIL', 'mods folder = %s (a test folder under out\\)' % md[2].group(1) if okp else 'mods folder %s is not a test folder under out\\' % md[2].group(1), line_of(md)))
    ud = mod('user_data_root')
    okud = ud and re.search(r'out\\bench\\userdata_(work|frozen)', ud[2].group(1), re.I)
    out.append(R('4.4', 'PASS' if okud else 'FAIL', 'user data = the frozen mirror %s' % ud[2].group(1) if okud else 'user_data_root is not out\\bench\\userdata_work*: it may be Jarrod\'s real save', line_of(ud)))
    sf = mod('wwe2k14_settings_file')
    out.append(R('4.5', 'PASS' if sf else 'WARN', 'launcher settings = a test file %s' % (sf[2].group(1) if sf else '') if sf else
                 'no wwe2k14_settings_file: a release exe could read/write Jarrod\'s launcher settings (a +dev build never applies them)', line_of(sf)))
    ac = log.find(r'\[wwe2k14\]\[audio_capture\] (wrote \d+ audio frame\(s\), \d+ samples \(([\d.]+) s\)|first audio frame)')
    wr = log.find(r'\[wwe2k14\]\[audio_capture\] wrote \d+ audio frame')
    out.append(R('4.6', 'PASS' if wr else 'WARN', 'audio capture written' if wr else 'no "audio_capture wrote" line (the game was killed, or the capture flag is missing)', line_of(wr or ac)))
    return out


# ------------------------------------------------------------------------------------------------------------------------------ driver
def run_all(log_text, man, capture, ffmpeg):
    log = Log(log_text)
    res = check_entrance(log, man) + check_cae(log, man) + check_loudness(log, man, capture, ffmpeg) + check_hidden(log, man)
    return res


def summarize(res):
    counts = {}
    for r in res:
        counts[r.status] = counts.get(r.status, 0) + 1
    return counts


def find_log(run, logs_dir):
    cands = [os.path.join(d, 'wwe2k14_%s.log' % run) for d in ([logs_dir] if logs_dir else []) + [
        os.path.join(ROOT, 'out', 'build', 'win-amd64-release', 'logs'), os.path.join(ROOT, 'logs')]]
    for c in cands:
        if os.path.isfile(c):
            return c
    sys.exit('no log for run %s; tried:\n  ' % run + '\n  '.join(cands))


def main(argv):
    if '--selftest' in argv:
        sys.path.insert(0, HERE)
        import music_live_check_selftest as t
        sys.exit(t.main())
    opt = {'--run': None, '--log': None, '--manifest': None, '--capture': None, '--logs': None, '--ffmpeg': None}
    a = argv[:]
    while a:
        k = a.pop(0)
        if k not in opt or not a:
            sys.exit(__doc__)
        opt[k] = a.pop(0)
    if not opt['--run'] and not opt['--log']:
        sys.exit(__doc__)
    path = opt['--log'] or find_log(opt['--run'], opt['--logs'])
    man = json.load(open(opt['--manifest'])) if opt['--manifest'] else {}
    capture = opt['--capture'] or man.get('capture')
    ffmpeg = opt['--ffmpeg'] or shutil.which('ffmpeg')
    text = open(path, encoding='utf-8', errors='replace').read()
    print('music_live_check: %s' % path)
    res = run_all(text, man, capture, ffmpeg)
    for r in res:
        print(r)
    c = summarize(res)
    print('\nsummary: ' + ', '.join('%d %s' % (v, k) for k, v in sorted(c.items())))
    sys.exit(1 if c.get('FAIL') else 0)


if __name__ == '__main__':
    main(sys.argv[1:])
