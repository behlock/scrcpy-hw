#ifndef SC_WEBSHARE_H264_RTP_H
#define SC_WEBSHARE_H264_RTP_H

#include "common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Convert an H.264 Annex-B byte stream (NAL units separated by
 * 0x00000001 / 0x000001 start codes) to AVCC format (each NAL unit
 * prefixed by a 4-byte big-endian length).
 *
 * On success, *out is malloc'd (caller frees) and *out_len is set.
 * Returns false on malformed input or allocation failure.
 *
 * libdatachannel's H264 RTP handler expects the AVCC layout because it can
 * trivially walk the NAL boundaries to apply RFC 6184 fragmentation.
 */
bool
sc_h264_annexb_to_avcc(const uint8_t *in, size_t in_len,
                       uint8_t **out, size_t *out_len);

#endif
