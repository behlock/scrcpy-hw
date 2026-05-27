# scrcpy-hw

<img src="app/data/scrcpy.svg" width="96" height="96" alt="scrcpy" align="right" />

Personal fork of [Genymobile/scrcpy](https://github.com/Genymobile/scrcpy) that adds **browser-based mirroring** over WebRTC — a phone on the same Wi-Fi scans a QR code and watches the live device mirror in any modern browser. No app to install on the viewer, no relay server, no re-encoding.

Everything else from upstream still works exactly the same. The fork is opt-in at build time (`-Dwebshare=true`) and behind a single CLI flag (`--web-share`), so a build without it is byte-equivalent to upstream behaviour.

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
