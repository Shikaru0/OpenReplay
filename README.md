# OpenReplay 🎬

**OpenReplay** is a lightweight, open-source screen clipping and streaming tool, built with C++, FFmpeg, and Dear ImGui. Focused on having a lot of configuration without being bloated.

## Features

- Screen capture via DXGI output duplication
- Loopback audio capture with per-device selection
- Microphone capture (separate audio track)
- PNG/JPEG screenshots
- RTMP streaming (BETA)

## Configuration

- Quality presets: Lossless, High, Standard, Stream
- Profile system: save, load, export, and import configurations
- Multiple output formats: MP4, MKV, WEBM, MOV, AVI, WAV, FLAC, MP3, OGG
- Video bitrate, keyframe interval, VBV buffer, encoder preset, pre-analysis
- Recording buffer size, clip duration, FPS, resolution
- Overlay corner, size, opacity, color, FPS display toggle
- Auto-save on stop, minimize to tray, capture cursor toggle

## Requirements

**Running** (pre-built binary):
- Windows 11 (x64)
- GPU with DirectX 11 support

**Building from source**:
- [CMake](https://cmake.org/) 3.25+
- Visual Studio 2022 with C++20 toolset
- Everything else is auto-downloaded (FFmpeg, Dear ImGui)

## External Packages

All third-party code is auto-downloaded at configure time or stored in-tree under `OpenReplay/external/`:

| Package | License | How it's obtained |
|---------|---------|-------------------|
| [FFmpeg](https://ffmpeg.org/) | LGPL/GPL | Auto-downloaded with `-DAUTO_DOWNLOAD_FFMPEG=ON` |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT | Auto-downloaded from GitHub |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | In-tree (`external/json.hpp`) |
| [stb_image_write](https://github.com/nothings/stb) | MIT | In-tree (`external/stb_image_write.h`) |

## Quick Start

```powershell
# Clone the repo
git clone https://github.com/Shikaru0/OpenReplay
cd OpenReplay

# Configure with CMake (auto-downloads FFmpeg + Dear ImGui)
cmake -B build -S .

# Build
cmake --build build --config Release

# Run
.\build\OpenReplay\Release\OpenReplay.exe
```

## Build Configuration

| CMake Option | Default | Description |
|-------------|---------|-------------|
| `FFMPEG_DIR` | (empty) | Path to FFmpeg dev SDK (skip auto-download) |
| `AUTO_DOWNLOAD_FFMPEG` | ON | Auto-download FFmpeg SDK at configure time |