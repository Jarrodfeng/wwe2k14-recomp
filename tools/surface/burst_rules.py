r"""S102 B1: the burst-aware thermal verdict of a bench window (docs\research\part2\R28_power_budget.md, section 4).

The Surface Pro 6 opens every run in a power burst (cpu_perf about 145 %, 20-26 W, paid from an energy credit of 200-235 J that 50+ s of idle refills)
and falls to 13-14 W / 102-112 % after 10-40 s. heat_rules.py's "steady" cannot see this (288 of 304 R28 windows were "steady", 110 of them at 16.5-30 W).
This module classifies a WINDOW (the route's own marks) from power.csv, from 5 s before the window to its end:

  SUSTAINED  no sample in the window or the 5 s before is a burst sample (cpu_perf >= BURST_PERF or package > SAMPLE_W_MAX) AND the window means are
             inside the sustained band (package SUSTAINED_W_LO..SUSTAINED_W_HI, cpu_perf PERF_LO..PERF_HI). The number is a player's number.
  BURST      a sample inside the window is a burst sample: the number includes the burst (an upper bound, not the bar).
  MIXED      no burst sample in the window, but one in the 5 s before it (the burst had just ended: the clock may still be settling), or the window means sit
             above the sustained band (a warm chip that never reached the plateau).
  COOL       no burst sample, means under the band (a light scene, a chip that sagged below the plateau, or the load was not a bench): not the bar's state.
  NONE       no usable power samples in the window (missing columns, no rows).

Numbers (all from R28, one place, recalibrate there): the sustained plateau is 13.0-14.0 W / 102-112 %, sagging to 12.6-13.3 W / 102-106 % after
40-60 min (bar6 Bryan 13.0 W / 102 %, bar6 match 12.9 W / 104 %): hence a mean band of 12.5-14.6 W (R28's 13.0-14.6 with the sag; the per-sample
ceiling is 15.5 W, R28 4.1) and 100-115 %. Per-sample lower edges are not tested (a 1 Hz sample dips a watt in a sustained window).

normalised(fps, perf) = fps x (110 / perf)^0.5, R28's exponent (0.46-0.53 in its fits, ONE clean pair, unconfirmed as a law). Printed beside the raw fps, never instead.
"""
import csv
import math

PRE_S = 5.0            # seconds before the window whose samples must also be burst-free
BURST_PERF = 125.0     # a sample at or above this cpu_perf is a burst sample (R28: bursts run 130-148 %, sustained 102-115 %)
SAMPLE_W_MAX = 15.5    # a sample above this package power (W) is a burst sample (R28 4.1)
SUSTAINED_W_LO = 12.5  # window-mean package band (W): R28's plateau 13.0-14.6, with the 12.6-13.3 sag of 40-60 min runs
SUSTAINED_W_HI = 14.6
PERF_LO = 100.0        # window-mean cpu_perf band (%)
PERF_HI = 115.0
NORM_PERF = 110.0      # R28's reference clock for the normalised fps
NORM_EXP = 0.5         # R28 exponent, unconfirmed as a law
STATES = ('SUSTAINED', 'BURST', 'MIXED', 'COOL', 'NONE')


def _f(x):
    try:
        v = float(x)
    except (TypeError, ValueError):
        return None
    return v if math.isfinite(v) else None


def clock(h, m, s):
    return int(h) * 3600 + int(m) * 60 + float(s)


def read_power(path):
    """power.csv -> [(clock seconds, cpu_perf %, package W)] (a sample without both readings is dropped; milliwatts -> watts)."""
    out = []
    with open(path, newline='', encoding='utf-8', errors='replace') as f:
        for r in csv.DictReader(f):
            try:
                t = clock(*r['time'].split(':'))
            except (ValueError, TypeError, KeyError, AttributeError):
                continue
            p, w = _f(r.get('cpu_perf')), _f(r.get('rapl_pkg'))
            if p is None or w is None:
                continue
            out.append((t, p, w / 1000.0))
    return out


def is_burst(perf, w):
    return perf >= BURST_PERF or w > SAMPLE_W_MAX


def classify(samples, c0, c1):
    """samples: [(t, perf %, watts)]; window clock seconds c0..c1. -> dict(state, why, perf, w, perf_max, w_max, n, n_pre, n_burst, n_burst_pre)."""
    win = [s for s in samples if c0 <= s[0] <= c1]
    pre = [s for s in samples if c0 - PRE_S <= s[0] < c0]
    if not win:
        return dict(state='NONE', why='no power sample in the window', perf=None, w=None, perf_max=None, w_max=None, n=0, n_pre=len(pre), n_burst=0, n_burst_pre=0)
    perf = sum(s[1] for s in win) / len(win)
    w = sum(s[2] for s in win) / len(win)
    nb = sum(1 for s in win if is_burst(s[1], s[2]))
    nbp = sum(1 for s in pre if is_burst(s[1], s[2]))
    info = dict(perf=perf, w=w, perf_max=max(s[1] for s in win), w_max=max(s[2] for s in win), n=len(win), n_pre=len(pre), n_burst=nb, n_burst_pre=nbp)
    if nb:
        lead = 0   # contiguous burst samples from the window's start: the burst TAIL of a run (its credit ended inside the window)
        for s in win:
            if not is_burst(s[1], s[2]):
                break
            lead += 1
        st, why = 'BURST', f'{nb} of {len(win)} s at a burst clock or power (cpu_perf >= {BURST_PERF:g} % or > {SAMPLE_W_MAX:g} W)'
        if lead == nb and nb < len(win):
            why += f'; a burst tail: it ended {lead} s into the window, the other {len(win) - nb} s are below it'
        info['lead'] = lead
    elif nbp:
        st, why = 'MIXED', f'the burst ended inside the {PRE_S:g} s before the window ({nbp} of {len(pre)} s)'
    elif w > SUSTAINED_W_HI or perf > PERF_HI:
        st, why = 'MIXED', f'above the sustained band ({w:.1f} W / {perf:.0f} %; band {SUSTAINED_W_LO:g}-{SUSTAINED_W_HI:g} W, {PERF_LO:g}-{PERF_HI:g} %)'
    elif w < SUSTAINED_W_LO or perf < PERF_LO:
        st, why = 'COOL', f'below the sustained band ({w:.1f} W / {perf:.0f} %; band {SUSTAINED_W_LO:g}-{SUSTAINED_W_HI:g} W, {PERF_LO:g}-{PERF_HI:g} %)'
    else:
        st, why = 'SUSTAINED', f'no burst sample in the window or the {PRE_S:g} s before ({len(pre)} s seen)'
    info.update(state=st, why=why)
    return info


def normalised(fps, perf):
    """fps x (110 / perf)^0.5, None without a clock. R28's exponent: unconfirmed as a law."""
    if fps is None or not perf or perf <= 0:
        return None
    return fps * (NORM_PERF / perf) ** NORM_EXP


def text(info):
    """The suffix bench_line prints after the heat numbers: 'thermal SUSTAINED (why)'. No '|' and no '[' (ab_line's regex reads the heat part)."""
    return f'thermal {info["state"]} ({info["why"]}; max {info["perf_max"]:.0f} %, {info["w_max"]:.1f} W)' if info['perf_max'] is not None else f'thermal {info["state"]} ({info["why"]})'


def norm_text(fps, info):
    n = normalised(fps, info.get('perf'))
    return '' if n is None else f'normalised to {NORM_PERF:g} % {n:.1f} fps (R28 exponent {NORM_EXP:g}, unconfirmed as a law)'
