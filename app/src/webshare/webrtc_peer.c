#include "webrtc_peer.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <rtc/rtc.h>

#include "util/log.h"

#define VIDEO_SSRC      0x12345678
// Browsers' default offer uses sequential numeric mids ("0", "1", ...). With
// only one m-line (BUNDLE 0) the video mid is always "0". Match it so the
// track binds to the offered m-line and becomes open.
#define VIDEO_MID       "0"
#define VIDEO_CNAME     "scrcpy-webshare"

struct sc_webrtc_peer {
    int pc;              // libdatachannel peer connection id
    atomic_int track;    // video track id; set on first offer (WS thread),
                         // read on the demuxer thread for every packet
    struct sc_webrtc_peer_callbacks cbs;
    void *cb_userdata;

    // Touched only by the demuxer thread (send_h264), no synchronization
    // needed.
    bool sps_pps_sent;
    // Set true once we see our first keyframe after the track opens.
    // Until then we drop frames (no anchor for the decoder).
    bool started;
};

// ---- libdatachannel internal logger ----------------------------------------

static void
rtc_log_cb(rtcLogLevel level, const char *message) {
    // libdatachannel is chatty at INFO; only surface warnings and above so a
    // running session doesn't spam the terminal.
    if (level == RTC_LOG_WARNING) {
        LOGW("libdatachannel: %s", message);
    } else if (level <= RTC_LOG_ERROR) {
        LOGE("libdatachannel: %s", message);
    }
}

static void
init_logger_once(void) {
    static bool inited = false;
    if (!inited) {
        rtcInitLogger(RTC_LOG_WARNING, rtc_log_cb);
        inited = true;
    }
}

void
sc_webrtc_global_shutdown(void) {
    rtcInitLogger(RTC_LOG_NONE, NULL);
}

// ---- libdatachannel callback bridges ---------------------------------------
//
// libdatachannel's C API invokes callbacks with the PeerConnection id and a
// user pointer. We register ourselves as the user pointer.

static void
on_local_desc(int pc, const char *sdp, const char *type, void *userdata) {
    (void) pc;
    struct sc_webrtc_peer *p = userdata;
    if (p && p->cbs.on_local_description) {
        p->cbs.on_local_description(p, type, sdp, p->cb_userdata);
    }
}

static void
on_local_cand(int pc, const char *cand, const char *mid, void *userdata) {
    (void) pc;
    struct sc_webrtc_peer *p = userdata;
    if (p && p->cbs.on_local_candidate) {
        p->cbs.on_local_candidate(p, cand, mid, p->cb_userdata);
    }
}

static void
on_track_open(int id, void *userdata) {
    (void) id;
    struct sc_webrtc_peer *p = userdata;
    if (p && p->cbs.on_track_open) {
        p->cbs.on_track_open(p, p->cb_userdata);
    }
}

static void
on_track_closed(int id, void *userdata) {
    (void) id;
    (void) userdata;
}

static void
on_state_change(int pc, rtcState state, void *userdata) {
    (void) pc;
    struct sc_webrtc_peer *p = userdata;
    if (!p) return;
    if (state == RTC_DISCONNECTED || state == RTC_FAILED
            || state == RTC_CLOSED) {
        if (p->cbs.on_state_terminal) {
            p->cbs.on_state_terminal(p, p->cb_userdata);
        }
    }
}

// ---- public API ------------------------------------------------------------

struct sc_webrtc_peer *
sc_webrtc_peer_create(const struct sc_webrtc_peer_callbacks *cbs,
                      void *cb_userdata) {
    init_logger_once();
    struct sc_webrtc_peer *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->cbs = *cbs;
    p->cb_userdata = cb_userdata;

    rtcConfiguration cfg = {0};
    // No STUN/TURN: LAN-only. ICE will gather host candidates.
    // Disable auto-negotiation so we don't race the browser's offer with
    // an automatic offer of our own. We are strictly the answerer.
    cfg.disableAutoNegotiation = true;
    p->pc = rtcCreatePeerConnection(&cfg);
    if (p->pc <= 0) {
        LOGE("rtcCreatePeerConnection failed: %d", p->pc);
        free(p);
        return NULL;
    }

    rtcSetUserPointer(p->pc, p);
    rtcSetLocalDescriptionCallback(p->pc, on_local_desc);
    rtcSetLocalCandidateCallback(p->pc, on_local_cand);
    rtcSetStateChangeCallback(p->pc, on_state_change);

    // The track is created lazily in set_remote_description, once we know
    // which payload type the browser actually offered for H.264. Hardcoding a
    // PT here (we used to use 96) causes the answer's PT to not match any of
    // the offer's PTs — Chrome accepts the SDP, but its receiver has no
    // mapping for our PT and silently drops every RTP packet.
    atomic_init(&p->track, 0);
    return p;
}

