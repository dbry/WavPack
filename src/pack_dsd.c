////////////////////////////////////////////////////////////////////////////
//                           **** DSDPACK ****                            //
//         Lossless DSD (Direct Stream Digital) Audio Compressor          //
//                Copyright (c) 2013 - 2016 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// pack_dsd.c

// This module actually handles the compression of the DSD audio data.

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "wavpack_local.h"

#define MAX_HISTORY_BITS    5
#define MAX_PROBABILITY     0xa0    // set to 0xff to disable RLE encoding for probabilities table

///////////////////////////// executable code ////////////////////////////////

// This function initializes everything required to pack WavPack DSD bitstreams
// and must be called BEFORE any other function in this module.

void pack_dsd_init (WavpackContext *wpc)
{
    WavpackStream *wps = wpc->streams [wpc->current_stream];

    wps->sample_index = 0;
}

// Pack an entire block of samples (either mono or stereo) into a completed
// WavPack block. This function is actually a shell for pack_samples() and
// performs tasks like handling any shift required by the format, preprocessing
// of floating point data or integer data over 24 bits wide, and implementing
// the "extra" mode (via the extra?.c modules). It is assumed that there is
// sufficient space for the completed block at "wps->blockbuff" and that
// "wps->blockend" points to the end of the available space. A return value of
// FALSE indicates an error.

static int pack_dsd_samples (WavpackContext *wpc, int32_t *buffer);

int pack_dsd_block (WavpackContext *wpc, int32_t *buffer)
{
    return pack_dsd_samples (wpc, buffer);
}

// Pack an entire block of samples (either mono or stereo) into a completed
// WavPack block. It is assumed that there is sufficient space for the
// completed block at "wps->blockbuff" and that "wps->blockend" points to the
// end of the available space. A return value of FALSE indicates an error.
// Any unsent metadata is transmitted first, then required metadata for this
// block is sent, and finally the compressed integer data is sent. If a "wpx"
// stream is required for floating point data or large integer data, then this
// must be handled outside this function. To find out how much data was written
// the caller must look at the ckSize field of the written WavpackHeader, NOT
// the one in the WavpackStream.

static int encode_buffer (WavpackStream *wps, int32_t *buffer, int num_samples, unsigned char *destination);

static int pack_dsd_samples (WavpackContext *wpc, int32_t *buffer)
{
    WavpackStream *wps = wpc->streams [wpc->current_stream];
    uint32_t flags = wps->wphdr.flags, mult = wpc->dsd_multiplier, data_count, bc;
    uint32_t sample_count = wps->wphdr.block_samples;
    unsigned char *dsd_encoding, dsd_power = 0;
    WavpackMetadata wpmd;
    int32_t res;

    wps->wphdr.ckSize = sizeof (WavpackHeader) - 8;
    memcpy (wps->blockbuff, &wps->wphdr, sizeof (WavpackHeader));

    if (wpc->metacount) {
        WavpackMetadata *wpmdp = wpc->metadata;

        while (wpc->metacount) {
            copy_metadata (wpmdp, wps->blockbuff, wps->blockend);
            wpc->metabytes -= wpmdp->byte_length;
            free_metadata (wpmdp++);
            wpc->metacount--;
        }

        free (wpc->metadata);
        wpc->metadata = NULL;
    }

    if (!sample_count)
        return TRUE;

    memcpy (&wps->wphdr, wps->blockbuff, sizeof (WavpackHeader));

    dsd_encoding = wps->blockbuff + ((WavpackHeader *) wps->blockbuff)->ckSize + 12;

    while (mult >>= 1)
        dsd_power++;

    *dsd_encoding++ = dsd_power;

    res = encode_buffer (wps, buffer, sample_count, dsd_encoding + 1);

    if (res == -1) {
        int num_samples = sample_count * ((flags & MONO_FLAG) ? 1 : 2);
        *dsd_encoding++ = 0;

        data_count = num_samples + 2;

        while (num_samples--) {
            *dsd_encoding++ = *buffer++;
        }
    }
    else {
        *dsd_encoding = 1;
        data_count = res + 2;
    }

    if (data_count) {
        unsigned char *cptr = wps->blockbuff + ((WavpackHeader *) wps->blockbuff)->ckSize + 8;

        if (data_count & 1) {
            *cptr++ = ID_DSD_BLOCK | ID_LARGE | ID_ODD_SIZE;
            data_count++;
        }
        else
            *cptr++ = ID_DSD_BLOCK | ID_LARGE;

        *cptr++ = data_count >> 1;
        *cptr++ = data_count >> 9;
        *cptr++ = data_count >> 17;
        ((WavpackHeader *) wps->blockbuff)->ckSize += data_count + 4;
    }

    wps->sample_index += sample_count;
    return TRUE;
}

