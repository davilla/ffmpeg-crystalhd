/*
 * a64 video encoder - multicolor modes
 * Copyright (c) 2009 Tobias Bindhammer
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * a64 video encoder - multicolor modes
 */

#include "a64enc.h"
#include "a64colors.h"
#include "a64tables.h"
#include "elbg.h"
#include "libavutil/intreadwrite.h"

#define DITHERSTEPS   8
#define CHARSET_CHARS 256

/* gray gradient */
static const int mc_colors[5]={0x0,0xb,0xc,0xf,0x1};

static void to_meta_with_crop(AVCodecContext *avctx, AVFrame *p, int *dest)
{
    int blockx, blocky, x, y;
    int luma = 0;
    int height = FFMIN(avctx->height,C64YRES);
    int width  = FFMIN(avctx->width ,C64XRES);
    uint8_t *src = p->data[0];

    for (blocky = 0; blocky < height; blocky += 8) {
        for (blockx = 0; blockx < C64XRES; blockx += 8) {
            for (y = blocky; y < blocky+8 && y < height; y++) {
                for (x = blockx; x < blockx+8 && x < C64XRES; x += 2) {
                    if(x < width) {
                        /* build average over 2 pixels */
                        luma = (src[(x + 0 + y * p->linesize[0])] +
                                src[(x + 1 + y * p->linesize[0])]) / 2;
                        /* write blocks as linear data now so they are suitable for elbg */
                        dest[0] = luma;
                    }
                    dest++;
                }
            }
        }
    }
}

static void render_charset(AVCodecContext *avctx, uint8_t *charset,
                           uint8_t *colrammap)
{
    A64Context *c = avctx->priv_data;
    uint8_t row1;
    int charpos, x, y;
    int a, b;
    uint8_t pix;
    int lowdiff, highdiff;
    int *best_cb = c->mc_best_cb;
    static uint8_t index1[256];
    static uint8_t index2[256];
    static uint8_t dither[256];
    int i;
    int distance;

    /* generate lookup-tables for dither and index before looping */
    i = 0;
    for (a=0; a < 256; a++) {
        if(i < 4 && a == c->mc_luma_vals[i+1]) {
            distance = c->mc_luma_vals[i+1] - c->mc_luma_vals[i];
            for(b = 0; b <= distance; b++) {
                  dither[c->mc_luma_vals[i]+b] = b * (DITHERSTEPS - 1) / distance;
            }
            i++;
        }
        if(i >=4 ) dither[a] = 0;
        index1[a] = i;
        index2[a] = FFMIN(i+1, 4);
    }
    /* and render charset */
    for (charpos = 0; charpos < CHARSET_CHARS; charpos++) {
        lowdiff  = 0;
        highdiff = 0;
        for (y = 0; y < 8; y++) {
            row1 = 0;
            for (x = 0; x < 4; x++) {
                pix = best_cb[y * 4 + x];

                /* accumulate error for brightest/darkest color */
                if (index1[pix] >= 3)
                    highdiff += pix - c->mc_luma_vals[3];
                if (index1[pix] < 1)
                    lowdiff += c->mc_luma_vals[1] - pix;

                row1 <<= 2;

                if (multi_dither_patterns[dither[pix]][(y & 3)][x & 3])
                    row1 |= 3-(index2[pix] & 3);
                else
                    row1 |= 3-(index1[pix] & 3);
            }
            charset[y+0x000] = row1;
        }
        /* do we need to adjust pixels? */
        if (highdiff > 0 && lowdiff > 0) {
            if (lowdiff > highdiff) {
                for (x = 0; x < 32; x++)
                    best_cb[x] = FFMIN(c->mc_luma_vals[3], best_cb[x]);
            } else {
                for (x = 0; x < 32; x++)
                    best_cb[x] = FFMAX(c->mc_luma_vals[1], best_cb[x]);
            }
            charpos--;          /* redo now adjusted char */
        /* no adjustment needed, all fine */
        } else {
            /* advance pointers */
            best_cb += 32;
            charset += 8;

            /* remember colorram value */
            colrammap[charpos] = (highdiff > 0) + 8;
        }
    }
}

static av_cold int a64multi_close_encoder(AVCodecContext *avctx)
{
    A64Context *c = avctx->priv_data;
    av_free(c->mc_meta_charset);
    av_free(c->mc_best_cb);
    av_free(c->mc_charmap);
    av_free(c->mc_charset);
    return 0;
}

