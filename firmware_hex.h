/*
 * Copyright (c) 2026 Dave, KY4WI
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TKX4X_FIRMWARE_HEX_H
#define TKX4X_FIRMWARE_HEX_H

#include <stddef.h>
#include <stdint.h>

#define KENWOOD_FIRMWARE_BYTES 0x6600u
#define KENWOOD_HEX_EXPORT_BYTES 73478u

/* Decode Kenwood's addressed Intel HEX: contiguous 0x6600 bytes, optional
   three-byte trailer (big-endian additive sum and radio family), then EOF.
   The destination is not modified unless the entire file validates. */
int kenwood_hex_decode(const uint8_t *text, size_t size,
                       uint8_t image[KENWOOD_FIRMWARE_BYTES],
                       int *family, char *error, size_t error_size);

/* The interactive loader accepts intact records even when the embedded
   checksums are stale. These warnings do not certify firmware correctness. */
struct kenwood_hex_warnings {
    unsigned bad_record_checksums;
    unsigned first_bad_record_line;
    int firmware_checksum_mismatch;
    uint16_t trailer_checksum;
    uint16_t calculated_checksum;
};
int kenwood_hex_decode_with_warnings(const uint8_t *text, size_t size,
                                     uint8_t image[KENWOOD_FIRMWARE_BYTES],
                                     int *family, struct kenwood_hex_warnings *warnings,
                                     char *error, size_t error_size);

/* Generate the Kenwood format observed in original TK-x4x firmware files:
   three leading CRLFs, 16-byte records, three-byte trailer, EOF, DOS CRLF.
   family must be 4 (TK-840) or 5 (TK-940/941). */
int kenwood_hex_encode(const uint8_t image[KENWOOD_FIRMWARE_BYTES],
                       int family, uint8_t *out, size_t capacity,
                       size_t *written);
#endif
