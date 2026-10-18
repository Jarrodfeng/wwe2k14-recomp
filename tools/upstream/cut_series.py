"""Session 15 (Track B): cut our ReXGlue SDK changes into a patch series for a possible upstream package.

PREP ONLY: writes files under patches\\upstream\\, never contacts anyone. Our own SDK code only (no game data).

  * One patch per first-parent step of `wwe2k14-local` since upstream f5337cd (a feature merge = its whole branch),
    numbered in history order, so the series applied in order to f5337cd gives exactly `wwe2k14-local`.
  * 0000-readback-eviction-fix.patch: the readback eviction fix alone (hunks picked out of the Sessions 3-5 commit
    285328a), against f5337cd, so it can be read and applied without the rest.
Each patch starts with a plain-text header (class, what it does or fixes, evidence, how it is turned on / off), taken
from FEATURES below (sources: docs\\upstream\\classification_draft.md and the PLAYBOOK). git apply skips text before the
first "diff --git" line.
Every patch is checked: the series in order against f5337cd (temporary index, no worktree), and the standalone
readback patch against f5337cd. The result is printed and written to patches\\upstream\\CHECK.txt.

Usage: py tools\\upstream\\cut_series.py
"""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SDK = REPO / "thirdparty" / "rexglue-sdk-src"
OUT = REPO / "patches" / "upstream"
BASE, HEAD = "f5337cd", "wwe2k14-local"

