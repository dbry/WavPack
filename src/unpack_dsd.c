////////////////////////////////////////////////////////////////////////////
//                           **** DSDPACK ****                            //
//         Lossless DSD (Direct Stream Digital) Audio Compressor          //
//                Copyright (c) 2013 - 2024 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// unpack_dsd.c

// This module actually handles the uncompression of the DSD audio data.

#ifdef ENABLE_DSD

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "wavpack_local.h"

///////////////////////////// executable code ////////////////////////////////

// This function initializes the main range-encoded data for DSD audio samples

static int init_dsd_block_fast (WavpackStream *wps, WavpackMetadata *wpmd);
static int init_dsd_block_high (WavpackStream *wps, WavpackMetadata *wpmd);
static int decode_fast (WavpackStream *wps, int32_t *output, int sample_count);
static int decode_high (WavpackStream *wps, int32_t *output, int sample_count);

int init_dsd_block (WavpackStream *wps, WavpackMetadata *wpmd)
{
    if (wpmd->byte_length < 2)
        return FALSE;

    wps->dsd.byteptr = (unsigned char *)wpmd->data;
    wps->dsd.endptr = wps->dsd.byteptr + wpmd->byte_length;

    if (*wps->dsd.byteptr > 31)
        return FALSE;

    // safe to cast away const on stream 0 only
    if (!wps->stream_index)
        ((WavpackContext *)wps->wpc)->dsd_multiplier = 1U << *wps->dsd.byteptr++;
    else
        wps->dsd.byteptr++;

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
    else if (wps->dsd.mode == 3)
        return init_dsd_block_high (wps, wpmd);
    else
        return FALSE;
}

