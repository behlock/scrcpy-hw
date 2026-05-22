// scrcpy --web-share viewer.

(() => {
  const log = (...args) => {
    console.log("[scrcpy]", ...args);
  };

  const statusEl = document.getElementById("status");
  const video = document.getElementById("v");

  const setStatus = (text, hidden) => {
    statusEl.textContent = text;
    statusEl.classList.toggle("connected", !!hidden);
    log("status:", text);
  };

  setStatus("Connecting (boot)…", false);
  log("location:", location.href);
  log("RTCPeerConnection:", typeof RTCPeerConnection);
  log("WebSocket:", typeof WebSocket);

  let pc;
  try {
    // We're a LAN-only deployment, so the host's own IP is reachable
    // without STUN — but iOS Safari has been observed to either skip ICE
    // gathering entirely or emit only mDNS (.local) host candidates that
    // libdatachannel on the desktop can't resolve. Giving the browser a
    // public STUN URL forces it to also gather a server-reflexive
    // candidate, which makes the connection negotiate reliably.
    pc = new RTCPeerConnection({
      iceServers: [{ urls: "stun:stun.l.google.com:19302" }],
    });
    log("pc created");
  } catch (e) {
    setStatus("RTCPeerConnection failed: " + e.message, false);
    return;
  }
  window.pc = pc;

  try {
    pc.addTransceiver("video", { direction: "recvonly" });
    log("transceiver added");
  } catch (e) {
    setStatus("addTransceiver failed: " + e.message, false);
    return;
  }

  // Pin the WebRTC receiver-side jitter buffer and playout delay to zero on
  // every receiver. Some browsers reset these across renegotiations, so we
  // re-apply after each setRemoteDescription rather than only once in
  // ontrack.
  const applyLowLatencyHints = () => {
    try {
      for (const r of pc.getReceivers()) {
        try { r.playoutDelayHint = 0; } catch (_) {}
        try {
          if ("jitterBufferTarget" in r) {
            r.jitterBufferTarget = 0;
          }
        } catch (_) {}
      }
    } catch (_) {}
  };

  pc.ontrack = (e) => {
    log("ontrack", e.track.kind);
    if (e.track.kind !== "video") return;
    applyLowLatencyHints();

    if (e.streams && e.streams[0]) {
      video.srcObject = e.streams[0];
    } else {
      const ms = new MediaStream();
      ms.addTrack(e.track);
      video.srcObject = ms;
    }

    // Periodically: if the playback head is falling behind the latest
    // available frame, fast-forward by speeding up playback or seeking
    // straight to the live edge. Tightened from 500ms/0.5s/1.1× to
    // 250ms/0.25s/1.5× for the lowest steady-state latency.
    const tighten = () => {
      try {
        if (video.buffered && video.buffered.length) {
          const latest = video.buffered.end(video.buffered.length - 1);
          const behind = latest - video.currentTime;
          if (behind > 0.25) {
            video.currentTime = latest;     // jump to live
          } else if (behind > 0.08) {
            video.playbackRate = 1.5;        // catch up
          } else {
            video.playbackRate = 1.0;
          }
        }
      } catch (_) {}
    };
    setInterval(tighten, 250);
  };

  pc.onicecandidate = (e) => {
    if (e.candidate && e.candidate.candidate) {
      ws.send(JSON.stringify({
        type: "candidate",
        candidate: e.candidate.candidate,
        mid: e.candidate.sdpMid || "0",
      }));
    }
  };

  // Mobile / desktop browsers refuse programmatic fullscreen requests
  // without a user gesture, so the best we can do is "go fullscreen on the
  // first tap and stay there".
  //
  // We deliberately do NOT fall back to iOS Safari's
  // `video.webkitEnterFullscreen()`: that swaps to iOS's native media
  // player which has its own buffer and ignores playoutDelayHint, adding
  // ~200-500 ms. Staying inline keeps the lowest latency. iPad and iOS 16+
  // Safari support the standard requestFullscreen API on documentElement,
  // and on older iPhone Safari versions the inline player already fills
  // the screen anyway.
  const isFullscreen = () =>
    !!(document.fullscreenElement || document.webkitFullscreenElement);

  const enterFullscreen = () => {
    if (isFullscreen()) return;
    const el = document.documentElement;
    if (el.requestFullscreen) {
      el.requestFullscreen().catch((err) => {
        log("requestFullscreen failed:", err && err.name);
      });
    }
  };
  document.addEventListener("click", enterFullscreen);
  document.addEventListener("touchend", enterFullscreen);

  const refreshStatusAfterFs = () => {
    if (pc.connectionState === "connected") {
      setStatus(isFullscreen() ? "Live" : "Tap for fullscreen",
                isFullscreen());
    }
  };
  document.addEventListener("fullscreenchange", refreshStatusAfterFs);
  document.addEventListener("webkitfullscreenchange", refreshStatusAfterFs);

  pc.onconnectionstatechange = () => {
    log("pc state:", pc.connectionState);
    switch (pc.connectionState) {
      case "connected":
        setStatus(isFullscreen() ? "Live" : "Tap for fullscreen",
                  isFullscreen());
        break;
      case "connecting": setStatus("Negotiating…", false); break;
      case "disconnected": setStatus("Reconnecting…", false); break;
      case "failed":
      case "closed":     setStatus("Disconnected.", false); break;
    }
  };

  const wsProto = location.protocol === "https:" ? "wss:" : "ws:";
  const wsUrl = `${wsProto}//${location.host}/signal`;
  log("ws connect:", wsUrl);

  let ws;
  try {
    ws = new WebSocket(wsUrl);
  } catch (e) {
    setStatus("WebSocket ctor failed: " + e.message, false);
    return;
  }
  ws.binaryType = "arraybuffer";
  window.ws = ws;

  ws.onopen = async () => {
    log("ws OPEN");
    setStatus("Negotiating…", false);
    try {
      const offer = await pc.createOffer();
      log("createOffer ok, len:", offer.sdp.length);
      await pc.setLocalDescription(offer);
      log("setLocalDescription ok");
      ws.send(JSON.stringify({ type: offer.type, sdp: offer.sdp }));
      log("offer sent");
    } catch (err) {
      log("offer error:", err);
      setStatus("Offer failed: " + err.message, false);
    }
  };

  ws.onmessage = async (evt) => {
    let msg;
    try { msg = JSON.parse(evt.data); } catch { return; }
    log("ws msg:", msg.type);
    if (msg.type === "answer" || msg.type === "offer") {
      try {
        await pc.setRemoteDescription({ type: msg.type, sdp: msg.sdp });
        // Re-pin low-latency hints — they can be reset across each
        // renegotiation on some browsers.
        applyLowLatencyHints();
        if (msg.type === "offer") {
          const answer = await pc.createAnswer();
          await pc.setLocalDescription(answer);
          ws.send(JSON.stringify({ type: answer.type, sdp: answer.sdp }));
        }
      } catch (err) {
        log("SDP error:", err);
        setStatus("SDP failed: " + err.message, false);
      }
    } else if (msg.type === "candidate") {
      try {
        await pc.addIceCandidate({
          candidate: msg.candidate,
          sdpMid: msg.mid || "0",
        });
      } catch (err) {
        log("addIceCandidate error:", err);
      }
    }
  };

  ws.onclose = (e) => {
    log("ws CLOSE code=", e.code, "reason=", e.reason);
    setStatus("Signaling closed (" + e.code + ").", false);
  };
  ws.onerror = (e) => {
    log("ws ERROR", e);
    setStatus("Signaling error.", false);
  };
})();