# slug -> (class, what, evidence, toggle). Classes: RGF runtime-generic fix, RGO runtime-generic optimisation (opt-in),
# DIAG diagnostics / tooling, 2K14 = only useful for this game, MIXED = several of these in one commit.
FEATURES = {
    "sessions3-5": ("MIXED (RGF + RGO + DIAG)",
                    "Sessions 3-5 in one commit: Vulkan GPU crash diagnostics (VK_EXT_device_fault + NV checkpoints), "
                    "the readback eviction fix (also cut alone as 0000), event timeline, WAIT_REG_MEM spin, async submit "
                    "thread, deferred descriptor writes, cheaper per-draw setting lookups.",
                    "PLAYBOOK Sessions 3-5: readback fix 5/5 crashes -> 0/2 (runs 020-026); spin: frames >= 45 ms "
                    "10.0% -> 0.1% (runs 062/063); async submit + deferred writes: Bryan entrance slow frames 133 -> 9 "
                    "(runs 066-073).",
                    "vulkan_gpu_crash_diagnostics, gpu_wait_reg_mem_spin, vulkan_async_submit, vulkan_submit_on_wait, "
                    "vulkan_deferred_descriptor_writes (all default off); the readback fix is always on "
                    "(vulkan_readback_evict_in_flight=true restores the old policy)."),
    "s6-counters": ("DIAG", "Per-frame counters in the timeline, frame anatomy CSV, GPU time per submission.",
                    "Measurement tool for Session 6 (PLAYBOOK Session 6).",
                    "gpu_anatomy_capture, gpu_anatomy_path, vulkan_gpu_timestamps (off)."),
    "s6-cp1": ("RGO", "Type-0 register packets through a bulk range path; GPU debug-log check cached.",
               "In-run A/B (CH103 heavy entrance): CP busy -4.8 +-1.3 ms/frame heavy, -1.73 +-0.26 match; _verify 0 "
               "mismatches (PLAYBOOK Session 6 table).", "gpu_type0_bulk_writes (+_verify), default off."),
    "s6-cp2": ("RGO", "SharedMemory::RequestRanges: no per-call heap allocation, lock-free all-valid pre-check.",
               "A/B: -3.8 +-1.0 ms/frame heavy (PLAYBOOK Session 6).", "gpu_request_ranges_fast (+_verify), off."),
    "s6-cp3": ("RGO", "Float-constant runs gathered; inline bit helpers.",
               "No separate A/B found (unconfirmed gain).", "vulkan_float_constant_runs (+_verify), off."),
    "s6-cp6": ("RGO", "Value-keyed sampler parameter cache.", "A/B: -2.5 +-2.0 ms/frame (PLAYBOOK Session 6).",
               "vulkan_sampler_param_cache (+_verify), off."),
    "s6-cp5": ("RGO", "Reuse texture / sampler descriptor sets across draws when unchanged.",
               "A/B: -2.2 +-1.3 ms/frame (PLAYBOOK Session 6).", "vulkan_reuse_texture_bindings (+_verify), off."),
    "s6-o1": ("RGO", "Precise guest vblank timer (high-resolution waitable timer + short spin) instead of Sleep(1).",
              "A/B ~0 on this PC (vblanks were ~1 ms late; PLAYBOOK Session 6).",
              "gpu_vblank_precise_timer, gpu_vblank_precise_timer_spin_us, off."),
    "s6-h1": ("RGO", "The command processor no longer waits for background pipeline compiles; a draw with no ready "
                     "pipeline holds its frame instead.",
              "Empty shader cache: hitches >= 100 ms 11 (worst 2.4 s) -> 2 (0.2 s) (PLAYBOOK Session 6).",
              "vulkan_pipeline_no_submit_wait (+_verify), vulkan_pipeline_sync_depth_only, off."),
    "s6-drain": ("RGO", "Swap submissions through the submit thread; the presenter waits on a queue-order ticket.",
                 "A/B: -1.6 +-1.2 ms/frame (PLAYBOOK Session 6).", "vulkan_async_swap, off (needs async submit)."),
    "s6-mergefix": ("RGO", "Build fix after the DRAIN merge (one scope for the COUNTERS processor number).", "-",
                    "-"),
    "s6-cp1b": ("RGO", "Split mixed type-0 register ranges; skip side-effect-free registers.",
                "A/B: -0.7 +-0.7 ms/frame (gain shown in combination only).", "gpu_type0_block_split (+_verify), off."),
    "s6-resid": ("RGO", "Switch off the stale-prone vertex residency cache (Canary dropped it); streaming copy for "
                        "large uploads.", "A/B 0 +-0.7 ms/frame (correctness-motivated).",
                 "vulkan_vertex_residency_cache_off, vulkan_vertex_residency_check, gpu_upload_streaming_copy, off."),
    "s6-viewport": ("RGO", "One-entry GetHostViewportInfo cache.", "A/B -0.5 +-0.5 ms/frame (in combination only).",
                    "vulkan_viewport_info_cache (+_verify), off."),
    "s6-cp7": ("RGO", "Constants as dynamic UBO offsets into one persistent descriptor set per pool page.",
               "A/B -0.4 +-0.3 ms/frame CP, -1.0 submit thread (PLAYBOOK Session 6).",
               "vulkan_constants_dynamic_offsets, _per_draw_sets (+_verify), off; needs a restart."),
    "s6-cp8": ("RGO", "Render-target cache: skip the FSI ownership update on a repeated no-op key.",
               "A/B -0.8 +-0.4 ms/frame (PLAYBOOK Session 6).", "gpu_rt_cache_skip_unchanged (+_verify), off."),
    "s6-cp9": ("RGO", "Skip unchanged render-state / texture parts of the system constants.",
               "A/B -0.7 +-0.5 ms/frame (in combination only).", "vulkan_system_constants_skip (+_verify), off."),
    "s6-debug-skip-vs": ("DIAG", "Skip draws by vertex-shader hash (to see what a shader draws).", "Tool.",
                         "gpu_debug_skip_vertex_shader (empty = off)."),
    "s7-stage0": ("DIAG", "Predicated-tiling counters and log.", "Tool (PLAYBOOK Session 7).",
                  "gpu_tiling_log_frames (0 = off)."),
    "s7-submitfix": ("RGF", "Failed async submissions / presenter fence signals can no longer hang a later wait "
                            "(retry with backoff, then signal).", "Review finding on our own async submit path "
                                                                 "(PLAYBOOK Session 7).", "Always on (error paths)."),
    "s7-texreq": ("RGO", "Skip per-draw texture request work when nothing changed.",
                  "Verify run 127: 0 mismatches; A/B run 128: -1.12 +-0.53 ms/frame heavy (docs\\session7_prep_notes.md).",
                  "gpu_request_textures_skip_unchanged (+_verify), off."),
    "s7-seams": ("RGF candidate", "Port of Xenia Canary #1257: negative host viewport origin for predicated tiling.",
                 "No seams seen in this title either way (run 126); unconfirmed whether it matters.",
                 "gpu_tiling_viewport_fix (+_stats), off."),
    "s7-protect": ("RGO", "Join VirtualProtect runs over already-protected pages.", "No gain measured (run 133).",
                   "gpu_protect_batch (+_verify), off."),
    "s7-extents": ("2K14", "CPU-computed screen extents for listed vertex shaders (the shader list is this game's).",
                   "Shadow mode ~21 ms/frame CPU: not viable as is (PLAYBOOK Session 7).",
                   "gpu_extents_shaders / _mode, off."),
    "s7b-upskip": ("RGO", "Upload skip: XXH3 per 4 KB page, skip re-uploads of bytes the GPU already holds.",
                   "Verify 0 mismatches (runs 152 / 159); GPU -4.6 ms/frame (run 158); "
                   "docs\\upstream\\gpu_upload_skip_unchanged.md.", "gpu_upload_skip_unchanged (+_stats, _verify), off."),
    "s7b-barnarrow": ("RGO", "Upload WAR barrier waits only for tracked stages.", "No effect measured (run 163).",
                      "vulkan_shared_memory_narrow_barrier, off."),
    "s7b-rebar": ("RGO", "Optional device-local upload / uniform pools; heap logging.",
                  "Not measured (no Resizable BAR on the test PC).",
                  "vulkan_upload_pool_device_local, vulkan_uniform_pool_device_local, off."),
    "s7b-gpumarks": ("DIAG", "Per-row GPU timestamps in the anatomy capture.", "Tool.", "gpu_anatomy_gpu_marks, off."),
    "s7c-causes": ("DIAG", "Stall attribution columns in the anatomy capture, cpf_sync counters.", "Tool.",
                   "Always on (counters)."),
    "s7c-noopskip": ("RGO", "Skip FSI draws that cannot change anything.", "Tiny effect (run 165).",
                     "gpu_skip_noop_draws (+_stats), off."),
    "s7c-texdirect": ("RGO", "At resolution_scale > 1, a compute shader writes scaled texture loads straight into the "
                             "image instead of a scratch buffer + copy.",
                      "Verify 0 mismatches in 31,400 / 45,626 loads (runs 166 / 172); 4K GPU -3.40 ms/frame (run 167); "
                      "docs\\upstream\\vulkan_texture_load_direct.md.",
                      "vulkan_texture_load_direct (+_verify), off."),
    "s7c-texload": ("RGO + DIAG", "Cheap fixes for loads still on the old path; per-load GPU marks / CSV.",
                    "Not A/B'd.", "vulkan_texture_load_discard_old, vulkan_texture_load_scratch_ring, off."),
    "s8-flight": ("DIAG", "Registry so the Vulkan command processor serves its newest crash-checkpoint records to an "
                          "app's flight recorder.", "Forced hang / crash dumps (run 297).",
                  "vulkan_flight_gpu_records, off."),
    "s14-vblank-rate": ("2K14 (generic mechanism)", "Feed the guest vblank at a chosen rate while vsync is on (slow "
                                                    "motion for titles that step a fixed amount per frame).",
                        "Runs 1059 / 1060: match clock follows the rate exactly.", "gpu_vblank_rate_override_hz (0 = off)."),
}

