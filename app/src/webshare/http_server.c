#include "http_server.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat.h"
#include "util/log.h"
#include "util/memory.h"

#define MAX_REQUEST_LINE      8192
#define MAX_HEADER_TOTAL      8192
#define WS_GUID               "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_FRAME_MAX_PAYLOAD  (256 * 1024)
// Cap how long a single HTTP request can sit half-read before we drop the
// connection. This bounds slow / stuck peers without affecting healthy WebRTC
// signaling (the request itself is tiny and arrives in a few packets).
#define HTTP_REQUEST_RECV_TIMEOUT_MS  10000

// One per accepted TCP connection. Promoted to a real "WebSocket connection"
// once the Upgrade succeeds. Either way, the thread runs to completion and is
// joined on server shutdown.
struct sc_ws_conn {
    struct sc_http_server *owner;
    sc_socket socket;
    sc_thread thread;
    sc_mutex send_mutex;
    bool send_mutex_inited;
    bool is_ws;  // set once the Upgrade handshake succeeds
    bool open;   // true while the WS read loop is running
};

// ---- minimal SHA-1 (RFC 3174) ----------------------------------------------

struct sha1 {
    uint32_t state[5];
    uint64_t count;
    uint8_t  buf[64];
};

static void
sha1_init(struct sha1 *s) {
    s->state[0] = 0x67452301;
    s->state[1] = 0xEFCDAB89;
    s->state[2] = 0x98BADCFE;
    s->state[3] = 0x10325476;
    s->state[4] = 0xC3D2E1F0;
    s->count = 0;
}

#define ROL(x,n) (((x) << (n)) | ((x) >> (32 - (n))))

static void
sha1_block(struct sha1 *s, const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = ((uint32_t) block[i*4] << 24)
             | ((uint32_t) block[i*4+1] << 16)
             | ((uint32_t) block[i*4+2] << 8)
             |  (uint32_t) block[i*4+3];
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = ROL(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    }
    uint32_t a = s->state[0], b = s->state[1], c = s->state[2];
    uint32_t d = s->state[3], e = s->state[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;            k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;            k = 0xCA62C1D6; }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    s->state[0] += a; s->state[1] += b; s->state[2] += c;
    s->state[3] += d; s->state[4] += e;
}

static void
sha1_update(struct sha1 *s, const uint8_t *data, size_t len) {
    size_t pos = (size_t)(s->count & 63);
    s->count += len;
    if (pos) {
        size_t take = 64 - pos;
        if (take > len) take = len;
        memcpy(s->buf + pos, data, take);
        data += take; len -= take;
        if (pos + take == 64) {
            sha1_block(s, s->buf);
            pos = 0;
        } else {
            return;
        }
    }
    while (len >= 64) {
        sha1_block(s, data);
        data += 64; len -= 64;
    }
    if (len) memcpy(s->buf, data, len);
}

static void
sha1_final(struct sha1 *s, uint8_t out[20]) {
    uint64_t bits = s->count * 8;
    size_t pos = (size_t)(s->count & 63);
    s->buf[pos++] = 0x80;
    if (pos > 56) {
        memset(s->buf + pos, 0, 64 - pos);
        sha1_block(s, s->buf);
        pos = 0;
    }
    memset(s->buf + pos, 0, 56 - pos);
    for (int i = 7; i >= 0; --i) {
        s->buf[56 + (7 - i)] = (uint8_t)(bits >> (i * 8));
    }
    sha1_block(s, s->buf);
    for (int i = 0; i < 5; ++i) {
        out[i*4]   = (uint8_t)(s->state[i] >> 24);
        out[i*4+1] = (uint8_t)(s->state[i] >> 16);
        out[i*4+2] = (uint8_t)(s->state[i] >> 8);
        out[i*4+3] = (uint8_t) s->state[i];
    }
}

// ---- minimal base64 encode -------------------------------------------------

