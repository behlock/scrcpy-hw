#ifndef SC_WEB_SHARE_H
#define SC_WEB_SHARE_H

#include "common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "controller.h"
#include "http_server.h"
#include "trait/packet_sink.h"
#include "util/thread.h"

/**
 * Multi-viewer web mirror.
 *
 * Acts as a packet sink on the video demuxer: every encoded H.264 AVPacket
 * is fanned out as RTP via libdatachannel to every connected WebRTC peer.
 *
 * The browser-side viewer is served from embedded HTML+JS assets at GET /.
 * A QR code containing the connect URL is printed to the terminal at startup.
 */
struct sc_web_share {
    struct sc_packet_sink packet_sink;

    struct sc_http_server http;
    bool http_initialized;
    uint16_t port;
    uint32_t local_ipv4;  // host byte order
    bool started;

    // Optional: used to send RESET_VIDEO (force-keyframe) when a viewer joins.
    // May be NULL if scrcpy was launched with --no-control.
    struct sc_controller *controller;

    sc_mutex peers_mutex;
    struct sc_web_share_peer **peers;
    size_t peers_count;
    size_t peers_capacity;

    // SPS+PPS extracted from AVCodecContext.extradata on first open(),
    // pre-converted to AVCC so it can be prepended to each new peer's first
    // keyframe without re-converting. NULL if the source had no extradata
    // (the scrcpy case: SPS+PPS arrive inline at every IDR).
    uint8_t *sps_pps_avcc;
    size_t sps_pps_avcc_len;
    uint32_t clock_rate;
    int64_t time_base_num;
    int64_t time_base_den;
    int64_t first_pts;
    bool first_pts_set;

    char connect_url[256];
};

bool
sc_web_share_init(struct sc_web_share *ws, uint16_t port);

void
sc_web_share_set_controller(struct sc_web_share *ws,
                            struct sc_controller *controller);

bool
sc_web_share_start(struct sc_web_share *ws);

void
sc_web_share_stop(struct sc_web_share *ws);

void
sc_web_share_destroy(struct sc_web_share *ws);

/**
 * URL the QR code resolves to (e.g. "http://192.168.x.y:8000/"), populated by
 * start(). NULL-terminated.
 */
const char *
sc_web_share_get_url(const struct sc_web_share *ws);

#endif
