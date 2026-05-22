// scrcpy --web-share viewer.

(() => {
  const log = (...args) => {
    console.log("[scrcpy]", ...args);
  };

  const statusEl = document.getElementById("status");
  const video = document.getElementById("v");

  const setStatus = (text, connected) => {
    statusEl.textContent = text;
    statusEl.classList.toggle("connected", !!connected);
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

  pc.ontrack = (e) => {
    log("ontrack", e.track.kind);
    if (e.track.kind === "video") {
      try {
        e.receiver.playoutDelayHint = 0;
        if ("jitterBufferTarget" in e.receiver) {
          e.receiver.jitterBufferTarget = 0;
        }
      } catch (err) {
        log("playoutDelayHint set failed:", err.message);
      }
      if (e.streams && e.streams[0]) {
        video.srcObject = e.streams[0];
      } else {
        const ms = new MediaStream();
        ms.addTrack(e.track);
        video.srcObject = ms;
      }

      // Periodically: if the playback head is falling behind the latest
      // available frame, fast-forward by setting playbackRate temporarily
      // above 1, or jumping to the latest buffered time. iOS Safari is
      // especially fond of building up a slack queue here.
      const tighten = () => {
        try {
          if (video.buffered && video.buffered.length) {
            const latest = video.buffered.end(video.buffered.length - 1);
            const behind = latest - video.currentTime;
            if (behind > 0.5) {
              video.currentTime = latest;     // jump to live
            } else if (behind > 0.15) {
              video.playbackRate = 1.1;        // gently catch up
            } else {
              video.playbackRate = 1.0;
            }
          }
        } catch (_) {}
      };
      setInterval(tighten, 500);
    }
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

  pc.onconnectionstatechange = () => {
    log("pc state:", pc.connectionState);
    switch (pc.connectionState) {
      case "connected":  setStatus("Live", true); break;
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
