# Fix Stuck Capture Border

**Version 1.2.0.0 — 2026-09-18**
Copyright © 2026 Rekow IT

A single-file native Win32 tool (C17, no runtime dependencies) that detects a
stuck Windows Graphics Capture border — the yellow "your screen is being
captured" frame that sometimes stays on screen after a capture session ends —
identifies the processes likely to own it, and clears it.

---

## How it works

The border is drawn by `dwm.exe` on behalf of an open
`Windows.Graphics.Capture` session. It is not an enumerable window, so there is
no API that can be asked about it. Instead the tool reads the outer pixels of
every monitor with plain GDI (`BitBlt` into a DIB section) and looks for a
saturated yellow line running along the edges.

The probe uses GDI only. It does **not** open a capture session, so it cannot
itself cause the condition it looks for.

A screen edge counts as *hot* when at least `EdgeCoverage` (default 70 %) of the
sampled positions along it contain a matching pixel; a monitor counts as
*detected* when at least `MinHotEdges` (default 3) of its four edges are hot.

## Using it

1. **Probe now** — reads all four edges of every monitor and reports the
   verdict plus per-edge coverage (`T/B/L/R`). *Auto-probe every 10s* repeats it.
2. **Manual override: I see the frame** — on some systems the border is excluded
   from GDI capture output, so the probe reports clean although the frame is
   plainly visible. Ticking this enables the fix buttons regardless of the
   verdict.
3. **Candidate processes** — every process that has a `GraphicsCapture*.dll`
   loaded or is a known capture host, ranked. Rows highlighted in yellow have
   the capture DLL loaded and are the strongest suspects. Processes whose module
   list cannot be read (elevated or protected) are listed as
   `modules unreadable`.
4. Clear it, escalating only as far as needed:
   * **Kill selected** — end the suspected owner. Killing `explorer` is handled
     specially: the shell is restarted if Windows does not bring it back.
   * **Restart Explorer (primary fix)** — resolves the majority of cases.
   * **Restart DWM (escalation)** — terminates `dwm.exe`; the screen blanks for
     a second or two and DWM restarts automatically. **Requires running the tool
     as administrator**, otherwise the button stays disabled and reads
     *needs admin*. Over a remote session the connection may drop briefly.

The fix buttons stay disabled until either a probe detects a border or the
manual override is ticked.

## Notification area

The tray icon is present for as long as the tool runs and doubles as the
status light:

| Icon | Meaning |
| --- | --- |
| grey frame | resting — nothing detected, or not probed yet |
| yellow frame | the last probe found a capture frame |

The tooltip carries the same verdict, naming the monitor when one is found, so
the tool can sit in the tray with auto-probe on and report without a window.

* **Single left click** — show the window.
* **Double left click** — go straight for the primary fix: restart Explorer.
  No confirmation, so treat the icon with the same care as the button.
* **Right click** — the menu:
  * **Show window**
  * **Start minimised to tray** — start with the window hidden (`StartInTray`
    in the config)
  * **Start with Windows** — writes `FixStuckCaptureBorder` to
    `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`. Per user, no
    elevation, no scheduled task. The two options are independent: enable both
    for a tool that comes up silently in the tray at logon.
  * **About**
  * **Exit**

Minimising sends the window to the tray instead of the taskbar. Closing the
window still exits the program — use **Exit** or the close button, they do the
same thing.

The icon survives an Explorer restart, including one the tool performs itself:
`TaskbarCreated` is handled and the icon re-added.

## Dark mode

On first run the app follows the Windows *Apps mode* setting and keeps
following it, live — switching Windows to dark repaints the window without a
restart. Ticking or clearing **Dark mode** overrides that permanently and is
remembered in the config; delete the `DarkMode` key (or set it to `-1`) to go
back to following Windows.

Windows never exposed dark common controls through a public API, so this uses
the private `uxtheme.dll` ordinals available from Windows 10 1809 (build
17763). On anything older the checkbox is disabled and the app stays light.
Push buttons, check boxes and the group box have no usable dark theme and are
painted by hand — a themed check box draws its own label and ignores the colour
the parent supplies, so box and label are both drawn — as is the list view
header; grid lines and 3D client edges are dropped
in dark mode because they are drawn in a fixed light colour.

## Calibration (train on this PC)

The built-in heuristic is tuned for the standard yellow frame. If your machine
draws a different colour, or the probe is unreliable, teach it:

1. With **no** frame visible, press **1. Baseline (NO frame visible)**. Every
   colour currently present at the screen edges is recorded as "normal".
2. Reproduce the stuck frame, then press **2. Learn frame (frame VISIBLE)**.
   The most common colour at the edges that was *not* in the baseline becomes
   the border colour; it is shown in the swatch and matched from then on within
   `Tolerance` (euclidean RGB distance, default 30).
3. **Reset** returns to the built-in heuristic and clears the baseline.

Both steps write the config immediately.

## Configuration

`StuckCaptureBorder.config.json` is written next to `capturefix.exe` when that
folder is writable, otherwise to `%APPDATA%\StuckCaptureBorder\`. The path in
use is shown under the calibration buttons.

| Key | Default | Meaning |
| --- | --- | --- |
| `BaselineKeys` | `[]` | Quantised colours (`r-g-b`, each 0–7) seen at the edges with no frame |
| `FrameColor` | `null` | Learned border colour `{R,G,B}`; `null` = use the built-in yellow heuristic |
| `Tolerance` | `30` | Euclidean RGB distance for a pixel to count as a match |
| `BandPx` | `6` | How deep from each edge to sample |
| `SampleStride` | `8` | Sample every Nth pixel along an edge |
| `EdgeCoverage` | `0.7` | Fraction of an edge that must match for it to be hot |
| `MinHotEdges` | `3` | Hot edges needed to call a border detected |
| `DarkMode` | `-1` | `-1` follow the Windows app mode, `0` light, `1` dark |
| `StartInTray` | `0` | `1` starts with the window hidden in the tray |
| `CalibratedOn` | `null` | Timestamp of the last successful step 2 |

There is no UI for the numeric thresholds; edit the file and restart the tool.

## Building

```
build.bat
```

Prefers MSVC (VS 2022 Build Tools, `vcvars64.bat`), falls back to MinGW-w64
gcc + windres. Both build clean at `/W4` and `-Wall -Wextra`. Output:
`capturefix.exe`, x64, statically linked CRT under MSVC, ~200 KB.

### Files

| File | |
| --- | --- |
| `capturefix.c` | The whole program |
| `theme.c` / `theme.h` | Dark mode: uxtheme ordinals plus the hand-painted controls |
| `resource.h` | Shared resource ids and version constants |
| `capturefix.rc` | Icon, manifest, version info |
| `capturefix.manifest` | Common Controls 6, `dpiAware`, `asInvoker` |
| `capturefix.ico` | Application icon, also the tray icon when a frame is detected |
| `capturefix_gray.ico` | Desaturated copy, the tray icon at rest |
| `build.bat` | MSVC / MinGW build |

## Notes and limitations

* The process is system-DPI aware before any GDI work, so edge strips are
  sampled at true screen coordinates on high-DPI displays. Per-monitor DPI is
  not used: probing reads the virtual screen, which needs no scaling.
* Probing, process scanning, calibration and the kill/restart actions run on a
  worker thread, so the window stays responsive.
* The module scan cannot see inside elevated or protected processes unless the
  tool itself runs elevated — that is what `modules unreadable` means.
* A false negative is expected on systems that exclude the border from GDI
  capture output; that is what the manual override is for.
* Killing the wrong process is on you: the ranking is a heuristic, not proof of
  ownership.
