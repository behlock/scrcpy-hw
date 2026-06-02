#!/usr/bin/env python3
"""
glyph_sidecar.py — host-side viewer for Nothing phone glyph LEDs.

Spawned by scrcpy when `--glyph` is passed. Tails `adb logcat` for the
system's `NtGlyphServiceImpl: setLightFrame[<id>] frameColors[<N>] [...]`
lines and re-broadcasts each frame as Server-Sent Events to a tiny localhost
HTML page that renders the phone's back illustration with per-zone (Phone 2)
or per-dot (Phone 4a Pro) glow. The page is opened in a chromeless
Chrome/Brave/Edge app-mode window (falls back to the default browser).

Supported devices (detected via `ro.product.model`):
  * A065  → Phone (2)        — 33-element frames
  * A069P → Phone (4a) Pro   — 137-element matrix frames

No on-device companion app is required: we observe whatever app is currently
driving the glyphs (notifications, audio-reactive, the Nothing assistant,
etc.) by passively reading the system log.
"""

from __future__ import annotations

import argparse
import glob
import http.server
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from typing import Optional

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

HTTP_PORT_DEFAULT = 33134    # the browser connects here

# Both Phone (2) and Phone (4a) Pro log per-frame state through
# NtGlyphServiceImpl with a `setLightFrame[<lightId>] frameColors[<N>] [...]`
# line. We match generically and dispatch by frame size to the right renderer.
GLYPH_LOGCAT_TAG = "NtGlyphServiceImpl"
GLYPH_LOGCAT_LINE_RE = re.compile(
    r"setLightFrame\[\d+\]\s+frameColors\[(\d+)\]\s+\[([0-9,\s\-]+)\]"
)
PHONE2_FRAME_SIZE = 33
PHONE4APRO_FRAME_SIZE = 137
ZONE_COUNT = PHONE2_FRAME_SIZE  # back-compat for code that still uses it

# Compositions authored for Phone (1) — including Glyph Composer ringtones —
# are played back as a simplified 5-channel "glyph group" frame
# (`setLightFrame[..] frameColors[5] [...]`) instead of the per-segment
# `frameColors[33]`. Each channel drives one whole Phone (2) glyph group. The
# native reader filtered to size 33 and silently dropped these, so such
# ringtones lit the phone but never the mirror. We expand a 5-channel frame
# onto the 33 render zones so it mirrors like the native path.
#
# Group → Phone (2) zone indices are from the Glyph Developer Kit "Nothing
# Phone (2)" table: A (camera) = 0,1 · B (diagonal) = 2 · C (circle) = 3..23 ·
# E (bottom dot) = 24 · D (USB line) = 25..32. Channel order is the Glyph
# Composer A..E order. If a glyph group lights in the wrong place, reorder this
# list (same spirit as the PATH_TO_ZONE caveat above).
PHONE2_GROUP_ZONES = [
    [0, 1],                  # ch0 = A  (camera)
    [2],                     # ch1 = B  (diagonal accent)
    list(range(3, 24)),      # ch2 = C  (the big circle: C1_1..C1_16, C2..C6)
    list(range(25, 33)),     # ch3 = D  (USB line: D1_1..D1_8)
    [24],                    # ch4 = E  (bottom-centre dot)
]
PHONE2_GROUP_FRAME_SIZE = len(PHONE2_GROUP_ZONES)  # 5


def _expand_phone2_groups(vals: list[int]) -> list[int]:
    """Map a 5-channel glyph-group frame onto a 33-zone Phone (2) frame by
    copying each channel's value to every zone in that group."""
    out = [0] * PHONE2_FRAME_SIZE
    for ch, zones in enumerate(PHONE2_GROUP_ZONES):
        v = vals[ch]
        for z in zones:
            out[z] = v
    return out


# ---------- Phone (2) renderer (uses vendored SVG of the back of the phone) ----

