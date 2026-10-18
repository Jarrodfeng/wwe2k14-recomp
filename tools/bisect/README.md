# tools/bisect: which flag or which commit changes the outcome

Session 8, task 14. One command answers "which of these settings changes this screenshot / crashes / logs this line /
breaks this perf bar" or "which commit did". Every game launch goes through `tools\bench\run_list.ps1` (run lock,
frozen user data mirrored before each run, timeline on, a run still going after `--wait` seconds is killed).

```
py tools\bisect\autobisect.py presets
py tools\bisect\autobisect.py flags   --route ROUTE [--until LABEL] --check CHECK [--base "FLAGS"] (--preset NAME | --flags "FLAGS") [--strategy split|each] [--aa]
py tools\bisect\autobisect.py commits --repo ours|sdk --good OLD --bad NEW --route ROUTE [--until LABEL] --check CHECK [--base "FLAGS"]
```

Common options: `--dry-run` (print the plan, launch nothing), `--hold-lock` (hold the run lock for the whole bisect;
default: per probe, so other agents' runs can go in between), `--wait N` (run_list kill time, default 720 s),
`--no-reuse`, `--shot-mad X`, `--shot-off X`. Flag strings are ONE argument: `--base="--a=1 --b=2"` (with `=`, so
argparse does not read the value as an option).

Output: a few lines on the console; details in `out\harness\bisect\<stamp>\bisect.log` (every launch command and the
run_list output) and `result.json` (every probe: flags, run, outcome, detail, screenshot metrics). Everything is under
`out\harness\` (gitignored): routes, exe snapshots, worktrees, captures.

## Routes

`--route` is an input script (a name in `tools\input_scripts\` or a path). `--until LABEL` writes a copy that stops
after `screenshot LABEL` (then `wait_frames 60`, the PLAYBOOK rule for the async screenshot save, and `quit`) to
`out\harness\bisect\routes\`; a label inside a `repeat` ... `end_repeat` block is refused (the cut copy would not
load). Example: `determinism_lesnar.txt --until d_match0900` is Jarrod's Lesnar vs Bryan route
up to 900 frames into the match, about 3.5 minutes per probe.

## Checks (`--check`, one per bisect)

Each probe gives a yes/no outcome; "changed" means it differs from the reference probe's outcome.

| Check | Outcome | Notes |
|---|---|---|
| `crash` | the run did not end with `EXITED ..., exit code 0`, or its log has a hard-fail line | `bench_report.HARD_FAILS` (crash backtrace, device lost, script FAILED ...) |
| `log:REGEX` | REGEX (Python `re`) matches a line of the run's log | e.g. `log:modified: wwe2k14_fixed_seed = '1234'` |
| `shot:LABEL` | `captures\NNN_LABEL.bmp` differs from the reference probe's | different sizes: the larger is resized to the smaller (area average), as in the run-175 cross-resolution pre-check; changed if MAD > `--shot-mad` (2.0) or > `--shot-off` (0.5) % of pixels are off by > 32 in any channel. Run 175 (4K logo screens vs 720p, resampling only): MAD 0.8-1.1, <= 0.11 %. |
| `perf:SCENE.METRIC<X` / `>X` | regress.py's scene numbers (`brock`, `second`, `match`; `fps`, `n45`, `gpu_ms`, `pct20`, `min_full_window_fps`, `wall_s`) | bench routes only (anchors 343.bik2 / 103 or 324.bik2 / M3.PAC) |
| `perf:bar60` | 7b/7c's 60-fps bar fails on any scene | every 600-frame window >= 59.5 fps and < 0.5 % of frames > 20 ms |

A probe that cannot be judged (no run, missing screenshot, aspect differs) stops the bisect as INCONCLUSIVE with the
reason; with `crash` a crash is the outcome itself, including a game that died before it created its log (run_list
prints no `wwe2k14_NNN` but an `EXITED ... exit code <non-zero>` / `KILLED` / `STILL RUNNING` line, e.g. a DLL loader
failure 0xC0000139); only "nothing launched" stays unjudged. An invalid `log:` regex is refused up front. Screenshots of animated scenes need a deterministic moment: use
`--base="--wwe2k14_fixed_seed=1234"` and a match frame (docs/systems/determinism.md: same seed, match screenshots
pixel-identical or MAD <= 0.02), and run `--aa` once to measure the route's noise.

## Flags mode

`probe(S)` = the route with `--base` plus the flag subset S (a flag in S replaces the same flag in the base).

1. Reference: `probe({})`. With `--aa`, the reference again: if the two differ, the noise is above the threshold and
   nothing is bisected.
2. All candidates: must change the outcome, else there is nothing to find.
3. `--strategy split` (default): adaptive group testing. Both halves of a changing set are tested; each half that
   changes is split again, down to single flags. A set that changes while neither half does is reported as an
   INTERACTION (only the combination matters). Cost about 2 x (flags found) x log2(candidates) probes; worst case
   2n - 2. It assumes adding flags never hides another flag's effect (see demonstration 1 for a case where it does).
   `--strategy each`: every flag alone (n probes), no such assumption.
4. The flags not found are tested together on top of the base (from the cache when that exact set already ran): if
   they change the outcome too, the report says so (an effect that needs several of them).
5. `--minimize`: each INTERACTION set (and a "rest" that changes the outcome) is shrunk to a 1-minimal set by
   one-at-a-time removal (as in delta debugging; one probe per flag in the set), so the report names the flags the
   effect needs together instead of a whole half.

`--preset NAME` expands a preset into candidates, parsed from `src\wwe2k14_app.h` (OnPreSetup), so it follows the code:
`s6`, `s7` (each with a base flag `--wwe2k14_s6_defaults=false` / `--wwe2k14_s7_defaults=false`, so every component is
a real change), `1440p60`, `4k60`. `py tools\bisect\autobisect.py presets` lists them. Note that the 1440p/4K presets'
`gpu_upload_skip_unchanged` and `vulkan_texture_load_direct` are S7 defaults already: with the default base they are
no-ops; add `--wwe2k14_s7_defaults=false` to `--base` to test them for real. A `--flags` value overrides the preset
component of the same name (e.g. `--preset 4k60 --flags="--resolution_scale=2"`), with a NOTE in the output.

**Exe snapshot (flags mode):** the exe and its staged SDK DLLs (+ map/PDBs) are copied under the run lock to
`out\harness\bisect\exe\snap_<sha12>\` (sha of the copied exe + DLLs; the same build reuses its folder) and every probe
runs that copy, so another agent's build cannot change the exe between probes. Its runs have their own numbers and
`logs\` / `captures\` in that folder. `--no-snapshot` runs `--exe` in place. Probes of a non-default exe get
`--game_data_root=<repo>\game_data\extracted` (the app's upward search reaches only 6 folders up).

**Probe reuse:** `out\harness\bisect\probe_cache.json` maps (exe + staged rex*.dll path/size/mtime, route file
bytes, flag set, --wait) to a run. A later call (resume after a stop, other thresholds, another check on the same
runs) re-evaluates that run instead of launching it again. `--no-reuse` launches everything.

## Commits mode

First-parent commits after `--good` up to `--bad` (`git rev-list --first-parent`; on the SDK branch that is one
probe per merge). The reference is `--good`; `--bad` must differ; then a binary search for the first commit whose
outcome differs from `--good`. The route and check come from the main tree (the test stays fixed, the code varies).
Each commit is built by `tools\bisect\wt_build.ps1` under the run lock (no game run while it loads the CPU):

Build inputs are fixed at the start: `--exe`'s folder (default the main build) is snapshotted once under the run lock
(`out\harness\bisect\exe\snap_<sha12>\`, as in flags mode). For `ours` its rex*.dll must pass the SDK pin check
(`tools\build.ps1 -CheckOnly -ExeDir <snapshot> -SdkBin <snapshot>`), and those DLLs are staged into every commit's
build; for `sdk` its exe is the one copied for every commit. Later builds or SDK restages by other agents cannot give
two commits different inputs.

The worktree belongs to one bisect at a time: wt_build.ps1 writes `<worktree>.owner` (`pid=N`) and refuses (exit 7)
when another live process owns it; autobisect.py deletes it when it ends (a dead owner is taken over). Without this,
a second bisect could check out another commit in the same worktree between the first bisect's build and its run
(the lock is released in between unless `--hold-lock`).

A commit that does not build stops the bisect (INCONCLUSIVE, with the build log); there is no "skip" yet: narrow
`--good`/`--bad` around it. `--good` must be on `--bad`'s first-parent (mainline) history, else the tool refuses (a
side-branch commit would make the search non-monotonic).

- `--repo ours`: a git worktree of this repo at `out\harness\bisect\wt\ours` (detached; created on first use) with its
  own build folder. While a build runs, `game_data\extracted` inside it is a junction to the main one (codegen reads
  the XEX named in the manifest; nothing writes there); the junction is removed when the build ends, and game runs get
  `--game_data_root` instead. The worktree's codegen step runs on its first build (~90 s): ninja reruns a
  custom command with no `.ninja_log` entry in a fresh build folder, so copying the main `generated\` would not save
  it. After that, a checkout only touches files that differ, so codegen reruns only for commits whose manifest or
  `config\*.toml` differ, which is exactly when generated code can differ (task 2: codegen output is reproducible).
  The exe folder gets the snapshot's pinned SDK DLLs, so only our code differs between commits.
  `generated\rexglue.cmake` adds the generated sources only if `generated\default\sources.cmake` exists at configure
  time, so wt_build reconfigures when the build folder was configured without them, and reconfigures + builds again
  when a build's codegen wrote a new source list (without this the first build of a fresh worktree fails at link:
  seen on the first try, 08:53; the fresh-worktree branch was verified on a brand-new worktree at 09:42, see
  demonstration 3: ~250 s for that first build).
- `--repo sdk`: a worktree of `thirdparty\rexglue-sdk-src` at `thirdparty\sdk-wt\bisect`; each submodule folder is a
  junction to the main SDK tree's checkout, allowed only when the commit pins the same submodule commit (nothing is
  fetched; a submodule not initialized in the main tree is left empty; the junctions exist only during the build).
  Configured like the main SDK build: the worktree's own `win-amd64` preset (Ninja Multi-Config, clang/clang++,
  `-march=x86-64-v2`, C++23) plus build_sdk.ps1's Release flag strings plus every `REXGLUE_*` option of the main SDK
  cache (`thirdparty\rexglue-sdk-src\out\build\win-amd64\CMakeCache.txt`: `REXGLUE_USE_VULKAN=ON`, D3D12, Tracy,
  perf counters ...). The first version ran a plain `cmake -S . -B`, which picks the Visual Studio generator / MSVC,
  and the SDK stops with "ReXGlue requires the Clang compiler" (review finding). Built WITHOUT install (the install
  prefix is the shared SDK) into the worktree's `out\build\win-amd64`. Then
  `out\harness\bisect\exe\sdk_<commit9>\` gets a copy of the snapshot's exe and that build's `rexgpu-xenos` /
  `rexruntime` DLLs + PDBs. Those DLLs are unpinned (expected for bisect) and never go into the main exe folder. The
  exe is not rebuilt against the commit's headers: fine for wwe2k14-local commits that change SDK internals and
  cvars; a commit that changes an exported class layout makes the run crash at start.

Old builds and new flags: ReXGlue keeps an unknown `--flag` pending instead of failing (src/core/cvar.cpp), so a
base flag an old commit does not define is simply ignored there, and the app logs `modified: NAME = 'V' (source:
command line)` only where the setting exists. That makes "which commit added setting X" a `log:` check.

Offline self-test of the search logic (fake probes, no game): `py tools\bisect\test_autobisect.py` (one / two
culprits, AND interaction, nothing changes, A/A noise, the tool's own `commit_search` for every position with 1-9
commits plus an unjudgeable commit, preset override, crash without a log, bad regex, `--until` inside a repeat
block). Changing `hi = mid` to `lo = mid` in `commit_search` makes it fail (36 failures).

`--cached-only` launches nothing and judges only probes an earlier call already ran (e.g. the same bisect with other
thresholds); it stops at the first probe that would need a run.

## Thresholds: "looks different" vs "any change"

The default shot thresholds (MAD 2.0, 0.5 % of pixels off by > 32) are the run-175 "resampling only" level: they
answer "does it look different beyond resolution resampling". When the route's A/A is exactly 0 (fixed seed, match
frame), `--shot-mad 0.05 --shot-off 0.01` answers "does it change any pixel". Demonstration 1 shows the difference.

## Demonstration 1 (Session 8, 2026-10-09): which component of the 4K preset changes a match screenshot

```
py tools\bisect\autobisect.py flags --route determinism_lesnar.txt --until d_match0900 --base="--wwe2k14_fixed_seed=1234 --wwe2k14_s7_defaults=false" --preset 4k60 --check shot:d_match0900 --aa
```

Route: Jarrod's Lesnar vs Bryan route up to 900 frames into the match (~3.5 min per probe). Base: fixed seed, and
the S7 defaults off so the preset's upload skip and direct texture loads are real changes. Exe: snapshot
`snap_518d94c3204f` (the main build as of 08:08), runs 001-008 in its `logs\`. It ran as two calls: the first
(lock per probe, `--aa`) ran the reference and the A/A and was then stopped, because with 7 jobs queued each probe
waited ~15 min; the second (same command plus `--exe <snapshot exe> --no-snapshot --hold-lock`, no `--aa`) reused
run 001 from the probe cache and ran 003-008 back to back.

| Run | Flags on top of the base | vs reference 001 (downscaled when 4K) | Default thresholds |
|---|---|---|---|
| 001 | (reference) | | |
| 002 | (reference again, A/A) | MAD 0.000, max 0: pixel-identical | same |
| 003 | all five | MAD 6.72, 8.57 % off > 32 | changed |
| 004 | force_no_msaa + resolution_scale=3 | MAD 1.40, 0.24 % off > 32 | same (below the resampling level) |
| 005 | entrance_60fps + upload skip + direct load | MAD 5.64, 8.14 % off > 32 | changed |
| 006 | entrance_60fps | MAD 5.64, 8.14 % (identical numbers to 005) | changed |
| 007 | upload skip + direct load | MAD 0.000, max 0: pixel-identical | same |
| 008 | the other four together | MAD 1.40, 0.24 % (same as 004) | same |

Result (default thresholds, 6 game runs, 21 min holding the lock): **`--wwe2k14_entrance_60fps=true` alone changes
the match screenshot**; the other four together do not. Looking at the images (local only): with 60 fps entrances
the wrestlers and referee are in the same pose at match frame 900, but the crowd differs (other crowd poses, a sign
that is not in the reference). Why is unconfirmed (one guess, not tested: the crowd's animation state advances with
the entrance's frame count). The upload skip and direct texture loads change no pixel (as their verify runs said).
No MSAA + 4K differ from the reference (MAD 1.40, max 123) but stay under the "resampling only" level.

Second call, "any change" thresholds (A/A = 0), same snapshot, runs 001-008 reused from the probe cache:

```
py tools\bisect\autobisect.py flags --route determinism_lesnar.txt --until d_match0900 --base="--wwe2k14_fixed_seed=1234 --wwe2k14_s7_defaults=false" --preset 4k60 --check shot:d_match0900 --exe out\harness\bisect\exe\snap_518d94c3204f\wwe2k14.exe --no-snapshot --shot-mad 0.05 --shot-off 0.01 --hold-lock
```

| Run | Flags on top of the base | vs reference 001 |
|---|---|---|
| 009 | force_no_msaa | MAD 0.58, 0.55 % off > 32, max 126 (720p) |
| 010 | resolution_scale=3 | MAD 1.35, 0.16 % off > 32, max 123 (4K downscaled) |

Result (2 new game runs, 6.9 min): **`force_no_msaa`, `resolution_scale=3` and `entrance_60fps` each change the
match screenshot; the upload skip + direct texture loads change no pixel.** That is the expected answer for the
first two and the last two; entrance_60fps changing the match's crowd was not expected.

**Limitation found by this demo:** the split search assumes an effect never disappears when more flags are added.
That is false for thresholded image metrics: no MSAA alone puts 0.55 % of pixels off by > 32 (over the default 0.5 %),
but no MSAA + 4K together only 0.24 % (the 4K supersampling smooths the edges again), so with the default thresholds
the split never looked inside that pair and did not report `force_no_msaa`. When effects can cancel, use
`--strategy each` (every flag alone) or "any change" thresholds.

## Demonstration 3 (Session 8): which commit added the fixed-seed setting

```
py tools\bisect\autobisect.py commits --repo ours --good f5808a9 --bad 5d93fb2 --route selftest_title.txt --base="--wwe2k14_fixed_seed=1234" --check "log:modified: wwe2k14_fixed_seed = '1234'" --hold-lock
```

Three first-parent commits after f5808a9 (646b5cd Phase 2, e4c91b1 and 5d93fb2 docs only). Worktree
`out\harness\bisect\wt\ours`, exe runs in its `out\build\win-amd64-release\logs\`:

| Commit | Build (under the lock) | Run | Outcome |
|---|---|---|---|
| f5808a9 (good, reference) | reconfigure + codegen + full build, 89 s | 001, 42 s | no `modified: wwe2k14_fixed_seed` line (the old build keeps the unknown flag pending) |
| 5d93fb2 (bad) | codegen (manifest differs) + 8 steps, 104 s | 002, 42 s | line present: differs |
| 646b5cd (mid) | codegen, nothing relinked, 3 s | 002 reused (byte-same exe: same path/size/mtime) | line present: differs |

Result: **first commit that changes the outcome: 646b5cd** (Session 8 Phase 2, which added `src\determinism.h` with
`wwe2k14_fixed_seed`), which is right (`git grep fixed_seed f5808a9 -- src` finds nothing; 646b5cd's
`src\determinism.h` has `REXCVAR_DEFINE_INT64(wwe2k14_fixed_seed ...)`). 2 game runs, 4.7 min holding the lock. The main tree, its build
folder and exe folder were not touched. The first try (08:53) failed at link: a fresh worktree's first build ran
codegen but was configured without the generated sources; wt_build.ps1 now reconfigures (see Commits mode). That
09:01 rerun went through the "configured without them" branch (the failed try had left `generated\`), so the
fresh-worktree branch was still unverified then.

**Re-run after the review fixes (09:42-09:49, `out\harness\bisect\20261009_094158_22212`):** the old worktree was
deleted with `wt_build.ps1 -Clean -Repo ours` (1 junction removed as a link; `game_data\extracted` still 568 files,
7909 MB before and after), then the same command as above on a brand-new worktree. Exe snapshot
`snap_a67cb1d674f5`, pin check PASS (pin-728667b-1009-0826); every commit's exe folder got the snapshot's DLLs (same
MD5). f5808a9: configure, codegen, "codegen wrote a new generated source list: reconfigure + build again", build exit
0 in 248 s (the fresh-worktree branch, now verified), run 001 42 s, no line. 5d93fb2: 89 s, run 002 42 s, line
present. 646b5cd: 3 s (no relink), run 002 reused. Verdict again **646b5cd**. After each build the junction was
removed (`removed link ...game_data\extracted`), and the owner file was deleted when the bisect ended. A
`wt_build.ps1 -DryRun` and a `-Clean` started while the bisect ran were refused (exit 7, "in use: pid=22212 (live)").

**Deleting a bisect worktree: NEVER with `git worktree remove`.** The worktrees hold junctions into the main tree
during a build (`game_data\extracted`; the SDK submodule checkouts), and `git worktree remove --force` follows a
junction and deletes the target's contents (reproduced on Git 2.52.0.windows.1 with a dummy target: the target was
left empty). wt_build.ps1 now removes its junctions at the end of every build (and before every checkout), and the one
supported way to delete a worktree is:

```
pwsh -NoProfile -File tools\bisect\wt_build.ps1 -Clean -Repo ours
pwsh -NoProfile -File tools\bisect\wt_build.ps1 -Clean -Repo sdk
```

`-Clean` removes every reparse point in the tree as a link only (without descending into it), re-checks that none is
left, deletes the folder, then runs `git worktree prune`. It refuses while a live bisect owns the worktree.

`--repo sdk` was checked with a dry run (`wt_build.ps1 -Repo sdk -Commit wwe2k14-local~2 -DryRun`: worktree plan,
20 submodule junctions, catch2 and moltenvk left empty because they are not initialized in the main SDK tree
either) and, after the review fix, with a real configure only (09:50, under the run lock, env
`WT_BUILD_CONFIGURE_ONLY=1`, which stops wt_build after configure with exit 8): worktree at wwe2k14-local (728667b),
20 junctions, configure done in 76 s with Clang 23.1.2, Ninja Multi-Config, `-march=x86-64-v2`, Release
`-O3 -DNDEBUG -g -gcodeview`, `REXGLUE_USE_VULKAN=ON` (all REXGLUE_* options copied from the main cache); the 20
junctions were removed after it, and `-Clean -Repo sdk` deleted the worktree (the main SDK submodules: 18011 files
before and after; their `git status` unchanged). **No SDK worktree build and no DLL-swap game run were made: SDK
commit bisect beyond configure is still unverified.**

## Demonstration 2 (no new runs): an interaction, on the same runs

`--cached-only --check perf:bar60` on demonstration 1's runs: the reference fails the 60-fps bar (entrances at 30 fps,
brock min window 30.0), all five pass; no-MSAA + 4K alone still fail (30 fps entrances), and 60 fps entrances +
upload skip + direct load alone fail too (brock min window 42.15, 8.7 % of frames > 20 ms: with MSAA the entrance
does not hold 60, as Session 7 prep found). Neither half changes the outcome, so the report is
"only in combination: all five". `--minimize` would narrow it (5 runs, not made; expected from Session 7 prep, not
confirmed here: entrance_60fps + force_no_msaa). The same runs judged with `crash` (no crash anywhere: nothing to
bisect), `perf:match.fps<59.5` (match 59.99 in the reference and the all-five run: nothing to bisect) and `log:modified: resolution_scale = '3'` exercise the other
checks on real logs.