static void
base64_encode(const uint8_t *in, size_t len, char *out) {
    static const char *T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, o = 0;
    for (i = 0; i + 2 < len; i += 3) {
        uint32_t v = ((uint32_t) in[i] << 16)
                   | ((uint32_t) in[i+1] << 8)
                   |  (uint32_t) in[i+2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = T[(v >> 6)  & 63];
        out[o++] = T[v & 63];
    }
    if (i < len) {
        uint32_t v = (uint32_t) in[i] << 16;
        if (i + 1 < len) v |= (uint32_t) in[i+1] << 8;
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? T[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
}

// ---- low-level I/O helpers -------------------------------------------------

static bool
send_all(sc_socket sock, const void *buf, size_t len) {
    return net_send_all(sock, buf, len) == (ssize_t) len;
}

static ssize_t
recv_line(sc_socket sock, char *buf, size_t cap) {
    // Read one CRLF-terminated line, byte-by-byte (signaling traffic is tiny).
    size_t n = 0;
    while (n + 1 < cap) {
        char c;
        ssize_t r = net_recv(sock, &c, 1);
        if (r != 1) return -1;
        buf[n++] = c;
        if (n >= 2 && buf[n-2] == '\r' && buf[n-1] == '\n') {
            buf[n-2] = '\0';
            return (ssize_t)(n - 2);
        }
    }
    return -1;
}

static bool
ascii_iequal(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
    }
    return true;
}

static char *
header_lookup(char headers[][512], size_t n, const char *name) {
    size_t nlen = strlen(name);
    for (size_t i = 0; i < n; ++i) {
        char *h = headers[i];
        char *c = strchr(h, ':');
        if (!c) continue;
        if ((size_t)(c - h) != nlen) continue;
        if (!ascii_iequal(h, name, nlen)) continue;
        char *v = c + 1;
        while (*v == ' ' || *v == '\t') ++v;
        return v;
    }
    return NULL;
}

// ---- static asset serving --------------------------------------------------

static void
serve_static(sc_socket sock, const char *content_type,
             const uint8_t *body, size_t body_len) {
    char header[256];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Cache-Control: no-store\r\n"
                     "Connection: close\r\n"
                     "\r\n", content_type, body_len);
    if (n < 0 || n >= (int) sizeof(header)) return;
    send_all(sock, header, (size_t) n);
    if (body_len) send_all(sock, body, body_len);
}

static void
serve_404(sc_socket sock) {
    const char *resp =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n";
    send_all(sock, resp, strlen(resp));
}

// ---- WebSocket framing -----------------------------------------------------

static bool
ws_send_frame(struct sc_ws_conn *conn, uint8_t opcode,
              const void *payload, size_t len) {
    uint8_t hdr[10];
    size_t hdr_len;
    hdr[0] = (uint8_t) (0x80 | (opcode & 0x0F));  // FIN + opcode
    if (len < 126) {
        hdr[1] = (uint8_t) len;
        hdr_len = 2;
    } else if (len < 65536) {
        hdr[1] = 126;
        hdr[2] = (uint8_t)(len >> 8);
        hdr[3] = (uint8_t) len;
        hdr_len = 4;
    } else {
        hdr[1] = 127;
        for (int i = 0; i < 8; ++i) {
            hdr[2 + i] = (uint8_t)(len >> (56 - i * 8));
        }
        hdr_len = 10;
    }
    sc_mutex_lock(&conn->send_mutex);
    bool ok = send_all(conn->socket, hdr, hdr_len);
    if (ok && len) ok = send_all(conn->socket, payload, len);
    sc_mutex_unlock(&conn->send_mutex);
    return ok;
}

bool
sc_ws_conn_send_text(struct sc_ws_conn *conn, const char *text, size_t len) {
    if (!conn->open) return false;
    return ws_send_frame(conn, 0x1, text, len);
}

void
sc_ws_conn_interrupt(struct sc_ws_conn *conn) {
    // The conn's recv loop will return -1 next time it touches the socket.
    // Safe to call multiple times.
    net_interrupt(conn->socket);
}

static bool
ws_recv_exact(sc_socket sock, void *buf, size_t len) {
    return net_recv_all(sock, buf, len) == (ssize_t) len;
}

// Returns 1 on text frame received (text/len filled, caller frees *text),
// 0 on graceful close, -1 on error or unsupported frame.
static int
ws_recv_text_frame(sc_socket sock, char **out_text, size_t *out_len) {
    for (;;) {
        uint8_t h[2];
        if (!ws_recv_exact(sock, h, 2)) {
            return -1;
        }
        bool fin = h[0] & 0x80;
        uint8_t opcode = h[0] & 0x0F;
        bool masked = h[1] & 0x80;
        uint64_t plen = h[1] & 0x7F;
        if (plen == 126) {
            uint8_t e[2];
            if (!ws_recv_exact(sock, e, 2)) return -1;
            plen = ((uint64_t) e[0] << 8) | e[1];
        } else if (plen == 127) {
            uint8_t e[8];
            if (!ws_recv_exact(sock, e, 8)) return -1;
            plen = 0;
            for (int i = 0; i < 8; ++i) plen = (plen << 8) | e[i];
        }
        if (plen > WS_FRAME_MAX_PAYLOAD) return -1;
        uint8_t mask[4] = {0};
        if (masked) {
            if (!ws_recv_exact(sock, mask, 4)) return -1;
        }
        uint8_t *payload = NULL;
        if (plen) {
            payload = malloc((size_t) plen + 1);
            if (!payload) return -1;
            if (!ws_recv_exact(sock, payload, (size_t) plen)) {
                free(payload);
                return -1;
            }
            if (masked) {
                for (size_t i = 0; i < plen; ++i) payload[i] ^= mask[i & 3];
            }
            payload[plen] = '\0';
        }

        if (opcode == 0x8) {  // close
            free(payload);
            return 0;
        }
        if (opcode == 0x9 || opcode == 0xA) {
            // ping/pong: silently ignored (MVP)
            free(payload);
            continue;
        }
        if (opcode != 0x1 || !fin) {
            free(payload);
            return -1;
        }
        *out_text = (char *) payload;
        *out_len = (size_t) plen;
        return 1;
    }
}

// ---- request handling ------------------------------------------------------

static void
ws_run_loop(struct sc_ws_conn *conn) {
    struct sc_http_server *server = conn->owner;
    if (server->cbs.on_ws_open) {
        server->cbs.on_ws_open(conn, server->cb_userdata);
    }
    while (!server->stopping) {
        char *text = NULL;
        size_t len = 0;
        int r = ws_recv_text_frame(conn->socket, &text, &len);
        if (r <= 0) {
            free(text);
            break;
        }
        if (server->cbs.on_ws_message) {
            server->cbs.on_ws_message(conn, text, len, server->cb_userdata);
        }
        free(text);
    }
    conn->open = false;
    if (server->cbs.on_ws_close) {
        server->cbs.on_ws_close(conn, server->cb_userdata);
    }
}

static void
handle_request(struct sc_ws_conn *conn) {
    struct sc_http_server *server = conn->owner;
    sc_socket sock = conn->socket;

    char line[MAX_REQUEST_LINE];
    if (recv_line(sock, line, sizeof(line)) < 0) return;

    // Parse request line: "METHOD PATH VERSION"
    char *sp1 = strchr(line, ' ');
    if (!sp1) return;
    *sp1 = '\0';
    char *path = sp1 + 1;
    char *sp2 = strchr(path, ' ');
    if (!sp2) return;
    *sp2 = '\0';

    char headers[32][512];
    size_t hc = 0;
    size_t total = 0;
    while (hc < 32) {
        char hline[512];
        ssize_t r = recv_line(sock, hline, sizeof(hline));
        if (r < 0) return;
        if (r == 0) break;  // end of headers
        total += (size_t) r;
        if (total > MAX_HEADER_TOTAL) return;
        memcpy(headers[hc++], hline, sizeof(hline));
    }

    if (strcmp(line, "GET") != 0) {
        serve_404(sock);
        return;
    }

    char *q = strchr(path, '?');
    if (q) *q = '\0';

    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        serve_static(sock, "text/html; charset=utf-8",
                     server->index_html, server->index_html_len);
        return;
    }
    if (strcmp(path, "/app.js") == 0) {
        serve_static(sock, "application/javascript; charset=utf-8",
                     server->app_js, server->app_js_len);
        return;
    }
    if (strcmp(path, "/signal") == 0) {
        char *upgrade = header_lookup(headers, hc, "Upgrade");
        char *key = header_lookup(headers, hc, "Sec-WebSocket-Key");
        if (!upgrade || !key) { serve_404(sock); return; }
        if (strlen(upgrade) < 9 || !ascii_iequal(upgrade, "websocket", 9)) {
            serve_404(sock);
            return;
        }
        char src[256];
        int n = snprintf(src, sizeof(src), "%s%s", key, WS_GUID);
        if (n < 0 || n >= (int) sizeof(src)) return;
        uint8_t digest[20];
        struct sha1 sh;
        sha1_init(&sh);
        sha1_update(&sh, (uint8_t *) src, (size_t) n);
        sha1_final(&sh, digest);
        char accept[64];
        base64_encode(digest, 20, accept);

        char resp[512];
        int rn = snprintf(resp, sizeof(resp),
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n"
            "\r\n", accept);
        if (rn < 0 || rn >= (int) sizeof(resp)) return;
        if (!send_all(sock, resp, (size_t) rn)) return;

        conn->is_ws = true;
        conn->open = true;
        // From here on, idle WebSocket sessions are expected (e.g. between
        // user gestures). Drop the read timeout we set in conn_thread.
        net_set_recv_timeout(sock, 0);
        ws_run_loop(conn);
        return;
    }

    serve_404(sock);
}