# Map each white-filled path in `phone2.svg` (in document order, inside the
# masked group only) to its addressable zone index 0..32. Derived by reading
# each path's bounding-box and matching to the SDK's Phone (2) layout (see
# ~/projects/Glyph-Developer-Kit/README.md and Frame6-transparent.png).
PATH_TO_ZONE = [
    1,    # L8   A2          (lower curl of upper-left U)
    0,    # L9   A1          (upper arc of upper-left U)
    20,   # L10  C3          (short vertical bar, left edge, y≈190-245)
    23,   # L11  C6          (short vertical bar, right edge, y≈245-290)
    19,   # L12  C2          (curve from upper-mid to outer-left, y≈131-173)
    21,   # L13  C4          (large sweep, bottom-left, y≈317-368)
    22,   # L14  C5          (large sweep, bottom-right, y≈317-358)
    24,   # L15  E1          (small pill at bottom-center, y≈455-467)
    2,    # L16  B1          (diagonal accent, top-right)
    31,   # L17  D1_7        (y 400-407, second from top)
    32,   # L18  D1_8        (y 393-400, topmost)
    30,   # L19  D1_6        (y 407-414)
    26,   # L20  D1_2        (y 435-442)
    25,   # L21  D1_1        (y 442-449, bottommost — SDK "D1_1 is bottom")
    27,   # L22  D1_3        (y 428-435)
    28,   # L23  D1_4        (y 421-428)
    29,   # L24  D1_5        (y 414-421)
    14,   # L25  C1_12       (127, 120)
    13,   # L26  C1_11       (136, 121)
    12,   # L27  C1_10       (144, 123)
    15,   # L28  C1_13       (118, 120)
    17,   # L29  C1_15       (101, 120)
    18,   # L30  C1_16       ( 96, 121)
    16,   # L31  C1_14       (110, 120)
    11,   # L32  C1_9        (152, 125)
    5,    # L33  C1_3        (197, 151)
    4,    # L34  C1_2        (203, 157)
    6,    # L35  C1_4        (190, 145)
    3,    # L36  C1_1        (209, 163) — SDK "bottom-right" anchor
    8,    # L37  C1_6        (176, 136)
    7,    # L38  C1_5        (183, 140)
    9,    # L39  C1_7        (168, 132)
    10,   # L40  C1_8        (161, 128)
]
assert len(PATH_TO_ZONE) == 33
assert sorted(PATH_TO_ZONE) == list(range(33))


# ---------- SVG loading helpers ----------------------------------------------

def _read_svg(name: str) -> str:
    path = os.path.join(SCRIPT_DIR, name)
    try:
        with open(path, "r", encoding="utf-8") as f:
            return f.read()
    except FileNotFoundError:
        return ""


# Hidden theme-override gesture: triple-click in the bottom-right corner
# (no visible UI) cycles auto → dark → light → auto. Choice persists in
# localStorage. Renderers concatenate these into their templates.

THEME_CSS = """
  /* Invisible target zone for the triple-click gesture. */
  .theme-gesture {
    position: fixed; right: 0; bottom: 0;
    width: 80px; height: 80px;
    cursor: default; z-index: 9999;
  }
"""

THEME_HTML = '<div class="theme-gesture" id="theme-gesture"></div>'

THEME_JS = """
  const _html = document.documentElement;
  function _applyTheme() {
    const v = localStorage.getItem('glyph-theme');
    if (v === 'dark' || v === 'light') _html.setAttribute('data-theme', v);
    else _html.removeAttribute('data-theme');
  }
  let _clicks = 0;
  let _clickTimer = 0;
  document.getElementById('theme-gesture').addEventListener('click', () => {
    _clicks++;
    clearTimeout(_clickTimer);
    _clickTimer = setTimeout(() => { _clicks = 0; }, 700);
    if (_clicks >= 3) {
      _clicks = 0;
      clearTimeout(_clickTimer);
      const cur = localStorage.getItem('glyph-theme') || 'auto';
      const next = {auto: 'dark', dark: 'light', light: 'auto'}[cur];
      if (next === 'auto') localStorage.removeItem('glyph-theme');
      else localStorage.setItem('glyph-theme', next);
      _applyTheme();
    }
  });
  _applyTheme();
"""


def _tag_phone2_paths(raw: str) -> str:
    """Add `class="z" data-zone="<idx>"` to each glyph path inside the SVG's
    top-level `<g>`. Same path structure used by both Phone (2) theme variants."""
    if not raw:
        return raw
    m = re.search(r'<g[^>]*>', raw)
    if not m:
        return raw
    start = m.end()
    end_rel = raw[start:].find("</g>")
    if end_rel < 0:
        return raw
    end = start + end_rel

    counter = {"i": 0}

    def _tag(match: "re.Match[str]") -> str:
        idx = counter["i"]
        counter["i"] += 1
        zone = PATH_TO_ZONE[idx] if idx < len(PATH_TO_ZONE) else -1
        return f'fill="white" class="z" data-zone="{zone}"'

    return raw[:start] + re.sub(r'fill="white"', _tag, raw[start:end]) + raw[end:]


# ---------- Phone (2) renderer (dual theme) ----------------------------------

