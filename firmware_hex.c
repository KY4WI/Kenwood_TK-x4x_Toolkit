/*
 * Copyright (c) 2026 Dave, KY4WI
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "firmware_hex.h"
#include <stdio.h>
#include <string.h>

static void explain(char *message, size_t capacity, const char *why)
{
    if (capacity) snprintf(message, capacity, "%s", why);
}

static int nibble(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int pair(const uint8_t *s)
{
    int a = nibble(s[0]), b = nibble(s[1]);
    if (a < 0 || b < 0) return -1;
    return (a << 4) | b;
}

static uint16_t firmware_sum(const uint8_t *bytes)
{
    uint32_t result = 0;
    size_t i;
    for (i = 0; i < KENWOOD_FIRMWARE_BYTES; ++i) result += bytes[i];
    return (uint16_t)result;
}

static int kenwood_hex_decode_internal(const uint8_t *text, size_t size,
                                       uint8_t image[KENWOOD_FIRMWARE_BYTES],
                                       int *family,
                                       struct kenwood_hex_warnings *warnings,
                                       int accept_bad_checksums,
                                       char *error, size_t error_size)
{
    uint8_t temporary[KENWOOD_FIRMWARE_BYTES];
    size_t i = 0, next_address = 0, last_data_address = 0;
    unsigned last_data_count = 0;
    unsigned line_number = 1;
    int eof = 0, trailer = 0, trailer_family = 0;
    uint16_t trailer_sum = 0;
    struct kenwood_hex_warnings found = {0};

    if (family) *family = 0;
    if (warnings) *warnings = found;
    if (!text || !image) {
        explain(error, error_size, "Missing HEX input or destination.");
        return -1;
    }
    /* A size limit is defensive only. The file browser filters by .hex,
       not by size, and the parser validates all content. */
    if (!size || size > 1024u * 1024u) {
        explain(error, error_size, "HEX file is empty or unexpectedly large.");
        return -1;
    }
    while (i < size) {
        size_t first = i, last, len, j;
        unsigned record_line = line_number;
        int count, hi, lo, type, sum = 0;
        unsigned address;
        const uint8_t *p;
        while (i < size && text[i] != '\r' && text[i] != '\n') i++;
        last = i;
        while (i < size && (text[i] == '\r' || text[i] == '\n')) {
            if (text[i] == '\r' && i + 1 < size && text[i + 1] == '\n')
                i += 2;
            else
                i++;
            ++line_number;
        }
        while (first < last && (text[first] == ' ' || text[first] == '\t')) first++;
        while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t')) last--;
        len = last - first;
        if (!len) continue; /* Original Kenwood images start with blank lines. */
        if (eof || len < 11 || text[first] != ':') {
            explain(error, error_size, "Unexpected text or records after HEX EOF.");
            return -1;
        }
        p = text + first;
        count = pair(p + 1);
        hi = pair(p + 3);
        lo = pair(p + 5);
        type = pair(p + 7);
        if (count < 0 || hi < 0 || lo < 0 || type < 0 ||
            len != 11u + 2u * (size_t)count) {
            explain(error, error_size, "Invalid Intel HEX record length or header.");
            return -1;
        }
        for (j = 0; j < (size_t)count + 5u; j++) {
            int byte = pair(p + 1 + 2 * j);
            if (byte < 0) {
                explain(error, error_size, "Invalid HEX digit.");
                return -1;
            }
            sum = (sum + byte) & 255;
        }
        if (sum != 0) {
            if (!accept_bad_checksums) {
                explain(error, error_size, "Intel HEX record checksum mismatch.");
                return -1;
            }
            if (!found.bad_record_checksums)
                found.first_bad_record_line = record_line;
            found.bad_record_checksums++;
        }
        address = ((unsigned)hi << 8) | (unsigned)lo;
        if (type == 1 && count == 0 && address == 0) {
            eof = 1;
            continue;
        }
        if (type != 0 || count == 0) {
            explain(error, error_size, "Unsupported Intel HEX record type.");
            return -1;
        }
        if (address == KENWOOD_FIRMWARE_BYTES &&
            next_address == KENWOOD_FIRMWARE_BYTES && !trailer &&
            count == 3) {
            trailer_sum = (uint16_t)((pair(p + 9) << 8) | pair(p + 11));
            trailer_family = pair(p + 13);
            if (trailer_family != 4 && trailer_family != 5) {
                if (error_size)
                    snprintf(error,error_size,
                             "Record at 0x%04X has model byte %02X.\n"
                             "TK-x4x requires 04 (TK-840) or 05 (TK-940/941).",
                             (unsigned)address, (unsigned)trailer_family);
                return -1;
            }
            trailer = 1;
            continue;
        }
        if (trailer || (size_t)address != next_address) {
            explain(error, error_size, "HEX addresses overlap, skip data, or follow the firmware trailer.");
            return -1;
        }
        last_data_address = next_address;
        last_data_count = (unsigned)count;
        /* Continue validating the address map even if the firmware image
           is larger than the 0x6600-byte TK-x4x firmware area. This yields
           the actual size instead of mislabeling other radios' HEX data as
           an invalid checksum trailer. Never write beyond temporary[]. */
        if (next_address < KENWOOD_FIRMWARE_BYTES) {
            size_t copy_count = (size_t)count;
            if (copy_count > KENWOOD_FIRMWARE_BYTES - next_address)
                copy_count = KENWOOD_FIRMWARE_BYTES - next_address;
            for (j = 0; j < copy_count; ++j)
                temporary[next_address + j] = (uint8_t)pair(p + 9 + 2 * j);
        }
        next_address += (size_t)count;
    }
    if (!eof) {
        explain(error, error_size, "Missing Intel HEX EOF record.");
        return -1;
    }
    if (next_address > KENWOOD_FIRMWARE_BYTES) {
        if (error_size) {
            if (last_data_count == 2 &&
                last_data_address + last_data_count == next_address &&
                last_data_address >= KENWOOD_FIRMWARE_BYTES) {
                snprintf(error,error_size,
                         "Incompatible firmware size/layout:\n"
                         "  Data: %zu bytes (0x%zX)\n"
                         "  Extra: 2-byte record at 0x%zX\n"
                         "  Expected: %u bytes (0x%X), plus an optional\n"
                         "  3-byte checksum/model record at 0x%X.",
                         last_data_address,last_data_address,last_data_address,
                         (unsigned)KENWOOD_FIRMWARE_BYTES,
                         (unsigned)KENWOOD_FIRMWARE_BYTES,
                         (unsigned)KENWOOD_FIRMWARE_BYTES);
            } else {
                snprintf(error,error_size,
                         "Incompatible firmware size/layout:\n"
                         "  Data: %zu bytes (0x%zX)\n"
                         "  Expected: %u bytes (0x%X).",
                         next_address,next_address,
                         (unsigned)KENWOOD_FIRMWARE_BYTES,
                         (unsigned)KENWOOD_FIRMWARE_BYTES);
            }
        }
        return -1;
    }
    if (next_address != KENWOOD_FIRMWARE_BYTES) {
        if (error_size)
            snprintf(error,error_size,
                     "Incomplete firmware image:\n"
                     "  Data: %zu bytes (0x%zX)\n"
                     "  Expected: %u bytes (0x%X).",
                     next_address,next_address,(unsigned)KENWOOD_FIRMWARE_BYTES,
                     (unsigned)KENWOOD_FIRMWARE_BYTES);
        return -1;
    }
    if (trailer) {
        found.trailer_checksum = trailer_sum;
        found.calculated_checksum = firmware_sum(temporary);
        if (found.calculated_checksum != trailer_sum) {
            if (!accept_bad_checksums) {
                explain(error, error_size, "Kenwood firmware checksum mismatch.");
                return -1;
            }
            found.firmware_checksum_mismatch = 1;
        }
    }
    memcpy(image, temporary, KENWOOD_FIRMWARE_BYTES);
    if (family) *family = trailer_family;
    if (warnings) *warnings = found;
    if (error_size) error[0] = '\0';
    return 0;
}