static int
conn_thread(void *arg) {
    struct sc_ws_conn *conn = arg;
    // Bound the time a request can spend half-read. Cleared after the
    // WebSocket upgrade succeeds, since signaling messages may legitimately
    // arrive minutes apart.
    net_set_recv_timeout(conn->socket, HTTP_REQUEST_RECV_TIMEOUT_MS);
    handle_request(conn);
    net_close(conn->socket);
    return 0;
}

static int
listener_thread(void *arg) {
    struct sc_http_server *s = arg;
    while (!s->stopping) {
        sc_socket client = net_accept(s->server_socket);
        if (client == SC_SOCKET_NONE) {
            break;
        }
        struct sc_ws_conn *conn = calloc(1, sizeof(*conn));
        if (!conn) {
            net_close(client);
            continue;
        }
        conn->owner = s;
        conn->socket = client;
        if (!sc_mutex_init(&conn->send_mutex)) {
            net_close(client);
            free(conn);
            continue;
        }
        conn->send_mutex_inited = true;

        sc_mutex_lock(&s->conns_mutex);
        if (s->conns_count == s->conns_capacity) {
            size_t newcap = s->conns_capacity ? s->conns_capacity * 2 : 8;
            struct sc_ws_conn **r =
                reallocarray(s->conns, newcap, sizeof(*r));
            if (!r) {
                sc_mutex_unlock(&s->conns_mutex);
                sc_mutex_destroy(&conn->send_mutex);
                net_close(client);
                free(conn);
                continue;
            }
            s->conns = r;
            s->conns_capacity = newcap;
        }
        s->conns[s->conns_count++] = conn;
        sc_mutex_unlock(&s->conns_mutex);

        if (!sc_thread_create(&conn->thread, conn_thread,
                              "scrcpy-ws-conn", conn)) {
            sc_mutex_lock(&s->conns_mutex);
            for (size_t i = 0; i < s->conns_count; ++i) {
                if (s->conns[i] == conn) {
                    s->conns[i] = s->conns[--s->conns_count];
                    break;
                }
            }
            sc_mutex_unlock(&s->conns_mutex);
            sc_mutex_destroy(&conn->send_mutex);
            net_close(client);
            free(conn);
            continue;
        }
    }
    return 0;
}