def render_html_phone2() -> str:
    """Phone (2) viewer. Two SVG variants are embedded — and the browser's
    `prefers-color-scheme` decides which is visible.

    Cutout inversion: in dark mode we show the DARK-bodied phone (so it reads
    as a black silhouette against the dark page bg, with white glyphs
    standing out); in light mode we show the LIGHT-bodied phone."""
    svg_for_dark  = _tag_phone2_paths(_read_svg("phone2-light.svg"))  # dark body
    svg_for_light = _tag_phone2_paths(_read_svg("phone2-dark.svg"))   # light body
    return f"""<!doctype html>
<html><head><meta charset="utf-8"><title>Phone (2) Glyphs</title>
<style>
  :root {{ color-scheme: dark; }}
  html, body {{ margin: 0; padding: 0;
    font-family: -apple-system, system-ui, sans-serif;
    height: 100%; overflow: hidden;
    background: #000; color: #ddd; }}
  /* Manual overrides (always win). */
  :root[data-theme="light"], :root[data-theme="light"] body {{
    background: #f4f4f4; color: #1a1a1a;
  }}
  :root[data-theme="dark"], :root[data-theme="dark"] body {{
    background: #000; color: #ddd;
  }}
  .stage {{ width: 100vw; height: calc(100vh - 32px); padding: 16px 0;
    position: relative; }}
  .variant {{ width: 100%; height: 100%; }}
  .variant svg {{ width: 100%; height: 100%; display: block; overflow: visible; }}
  /* Default = dark mode appearance regardless of system. */
  #variant-light {{ display: none; }}
  :root[data-theme="light"] #variant-dark  {{ display: none; }}
  :root[data-theme="light"] #variant-light {{ display: block; }}
  :root[data-theme="dark"] #variant-light {{ display: none; }}
  :root[data-theme="dark"] #variant-dark  {{ display: block; }}
  /* Default (off) — dim shade; JS replaces fill+filter when zone is on.
     No CSS transition: the source modulates at ~60 Hz, so a 30ms transition
     would low-pass-filter (smear) fast changes into mush, and animating the
     drop-shadow re-rasterizes the blur on every frame it's mid-transition
     (i.e. constantly). We track the source 1:1 and let the rAF loop pace it. */
  #variant-dark  svg path.z {{ fill: #262626; }}
  #variant-light svg path.z {{ fill: #d8d8d8; }}
{THEME_CSS}
</style></head>
<body>
  {THEME_HTML}
  <div class="stage" id="stage">
    <div class="variant" id="variant-dark">{svg_for_dark}</div>
    <div class="variant" id="variant-light">{svg_for_light}</div>
  </div>
<script>
{THEME_JS}
  document.querySelectorAll('#stage svg').forEach(s => {{
    s.setAttribute('viewBox', '0 0 230 490');
    s.removeAttribute('width');
    s.removeAttribute('height');
    s.setAttribute('preserveAspectRatio', 'xMidYMid meet');
  }});

  const darkMQ = window.matchMedia('(prefers-color-scheme: dark)');

  // Group SVG paths by zone within each variant.
  function byZone(rootSelector) {{
    const map = new Map();
    for (const p of document.querySelectorAll(rootSelector + ' svg path.z')) {{
      const z = parseInt(p.getAttribute('data-zone'), 10);
      if (!Number.isFinite(z) || z < 0) continue;
      if (!map.has(z)) map.set(z, []);
      map.get(z).push(p);
    }}
    return map;
  }}
  const byZoneDark  = byZone('#variant-dark');
  const byZoneLight = byZone('#variant-light');

  function styleForDark(v) {{
    const vg = Math.pow(Math.min(1, v * 2.2), 0.4);
    if (vg <= 0.02) return {{ fill: '#262626', filter: '' }};
    const lvl = Math.round(200 + vg * 55);
    return {{
      fill: `rgb(${{lvl}},${{lvl}},${{lvl}})`,
      filter: `drop-shadow(0 0 ${{4 + vg * 10}}px rgba(255,255,255,${{0.7 + vg * 0.3}})) `
            + `drop-shadow(0 0 ${{12 + vg * 22}}px rgba(255,255,255,${{0.45 + vg * 0.4}})) `
            + `drop-shadow(0 0 ${{28 + vg * 40}}px rgba(255,255,255,${{0.25 + vg * 0.35}}))`,
    }};
  }}
  function styleForLight(v) {{
    // In light mode the glyphs are white/light by default (matches the
    // physical LEDs being off-invisible against the phone body). When lit
    // we keep them white but add a soft dark halo for visible depth.
    const vg = Math.pow(Math.min(1, v * 2.2), 0.4);
    if (vg <= 0.02) return {{ fill: '#d8d8d8', filter: '' }};
    return {{
      fill: '#ffffff',
      filter: `drop-shadow(0 0 ${{3 + vg * 8}}px rgba(0,0,0,${{0.45 + vg * 0.3}})) `
            + `drop-shadow(0 0 ${{10 + vg * 18}}px rgba(0,0,0,${{0.25 + vg * 0.3}}))`,
    }};
  }}

  // --- Render --------------------------------------------------------------
  // Paint each frame as it arrives over SSE. The server only ever holds the
  // newest frame (see Subscriber), so a momentarily-slow client coalesces to
  // the latest instead of replaying a stale backlog. We render in the message
  // handler — NOT on requestAnimationFrame — because rAF is throttled/paused
  // when the window is unfocused or occluded (it sits next to the scrcpy
  // mirror), which made the glyph window look frozen. Each paint is cheap:
  // only the visible theme variant, and only zones whose quantised brightness
  // changed (no CSS transition, so changes are instant, not smeared).
  function effectiveDark() {{
    const t = _html.getAttribute('data-theme');
    if (t === 'dark') return true;
    if (t === 'light') return false;
    // Auto (no manual theme): the CSS shows the dark-bodied variant on a
    // black page regardless of system appearance, so paint the dark variant
    // to match. Following darkMQ here would paint the hidden light variant on
    // a light-mode host, leaving the visible dark phone permanently unlit.
    return true;
  }}

  const applied = new WeakMap();   // path -> last brightness bucket painted
  function paint(map, styleFn, zones, force) {{
    for (const [z, plist] of map) {{
      const v = Math.max(0, Math.min(1, +zones[z] || 0));
      const key = Math.round(v * 64);          // 65 buckets: stable + cheap
      for (const p of plist) {{
        if (!force && applied.get(p) === key) continue;
        applied.set(p, key);
        const s = styleFn(v);
        p.style.fill = s.fill;
        p.style.filter = s.filter;
      }}
    }}
  }}

  let latestZones = [];
  let lastDark = null;
  function render(force) {{
    const dark = effectiveDark();
    if (dark !== lastDark) {{ force = true; lastDark = dark; }}
    paint(dark ? byZoneDark : byZoneLight,
          dark ? styleForDark : styleForLight,
          latestZones, force);
  }}

  const es = new EventSource('/events');
  es.onmessage = ev => {{
    try {{ latestZones = JSON.parse(ev.data).zones || []; render(false); }}
    catch (e) {{}}
  }};
  // Re-paint when the effective theme flips (system preference or the manual
  // triple-click gesture, which toggles data-theme on <html>).
  darkMQ.addEventListener('change', () => render(true));
  new MutationObserver(() => render(true))
    .observe(_html, {{ attributes: true, attributeFilter: ['data-theme'] }});
  render(true);   // initial off-state
</script>
</body></html>
"""


