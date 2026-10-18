# WWE 2K14 native PC recompilation ({{VERSION}})

A fan preservation project: the Xbox 360 version of WWE 2K14 recompiled to run natively on Windows. 2K14 never came
out on PC. **No game files are included.** You need your own copy of the Xbox 360 disc.

Build {{VERSION}} (commit {{COMMIT}}).

## What you need

- Windows 10 or 11, 64-bit.
- A graphics card with Vulkan support (any recent NVIDIA, AMD or Intel GPU with up-to-date drivers).
- Nothing else to install: the Microsoft Visual C++ runtime files the game needs are included (next to the game).
- An Xbox-compatible controller is recommended. A keyboard mode exists (Settings -> Advanced) but is untested.
- Your own WWE 2K14 Xbox 360 disc, copied to your PC as a folder or a disc image (below).

## Copying your disc

There is one WWE 2K14 disc for the Xbox 360 (the same disc was sold in the US and Europe). This build supports it:
**version 0.0.0.2, with no title update**. The launcher checks your `default.xex`:

| | |
|---|---|
| Title id | 545408B2 |
| Version | 0.0.0.2 |
| SHA-256 of `default.xex` | `544EF8B505AEBCECE3459865A4E27E1A7CFAD568406514F27E1115B378D3A8FB` |

Two ways to copy it:

- **A folder:**
  1. Install the game from the disc to your Xbox 360's hard drive or a USB stick.
  2. Connect that drive to your PC and read it with a FATX tool (e.g. FATXplorer).
  3. Copy the game's folder to your PC. It must contain `default.xex` and the `pac`, `movies` and `sound` folders.
- **A disc image (`.iso`)** made from your disc (a full image of the disc or a trimmed one): pick it in the launcher
  with **Disc image...**. Nothing needs unpacking.

A normal PC DVD drive can't read the game part of an Xbox 360 disc (it only sees a short "play this on an Xbox 360"
video), so putting the disc in your PC's drive doesn't work: a disc image needs a drive made for copying Xbox 360
discs.

If your copy is a single package file (STFS) instead of a folder, extract it first. Use the files straight from your
disc: a title update changes `default.xex`, and the launcher will say so. A title update file (`default.xexp`) next
to `default.xex` is ignored: the game plays the disc version.

If the launcher says your disc image is **incomplete**, the copy was cut short: copy it again.

## First run

1. Unzip this folder anywhere, e.g. `C:\Games\WWE2K14`.
2. Start **`wwe2k14_launcher.exe`**.
3. **Windows SmartScreen** may say "Windows protected your PC", because this program isn't signed. Click **More info**,
   then **Run anyway**. Some antivirus programs flag new unsigned programs too; that's a false positive. You can check
   the zip against the SHA-256 published with it.
4. The first-run wizard asks for your game folder or disc image, checks it, finds your save (if you have one) and suggests settings
   for your PC (language, graphics card, resolution, frame rate), which you can change right there.
5. Click **PLAY GAME**, or **QUICK MATCH** to set up a match in the launcher and start straight into it.

Your saves go to `Documents\wwe2k14` (the same place every time you play).

## Controls

The game plays as it did on the Xbox 360, with a controller. On PC:

| Key | What it does |
|---|---|
| **F8** | CineCam: a free camera (turn it on in Settings -> Game). Hold the right mouse button to look, W/A/S/D to move. |
| **F9** | A run-in: another wrestler interferes (turn it on in Settings -> Game; experimental). |
| **F3** | Shows the frame rate. |
| **Alt+F4** | Quit. The main menu also has QUIT TO DESKTOP at the bottom. |

## Settings

In the launcher: **Settings** (Game, Display, Graphics, Advanced). Frame rate, resolution and upscaling are also in
the game itself: **Options -> GRAPHICS**, after GAMEPLAY. Both change the same saved choices. In the game, DLAA on /
off applies at once; the DLSS modes, the frame rate and the resolution apply the next time you start the game.

- **Language:** English, Deutsch, Français, Español or Italiano (the five on the disc). Automatic follows Windows.
- **Graphics card:** Automatic uses your dedicated graphics card, also on laptops that have a second, built-in one.
  You can pick a card by name.
- **Display:** fullscreen or windowed.
- **Resolution:** 720p, 1440p or 4K. 1440p is the default.
- **Frame rate:**
  - 60: entrances and matches at 60 fps (the default).
  - 120: entrances and matches at 120 (needs a monitor faster than 60 Hz).
  - Uncapped: matches at your monitor's refresh rate, entrances at up to 120.
- **Upscaling:**
  - On an NVIDIA RTX card: DLAA, or DLSS Ultra Performance / Performance / Quality.
  - NVIDIA's DLSS files are included (the `nvidia` folder), so it works as is.
  - To use a newer DLSS, point Settings -> Graphics at a folder with NVIDIA's Streamline runtime.
  - On other cards: FSR support is coming soon.
- **Original look** (Settings -> Graphics): the Xbox 360 picture, 720p with 30 fps entrances.

## Mods

- **Trons:** play your own video (MP4 / MOV / MKV) as any wrestler's titantron.
- **Titles:** replace a title reveal video, transparency included.
- **Logos:** import PNG pictures as real Create-a-Logo logos.
  - The launcher backs up your save first.
  - Then the game makes and saves each logo itself.
  - Use them on attires like any logo you paint. The game allows at most 2 different 256x256 logos on one custom
    superstar.
  - You need a save first: play once, then come back.

## Something went wrong?

Use **Report a problem** in the launcher (bottom left). It makes a `.zip` and opens its folder. Inside are:
- the latest game log and the performance report;
- the launcher's settings and the version;
- your PC's basics (GPU, CPU, RAM, Windows).

Your user name is removed from file paths, and nothing is sent anywhere. Send the zip to whoever gave you this build.

If the game closes right after starting, click **Try again**: a known audio start-up crash sometimes happens and a
second start fixes it.

## Disclaimer

A non-commercial fan project. Not affiliated with, endorsed by or connected to WWE, 2K, Take-Two Interactive,
Yuke's, Visual Concepts or Microsoft. WWE 2K14 and all related names and marks belong to their owners. This
download contains no game files, and the program does nothing without your own copy of the game. Never pay for
this; it is free.

See `LICENSE` for this project's code and `THIRD_PARTY_NOTICES.md` for the libraries it uses.

## Credits and included third-party files

- **Upscaling: NVIDIA DLSS.** NVIDIA, the NVIDIA logo, DLSS and RTX are trademarks and/or registered trademarks of
  NVIDIA Corporation.
  - The files in the `nvidia` folder belong to NVIDIA and come under NVIDIA's own licence
    (`nvidia\nvngx_dlss.license.txt`, `nvidia\streamline.license.txt`).
  - You may use them only with this program.
  - You may not reverse engineer, decompile or disassemble them, or distribute them on their own.
- **The Microsoft Visual C++ runtime** (`msvcp140*.dll`, `vcruntime140*.dll`): Microsoft's redistributable files,
  included unmodified.
- Built with the ReXGlue SDK (portions from the Xenia project). Other libraries: `THIRD_PARTY_NOTICES.md`.