bool
sc_http_server_init(struct sc_http_server *s) {
    memset(s, 0, sizeof(*s));
    s->server_socket = SC_SOCKET_NONE;
    if (!sc_mutex_init(&s->conns_mutex)) {
        return false;
    }
    return true;
}

bool
sc_http_server_start(struct sc_http_server *s, uint16_t port,
                     const uint8_t *index_html, size_t index_html_len,
                     const uint8_t *app_js, size_t app_js_len,
                     const struct sc_http_server_callbacks *cbs,
                     void *cb_userdata) {
    s->server_socket = net_socket();
    if (s->server_socket == SC_SOCKET_NONE) {
        LOGE("HTTP server: socket() failed");
        return false;
    }
    if (!net_listen(s->server_socket, 0, port, 8)) {
        LOGE("HTTP server: listen on port %u failed", (unsigned) port);
        net_close(s->server_socket);
        s->server_socket = SC_SOCKET_NONE;
        return false;
    }
    s->port = port;
    s->index_html = index_html;
    s->index_html_len = index_html_len;
    s->app_js = app_js;
    s->app_js_len = app_js_len;
    s->cbs = *cbs;
    s->cb_userdata = cb_userdata;

    if (!sc_thread_create(&s->listener, listener_thread,
                          "scrcpy-http", s)) {
        net_close(s->server_socket);
        s->server_socket = SC_SOCKET_NONE;
        return false;
    }
    return true;
}

void
sc_http_server_stop(struct sc_http_server *s) {
    s->stopping = true;
    if (s->server_socket != SC_SOCKET_NONE) {
        net_interrupt(s->server_socket);
    }
    // Wake all active connections so their recv() returns.
    sc_mutex_lock(&s->conns_mutex);
    for (size_t i = 0; i < s->conns_count; ++i) {
        net_interrupt(s->conns[i]->socket);
    }
    sc_mutex_unlock(&s->conns_mutex);
}

void
sc_http_server_destroy(struct sc_http_server *s) {
    if (s->server_socket != SC_SOCKET_NONE) {
        sc_thread_join(&s->listener, NULL);
        net_close(s->server_socket);
        s->server_socket = SC_SOCKET_NONE;
    }
    // Join every conn thread. We do not hold the mutex during join: by this
    // point the listener is stopped and no new conns are being added.
    for (size_t i = 0; i < s->conns_count; ++i) {
        struct sc_ws_conn *c = s->conns[i];
        sc_thread_join(&c->thread, NULL);
        if (c->send_mutex_inited) {
            sc_mutex_destroy(&c->send_mutex);
        }
        free(c);
    }
    free(s->conns);
    s->conns = NULL;
    s->conns_count = 0;
    s->conns_capacity = 0;
    sc_mutex_destroy(&s->conns_mutex);
}
