#ifndef SC_WEBSHARE_WEBRTC_PEER_H
#define SC_WEBSHARE_WEBRTC_PEER_H

#include "common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * One WebRTC PeerConnection per browser viewer.
 *
 * Built on libdatachannel's C API (https://github.com/paullouisageneau/libdatachannel).
 * To build, install libdatachannel >= 0.20 and enable -Dwebshare=true. The
 * Meson option declares the dependency.
 */

struct sc_webrtc_peer;

struct sc_webrtc_peer_callbacks {
    /**
     * Called when libdatachannel produces a local SDP description (answer or
     * subsequent renegotiation). `sdp` is a NUL-terminated string. `type` is
     * "offer" / "answer" / "pranswer" / "rollback".
     */
    void (*on_local_description)(struct sc_webrtc_peer *peer,
                                 const char *type, const char *sdp,
                                 void *userdata);
    /**
     * Called when libdatachannel emits a local ICE candidate. `cand` is the
     * candidate line (a=candidate:...). `mid` is the SDP media id.
     */
    void (*on_local_candidate)(struct sc_webrtc_peer *peer,
                               const char *cand, const char *mid,
                               void *userdata);
    /**
     * Called once when the video track transitions to the open state (the
     * first moment rtcSendMessage will actually transmit). The owner should
     * use this to ask the source for a fresh keyframe so the viewer doesn't
     * wait for the next natural IDR.
     */
    void (*on_track_open)(struct sc_webrtc_peer *peer, void *userdata);
    /**
     * Called when the underlying peer connection enters a terminal state
     * (disconnected/failed/closed). The owner should clean up the peer.
     */
    void (*on_state_terminal)(struct sc_webrtc_peer *peer, void *userdata);
};

/**
 * Create a peer with one outbound H.264 video track. `cb_userdata` is passed
 * to all callbacks. Returns NULL on failure.
 */
struct sc_webrtc_peer *
sc_webrtc_peer_create(const struct sc_webrtc_peer_callbacks *cbs,
                      void *cb_userdata);

void
sc_webrtc_peer_destroy(struct sc_webrtc_peer *peer);

/** Apply a remote SDP description received from the browser. */
bool
sc_webrtc_peer_set_remote_description(struct sc_webrtc_peer *peer,
                                      const char *type, const char *sdp);

/** Apply a remote ICE candidate received from the browser. */
bool
sc_webrtc_peer_add_remote_candidate(struct sc_webrtc_peer *peer,
                                    const char *cand, const char *mid);

/**
 * Send one H.264 access unit on the video track. The buffer must already be in
 * AVCC framing (each NAL unit prefixed by a 4-byte big-endian length); the
 * caller converts the source Annex-B stream once and passes the same buffer to
 * every peer.
 *
 * If `prepend_avcc` is non-NULL on the first keyframe seen by this peer, it is
 * prepended to the access unit (intended for cached SPS+PPS from extradata).
 *
 * `pts_90khz` is the RTP timestamp (90 kHz clock). The caller is responsible
 * for converting AVPacket->pts to this clock.
 *
 * Returns true on best-effort send (RTP is unreliable; we don't block).
 */
bool
sc_webrtc_peer_send_h264(struct sc_webrtc_peer *peer,
                         const uint8_t *avcc, size_t avcc_len,
                         const uint8_t *prepend_avcc, size_t prepend_avcc_len,
                         uint32_t pts_90khz, bool is_keyframe);

/**
 * Detach libdatachannel's log callback. Must be called before the process
 * begins SDL/libc teardown, otherwise a libdc background thread can call
 * into already-freed logging state and crash on exit.
 */
void
sc_webrtc_global_shutdown(void);

#endif
