////////////////////////////////////////////////////////////////////////////
//                           **** DSDPACK ****                            //
//         Lossless DSD (Direct Stream Digital) Audio Compressor          //
//                Copyright (c) 2013 - 2016 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// unpack_dsd.c

// This module actually handles the uncompression of the DSD audio data.

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "wavpack_local.h"

///////////////////////////// executable code ////////////////////////////////

// This function initialzes the main range-encoded data for DSD audio samples

static int init_dsd_block_fast (WavpackStream *wps, WavpackMetadata *wpmd);
static int init_dsd_block_high (WavpackStream *wps, WavpackMetadata *wpmd);
static int decode_fast (WavpackStream *wps, int32_t *output, int sample_count);
static int decode_high (WavpackStream *wps, int32_t *output, int sample_count);

int init_dsd_block (WavpackContext *wpc, WavpackMetadata *wpmd)
{
    WavpackStream *wps = wpc->streams [wpc->current_stream];
    unsigned char dsd_power;

    if (wpmd->byte_length < 2)
        return FALSE;

    wps->dsd.byteptr = wpmd->data;
    wps->dsd.endptr = wps->dsd.byteptr + wpmd->byte_length;

    dsd_power = *wps->dsd.byteptr++;
    wpc->dsd_multiplier = 1 << dsd_power;

    wps->dsd.mode = *wps->dsd.byteptr++;

    if (!wps->dsd.mode) {
        if (wps->dsd.endptr - wps->dsd.byteptr != wps->wphdr.block_samples * (wps->wphdr.flags & MONO_DATA ? 1 : 2)) {
            return FALSE;
        }

        wps->dsd.ready = 1;
        return TRUE;
    }

    if (wps->dsd.mode == 1)
        return init_dsd_block_fast (wps, wpmd);
    else if (wps->dsd.mode == 2)
        return init_dsd_block_high (wps, wpmd);
    else
        return FALSE;
}

int32_t unpack_dsd_samples (WavpackContext *wpc, int32_t *buffer, uint32_t sample_count)
{
    WavpackStream *wps = wpc->streams [wpc->current_stream];
    uint32_t flags = wps->wphdr.flags, crc = wps->crc;
    int bytes_to_copy;

    // don't attempt to decode past the end of the block, but watch out for overflow!

    if (wps->sample_index + sample_count > GET_BLOCK_INDEX (wps->wphdr) + wps->wphdr.block_samples &&
        GET_BLOCK_INDEX (wps->wphdr) + wps->wphdr.block_samples - wps->sample_index < sample_count)
            sample_count = (uint32_t) (GET_BLOCK_INDEX (wps->wphdr) + wps->wphdr.block_samples - wps->sample_index);

    if (GET_BLOCK_INDEX (wps->wphdr) > wps->sample_index || wps->wphdr.block_samples < sample_count)
        wps->mute_error = TRUE;

    if (wps->mute_error) {
        if (wpc->reduced_channels == 1 || wpc->config.num_channels == 1 || (flags & MONO_FLAG))
            memset (buffer, 0, sample_count * 4);   // TODO: DSD mute should be 0x55, not zero
        else
            memset (buffer, 0, sample_count * 8);

        wps->sample_index += sample_count;
        return sample_count;
    }

    if (!wps->dsd.mode) {
        int total_samples = sample_count * ((flags & MONO_DATA) ? 1 : 2);
        int32_t *bptr = buffer;

        if (wps->dsd.endptr - wps->dsd.byteptr < total_samples)
            total_samples = wps->dsd.endptr - wps->dsd.byteptr;

        while (total_samples--)
            wps->crc += (wps->crc << 1) + (*bptr++ = *wps->dsd.byteptr++);
    }
    else if (wps->dsd.mode == 1)
        decode_fast (wps, buffer, sample_count);
    else
        decode_high (wps, buffer, sample_count);

    if (flags & FALSE_STEREO) {
        int32_t *dptr = buffer + sample_count * 2;
        int32_t *sptr = buffer + sample_count;
        int32_t c = sample_count;

        while (c--) {
            *--dptr = *--sptr;
            *--dptr = *sptr;
        }
    }

    wps->sample_index += sample_count;

    return sample_count;
}

