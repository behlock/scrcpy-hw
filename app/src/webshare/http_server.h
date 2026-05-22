#ifndef SC_WEBSHARE_HTTP_SERVER_H
#define SC_WEBSHARE_HTTP_SERVER_H

#include "common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util/net.h"
#include "util/thread.h"

struct sc_ws_conn; // opaque handle for a connected WebSocket client

/**
 * Callback invoked on the connection's thread when the client opens the
 * WebSocket and on each inbound text message.
 *
 * `text` is NUL-terminated (allocated and owned by the server during the call;
 * do not retain). `userdata` is the value passed to sc_http_server_start.
 *
 * - `on_open` is called exactly once after the WebSocket handshake.
 * - `on_message` is called for each text frame received.
 * - `on_close` is called exactly once after the WebSocket closes, on the same
 *   thread. After it returns, the conn handle is invalid.
 */
struct sc_http_server_callbacks {
    void (*on_ws_open)(struct sc_ws_conn *conn, void *userdata);
    void (*on_ws_message)(struct sc_ws_conn *conn, const char *text,
                          size_t len, void *userdata);
    void (*on_ws_close)(struct sc_ws_conn *conn, void *userdata);
};

struct sc_http_server {
    sc_socket server_socket;
    uint16_t port;
    sc_thread listener;
    bool stopping;

    // Embedded static assets served on GET /
    const uint8_t *index_html;
    size_t index_html_len;
    const uint8_t *app_js;
    size_t app_js_len;

    struct sc_http_server_callbacks cbs;
    void *cb_userdata;

    // Active WebSocket connections (each runs its own thread).
    // Stored simply for shutdown coordination.
    sc_mutex conns_mutex;
    struct sc_ws_conn **conns;
    size_t conns_count;
    size_t conns_capacity;
};

bool
sc_http_server_init(struct sc_http_server *s);

bool
sc_http_server_start(struct sc_http_server *s, uint16_t port,
                     const uint8_t *index_html, size_t index_html_len,
                     const uint8_t *app_js, size_t app_js_len,
                     const struct sc_http_server_callbacks *cbs,
                     void *cb_userdata);

void
sc_http_server_stop(struct sc_http_server *s);

void
sc_http_server_destroy(struct sc_http_server *s);

/**
 * Send a text frame on this WebSocket. Returns false on send error or if the
 * connection is closing. Safe to call from any thread.
 */
bool
sc_ws_conn_send_text(struct sc_ws_conn *conn, const char *text, size_t len);

/**
 * Force this connection's read loop to exit so the conn thread can wind down.
 * Safe to call from any thread. The conn handle becomes invalid only after
 * the matching on_ws_close fires on the conn thread.
 */
void
sc_ws_conn_interrupt(struct sc_ws_conn *conn);

#endif