int32_t unpack_dsd_samples (WavpackStream *wps, int32_t *buffer, uint32_t sample_count)
{
    uint32_t flags = wps->wphdr.flags;

    // don't attempt to decode past the end of the block, but watch out for overflow!

    if (wps->sample_index + sample_count > GET_BLOCK_INDEX (wps->wphdr) + wps->wphdr.block_samples &&
        (uint32_t) (GET_BLOCK_INDEX (wps->wphdr) + wps->wphdr.block_samples - wps->sample_index) < sample_count)
            sample_count = (uint32_t) (GET_BLOCK_INDEX (wps->wphdr) + wps->wphdr.block_samples - wps->sample_index);

    if (GET_BLOCK_INDEX (wps->wphdr) > wps->sample_index || wps->wphdr.block_samples < sample_count)
        wps->mute_error = TRUE;

    if (!wps->mute_error) {
        if (!wps->dsd.mode) {
            int total_samples = sample_count * ((flags & MONO_DATA) ? 1 : 2);
            int32_t *bptr = buffer;

            if (wps->dsd.endptr - wps->dsd.byteptr < total_samples)
                total_samples = (int)(wps->dsd.endptr - wps->dsd.byteptr);

            while (total_samples--)
                wps->crc += (wps->crc << 1) + (*bptr++ = *wps->dsd.byteptr++);
        }
        else if (wps->dsd.mode == 1) {
            if (!decode_fast (wps, buffer, sample_count))
                wps->mute_error = TRUE;
        }
        else if (!decode_high (wps, buffer, sample_count))
            wps->mute_error = TRUE;

        // If we just finished this block, then it's time to check if the applicable checksum matches. Like
        // other decoding errors, this is indicated by setting the mute_error flag.

        if (wps->sample_index + sample_count == GET_BLOCK_INDEX (wps->wphdr) + wps->wphdr.block_samples &&
            !wps->mute_error && wps->crc != wps->wphdr.crc)
                wps->mute_error = TRUE;
    }

    if (wps->mute_error) {
        int samples_to_null;
        if (wps->wpc->reduced_channels == 1 || wps->wpc->config.num_channels == 1 || (flags & MONO_FLAG))
            samples_to_null = sample_count;
        else
            samples_to_null = sample_count * 2;

        while (samples_to_null--)
            *buffer++ = 0x55;

        wps->sample_index += sample_count;
        return sample_count;
    }

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

static int init_dsd_block_fast (WavpackStream *wps, WavpackMetadata *wpmd)
{
    unsigned char history_bits, max_probability, *lb_ptr;
    int total_summed_probabilities = 0, bi, i;

    (void) wpmd;

    if (wps->dsd.byteptr == wps->dsd.endptr)
        return FALSE;

    history_bits = *wps->dsd.byteptr++;

    if (wps->dsd.byteptr == wps->dsd.endptr || history_bits > MAX_HISTORY_BITS)
        return FALSE;

    wps->dsd.history_bins = 1 << history_bits;

    free_dsd_tables (wps);
    lb_ptr = wps->dsd.lookup_buffer = (unsigned char *)malloc (wps->dsd.history_bins * MAX_BYTES_PER_BIN);
    wps->dsd.value_lookup = (unsigned char **)malloc (sizeof (*wps->dsd.value_lookup) * wps->dsd.history_bins);
    memset (wps->dsd.value_lookup, 0, sizeof (*wps->dsd.value_lookup) * wps->dsd.history_bins);
    wps->dsd.summed_probabilities = (uint16_t (*)[256])malloc (sizeof (*wps->dsd.summed_probabilities) * wps->dsd.history_bins);
    wps->dsd.probabilities = (unsigned char (*)[256])malloc (sizeof (*wps->dsd.probabilities) * wps->dsd.history_bins);

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
                *outptr++ = (unsigned char)code;
            else
                break;
        }

        if (outptr < outend || (wps->dsd.byteptr < wps->dsd.endptr && *wps->dsd.byteptr++))
            return FALSE;
    }
    else if (wps->dsd.endptr - wps->dsd.byteptr > (int) sizeof (*wps->dsd.probabilities) * wps->dsd.history_bins) {
        memcpy (wps->dsd.probabilities, wps->dsd.byteptr, sizeof (*wps->dsd.probabilities) * wps->dsd.history_bins);
        wps->dsd.byteptr += sizeof (*wps->dsd.probabilities) * wps->dsd.history_bins;
    }
    else
        return FALSE;

    for (bi = 0; bi < wps->dsd.history_bins; ++bi) {
        int32_t sum_values;

        for (sum_values = i = 0; i < 256; ++i)
            wps->dsd.summed_probabilities [bi] [i] = (uint16_t)(sum_values += wps->dsd.probabilities [bi] [i]);

        if (sum_values) {
            if ((total_summed_probabilities += sum_values) > wps->dsd.history_bins * MAX_BYTES_PER_BIN)
                return FALSE;

            wps->dsd.value_lookup [bi] = lb_ptr;

            for (i = 0; i < 256; i++) {
                int c = wps->dsd.probabilities [bi] [i];

                while (c--)
                    *lb_ptr++ = (unsigned char)i;
            }
        }
    }

    if (wps->dsd.endptr - wps->dsd.byteptr < 4 || total_summed_probabilities > wps->dsd.history_bins * MAX_BYTES_PER_BIN)
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
        unsigned int mult, index, code, i;

        if (!wps->dsd.summed_probabilities [wps->dsd.p0] [255])
            return 0;

        mult = (wps->dsd.high - wps->dsd.low) / wps->dsd.summed_probabilities [wps->dsd.p0] [255];

        if (!mult) {
            if (wps->dsd.endptr - wps->dsd.byteptr >= 4)
                for (i = 4; i--;)
                    wps->dsd.value = (wps->dsd.value << 8) | *wps->dsd.byteptr++;
            else
                return 0;

            wps->dsd.low = 0;
            wps->dsd.high = 0xffffffff;
            mult = wps->dsd.high / wps->dsd.summed_probabilities [wps->dsd.p0] [255];

            if (!mult)
                return 0;
        }

        index = (wps->dsd.value - wps->dsd.low) / mult;

        if (index >= wps->dsd.summed_probabilities [wps->dsd.p0] [255])
            return 0;

        if ((*output++ = code = wps->dsd.value_lookup [wps->dsd.p0] [index]) != 0)
            wps->dsd.low += wps->dsd.summed_probabilities [wps->dsd.p0] [code-1] * mult;

        wps->dsd.high = wps->dsd.low + wps->dsd.probabilities [wps->dsd.p0] [code] * mult - 1;
        wps->crc += (wps->crc << 1) + code;

        if (wps->wphdr.flags & MONO_DATA)
            wps->dsd.p0 = code & (wps->dsd.history_bins-1);
        else {
            wps->dsd.p0 = wps->dsd.p1;
            wps->dsd.p1 = code & (wps->dsd.history_bins-1);
        }

        while (DSD_BYTE_READY (wps->dsd.high, wps->dsd.low)) {
            if (wps->dsd.byteptr >= wps->dsd.endptr)
                return 0;

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

#define PRECISION 20
#define VALUE_ONE (1 << PRECISION)
#define PRECISION_USE 12

#define RATE_S 20

static void init_ptable (int32_t *table, int rate_i, int rate_s)
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

    (void) wpmd;

    if (wps->dsd.endptr - wps->dsd.byteptr < ((flags & MONO_DATA) ? 13 : 20))
        return FALSE;

    rate_i = *wps->dsd.byteptr++;
    rate_s = *wps->dsd.byteptr++;

    if (rate_s != RATE_S)
        return FALSE;

    if (!wps->dsd.ptable)
        wps->dsd.ptable = (int32_t *)malloc (PTABLE_BINS * sizeof (*wps->dsd.ptable));

    init_ptable (wps->dsd.ptable, rate_i, rate_s);

    for (channel = 0; channel < ((flags & MONO_DATA) ? 1 : 2); ++channel) {
        DSDfilters *sp = wps->dsd.filters + channel;

        sp->filter1 = *wps->dsd.byteptr++ << (PRECISION - 8);
        sp->filter2 = *wps->dsd.byteptr++ << (PRECISION - 8);
        sp->filter3 = *wps->dsd.byteptr++ << (PRECISION - 8);
        sp->filter4 = *wps->dsd.byteptr++ << (PRECISION - 8);
        sp->filter5 = *wps->dsd.byteptr++ << (PRECISION - 8);
        sp->filter6 = 0;
        sp->factor = *wps->dsd.byteptr++ & 0xff;
        sp->factor |= (*wps->dsd.byteptr++ << 8) & 0xff00;
        sp->factor = (int32_t)((uint32_t)sp->factor << 16) >> 16;
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
    int total_samples = sample_count, stereo = (wps->wphdr.flags & MONO_DATA) ? 0 : 1;
    DSDfilters *sp = wps->dsd.filters;

    while (total_samples--) {
        int bitcount = 8;

        sp [0].value = sp [0].filter1 - sp [0].filter5 + ((sp [0].filter6 * sp [0].factor) >> 2);

        if (stereo)
            sp [1].value = sp [1].filter1 - sp [1].filter5 + ((sp [1].filter6 * sp [1].factor) >> 2);

        while (bitcount--) {
            int32_t *pp = wps->dsd.ptable + ((sp [0].value >> (PRECISION - PRECISION_USE)) & PTABLE_MASK);
            uint32_t split = wps->dsd.low + ((wps->dsd.high - wps->dsd.low) >> 8) * (*pp >> 16);

            if (wps->dsd.value <= split) {
                wps->dsd.high = split;
                *pp += (UP - *pp) >> DECAY;
                sp [0].filter0 = -1;
            }
            else {
                wps->dsd.low = split + 1;
                *pp += (DOWN - *pp) >> DECAY;
                sp [0].filter0 = 0;
            }

            while (DSD_BYTE_READY (wps->dsd.high, wps->dsd.low)) {
                if (wps->dsd.byteptr >= wps->dsd.endptr)
                    return 0;

                wps->dsd.value = (wps->dsd.value << 8) | *wps->dsd.byteptr++;
                wps->dsd.high = (wps->dsd.high << 8) | 0xff;
                wps->dsd.low <<= 8;
            }

            sp [0].value += sp [0].filter6 * 8;
            sp [0].byte = (sp [0].byte << 1) | (sp [0].filter0 & 1);
            sp [0].factor += (((sp [0].value ^ sp [0].filter0) >> 31) | 1) & ((sp [0].value ^ (sp [0].value - (sp [0].filter6 * 16))) >> 31);
            sp [0].filter1 += ((sp [0].filter0 & VALUE_ONE) - sp [0].filter1) >> 6;
            sp [0].filter2 += ((sp [0].filter0 & VALUE_ONE) - sp [0].filter2) >> 4;
            sp [0].filter3 += (sp [0].filter2 - sp [0].filter3) >> 4;
            sp [0].filter4 += (sp [0].filter3 - sp [0].filter4) >> 4;
            sp [0].value = (sp [0].filter4 - sp [0].filter5) >> 4;
            sp [0].filter5 += sp [0].value;
            sp [0].filter6 += (sp [0].value - sp [0].filter6) >> 3;
            sp [0].value = sp [0].filter1 - sp [0].filter5 + ((sp [0].filter6 * sp [0].factor) >> 2);

            if (!stereo)
                continue;

            pp = wps->dsd.ptable + ((sp [1].value >> (PRECISION - PRECISION_USE)) & PTABLE_MASK);
            split = wps->dsd.low + ((wps->dsd.high - wps->dsd.low) >> 8) * (*pp >> 16);

            if (wps->dsd.value <= split) {
                wps->dsd.high = split;
                *pp += (UP - *pp) >> DECAY;
                sp [1].filter0 = -1;
            }
            else {
                wps->dsd.low = split + 1;
                *pp += (DOWN - *pp) >> DECAY;
                sp [1].filter0 = 0;
            }

            while (DSD_BYTE_READY (wps->dsd.high, wps->dsd.low)) {
                if (wps->dsd.byteptr >= wps->dsd.endptr)
                    return 0;

                wps->dsd.value = (wps->dsd.value << 8) | *wps->dsd.byteptr++;
                wps->dsd.high = (wps->dsd.high << 8) | 0xff;
                wps->dsd.low <<= 8;
            }

            sp [1].value += sp [1].filter6 * 8;
            sp [1].byte = (sp [1].byte << 1) | (sp [1].filter0 & 1);
            sp [1].factor += (((sp [1].value ^ sp [1].filter0) >> 31) | 1) & ((sp [1].value ^ (sp [1].value - (sp [1].filter6 * 16))) >> 31);
            sp [1].filter1 += ((sp [1].filter0 & VALUE_ONE) - sp [1].filter1) >> 6;
            sp [1].filter2 += ((sp [1].filter0 & VALUE_ONE) - sp [1].filter2) >> 4;
            sp [1].filter3 += (sp [1].filter2 - sp [1].filter3) >> 4;
            sp [1].filter4 += (sp [1].filter3 - sp [1].filter4) >> 4;
            sp [1].value = (sp [1].filter4 - sp [1].filter5) >> 4;
            sp [1].filter5 += sp [1].value;
            sp [1].filter6 += (sp [1].value - sp [1].filter6) >> 3;
            sp [1].value = sp [1].filter1 - sp [1].filter5 + ((sp [1].filter6 * sp [1].factor) >> 2);
        }

        wps->crc += (wps->crc << 1) + (*output++ = sp [0].byte & 0xff);
        sp [0].factor -= (sp [0].factor + 512) >> 10;

        if (stereo) {
            wps->crc += (wps->crc << 1) + (*output++ = wps->dsd.filters [1].byte & 0xff);
            wps->dsd.filters [1].factor -= (wps->dsd.filters [1].factor + 512) >> 10;
        }
    }

    return sample_count;
}

/*------------------------------------------------------------------------------------------------------------------------*/

#define NUM_FILTER_TERMS    104  // must be an odd multiple of eight, currently just 56 or 104

#if NUM_FILTER_TERMS == 56

// 56 term decimation filter (Gaussian)
// linear-phase, no negative terms, no ringing
// 0.4 dB down at 20 kHz
// 6.0 dB down at 75 kHz
// 36 dB down at 176 kHz (Nyquist)
// 120 dB stopband attenuation

static const int32_t decm_filter [] = {
    4, 17, 56, 147, 336, 692, 1315, 2337,
    3926, 6281, 9631, 14216, 20275, 28021, 37619, 49155,
    62616, 77870, 94649, 112551, 131049, 149507, 167220, 183448,
    197472, 208636, 216402, 220385, 220385, 216402, 208636, 197472,
    183448, 167220, 149507, 131049, 112551, 94649, 77870, 62616,
    49155, 37619, 28021, 20275, 14216, 9631, 6281, 3926,
    2337, 1315, 692, 336, 147, 56, 17, 4,
};

#endif

#if NUM_FILTER_TERMS == 104

// 104 term decimation filter (Sinc + Blackman-Harris, 80 kHz lowpass)
// linear-phase, negative terms, ringing possible
// 0.1 dB down at 20 kHz
// 6.0 dB down at 80 kHz
// 80 dB down at 176 kHz (Nyquist)
// 120 dB stopband attenuation

static const int32_t decm_filter [] = {
    1, 4, 13, 31, 63, 113, 184, 279,
    399, 538, 687, 829, 937, 975, 894, 639,
    147, -651, -1819, -3415, -5478, -8019, -11015, -14393,
    -18028, -21728, -25237, -28233, -30328, -31084, -30024, -26649,
    -20471, -11030, 2066, 19117, 40300, 65640, 94995, 128039,
    164259, 202961, 243285, 284233, 324702, 363526, 399531, 431580,
    458626, 479766, 494279, 501664, 501664, 494279, 479766, 458626,
    431580, 399531, 363526, 324702, 284233, 243285, 202961, 164259,
    128039, 94995, 65640, 40300, 19117, 2066, -11030, -20471,
    -26649, -30024, -31084, -30328, -28233, -25237, -21728, -18028,
    -14393, -11015, -8019, -5478, -3415, -1819, -651, 147,
    639, 894, 975, 937, 829, 687, 538, 399,
    279, 184, 113, 63, 31, 13, 4, 1
};

#endif

#define HISTORY_BYTES       (NUM_FILTER_TERMS / 8)      // required history bytes for DSD decimation
#define DELAY_SAMPLES       ((HISTORY_BYTES - 1) / 2)   // decimated DSD is delayed this amount

typedef struct {
    unsigned char delay [HISTORY_BYTES];
} DecimateDSDchannel;

typedef struct {
    int32_t conv_tables [HISTORY_BYTES] [256];
    int32_t filter_sums [DELAY_SAMPLES + 1];
    int flags, num_channels, reset;
    DecimateDSDchannel *chans;
    int64_t output_index;
} DecimateDSD;

void *decimate_dsd_init (int num_channels)
{
    DecimateDSD *cxt = (DecimateDSD *)malloc (sizeof (DecimateDSD));
    double filter_scale;
    int i, j;

    if (!cxt)
        return cxt;

    memset (cxt, 0, sizeof (*cxt));
    cxt->num_channels = num_channels;

    for (int start_byte = 0; start_byte <= DELAY_SAMPLES; ++start_byte)
        for (i = start_byte * 8; i < NUM_FILTER_TERMS - start_byte * 8; ++i)
            cxt->filter_sums [start_byte] += decm_filter [i];

    filter_scale = ((1 << 23) - 1) / (double) cxt->filter_sums [0] * 16.0;

    for (i = 0; i < NUM_FILTER_TERMS; ++i) {
        int scaled_term = (int) floor (decm_filter [i] * filter_scale + 0.5);

        if (scaled_term) {
            for (j = 0; j < 256; ++j)
                if (j & (0x80 >> (i & 0x7)))
                    cxt->conv_tables [i >> 3] [j] += scaled_term;
                else
                    cxt->conv_tables [i >> 3] [j] -= scaled_term;
        }
    }

    if (num_channels) {
        cxt->chans = (DecimateDSDchannel *)malloc (num_channels * sizeof (DecimateDSDchannel));

        if (!cxt->chans) {
            free (cxt);
            return NULL;
        }
    }

    decimate_dsd_reset (cxt);

    return cxt;
}

void decimate_dsd_reset (void *decimate_context)
{
    DecimateDSD *cxt = (DecimateDSD *) decimate_context;
    int chan = 0, i;

    if (!cxt)
        return;

    for (chan = 0; chan < cxt->num_channels; ++chan)
        for (i = 0; i < HISTORY_BYTES; ++i)
            cxt->chans [chan].delay [i] = 0x55;

    cxt->output_index = 0;
    cxt->reset = 1;
}

int decimate_dsd_min_samples (void *decimate_context)
{
    return HISTORY_BYTES;
}

int decimate_dsd_run (void *decimate_context, int32_t *samples, int numInputFrames)
{
    DecimateDSD *cxt = (DecimateDSD *) decimate_context;
    int32_t *outsamptr = samples;
    int numOutputFrames = 0;

    if (!cxt)
        return 0;

    if (numInputFrames < 0) {
        if (cxt->reset)
            return 0;

        for (int j = 1; j < DELAY_SAMPLES; ++j)
            for (int chan = 0; chan < cxt->num_channels; ++chan) {
                DecimateDSDchannel *sp = cxt->chans + chan;
                int32_t sum = 0;

                for (int i = j * 2; i < HISTORY_BYTES; ++i)
                    sum += cxt->conv_tables [i - j] [sp->delay [i]];

                sum = (int32_t) floor ((double) sum * cxt->filter_sums [0] / cxt->filter_sums [j] + 0.5);
                *outsamptr++ = (sum + 8) >> 4;
            }

        for (int chan = 0; chan < cxt->num_channels; ++chan) {
            *outsamptr = ((outsamptr [-cxt->num_channels] * 3) - outsamptr [-cxt->num_channels * 2]) / 2;
            outsamptr++;
        }

        decimate_dsd_reset (cxt);
        return DELAY_SAMPLES;
    }

    for (int i = 0; i < numInputFrames; ++i) {
        cxt->output_index++;

        for (int chan = 0; chan < cxt->num_channels; ++chan) {
            DecimateDSDchannel *sp = cxt->chans + chan;
            int32_t sum = 0, i;

            for (i = 0; i < HISTORY_BYTES-1; ++i)
                sum += cxt->conv_tables [i] [sp->delay [i] = sp->delay [i+1]];

            sum += cxt->conv_tables [i] [sp->delay [i] = *samples++];

            if (cxt->output_index >= HISTORY_BYTES) {
                *outsamptr++ = (sum + 8) >> 4;
                numOutputFrames++;
            }
        }

        if (cxt->output_index == HISTORY_BYTES - 2) {
            outsamptr += cxt->num_channels;     // make room for first sample which we reverse extrapolate

            for (int j = DELAY_SAMPLES - 1; j; j--)
                for (int chan = 0; chan < cxt->num_channels; ++chan) {
                    DecimateDSDchannel *sp = cxt->chans + chan;
                    int32_t sum = 0;

                    for (int i = 2; i <= (HISTORY_BYTES + 1) - (j * 2); ++i)
                        sum += cxt->conv_tables [i + j - 2] [sp->delay [i]];

                    sum = (int32_t) floor ((double) sum * cxt->filter_sums [0] / cxt->filter_sums [j] + 0.5);
                    *outsamptr++ = (sum + 8) >> 4;
                    numOutputFrames++;
                }

            outsamptr -= cxt->num_channels * DELAY_SAMPLES;

            for (int chan = 0; chan < cxt->num_channels; ++chan) {
                *outsamptr = ((outsamptr [cxt->num_channels] * 3) - outsamptr [cxt->num_channels * 2]) / 2;
                numOutputFrames++;
                outsamptr++;
            }

            outsamptr += cxt->num_channels * (DELAY_SAMPLES - 1);
            cxt->reset = 0;
        }
    }

    numOutputFrames /= cxt->num_channels;

    return numOutputFrames;
}

void decimate_dsd_destroy (void *decimate_context)
{
    DecimateDSD *cxt = (DecimateDSD *) decimate_context;

    if (!cxt)
        return;

    if (cxt->chans)
        free (cxt->chans);

    free (cxt);
}

#endif      // ENABLE_DSD