/*------------------------------------------------------------------------------------------------------------------------*/

// #define DSD_BYTE_READY(low,high) (((low) >> 24) == ((high) >> 24))
// #define DSD_BYTE_READY(low,high) (!(((low) ^ (high)) >> 24))
#define DSD_BYTE_READY(low,high) (!(((low) ^ (high)) & 0xff000000))
#define MAX_HISTORY_BITS    5

static int init_dsd_block_fast (WavpackStream *wps, WavpackMetadata *wpmd)
{
    unsigned char dsd_power, history_bits, max_probability;
    int total_summed_probabilities = 0, i;

    if (wps->dsd.byteptr == wps->dsd.endptr)
        return FALSE;

    history_bits = *wps->dsd.byteptr++;

    if (wps->dsd.byteptr == wps->dsd.endptr || history_bits > MAX_HISTORY_BITS)
        return FALSE;

    wps->dsd.history_bins = 1 << history_bits;

    wps->dsd.value_lookup = malloc (sizeof (*wps->dsd.value_lookup) * wps->dsd.history_bins);
    memset (wps->dsd.value_lookup, 0, sizeof (*wps->dsd.value_lookup) * wps->dsd.history_bins);
    wps->dsd.summed_probabilities = malloc (sizeof (*wps->dsd.summed_probabilities) * wps->dsd.history_bins);
    wps->dsd.probabilities = malloc (sizeof (*wps->dsd.probabilities) * wps->dsd.history_bins);

    max_probability = *wps->dsd.byteptr++;

    if (max_probability < 0xff) {
        unsigned char *outptr = (unsigned char *) wps->dsd.probabilities;
        unsigned char *outend = outptr + sizeof (*wps->dsd.probabilities) * wps->dsd.history_bins;

        while (outptr < outend && wps->dsd.byteptr < wps->dsd.endptr) {
            int code = *wps->dsd.byteptr++;

            if (code > max_probability) {
                int zcount = code - max_probability;

                while (outptr < outend && zcount--)
                    *outptr++ = 0;
            }
            else if (code)
                *outptr++ = code;
            else
                break;
        }

        if (outptr < outend || (wps->dsd.byteptr < wps->dsd.endptr && *wps->dsd.byteptr++))
            return FALSE;
    }
    else if (wps->dsd.endptr - wps->dsd.byteptr > sizeof (*wps->dsd.probabilities) * wps->dsd.history_bins) {
        memcpy (wps->dsd.probabilities, wps->dsd.byteptr, sizeof (*wps->dsd.probabilities) * wps->dsd.history_bins);
        wps->dsd.byteptr += sizeof (*wps->dsd.probabilities) * wps->dsd.history_bins;
    }
    else
        return FALSE;

    for (wps->dsd.p0 = 0; wps->dsd.p0 < wps->dsd.history_bins; ++wps->dsd.p0) {
        int32_t sum_values;
        unsigned char *vp;

        for (sum_values = i = 0; i < 256; ++i)
            wps->dsd.summed_probabilities [wps->dsd.p0] [i] = sum_values += wps->dsd.probabilities [wps->dsd.p0] [i];

        if (sum_values) {
            total_summed_probabilities += sum_values;
            vp = wps->dsd.value_lookup [wps->dsd.p0] = malloc (sum_values);

            for (i = 0; i < 256; i++) {
                int c = wps->dsd.probabilities [wps->dsd.p0] [i];

                while (c--)
                    *vp++ = i;
            }
        }
    }

    if (wps->dsd.endptr - wps->dsd.byteptr < 4 || total_summed_probabilities > wps->dsd.history_bins * 1280)
        return FALSE;

    for (i = 4; i--;)
        wps->dsd.value = (wps->dsd.value << 8) | *wps->dsd.byteptr++;

    wps->dsd.p0 = wps->dsd.p1 = 0;
    wps->dsd.low = 0; wps->dsd.high = 0xffffffff;
    wps->dsd.ready = 1;

    return TRUE;
}

