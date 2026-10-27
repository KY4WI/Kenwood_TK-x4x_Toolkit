/* Copyright (c) 2026 Dave, KY4WI */
/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Synthetic firmware only: never embed or distribute copyrighted images. */
#include "../firmware_hex.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int hex_digit(uint8_t c)
{
    if(c>='0' && c<='9')return c-'0';
    if(c>='A' && c<='F')return c-'A'+10;
    return -1;
}

static unsigned hex_byte(const uint8_t *p)
{
    return (unsigned)((hex_digit(p[0])<<4) | hex_digit(p[1]));
}

/* Recompute one Intel HEX record checksum after altering a data byte;
   this isolates the independent Kenwood trailer checksum check. */
static void correct_record_checksum(uint8_t *record)
{
    static const char digits[]="0123456789ABCDEF";
    unsigned count=hex_byte(record+1),sum=0,byte;
    size_t i;
    for(i=0;i<count+4u;i++)sum+=hex_byte(record+1u+2u*i);
    byte=(0u-sum)&255u;
    record[9u+2u*count]=(uint8_t)digits[byte>>4];
    record[10u+2u*count]=(uint8_t)digits[byte&15u];
}

static size_t append_synthetic_record(uint8_t *buffer, size_t pos,
                                      unsigned address, unsigned type,
                                      const uint8_t *data, unsigned count)
{
    static const char digits[] = "0123456789ABCDEF";
    unsigned sum = count + (address >> 8) + (address & 255u) + type;
    unsigned k;
    uint8_t values[260];
    values[0] = (uint8_t)count;
    values[1] = (uint8_t)(address >> 8);
    values[2] = (uint8_t)address;
    values[3] = (uint8_t)type;
    for(k = 0; k < count; k++){
        values[4u + k] = data[k];
        sum += data[k];
    }
    values[4u + count] = (uint8_t)(0u - sum);
    buffer[pos++] = ':';
    for(k = 0; k < count + 5u; k++){
        buffer[pos++] = (uint8_t)digits[values[k] >> 4];
        buffer[pos++] = (uint8_t)digits[values[k] & 15u];
    }
    buffer[pos++] = '\r';
    buffer[pos++] = '\n';
    return pos;
}

/* Diagnostics are displayed on an 80-column text terminal. */
static void assert_80_column_lines(const char *message)
{
    size_t width=0;
    const unsigned char *p=(const unsigned char *)message;
    for(;*p;p++){
        if(*p=='\n'){
            assert(width<=80u);
            width=0u;
        }else{
            width++;
            assert(width<=80u);
        }
    }
}

