# Build Papyrus: an ultra-light 4K120 game clipper for Windows

## Role
You are a senior Windows media engineer (Direct3D 11, Windows Graphics Capture, NVENC SDK, WASAPI, MP4 muxing). Build a native instant-replay clipper whose #1 goal is the lowest possible RAM, CPU and GPU-3D usage while recording up to 4K (3840x2160) at 120 fps, with lower quality tiers below that.

Before writing code: restate the architecture below in your own words, list the top 5 technical risks and how you'll test each, then start Milestone 1. Do not skip ahead.

## Target machine (primary test rig)
- Windows 11 (24H2 or newer), AMD Ryzen 9 9900X3D, NVIDIA RTX 5070 (ONE 9th-gen NVENC engine, supports H.264/HEVC/AV1 incl. 10-bit), 32 GB DDR5, NVMe SSD, high-refresh OLED monitor.
- Must still run on any GPU later: NVIDIA (NVENC), AMD (AMF), Intel (QSV), with Media Foundation hardware encoder as a fallback. NVIDIA first.

## Hard performance budgets (treat as acceptance criteria)
- Engine RAM while recording 4K120 in disk-buffer mode: <= 150 MB private working set.
- Engine RAM in RAM-buffer mode: <= 100 MB + (bitrate/8 x buffer seconds). Show this estimate in settings before the user picks it.
- CPU: <= 2% average of total CPU on the target machine while recording 4K120.
- GPU 3D engine usage added by the app: <= 3%. Encoding must happen on the NVENC block, not CUDA/3D cores.
- Zero dropped or duplicated-by-mistake frames over a 30-minute 4K120 session; output plays at a constant 120 fps.
- Saving a 30 s 4K120 clip takes < 1 s and causes no visible in-game hitch.
- Startup to "recording" in < 2 s. Engine binary + deps < 30 MB.

## Non-negotiable rules
1. No Electron, CEF, WebView2, .NET runtime, Python or any garbage-collected runtime in the recording engine.
2. A raw frame must never leave the GPU. No GPU->CPU readback, no CPU colour conversion, no software encoder in the default path.
3. No DLL injection or game hooks. Must be safe with Vanguard, Easy Anti-Cheat, BattlEye and FACEIT.
4. Store only compressed packets in the replay buffer. Never buffer decoded frames.
5. No per-frame or per-packet heap allocation in steady state. Pre-allocate everything at start.
6. No network calls, no telemetry, no accounts. Everything local.
7. Hotkeys via RegisterHotKey, never low-level keyboard hooks.

