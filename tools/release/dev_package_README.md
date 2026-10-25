# WWE 2K14 recomp: developer package (Android port)

Made by `tools\release\make_dev_package.ps1` on {{DATE}} from `main` {{GAME_COMMIT}}. Patched ReXGlue SDK: our
branch `wwe2k14-local` at {{SDK_COMMIT}}, tree `{{SDK_TREE}}` (you rebuild it, section 3). Licence: BSD-3 (`LICENSE`).

## What is in here

- `wwe2k14-recomp\` - the project's source code (what GitHub gets, plus the Android port merged on 2026-10-25),
  `ANDROID_PLAN.md` (the port's plan, decisions and progress log: read it first) and
  `docs\research\android_port_s62.md` (how the port was approached).
- Our changes to the ReXGlue SDK (the recompiler and runtime) as a verified chain of diffs in
  `patches\rexglue-sdk\full\`, plus `tools\sdk_patches\apply_series.ps1`, which rebuilds the SDK from upstream.
- Not in here, on purpose: anything from the game (see the rules below), build output, Jarrod's working notes.

## Rules (the project's hard rules; please keep them)

1. **Bring your own copy of the game.** You need your own Xbox 360 WWE 2K14 disc, dumped yourself. Never share game
   files, the `.xex`, the generated C++ (`generated\`), screenshots of raw asset dumps, or APKs / exes built from them
   with anyone (whether a built APK may be shared at all is still an open question for Jarrod).
2. **Our own code only.** Other ReXGlue Android ports (for example the WWE '12 Android alpha) are GPL-3.0; this project
   is BSD-3. Do not open or copy their source while working on this one.
3. Non-commercial: no paid builds, donations or Patreon tied to the project.

## 1. Install the tools (each from its official source; licences do not allow us to bundle them)

| tool | version we build with | where |
|---|---|---|
| Windows 10 / 11 x64 | - | |
| Visual Studio 2022 (Community is fine), workload "Desktop development with C++" | 17.x (brings MSVC libs, Windows SDK, CMake, Ninja) | https://visualstudio.microsoft.com/vs/ |
| LLVM / Clang, installed to `C:\Program Files\LLVM` | 20 or newer (we use 23.1.2); VS's bundled Clang 19 is too old | https://github.com/llvm/llvm-project/releases |
| Python | 3.12, launcher `py` on PATH | https://www.python.org/downloads/ |
| Git | any recent | https://git-scm.com/ |
| Android command-line tools, then with `sdkmanager`: `platform-tools`, `build-tools;36.0.0`, `platforms;android-36`, `ndk;29.0.14206865` | SDK in `%LOCALAPPDATA%\Android\Sdk` (Android Studio's default) | https://developer.android.com/studio#command-line-tools-only |
| JDK | 17 or newer (we use 24) | e.g. `winget install --id Microsoft.OpenJDK.17` |
| scrcpy (optional, mirrors the phone) | any | `winget install --id Genymobile.scrcpy` |

Accept the Android SDK licences yourself (`sdkmanager --licenses`). Check the Android side with
`pwsh -File tools\android\doctor.ps1`.

## 2. Folder location (important)

Eight PC-side scripts still hardcode `C:\Users\Jarrod\wwe2k14-recomp` (`tools\build.ps1`, `tools\build_sdk.ps1`,
`tools\runlock.ps1`, `tools\bench\run_list.ps1`, `run_bench.ps1`, `run_timed_concurrent.ps1`, `ab_paired.py`,
`lanes_report.py`). Either unpack to that exact path, or replace that string with your folder in those files.

## 3. The SDK

From the `wwe2k14-recomp` folder:

```
pwsh -File tools\sdk_patches\apply_series.ps1 -ExpectTree {{SDK_TREE}}
```

It clones upstream rexglue/rexglue-sdk at tag `v0.10.0` (commit `f5337cd`) into `thirdparty\rexglue-sdk-src`, applies
our 31 diffs in order, checks the result is exactly our tree (it stops if not), makes branch `wwe2k14-local` and fetches
the submodules from their public repositories. Tested 2026-10-25: the tree matched byte for byte.

## 4. Your game files

- Dump your disc and put the untouched copy in `game_data\original\`, the extracted files in `game_data\extracted\`
  (the codegen reads `game_data\extracted\default.xex`). The project's dump: title ID `545408B2`, `default.xex` Media ID
  `0D94D1D2` (check yours with `py tools\xex_headers.py game_data\extracted\default.xex`); a matching full disc image
  is 8,738,846,720 bytes, SHA-1 `d671d3a5ea044005d870a4ffbaea4b6908cca6f6` (redump). Another version may need other
  codegen fixes.
- For the phone: a disc image (ISO) of your disc at `game_data\iso\wwe2k14_test.iso`, pushed once with
  `pwsh -File tools\android\run.ps1 -PushGame` (about 8 GB over USB).
- Test runs copy a frozen save folder from `out\bench\userdata_frozen`: boot the PC build once to the main menu and copy
  its user data folder there.

## 5. Build and run

PC first (it runs the codegen from your `.xex` into `generated\`, which the Android build reuses):

```
pwsh -File tools\build_sdk.ps1 -Run first -Reconfigure -Pin "my first SDK build"
pwsh -File tools\build.ps1 -Run 001
```

(`-Pin` records your SDK build's DLL hashes in `tools\sdk_pins.json`; `tools\build.ps1` refuses unpinned DLLs.)

Android (phone with USB debugging on, `adb devices` lists it):

```
pwsh -File tools\android\build_all.ps1 -Run b01
adb install -r out\android\apk\wwe2k14-android-dev.apk
pwsh -File tools\android\run.ps1 -Run 001 -Seconds 60
```

Logs and logcat land in `out\android\runs\NNN`. `ANDROID_PLAN.md` (progress log at the bottom) lists what works, the
open problems and the tools (`ab.ps1`, `sample.ps1`, `demo_fps.py`, `hitch_context.py`).

## 6. Sending work back

Make your changes on a branch of your own (`git init` + a first commit of this folder, then branch), and send Jarrod a
patch (`git format-patch`) or a zip of the changed files. SDK changes: a patch from the SDK repo.