// Pick an H.264 payload type from the remote offer's SDP. Prefers a PT whose
// fmtp line contains packetization-mode=1 (needed for FU-A fragmentation of
// our large keyframes). Falls back to the first H.264 PT if none match.
// Returns 0 if no H.264 PT is offered at all.
static int
pick_h264_pt_from_offer(const char *sdp) {
    if (!sdp) return 0;
    int fallback = 0;
    const char *p = sdp;
    while ((p = strstr(p, "a=rtpmap:")) != NULL) {
        const char *q = p + 9; // strlen("a=rtpmap:")
        int pt = 0;
        while (*q >= '0' && *q <= '9') {
            pt = pt * 10 + (*q - '0');
            ++q;
        }
        if (*q != ' ' || strncmp(q + 1, "H264/90000", 10) != 0) {
            ++p;
            continue;
        }
        // Reject false-prefix matches like "H264/900001". A valid rtpmap line
        // ends the codec token with EOL, space, or "/<encoding-parameters>".
        char term = q[1 + 10];
        if (term != '\0' && term != '\r' && term != '\n'
                && term != ' ' && term != '/') {
            ++p;
            continue;
        }
        if (fallback == 0) fallback = pt;
        // Find the matching fmtp line for this PT.
        char needle[32];
        int nn = snprintf(needle, sizeof(needle), "a=fmtp:%d ", pt);
        if (nn > 0 && (size_t) nn < sizeof(needle)) {
            const char *fmtp = strstr(sdp, needle);
            if (fmtp) {
                const char *eol = strpbrk(fmtp, "\r\n");
                size_t llen = eol ? (size_t)(eol - fmtp) : strlen(fmtp);
                // Look for "packetization-mode=1" within this fmtp line.
                const size_t needle_len = 20; // strlen("packetization-mode=1")
                for (size_t i = 0; i + needle_len <= llen; ++i) {
                    if (memcmp(fmtp + i, "packetization-mode=1",
                               needle_len) == 0) {
                        return pt;
                    }
                }
            }
        }
        ++p;
    }
    return fallback;
}

void
sc_webrtc_peer_destroy(struct sc_webrtc_peer *peer) {
    if (!peer) return;
    if (peer->pc > 0) {
        rtcClosePeerConnection(peer->pc);
        rtcDeletePeerConnection(peer->pc);
    }
    free(peer);
}

bool
sc_webrtc_peer_set_remote_description(struct sc_webrtc_peer *peer,
                                      const char *type, const char *sdp) {
    // On the first offer, pick an H.264 PT from the offer and add our track
    // with that PT. The answer must reuse a PT from the offer (RFC 3264);
    // using any other PT (e.g. a hardcoded 96) makes Chrome silently drop
    // every RTP packet on the receive side -> "Live" with black screen.
    if (strcmp(type, "offer") == 0
            && atomic_load_explicit(&peer->track, memory_order_relaxed) <= 0) {
        int pt = pick_h264_pt_from_offer(sdp);
        if (pt <= 0) {
            LOGE("Web share: remote offer has no H.264 codec.");
            return false;
        }

        rtcTrackInit track_init = {
            .direction = RTC_DIRECTION_SENDONLY,
            .codec = RTC_CODEC_H264,
            .payloadType = pt,
            .ssrc = VIDEO_SSRC,
            .mid = VIDEO_MID,
            .name = "scrcpy",
            .msid = "scrcpy",
            .trackId = "scrcpy-video",
            // Constrained Baseline 5.2 — the level must be >= the actual
            // stream's level or strict HW decoders refuse the AU.
            .profile = "42e034",
        };
        int track_id = rtcAddTrackEx(peer->pc, &track_init);
        if (track_id <= 0) {
            LOGE("rtcAddTrackEx failed: %d", track_id);
            return false;
        }
        rtcSetUserPointer(track_id, peer);
        rtcSetOpenCallback(track_id, on_track_open);
        rtcSetClosedCallback(track_id, on_track_closed);

        rtcPacketizerInit pinit = {
            .ssrc = VIDEO_SSRC,
            .cname = VIDEO_CNAME,
            .payloadType = pt,
            .clockRate = 90000,
            .sequenceNumber = 0,
            .timestamp = 0,
            .nalSeparator = RTC_NAL_SEPARATOR_LENGTH,
            .maxFragmentSize = 1200,
            .playoutDelayId = 1,
            .playoutDelayMin = 0,
            .playoutDelayMax = 0,
        };
        rtcSetH264Packetizer(track_id, &pinit);
        rtcChainRtcpSrReporter(track_id);
        rtcChainRtcpNackResponder(track_id, 512);

        // Publish the track id last, with release ordering, so the demuxer
        // thread sees a fully-configured track when it observes a non-zero
        // value.
        atomic_store_explicit(&peer->track, track_id, memory_order_release);
    }

    int rc = rtcSetRemoteDescription(peer->pc, sdp, type);
    if (rc != RTC_ERR_SUCCESS) {
        LOGE("rtcSetRemoteDescription failed: %d", rc);
        return false;
    }
    if (strcmp(type, "offer") == 0) {
        rc = rtcSetLocalDescription(peer->pc, "answer");
        if (rc != RTC_ERR_SUCCESS) {
            LOGE("rtcSetLocalDescription(answer) failed: %d", rc);
            return false;
        }
    }
    return true;
}

