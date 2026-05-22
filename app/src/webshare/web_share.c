#include "web_share.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>

#include "compat.h"
#include "control_msg.h"
#include "h264_rtp.h"
#include "qr_render.h"
#include "util/log.h"
#include "util/memory.h"
#include "util/str.h"
#include "webrtc_peer.h"

// Forward decls
struct sc_web_share_peer;

struct sc_web_share_peer {
    struct sc_web_share *owner;
    struct sc_ws_conn *ws_conn;
    struct sc_webrtc_peer *rtc;
    // Set on libdatachannel's signaling thread when the peer connection
    // enters a terminal state; read by the demuxer thread on every packet.
    atomic_bool dead;
};

// ---- tiny JSON-string-value extractor --------------------------------------
//
// Browser sends: {"type":"offer","sdp":"..."} and
//                {"type":"candidate","candidate":"...","mid":"video"}
//
// Only string values are needed. Returns malloc'd unescaped value or NULL.

static char *
json_unescape(const char *s, size_t len) {
    char *out = malloc(len + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = s[i];
        if (c == '\\' && i + 1 < len) {
            char n = s[++i];
            switch (n) {
                case '"':  out[o++] = '"'; break;
                case '\\': out[o++] = '\\'; break;
                case '/':  out[o++] = '/'; break;
                case 'b':  out[o++] = '\b'; break;
                case 'f':  out[o++] = '\f'; break;
                case 'n':  out[o++] = '\n'; break;
                case 'r':  out[o++] = '\r'; break;
                case 't':  out[o++] = '\t'; break;
                case 'u':
                    if (i + 4 < len) {
                        // Only BMP non-surrogate chars handled; SDP/ICE are ASCII anyway.
                        unsigned cp = 0;
                        for (int k = 0; k < 4; ++k) {
                            char hx = s[i + 1 + k];
                            cp <<= 4;
                            if (hx >= '0' && hx <= '9') cp |= hx - '0';
                            else if (hx >= 'a' && hx <= 'f') cp |= hx - 'a' + 10;
                            else if (hx >= 'A' && hx <= 'F') cp |= hx - 'A' + 10;
                        }
                        i += 4;
                        if (cp < 0x80) out[o++] = (char) cp;
                        else if (cp < 0x800) {
                            out[o++] = (char)(0xC0 | (cp >> 6));
                            out[o++] = (char)(0x80 | (cp & 0x3F));
                        } else {
                            out[o++] = (char)(0xE0 | (cp >> 12));
                            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            out[o++] = (char)(0x80 | (cp & 0x3F));
                        }
                    }
                    break;
                default: out[o++] = n; break;
            }
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
    return out;
}

// Find string field `key` in a flat JSON object. Returns malloc'd value or NULL.
static char *
json_get_string(const char *json, const char *key) {
    size_t klen = strlen(key);
    const char *p = json;
    while ((p = strstr(p, "\""))) {
        // Try to match "<key>":
        if (strncmp(p + 1, key, klen) == 0 && p[1 + klen] == '"') {
            const char *colon = p + 2 + klen;
            while (*colon == ' ' || *colon == '\t' || *colon == ':' || *colon == '\n') {
                if (*colon == ':') { ++colon; break; }
                ++colon;
            }
            while (*colon == ' ' || *colon == '\t' || *colon == '\n') ++colon;
            if (*colon != '"') return NULL;
            ++colon;
            // Find unescaped closing quote
            const char *end = colon;
            while (*end) {
                if (*end == '\\' && end[1]) { end += 2; continue; }
                if (*end == '"') break;
                ++end;
            }
            if (*end != '"') return NULL;
            return json_unescape(colon, (size_t)(end - colon));
        }
        ++p;
    }
    return NULL;
}

// JSON-escape a string into `out` (cap bytes incl. NUL). Returns length or -1.
static int
json_escape_into(char *out, size_t cap, const char *s) {
    size_t o = 0;
    for (size_t i = 0; s[i]; ++i) {
        unsigned char c = (unsigned char) s[i];
        const char *esc = NULL;
        char buf[8];
        switch (c) {
            case '"':  esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            case '\b': esc = "\\b";  break;
            case '\f': esc = "\\f";  break;
            default:
                if (c < 0x20) {
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    esc = buf;
                }
                break;
        }
        if (esc) {
            size_t l = strlen(esc);
            if (o + l + 1 >= cap) return -1;
            memcpy(out + o, esc, l);
            o += l;
        } else {
            if (o + 1 + 1 >= cap) return -1;
            out[o++] = (char) c;
        }
    }
    if (o + 1 > cap) return -1;
    out[o] = '\0';
    return (int) o;
}

// ---- WebRTC peer callbacks -> WebSocket signal -----------------------------

static void
peer_send_signal(struct sc_web_share_peer *peer, const char *type,
                 const char *body_key1, const char *body_val1,
                 const char *body_key2, const char *body_val2) {
    // Compose JSON. Pessimistic: assume up to 16 KB SDP after escaping.
    size_t cap = 32768;
    char *msg = malloc(cap);
    if (!msg) return;
    int n = snprintf(msg, cap, "{\"type\":\"%s\"", type);
    if (n < 0 || (size_t) n >= cap) { free(msg); return; }
    size_t off = (size_t) n;

    char esc[16384];
    if (body_val1) {
        if (json_escape_into(esc, sizeof(esc), body_val1) < 0) { free(msg); return; }
        n = snprintf(msg + off, cap - off, ",\"%s\":\"%s\"", body_key1, esc);
        if (n < 0 || (size_t)(off + n) >= cap) { free(msg); return; }
        off += (size_t) n;
    }
    if (body_val2) {
        if (json_escape_into(esc, sizeof(esc), body_val2) < 0) { free(msg); return; }
        n = snprintf(msg + off, cap - off, ",\"%s\":\"%s\"", body_key2, esc);
        if (n < 0 || (size_t)(off + n) >= cap) { free(msg); return; }
        off += (size_t) n;
    }
    if (off + 2 > cap) { free(msg); return; }
    msg[off++] = '}';
    msg[off] = '\0';

    sc_ws_conn_send_text(peer->ws_conn, msg, off);
    free(msg);
}

static void
on_local_description(struct sc_webrtc_peer *rtc, const char *type,
                     const char *sdp, void *userdata) {
    (void) rtc;
    struct sc_web_share_peer *peer = userdata;
    if (atomic_load_explicit(&peer->dead, memory_order_acquire)) return;
    peer_send_signal(peer, type, "sdp", sdp, NULL, NULL);
}

static void
on_local_candidate(struct sc_webrtc_peer *rtc, const char *cand,
                   const char *mid, void *userdata) {
    (void) rtc;
    struct sc_web_share_peer *peer = userdata;
    if (atomic_load_explicit(&peer->dead, memory_order_acquire)) return;
    peer_send_signal(peer, "candidate", "candidate", cand,
                     "mid", mid ? mid : "video");
}

static void
on_state_terminal(struct sc_webrtc_peer *rtc, void *userdata) {
    (void) rtc;
    struct sc_web_share_peer *peer = userdata;
    // Stop the packet sink from sending more frames to this peer.
    atomic_store_explicit(&peer->dead, true, memory_order_release);
    // Force the WebSocket loop on the conn thread to exit. on_ws_close will
    // then remove + destroy the peer; without this, a viewer whose WebRTC
    // session has terminated but keeps its WebSocket open would leak the
    // sc_web_share_peer slot for the lifetime of the session.
    sc_ws_conn_interrupt(peer->ws_conn);
}

static void
on_track_open(struct sc_webrtc_peer *rtc, void *userdata) {
    (void) rtc;
    struct sc_web_share_peer *peer = userdata;
    if (atomic_load_explicit(&peer->dead, memory_order_acquire)) return;
    // The track is now actually sending media. The IDR that was emitted in
    // response to RESET_VIDEO at ws-open time was dropped (track wasn't open
    // yet). Ask the device for another fresh keyframe so the viewer doesn't
    // wait for the next natural IDR (potentially several seconds away).
    struct sc_web_share *ws = peer->owner;
    if (ws->controller) {
        struct sc_control_msg msg;
        msg.type = SC_CONTROL_MSG_TYPE_RESET_VIDEO;
        if (!sc_controller_push_msg(ws->controller, &msg)) {
            LOGW("Web share: failed to request keyframe on track open");
        }
    }
}

static const struct sc_webrtc_peer_callbacks PEER_CBS = {
    .on_local_description = on_local_description,
    .on_local_candidate = on_local_candidate,
    .on_track_open = on_track_open,
    .on_state_terminal = on_state_terminal,
};

// ---- HTTP server callbacks -------------------------------------------------

static bool
peers_add(struct sc_web_share *ws, struct sc_web_share_peer *peer) {
    bool ok = false;
    sc_mutex_lock(&ws->peers_mutex);
    if (ws->peers_count == ws->peers_capacity) {
        size_t newcap = ws->peers_capacity ? ws->peers_capacity * 2 : 4;
        struct sc_web_share_peer **r =
            reallocarray(ws->peers, newcap, sizeof(*r));
        if (r) {
            ws->peers = r;
            ws->peers_capacity = newcap;
        }
    }
    if (ws->peers_count < ws->peers_capacity) {
        ws->peers[ws->peers_count++] = peer;
        ok = true;
    }
    sc_mutex_unlock(&ws->peers_mutex);
    return ok;
}

static void
peers_remove(struct sc_web_share *ws, struct sc_web_share_peer *peer) {
    sc_mutex_lock(&ws->peers_mutex);
    for (size_t i = 0; i < ws->peers_count; ++i) {
        if (ws->peers[i] == peer) {
            ws->peers[i] = ws->peers[--ws->peers_count];
            break;
        }
    }
    sc_mutex_unlock(&ws->peers_mutex);
}

static void
on_ws_open(struct sc_ws_conn *conn, void *userdata) {
    struct sc_web_share *ws = userdata;
    struct sc_web_share_peer *peer = calloc(1, sizeof(*peer));
    if (!peer) return;
    peer->owner = ws;
    peer->ws_conn = conn;
    atomic_init(&peer->dead, false);
    peer->rtc = sc_webrtc_peer_create(&PEER_CBS, peer);
    if (!peer->rtc) {
        free(peer);
        return;
    }
    if (!peers_add(ws, peer)) {
        LOGW("Web share: could not register viewer (out of memory)");
        sc_webrtc_peer_destroy(peer->rtc);
        free(peer);
        return;
    }
    LOGI("Web share: viewer connected");

    // Ask the device to emit a fresh keyframe so this new viewer doesn't have
    // to wait for the next natural IDR (which can be several seconds).
    if (ws->controller) {
        struct sc_control_msg msg;
        msg.type = SC_CONTROL_MSG_TYPE_RESET_VIDEO;
        if (!sc_controller_push_msg(ws->controller, &msg)) {
            LOGW("Web share: failed to request keyframe");
        }
    }
}

static struct sc_web_share_peer *
peer_for_conn(struct sc_web_share *ws, struct sc_ws_conn *conn) {
    struct sc_web_share_peer *found = NULL;
    sc_mutex_lock(&ws->peers_mutex);
    for (size_t i = 0; i < ws->peers_count; ++i) {
        if (ws->peers[i]->ws_conn == conn) {
            found = ws->peers[i];
            break;
        }
    }
    sc_mutex_unlock(&ws->peers_mutex);
    return found;
}

static void
on_ws_message(struct sc_ws_conn *conn, const char *text, size_t len,
              void *userdata) {
    (void) len;
    struct sc_web_share *ws = userdata;
    struct sc_web_share_peer *peer = peer_for_conn(ws, conn);
    if (!peer || peer->dead) {
        return;
    }

    char *type = json_get_string(text, "type");
    if (!type) return;

    if (strcmp(type, "offer") == 0 || strcmp(type, "answer") == 0) {
        char *sdp = json_get_string(text, "sdp");
        if (sdp) {
            sc_webrtc_peer_set_remote_description(peer->rtc, type, sdp);
            free(sdp);
        }
    } else if (strcmp(type, "candidate") == 0) {
        char *cand = json_get_string(text, "candidate");
        char *mid = json_get_string(text, "mid");
        if (cand) {
            sc_webrtc_peer_add_remote_candidate(peer->rtc, cand, mid);
        }
        free(cand);
        free(mid);
    }

    free(type);
}

static void
on_ws_close(struct sc_ws_conn *conn, void *userdata) {
    struct sc_web_share *ws = userdata;
    struct sc_web_share_peer *peer = peer_for_conn(ws, conn);
    if (!peer) return;
    peers_remove(ws, peer);
    sc_webrtc_peer_destroy(peer->rtc);
    free(peer);
    LOGI("Web share: viewer disconnected");
}

// ---- packet sink ops -------------------------------------------------------

static uint32_t
pts_to_90khz(struct sc_web_share *ws, int64_t pts) {
    if (pts == AV_NOPTS_VALUE) return 0;
    if (!ws->first_pts_set) {
        ws->first_pts = pts;
        ws->first_pts_set = true;
    }
    // scrcpy's wire protocol carries PTS in microseconds (see doc/develop.md).
    // Convert to 90 kHz: us * 90000 / 1000000 = us * 9 / 100.
    int64_t delta_us = pts - ws->first_pts;
    return (uint32_t)((delta_us * 9) / 100);
}

static bool
sink_open(struct sc_packet_sink *sink, AVCodecContext *ctx,
          const struct sc_stream_session *session) {
    (void) session;
    struct sc_web_share *ws = container_of(sink, struct sc_web_share, packet_sink);
    if (ctx->codec_id != AV_CODEC_ID_H264) {
        LOGE("Web share requires H.264 video. Got codec id %d.",
             (int) ctx->codec_id);
        return false;
    }
    if (ctx->extradata_size > 0) {
        // Expect Annex-B (starts with a 0x000001 or 0x00000001 start code).
        // The other common form is AVCC (`extradata[0] == 0x01`); reject it
        // explicitly rather than feed it through the Annex-B parser and emit
        // garbage on the wire.
        const uint8_t *ed = ctx->extradata;
        bool annexb = (ctx->extradata_size >= 3
                       && ed[0] == 0 && ed[1] == 0
                       && (ed[2] == 1 || (ctx->extradata_size >= 4
                                          && ed[2] == 0 && ed[3] == 1)));
        if (!annexb) {
            LOGE("Web share: unsupported H.264 extradata format "
                 "(expected Annex-B, got %s).",
                 ed[0] == 0x01 ? "AVCC" : "unknown");
            return false;
        }
        // Pre-convert to AVCC once at open time; every new peer's first
        // keyframe prepends this same buffer in sink_push.
        if (!sc_h264_annexb_to_avcc(ed, (size_t) ctx->extradata_size,
                                    &ws->sps_pps_avcc,
                                    &ws->sps_pps_avcc_len)) {
            LOGE("Web share: failed to convert SPS/PPS extradata to AVCC.");
            return false;
        }
    }
    ws->time_base_num = ctx->time_base.num;
    ws->time_base_den = ctx->time_base.den;
    return true;
}

static void
sink_close(struct sc_packet_sink *sink) {
    (void) sink;
    // Per-peer cleanup happens on WS close.
}

static bool
sink_push(struct sc_packet_sink *sink, const AVPacket *packet) {
    struct sc_web_share *ws = container_of(sink, struct sc_web_share, packet_sink);
    uint32_t ts = pts_to_90khz(ws, packet->pts);
    bool is_key = (packet->flags & AV_PKT_FLAG_KEY) != 0;

    // libdatachannel's H.264 RTP handler expects NAL units with 4-byte length
    // prefix (Annex-B's start codes must be converted). Each call to
    // rtcSendMessage is treated as ONE access unit (RFC 6184): the packetizer
    // sets the RTP marker bit on the last fragment of the call.
    //
    // Convert ONCE per packet; the same AVCC buffer is shared by every peer.
    uint8_t *avcc = NULL;
    size_t avcc_len = 0;
    if (!sc_h264_annexb_to_avcc(packet->data, (size_t) packet->size,
                                &avcc, &avcc_len)) {
        return true;  // drop on conversion failure, keep the pipeline alive
    }

    sc_mutex_lock(&ws->peers_mutex);
    for (size_t i = 0; i < ws->peers_count; ++i) {
        struct sc_web_share_peer *peer = ws->peers[i];
        if (atomic_load_explicit(&peer->dead, memory_order_acquire)) continue;
        sc_webrtc_peer_send_h264(peer->rtc, avcc, avcc_len,
                                 ws->sps_pps_avcc, ws->sps_pps_avcc_len,
                                 ts, is_key);
    }
    sc_mutex_unlock(&ws->peers_mutex);

    free(avcc);
    return true;
}

static const struct sc_packet_sink_ops SINK_OPS = {
    .open = sink_open,
    .close = sink_close,
    .push = sink_push,
};

// ---- lifecycle -------------------------------------------------------------

extern const uint8_t sc_webshare_index_html[];
extern const size_t sc_webshare_index_html_len;
extern const uint8_t sc_webshare_app_js[];
extern const size_t sc_webshare_app_js_len;

void
sc_web_share_set_controller(struct sc_web_share *ws,
                            struct sc_controller *controller) {
    ws->controller = controller;
}

bool
sc_web_share_init(struct sc_web_share *ws, uint16_t port) {
    memset(ws, 0, sizeof(*ws));
    ws->port = port;
    ws->packet_sink.ops = &SINK_OPS;
    if (!sc_mutex_init(&ws->peers_mutex)) return false;
    if (!sc_http_server_init(&ws->http)) {
        sc_mutex_destroy(&ws->peers_mutex);
        return false;
    }
    ws->http_initialized = true;
    return true;
}

bool
sc_web_share_start(struct sc_web_share *ws) {
    if (!net_get_local_ipv4(&ws->local_ipv4)) {
        LOGE("Web share: could not detect a local LAN IP; is the host online?");
        return false;
    }
    snprintf(ws->connect_url, sizeof(ws->connect_url),
             "http://%u.%u.%u.%u:%u/",
             (ws->local_ipv4 >> 24) & 0xFF,
             (ws->local_ipv4 >> 16) & 0xFF,
             (ws->local_ipv4 >> 8) & 0xFF,
             ws->local_ipv4 & 0xFF,
             (unsigned) ws->port);

    struct sc_qr qr;
    if (sc_qr_encode(&qr, ws->connect_url)) {
        fprintf(stderr,
                "\nWeb share: scan this QR with a phone on the same Wi-Fi "
                "to watch the mirror in a browser.\nURL: %s\n\n",
                ws->connect_url);
        sc_qr_print(&qr, stderr);
    }

    static const struct sc_http_server_callbacks HTTP_CBS = {
        .on_ws_open = on_ws_open,
        .on_ws_message = on_ws_message,
        .on_ws_close = on_ws_close,
    };
    if (!sc_http_server_start(&ws->http, ws->port,
                              sc_webshare_index_html, sc_webshare_index_html_len,
                              sc_webshare_app_js, sc_webshare_app_js_len,
                              &HTTP_CBS, ws)) {
        return false;
    }
    ws->started = true;
    LOGI("Web share: listening on %s", ws->connect_url);
    return true;
}

void
sc_web_share_stop(struct sc_web_share *ws) {
    if (!ws->started) return;
    sc_http_server_stop(&ws->http);
}

void
sc_web_share_destroy(struct sc_web_share *ws) {
    if (ws->http_initialized) {
        sc_http_server_destroy(&ws->http);
    }
    // Tear down peers (the HTTP server already joined all conn threads, so no
    // on_ws_close callback will fire from here on).
    for (size_t i = 0; i < ws->peers_count; ++i) {
        sc_webrtc_peer_destroy(ws->peers[i]->rtc);
        free(ws->peers[i]);
    }
    free(ws->peers);
    free(ws->sps_pps_avcc);
    sc_mutex_destroy(&ws->peers_mutex);
    // Detach libdatachannel's log callback so its background threads cannot
    // call into our log code after SDL has been torn down by atexit().
    sc_webrtc_global_shutdown();
}

const char *
sc_web_share_get_url(const struct sc_web_share *ws) {
    return ws->connect_url;
}
