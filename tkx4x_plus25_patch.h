/*
 * Copyright (c) 2026 Dave, KY4WI
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TKX4X_PLUS25_PATCH_H
#define TKX4X_PLUS25_PATCH_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Legacy +25 MHz fixed-offset patch in the known TK-940/941 revisions.
   All three complete instruction sequences must be stock or recognized
   patched forms. Never infer a patch from one changed immediate alone.
   Addresses are CPU addresses; the image begins at 0x8000. */
#define TKX4X_PLUS25_IMAGE_BASE 0x8000u
#define TKX4X_PLUS25_IMAGE_BYTES 0x6600u
#define TKX4X_PLUS25_SITE_BYTES 7u
#define TKX4X_PLUS25_UNKNOWN (-1)
#define TKX4X_PLUS25_STOCK 0
#define TKX4X_PLUS25_PRESENT 1

static const uint8_t tkx4x_plus25_stock[7] =
    {0x2D,0xC0,0xFD,0x1A,0x20,0x82,0x05};
static const uint8_t tkx4x_plus25_short[7] =
    {0x2D,0x20,0x02,0x1A,0x20,0x82,0x05};
static const uint8_t tkx4x_plus25_full[7] =
    {0x2D,0x20,0x02,0x1A,0x20,0x00,0x00};

static inline const uint16_t *tkx4x_plus25_sites(unsigned revision)
{
    static const uint16_t ab00[3]={0x9AB0u,0x9C4Bu,0x9C7Au};
    static const uint16_t b5ec[3]={0x9AB6u,0x9C58u,0x9C87u};
    static const uint16_t v2cca[3]={0x985Bu,0x99FDu,0x9A2Cu};
    static const uint16_t v8e9d[3]={0x9889u,0x9A2Bu,0x9A5Au};
    switch(revision){
    case 0xAB00u:return ab00;
    case 0xB5ECu:return b5ec;
    case 0x2CCAu:return v2cca;
    case 0x8E9Du:return v8e9d;
    default:return NULL;
    }
}

static inline int tkx4x_plus25_site_kind(const uint8_t *site)
{
    if(!memcmp(site,tkx4x_plus25_stock,TKX4X_PLUS25_SITE_BYTES))return 0;
    if(!memcmp(site,tkx4x_plus25_short,TKX4X_PLUS25_SITE_BYTES))return 1;
    if(!memcmp(site,tkx4x_plus25_full,TKX4X_PLUS25_SITE_BYTES))return 2;
    return -1;
}

static inline int tkx4x_plus25_state(const uint8_t *image,unsigned revision)
{
    const uint16_t *sites=tkx4x_plus25_sites(revision);
    unsigned i,stock=0,patched=0;
    if(!image || !sites)return TKX4X_PLUS25_UNKNOWN;
    for(i=0;i<3u;i++){
        size_t off=(size_t)(sites[i]-TKX4X_PLUS25_IMAGE_BASE);
        int kind;
        if(off+TKX4X_PLUS25_SITE_BYTES>TKX4X_PLUS25_IMAGE_BYTES)
            return TKX4X_PLUS25_UNKNOWN;
        kind=tkx4x_plus25_site_kind(image+off);
        if(kind==0)stock++;
        else if(kind>0)patched++;
        else return TKX4X_PLUS25_UNKNOWN;
    }
    if(stock==3u)return TKX4X_PLUS25_STOCK;
    if(patched==3u)return TKX4X_PLUS25_PRESENT;
    return TKX4X_PLUS25_UNKNOWN;
}

static inline int tkx4x_plus25_restore(uint8_t *image,unsigned revision)
{
    const uint16_t *sites=tkx4x_plus25_sites(revision);
    unsigned i;
    if(tkx4x_plus25_state(image,revision)!=TKX4X_PLUS25_PRESENT)
        return -1;
    for(i=0;i<3u;i++)
        memcpy(image+(size_t)(sites[i]-TKX4X_PLUS25_IMAGE_BASE),
               tkx4x_plus25_stock,TKX4X_PLUS25_SITE_BYTES);
    return tkx4x_plus25_state(image,revision)==TKX4X_PLUS25_STOCK?0:-1;
}

/* Preserve an independently installed legacy +25 MHz patch while removing
   another patch. This copies only the three validated seven-byte sites. */
static inline int tkx4x_plus25_reapply(uint8_t *dest,const uint8_t *source,
                                       unsigned revision)
{
    const uint16_t *sites=tkx4x_plus25_sites(revision);
    unsigned i;
    if(!sites || tkx4x_plus25_state(source,revision)!=TKX4X_PLUS25_PRESENT ||
       tkx4x_plus25_state(dest,revision)!=TKX4X_PLUS25_STOCK)return -1;
    for(i=0;i<3u;i++){
        size_t off=(size_t)(sites[i]-TKX4X_PLUS25_IMAGE_BASE);
        memcpy(dest+off,source+off,TKX4X_PLUS25_SITE_BYTES);
    }
    return tkx4x_plus25_state(dest,revision)==TKX4X_PLUS25_PRESENT?0:-1;
}
#endif