# ---------- Phone (4a) Pro renderer (isometric, dual theme) ------------------

_PHONE4APRO_DOT_RE = re.compile(
    r'<path d="(M[0-9. ]+(?:L[0-9. ]+)+Z)" fill="#1C1C1C"'
)


def _tag_phone4apro_dots(raw: str) -> str:
    """Tag each diamond LED path (M..L..L..L..L..Z, fill=#1C1C1C) in the
    (4a) Pro SVG with `class="z" data-idx="N"`. Non-diamond #1C1C1C paths
    (body details / curved accents) are intentionally NOT tagged — the SVG
    has 145 #1C1C1C paths in some variants but only 137 are actual LEDs."""
    if not raw:
        return raw
    counter = {"i": 0}

    def _tag(m):
        i = counter["i"]
        counter["i"] += 1
        return f'<path d="{m.group(1)}" fill="#1C1C1C" class="z" data-idx="{i}"'

    return _PHONE4APRO_DOT_RE.sub(_tag, raw)


def render_html_phone4apro() -> str:
    # We light each diamond independently using `data-idx` and the live
    # `setLightFrame[123] frameColors[137]` array from NtGlyphServiceImpl.
    # Cutout inversion: dark mode shows the dark-bodied isometric; light
    # mode shows the light-bodied one.
    svg_dark  = _tag_phone4apro_dots(_read_svg("phone4apro-off-light.svg"))
    svg_light = _tag_phone4apro_dots(_read_svg("phone4apro-off-dark.svg"))
    return f"""<!doctype html>
<html><head><meta charset="utf-8"><title>Phone (4a) Pro Glyphs</title>
<style>
  :root {{ color-scheme: dark; }}
  html, body {{ margin: 0; padding: 0;
    font-family: -apple-system, system-ui, sans-serif;
    height: 100%; overflow: hidden;
    background: #000; color: #ddd; }}
  :root[data-theme="light"], :root[data-theme="light"] body {{
    background: #f4f4f4; color: #1a1a1a;
  }}
  :root[data-theme="dark"], :root[data-theme="dark"] body {{
    background: #000; color: #ddd;
  }}
  .stage {{ position: relative; width: 100vw;
    height: calc(100vh - 32px); padding: 16px 0; }}
  .variant {{ width: 100%; height: 100%; }}
  .variant svg {{ width: 100%; height: 100%; display: block; overflow: visible; }}
  /* Default = dark mode appearance regardless of system. */
  #variant-light {{ display: none; }}
  :root[data-theme="light"] #variant-dark  {{ display: none; }}
  :root[data-theme="light"] #variant-light {{ display: block; }}
  :root[data-theme="dark"]  #variant-light {{ display: none; }}
  :root[data-theme="dark"]  #variant-dark  {{ display: block; }}
  /* Per-diamond styling. Off state is the SVG's intrinsic #1C1C1C; JS sets
     inline fill+filter when a dot is lit. No CSS transition — see the Phone (2)
     note: at ~60 Hz it smears fast changes and re-rasterizes the blur every
     frame. The rAF loop paces updates instead. */
{THEME_CSS}
</style></head>
<body>
  {THEME_HTML}
  <div class="stage" id="stage">
    <div class="variant" id="variant-dark">{svg_dark}</div>
    <div class="variant" id="variant-light">{svg_light}</div>
  </div>
<script>
{THEME_JS}
  for (const s of document.querySelectorAll('#stage svg')) {{
    s.removeAttribute('width');
    s.removeAttribute('height');
    s.setAttribute('preserveAspectRatio', 'xMidYMid meet');
  }}

  function byIdx(rootSel) {{
    const m = new Map();
    for (const p of document.querySelectorAll(rootSel + ' svg path.z')) {{
      const i = parseInt(p.getAttribute('data-idx'), 10);
      if (!Number.isFinite(i) || i < 0) continue;
      if (!m.has(i)) m.set(i, []);
      m.get(i).push(p);
    }}
    return m;
  }}
  const byIdxDark  = byIdx('#variant-dark');
  const byIdxLight = byIdx('#variant-light');

  function styleForDark(v) {{
    const vg = Math.sqrt(Math.min(1, v * 1.6));
    if (vg <= 0.02) return {{ fill: '#1C1C1C', filter: '' }};
    const lvl = Math.round(120 + vg * 135);
    return {{
      fill: `rgb(${{lvl}},${{lvl}},${{lvl}})`,
      filter: `drop-shadow(0 0 ${{3 + vg * 6}}px rgba(255,255,255,${{0.4 + vg * 0.5}}))`,
    }};
  }}
  function styleForLight(v) {{
    // In light mode the dots are dark by default. To make "lit" stand out
    // we flip to bright/white with a soft dark halo for depth.
    const vg = Math.sqrt(Math.min(1, v * 1.6));
    if (vg <= 0.02) return {{ fill: '#1C1C1C', filter: '' }};
    const lvl = Math.round(200 + vg * 55);   // 200 → 255 (near-white)
    return {{
      fill: `rgb(${{lvl}},${{lvl}},${{lvl}})`,
      filter: `drop-shadow(0 0 ${{2 + vg * 6}}px rgba(0,0,0,${{0.35 + vg * 0.3}}))`,
    }};
  }}

  // --- Render --------------------------------------------------------------
  // Same approach as Phone (2): paint each frame in the SSE message handler
  // (NOT on requestAnimationFrame, which is paused when the window is occluded
  // — that froze the mirror). Only the visible theme variant, and only dots
  // whose quantised brightness changed. With 137 dots the skip is what keeps
  // each paint cheap.
  const darkMQ = window.matchMedia('(prefers-color-scheme: dark)');
  function effectiveDark() {{
    const t = _html.getAttribute('data-theme');
    if (t === 'dark') return true;
    if (t === 'light') return false;
    // Auto (no manual theme): the CSS shows the dark-bodied variant on a
    // black page regardless of system appearance, so paint the dark variant
    // to match. Following darkMQ here would paint the hidden light variant on
    // a light-mode host, leaving the visible dark phone permanently unlit.
    return true;
  }}

  const applied = new WeakMap();   // path -> last brightness bucket painted
  function paint(map, styleFn, zones, force) {{
    for (const [i, plist] of map) {{
      const v = Math.max(0, Math.min(1, +zones[i] || 0));
      const key = Math.round(v * 64);
      for (const p of plist) {{
        if (!force && applied.get(p) === key) continue;
        applied.set(p, key);
        const s = styleFn(v);
        p.style.fill = s.fill;
        p.style.filter = s.filter;
      }}
    }}
  }}

  let latestZones = [];
  let lastDark = null;
  function render(force) {{
    const dark = effectiveDark();
    if (dark !== lastDark) {{ force = true; lastDark = dark; }}
    paint(dark ? byIdxDark : byIdxLight,
          dark ? styleForDark : styleForLight,
          latestZones, force);
  }}

  const es = new EventSource('/events');
  es.onmessage = ev => {{
    try {{ latestZones = JSON.parse(ev.data).zones || []; render(false); }}
    catch (e) {{}}
  }};
  darkMQ.addEventListener('change', () => render(true));
  new MutationObserver(() => render(true))
    .observe(_html, {{ attributes: true, attributeFilter: ['data-theme'] }});
  render(true);   // initial off-state
</script>
</body></html>
"""


