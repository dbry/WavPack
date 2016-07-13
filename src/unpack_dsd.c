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

#define MAX_HISTORY_BITS    5

///////////////////////////// executable code ////////////////////////////////

// This function initialzes the main range-encoded data for DSD audio samples

int init_dsd_block (WavpackContext *wpc, WavpackMetadata *wpmd)
{
    WavpackStream *wps = wpc->streams [wpc->current_stream];
    unsigned char dsd_power, history_bits, max_probability;
    int i;

    if (wpmd->byte_length < 2)
        return FALSE;

    wps->dsd.byteptr = wpmd->data;
    wps->dsd.endptr = wps->dsd.byteptr + wpmd->byte_length;

    dsd_power = *wps->dsd.byteptr++;
    for (wpc->dsd_multiplier = 1; dsd_power--; wpc->dsd_multiplier <<= 1);

    wps->dsd.dsd_mode = *wps->dsd.byteptr++;

    if (!wps->dsd.dsd_mode) {
        if (wps->dsd.endptr - wps->dsd.byteptr != wps->wphdr.block_samples * (wps->wphdr.flags & MONO_FLAG ? 1 : 2))
            return FALSE;

        wps->dsd.ready = 1;
        return TRUE;
    }

    if (wps->dsd.byteptr == wps->dsd.endptr)
        return FALSE;

    history_bits = *wps->dsd.byteptr++;

    if (wps->dsd.byteptr == wps->dsd.endptr || history_bits > MAX_HISTORY_BITS)
        return FALSE;

    wps->dsd.history_bins = 1 << history_bits;

    if (!wps->dsd.allocated_bins || wps->dsd.allocated_bins < wps->dsd.history_bins) {
        wps->dsd.value_lookup = realloc (wps->dsd.value_lookup, sizeof (*wps->dsd.value_lookup) * wps->dsd.history_bins);
        wps->dsd.summed_probabilities = realloc (wps->dsd.summed_probabilities, sizeof (*wps->dsd.summed_probabilities) * wps->dsd.history_bins);
        wps->dsd.probabilities = realloc (wps->dsd.probabilities, sizeof (*wps->dsd.probabilities) * wps->dsd.history_bins);
        wps->dsd.allocated_bins = wps->dsd.history_bins;
    }

    max_probability = *wps->dsd.byteptr++;

    if (max_probability < 0xff) {
        unsigned char *outp = (unsigned char *) wps->dsd.probabilities;

        while (wps->dsd.byteptr != wps->dsd.endptr) {
            if (*wps->dsd.byteptr > max_probability) {
                int zcount = *wps->dsd.byteptr++ - max_probability;

                while (zcount--)
                    *outp++ = 0;
            }
            else if (*wps->dsd.byteptr)
                *outp++ = *wps->dsd.byteptr++;
            else {
                wps->dsd.byteptr++;
                break;
            }
        }

        if (outp != (unsigned char *) wps->dsd.probabilities + sizeof (*wps->dsd.probabilities) * wps->dsd.history_bins) {
            fprintf (stderr, "inexact fatal decoding error, length = %d, written = %d, read = %d!\n",
                wpmd->byte_length, (int)(outp - (unsigned char *) wps->dsd.probabilities), (int)(wps->dsd.byteptr - (unsigned char *) wpmd->data));
            return FALSE;
        }
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

        vp = wps->dsd.value_lookup [wps->dsd.p0] = malloc (sum_values);

        for (i = 0; i < 256; i++) {
            int c = wps->dsd.probabilities [wps->dsd.p0] [i];

            while (c--)
                *vp++ = i;
        }
    }

    if (wps->dsd.endptr - wps->dsd.byteptr < 4)
        return FALSE;

    for (i = 4; i--;)
        wps->dsd.value = (wps->dsd.value << 8) | *wps->dsd.byteptr++;

    wps->dsd.p0 = wps->dsd.p1 = 0;
    wps->dsd.low = 0; wps->dsd.high = 0xffffffff;
    wps->dsd.ready = 1;

    return TRUE;
}

static int decode_fast (WavpackStream *wps, int32_t *output, int sample_count);

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
            memset (buffer, 0, sample_count * 4);
        else
            memset (buffer, 0, sample_count * 8);

        wps->sample_index += sample_count;
        return sample_count;
    }

    decode_fast (wps, buffer, sample_count);

    wps->sample_index += sample_count;
    wps->crc = wps->wphdr.crc;          // TODO: no cheating!!

    return sample_count;
}

/*------------------------------------------------------------------------------------------------------------------------*/

static int decode_fast (WavpackStream *wps, int32_t *output, int sample_count)
{
    int total_samples = sample_count;

    if (!(wps->wphdr.flags & MONO_FLAG))
        total_samples *= 2;

    if (!wps->dsd.dsd_mode) {
        if (wps->dsd.endptr - wps->dsd.byteptr < total_samples) {
            total_samples = wps->dsd.endptr - wps->dsd.byteptr;

            if (wps->wphdr.flags & MONO_FLAG)
                sample_count = total_samples;
            else {
                total_samples &= ~1;
                sample_count = total_samples >> 1;
            }
        }

        while (total_samples--)
            *output++ = *wps->dsd.byteptr++;

        return sample_count;
    }

    while (total_samples--) {
        int mult = (wps->dsd.high - wps->dsd.low) / wps->dsd.summed_probabilities [wps->dsd.p0] [255], i;

        if (!mult) {
            if (wps->dsd.endptr - wps->dsd.byteptr >= 4)
                for (i = 4; i--;)
                    wps->dsd.value = (wps->dsd.value << 8) | *wps->dsd.byteptr++;

            wps->dsd.low = 0;
            wps->dsd.high = 0xffffffff;
            mult = wps->dsd.high / wps->dsd.summed_probabilities [wps->dsd.p0] [255];
        }

        if (*output++ = i = wps->dsd.value_lookup [wps->dsd.p0] [(wps->dsd.value - wps->dsd.low) / mult])
            wps->dsd.low += wps->dsd.summed_probabilities [wps->dsd.p0] [i-1] * mult;

        wps->dsd.high = wps->dsd.low + wps->dsd.probabilities [wps->dsd.p0] [i] * mult - 1;

        if (wps->wphdr.flags & MONO_FLAG)
            wps->dsd.p0 = i & (wps->dsd.history_bins-1);
        else {
            wps->dsd.p0 = wps->dsd.p1;
            wps->dsd.p1 = i & (wps->dsd.history_bins-1);
        }

        while ((wps->dsd.high >> 24) == (wps->dsd.low >> 24) && wps->dsd.byteptr < wps->dsd.endptr) {
            wps->dsd.value = (wps->dsd.value << 8) | *wps->dsd.byteptr++;
            wps->dsd.high = (wps->dsd.high << 8) | 0xff;
            wps->dsd.low <<= 8;
        }
    }

    return sample_count;
}