static int decode_fast (WavpackStream *wps, int32_t *output, int sample_count)
{
    int total_samples = sample_count;

    if (!(wps->wphdr.flags & MONO_DATA))
        total_samples *= 2;

    while (total_samples--) {
        int mult, index, code, i;

        if (!wps->dsd.summed_probabilities [wps->dsd.p0] [255])
            return 0;

        mult = (wps->dsd.high - wps->dsd.low) / wps->dsd.summed_probabilities [wps->dsd.p0] [255];

        if (!mult) {
            if (wps->dsd.endptr - wps->dsd.byteptr >= 4)
                for (i = 4; i--;)
                    wps->dsd.value = (wps->dsd.value << 8) | *wps->dsd.byteptr++;

            wps->dsd.low = 0;
            wps->dsd.high = 0xffffffff;
            mult = wps->dsd.high / wps->dsd.summed_probabilities [wps->dsd.p0] [255];

            if (!mult)
                return 0;
        }

        index = (wps->dsd.value - wps->dsd.low) / mult;

        if (index >= wps->dsd.summed_probabilities [wps->dsd.p0] [255])
            return 0;

        if (*output++ = code = wps->dsd.value_lookup [wps->dsd.p0] [index])
            wps->dsd.low += wps->dsd.summed_probabilities [wps->dsd.p0] [code-1] * mult;

        wps->dsd.high = wps->dsd.low + wps->dsd.probabilities [wps->dsd.p0] [code] * mult - 1;
        wps->crc += (wps->crc << 1) + code;

        if (wps->wphdr.flags & MONO_DATA)
            wps->dsd.p0 = code & (wps->dsd.history_bins-1);
        else {
            wps->dsd.p0 = wps->dsd.p1;
            wps->dsd.p1 = code & (wps->dsd.history_bins-1);
        }

        while (DSD_BYTE_READY (wps->dsd.high, wps->dsd.low) && wps->dsd.byteptr < wps->dsd.endptr) {
            wps->dsd.value = (wps->dsd.value << 8) | *wps->dsd.byteptr++;
            wps->dsd.high = (wps->dsd.high << 8) | 0xff;
            wps->dsd.low <<= 8;
        }
    }

    return sample_count;
}

/*------------------------------------------------------------------------------------------------------------------------*/

#define PTABLE_BITS 8
#define PTABLE_BINS (1<<PTABLE_BITS)
#define PTABLE_MASK (PTABLE_BINS-1)

#define UP   0x010000fe
#define DOWN 0x00010000
#define DECAY 8

#define PRECISION 24
#define VALUE_ONE (1 << PRECISION)
#define PRECISION_USE 12

#define RATE_S 20

static void init_ptable (int *table, int rate_i, int rate_s)
{
    int value = 0x808000, rate = rate_i << 8, c, i;

    for (c = (rate + 128) >> 8; c--;)
        value += (DOWN - value) >> DECAY;

    for (i = 0; i < PTABLE_BINS/2; ++i) {
        table [i] = value;
        table [PTABLE_BINS-1-i] = 0x100ffff - value;

        if (value > 0x010000) {
            rate += (rate * rate_s + 128) >> 8;

            for (c = (rate + 64) >> 7; c--;)
                value += (DOWN - value) >> DECAY;
        }
    }
}