#if (MAX_PROBABILITY < 0xff)

static int rle_encode (unsigned char *src, int bcount, unsigned char *destination)
{
    int max_rle_zeros = 0xff - MAX_PROBABILITY;
    unsigned char *dp = destination;
    int zcount = 0;

    while (bcount--) {
        if (*src) {
            while (zcount) {
                *dp++ = MAX_PROBABILITY + (zcount > max_rle_zeros ? max_rle_zeros : zcount);
                zcount -= (zcount > max_rle_zeros ? max_rle_zeros : zcount);
            }

            *dp++ = *src++;
        }
        else {
            zcount++;
            src++;
        }
    }

    while (zcount) {
        *dp++ = MAX_PROBABILITY + (zcount > max_rle_zeros ? max_rle_zeros : zcount);
        zcount -= (zcount > max_rle_zeros ? max_rle_zeros : zcount);
    }

    *dp++ = 0;

    return (int)(dp - destination);
}

#endif

static void calculate_probabilities (int hist [256], unsigned char probs [256], unsigned short prob_sums [256])
{
    int divisor, min_value, max_value, sum_values;
    int min_hits = 0x7fffffff, max_hits = 0, i;

    for (i = 0; i < 256; ++i) {
        if (hist [i] < min_hits) min_hits = hist [i];
        if (hist [i] > max_hits) max_hits = hist [i];
    }

    if (max_hits == 0) {
        memset (probs, 0, sizeof (*probs) * 256);
        memset (prob_sums, 0, sizeof (*prob_sums) * 256);
        return;
    }

//  fprintf (stderr, "process_histogram(): hits = %d to %d\n", min_hits, max_hits);

    if (max_hits > MAX_PROBABILITY)
        divisor = ((max_hits << 8) + (MAX_PROBABILITY >> 1)) / MAX_PROBABILITY;
    else
        divisor = 0;

    while (1) {
        min_value = 0x7fffffff; max_value = 0; sum_values = 0;

        for (i = 0; i < 256; ++i) {
            int value;

            if (hist [i]) {
                if (divisor) {
                    if (!(value = ((hist [i] << 8) + (divisor >> 1)) / divisor))
                        value = 1;
                }
                else
                    value = hist [i];

                if (value < min_value) min_value = value;
                if (value > max_value) max_value = value;
            }
            else
                value = 0;

            prob_sums [i] = sum_values += value;
            probs [i] = value;
        }

        if (max_value > MAX_PROBABILITY) {
            divisor++;
            continue;
        }

#if 0   // this code reduces probability values when they are completely redundant (i.e., common divisor), but
        // this doesn't really happen often enough to make it worthwhile

        if (min_value > 1) {
            for (i = 0; i < 256; ++i)
                if (probs [i] % min_value)
                    break;

            if (i == 256) {
                for (i = 0; i < 256; ++i) {
                    prob_sums [i] /= min_value;
                    probs [i] /= min_value;
                }

                // fprintf (stderr, "fixed min_value = %d, divisor = %d, probs_sum = %d\n", min_value, divisor, prob_sums [255]);
            }
        }
#endif

        break;
    }
}

