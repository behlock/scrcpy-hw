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

- **Theme**: defaults to the system's color-scheme preference. Triple-click
  the bottom-right 80x80px corner of the window to cycle
  *auto -> dark -> light -> auto*. Choice persists across reloads
  (`localStorage`).
- **Resize**: the SVG scales to the window, preserving aspect ratio.
- **Close**: closing the scrcpy window also tears down the sidecar (SIGTERM)
  and the Chrome app-mode window's profile dir.

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
