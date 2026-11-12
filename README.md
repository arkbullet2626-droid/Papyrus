# Papyrus

An ultra-light instant-replay game clipper for Windows by Opifex Studios.

Papyrus keeps the last 30 seconds of your screen in memory as compressed video. Press **Ctrl+Shift+F9** and those 30 seconds are saved as an `.mp4`.

**Status: early development.** NVIDIA cards only, video only.

## Download

Get the latest `Papyrus-Setup-x.y.z.exe` from the [Releases page](https://github.com/arkbullet2626-droid/Papyrus/releases/latest).

The installer is not code-signed yet, so Windows SmartScreen shows "Windows protected your PC". Click **More info**, then **Run anyway**.

## Using it

1. Open **Papyrus** from the Start menu. The replay buffer starts **off**.
2. Click the big button to turn the replay buffer on. Click it again to turn it off.
3. While it is on, press **Ctrl+Shift+F9** (or click **Save clip now**) to save the last 30 seconds. High beep = saved, low beep = failed.
4. Set the save hotkey, clip length (15 s to 2 min), frame rate (120/60/30) and quality (4K down to 480p) in the same window.
5. Tick **Always on** to have Papyrus start with Windows with the buffer already on.
6. Closing the window keeps Papyrus in the tray near the clock. Right-click the icon and choose **Exit Papyrus** to quit.

Clips are in `Videos\Papyrus`.

The video is HEVC (H.265). VLC plays it directly; Windows' own players need the "HEVC Video Extensions" add-on from the Microsoft Store.

## Requirements

- Windows 10 version 1903 or newer (Windows 11 recommended)
- NVIDIA graphics card with driver 570 or newer

## How it works

- Capture: Windows Graphics Capture. No hooks, no DLL injection.
- Colour conversion on the GPU, encoding on the card's NVENC block. A raw frame never leaves the GPU.
- Only compressed packets are buffered, in one pre-allocated block of RAM.
- Saving a clip copies packets into an MP4 without re-encoding.
- No network connections, no telemetry, no accounts.

The full design and the performance budgets are in [docs/SPEC.md](docs/SPEC.md).

## Building from source

Needs Python with `pip install ziglang`.

```
sh build.sh
```

This produces `dist/papyrus-engine.exe`. The setup wizard is built with Inno Setup 6 from `installer/papyrus.iss`; see `.github/workflows/release.yml`.

## Third-party

`third_party/nv-codec-headers/nvEncodeAPI.h` comes from [FFmpeg/nv-codec-headers](https://github.com/FFmpeg/nv-codec-headers) under the MIT licence.
