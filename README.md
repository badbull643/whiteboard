# Whiteboard

A fast, simple infinite whiteboard for Windows. Native C++ / Win32 with Direct3D 11 + Direct2D. It ships as a single ~350 KB `.exe` with no DLLs and nothing to install.

## Build

Requires Visual Studio 2022 (C++ workload) and CMake 3.20+.

```
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output: `build/Release/Whiteboard.exe`

## Controls

| Action | Mouse | Keyboard |
|---|---|---|
| Draw | Left-drag (single click = dot) | `P` pen |
| Erase whole strokes | Right-drag (any tool), or left-drag with the eraser | `E` eraser |
| Cancel the current stroke / erase | | `Esc` |
| Pen color | Toolbar swatches, **+** = custom color | `1`–`8`, `9` custom |
| Pen width | Toolbar dots | `,` thinner · `.` thicker |
| Undo / redo | Toolbar arrows | `Ctrl+Z` · `Ctrl+Y` / `Ctrl+Shift+Z` |
| Clear board (undoable) | Right-click menu | `Ctrl+Del` |
| Zoom (around the cursor, smooth) | `Ctrl`+wheel, touchpad pinch | `+` / `-`, `Ctrl+0` = 100% |
| Pan | Middle-drag, `Space`+drag, wheel (`Shift`+wheel = sideways) | |
| Reset view | | `Home` |
| Grid: lines / dots / off | Toolbar grid button | `G` |
| Grid size | Toolbar grid button → size | `[` / `]` presets, `Shift+[` / `Shift+]` ±1 px |
| Dark / light mode | Toolbar sun/moon | `D` |
| New / Open / Save / Save as | Right-click menu, or drop a `.wbd` file on the window | `Ctrl+N` · `Ctrl+O` · `Ctrl+S` · `Ctrl+Shift+S` |
| Fullscreen | | `F11` (`Esc` leaves) |
| Shortcut line at the bottom | | `F1` |
| Stats panel | | `F3` |

Everything else is in the right-click menu.

## Saving

- **Your session is restored automatically.** When you close the app (or every 30 s if something changed), the board is saved to `%LOCALAPPDATA%\Whiteboard\session.wbd`, together with its file name, unsaved changes, view, grid, theme, pen and window position. Next start brings it all back, so there's no "save?" prompt on exit.
- **`Ctrl+S` saves to a `.wbd` file** of your choice. The title bar shows `*` while there are unsaved changes.
- **The `.wbd` format** is compact binary (~8 bytes per point) with a checksum. Damaged files are refused instead of half-loaded, and saving never leaves a half-written file. The format is described in [src/FileIO.h](src/FileIO.h).

## Performance

Measured on a GTX 1080 at 1280×800, with a 1M-point board (6,658 strokes):

| | |
|---|---|
| CPU per frame during a zoom/pan sweep (F10) | avg 1.1 ms, p95 1.5 ms, locked at 144 fps |
| Whole 1M-point board on screen | ~2.6 ms per frame, 1 draw call |
| Idle CPU | 0% (draws only when something changes) |
| Window visible after launch | ~45 ms (theme-colored; first full frame ~225 ms, mostly GPU driver load) |
| Save / load 1M points | ~75 ms / ~150 ms (7.8 MB) |

How it gets there:
- **Strokes are tessellated once** into one GPU vertex buffer and drawn by a custom shader (anti-aliasing, round caps and minimum width per pixel). Zooming costs no CPU.
- **A hierarchical spatial hash** finds visible strokes and powers the eraser.
- **Positions are stored as doubles** (with float offsets per stroke), so the board stays exact anywhere.
- **The GPU driver loads on a background thread** while the window appears.
- **Autosave writes on a background thread.**

With the stats panel open (`F3`): `F9` adds a 1M-point stress test (one `Ctrl+Z` removes it), and `F10` runs the benchmark.

## Code map

| File | What |
|---|---|
| `src/App.cpp` | Window, input, tools, commands, files, session |
| `src/Renderer.cpp` | D3D11/D2D setup, frame, grid/HUD/toolbar drawing, stroke GPU buffers |
| `src/StrokeShader.h` | Stroke vertex + pixel shader (HLSL) |
| `src/StrokeMesh.cpp` | Stroke → triangle strip (smoothing, joins, caps) |
| `src/StrokeBuilder.cpp` | Live stroke input: filtering, smoothing, simplification |
| `src/Board.cpp` | Strokes, undo/redo commands, eraser hit test |
| `src/SpatialIndex.cpp` | Hierarchical spatial hash |
| `src/Grid.cpp` | Adaptive grid |
| `src/Toolbar.cpp` | Toolbar layout / hit testing |
| `src/FileIO.cpp` | `.wbd` save/load, background writer |
| `tools/make_icon.ps1` | Regenerates `src/res/app.ico` |

Pen pressure isn't used yet. A pen works, but as a mouse with constant width.
