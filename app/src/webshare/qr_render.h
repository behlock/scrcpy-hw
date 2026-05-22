#ifndef SC_QR_RENDER_H
#define SC_QR_RENDER_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "qrcodegen.h"

struct sc_qr {
    uint8_t buf[qrcodegen_BUFFER_LEN_MAX];
};

bool
sc_qr_encode(struct sc_qr *qr, const char *text);

/**
 * Print the QR to the given stream as UTF-8 half-block characters with a
 * single-module-thick light quiet zone. Two QR modules per terminal row.
 */
void
sc_qr_print(const struct sc_qr *qr, FILE *out);

#endif