static int encode_buffer (WavpackStream *wps, int32_t *buffer, int num_samples, unsigned char *destination)
{
    uint32_t flags = wps->wphdr.flags;
    int history_bins, bc, p0 = 0, p1 = 0;
    unsigned int low = 0, high = 0xffffffff, mult;
    unsigned short (*summed_probabilities) [256];
    unsigned char (*probabilities) [256];
    unsigned char *dp = destination, *ep;
    int total_summed_probabilities = 0;
    int (*histogram) [256];
    int32_t *bp = buffer;
    char history_bits;

    if (!(flags & MONO_FLAG))
        num_samples *= 2;

    if (num_samples < 280)
        return -1;
    else if (num_samples < 560)
        history_bits = 0;
    else if (num_samples < 1725)
        history_bits = 1;
    else if (num_samples < 5000)
        history_bits = 2;
    else if (num_samples < 14000)
        history_bits = 3;
    else if (num_samples < 28000)
        history_bits = 4;
    else if (num_samples < 76000)
        history_bits = 5;
    else if (num_samples < 130000)
        history_bits = 6;
    else if (num_samples < 300000)
        history_bits = 7;
    else
        history_bits = 8;

    if (history_bits > MAX_HISTORY_BITS)
        history_bits = MAX_HISTORY_BITS;

    history_bins = 1 << history_bits;
    histogram = malloc (sizeof (*histogram) * history_bins);
    memset (histogram, 0, sizeof (*histogram) * history_bins);
    probabilities = malloc (sizeof (*probabilities) * history_bins);
    summed_probabilities = malloc (sizeof (*summed_probabilities) * history_bins);

    bc = num_samples;

    if (flags & MONO_FLAG)
        while (bc--) {
            histogram [p0] [*bp & 0xff]++;
            p0 = *bp++ & (history_bins-1);
        }
    else
        while (bc--) {
            histogram [p0] [*bp & 0xff]++;
            p0 = p1;
            p1 = *bp++ & (history_bins-1);
        }

    for (p0 = 0; p0 < history_bins; p0++) {
        calculate_probabilities (histogram [p0], probabilities [p0], summed_probabilities [p0]);
        total_summed_probabilities += summed_probabilities [p0] [255];
    }

    // This code detects the case where the required value lookup tables grow silly big and cuts them back down. This would
    // normally only happen with large blocks or poorly compressible data. The target is to guarantee that the total memory
    // required for all three decode tables will be 2K bytes per history bin.

    while (total_summed_probabilities > history_bins * 1280) {
        int max_sum = 0, sum_values = 0, largest_bin;

        for (p0 = 0; p0 < history_bins; ++p0)
            if (summed_probabilities [p0] [255] > max_sum) {
                max_sum = summed_probabilities [p0] [255];
                largest_bin = p0;
            }

        total_summed_probabilities -= max_sum;
        p0 = largest_bin;

        for (p1 = 0; p1 < 256; ++p1)
            summed_probabilities [p0] [p1] = sum_values += probabilities [p0] [p1] = (probabilities [p0] [p1] + 1) >> 1;

        total_summed_probabilities += summed_probabilities [p0] [255];
        // fprintf (stderr, "processed bin 0x%02x, bin: %d --> %d, new sum = %d\n",
        //     p0, max_sum, summed_probabilities [p0] [255], total_summed_probabilities);
    }

    free (histogram);
    bp = buffer;
    bc = num_samples;
    *dp++ = history_bits;
    *dp++ = MAX_PROBABILITY;
    ep = destination + num_samples - 10;

#if (MAX_PROBABILITY < 0xff)
    dp += rle_encode ((unsigned char *) probabilities, sizeof (*probabilities) * history_bins, dp);
#else
    memcpy (dp, probabilities, sizeof (*probabilities) * history_bins);
    dp += sizeof (*probabilities) * history_bins;
#endif

    p0 = p1 = 0;

    while (dp < ep && bc--) {

        mult = (high - low) / summed_probabilities [p0] [255];

        if (!mult) {
            high = low;

            while ((high >> 24) == (low >> 24)) {
                *dp++ = high >> 24;
                high = (high << 8) | 0xff;
                low <<= 8;
            }

            mult = (high - low) / summed_probabilities [p0] [255];
        }

        if (*bp & 0xff)
            low += summed_probabilities [p0] [(*bp & 0xff)-1] * mult;

        high = low + probabilities [p0] [*bp & 0xff] * mult - 1;

        while ((high >> 24) == (low >> 24)) {
            *dp++ = high >> 24;
            high = (high << 8) | 0xff;
            low <<= 8;
        }

        if (flags & MONO_FLAG)
            p0 = *bp++ & (history_bins-1);
        else {
            p0 = p1;
            p1 = *bp++ & (history_bins-1);
        }
    }

    high = low;

    while ((high >> 24) == (low >> 24)) {
        *dp++ = high >> 24;
        high = (high << 8) | 0xff;
        low <<= 8;
    }

    free (summed_probabilities);
    free (probabilities);

    if (dp < ep)
        return (int)(dp - destination);
    else
        return -1;
}