# ---------- SSE broadcaster ----------------------------------------------------

class Subscriber:
    """Single-slot latest-frame mailbox for one SSE client.

    The glyph source runs at ~60 Hz with nearly every frame changing. If a
    client (browser tab) momentarily falls behind, replaying a queued backlog
    would only ever show stale LED states. So we keep just the *newest* frame:
    push() overwrites, get() returns it once. A slow client therefore skips
    intermediate frames and snaps to the current state — exactly right for a
    realtime mirror, and it means a slow client can never back-pressure the
    logcat reader or build an unbounded queue."""

    __slots__ = ("_cond", "_latest", "_pending")

    def __init__(self) -> None:
        self._cond = threading.Condition()
        self._latest: Optional[bytes] = None
        self._pending = False

    def put(self, payload: bytes) -> None:
        with self._cond:
            self._latest = payload      # coalesce: newest wins
            self._pending = True
            self._cond.notify()

    def get(self, timeout: float) -> Optional[bytes]:
        """Block until a frame is available, returning it; or return None if
        `timeout` elapses first (the caller then sends an SSE keepalive)."""
        with self._cond:
            if not self._pending:
                self._cond.wait(timeout)
            if not self._pending:
                return None
            self._pending = False
            return self._latest


class Broadcaster:
    """Thread-safe fan-out: every push() goes to every connected SSE client."""

    def __init__(self):
        self.lock = threading.Lock()
        self.subscribers: list[Subscriber] = []
        self.last: Optional[bytes] = None

    def subscribe(self) -> Subscriber:
        s = Subscriber()
        with self.lock:
            self.subscribers.append(s)
            if self.last is not None:
                s.put(self.last)
        return s

    def unsubscribe(self, s: Subscriber) -> None:
        with self.lock:
            try:
                self.subscribers.remove(s)
            except ValueError:
                pass

    def push(self, payload_json: bytes) -> None:
        with self.lock:
            self.last = payload_json
            for s in self.subscribers:
                s.put(payload_json)