bool
sc_webrtc_peer_add_remote_candidate(struct sc_webrtc_peer *peer,
                                    const char *cand, const char *mid) {
    int rc = rtcAddRemoteCandidate(peer->pc, cand, mid ? mid : VIDEO_MID);
    if (rc != RTC_ERR_SUCCESS) {
        LOGE("rtcAddRemoteCandidate failed: %d", rc);
        return false;
    }
    return true;
}

bool
sc_webrtc_peer_send_h264(struct sc_webrtc_peer *peer,
                         const uint8_t *avcc, size_t avcc_len,
                         const uint8_t *prepend_avcc, size_t prepend_avcc_len,
                         uint32_t pts_90khz, bool is_keyframe) {
    int track = atomic_load_explicit(&peer->track, memory_order_acquire);
    if (track <= 0) return false;

    // Until ICE + DTLS + media negotiation has finished, the track silently
    // discards anything we send. If we marked "started" / "sps_pps_sent" on
    // those dropped sends, the viewer would then miss the parameter sets that
    // gate H.264 decoding -> permanent black screen even after the track
    // opens. So gate everything on the track being open.
    if (!rtcIsOpen(track)) return false;

    if (!peer->started) {
        if (!is_keyframe) return false;
        peer->started = true;
    }

    // On keyframes, prepend the cached SPS+PPS into the SAME access unit as
    // the IDR. Sending them as a separate rtcSendMessage call would have the
    // packetizer mark them as a completed AU with no slice — the browser
    // would then receive the IDR as a separate AU with no parameter set and
    // silently drop every frame.
    //
    // In practice this branch is dead for scrcpy: its H.264 stream has empty
    // AVCodecContext.extradata; SPS+PPS arrive inline at the start of every
    // IDR's AU and sc_h264_annexb_to_avcc emits them as the first two NAL
    // units. Kept as a safety net for sources that populate extradata.
    const uint8_t *send_buf = avcc;
    size_t send_len = avcc_len;
    uint8_t *prepended = NULL;
    bool prepended_sps_pps = false;
    if (is_keyframe && !peer->sps_pps_sent && prepend_avcc && prepend_avcc_len) {
        prepended = malloc(prepend_avcc_len + avcc_len);
        if (prepended) {
            memcpy(prepended, prepend_avcc, prepend_avcc_len);
            memcpy(prepended + prepend_avcc_len, avcc, avcc_len);
            send_buf = prepended;
            send_len = prepend_avcc_len + avcc_len;
            prepended_sps_pps = true;
        }
    }

    // Set the RTP timestamp from the source PTS so the receiver's jitter
    // buffer can pace frames correctly. Without this, libdatachannel
    // auto-increments based on a fixed FPS assumption, which makes the
    // browser buffer extra to "catch up" — adding seconds of latency.
    rtcSetTrackRtpTimestamp(track, pts_90khz);

    int rc = rtcSendMessage(track, (const char *) send_buf, (int) send_len);
    free(prepended);
    if (rc < 0) {
        return false;
    }
    // Only latch sps_pps_sent on a successful send.
    if (prepended_sps_pps) {
        peer->sps_pps_sent = true;
    }
    return true;
}