# First-parent commit -> slug (history order). Second merges of the same branch get "-2".
SLUGS = {
    "285328a": "sessions3-5", "9e64d4d": "s6-counters", "372a974": "s6-cp1", "791f0ad": "s6-cp2",
    "40047c0": "s6-cp3", "ef486c6": "s6-cp6", "eabf7d4": "s6-cp5", "f4e9218": "s6-o1", "7924ce3": "s6-h1",
    "024e85a": "s6-drain", "6b20481": "s6-mergefix", "5278d11": "s6-cp1b", "f43c78b": "s6-resid",
    "1056402": "s6-viewport", "3cf2fbf": "s6-cp7", "04de2f6": "s6-cp8", "aa80d42": "s6-cp9",
    "ef1b119": "s6-debug-skip-vs", "24e1042": "s7-stage0", "73c30d3": "s7-submitfix", "128e712": "s7-texreq",
    "8aa3cd6": "s7-seams", "d5afabf": "s7-stage0", "fdde345": "s7-submitfix", "9a6e6e8": "s7-seams",
    "ec65b10": "s7-protect", "36ac206": "s7-extents", "9820278": "s7b-upskip", "c3d7ca2": "s7b-barnarrow",
    "cd63556": "s7b-rebar", "7f8cedc": "s7b-gpumarks", "8b68939": "s7c-causes", "d9845ed": "s7c-noopskip",
    "b627cbc": "s7c-texdirect", "d251216": "s7c-texload", "728667b": "s8-flight", "32fae13": "s14-vblank-rate",
}

