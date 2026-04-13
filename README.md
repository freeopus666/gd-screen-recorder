# <cr>GD</c> <co>Screen</c> <cg>Recorder</c>

Turn Geometry Dash into a <cy>creator-ready</c> recording studio.

**GD Screen Recorder** captures gameplay **directly inside the game**, saves everything into **one clean `.mp4`**, keeps your audio organized, and is built to feel **fast, smooth, and effortless** from the very first press of `F5`.

---

## <cb>Why this mod feels different</c>

- <cg>Record directly in-game</c>  
  No OBS scene setup, no window capture headaches, no messy desktop footage.

- <co>One clean export</c>  
  Your session is saved as a polished `.mp4` that is ready for previews, showcases, completions, highlights, and uploads.

- <cp>Three-track audio layout</c>  
  You do not just get a flat recording. You get a flexible audio setup inside one file:
  - `Track 1` = `Game + Mic`
  - `Track 2` = `Game Audio only`
  - `Track 3` = `Microphone only`

- <cl>Optimized capture pipeline</c>  
  Async GPU readback, optional GPU downscale, smart encoder detection, and safer fallback paths help keep recording smooth even on weaker or older systems.

- <cj>Hotkey-first workflow</c>  
  Everything important is available instantly while you play, so recording feels like part of the game instead of a separate setup.

---

## <ca>Built for real Geometry Dash moments</c>

This mod was made for the clips players actually want to keep:

- level completions
- showcase videos
- layout previews
- practice runs
- challenge attempts
- funny fails
- highlight reels
- YouTube-ready gameplay footage

Whether you want a clean preview for a new level or a polished completion clip worth posting, **GD Screen Recorder** is built to make that process feel simple.

---

## <cf>Performance that stays lightweight</c>

This mod is designed to stay <cg>lightweight</c> instead of turning every recording into a lag test.

On optimized setups, recording can stay around <cg>~10%</c> resource usage while still delivering smooth footage, clean audio separation, and stable capture behavior. Actual usage depends on your **encoder**, **resolution**, **recording FPS**, and **hardware**.

When hardware encoding is available, the mod is at its best. When it is not, the recorder still uses smarter fallback behavior to keep recordings usable instead of letting the output collapse into slideshow-like footage.

That means the goal is not just to record — the goal is to record in a way that still feels playable.

---

## <cy>What you get</c>

- Direct in-game recording
- One `.mp4` output
- Separate game and microphone audio handling
- Direct game-audio capture with fallback support
- Optional microphone recording
- GPU-optimized capture path
- Hardware encoder auto-detection
- CPU fallback encoders for unsupported setups
- On-screen `REC` indicator
- Built-in screenshot hotkey
- Safer behavior for microphone and audio backend failures
- Performance logging options for troubleshooting

---

## <cs>Hotkeys</c>

- `F5` — Start / Stop recording
- `F8` — Cycle microphone device
- `F11` — Toggle the `REC` indicator
- `F12` — Save a screenshot

---

## <cc>Requirements</c>

To record video, the mod needs:

- `ffmpeg.exe` in your **Geometry Dash folder** or available in your **PATH**

Recommended baseline for stable `720p30` recording:

- `Windows 10` or newer
- `2 cores / 4 threads` minimum
- `4 GB RAM` minimum
- Hardware encoding support such as **Intel Quick Sync**, **NVIDIA NVENC**, or **AMD AMF** for the smoothest results

For weaker systems, lower settings like `480p30` can provide a better experience.

---

## <cd>Safety and recovery features</c>

This mod includes quality-of-life safeguards so one failed device does not have to ruin the whole session:

- `Warn If Game Audio Unsupported`
- `Auto Disable Mic On Error`
- `Log Performance Issues`

These options help recording stay practical even when the machine, drivers, or audio stack are not perfect.

---

## <cr>Capture the level. Keep the moment.</c>

From first attempts to final completions, **GD Screen Recorder** turns Geometry Dash into a fast, clean, and flexible recording setup built right into the game.

If you want gameplay footage that looks polished without building an entire recording workflow around it, this is the mod for you.

[Source Code](https://github.com/FreeOpus666/gd-screen-recorder)
