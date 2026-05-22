# Web share (`--web-share`)

Broadcast the mirrored Android device to anyone on the local network — a phone
on the same Wi-Fi scans a QR code, opens a browser, and watches the live
mirror with low (~100–300 ms) latency over WebRTC.

```
scrcpy --web-share
```

This starts an HTTP/WebRTC server on TCP port 8000 (override with
`--web-share=9000`) and prints a QR code (plus the connect URL) to the
terminal. Each browser that scans the QR negotiates its own WebRTC
PeerConnection and receives an RTP H.264 stream that pass-throughs the device
encoder — no re-encoding on the host.

## Requirements

- The device must encode in H.264 (the default). Mobile-browser WebRTC
  universally supports H.264; `--video-codec=h265|av1` would silently fail in
  most viewers, so scrcpy refuses to enable web share in that case and just
  prints a warning.
- All viewers must be on the same LAN as the desktop host. The scrcpy host
  configures no STUN/TURN/relay servers; the browser-side viewer references
  a public STUN URL only to coax iOS Safari into emitting non-mDNS ICE
  candidates — no media or signaling traffic ever leaves the LAN.
- This is video-only and view-only. Audio and remote control from the browser
  are out of scope for the initial version. Each new viewer does cause one
  RESET_VIDEO control message to be sent to the device (to request a fresh
  keyframe so the viewer doesn't wait several seconds for the next natural
  IDR); no other device traffic originates from viewers.
- No authentication. The QR is the access control: anyone on the Wi-Fi who
  scans it can watch. Acceptable for a controlled demo environment.

## First-run firewall prompt

The first time the feature runs, macOS / Windows Defender / Linux desktop
firewalls will prompt to allow incoming connections on port 8000. Approve it.

## Building

Web share is opt-in at build time and depends on
[libdatachannel](https://github.com/paullouisageneau/libdatachannel) (MPL 2.0,
~500 KB).

```
brew install libdatachannel        # macOS
sudo apt install libdatachannel-dev # Debian / Ubuntu

meson setup x -Dwebshare=true
ninja -C x
```

The bundled QR encoder is **Project Nayuki's qrcodegen-c**
(<https://github.com/nayuki/QR-Code-generator>, MIT). The full implementation
is vendored in `app/src/webshare/qrcodegen.{c,h}`; no separate dependency is
required.

## How it works

```
device (H.264) ── ADB ──► video_demuxer ──► [decoder, recorder, web_share]
                                                                  │
                                                       Annex-B → AVCC
                                                                  │
                                  libdatachannel ──► RTP/SRTP ──► browser
```

`sc_web_share` registers itself as a third packet sink on the video demuxer
(alongside the decoder and optional recorder). Each viewer connects via the
embedded HTTP server (`GET /` returns a single-page viewer), upgrades to a
WebSocket on `/signal` for SDP/ICE exchange, and ends up with a WebRTC
PeerConnection that receives H.264 RTP directly.

Bandwidth scales linearly per viewer (RTP is per-peer). Five viewers at 8 Mbps
each ≈ 40 Mbps uplink from the desktop.

## Browser compatibility

- iOS Safari 14+
- Android Chrome (any current version)
- Desktop Chrome / Firefox / Safari with WebRTC + H.264 (i.e. any current
  build)

## Tuning for lower latency

Steady-state glass-to-glass latency on a clean LAN is typically ~150–400 ms.
A few extra knobs:

- `--video-bit-rate=4M` (or `--video-bit-rate=6M`) — lower than the 8 Mbps
  default cuts encoder queueing and shrinks each AU, often shaving
  50–100 ms. Affects the local scrcpy window too.
- `--max-fps=60` (default) — ensure you're not implicitly throttled by an
  older device's encoder. If frames are dropping, fall back to `--max-fps=30`.
- 5 GHz Wi-Fi for the host **and** the viewer. 2.4 GHz can add 50+ ms of
  jitter on a busy channel.
- Wire the desktop host to the router with Ethernet when possible.

The browser-side viewer pins `playoutDelayHint=0` and `jitterBufferTarget=0`
on every receiver after each renegotiation, declines iOS Safari's native
fullscreen player (which would re-introduce buffering), and aggressively
catches the `<video>` playback head back to the live edge every 250 ms.
