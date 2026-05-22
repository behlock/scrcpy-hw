#include "qr_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/log.h"

#define QUIET_MODULES 2

bool
sc_qr_encode(struct sc_qr *qr, const char *text) {
    uint8_t temp[qrcodegen_BUFFER_LEN_MAX];
    bool ok = qrcodegen_encodeText(text, temp, qr->buf,
                                   qrcodegen_Ecc_MEDIUM,
                                   qrcodegen_VERSION_MIN,
                                   qrcodegen_VERSION_MAX,
                                   qrcodegen_Mask_AUTO,
                                   true);
    if (!ok) {
        LOGE("QR encoding failed for: %s", text);
    }
    return ok;
}

static bool
qr_module_with_quiet(const struct sc_qr *qr, int size, int x, int y) {
    int qx = x - QUIET_MODULES;
    int qy = y - QUIET_MODULES;
    if (qx < 0 || qy < 0 || qx >= size || qy >= size) {
        return false; // quiet zone is light
    }
    return qrcodegen_getModule(qr->buf, qx, qy);
}

void
sc_qr_print(const struct sc_qr *qr, FILE *out) {
    int size = qrcodegen_getSize(qr->buf);
    int dim = size + 2 * QUIET_MODULES;

    // Two QR modules per terminal cell: top half = even row, bottom = odd.
    // We set ANSI white background + black foreground so the printed block
    // glyphs (dark modules) appear black on white — scannable by every phone
    // camera regardless of the host terminal's color scheme.
    static const char *const FULL  = "\xe2\x96\x88"; // U+2588 full block
    static const char *const UPPER = "\xe2\x96\x80"; // U+2580 upper half
    static const char *const LOWER = "\xe2\x96\x84"; // U+2584 lower half
    static const char *const EMPTY = " ";

    fputs("\033[107;30m", out); // bright white bg, black fg
    for (int y = 0; y < dim; y += 2) {
        for (int x = 0; x < dim; ++x) {
            bool top = qr_module_with_quiet(qr, size, x, y);
            bool bot = (y + 1 < dim) && qr_module_with_quiet(qr, size, x, y + 1);
            const char *glyph;
            if (top && bot) {
                glyph = FULL;
            } else if (top) {
                glyph = UPPER;
            } else if (bot) {
                glyph = LOWER;
            } else {
                glyph = EMPTY;
            }
            fputs(glyph, out);
        }
        fputs("\033[0m\n\033[107;30m", out); // reset + restore for next row
    }
    fputs("\033[0m\n", out);
    fflush(out);
}
