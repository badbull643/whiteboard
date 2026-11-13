# Infinite Whiteboard — Plan

A small, very fast, native Windows whiteboard: open a board, draw with pens, pan and zoom without limits, with a grid you can resize.

---

## 1. Tech choice

| Choice | Decision | Why |
|---|---|---|
| Language | **C++20** | As fast as Rust here. It's also already set up on this machine (VS 2022 Community, CMake, MinGW g++). Rust isn't installed. |
| Windowing | **Raw Win32 API** | No framework overhead, starts instantly, and the exe stays small (~200–400 KB). |
| Rendering | **Direct2D 1.1 on a D3D11 device** + DXGI flip-model swap chain | Lines are GPU-accelerated and antialiased. It's built into Windows, so there's nothing to ship or install. |
| Input | **WM_POINTER** API | One code path for mouse, pen and touch. Gives pen pressure and the full high-rate point history (no dropped points on fast strokes). |
| Build | CMake, MSVC `/O2 /GL /LTCG /arch:AVX2` | Release build with link-time optimization. |
| Dependencies | **None** besides the Windows SDK (d2d1, d3d11, dxgi) | Single portable `.exe`. |

> Rust alternative: the same design works in Rust with the `windows` crate. Speed would be the same, but you'd have to install the toolchain. C++ is recommended.

---

## 2. Features (v1, kept small)

- **New / Open / Save** a board (`.wbd` binary file). Autosaves on exit.
- **Pen tool**: pick a color (6–8 presets plus a custom color picker) and a width (slider or 1–5 presets). Pen pressure changes the width when you use a pen.
- **Eraser**: removes the whole stroke it touches. Simple and fast.
- **Infinite canvas**: pan with middle mouse, Space+drag or two-finger touch. Zoom with Ctrl+wheel or pinch, centered on the cursor.
- **Grid**: lines / dots / off. **Size is selectable** (8, 16, 32, 64, 128 px, or a custom value). When zoomed far out, the grid adapts so it never turns into a solid gray mass.
- **Undo / Redo** (Ctrl+Z / Ctrl+Y).
- **Clear board**, **Reset view** (Home key).
- **Light / dark mode** (`D` or right-click menu). Starts in the same mode as Windows. The default ink flips color with the theme, so drawings stay visible in both.

Out of scope for v1: text, shapes, images, selection/move, layers, multi-user.

---

## 3. Architecture

```
src/
  main.cpp          WinMain, window creation, message loop
  App.h/.cpp        App state, tool switching, glue code
  Renderer.h/.cpp   D3D11/D2D device, swap chain, frame drawing, resize/device-lost
  Camera.h          world <-> screen transforms (double precision)
  Board.h/.cpp      stroke storage + spatial index
  Stroke.h          stroke data (points, color, width, bounds)
  Input.h/.cpp      WM_POINTER handling, point history, smoothing
  Grid.h/.cpp       visible-only grid generation
  History.h/.cpp    undo/redo command stack
  Toolbar.h/.cpp    small floating toolbar drawn with D2D
  FileIO.h/.cpp     .wbd save/load
CMakeLists.txt
```

### 3.1 Infinite space and coordinates
- **Camera** stores `double offsetX, offsetY, zoom`. Zoom is clamped to about 0.01×–100×.
- **Strokes** store an `origin` (double, world space) plus points as `float` *relative to that origin*. Floats stay precise even millions of units from the center, and memory stays at 8 bytes per point (plus 4 for pressure if present).
- At draw time each stroke's transform is `origin - camera`, done in double and then converted to float. This avoids the "jitter far from origin" problem.

### 3.2 Spatial index (so only what's visible costs anything)
- A **sparse, hierarchical hash grid**: level L has tiles of 1024·4^L world units, stored as `unordered_map<(level, x, y), vector<StrokeId>>`.
- Each stroke goes into the smallest level where it spans at most 2×2 tiles, so even huge strokes sit in 1–4 tiles.
- Each frame, list the visible tiles (or scan the existing tiles when zoomed far out), collect stroke IDs (deduplicated with a query stamp, sorted into draw order), then test each stroke's bounding box against the viewport.
- The eraser uses the same lookup. It runs a bounding-box test first, then a precise point-to-segment distance test.
- Frame cost depends on **what's on screen**, not on board size.

### 3.3 Rendering pipeline
- **Render on demand only.** Redraw only on input, resize or camera change. The CPU sits at 0% when idle, with no busy loop.
- Flip-model swap chain (`DXGI_SWAP_EFFECT_FLIP_DISCARD`), `SetMaximumFrameLatency(1)` + a waitable swap chain for **minimum pen-to-ink latency**.
- **Direct2D** draws the background, grid and text. **Strokes use their own Direct3D 11 shader** on the same device.
  - *Why:* Direct2D geometry realizations were tried first. Profiling showed ~7 µs CPU per stroke drawn, plus expensive re-tessellation on zoom: 4.7 s per frame with 3k strokes visible.