# ---------- HTTP server --------------------------------------------------------

def make_handler(html: str, broadcaster: Broadcaster):
    class Handler(http.server.BaseHTTPRequestHandler):
        # Silence logging — sidecar runs unattended.
        def log_message(self, fmt, *args): pass

        def do_GET(self):
            if self.path == "/" or self.path == "/index.html":
                body = html.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            if self.path == "/events":
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-cache")
                self.send_header("Connection", "keep-alive")
                self.end_headers()
                sub = broadcaster.subscribe()
                try:
                    while True:
                        payload = sub.get(timeout=15)
                        if payload is None:
                            # keepalive comment
                            self.wfile.write(b": ping\n\n")
                            self.wfile.flush()
                        else:
                            self.wfile.write(b"data: ")
                            self.wfile.write(payload)
                            self.wfile.write(b"\n\n")
                            self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    pass
                finally:
                    broadcaster.unsubscribe(sub)
                return
            self.send_response(404)
            self.end_headers()
    return Handler


# ---------- phone bridge -------------------------------------------------------

@dataclass
class Sidecar:
    serial: str

    def adb(self) -> list[str]:
        cmd = ["adb"]
        if self.serial:
            cmd += ["-s", self.serial]
        return cmd

    def detect_model(self) -> str:
        """Return the device's `ro.product.model` (e.g. "A065", "A069P") or "" if
        the query fails. Used to pick the appropriate renderer."""
        try:
            out = subprocess.run(
                self.adb() + ["shell", "getprop", "ro.product.model"],
                capture_output=True, text=True, timeout=5,
            )
            return out.stdout.strip()
        except Exception:
            return ""

    def spawn_logcat(self, tag: str = GLYPH_LOGCAT_TAG) -> subprocess.Popen:
        # -T 1 starts from "now" (skips backlog).
        return subprocess.Popen(
            self.adb() + ["shell", "logcat", "-T", "1", f"{tag}:D", "*:S"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            bufsize=1,
            text=True,
        )


# ---------- Per-device profile -----------------------------------------------

# Map `ro.product.model` → (display name, html renderer fn, log reader fn or None).
# The log reader, if any, is invoked on a background thread and pushes JSON
# payloads to the broadcaster. None means "no live state mirror yet — static
# illustration only".

@dataclass
class Profile:
    name: str
    render_html: "callable"
    log_reader: "callable | None"


def _make_log_reader(native_size: int,
                     expanders: "dict[int, callable] | None" = None):
    """Build a reader that tails logcat and emits {zones:[...0..1...], ts:<ms>}
    JSON frames of `native_size` length.

    `native_size` frames are used as-is. `expanders` maps any *other* accepted
    frame size to a function that expands that frame to `native_size` (e.g. a
    5-channel glyph-group frame → 33 zones on Phone (2)). Frames whose size is
    neither native nor in `expanders` are ignored (the device may drive several
    independent lights in parallel)."""
    expanders = expanders or {}
    accepted = {native_size, *expanders}

    def _reader(sidecar: Sidecar, broadcaster: "Broadcaster",
                stop: threading.Event) -> None:
        while not stop.is_set():
            proc = None
            try:
                proc = sidecar.spawn_logcat(GLYPH_LOGCAT_TAG)
            except Exception as e:
                print(f"glyph_sidecar: logcat spawn failed: {e}", file=sys.stderr)
                time.sleep(2)
                continue
            assert proc.stdout is not None
            max_seen = 1
            try:
                for line in proc.stdout:
                    if stop.is_set():
                        break
                    m = GLYPH_LOGCAT_LINE_RE.search(line)
                    if not m:
                        continue
                    n_logged = int(m.group(1))
                    if n_logged not in accepted:
                        continue
                    try:
                        vals = [int(x) for x in m.group(2).split(",")
                                if x.strip() != ""]
                    except ValueError:
                        continue
                    if len(vals) != n_logged:
                        continue
                    if n_logged != native_size:
                        vals = expanders[n_logged](vals)   # → native_size
                    m_local = max(vals)
                    if m_local > max_seen:
                        max_seen = m_local
                    norm = [round(v / max_seen, 3) if max_seen > 0 else 0.0
                            for v in vals]
                    payload = json.dumps(
                        {"zones": norm, "ts": int(time.time() * 1000)}
                    ).encode("utf-8")
                    broadcaster.push(payload)
            except Exception as e:
                print(f"glyph_sidecar: logcat read error: {e}", file=sys.stderr)
            finally:
                if proc is not None:
                    try:
                        proc.terminate()
                        proc.wait(timeout=1)
                    except Exception:
                        pass
            if not stop.is_set():
                time.sleep(0.5)
    return _reader


PROFILES: dict[str, Profile] = {
    "A065":  Profile("Phone (2)",      render_html_phone2,
                     _make_log_reader(PHONE2_FRAME_SIZE,
                                      {PHONE2_GROUP_FRAME_SIZE: _expand_phone2_groups})),
    "A069P": Profile("Phone (4a) Pro", render_html_phone4apro,  _make_log_reader(PHONE4APRO_FRAME_SIZE)),
}


def pick_profile(model: str) -> Profile:
    if model in PROFILES:
        return PROFILES[model]
    # Unknown model: default to Phone (2) which is the most universally lit.
    print(f"glyph_sidecar: unknown device model {model!r}; defaulting to "
          f"Phone (2) renderer.", file=sys.stderr)
    return PROFILES["A065"]


PROFILE_DIR_PREFIX = "/tmp/glyph-mirror-"


def _cleanup_stale_profiles() -> None:
    """Reap throwaway Chrome profiles left by sessions that were killed before
    their own cleanup ran. Each dir ends in `-<pid>`; we only remove ones whose
    pid is dead, so a concurrently-running sidecar's window is never disturbed."""
    for path in glob.glob(PROFILE_DIR_PREFIX + "*"):
        try:
            pid = int(path.rsplit("-", 1)[-1])
        except ValueError:
            continue
        try:
            os.kill(pid, 0)            # alive → leave it
        except ProcessLookupError:
            shutil.rmtree(path, ignore_errors=True)   # dead → reap
        except OSError:
            continue                   # exists but not ours → leave it


def open_app_window(url: str):
    """Open `url` in a chromeless app-mode window if a Chromium browser is
    installed, otherwise fall back to the system default browser.

    Returns `(browser_proc, profile_dir)` for the app-mode window so the caller
    can close the window and remove its throwaway profile on exit; both are
    None when we fell back to the default browser."""
    chromium_paths = [
        ("/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
         "google-chrome", "Google Chrome"),
        ("/Applications/Brave Browser.app/Contents/MacOS/Brave Browser",
         "brave", "Brave Browser"),
        ("/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
         "edge", "Microsoft Edge"),
        ("/Applications/Arc.app/Contents/MacOS/Arc", "arc", "Arc"),
    ]
    bin_path = None
    label = ""
    app_name = ""
    if sys.platform == "darwin":
        for p, lab, app in chromium_paths:
            if os.path.exists(p):
                bin_path = p
                label = lab
                app_name = app
                break
    if bin_path:
        # Use a dedicated --user-data-dir so the window is independent of the
        # user's normal Chrome session (which may already be running with the
        # default profile, making --app= a no-op).
        profile = f"{PROFILE_DIR_PREFIX}{label}-{os.getpid()}"
        try:
            proc = subprocess.Popen([
                bin_path,
                f"--app={url}",
                f"--user-data-dir={profile}",
                "--window-size=380,680",
                "--no-first-run",
                "--no-default-browser-check",
            ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            # The app-mode window often opens *behind* the scrcpy mirror. Pull
            # the browser to the front so the glyph viewer is actually visible.
            if app_name:
                subprocess.Popen(
                    ["osascript", "-e",
                     f'tell application "{app_name}" to activate'],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return proc, profile
        except Exception as e:
            print(f"glyph_sidecar: app-mode launch failed ({e}), "
                  "falling back to default browser.", file=sys.stderr)
    try:
        if sys.platform == "darwin":
            subprocess.Popen(["open", url])
        elif sys.platform.startswith("linux"):
            subprocess.Popen(["xdg-open", url])
    except Exception:
        pass
    return None, None


# ---------- entry point -------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial", default="")
    ap.add_argument("--http-port", type=int, default=HTTP_PORT_DEFAULT)
    args = ap.parse_args()

    # Reap throwaway Chrome profiles left behind by sessions that were killed
    # before they could clean up (a clean exit removes its own dir below).
    _cleanup_stale_profiles()

    sidecar = Sidecar(serial=args.serial)
    model = sidecar.detect_model()
    profile = pick_profile(model)
    print(f"glyph_sidecar: device model={model!r} → renderer={profile.name}",
          file=sys.stderr)

    broadcaster = Broadcaster()
    html = profile.render_html()
    handler = make_handler(html, broadcaster)

    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.http_port), handler)
    actual_port = server.server_address[1]
    url = f"http://127.0.0.1:{actual_port}/"
    print(f"glyph_sidecar: serving {url}", file=sys.stderr)

    stop = threading.Event()
    # scrcpy stops us with SIGTERM (see glyph_sidecar.c). Translate it into the
    # stop event so the `finally` below runs — closing the browser window and
    # removing its throwaway profile — instead of the process dying mid-block.
    try:
        signal.signal(signal.SIGTERM, lambda *_: stop.set())
    except Exception:
        pass
    if profile.log_reader is not None:
        t_phone = threading.Thread(target=profile.log_reader,
                                   args=(sidecar, broadcaster, stop), daemon=True)
        t_phone.start()
    t_http = threading.Thread(target=server.serve_forever, daemon=True)
    t_http.start()

    # Prefer a chromeless app-mode window (Chrome / Brave / Edge / Arc) so the
    # viewer feels like a native window alongside the scrcpy mirror. Fall back
    # to the default browser if no Chromium-family browser is installed.
    browser_proc, browser_profile = open_app_window(url)

    try:
        # Block until SIGTERM/SIGINT.
        while not stop.is_set():
            time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        server.shutdown()
        # Tear down the app-mode window and its throwaway profile so closing
        # scrcpy leaves nothing behind.
        if browser_proc is not None:
            try:
                browser_proc.terminate()
                browser_proc.wait(timeout=2)
            except Exception:
                pass
        if browser_profile:
            shutil.rmtree(browser_profile, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