## Architecture
Two processes:
- `engine.exe` (C++20, Win32, no UI framework): capture, encode, buffer, save, tray icon (Shell_NotifyIcon), hotkeys, toast notifications. Runs all the time.
- `studio.exe` (WinUI 3 / C# is fine): settings, clip library, lossless trim. Launched only when the user opens it from the tray; fully exits when closed. Talks to the engine over a named pipe with a tiny JSON or binary protocol. The engine must work perfectly with studio closed.

Engine threads (MMCSS via AvSetMmThreadCharacteristics "Capture" for capture/encode):
1. Capture thread
2. Encode submit + completion thread (NVENC async mode)
3. Audio thread(s)
4. Buffer/mux writer thread (low I/O priority)
5. Control thread (pipe, hotkeys, tray)

## Pipeline details

### 1. Capture (Windows Graphics Capture first, DXGI Desktop Duplication fallback)
- Use WGC via IGraphicsCaptureItemInterop::CreateForWindow (game window) or CreateForMonitor (fullscreen/exclusive cases).
- Direct3D11CaptureFramePool::CreateFreeThreaded with BGRA8 (SDR) or R16G16B16A16Float (HDR), 2-3 buffers.
- At runtime, if available: IsBorderRequired = false, MinUpdateInterval = 1/target fps, IsCursorCaptureEnabled per setting. Feature-detect with ApiInformation; never crash if missing.
- Fallback to DXGI Desktop Duplication (IDXGIOutput5::DuplicateOutput1) when WGC fails or delivers no frames for 2 s; log which path each session uses.
- Auto-follow the foreground game: detect fullscreen/borderless foreground window, switch capture target without restarting the encoder when resolution matches.

### 2. Frame pacing (constant frame rate)
- WGC only delivers frames when content changes. Drive the encoder from a QPC-based clock at exactly the target fps: if no new frame arrived, re-submit the last texture; if two arrived, keep the newest.
- Timestamps from QueryPerformanceCounter, converted to the stream time base.

### 3. GPU colour conversion and scaling
- Convert BGRA -> NV12 (SDR, 8-bit) or FP16 -> P010 (HDR, 10-bit, BT.2020 PQ) on the GPU with ID3D11VideoProcessor (preferred) or a compute shader.
- Downscale on the GPU in the same pass for lower tiers (e.g. 4K game -> 1440p clip).
- Pool of 4-6 NV12/P010 textures, registered ONCE with NVENC via nvEncRegisterResource (NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX) and reused with nvEncMapInputResource/nvEncUnmapInputResource.

### 4. Encode (NVENC SDK directly, not through FFmpeg)
- Device type NV_ENC_DEVICE_TYPE_DIRECTX on the same D3D11 device as capture.
- Query caps with nvEncGetEncodeCaps; pick AV1 10-bit or HEVC Main10 by user choice; H.264 only for compatibility tier.
- Defaults for 4K120: preset P4, tuning HIGH_QUALITY, rate control VBR with target + max bitrate (see quality ladder), GOP = 1 second (120 frames), IDR every GOP so any second is a valid clip start, B-frames 0-2 (benchmark both), lookahead OFF, multipass OFF or quarter-res, repeat SPS/PPS on IDR.
- First-run benchmark: encode 300 synthetic 4K frames at P3, P4, P5. Pick the slowest preset whose average frame time stays < 70% of the frame budget (8.33 ms at 120 fps). Re-check live: if encode time exceeds budget for 1 s, step down one preset and log it.
- Async mode with completion events; bitstream buffers pre-allocated (6-8).

### 5. Audio
- WASAPI per-process loopback (ActivateAudioInterfaceAsync with AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK, include process tree) for the game.
- Separate tracks: game, system (classic loopback, excluding the game tree), microphone (optional, off by default).
- AAC-LC 48 kHz 160 kbps per track via the Media Foundation AAC encoder. Audio timestamps on the same QPC clock as video; correct drift.

### 6. Replay buffer (the RAM saver)
Two modes, user-selectable, with a live size estimate:
- RAM ring: one pre-allocated slab of (max bitrate/8 x seconds x 1.2). Packets appended with a small index (pts, offset, size, isKeyframe, track). Oldest GOPs evicted whole. Default for buffers <= 60 s at <= 1440p.
- Disk ring (default for 4K or buffers > 60 s): pre-allocated segment files on a user-chosen NVMe folder, one file per 2 s GOP-aligned segment, recycled in place (no file create/delete churn). RAM holds only the segment being written (~25-40 MB at 4K120). Write with FILE_FLAG_SEQUENTIAL_SCAN and low I/O priority.
- Show estimated SSD writes per hour in settings so the user understands the trade-off.

### 7. Save a clip
- Hotkey (default Alt+F10, configurable) saves the last N seconds (default 30, options 15/30/60/120/300).
- Find the nearest keyframe at or before (now - N). Remux packets to MP4 with NO re-encode, faststart (moov at front), all audio tracks labelled.
- Muxer: a small self-contained MP4 writer, or libavformat built remux-only under LGPL (document the licence). Runs on the writer thread at low priority.
- Toast notification "Clip saved (00:30, 4K120)" with Open/Show in folder. Optional sound cue.
- Second hotkey: start/stop a full manual recording (writes straight to disk, same pipeline).

### 8. Quality ladder (all user-selectable; defaults shown)
| Tier | Resolution | FPS | Codec | Target / max Mbps |
| Max | 3840x2160 | 120 | HEVC 10-bit | 100 / 150 |
| Efficient 4K | 3840x2160 | 120 | AV1 10-bit | 70 / 100 |
| 4K 60 | 3840x2160 | 60 | HEVC or AV1 | 60 / 90 |
| 1440p 120 | 2560x1440 | 120 | HEVC or AV1 | 50 / 75 |
| 1440p 60 | 2560x1440 | 60 | HEVC or AV1 | 35 / 50 |
| 1080p 120 | 1920x1080 | 120 | HEVC or AV1 | 30 / 45 |
| 1080p 60 | 1920x1080 | 60 | HEVC or AV1 | 20 / 30 |
| Compatible | 1920x1080 | 60 | H.264 | 25 / 35 |
If the game renders below the chosen tier, record at native resolution; never upscale.

### 9. Built-in diagnostics
- `engine.exe --stats` and a Stats page in studio: private working set, CPU %, NVENC utilisation (NVML), capture fps, encode ms/frame, dropped/duplicated frames, buffer fill, disk write MB/s, current preset.
- Rolling local log file (max 5 MB, 3 files).

## Studio app (v1 scope)
- Settings: tier, buffer length, buffer mode, save folder, hotkeys, audio tracks, start with Windows.
- Library: grid of saved clips with thumbnails generated lazily via hardware decode, game name from the captured process.
- Lossless trim: cut on keyframes, no re-encode.

## Milestones (deliver and test each before the next)
1. CLI proof: WGC -> GPU NV12 -> NVENC HEVC -> raw .h265 file at 4K120 for 60 s; print stats. Prove budgets 2-3.
2. RAM ring buffer + hotkey save to MP4 (video only).
3. Audio: game/system/mic tracks, A/V sync within 1 frame over 30 min.
4. Tray, toasts, named-pipe control, settings file, start with Windows.
5. Disk ring, quality ladder, preset auto-benchmark, HDR/P010, AV1.
6. Studio app: settings, library, lossless trim.
7. AMD AMF and Intel QSV backends behind the same IEncoder interface; Media Foundation fallback.

## How you deliver every version
For every new version, give me: (1) the download / built files, (2) full step-by-step instructions to open and run it, assuming I'm not a professional developer, and (3) the complete list of tests to do for that version, including the exact numbers to check against the budgets above (Task Manager columns, --stats output, expected clip length, fps and file size).

## Out of scope for v1
Cloud upload, social features, auto-clipping/AI event detection, in-game overlay, webcam, streaming. Design so these could be added later as separate processes that never run inside the engine.