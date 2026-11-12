# Papyrus

An ultra-light instant-replay game clipper for Windows by Opifex Studios.

Papyrus keeps the last 30 seconds of your screen in memory as compressed video. Press **Ctrl+Shift+F9** and those 30 seconds are saved as an `.mp4`.

**Status: early development.** NVIDIA cards only.

## Download

Get the latest `Papyrus-Setup-x.y.z.exe` from the [Releases page](https://github.com/arkbullet2626-droid/Papyrus/releases/latest).

The installer is not code-signed yet, so Windows SmartScreen shows "Windows protected your PC". Click **More info**, then **Run anyway**.

## Using it

Open **Papyrus** from the Start menu.

- **Clips** shows your clips as a grid of thumbnails. Rest the pointer on one for 2 seconds to preview it; click it to open the editor.
- **The editor** plays the clip, shows each sound track on its own row, and lets you shorten the clip, drop or shorten individual sound tracks, change a track's volume, and save the result as a new clip. The picture is never re-encoded.
- **Settings** has the replay buffer switch (off until you turn it on), Always on, what to record (fullscreen apps and games only, or the whole screen), the save hotkey (Ctrl+Shift+F9 by default), clip length, frame rate, quality and the microphone.

While the buffer is on, press the hotkey to save the last 30 seconds. High beep = saved, low beep = failed. Clips go to `Videos\Papyrus`.

Closing the window does not stop recording: a small recorder stays in the tray near the clock. Right-click its icon and choose **Exit Papyrus** to quit.

The video is HEVC (H.265). VLC plays it directly; Windows' own players need the "HEVC Video Extensions" add-on from the Microsoft Store.

## Requirements

- Windows 10 version 1903 or newer (Windows 11 recommended)
- NVIDIA graphics card with driver 570 or newer

## How it works

- Capture: Windows Graphics Capture. No hooks, no DLL injection.
- Colour conversion on the GPU, encoding on the card's NVENC block. A raw frame never leaves the GPU.
- Sound is captured per app with WASAPI: the game, Spotify, Discord and an optional microphone each get their own AAC track. No other app is recorded.
- Only compressed packets are buffered, in one pre-allocated block of RAM.
- Saving a clip copies packets into an MP4 without re-encoding.
- No network connections, no telemetry, no accounts.

The full design and the performance budgets are in [docs/SPEC.md](docs/SPEC.md).

## Building from source

Needs Python with `pip install ziglang`.

```
sh build.sh
```

This produces `dist/papyrus-engine.exe`. The Papyrus app is a WinUI 3 project in `app/` (`dotnet publish`, .NET 8). The setup wizard is built with Inno Setup 6 from `installer/papyrus.iss`; see `.github/workflows/release.yml`.

## Licence

Papyrus is released under the [MIT License](LICENSE). Copyright (c) 2026 Opifex Studios.

## Third-party

`third_party/nv-codec-headers/nvEncodeAPI.h` comes from [FFmpeg/nv-codec-headers](https://github.com/FFmpeg/nv-codec-headers) under the MIT licence.
