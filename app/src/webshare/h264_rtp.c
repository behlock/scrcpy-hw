#include "h264_rtp.h"

#include <stdlib.h>
#include <string.h>

static bool
is_start4(const uint8_t *p, size_t remain) {
    return remain >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1;
}

static bool
is_start3(const uint8_t *p, size_t remain) {
    return remain >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1;
}

bool
sc_h264_annexb_to_avcc(const uint8_t *in, size_t in_len,
                       uint8_t **out, size_t *out_len) {
    // Walk NAL boundaries first, then size and emit. Two-pass keeps it simple
    // and avoids realloc churn.
    if (!in || !in_len) return false;

    // First pass: find NAL unit ranges (start of payload, length).
    struct nal { size_t off; size_t len; };
    struct nal *nals = NULL;
    size_t nal_count = 0;
    size_t nal_cap = 0;

    size_t i = 0;
    // Skip leading start code (if any)
    while (i < in_len) {
        size_t code_len;
        if (is_start4(in + i, in_len - i)) code_len = 4;
        else if (is_start3(in + i, in_len - i)) code_len = 3;
        else break;
        i += code_len;
        // Find next start code (or EOF)
        size_t start = i;
        size_t end = in_len;
        for (size_t j = i; j + 2 < in_len; ++j) {
            if (is_start3(in + j, in_len - j) || is_start4(in + j, in_len - j)) {
                end = j;
                break;
            }
        }
        if (end < start) end = start;
        if (nal_count == nal_cap) {
            size_t newcap = nal_cap ? nal_cap * 2 : 8;
            struct nal *r = realloc(nals, newcap * sizeof(*nals));
            if (!r) {
                free(nals);
                return false;
            }
            nals = r;
            nal_cap = newcap;
        }
        nals[nal_count].off = start;
        nals[nal_count].len = end - start;
        ++nal_count;
        i = end;
    }

    if (!nal_count) {
        free(nals);
        return false;
    }

    // Second pass: total size = sum(4 + nal.len)
    size_t total = 0;
    for (size_t k = 0; k < nal_count; ++k) total += 4 + nals[k].len;

    uint8_t *buf = malloc(total);
    if (!buf) {
        free(nals);
        return false;
    }

    uint8_t *w = buf;
    for (size_t k = 0; k < nal_count; ++k) {
        uint32_t L = (uint32_t) nals[k].len;
        w[0] = (uint8_t)(L >> 24);
        w[1] = (uint8_t)(L >> 16);
        w[2] = (uint8_t)(L >> 8);
        w[3] = (uint8_t) L;
        w += 4;
        memcpy(w, in + nals[k].off, nals[k].len);
        w += nals[k].len;
    }

    free(nals);
    *out = buf;
    *out_len = total;
    return true;
}
