/*****************************************************************************
 * timestamps_filter.h: Seamless timestamps helper
 *****************************************************************************
 * Copyright © 2018 VideoLabs, VideoLAN, VLC authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/
#ifndef VLC_TIMESTAMPS_FILTER_H
#define VLC_TIMESTAMPS_FILTER_H

#include <vlc_common.h>
#include <vlc_es_out.h>

#include "moving_avg.h"

//#define DEBUG_TIMESTAMPS_FILTER

enum
{
    ES_OUT_TF_FILTER_GET_TIME = (ES_OUT_PRIVATE_START + 1),
    ES_OUT_TF_FILTER_DISCONTINUITY,
    ES_OUT_TF_FILTER_RESET,
};

struct timestamps_filter_s
{
    struct moving_average_s mva;
    vlc_tick_t sequence_offset;
    vlc_tick_t contiguous_last;
    unsigned sequence;
};

struct tf_es_out_id_s
{
    es_out_id_t *id;
    vlc_fourcc_t fourcc;
    struct timestamps_filter_s tf;
    vlc_tick_t pcrdiff;
    unsigned pcrpacket;
    bool contiguous;
};

struct tf_es_out_s
{
    es_out_t *original_es_out;
    DECL_ARRAY(struct tf_es_out_id_s *) es_list;
    struct timestamps_filter_s pcrtf;
    bool b_discontinuity;
    es_out_t es_out;
};

static void timestamps_filter_init(struct timestamps_filter_s *tf)
{
    mva_init(&tf->mva);
    tf->sequence_offset = 0;
    tf->contiguous_last = 0;
    tf->sequence = -1;
}

static void timestamps_filter_push(const char *s, struct timestamps_filter_s *tf,
                                   vlc_tick_t i_dts, vlc_tick_t i_length,
                                   bool b_discontinuity, bool b_contiguous)
{
    if(i_dts == 0 && i_length == 0)
        return;

    struct mva_packet_s *prev = mva_getLastPacket(&tf->mva);
    if (prev)
    {
        if(prev->dts == i_dts)
            return; /* duplicate packet */

        if(b_contiguous)
        {
            const int64_t i_maxdiff = tf->mva.i_packet > MVA_PACKETS ? mva_get(&tf->mva) * 2 : CLOCK_FREQ;
            if(llabs(i_dts - prev->dts) > i_maxdiff || b_discontinuity) /* Desync */
            {
                prev->diff = mva_get(&tf->mva);
                tf->sequence_offset = tf->contiguous_last - i_dts + prev->diff;
#ifdef DEBUG_TIMESTAMPS_FILTER
                printf("%4.4s found offset of %ld\n", s, (prev->dts - i_dts));
#endif
            }
            else prev->diff = i_dts - prev->dts;
        }

#ifdef DEBUG_TIMESTAMPS_FILTER
        vlc_tick_t next = prev->dts + mva_get(&tf->mva);

        printf("%4.4s expected %ld / %ld , prev %ld+%ld error %ld comp %ld\n",
               s, next, i_dts, prev->dts, mva_get(&tf->mva),
               b_contiguous ? llabs(i_dts - next): 0, i_dts + tf->sequence_offset);
#else
        VLC_UNUSED(s);
#endif
    }

    tf->contiguous_last = i_dts + tf->sequence_offset;

    mva_add(&tf->mva, i_dts, i_length);
}

static void timestamps_filter_es_out_Reset(struct tf_es_out_s *out)
{
    for(int i=0; i<out->es_list.i_size; i++)
    {
        struct tf_es_out_id_s *cur = (struct tf_es_out_id_s *)out->es_list.p_elems[i];
        timestamps_filter_init(&cur->tf);
    }
    timestamps_filter_init(&out->pcrtf);
    out->b_discontinuity = false;
}

static int timestamps_filter_es_out_Control(es_out_t *out, int i_query, va_list va_list)
{
    struct tf_es_out_s *p_sys = container_of(out, struct tf_es_out_s, es_out);
    switch(i_query)
    {
        case ES_OUT_SET_PCR:
        case ES_OUT_SET_GROUP_PCR:
        {
            int i_group;
            if(i_query == ES_OUT_SET_GROUP_PCR)
                i_group = va_arg(va_list, int);
            else
                i_group = 0;
            int64_t pcr = va_arg(va_list, int64_t);

            timestamps_filter_push("PCR ", &p_sys->pcrtf, pcr, 0, p_sys->b_discontinuity, true);

            pcr += p_sys->pcrtf.sequence_offset;

            if(i_query == ES_OUT_SET_GROUP_PCR)
                return es_out_Control(p_sys->original_es_out, i_query, i_group, pcr);
            else
                return es_out_SetPCR(p_sys->original_es_out, pcr);
        }
            break;
        case ES_OUT_RESET_PCR:
        {
            timestamps_filter_es_out_Reset(p_sys);
            break;
        }
        /* Private controls, never forwarded */
        case ES_OUT_TF_FILTER_GET_TIME:
        {
            *(va_arg(va_list, int64_t*)) = p_sys->pcrtf.contiguous_last;
            return VLC_SUCCESS;
        }
        case ES_OUT_TF_FILTER_DISCONTINUITY:
        {
            p_sys->b_discontinuity = true;
            return VLC_SUCCESS;
        }
        case ES_OUT_TF_FILTER_RESET:
        {
            timestamps_filter_es_out_Reset(p_sys);
            return VLC_SUCCESS;
        }
        /* !Private controls */
        default:
            break;
    }
    return es_out_vaControl(p_sys->original_es_out, i_query, va_list);
}