/* Strict decoder remains available for clients needing integrity enforcement. */
int kenwood_hex_decode(const uint8_t *text, size_t size,
                       uint8_t image[KENWOOD_FIRMWARE_BYTES],
                       int *family, char *error, size_t error_size)
{
    return kenwood_hex_decode_internal(text,size,image,family,NULL,0,error,error_size);
}

/* Interactive import: report mismatches rather than veto readable firmware. */
int kenwood_hex_decode_with_warnings(const uint8_t *text, size_t size,
                                     uint8_t image[KENWOOD_FIRMWARE_BYTES],
                                     int *family, struct kenwood_hex_warnings *warnings,
                                     char *error, size_t error_size)
{
    return kenwood_hex_decode_internal(text,size,image,family,warnings,1,
                                       error,error_size);
}

static const char digits[] = "0123456789ABCDEF";

static int append_hex(uint8_t *out, size_t cap, size_t *position, unsigned byte)
{
    if (*position + 2 > cap) return -1;
    out[(*position)++] = (uint8_t)digits[(byte >> 4) & 15u];
    out[(*position)++] = (uint8_t)digits[byte & 15u];
    return 0;
}

static int append_record(uint8_t *out, size_t cap, size_t *position,
                         unsigned address, unsigned type,
                         const uint8_t *bytes, unsigned count)
{
    unsigned i, sum = count + (address >> 8) + (address & 255u) + type;
    if (count > 16 || *position + 13u + 2u * count > cap) return -1;
    out[(*position)++] = ':';
    if (append_hex(out, cap, position, count) ||
        append_hex(out, cap, position, address >> 8) ||
        append_hex(out, cap, position, address) ||
        append_hex(out, cap, position, type)) return -1;
    for (i = 0; i < count; i++) {
        if (append_hex(out, cap, position, bytes[i])) return -1;
        sum += bytes[i];
    }
    if (append_hex(out, cap, position, (0u - sum) & 255u)) return -1;
    out[(*position)++] = '\r';
    out[(*position)++] = '\n';
    return 0;
}

int kenwood_hex_encode(const uint8_t image[KENWOOD_FIRMWARE_BYTES],
                       int family, uint8_t *out, size_t capacity,
                       size_t *written)
{
    size_t offset, pos = 0;
    uint16_t sum;
    uint8_t trailer[3];
    if (!image || !out || !written || (family != 4 && family != 5) ||
        capacity < KENWOOD_HEX_EXPORT_BYTES) return -1;
    memcpy(out, "\r\n\r\n\r\n", 6);
    pos = 6;
    for (offset = 0; offset < KENWOOD_FIRMWARE_BYTES; offset += 16) {
        if (append_record(out, capacity, &pos, (unsigned)offset, 0,
                          image + offset, 16)) return -1;
    }
    sum = firmware_sum(image);
    trailer[0] = (uint8_t)(sum >> 8);
    trailer[1] = (uint8_t)sum;
    trailer[2] = (uint8_t)family;
    if (append_record(out, capacity, &pos, KENWOOD_FIRMWARE_BYTES,
                      0, trailer, 3) ||
        append_record(out, capacity, &pos, 0, 1, NULL, 0)) return -1;
    if (pos != KENWOOD_HEX_EXPORT_BYTES) return -1;
    *written = pos;
    return 0;
}