int main(void)
{
    uint8_t *a = malloc(KENWOOD_FIRMWARE_BYTES);
    uint8_t *b = malloc(KENWOOD_FIRMWARE_BYTES);
    uint8_t *hex = malloc(KENWOOD_HEX_EXPORT_BYTES);
    char why[384];
    size_t n, i, count;
    int tag;
    struct kenwood_hex_warnings warnings;
    assert(a && b && hex);
    for (i = 0; i < KENWOOD_FIRMWARE_BYTES; i++)
        a[i] = (uint8_t)(i * 37u + 11u);
    assert(kenwood_hex_encode(a, 5, hex, KENWOOD_HEX_EXPORT_BYTES, &n) == 0);
    assert(n == 73478u);
    assert(!memcmp(hex, "\r\n\r\n\r\n:10", 9));
    assert(!memcmp(hex + n - 13, ":00000001FF\r\n", 13));
    assert(kenwood_hex_decode(hex, n, b, &tag, why, sizeof(why)) == 0);
    assert(tag == 5 && !memcmp(a, b, KENWOOD_FIRMWARE_BYTES));
    assert(kenwood_hex_decode_with_warnings(hex,n,b,&tag,&warnings,
                                            why,sizeof(why))==0);
    assert(!warnings.bad_record_checksums && !warnings.firmware_checksum_mismatch);

    /* A stale Kenwood trailer must warn, not prevent loading edited data. */
    hex[15] = (hex[15] == '0') ? '1' : '0';
    assert(kenwood_hex_decode(hex,n,b,&tag,why,sizeof(why)) != 0);
    assert(kenwood_hex_decode_with_warnings(hex,n,b,&tag,&warnings,
                                            why,sizeof(why))==0);
    assert(warnings.bad_record_checksums==1);
    assert(warnings.first_bad_record_line==4);
    assert(warnings.firmware_checksum_mismatch==1);
    assert(b[0]!=a[0] && !memcmp(a+1,b+1,KENWOOD_FIRMWARE_BYTES-1));

    /* Repair only the record checksum; the trailer is still stale. */
    correct_record_checksum(hex+6);
    assert(kenwood_hex_decode(hex,n,b,&tag,why,sizeof(why)) != 0);
    assert(kenwood_hex_decode_with_warnings(hex,n,b,&tag,&warnings,
                                            why,sizeof(why))==0);
    assert(!warnings.bad_record_checksums);
    assert(warnings.firmware_checksum_mismatch);
    assert(warnings.calculated_checksum!=warnings.trailer_checksum);
    assert(b[0]!=a[0]);

    /* An invalid record checksum alone is also a non-blocking warning. */
    assert(kenwood_hex_encode(a,5,hex,KENWOOD_HEX_EXPORT_BYTES,&n)==0);
    hex[47]=(hex[47]=='0')?'1':'0';
    assert(kenwood_hex_decode_with_warnings(hex,n,b,&tag,&warnings,
                                            why,sizeof(why))==0);
    assert(warnings.bad_record_checksums==1);
    assert(!warnings.firmware_checksum_mismatch);
    assert(!memcmp(a,b,KENWOOD_FIRMWARE_BYTES));

    /* Structural errors are still rejected; previous data is untouched. */
    assert(kenwood_hex_encode(a,5,hex,KENWOOD_HEX_EXPORT_BYTES,&n)==0);
    hex[11]=(hex[11]=='0')?'1':'0';
    memset(b,0xa5,KENWOOD_FIRMWARE_BYTES);
    assert(kenwood_hex_decode_with_warnings(hex,n,b,&tag,&warnings,
                                            why,sizeof(why))!=0);
    for(i=0;i<KENWOOD_FIRMWARE_BYTES;i++)assert(b[i]==0xa5);

    /* A different Kenwood radio can have valid Intel HEX but a 32 KiB
       payload and a two-byte record at 0x8000 instead of the TK-x4x
       0x6600-byte firmware and optional three-byte record at 0x6600. */
    {
        uint8_t *large = malloc(93000u);
        const uint8_t footer[2] = {0x75,0x44};
        size_t used = 6u, offset;
        assert(large);
        memcpy(large,"\r\n\r\n\r\n",6u);
        for(offset=0;offset<0x8000u;offset+=16u){
            uint8_t data[16];
            size_t j;
            for(j=0;j<16u;j++)data[j]=(uint8_t)((offset+j)*37u+11u);
            used=append_synthetic_record(large,used,(unsigned)offset,0,data,16u);
        }
        used=append_synthetic_record(large,used,0x8000u,0,footer,2u);
        used=append_synthetic_record(large,used,0,1,NULL,0u);
        assert(used==92196u);
        memset(b,0xa5,KENWOOD_FIRMWARE_BYTES);
        assert(kenwood_hex_decode_with_warnings(large,used,b,&tag,
                                                &warnings,why,sizeof(why))!=0);
        assert(strstr(why,"Incompatible firmware size/layout:\n")!=NULL);
        assert_80_column_lines(why);
        assert(strstr(why,"32768 bytes (0x8000)")!=NULL);
        assert(strstr(why,"2-byte record at 0x8000")!=NULL);
        assert(strstr(why,"26112 bytes (0x6600)")!=NULL);
        for(offset=0;offset<KENWOOD_FIRMWARE_BYTES;offset++)
            assert(b[offset]==0xa5);
        free(large);
    }

    assert(kenwood_hex_encode(a, 4, hex, KENWOOD_HEX_EXPORT_BYTES, &n) == 0);
    /* Check LF-only file with all the optional initial empty lines removed. */
    count = 0;
    for (i = 6; i < n; i++)
        if (hex[i] != '\r') hex[count++] = hex[i];
    assert(kenwood_hex_decode(hex, count, b, &tag, why, sizeof(why)) == 0);
    assert(tag == 4 && !memcmp(a, b, KENWOOD_FIRMWARE_BYTES));
    printf("Firmware HEX: strict and permissive checksum handling, structural validation, LF-only OK\n");
    free(a);
    free(b);
    free(hex);
    return 0;
}
