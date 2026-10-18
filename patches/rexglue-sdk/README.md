# Our ReXGlue SDK changes

The game runs on our fork of the ReXGlue SDK: upstream https://github.com/rexglue/rexglue-sdk at commit `f5337cd`, plus
our changes on the local branch `wwe2k14-local` (in `thirdparty\rexglue-sdk-src`, which this repo doesn't track).

## `full\`: the complete, verified set (use this)

`full\01-sessions03-07c.diff` ... `full\14-session26b.diff`: one diff per session, from each session's last pinned SDK
build to the next (`tools\sdk_pins.json`). Applied in order on upstream `f5337cd`, they give **exactly** the SDK
the game is built with (`wwe2k14-local` 7412a80, pin-7412a80-1017-2115). Checked by
`py tools\sdk_patches\export_series.py --check`.

To rebuild our SDK from scratch:

```
git clone https://github.com/rexglue/rexglue-sdk rexglue-sdk-src
cd rexglue-sdk-src
git checkout f5337cd
git apply ..\wwe2k14-recomp\patches\rexglue-sdk\full\01-sessions03-07c.diff
```

(then the other diffs in order), then `tools\build_sdk.ps1`.

After a session that changes the SDK:
1. add a line to `STEPS` in `tools\sdk_patches\export_series.py` (the session's last pinned commit);
2. run it: it rewrites `full\` and must print PASS;
3. commit `full\`.

## `00NN-*.patch`: the historical per-session files

Kept because `tools\sdk_pins.json` and the session notes refer to them by name. They are **not** a complete series:
some are summaries, commits 87-103 (Session 15 / 15b) are in none of them, and the branch has 35 merge commits, which
per-commit patches can't carry. Applied in order they stop at 0015 (checked in Session 25).

Note: on Windows, the `thirdparty\libmspack` submodule shows 15 "modified" files. Upstream stores them as symlinks;
the checkout turns them into identical copies. That's not a code change (checked: all 15 equal their targets).
