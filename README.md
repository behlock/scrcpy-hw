# scrcpy-hw

<img src="app/data/scrcpy.svg" width="96" height="96" alt="scrcpy" align="right" />

Personal fork of [Genymobile/scrcpy](https://github.com/Genymobile/scrcpy) that adds **browser-based mirroring** over WebRTC — a phone on the same Wi-Fi scans a QR code and watches the live device mirror in any modern browser. No app to install on the viewer, no relay server, no re-encoding.

Everything else from upstream still works exactly the same. The fork is opt-in at build time (`-Dwebshare=true`) and behind a single CLI flag (`--web-share`), so a build without it is byte-equivalent to upstream behaviour.

## Fork additions

| Commit | What |
| --- | --- |
| `f070ae1` | Initial `--web-share` implementation: embedded HTTP server, WebRTC signalling over WebSocket, H.264 RTP packetiser, terminal QR code, single-page viewer. |
| `fc0c725` | iOS Safari support — H.264 profile-id negotiation in the SDP offer. |
| `c88ef1b` | Harden Safari viewer — `playoutDelayHint=0`, `jitterBufferTarget=0`, decline native fullscreen player, catch the `<video>` head back to the live edge every 250 ms. |
| `90ec805` | Reliability pass — fix viewer disconnect races, harden the HTTP server against malformed requests, tighten socket cleanup in `net.c` to avoid fd leaks on long-running sessions. |
| `e133c45` | Mobile Safari latency tweak — narrow the RTP pacing window on the peer side, shaves ~50 ms of glass-to-glass on iPhone. |

Full design notes: [`doc/web-share.md`](doc/web-share.md).

## Usage

```bash
scrcpy --web-share              # default port 8000
scrcpy --web-share=9000         # override port
```

The terminal prints a QR code and a connect URL. Scan from any phone on the same LAN — the page is a single static viewer that negotiates its own WebRTC PeerConnection per viewer. The QR is the access control; there is no auth.

```
device (H.264) ── ADB ──► video_demuxer ──► [decoder, recorder, web_share]
                                                                 │
                                                      Annex-B → AVCC
                                                                 │
                                 libdatachannel ──► RTP/SRTP ──► browser
```

The video packet is passed through unchanged from the device encoder — no re-encoding on the host, so per-viewer CPU cost is near zero and quality matches the local scrcpy window. Bandwidth scales linearly per viewer (5 viewers × 8 Mbps ≈ 40 Mbps uplink).

### Constraints

- **H.264 only.** Mobile-browser WebRTC universally supports H.264; `--video-codec=h265|av1` would silently fail in most viewers, so the host refuses to enable web share in that case and prints a warning.
- **LAN only.** No STUN/TURN/relay is configured on the host. The browser-side viewer references a public STUN URL only to coax iOS Safari into emitting non-mDNS ICE candidates — no media or signalling ever leaves the LAN.
- **Video-only, view-only.** Audio and remote control from the browser are out of scope. Each new viewer triggers one `RESET_VIDEO` to the device so the browser gets a fresh keyframe instead of waiting seconds for the next natural IDR.

### Browser support

iOS Safari 14+, Android Chrome (current), desktop Chrome / Firefox / Safari (current). Tested most thoroughly on iPhone Safari, where ~150–300 ms steady-state latency is typical on a clean 5 GHz LAN.

## Build

Web share is opt-in and depends on [libdatachannel](https://github.com/paullouisageneau/libdatachannel) (MPL 2.0, ~500 KB). The QR encoder is [Project Nayuki's qrcodegen-c](https://github.com/nayuki/QR-Code-generator) (MIT), vendored under `app/src/webshare/qrcodegen.{c,h}`.

```bash
brew install libdatachannel              # macOS
sudo apt install libdatachannel-dev      # Debian / Ubuntu

meson setup x -Dwebshare=true
ninja -C x
```

Built without `-Dwebshare=true`, the binary is identical to upstream scrcpy.

## Upstream docs

For everything that isn't the web share feature, the upstream documentation applies as-is:

- [Connection](doc/connection.md) · [Video](doc/video.md) · [Audio](doc/audio.md) · [Control](doc/control.md)
- [Keyboard](doc/keyboard.md) · [Mouse](doc/mouse.md) · [Gamepad](doc/gamepad.md)
- [Device](doc/device.md) · [Window](doc/window.md) · [Recording](doc/recording.md)
- [Virtual display](doc/virtual-display.md) · [Camera](doc/camera.md) · [V4L2](doc/v4l2.md)
- [Tunnels](doc/tunnels.md) · [OTG](doc/otg.md) · [Shortcuts](doc/shortcuts.md)
- [Build instructions](doc/build.md) · [FAQ](FAQ.md)

## License

Apache 2.0 — same as upstream.

    Copyright (C) 2018 Genymobile
    Copyright (C) 2018-2026 Romain Vimont
