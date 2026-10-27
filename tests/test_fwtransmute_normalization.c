/* Copyright (c) 2026 Dave, KY4WI */
/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Regression tests use fabricated arrays, not Kenwood firmware. */
#include "../fwtransmute.c"
static int run(void)
{
    uint8_t *img=malloc(FW_IMAGE_SIZE),*out=malloc(FW_IMAGE_SIZE);
    int kind,rc=1;
    size_t i;
    if(!img || !out)goto done;
    /* Both known +25 MHz variants must normalize exactly to stock.
       Mixed or partially modified instruction sequences must be rejected. */
    {
        uint8_t site[7];
        memcpy(site,stock_25,sizeof(site));
        if(normalize_plus25(site)!=0 ||
           memcmp(site,stock_25,sizeof(site)))goto done;
        memcpy(site,patched_25_short,sizeof(site));
        if(normalize_plus25(site)!=1 ||
           memcmp(site,stock_25,sizeof(site)))goto done;
        memcpy(site,patched_25_full,sizeof(site));
        if(normalize_plus25(site)!=1 ||
           memcmp(site,stock_25,sizeof(site)))goto done;
        memcpy(site,patched_25_full,sizeof(site));
        site[5]=0x82; /* half-restored instruction pair is unsupported */
        if(normalize_plus25(site)!=-1)goto done;
        memcpy(site,stock_25,sizeof(site));
        site[5]=0x00; /* NOP pair without +25 immediate is unsupported */
        site[6]=0x00;
        if(normalize_plus25(site)!=-1)goto done;
    }
    /* 9306: stock, legacy, Enhanced TOT and the two-byte 10x test mode
       must normalize to exactly the same synthetic stock bytes. */
    {
        unsigned variant;
        const size_t legacy_off=0xB173u-FW_CPU_BASE;
        for(variant=0;variant<4u;variant++){
            memset(img,0xff,FW_IMAGE_SIZE);
            for(i=0;i<2u;i++){
                const struct tot_patch_region *r=&tk840_9306_tot_regions[i];
                memcpy(img+(r->address-FW_CPU_BASE),r->original,r->length);
            }
            memcpy(out,img,FW_IMAGE_SIZE);
            if(variant==1u){
                img[legacy_off]=0u;img[legacy_off+1u]=0u;
            }else if(variant>=2u){
                for(i=0;i<2u;i++){
                    const struct tot_patch_region *r=&tk840_9306_tot_regions[i];
                    memcpy(img+(r->address-FW_CPU_BASE),r->modified,r->length);
                }
                if(variant==3u){
                    img[0x94EFu-FW_CPU_BASE+37u]=0x01u;
                    img[0xB15Fu-FW_CPU_BASE+11u]=0x01u;
                }
            }
            if(normalize_tk8409306_tot(img,&kind) ||
               kind!=(int)variant || memcmp(img,out,FW_IMAGE_SIZE))goto done;
        }
        /* Mixed stock and Enhanced regions must not normalize. */
        memcpy(img+(tk840_9306_tot_regions[0].address-FW_CPU_BASE),
               tk840_9306_tot_regions[0].modified,48u);
        if(normalize_tk8409306_tot(img,&kind)!=-1)goto done;
        memset(out,0xa5,FW_IMAGE_SIZE);
        if(fwtransmute_restore_stock(img,FW_IMAGE_SIZE,out)!=FWTRANSMUTE_UNSUPPORTED)
            goto done;
        for(i=0;i<FW_IMAGE_SIZE;i++)if(out[i]!=0xa5)goto done;
    }
    memset(img,0xff,FW_IMAGE_SIZE);
    for(i=0;i<3u;i++){
        const struct tot_patch_region *r=&ab00_tot_regions[i];
        memcpy(img+r->address-FW_CPU_BASE,r->original,r->length);
    }
    if(normalize_ab00_tot(img,&kind) || kind!=0)goto done;
    for(i=0;i<3u;i++){
        const struct tot_patch_region *r=&ab00_tot_regions[i];
        memcpy(img+r->address-FW_CPU_BASE,r->modified,r->length);
    }
    if(normalize_ab00_tot(img,&kind) || kind!=1)goto done;
    for(i=0;i<3u;i++){
        const struct tot_patch_region *r=&ab00_tot_regions[i];
        if(memcmp(img+r->address-FW_CPU_BASE,r->original,r->length))goto done;
    }
    /* Both speed-up bytes are accepted and restored; one alone is rejected. */
    for(i=0;i<3u;i++){
        const struct tot_patch_region *r=&ab00_tot_regions[i];
        memcpy(img+r->address-FW_CPU_BASE,r->modified,r->length);
    }
    img[0x950Bu-FW_CPU_BASE]=0x01;
    if(!normalize_ab00_tot(img,&kind))goto done;
    img[0xB1A5u-FW_CPU_BASE]=0x01;
    if(normalize_ab00_tot(img,&kind) || kind!=1)goto done;
    for(i=0;i<3u;i++){
        const struct tot_patch_region *r=&ab00_tot_regions[i];
        if(memcmp(img+r->address-FW_CPU_BASE,r->original,r->length))goto done;
    }
    img[0xB1AEu-FW_CPU_BASE]=0;img[0xB1AFu-FW_CPU_BASE]=0;
    if(normalize_ab00_tot(img,&kind) || kind!=2)goto done;
    if(img[0xB1AEu-FW_CPU_BASE]!=0x81 || img[0xB1AFu-FW_CPU_BASE]!=0x01)goto done;
    memcpy(img+ab00_tot_regions[0].address-FW_CPU_BASE,ab00_tot_regions[0].modified,
           ab00_tot_regions[0].length);
    if(!normalize_ab00_tot(img,&kind))goto done;
    /* Round-trip combined legacy/enhanced TOT and +25 MHz variants.
       Synthetic bytes only: no original firmware images in the repository. */
    {
        unsigned tot_variant,plus_variant;
        for(tot_variant=0;tot_variant<2u;tot_variant++){
            for(plus_variant=0;plus_variant<2u;plus_variant++){
                unsigned j;
                memset(img,0xff,FW_IMAGE_SIZE);
                for(i=0;i<3u;i++){
                    const struct tot_patch_region *r=&ab00_tot_regions[i];
                    memcpy(img+(r->address-FW_CPU_BASE),r->original,r->length);
                    memcpy(img+(revs[3].plus25_addr[i]-FW_CPU_BASE),
                           stock_25,sizeof(stock_25));
                }
                memcpy(out,img,FW_IMAGE_SIZE);
                for(i=0;i<3u;i++)
                    memcpy(img+(revs[3].plus25_addr[i]-FW_CPU_BASE),
                           plus_variant?patched_25_full:patched_25_short,
                           sizeof(stock_25));
                if(tot_variant){
                    for(i=0;i<3u;i++){
                        const struct tot_patch_region *r=&ab00_tot_regions[i];
                        memcpy(img+(r->address-FW_CPU_BASE),r->modified,r->length);
                    }
                }else{
                    img[0xB1AEu-FW_CPU_BASE]=0;
                    img[0xB1AFu-FW_CPU_BASE]=0;
                }
                if(normalize_ab00_tot(img,&kind) ||
                   kind!=(tot_variant?1:2))goto done;
                for(j=0;j<3u;j++)
                    if(normalize_plus25(img+(revs[3].plus25_addr[j]-FW_CPU_BASE))!=1)
                        goto done;
                if(memcmp(img,out,FW_IMAGE_SIZE))goto done;
            }
        }
    }
    /* SHA-pinned 9306 -> 8355 recipe must cover the target exactly,
       and a corrupted or unknown image must never change caller output.
       Synthetic data only: successful full-image conversion is separately
       verified against the independent, user-supplied target reference. */
    {
        size_t pos=0,covered=0,spans=0;
        while(pos<sizeof(tk840_9306_8355_map)){
            size_t src,len;
            if(sizeof(tk840_9306_8355_map)-pos<3u)goto done;
            src=(size_t)tk840_9306_8355_map[pos] |
                ((size_t)tk840_9306_8355_map[pos+1u]<<8);
            len=tk840_9306_8355_map[pos+2u];pos+=3u;
            if(len==255u){
                if(sizeof(tk840_9306_8355_map)-pos<2u)goto done;
                len=(size_t)tk840_9306_8355_map[pos] |
                    ((size_t)tk840_9306_8355_map[pos+1u]<<8);
                pos+=2u;
            }
            if(!len || src>=FW_IMAGE_SIZE ||
               len>FW_IMAGE_SIZE-src ||
               covered+len>FW_IMAGE_SIZE)goto done;
            covered+=len;spans++;
        }
        if(covered!=FW_IMAGE_SIZE || spans!=TK840_9306_8355_SPANS)
            goto done;
    }
    memset(img,0x12,FW_IMAGE_SIZE);memset(out,0xa5,FW_IMAGE_SIZE);
    if(fwtransmute_to_8355(img,FW_IMAGE_SIZE,out)!=FWTRANSMUTE_UNSUPPORTED)
        goto done;
    for(i=0;i<FW_IMAGE_SIZE;i++)if(out[i]!=0xa5)goto done;
    if(fwtransmute_to_ab00(img,FW_IMAGE_SIZE,out,NULL)!=FWTRANSMUTE_UNSUPPORTED)goto done;
    for(i=0;i<FW_IMAGE_SIZE;i++)if(out[i]!=0xa5)goto done;
    rc=0;
done:
    free(img);free(out);
    return rc;
}
int main(void){int rc=run();if(!rc)puts("Synthetic transmuter tests passed.");return rc;}