# The readback eviction fix inside 285328a: these hunks of src/graphics/vulkan/command_processor.cpp (matched by the
# old-file start line of the hunk header, f5337cd numbering).
READBACK_FILE = "src/graphics/vulkan/command_processor.cpp"
READBACK_HUNKS = {55, 4648, 4694, 4701, 4726}


def git(*args, env=None, inp=None):
    r = subprocess.run(["git", "-C", str(SDK), *args], capture_output=True, text=True, encoding="utf-8",
                       errors="replace", env=env, input=inp)
    if r.returncode != 0 and not args[0] == "apply":
        sys.exit(f"git {' '.join(args)} failed: {r.stderr}")
    return r


def header(slug, title, extra=""):
    cls, what, ev, tog = FEATURES[slug]
    return (f"Subject: [ReXGlue] {title}\n\nClass: {cls}\nWhat: {what}\nEvidence: {ev}\nOn / off: {tog}\n{extra}"
            f"Prepared by the WWE 2K14 recomp project (Session 15, prep only, not sent). Measured on one PC "
            f"(Ryzen 9 5950X, RTX 4090), Vulkan backend, one title.\n\n")


def readback_patch():
    diff = git("diff", BASE, "285328a", "--", READBACK_FILE).stdout
    head, _, rest = diff.partition("\n@@")
    hunks = ("@@" + rest).split("\n@@")
    keep = []
    for i, h in enumerate(hunks):
        h = h if i == 0 else "@@" + h
        m = re.match(r"@@ -(\d+)", h)
        if m and int(m.group(1)) in READBACK_HUNKS:
            keep.append(h.rstrip("\n"))
    if len(keep) != len(READBACK_HUNKS):
        sys.exit(f"readback: found {len(keep)} of {len(READBACK_HUNKS)} hunks")
    return head + "\n" + "\n".join(keep) + "\n"


def check(patches, label):
    """git apply --check of the patches in order on BASE, in a temporary index (no worktree touched)."""
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(tmp) / "index"))
        git("read-tree", BASE, env=env)
        lines = []
        for p in patches:
            r = git("apply", "--cached", str(p), env=env)
            lines.append(f"{label} {p.name}: {'OK' if r.returncode == 0 else 'FAILED ' + r.stderr.strip()[:300]}")
            if r.returncode != 0:
                break
        else:
            tree = git("write-tree", env=env).stdout.strip()
            want = git("rev-parse", f"{HEAD}^{{tree}}").stdout.strip()
            if label == "series":
                lines.append(f"series result tree {tree} {'== ' + HEAD if tree == want else '!= ' + HEAD + ' ' + want}")
        return lines


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for old in OUT.glob("*.patch"):
        old.unlink()
    commits = git("log", "--first-parent", "--reverse", "--format=%h", f"{BASE}..{HEAD}").stdout.split()
    seen, series = {}, []
    for n, c in enumerate(commits, 1):
        slug = SLUGS.get(c[:7])
        if not slug:
            sys.exit(f"no slug for {c}")
        seen[slug] = seen.get(slug, 0) + 1
        name = slug + (f"-{seen[slug]}" if seen[slug] > 1 else "")
        subject = git("log", "-1", "--format=%s", c).stdout.strip()
        diff = git("diff", "--binary", f"{c}^1", c).stdout
        path = OUT / f"{n:04d}-{name}.patch"
        path.write_text(header(slug, subject, f"Commit: {c} (diff against {c}^1)\n") + diff, encoding="utf-8",
                        newline="\n")
        series.append(path)
    rb = OUT / "0000-readback-eviction-fix.patch"
    rb.write_text(header("sessions3-5", "Vulkan: never evict a resolve readback buffer a queued copy still writes",
                         "Scope of this patch: only the readback eviction fix out of 285328a "
                         f"({READBACK_FILE}: the two cvars, the resolve path's submission_written record, and "
                         "EvictOldReadbackBuffers). Applies alone to f5337cd.\n")
                  + readback_patch(), encoding="utf-8", newline="\n")
    report = check([rb], "standalone") + check(series, "series")
    (OUT / "CHECK.txt").write_text("\n".join(report) + "\n", encoding="utf-8")
    print("\n".join(report))
    print(f"{len(series)} series patches + 0000 in {OUT}")


if __name__ == "__main__":
    main()