- **Every finished stroke is tessellated once** (Catmull-Rom smoothing → adaptive flattening → triangle strip with round joins) and appended to **one big GPU vertex buffer**. Strips are stitched together with degenerate triangles.
  - Each vertex is 20 bytes: local position, offset direction, and packed stroke id / side / cap.
  - Per-stroke data (origin split into two floats for precision, width, color, flags) lives in a structured buffer.
  - Undo only flips a flag: no re-upload.
- **The vertex shader does the work:** it applies zoom, width, the 1 px minimum width and the theme ink color. The pixel shader computes anti-aliasing and **exact round caps** from distance to the centerline. Zooming costs no CPU at all.
- **Drawing:** >256 visible strokes → **one draw call for the whole buffer** (the GPU clips off-screen triangles for free). Fewer → only the visible ranges.
- **The stroke being drawn** is re-tessellated each frame into a small dynamic buffer and drawn with the same shader, so it looks the same before and after release.
- Handle device loss (`D2DERR_RECREATE_TARGET`, `DXGI_ERROR_DEVICE_REMOVED`) by recreating everything and re-uploading strokes from the CPU copy.
- **Measured** (GTX 1080, 1280×800, 1M points / 6,658 strokes): zoom/pan sweep avg **1.16 ms** CPU per frame (p95 2.0 ms), locked at 144 fps. Everything visible: 2.6 ms per frame, 1 draw call. One-time upload of 1M points: ~140 ms. GPU mesh: ~57 MB.

### 3.4 Grid
- Generated each frame **in screen space, visible lines only**. That's at most a few hundred lines, so it's essentially free.
- `screenSpacing = gridSize * zoom`. While it's < ~8 px, multiply the spacing by 4 (adaptive levels). Fade between levels so zooming looks smooth.
- Every Nth line is slightly stronger (major lines).
- Dots mode: draws a cached small dot bitmap tiled with a bitmap brush, which is cheap.
- Pixel-snapped 1 px lines so the grid looks crisp at any zoom.

### 3.5 Input and stroke quality
- `WM_POINTERDOWN/UPDATE/UP` + `GetPointerInfoHistory` to get every coalesced sample. Pens report at 200+ Hz.
- Light smoothing while drawing (a moving average over the last 2–3 points).
- On pen up: **Ramer–Douglas–Peucker** simplification (tolerance ~0.3 px at the current zoom). This usually cuts points by 60–90% with no visible change, which shrinks memory, files and render cost.
- Pressure → width per segment. Strokes with pressure are built as filled outline polygons instead of a single stroked path.

### 3.6 Undo/redo
- A command stack of `AddStroke{id}` / `EraseStrokes{ids}` / `Clear{ids}`.
- Erased strokes are flagged dead, not freed, so undo is O(1). A compaction pass runs on save.

### 3.7 File format (`.wbd`)
- Header: magic, version, camera, grid settings.
- Per stroke: color (u32), width (f32), origin (2×f64), point count (u32), raw point array (f32 pairs).
- Written in one buffered write and read with one read. Loading 1M points takes milliseconds.
- Later, if needed: LZ4 or zstd compression.

---

## 4. UI (minimal)
- A small **floating toolbar** at the top, drawn with D2D: Pen, Eraser, color swatches, width, grid toggle, grid size menu.
- The grid size menu is a native `TrackPopupMenu` (8/16/32/64/128/Custom…).
- Shortcuts: `P` pen, `E` eraser, `G` cycle grid mode, `[` / `]` grid size, `1–8` colors, `Ctrl+Z/Y`, `Ctrl+S/O/N`, `Home` reset view, `F11` fullscreen.
- Per-monitor DPI aware (v2 manifest).

---

## 5. Performance targets
| Metric | Target |
|---|---|
| Cold start to drawable window | < 100 ms |
| Pen-to-pixel latency | ~1 frame (flip model + latency 1) |
| Pan/zoom with 1M+ points on the board | locked to monitor refresh (60–240 Hz) |
| Idle CPU | 0% |
| Exe size | < 500 KB, no DLLs to ship |
| Memory | ~8–12 bytes per point + GPU realizations for visible strokes |

Built-in **debug overlay** (F3): frame time, visible strokes, total points, realization rebuild count. This lets us measure instead of guess.

---

## 6. Milestones
1. **Skeleton**: Win32 window, D3D11/D2D device + flip swap chain, render-on-demand loop, DPI awareness.
2. **Camera + grid**: pan/zoom around the cursor, adaptive grid with a selectable size.
3. **Pen drawing**: WM_POINTER input, live stroke, finalize → smoothed geometry realization.
4. **Spatial index + culling**: tile hash grid, visible-only drawing, F3 overlay. Stress-test with 1M generated points.
5. **Eraser + undo/redo.**
6. **Toolbar + shortcuts + colors/widths.**
7. **Save/Load + autosave.**
8. **Polish and profiling**: pressure, realization LOD tuning, optional cache layer, Release LTO build.

---

## 7. Build
```
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```
Output: `build/Release/Whiteboard.exe`
