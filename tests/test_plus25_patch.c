/* Copyright (c) 2026 Dave, KY4WI */
/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Synthetic +25 MHz patch tests. No copyrighted firmware included. */
#include "../tkx4x_plus25_patch.h"
#include <stdio.h>
#include <stdlib.h>

static int test_revision(unsigned revision)
{
    uint8_t *img=malloc(TKX4X_PLUS25_IMAGE_BYTES);
    uint8_t *other=malloc(TKX4X_PLUS25_IMAGE_BYTES);
    const uint16_t *sites=tkx4x_plus25_sites(revision);
    unsigned i,variation;
    int ok=0;
    if(!img || !other || !sites)goto done;
    for(i=0;i<3u;i++){
        size_t off=(size_t)(sites[i]-TKX4X_PLUS25_IMAGE_BASE);
        if(off+7u>TKX4X_PLUS25_IMAGE_BYTES)goto done;
    }
    for(variation=0;variation<3u;variation++){
        memset(img,0xff,TKX4X_PLUS25_IMAGE_BYTES);
        for(i=0;i<3u;i++){
            size_t off=(size_t)(sites[i]-TKX4X_PLUS25_IMAGE_BASE);
            memcpy(img+off,tkx4x_plus25_stock,7u);
        }
        if(tkx4x_plus25_state(img,revision)!=TKX4X_PLUS25_STOCK)goto done;
        memcpy(other,img,TKX4X_PLUS25_IMAGE_BYTES);
        for(i=0;i<3u;i++){
            size_t off=(size_t)(sites[i]-TKX4X_PLUS25_IMAGE_BASE);
            const uint8_t *patch= variation==0 ?
                tkx4x_plus25_short : (variation==1 ?
                tkx4x_plus25_full : (i==1 ?
                tkx4x_plus25_short : tkx4x_plus25_full));
            memcpy(img+off,patch,7u);
        }
        if(tkx4x_plus25_state(img,revision)!=TKX4X_PLUS25_PRESENT)goto done;
        if(tkx4x_plus25_reapply(other,img,revision))goto done;
        if(memcmp(other,img,TKX4X_PLUS25_IMAGE_BYTES))goto done;
        if(tkx4x_plus25_restore(img,revision))goto done;
        if(tkx4x_plus25_state(img,revision)!=TKX4X_PLUS25_STOCK)goto done;
        for(i=0;i<3u;i++){
            size_t off=(size_t)(sites[i]-TKX4X_PLUS25_IMAGE_BASE);
            if(memcmp(img+off,tkx4x_plus25_stock,7u))goto done;
        }
        if(tkx4x_plus25_restore(img,revision)!=-1)goto done;
        /* Partial patch must never be treated as a supported image. */
        memcpy(img+(size_t)(sites[0]-TKX4X_PLUS25_IMAGE_BASE),
               tkx4x_plus25_full,7u);
        if(tkx4x_plus25_state(img,revision)!=TKX4X_PLUS25_UNKNOWN)goto done;
        if(tkx4x_plus25_restore(img,revision)!=-1)goto done;
        if(tkx4x_plus25_reapply(other,img,revision)!=-1)goto done;
    }
    ok=1;
done:
    free(img);free(other);
    return ok;
}
int main(void)
{
    static const unsigned revisions[]={0xAB00,0xB5EC,0x2CCA,0x8E9D};
    size_t i;
    for(i=0;i<sizeof(revisions)/sizeof(revisions[0]);i++)
        if(!test_revision(revisions[i])){
            fprintf(stderr,"Synthetic +25 MHz patch test failed for %04X\n",revisions[i]);
            return 1;
        }
    if(tkx4x_plus25_sites(0x9306)!=NULL){
        fprintf(stderr,"TK-840 must not have a +25 MHz offset patch option\n");
        return 1;
    }
    puts("Synthetic +25 MHz patch recognition and round-trip passed.");
    return 0;
}