static av_cold int a64multi_init_encoder(AVCodecContext *avctx)
{
    A64Context *c = avctx->priv_data;
    int a;
    av_lfg_init(&c->randctx, 1);

    if (avctx->global_quality < 1) {
        c->mc_lifetime = 4;
    } else {
        c->mc_lifetime = avctx->global_quality /= FF_QP2LAMBDA;
    }

    av_log(avctx, AV_LOG_INFO, "charset lifetime set to %d frame(s)\n", c->mc_lifetime);

    /* precalc luma values for later use */
    for (a = 0; a < 5; a++) {
        c->mc_luma_vals[a]=a64_palette[mc_colors[a]][0] * 0.30 +
                           a64_palette[mc_colors[a]][1] * 0.59 +
                           a64_palette[mc_colors[a]][2] * 0.11;
    }

    c->mc_frame_counter = 0;
    c->mc_use_5col      = avctx->codec->id == CODEC_ID_A64_MULTI5;
    c->mc_meta_charset  = av_malloc(32000 * c->mc_lifetime * sizeof(int));
    c->mc_best_cb       = av_malloc(CHARSET_CHARS * 32 * sizeof(int));
    c->mc_charmap       = av_malloc(1000 * c->mc_lifetime * sizeof(int));
    c->mc_charset       = av_malloc(0x800 * sizeof(uint8_t));

    avcodec_get_frame_defaults(&c->picture);
    avctx->coded_frame            = &c->picture;
    avctx->coded_frame->pict_type = FF_I_TYPE;
    avctx->coded_frame->key_frame = 1;
    if (!avctx->codec_tag)
         avctx->codec_tag = AV_RL32("a64m");

    return 0;
}

static int a64multi_encode_frame(AVCodecContext *avctx, unsigned char *buf,
                                 int buf_size, void *data)
{
    A64Context *c = avctx->priv_data;
    AVFrame *pict = data;
    AVFrame *const p = (AVFrame *) & c->picture;

    int frame;
    int a;

    uint8_t colrammap[256];
    int *charmap = c->mc_charmap;
    int *meta    = c->mc_meta_charset;
    int *best_cb = c->mc_best_cb;
    int frm_size = 0x400 + 0x400 * c->mc_use_5col;
    int req_size;

    /* it is the last frame so prepare to flush */
    if (!data)
        c->mc_lifetime = c->mc_frame_counter;

    req_size = 0x800 + frm_size * c->mc_lifetime;

    if (req_size > buf_size) {
        av_log(avctx, AV_LOG_ERROR, "buf size too small (need %d, got %d)\n", req_size, buf_size);
        return AVERROR(EINVAL);
    }
    /* fill up mc_meta_charset with framedata until lifetime exceeds */
    if (c->mc_frame_counter < c->mc_lifetime) {
        *p = *pict;
        p->pict_type = FF_I_TYPE;
        p->key_frame = 1;
        to_meta_with_crop(avctx, p, meta + 32000 * c->mc_frame_counter);
        c->mc_frame_counter++;
        /* lifetime is not reached */
        return 0;
    }
    /* lifetime exceeded so now convert X frames at once */
    if (c->mc_frame_counter == c->mc_lifetime && c->mc_lifetime > 0) {
        c->mc_frame_counter = 0;
        ff_init_elbg(meta, 32, 1000 * c-> mc_lifetime, best_cb, CHARSET_CHARS, 5, charmap, &c->randctx);
        ff_do_elbg  (meta, 32, 1000 * c-> mc_lifetime, best_cb, CHARSET_CHARS, 5, charmap, &c->randctx);

        render_charset(avctx, buf, colrammap);

        for (frame = 0; frame < c->mc_lifetime; frame++) {
            for (a = 0; a < 1000; a++) {
                buf[0x800 + a] = charmap[a];
                if (c->mc_use_5col) buf[0xc00 + a] = colrammap[charmap[a]];
            }
            buf += frm_size;
            charmap += 1000;
        }
        return req_size;
    }
    return 0;
}

AVCodec a64multi_encoder = {
    .name           = "a64multi",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = CODEC_ID_A64_MULTI,
    .priv_data_size = sizeof(A64Context),
    .init           = a64multi_init_encoder,
    .encode         = a64multi_encode_frame,
    .close          = a64multi_close_encoder,
    .pix_fmts       = (enum PixelFormat[]) {PIX_FMT_GRAY8, PIX_FMT_NONE},
    .long_name      = NULL_IF_CONFIG_SMALL("Multicolor charset for Commodore 64"),
    .capabilities   = CODEC_CAP_DELAY,
};

AVCodec a64multi5_encoder = {
    .name           = "a64multi5",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = CODEC_ID_A64_MULTI5,
    .priv_data_size = sizeof(A64Context),
    .init           = a64multi_init_encoder,
    .encode         = a64multi_encode_frame,
    .close          = a64multi_close_encoder,
    .pix_fmts       = (enum PixelFormat[]) {PIX_FMT_GRAY8, PIX_FMT_NONE},
    .long_name      = NULL_IF_CONFIG_SMALL("Multicolor charset for Commodore 64, extended with 5th color (colram)"),
    .capabilities   = CODEC_CAP_DELAY,
};
