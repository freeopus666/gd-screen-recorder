# GD Screen Recorder

**A Geode mod for Geometry Dash by [FreeOpus666](https://github.com/FreeOpus666)**

Record your gameplay directly inside the game — no OBS, no window capture.  
GPU-accelerated encoding, two separate audio tracks, and async frame capture for stable 60 fps.

---

## Features

- 🎬 **Hardware encoding** — auto-detects NVIDIA (NVENC), AMD (AMF) or Intel (QSV), falls back to CPU (libx264)
- 🎙️ **Microphone** — recorded as Track 1 via DirectShow
- 🔊 **Game audio** — recorded as Track 2 via WASAPI loopback (works with any speakers or headphones, no Stereo Mix needed)
- 🎞️ **Dual audio tracks** in one `.mp4` — mute/unmute each track independently in any video editor or player
- ⚡ **Async PBO readback** — triple-buffered, zero GL stall, minimal FPS impact
- 📸 **Screenshot** (BMP) with one key
- 🔴 **On-screen REC indicator** with elapsed time

---

## Requirements

> ⚠️ **ffmpeg.exe is required.** The mod will not record without it.

1. Download **FFmpeg** from **[gyan.dev/ffmpeg/builds](https://www.gyan.dev/ffmpeg/builds/)**  
   → `ffmpeg-release-essentials.zip`
2. Open the archive, go to `bin/`
3. Copy **`ffmpeg.exe`** into your **Geometry Dash folder** (`C:\...\Geometry Dash\`)

---

## Hotkeys

| Key | Action |
|-----|--------|
| `F5` | Start / Stop recording |
| `F8` | Cycle microphone device |
| `F9` | Cycle game audio device |
| `F11` | Toggle REC indicator |
| `F12` | Screenshot (BMP) |

---

## Settings

Open **Geode → Mods → GD Screen Recorder → Settings**

| Setting | Description |
|---------|-------------|
| Video Quality | 1 = Low · 2 = Medium · 3 = High · 4 = Ultra |
| Recording FPS | 15–120 fps (default 60) |
| Video Encoder | Auto / NVIDIA / AMD / Intel / CPU |
| Output Folder | Where to save videos (default: `GD\recordings\`) |
| Show Indicator | Toggle blinking REC overlay |
| Record Microphone | Enable mic (Track 1) |
| Microphone Device | Device number or exact name (press F8 in-game to pick) |
| Record Game Audio | Enable game audio (Track 2) |
| Game Audio Device | Device number or exact name (press F9 in-game to pick) |

---

## Output

Videos are saved as `.mp4` to `Geometry Dash\recordings\GD_YYYYMMDD_HHMMSS.mp4`  
FFmpeg errors are logged to `recordings\~ffmpeg_log.txt` (deleted on success).

---

## Building from source

**Requirements:** CMake 3.21+, Visual Studio 2022 (C++ workload), [Geode SDK](https://docs.geode-sdk.org/getting-started/)

```bash
git clone https://github.com/JamStickGD/gd-screen-recorder
cd gd-screen-recorder
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config RelWithDebInfo
```

Or just run `BUILD_GD_RECORDER.bat` from the Geometry Dash folder — it does everything automatically.

---

## License

MIT — see [LICENSE](LICENSE)  
FFmpeg is **not** included. Download it separately from [gyan.dev/ffmpeg/builds](https://www.gyan.dev/ffmpeg/builds/).