static struct tf_es_out_id_s * timestamps_filter_es_out_getID(struct tf_es_out_s *p_sys, es_out_id_t *id)
{
    for(int i=0; i<p_sys->es_list.i_size; i++)
    {
        struct tf_es_out_id_s *cur = (struct tf_es_out_id_s *)p_sys->es_list.p_elems[i];
        if(cur->id != id)
            continue;
        return cur;
    }
    return NULL;
}

static int timestamps_filter_es_out_Send(es_out_t *out, es_out_id_t *id, block_t *p_block)
{
    struct tf_es_out_s *p_sys = container_of(out, struct tf_es_out_s, es_out);
    struct tf_es_out_id_s *cur = timestamps_filter_es_out_getID(p_sys, id);

    timestamps_filter_push((char*)&cur->fourcc, &cur->tf,
                            p_block->i_dts, p_block->i_length,
                           p_sys->b_discontinuity, cur->contiguous);

    /* Record diff with last PCR */
    if(p_sys->pcrtf.mva.i_packet > 0 &&
        p_sys->pcrtf.mva.i_packet != cur->pcrpacket)
    {
        cur->pcrpacket = p_sys->pcrtf.mva.i_packet;
        cur->pcrdiff = mva_getLastDTS(&cur->tf.mva) - mva_getLastDTS(&p_sys->pcrtf.mva);

        vlc_tick_t i_offsetdiff = cur->tf.sequence_offset - p_sys->pcrtf.sequence_offset;
        if(i_offsetdiff != 0)
        {
            cur->tf.sequence_offset -= i_offsetdiff;
#ifdef DEBUG_TIMESTAMPS_FILTER
            printf("PCR diff %ld %ld **********\n", cur->pcrdiff, i_offsetdiff);
#endif
        }
    }

    /* Fix timestamps */
    if(p_block->i_dts)
        p_block->i_dts += cur->tf.sequence_offset;
    if(p_block->i_pts)
        p_block->i_pts += cur->tf.sequence_offset;

    return es_out_Send(p_sys->original_es_out, id, p_block);
}

static void timestamps_filter_es_out_Delete(es_out_t *out)
{
    struct tf_es_out_s *p_sys = container_of(out, struct tf_es_out_s, es_out);
    for(int i=0; i<p_sys->es_list.i_size; i++)
        free(p_sys->es_list.p_elems[i]);
    ARRAY_RESET(p_sys->es_list);
    free(p_sys);
}

static es_out_id_t *timestamps_filter_es_out_Add(es_out_t *out, const es_format_t *fmt)
{
    struct tf_es_out_s *p_sys = container_of(out, struct tf_es_out_s, es_out);

    struct tf_es_out_id_s *tf_es_sys = malloc(sizeof(*tf_es_sys));
    if(!tf_es_sys)
        return NULL;
    timestamps_filter_init(&tf_es_sys->tf);
    tf_es_sys->fourcc = fmt->i_codec;
    tf_es_sys->pcrdiff = 0;
    tf_es_sys->pcrpacket = -1;
    tf_es_sys->contiguous = (fmt->i_cat == VIDEO_ES || fmt->i_cat == AUDIO_ES);

    tf_es_sys->id = es_out_Add(p_sys->original_es_out, fmt);
    if(!tf_es_sys->id)
    {
        free(tf_es_sys);
        return NULL;
    }

    ARRAY_APPEND(p_sys->es_list, tf_es_sys);

    return tf_es_sys->id;
}

static void timestamps_filter_es_out_Del(es_out_t *out, es_out_id_t *id)
{
    struct tf_es_out_s *p_sys = container_of(out, struct tf_es_out_s, es_out);

    es_out_Del(p_sys->original_es_out, id);

    for(int i=0; i<p_sys->es_list.i_size; i++)
    {
        struct tf_es_out_id_s *cur = (struct tf_es_out_id_s *)p_sys->es_list.p_elems[i];
        if(cur->id != id)
            continue;
        free(cur);
        ARRAY_REMOVE(p_sys->es_list, i);
        break;
    }
}

static const struct es_out_callbacks timestamps_filter_es_out_cbs =
{
    timestamps_filter_es_out_Add,
    timestamps_filter_es_out_Send,
    timestamps_filter_es_out_Del,
    timestamps_filter_es_out_Control,
    timestamps_filter_es_out_Delete,
};

static es_out_t * timestamps_filter_es_out_New(es_out_t *orig)
{
    struct tf_es_out_s *tf = malloc(sizeof(*tf));
    if(!tf)
    {
        free(tf);
        return NULL;
    }
    tf->original_es_out = orig;
    tf->b_discontinuity = false;
    timestamps_filter_init(&tf->pcrtf);
    ARRAY_INIT(tf->es_list);

    tf->es_out.cbs = &timestamps_filter_es_out_cbs;

    return &tf->es_out;
}

#endif