/* Copyright (c) 2026 Dave, KY4WI */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TKX4X_FWTRANSMUTE_H
#define TKX4X_FWTRANSMUTE_H
#include <stddef.h>
#include <stdint.h>
#define FWTRANSMUTE_IMAGE_SIZE 0x6600u
enum fwtransmute_status { FWTRANSMUTE_OK=0, FWTRANSMUTE_UNSUPPORTED=1, FWTRANSMUTE_ERROR=-1 };
struct fwtransmute_result {
    char source_revision[8];
    unsigned stages;
    int removed_tx;
    int removed_dealer;
    unsigned removed_plus25;
    unsigned removed_padding;
    int removed_tot; /* 1=Enhanced TOT, 2=unconditional legacy bypass */
    int was_stock_ab00;
};
int fwtransmute_to_ab00(const uint8_t *input,size_t size,uint8_t *output,
                       struct fwtransmute_result *report);
/* Convert recognized TK-840 9306 (including known TOT variants) to the
   exact verified stock 8355 image. No output changes on failure. */
int fwtransmute_to_8355(const uint8_t *input,size_t size,uint8_t *output);
/* Restore the same recognized firmware revision to exact stock. Never
   change output on failure. Includes reconstructed TK-840 9306 legacy and Enhanced TOT
   patch removal, verified against the full stock-candidate SHA-256. */
int fwtransmute_restore_stock(const uint8_t *input,size_t size,uint8_t *output);
#endif