static int init_dsd_block_high (WavpackStream *wps, WavpackMetadata *wpmd)
{
    uint32_t flags = wps->wphdr.flags;
    int channel, rate_i, rate_s, i;

    if (wps->dsd.endptr - wps->dsd.byteptr < ((flags & MONO_DATA) ? 13 : 20))
        return FALSE;

    rate_i = *wps->dsd.byteptr++;
    rate_s = *wps->dsd.byteptr++;

    if (rate_s != RATE_S)
        return FALSE;

    wps->dsd.ptable = malloc (PTABLE_BINS * sizeof (*wps->dsd.ptable));
    init_ptable (wps->dsd.ptable, rate_i, rate_s);

    for (channel = 0; channel < ((flags & MONO_DATA) ? 1 : 2); ++channel) {
        DSDfilters *sp = wps->dsd.filters + channel;

        sp->filter1 = *wps->dsd.byteptr++ << 16;
        sp->filter2 = *wps->dsd.byteptr++ << 16;
        sp->filter3 = *wps->dsd.byteptr++ << 16;
        sp->filter4 = *wps->dsd.byteptr++ << 16;
        sp->filter5 = *wps->dsd.byteptr++ << 16;
        sp->filter6 = 0;
        sp->factor = *wps->dsd.byteptr++ & 0xff;
        sp->factor |= (*wps->dsd.byteptr++ << 8) & 0xff00;
        sp->factor = (sp->factor << 16) >> 16;
    }

    wps->dsd.high = 0xffffffff;
    wps->dsd.low = 0x0;

    for (i = 4; i--;)
        wps->dsd.value = (wps->dsd.value << 8) | *wps->dsd.byteptr++;

    wps->dsd.ready = 1;

    return TRUE;
}

static int decode_high (WavpackStream *wps, int32_t *output, int sample_count)
{
    int total_samples = sample_count, channel = 0;

    if (!(wps->wphdr.flags & MONO_DATA))
        total_samples *= 2;

    while (total_samples--) {
        DSDfilters *sp = wps->dsd.filters + channel;
        int byte = 0, bitcount = 8;

        while (bitcount--) {
            int value = sp->filter1 - sp->filter5 + sp->filter6 * (sp->factor >> 2);
            int index = (value >> (PRECISION - PRECISION_USE)) & PTABLE_MASK;
            unsigned int range = wps->dsd.high - wps->dsd.low, split;
            int *val = wps->dsd.ptable + index;

            split = wps->dsd.low + ((range & 0xff000000) ? (range >> 8) * (*val >> 16) : ((range * (*val >> 16)) >> 8));
            value += sp->filter6 << 3;

            if (wps->dsd.value <= split) {
                wps->dsd.high = split;
                byte = (byte << 1) | 1;
                *val += (UP - *val) >> DECAY;
                sp->filter1 += (VALUE_ONE - sp->filter1) >> 6;
                sp->filter2 += (VALUE_ONE - sp->filter2) >> 4;

                if ((value ^ (value - (sp->filter6 << 4))) < 0)
                    sp->factor -= (value >> 31) | 1;
            }
            else {
                wps->dsd.low = split + 1;
                byte <<= 1;
                *val += (DOWN - *val) >> DECAY;
                sp->filter1 -= sp->filter1 >> 6;
                sp->filter2 -= sp->filter2 >> 4;

                if ((value ^ (value - (sp->filter6 << 4))) < 0)
                    sp->factor += (value >> 31) | 1;
            }

            while (DSD_BYTE_READY (wps->dsd.high, wps->dsd.low) && wps->dsd.byteptr < wps->dsd.endptr) {
                wps->dsd.value = (wps->dsd.value << 8) | *wps->dsd.byteptr++;
                wps->dsd.high = (wps->dsd.high << 8) | 0xff;
                wps->dsd.low <<= 8;
            }

            sp->filter3 += (sp->filter2 - sp->filter3) >> 4;
            sp->filter4 += (sp->filter3 - sp->filter4) >> 4;
            sp->filter5 += value = (sp->filter4 - sp->filter5) >> 4;
            sp->filter6 += (value - sp->filter6) >> 3;
        }

        wps->crc += (wps->crc << 1) + (*output++ = byte);

        if (!(wps->wphdr.flags & MONO_DATA))
            channel ^= 1;
    }

    return sample_count;
}
