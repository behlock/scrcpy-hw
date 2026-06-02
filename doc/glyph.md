# --glyph: Mirror Nothing phone glyph LED state on the host

When the `--glyph` flag is passed, scrcpy opens a second window on the host
that shows the live state of the back-glyph LEDs of the connected Nothing
phone, alongside the regular front-screen mirror.

```
./run x --glyph -s <serial>
```

This is **independent** of the front-screen mirror and of `--web-share`:

- The front-screen mirror is unchanged.
- `--web-share` continues to stream **only the front screen** to remote
  viewers. The host-side glyph window is never exposed over the network.

## Supported devices

| `ro.product.model` | Phone               | Frame size  | Rendering              |
| ------------------ | ------------------- | ----------- | ---------------------- |
| `A065`             | Phone (2)           | 33 zones    | Per-zone SVG paths     |
| `A069P`            | Phone (4a) Pro      | 137 dots    | Per-dot diamond matrix |

The device's model is detected automatically via
`adb shell getprop ro.product.model` and the matching renderer + SVG variants
are served.

## How it works

```
+-------------------+    fork()+execvp(python3)
|  scrcpy --glyph   | ----------------------+
+-------------------+                       |
                                            v
                              +-----------------------------+
                              |  glyph-sidecar/             |
                              |    glyph_sidecar.py         |
                              |                             |
                              |  - adb logcat NtGlyph...    |
                              |  - parse setLightFrame[N]   |
                              |  - SSE on /events           |
                              |  - serve SVG-based HTML     |
                              +--------------+--------------+
                                             |
                                             | open <url>
                                             v
                              +-----------------------------+
                              | Chrome --app=<url>          |
                              | (chromeless window)         |
                              +-----------------------------+
```

The sidecar is a **passive observer** — it reads the system service log on
the phone (`NtGlyphServiceImpl`), which captures the frame data every app
sends to the glyphs (notifications, audio-reactive apps, the Nothing
assistant, custom apps you build, etc.). No companion app is installed on
the phone.

## Catching fast patterns (≈60 Hz)

Glyph Composer ringtones drive the LEDs at ~60 frames/sec, with nearly every
frame changing brightness (a 16s clip can be ~1000 distinct frames, ~95% of
them differing from the one before). `adb logcat` delivers that rate fine —
the bottleneck used to be downstream, so the mirror visibly *smeared* fast
modulation. The pipeline is built to track it 1:1:

- **Coalesce, never backlog.** Each SSE client holds only the *newest* frame
  (`Subscriber`). A browser that briefly lags skips intermediate frames and
  snaps to the current LED state instead of replaying a stale queue — and it
  can never back-pressure the logcat reader.
- **Render in the SSE handler, not on rAF.** The browser paints each frame as
  it arrives over SSE. We deliberately do *not* drive rendering from
  `requestAnimationFrame`: a glyph window sitting next to (or behind) the
  scrcpy mirror is unfocused/occluded, and browsers throttle or fully pause
  rAF for hidden windows — which froze the mirror. The SSE `onmessage`
  callback keeps firing regardless of window focus.
- **No CSS transition.** A `transition` on `fill`/`filter` would low-pass
  the 60 Hz signal into mush *and* re-rasterize the glow blur every frame it
  is mid-transition. Removed — changes are applied instantly.
- **Only the visible variant, only moved zones.** Each frame touches just the
  active theme variant (the other is `display:none`) and skips any zone whose
  quantised brightness is unchanged, so a heavy 137-dot Phone (4a) Pro frame
  stays inside the 16 ms budget.

## Phone (1)-format (5-zone) compositions

Some ringtones — including Glyph Composer tracks authored for Phone (1) — play
on a Phone (2) as a simplified **5-channel glyph-group** frame:
`setLightFrame[..] frameColors[5] [...]` instead of the per-segment
`frameColors[33]`. The reader used to accept only the native size and silently
dropped these, so such ringtones lit the *phone* but never the *mirror* (a
native 33-zone composition like a Phone (2) track worked fine — hence the
"this one shows, that one doesn't" asymmetry).

The reader now also accepts the 5-channel frame and expands it onto the 33
render zones via `PHONE2_GROUP_ZONES` (group → zone indices, from the Glyph
Developer Kit Phone (2) table). Channel order is the Glyph Composer A..E order;
if a glyph group lights in the wrong place, reorder that list. `_make_log_reader`
takes a `{size: expander}` map, so other sizes/models can be added the same way.

## Building and running

```
meson setup x --buildtype=release
ninja -C x
./run x --glyph -s <serial>
```

To silence scrcpy's audio-buffer debug noise, add `--verbosity=info`:

```
./run x --glyph --verbosity=info -s <serial>
```

## UI

- **Theme**: defaults to a dark appearance (glowing white glyphs on black —
  the natural look for LEDs) regardless of the host's system color scheme.
  Triple-click the bottom-right 80x80px corner of the window to cycle
  *auto -> dark -> light -> auto* (auto == dark). Choice persists across
  reloads (`localStorage`). Note: the renderer must agree on the visible
  variant in auto mode — the CSS shows the dark-bodied phone, so the JS
  paints the dark variant too; following the system here would paint the
  hidden light variant on a light-mode host and leave the glyphs unlit.
- **Resize**: the SVG scales to the window, preserving aspect ratio.
- **Close**: closing the scrcpy window tears down the sidecar (SIGTERM), and
  the sidecar in turn closes its app-mode browser window and removes its
  throwaway `/tmp/glyph-mirror-*` profile dir. Stale profiles from a hard
  kill are reaped on the next launch.

## Files

| Path                                       | Role                                              |
| ------------------------------------------ | ------------------------------------------------- |
| `app/src/cli.c`, `app/src/options.{c,h}`   | `--glyph` flag wiring                             |
| `app/src/scrcpy.c`                         | Spawn/stop sidecar around the main session        |
| `app/src/glyph_sidecar.{c,h}`              | `fork()`+`execvp()` Python; SIGTERM on shutdown   |
| `run`                                      | Exports `SCRCPY_GLYPH_SIDECAR` env var            |
| `glyph-sidecar/glyph_sidecar.py`           | Logcat tailer + SSE server + Chrome app launcher  |
| `glyph-sidecar/phone2-{dark,light}.svg`    | Phone (2) artwork (dark + light theme variants)   |
| `glyph-sidecar/phone4apro-off-*.svg`       | Phone (4a) Pro isometric (dark + light variants)  |

## Environment variables (sidecar)

| Var                       | Purpose                                                                   |
| ------------------------- | ------------------------------------------------------------------------- |
| `SCRCPY_GLYPH_SIDECAR`    | Absolute path to `glyph_sidecar.py`. Set by `./run`.                      |
| `SCRCPY_GLYPH_PYTHON`     | Override the Python interpreter. Defaults to `/usr/bin/python3` on macOS. |

## Limitations

- Only Phone (2) and Phone (4a) Pro are mapped today. Adding another model
  is roughly: drop the SVG into `glyph-sidecar/`, add a path-or-dot tagger,
  and add an entry to `PROFILES` with the expected `frameColors[N]` size.
- The spatial mapping of frame indices -> SVG paths is hand-derived from
  document order and may be off for some elements. Iterate on the
  `PATH_TO_ZONE` array (Phone 2) or document-order tagging (Phone 4a Pro)
  if dots light up in the wrong positions.
- Windows is not supported (sidecar uses POSIX `fork`/`execvp`).
