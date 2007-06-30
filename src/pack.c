////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//              Copyright (c) 1998 - 2006 Conifer Software.               //
//               MMX optimizations (c) 2006 Joachim Henke                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// pack.c

// This module actually handles the compression of the audio data, except for
// the entropy coding which is handled by the words? modules. For efficiency,
// the conversion is isolated to tight loops that handle an entire buffer.

#include "wavpack_local.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#ifdef DEBUG_ALLOC
#define malloc malloc_db
#define realloc realloc_db
#define free free_db
void *malloc_db (uint32_t size);
void *realloc_db (void *ptr, uint32_t size);
void free_db (void *ptr);
int32_t dump_alloc (void);
#endif

//////////////////////////////// local tables ///////////////////////////////

// These two tables specify the characteristics of the decorrelation filters.
// Each term represents one layer of the sequential filter, where positive
// values indicate the relative sample involved from the same channel (1=prev),
// 17 & 18 are special functions using the previous 2 samples, and negative
// values indicate cross channel decorrelation (in stereo only).

static WavpackDecorrSpec fast_specs [] = {
        { 1, 2,18,17 },  // 0
};

static WavpackDecorrSpec default_specs [] = {
        { 1, 2,18,18, 2,17, 3 },         // 0
};

static WavpackDecorrSpec high_specs [] = {
        { 1, 2,18,18,18,-2, 2, 3, 5,-1,17, 4 },  // 0
};

static WavpackDecorrSpec very_high_specs [] = {
        { 1, 2,18,18, 2, 3,-2,18, 2, 4, 7, 5, 3, 6, 8,-1,18, 2 },        // 0
};

#define NUM_FAST_SPECS (sizeof (fast_specs) / sizeof (fast_specs [0]))
#define NUM_DEFAULT_SPECS (sizeof (default_specs) / sizeof (default_specs [0]))
#define NUM_HIGH_SPECS (sizeof (high_specs) / sizeof (high_specs [0]))
#define NUM_VERY_HIGH_SPECS (sizeof (very_high_specs) / sizeof (very_high_specs [0]))

///////////////////////////// executable code ////////////////////////////////

// This function initializes everything required to pack WavPack bitstreams
// and must be called BEFORE any other function in this module.

void pack_init (WavpackContext *wpc)
{
    WavpackStream *wps = wpc->streams [wpc->current_stream];

    wps->sample_index = 0;
    CLEAR (wps->decorr_passes);
    CLEAR (wps->dc);

    if (wpc->config.flags & CONFIG_AUTO_SHAPING)
        wps->dc.shaping_acc [0] = wps->dc.shaping_acc [1] =
            (wpc->config.sample_rate < 64000 || (wps->wphdr.flags & CROSS_DECORR)) ? -512L << 16 : 1024L << 16;
    else {
        int32_t weight = (int32_t) floor (wpc->config.shaping_weight * 1024.0 + 0.5);

        if (weight <= -1000)
            weight = -1000;

        wps->dc.shaping_acc [0] = wps->dc.shaping_acc [1] = weight << 16;
    }

    if (wpc->config.flags & CONFIG_VERY_HIGH_FLAG)
        wps->decorr_specs = very_high_specs;
    else if (wpc->config.flags & CONFIG_HIGH_FLAG)
        wps->decorr_specs = high_specs;
    else if (wpc->config.flags & CONFIG_FAST_FLAG)
        wps->decorr_specs = fast_specs;
    else
        wps->decorr_specs = default_specs;

    init_words (wps);
}

// Allocate room for and copy the decorrelation terms from the decorr_passes
// array into the specified metadata structure. Both the actual term id and
// the delta are packed into single characters.

void write_decorr_terms (WavpackStream *wps, WavpackMetadata *wpmd)
{
    int tcount = wps->num_terms;
    struct decorr_pass *dpp;
    char *byteptr;

    byteptr = wpmd->data = malloc (tcount + 1);
    wpmd->id = ID_DECORR_TERMS;

    for (dpp = wps->decorr_passes; tcount--; ++dpp)
        *byteptr++ = ((dpp->term + 5) & 0x1f) | ((dpp->delta << 5) & 0xe0);

    wpmd->byte_length = (int32_t)(byteptr - (char *) wpmd->data);
}

// Allocate room for and copy the decorrelation term weights from the
// decorr_passes array into the specified metadata structure. The weights
// range +/-1024, but are rounded and truncated to fit in signed chars for
// metadata storage. Weights are separate for the two channels

void write_decorr_weights (WavpackStream *wps, WavpackMetadata *wpmd)
{
    struct decorr_pass *dpp = wps->decorr_passes;
    int tcount = wps->num_terms, i;
    char *byteptr;

    byteptr = wpmd->data = malloc ((tcount * 2) + 1);
    wpmd->id = ID_DECORR_WEIGHTS;

    for (i = wps->num_terms - 1; i >= 0; --i)
        if (store_weight (dpp [i].weight_A) ||
            (!(wps->wphdr.flags & MONO_DATA) && store_weight (dpp [i].weight_B)))
                break;

    tcount = i + 1;

    for (i = 0; i < wps->num_terms; ++i) {
        if (i < tcount) {
            dpp [i].weight_A = restore_weight (*byteptr++ = store_weight (dpp [i].weight_A));

            if (!(wps->wphdr.flags & MONO_DATA))
                dpp [i].weight_B = restore_weight (*byteptr++ = store_weight (dpp [i].weight_B));
        }
        else
            dpp [i].weight_A = dpp [i].weight_B = 0;
    }

    wpmd->byte_length = (int32_t)(byteptr - (char *) wpmd->data);
}

// Allocate room for and copy the decorrelation samples from the decorr_passes
// array into the specified metadata structure. The samples are signed 32-bit
// values, but are converted to signed log2 values for storage in metadata.
// Values are stored for both channels and are specified from the first term
// with unspecified samples set to zero. The number of samples stored varies
// with the actual term value, so those must obviously be specified before
// these in the metadata list. Any number of terms can have their samples
// specified from no terms to all the terms, however I have found that
// sending more than the first term's samples is a waste. The "wcount"
// variable can be set to the number of terms to have their samples stored.

void write_decorr_samples (WavpackStream *wps, WavpackMetadata *wpmd)
{
    int tcount = wps->num_terms, wcount = 1, temp;
    struct decorr_pass *dpp;
    uchar *byteptr;

    byteptr = wpmd->data = malloc (256);
    wpmd->id = ID_DECORR_SAMPLES;

    for (dpp = wps->decorr_passes; tcount--; ++dpp)
        if (wcount) {
            if (dpp->term > MAX_TERM) {
                dpp->samples_A [0] = exp2s (temp = log2s (dpp->samples_A [0]));
                *byteptr++ = temp;
                *byteptr++ = temp >> 8;
                dpp->samples_A [1] = exp2s (temp = log2s (dpp->samples_A [1]));
                *byteptr++ = temp;
                *byteptr++ = temp >> 8;

                if (!(wps->wphdr.flags & MONO_DATA)) {
                    dpp->samples_B [0] = exp2s (temp = log2s (dpp->samples_B [0]));
                    *byteptr++ = temp;
                    *byteptr++ = temp >> 8;
                    dpp->samples_B [1] = exp2s (temp = log2s (dpp->samples_B [1]));
                    *byteptr++ = temp;
                    *byteptr++ = temp >> 8;
                }
            }
            else if (dpp->term < 0) {
                dpp->samples_A [0] = exp2s (temp = log2s (dpp->samples_A [0]));
                *byteptr++ = temp;
                *byteptr++ = temp >> 8;
                dpp->samples_B [0] = exp2s (temp = log2s (dpp->samples_B [0]));
                *byteptr++ = temp;
                *byteptr++ = temp >> 8;
            }
            else {
                int m = 0, cnt = dpp->term;

                while (cnt--) {
                    dpp->samples_A [m] = exp2s (temp = log2s (dpp->samples_A [m]));
                    *byteptr++ = temp;
                    *byteptr++ = temp >> 8;

                    if (!(wps->wphdr.flags & MONO_DATA)) {
                        dpp->samples_B [m] = exp2s (temp = log2s (dpp->samples_B [m]));
                        *byteptr++ = temp;
                        *byteptr++ = temp >> 8;
                    }

                    m++;
                }
            }

            wcount--;
        }
        else {
            CLEAR (dpp->samples_A);
            CLEAR (dpp->samples_B);
        }

    wpmd->byte_length = (int32_t)(byteptr - (uchar *) wpmd->data);
}

// Allocate room for and copy the noise shaping info into the specified
// metadata structure. These would normally be written to the
// "correction" file and are used for lossless reconstruction of
// hybrid data. The "delta" parameter is not yet used in encoding as it
// will be part of the "quality" mode.

void write_shaping_info (WavpackStream *wps, WavpackMetadata *wpmd)
{
    char *byteptr;
    int temp;

    byteptr = wpmd->data = malloc (12);
    wpmd->id = ID_SHAPING_WEIGHTS;

    wps->dc.error [0] = exp2s (temp = log2s (wps->dc.error [0]));
    *byteptr++ = temp;
    *byteptr++ = temp >> 8;
    wps->dc.shaping_acc [0] = exp2s (temp = log2s (wps->dc.shaping_acc [0]));
    *byteptr++ = temp;
    *byteptr++ = temp >> 8;

    if (!(wps->wphdr.flags & MONO_DATA)) {
        wps->dc.error [1] = exp2s (temp = log2s (wps->dc.error [1]));
        *byteptr++ = temp;
        *byteptr++ = temp >> 8;
        wps->dc.shaping_acc [1] = exp2s (temp = log2s (wps->dc.shaping_acc [1]));
        *byteptr++ = temp;
        *byteptr++ = temp >> 8;
    }

    if (wps->dc.shaping_delta [0] | wps->dc.shaping_delta [1]) {
        wps->dc.shaping_delta [0] = exp2s (temp = log2s (wps->dc.shaping_delta [0]));
        *byteptr++ = temp;
        *byteptr++ = temp >> 8;

        if (!(wps->wphdr.flags & MONO_DATA)) {
            wps->dc.shaping_delta [1] = exp2s (temp = log2s (wps->dc.shaping_delta [1]));
            *byteptr++ = temp;
            *byteptr++ = temp >> 8;
        }
    }

    wpmd->byte_length = (int32_t)(byteptr - (char *) wpmd->data);
}

// Allocate room for and copy the multichannel information into the specified
// metadata structure. The first byte is the total number of channels and the
// following bytes represent the channel_mask as described for Microsoft
// WAVEFORMATEX.

void write_channel_info (WavpackContext *wpc, WavpackMetadata *wpmd)
{
    uint32_t mask = wpc->config.channel_mask;
    char *byteptr;

    byteptr = wpmd->data = malloc (4);
    wpmd->id = ID_CHANNEL_INFO;
    *byteptr++ = wpc->config.num_channels;

    while (mask) {
        *byteptr++ = mask;
        mask >>= 8;
    }

    wpmd->byte_length = (int32_t)(byteptr - (char *) wpmd->data);
}

// Allocate room for and copy the configuration information into the specified
// metadata structure. Currently, we just store the upper 3 bytes of
// config.flags and only in the first block of audio data. Note that this is
// for informational purposes not required for playback or decoding (like
// whether high or fast mode was specified).

void write_config_info (WavpackContext *wpc, WavpackMetadata *wpmd)
{
    char *byteptr;

    byteptr = wpmd->data = malloc (4);
    wpmd->id = ID_CONFIG_BLOCK;
    *byteptr++ = (char) (wpc->config.flags >> 8);
    *byteptr++ = (char) (wpc->config.flags >> 16);
    *byteptr++ = (char) (wpc->config.flags >> 24);
    wpmd->byte_length = (int32_t)(byteptr - (char *) wpmd->data);
}

// Allocate room for and copy the non-standard sampling rateinto the specified
// metadata structure. We just store the lower 3 bytes of the sampling rate.
// Note that this would only be used when the sampling rate was not included
// in the table of 15 "standard" values.

void write_sample_rate (WavpackContext *wpc, WavpackMetadata *wpmd)

{
    char *byteptr;

    byteptr = wpmd->data = malloc (4);
    wpmd->id = ID_SAMPLE_RATE;
    *byteptr++ = (char) (wpc->config.sample_rate);
    *byteptr++ = (char) (wpc->config.sample_rate >> 8);
    *byteptr++ = (char) (wpc->config.sample_rate >> 16);
    wpmd->byte_length = (int32_t)(byteptr - (char *) wpmd->data);
}

// Pack an entire block of samples (either mono or stereo) into a completed
// WavPack block. This function is actually a shell for pack_samples() and
// performs tasks like handling any shift required by the format, preprocessing
// of floating point data or integer data over 24 bits wide, and implementing
// the "extra" mode (via the extra?.c modules). It is assumed that there is
// sufficient space for the completed block at "wps->blockbuff" and that
// "wps->blockend" points to the end of the available space. A return value of
// FALSE indicates an error.

static int pack_samples (WavpackContext *wpc, int32_t *buffer);

int pack_block (WavpackContext *wpc, int32_t *buffer)
{
    WavpackStream *wps = wpc->streams [wpc->current_stream];
    int32_t sample_count = wps->wphdr.block_samples;
    uint32_t flags = wps->wphdr.flags;

    if (flags & SHIFT_MASK) {
        int shift = (flags & SHIFT_MASK) >> SHIFT_LSB;
        int mag = (flags & MAG_MASK) >> MAG_LSB;
        uint32_t cnt = sample_count;
        int32_t *ptr = buffer;

        if (flags & MONO_DATA)
            while (cnt--)
                *ptr++ >>= shift;
        else
            while (cnt--) {
                *ptr++ >>= shift;
                *ptr++ >>= shift;
            }

        if ((mag -= shift) < 0)
            flags &= ~MAG_MASK;
        else
            flags -= (1 << MAG_LSB) * shift;

        wps->wphdr.flags = flags;
    }

    if (!wps->num_terms) {
        int j;

        CLEAR (wps->decorr_passes);
        wps->num_terms = (int) strlen (wps->decorr_specs->terms);

        for (j = 0; j < wps->num_terms; ++j) {
            wps->decorr_passes [j].delta = wps->decorr_specs->delta;

            if (wps->decorr_specs->terms [j] < 0) {
                if (flags & MONO_DATA)
                    wps->decorr_passes [j].term = 1;
                else if (!(flags & CROSS_DECORR))
                    wps->decorr_passes [j].term = -3;
                else
                    wps->decorr_passes [j].term = wps->decorr_specs->terms [j];
            }
            else
                wps->decorr_passes [j].term = wps->decorr_specs->terms [j];
        }
    }

    return pack_samples (wpc, buffer);
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

static void decorr_stereo_pass (struct decorr_pass *dpp, int32_t *buffer, int32_t sample_count);
static void decorr_stereo_pass_id2 (struct decorr_pass *dpp, int32_t *buffer, int32_t sample_count);

static int pack_samples (WavpackContext *wpc, int32_t *buffer)
{
    WavpackStream *wps = wpc->streams [wpc->current_stream];
    uint32_t flags = wps->wphdr.flags, data_count, crc, crc2, i;
    uint32_t sample_count = wps->wphdr.block_samples;
    int tcount, lossy = FALSE, m = 0, header;
    double noise_acc = 0.0, noise;
    struct decorr_pass *dpp;
    WavpackMetadata wpmd;
    int32_t *bptr;

    if ((flags & SUB_BLOCKS) && !wpc->metacount &&
        wpc->block_samples == sample_count && wps->sub_block_count) {
            wps->sub_block_count--;
            header = FALSE;
        }
        else {
            wps->sub_block_count = wpc->config.sub_blocks - 1;
            header = TRUE;
        }

    crc = crc2 = 0xffffffff;

    if (header) {
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

        write_decorr_terms (wps, &wpmd);
        copy_metadata (&wpmd, wps->blockbuff, wps->blockend);
        free_metadata (&wpmd);

        write_decorr_weights (wps, &wpmd);
        copy_metadata (&wpmd, wps->blockbuff, wps->blockend);
        free_metadata (&wpmd);

        write_decorr_samples (wps, &wpmd);
        copy_metadata (&wpmd, wps->blockbuff, wps->blockend);
        free_metadata (&wpmd);

        write_entropy_vars (wps, &wpmd);
        copy_metadata (&wpmd, wps->blockbuff, wps->blockend);
        free_metadata (&wpmd);

        if ((flags & SRATE_MASK) == SRATE_MASK && wpc->config.sample_rate != 44100) {
            write_sample_rate (wpc, &wpmd);
            copy_metadata (&wpmd, wps->blockbuff, wps->blockend);
            free_metadata (&wpmd);
        }

        if (flags & HYBRID_FLAG) {
            write_hybrid_profile (wps, &wpmd);
            copy_metadata (&wpmd, wps->blockbuff, wps->blockend);
            free_metadata (&wpmd);
        }

        if ((flags & INITIAL_BLOCK) &&
            (wpc->config.num_channels > 2 ||
            wpc->config.channel_mask != 0x5 - wpc->config.num_channels)) {
                write_channel_info (wpc, &wpmd);
                copy_metadata (&wpmd, wps->blockbuff, wps->blockend);
                free_metadata (&wpmd);
        }

        if ((flags & INITIAL_BLOCK) && !wps->sample_index) {
            write_config_info (wpc, &wpmd);
            copy_metadata (&wpmd, wps->blockbuff, wps->blockend);
            free_metadata (&wpmd);
        }

        bs_open_write (&wps->wvbits, wps->blockbuff + ((WavpackHeader *) wps->blockbuff)->ckSize + 12, wps->blockend);

        if (wpc->wvc_flag) {
            wps->wphdr.ckSize = sizeof (WavpackHeader) - 8;
            memcpy (wps->block2buff, &wps->wphdr, sizeof (WavpackHeader));

            if (flags & HYBRID_SHAPE) {
                write_shaping_info (wps, &wpmd);
                copy_metadata (&wpmd, wps->block2buff, wps->block2end);
                free_metadata (&wpmd);
            }

            bs_open_write (&wps->wvcbits, wps->block2buff + ((WavpackHeader *) wps->block2buff)->ckSize + 12, wps->block2end);
        }
    }
    else {
        bs_open_write (&wps->wvbits, wps->blockbuff + 4, wps->blockend);

        if (wpc->wvc_flag)
            bs_open_write (&wps->wvcbits, wps->block2buff + 4, wps->block2end);
    }

    /////////////////////// handle lossless mono mode /////////////////////////

    if (!(flags & HYBRID_FLAG) && (flags & MONO_DATA)) {
        for (bptr = buffer, i = 0; i < sample_count; ++i) {
            int32_t code = *bptr;

            crc += (crc << 1) + code;

            for (tcount = wps->num_terms, dpp = wps->decorr_passes; tcount--; dpp++) {
                int32_t sam;

                if (dpp->term > MAX_TERM) {
                    if (dpp->term & 1)
                        sam = 2 * dpp->samples_A [0] - dpp->samples_A [1];
                    else
                        sam = (3 * dpp->samples_A [0] - dpp->samples_A [1]) >> 1;

                    dpp->samples_A [1] = dpp->samples_A [0];
                    dpp->samples_A [0] = code;
                }
                else {
                    sam = dpp->samples_A [m];
                    dpp->samples_A [(m + dpp->term) & (MAX_TERM - 1)] = code;
                }

                code -= apply_weight (dpp->weight_A, sam);
                update_weight (dpp->weight_A, dpp->delta, sam, code);
            }

            m = (m + 1) & (MAX_TERM - 1);
            *bptr++ = code;
        }

        send_words_lossless (wps, buffer, sample_count);
    }

    //////////////////// handle the lossless stereo mode //////////////////////

    else if (!(flags & HYBRID_FLAG) && !(flags & MONO_DATA)) {
        int32_t *eptr = buffer + (sample_count * 2);

        for (bptr = buffer; bptr < eptr; bptr += 2)
            crc += (crc << 3) + (bptr [0] << 1) + bptr [0] + bptr [1];

        if (flags & JOINT_STEREO)
            for (bptr = buffer; bptr < eptr; bptr += 2)
                bptr [1] += ((bptr [0] -= bptr [1]) >> 1);

        for (tcount = wps->num_terms, dpp = wps->decorr_passes; tcount-- ; dpp++)
            if (((flags & MAG_MASK) >> MAG_LSB) >= 16 || dpp->delta != 2)
                decorr_stereo_pass (dpp, buffer, sample_count);
            else
                decorr_stereo_pass_id2 (dpp, buffer, sample_count);

        send_words_lossless (wps, buffer, sample_count);
    }

    /////////////////// handle the lossy/hybrid mono mode /////////////////////

    else if ((flags & HYBRID_FLAG) && (flags & MONO_DATA))
        for (bptr = buffer, i = 0; i < sample_count; ++i) {
            int32_t code, temp;

            crc2 += (crc2 << 1) + (code = *bptr++);

            if (flags & HYBRID_SHAPE) {
                int shaping_weight = (wps->dc.shaping_acc [0] += wps->dc.shaping_delta [0]) >> 16;
                temp = -apply_weight (shaping_weight, wps->dc.error [0]);

                if ((flags & NEW_SHAPING) && shaping_weight < 0 && temp) {
                    if (temp == wps->dc.error [0])
                        temp = (temp < 0) ? temp + 1 : temp - 1;

                    wps->dc.error [0] = -code;
                    code += temp;
                }
                else
                    wps->dc.error [0] = -(code += temp);
            }

            for (tcount = wps->num_terms, dpp = wps->decorr_passes; tcount-- ; dpp++)
                if (dpp->term > MAX_TERM) {
                    if (dpp->term & 1)
                        dpp->samples_A [2] = 2 * dpp->samples_A [0] - dpp->samples_A [1];
                    else
                        dpp->samples_A [2] = (3 * dpp->samples_A [0] - dpp->samples_A [1]) >> 1;

                    code -= (dpp->aweight_A = apply_weight (dpp->weight_A, dpp->samples_A [2]));
                }
                else
                    code -= (dpp->aweight_A = apply_weight (dpp->weight_A, dpp->samples_A [m]));

            code = send_word (wps, code, 0);

            while (--dpp >= wps->decorr_passes) {
                if (dpp->term > MAX_TERM) {
                    update_weight (dpp->weight_A, dpp->delta, dpp->samples_A [2], code);
                    dpp->samples_A [1] = dpp->samples_A [0];
                    dpp->samples_A [0] = (code += dpp->aweight_A);
                }
                else {
                    int32_t sam = dpp->samples_A [m];

                    update_weight (dpp->weight_A, dpp->delta, sam, code);
                    dpp->samples_A [(m + dpp->term) & (MAX_TERM - 1)] = (code += dpp->aweight_A);
                }
            }

            wps->dc.error [0] += code;
            m = (m + 1) & (MAX_TERM - 1);

            if ((crc += (crc << 1) + code) != crc2)
                lossy = TRUE;

            if (wpc->config.flags & CONFIG_CALC_NOISE) {
                noise = code - bptr [-1];

                noise_acc += noise *= noise;
                wps->dc.noise_ave = (wps->dc.noise_ave * 0.99) + (noise * 0.01);

                if (wps->dc.noise_ave > wps->dc.noise_max)
                    wps->dc.noise_max = wps->dc.noise_ave;
            }
        }

    /////////////////// handle the lossy/hybrid stereo mode ///////////////////

    else if ((flags & HYBRID_FLAG) && !(flags & MONO_DATA))
        for (bptr = buffer, i = 0; i < sample_count; ++i) {
            int32_t left, right, temp;
            int shaping_weight;

            left = *bptr++;
            crc2 += (crc2 << 3) + (left << 1) + left + (right = *bptr++);

            if (flags & HYBRID_SHAPE) {
                shaping_weight = (wps->dc.shaping_acc [0] += wps->dc.shaping_delta [0]) >> 16;
                temp = -apply_weight (shaping_weight, wps->dc.error [0]);

                if ((flags & NEW_SHAPING) && shaping_weight < 0 && temp) {
                    if (temp == wps->dc.error [0])
                        temp = (temp < 0) ? temp + 1 : temp - 1;

                    wps->dc.error [0] = -left;
                    left += temp;
                }
                else
                    wps->dc.error [0] = -(left += temp);

                shaping_weight = (wps->dc.shaping_acc [1] += wps->dc.shaping_delta [1]) >> 16;
                temp = -apply_weight (shaping_weight, wps->dc.error [1]);

                if ((flags & NEW_SHAPING) && shaping_weight < 0 && temp) {
                    if (temp == wps->dc.error [1])
                        temp = (temp < 0) ? temp + 1 : temp - 1;

                    wps->dc.error [1] = -right;
                    right += temp;
                }
                else
                    wps->dc.error [1] = -(right += temp);
            }

            if (flags & JOINT_STEREO)
                right += ((left -= right) >> 1);

            for (tcount = wps->num_terms, dpp = wps->decorr_passes; tcount-- ; dpp++)
                if (dpp->term > MAX_TERM) {
                    if (dpp->term & 1) {
                        dpp->samples_A [2] = 2 * dpp->samples_A [0] - dpp->samples_A [1];
                        dpp->samples_B [2] = 2 * dpp->samples_B [0] - dpp->samples_B [1];
                    }
                    else {
                        dpp->samples_A [2] = (3 * dpp->samples_A [0] - dpp->samples_A [1]) >> 1;
                        dpp->samples_B [2] = (3 * dpp->samples_B [0] - dpp->samples_B [1]) >> 1;
                    }

                    left -= (dpp->aweight_A = apply_weight (dpp->weight_A, dpp->samples_A [2]));
                    right -= (dpp->aweight_B = apply_weight (dpp->weight_B, dpp->samples_B [2]));
                }
                else if (dpp->term > 0) {
                    left -= (dpp->aweight_A = apply_weight (dpp->weight_A, dpp->samples_A [m]));
                    right -= (dpp->aweight_B = apply_weight (dpp->weight_B, dpp->samples_B [m]));
                }
                else {
                    if (dpp->term == -1)
                        dpp->samples_B [0] = left;
                    else if (dpp->term == -2)
                        dpp->samples_A [0] = right;

                    left -= (dpp->aweight_A = apply_weight (dpp->weight_A, dpp->samples_A [0]));
                    right -= (dpp->aweight_B = apply_weight (dpp->weight_B, dpp->samples_B [0]));
                }

            left = send_word (wps, left, 0);
            right = send_word (wps, right, 1);

            while (--dpp >= wps->decorr_passes)
                if (dpp->term > MAX_TERM) {
                    update_weight (dpp->weight_A, dpp->delta, dpp->samples_A [2], left);
                    update_weight (dpp->weight_B, dpp->delta, dpp->samples_B [2], right);

                    dpp->samples_A [1] = dpp->samples_A [0];
                    dpp->samples_B [1] = dpp->samples_B [0];

                    dpp->samples_A [0] = (left += dpp->aweight_A);
                    dpp->samples_B [0] = (right += dpp->aweight_B);
                }
                else if (dpp->term > 0) {
                    int k = (m + dpp->term) & (MAX_TERM - 1);

                    update_weight (dpp->weight_A, dpp->delta, dpp->samples_A [m], left);
                    dpp->samples_A [k] = (left += dpp->aweight_A);

                    update_weight (dpp->weight_B, dpp->delta, dpp->samples_B [m], right);
                    dpp->samples_B [k] = (right += dpp->aweight_B);
                }
                else {
                    if (dpp->term == -1) {
                        dpp->samples_B [0] = left + dpp->aweight_A;
                        dpp->aweight_B = apply_weight (dpp->weight_B, dpp->samples_B [0]);
                    }
                    else if (dpp->term == -2) {
                        dpp->samples_A [0] = right + dpp->aweight_B;
                        dpp->aweight_A = apply_weight (dpp->weight_A, dpp->samples_A [0]);
                    }

                    update_weight_clip (dpp->weight_A, dpp->delta, dpp->samples_A [0], left);
                    update_weight_clip (dpp->weight_B, dpp->delta, dpp->samples_B [0], right);
                    dpp->samples_B [0] = (left += dpp->aweight_A);
                    dpp->samples_A [0] = (right += dpp->aweight_B);
                }

            if (flags & JOINT_STEREO)
                left += (right -= (left >> 1));

            wps->dc.error [0] += left;
            wps->dc.error [1] += right;
            m = (m + 1) & (MAX_TERM - 1);

            if ((crc += (crc << 3) + (left << 1) + left + right) != crc2)
                lossy = TRUE;

            if (wpc->config.flags & CONFIG_CALC_NOISE) {
                noise = (double)(left - bptr [-2]) * (left - bptr [-2]);
                noise += (double)(right - bptr [-1]) * (right - bptr [-1]);

                noise_acc += noise /= 2.0;
                wps->dc.noise_ave = (wps->dc.noise_ave * 0.99) + (noise * 0.01);

                if (wps->dc.noise_ave > wps->dc.noise_max)
                    wps->dc.noise_max = wps->dc.noise_ave;
            }
        }

    if (m)
        for (tcount = wps->num_terms, dpp = wps->decorr_passes; tcount--; dpp++)
            if (dpp->term > 0 && dpp->term <= MAX_TERM) {
                int32_t temp_A [MAX_TERM], temp_B [MAX_TERM];
                int k;

                memcpy (temp_A, dpp->samples_A, sizeof (dpp->samples_A));
                memcpy (temp_B, dpp->samples_B, sizeof (dpp->samples_B));

                for (k = 0; k < MAX_TERM; k++) {
                    dpp->samples_A [k] = temp_A [m];
                    dpp->samples_B [k] = temp_B [m];
                    m = (m + 1) & (MAX_TERM - 1);
                }
            }

    if (wpc->config.flags & CONFIG_CALC_NOISE)
        wps->dc.noise_sum += noise_acc;

    flush_word (wps);
    data_count = bs_close_write (&wps->wvbits);

    if (data_count) {
        if (data_count != (uint32_t) -1) {
            uchar *cptr;

            if (header)
                cptr = wps->blockbuff + ((WavpackHeader *) wps->blockbuff)->ckSize + 8;
            else
                cptr = wps->blockbuff;

            if (data_count > 510) {
                *cptr++ = ID_WV_BITSTREAM | ID_LARGE;
                *cptr++ = data_count >> 1;
                *cptr++ = data_count >> 9;
                *cptr++ = data_count >> 17;

                if (header)
                    ((WavpackHeader *) wps->blockbuff)->ckSize += data_count + 4;
            }
            else {
                *cptr++ = ID_WV_BITSTREAM;
                *cptr++ = data_count >> 1;
                memmove (cptr, cptr + 2, data_count);

                if (header)
                    ((WavpackHeader *) wps->blockbuff)->ckSize += data_count + 2;
            }
        }
        else
            return FALSE;
    }

    if (header)
        ((WavpackHeader *) wps->blockbuff)->crc = (flags & SUB_BLOCKS) ?
            wps->wphdr.block_samples * wpc->config.sub_blocks : crc;

    if (wpc->wvc_flag) {
        data_count = bs_close_write (&wps->wvcbits);

        if (!header || (data_count && lossy)) {
            if (data_count != (uint32_t) -1) {
                uchar *cptr;

                if (header)
                    cptr = wps->block2buff + ((WavpackHeader *) wps->block2buff)->ckSize + 8;
                else
                    cptr = wps->block2buff;

                if (data_count > 510) {
                    *cptr++ = ID_WVC_BITSTREAM | ID_LARGE;
                    *cptr++ = data_count >> 1;
                    *cptr++ = data_count >> 9;
                    *cptr++ = data_count >> 17;

                    if (header)
                        ((WavpackHeader *) wps->block2buff)->ckSize += data_count + 4;
                }
                else {
                    *cptr++ = ID_WVC_BITSTREAM;
                    *cptr++ = data_count >> 1;
                    memmove (cptr, cptr + 2, data_count);

                    if (header)
                        ((WavpackHeader *) wps->block2buff)->ckSize += data_count + 2;
                }
            }
            else
                return FALSE;
        }

        if (header)
            ((WavpackHeader *) wps->block2buff)->crc = (flags & SUB_BLOCKS) ?
                wps->wphdr.block_samples * wpc->config.sub_blocks : crc2;
    }
    else if (lossy)
        wpc->lossy_blocks = TRUE;

    wps->sample_index += sample_count;
    return TRUE;
}

// Perform a pass of the stereo decorrelation as specified by the referenced
// dpp structure. This version is optimized for samples that can use the
// simple apply_weight macro (i.e. <= 16-bit audio) and for when the weight
// delta is 2 (which is the case with all the default, non -x modes). For
// cases that do not fit this model, the more general decorr_stereo_pass()
// is provided. Note that this function returns the dpp->samples_X[] values
// in the "normalized" positions for terms 1-8.

static void decorr_stereo_pass_id2 (struct decorr_pass *dpp, int32_t *buffer, int32_t sample_count)
{
    int32_t *bptr, *eptr = buffer + (sample_count * 2);
    int m, k;

    switch (dpp->term) {
        case 17:
            for (bptr = buffer; bptr < eptr; bptr += 2) {
                int32_t sam, tmp;

                sam = 2 * dpp->samples_A [0] - dpp->samples_A [1];
                dpp->samples_A [1] = dpp->samples_A [0];
                bptr [0] = tmp = (dpp->samples_A [0] = bptr [0]) - apply_weight_i (dpp->weight_A, sam);
                update_weight_d2 (dpp->weight_A, dpp->delta, sam, tmp);

                sam = 2 * dpp->samples_B [0] - dpp->samples_B [1];
                dpp->samples_B [1] = dpp->samples_B [0];
                bptr [1] = tmp = (dpp->samples_B [0] = bptr [1]) - apply_weight_i (dpp->weight_B, sam);
                update_weight_d2 (dpp->weight_B, dpp->delta, sam, tmp);
            }

            break;

        case 18:
            for (bptr = buffer; bptr < eptr; bptr += 2) {
                int32_t sam, tmp;

                sam = dpp->samples_A [0] + ((dpp->samples_A [0] - dpp->samples_A [1]) >> 1);
                dpp->samples_A [1] = dpp->samples_A [0];
                bptr [0] = tmp = (dpp->samples_A [0] = bptr [0]) - apply_weight_i (dpp->weight_A, sam);
                update_weight_d2 (dpp->weight_A, dpp->delta, sam, tmp);

                sam = dpp->samples_B [0] + ((dpp->samples_B [0] - dpp->samples_B [1]) >> 1);
                dpp->samples_B [1] = dpp->samples_B [0];
                bptr [1] = tmp = (dpp->samples_B [0] = bptr [1]) - apply_weight_i (dpp->weight_B, sam);
                update_weight_d2 (dpp->weight_B, dpp->delta, sam, tmp);
            }

            break;

        default:
            for (m = 0, k = dpp->term & (MAX_TERM - 1), bptr = buffer; bptr < eptr; bptr += 2) {
                int32_t sam, tmp;

                sam = dpp->samples_A [m];
                bptr [0] = tmp = (dpp->samples_A [k] = bptr [0]) - apply_weight_i (dpp->weight_A, sam);
                update_weight_d2 (dpp->weight_A, dpp->delta, sam, tmp);

                sam = dpp->samples_B [m];
                bptr [1] = tmp = (dpp->samples_B [k] = bptr [1]) - apply_weight_i (dpp->weight_B, sam);
                update_weight_d2 (dpp->weight_B, dpp->delta, sam, tmp);

                m = (m + 1) & (MAX_TERM - 1);
                k = (k + 1) & (MAX_TERM - 1);
            }

            if (m) {
                int32_t temp_A [MAX_TERM], temp_B [MAX_TERM];

                memcpy (temp_A, dpp->samples_A, sizeof (dpp->samples_A));
                memcpy (temp_B, dpp->samples_B, sizeof (dpp->samples_B));

                for (k = 0; k < MAX_TERM; k++) {
                    dpp->samples_A [k] = temp_A [m];
                    dpp->samples_B [k] = temp_B [m];
                    m = (m + 1) & (MAX_TERM - 1);
                }
            }

            break;

        case -1:
            for (bptr = buffer; bptr < eptr; bptr += 2) {
                int32_t sam_A, sam_B, tmp;

                sam_A = dpp->samples_A [0];
                bptr [0] = tmp = (sam_B = bptr [0]) - apply_weight_i (dpp->weight_A, sam_A);
                update_weight_clip_d2 (dpp->weight_A, dpp->delta, sam_A, tmp);

                bptr [1] = tmp = (dpp->samples_A [0] = bptr [1]) - apply_weight_i (dpp->weight_B, sam_B);
                update_weight_clip_d2 (dpp->weight_B, dpp->delta, sam_B, tmp);
            }

            break;

        case -2:
            for (bptr = buffer; bptr < eptr; bptr += 2) {
                int32_t sam_A, sam_B, tmp;

                sam_B = dpp->samples_B [0];
                bptr [1] = tmp = (sam_A = bptr [1]) - apply_weight_i (dpp->weight_B, sam_B);
                update_weight_clip_d2 (dpp->weight_B, dpp->delta, sam_B, tmp);

                bptr [0] = tmp = (dpp->samples_B [0] = bptr [0]) - apply_weight_i (dpp->weight_A, sam_A);
                update_weight_clip_d2 (dpp->weight_A, dpp->delta, sam_A, tmp);
            }

            break;

        case -3:
            for (bptr = buffer; bptr < eptr; bptr += 2) {
                int32_t sam_A, sam_B, tmp;

                sam_A = dpp->samples_A [0];
                sam_B = dpp->samples_B [0];

                dpp->samples_A [0] = tmp = bptr [1];
                bptr [1] = tmp -= apply_weight_i (dpp->weight_B, sam_B);
                update_weight_clip_d2 (dpp->weight_B, dpp->delta, sam_B, tmp);

                dpp->samples_B [0] = tmp = bptr [0];
                bptr [0] = tmp -= apply_weight_i (dpp->weight_A, sam_A);
                update_weight_clip_d2 (dpp->weight_A, dpp->delta, sam_A, tmp);
            }

            break;
    }
}

// Perform a pass of the stereo decorrelation as specified by the referenced
// dpp structure. This function is provided in both a regular C version and
// an MMX version (using intrinsics) written by Joachim Henke. The MMX version
// is significantly faster when the sample data requires the full-resolution
// apply_weight macro. However, when the data is lower resolution (<= 16-bit)
// then the difference is slight (or the MMX is even slower), so for these
// cases the simpler decorr_stereo_pass_id2() is used. Note that this function
// returns the dpp->samples_X[] values in the "normalized" positions for
// terms 1-8.

#ifdef OPT_MMX

static void decorr_stereo_pass (struct decorr_pass *dpp, int32_t *buffer, int32_t sample_count)
{
    const __m64
        delta = _mm_set1_pi32 (dpp->delta),
        fill = _mm_set1_pi32 (0x7bff),
        mask = _mm_set1_pi32 (0x7fff),
        round = _mm_set1_pi32 (512),
        zero = _mm_set1_pi32 (0);
    __m64
        weight_AB = _mm_set_pi32 (restore_weight (store_weight (dpp->weight_B)), restore_weight (store_weight (dpp->weight_A))),
        left_right, sam_AB, tmp0, tmp1, samples_AB [MAX_TERM];
    int k, m = 0;

    for (k = 0; k < MAX_TERM; ++k) {
        ((int32_t *) samples_AB) [k * 2] = exp2s (log2s (dpp->samples_A [k]));
        ((int32_t *) samples_AB) [k * 2 + 1] = exp2s (log2s (dpp->samples_B [k]));
    }

    if (dpp->term > 0) {
        if (dpp->term == 17) {
            while (sample_count--) {
                left_right = *(__m64 *) buffer;
                tmp0 = samples_AB [0];
                sam_AB = _m_paddd (tmp0, tmp0);
                sam_AB = _m_psubd (sam_AB, samples_AB [1]);
                samples_AB [0] = left_right;
                samples_AB [1] = tmp0;

                tmp0 = _m_paddd (sam_AB, sam_AB);
                tmp1 = _m_pand (sam_AB, mask);
                tmp0 = _m_psrldi (tmp0, 16);
                tmp1 = _m_pmaddwd (tmp1, weight_AB);
                tmp0 = _m_pmaddwd (tmp0, weight_AB);
                tmp1 = _m_paddd (tmp1, round);
                tmp0 = _m_pslldi (tmp0, 5);
                tmp1 = _m_psradi (tmp1, 10);
                left_right = _m_psubd (left_right, tmp0);
                left_right = _m_psubd (left_right, tmp1);

                *(__m64 *) buffer = left_right;

                tmp0 = _m_pxor (sam_AB, left_right);
                tmp0 = _m_psradi (tmp0, 31);
                tmp1 = _m_pxor (delta, tmp0);
                tmp1 = _m_psubd (tmp1, tmp0);
                sam_AB = _m_pcmpeqd (sam_AB, zero);
                tmp0 = _m_pcmpeqd (left_right, zero);
                tmp0 = _m_por (tmp0, sam_AB);
                tmp0 = _m_pandn (tmp0, tmp1);
                weight_AB = _m_paddd (weight_AB, tmp0);

                buffer += 2;
            }
        }
        else if (dpp->term == 18) {
            while (sample_count--) {
                left_right = *(__m64 *) buffer;
                tmp0 = samples_AB [0];
                sam_AB = _m_psubd (tmp0, samples_AB [1]);
                sam_AB = _m_psradi (sam_AB, 1);
                sam_AB = _m_paddd (sam_AB, tmp0);
                samples_AB [0] = left_right;
                samples_AB [1] = tmp0;

                tmp0 = _m_paddd (sam_AB, sam_AB);
                tmp1 = _m_pand (sam_AB, mask);
                tmp0 = _m_psrldi (tmp0, 16);
                tmp1 = _m_pmaddwd (tmp1, weight_AB);
                tmp0 = _m_pmaddwd (tmp0, weight_AB);
                tmp1 = _m_paddd (tmp1, round);
                tmp0 = _m_pslldi (tmp0, 5);
                tmp1 = _m_psradi (tmp1, 10);
                left_right = _m_psubd (left_right, tmp0);
                left_right = _m_psubd (left_right, tmp1);

                *(__m64 *) buffer = left_right;

                tmp0 = _m_pxor (sam_AB, left_right);
                tmp0 = _m_psradi (tmp0, 31);
                tmp1 = _m_pxor (delta, tmp0);
                tmp1 = _m_psubd (tmp1, tmp0);
                sam_AB = _m_pcmpeqd (sam_AB, zero);
                tmp0 = _m_pcmpeqd (left_right, zero);
                tmp0 = _m_por (tmp0, sam_AB);
                tmp0 = _m_pandn (tmp0, tmp1);
                weight_AB = _m_paddd (weight_AB, tmp0);

                buffer += 2;
            }
        }
        else {
            k = dpp->term & (MAX_TERM - 1);
            while (sample_count--) {
                left_right = *(__m64 *) buffer;
                sam_AB = samples_AB [m];
                samples_AB [k] = left_right;

                tmp0 = _m_paddd (sam_AB, sam_AB);
                tmp1 = _m_pand (sam_AB, mask);
                tmp0 = _m_psrldi (tmp0, 16);
                tmp1 = _m_pmaddwd (tmp1, weight_AB);
                tmp0 = _m_pmaddwd (tmp0, weight_AB);
                tmp1 = _m_paddd (tmp1, round);
                tmp0 = _m_pslldi (tmp0, 5);
                tmp1 = _m_psradi (tmp1, 10);
                left_right = _m_psubd (left_right, tmp0);
                left_right = _m_psubd (left_right, tmp1);

                *(__m64 *) buffer = left_right;

                tmp0 = _m_pxor (sam_AB, left_right);
                tmp0 = _m_psradi (tmp0, 31);
                tmp1 = _m_pxor (delta, tmp0);
                tmp1 = _m_psubd (tmp1, tmp0);
                sam_AB = _m_pcmpeqd (sam_AB, zero);
                tmp0 = _m_pcmpeqd (left_right, zero);
                tmp0 = _m_por (tmp0, sam_AB);
                tmp0 = _m_pandn (tmp0, tmp1);
                weight_AB = _m_paddd (weight_AB, tmp0);

                buffer += 2;
                k = (k + 1) & (MAX_TERM - 1);
                m = (m + 1) & (MAX_TERM - 1);
            }
        }
    }
    else {
        if (dpp->term == -1) {
            while (sample_count--) {
                left_right = *(__m64 *) buffer;
                sam_AB = samples_AB [0];
                samples_AB [0] = _m_punpckhdq (left_right, sam_AB);
                sam_AB = _m_punpckldq (sam_AB, left_right);

                tmp0 = _m_paddd (sam_AB, sam_AB);
                tmp1 = _m_pand (sam_AB, mask);
                tmp0 = _m_psrldi (tmp0, 16);
                tmp1 = _m_pmaddwd (tmp1, weight_AB);
                tmp0 = _m_pmaddwd (tmp0, weight_AB);
                tmp1 = _m_paddd (tmp1, round);
                tmp0 = _m_pslldi (tmp0, 5);
                tmp1 = _m_psradi (tmp1, 10);
                left_right = _m_psubd (left_right, tmp0);
                left_right = _m_psubd (left_right, tmp1);

                *(__m64 *) buffer = left_right;

                tmp0 = _m_pcmpeqd (sam_AB, zero);
                tmp1 = _m_pcmpeqd (left_right, zero);
                tmp0 = _m_por (tmp0, tmp1);
                tmp0 = _m_pandn (tmp0, delta);
                sam_AB = _m_pxor (sam_AB, left_right);
                sam_AB = _m_psradi (sam_AB, 31);
                tmp1 = _m_psubd (fill, sam_AB);
                weight_AB = _m_pxor (weight_AB, sam_AB);
                weight_AB = _m_paddd (weight_AB, tmp1);
                weight_AB = _m_paddsw (weight_AB, tmp0);
                weight_AB = _m_psubd (weight_AB, tmp1);
                weight_AB = _m_pxor (weight_AB, sam_AB);

                buffer += 2;
            }
        }
        else if (dpp->term == -2) {
            while (sample_count--) {
                left_right = *(__m64 *) buffer;
                sam_AB = samples_AB [0];
                samples_AB [0] = _m_punpckldq (sam_AB, left_right);
                sam_AB = _m_punpckhdq (left_right, sam_AB);

                tmp0 = _m_paddd (sam_AB, sam_AB);
                tmp1 = _m_pand (sam_AB, mask);
                tmp0 = _m_psrldi (tmp0, 16);
                tmp1 = _m_pmaddwd (tmp1, weight_AB);
                tmp0 = _m_pmaddwd (tmp0, weight_AB);
                tmp1 = _m_paddd (tmp1, round);
                tmp0 = _m_pslldi (tmp0, 5);
                tmp1 = _m_psradi (tmp1, 10);
                left_right = _m_psubd (left_right, tmp0);
                left_right = _m_psubd (left_right, tmp1);

                *(__m64 *) buffer = left_right;

                tmp0 = _m_pcmpeqd (sam_AB, zero);
                tmp1 = _m_pcmpeqd (left_right, zero);
                tmp0 = _m_por (tmp0, tmp1);
                tmp0 = _m_pandn (tmp0, delta);
                sam_AB = _m_pxor (sam_AB, left_right);
                sam_AB = _m_psradi (sam_AB, 31);
                tmp1 = _m_psubd (fill, sam_AB);
                weight_AB = _m_pxor (weight_AB, sam_AB);
                weight_AB = _m_paddd (weight_AB, tmp1);
                weight_AB = _m_paddsw (weight_AB, tmp0);
                weight_AB = _m_psubd (weight_AB, tmp1);
                weight_AB = _m_pxor (weight_AB, sam_AB);

                buffer += 2;
            }
        }
        else if (dpp->term == -3) {
            while (sample_count--) {
                left_right = *(__m64 *) buffer;
                sam_AB = samples_AB [0];
                tmp0 = _m_punpckhdq (left_right, left_right);
                samples_AB [0] = _m_punpckldq (tmp0, left_right);

                tmp0 = _m_paddd (sam_AB, sam_AB);
                tmp1 = _m_pand (sam_AB, mask);
                tmp0 = _m_psrldi (tmp0, 16);
                tmp1 = _m_pmaddwd (tmp1, weight_AB);
                tmp0 = _m_pmaddwd (tmp0, weight_AB);
                tmp1 = _m_paddd (tmp1, round);
                tmp0 = _m_pslldi (tmp0, 5);
                tmp1 = _m_psradi (tmp1, 10);
                left_right = _m_psubd (left_right, tmp0);
                left_right = _m_psubd (left_right, tmp1);

                *(__m64 *) buffer = left_right;

                tmp0 = _m_pcmpeqd (sam_AB, zero);
                tmp1 = _m_pcmpeqd (left_right, zero);
                tmp0 = _m_por (tmp0, tmp1);
                tmp0 = _m_pandn (tmp0, delta);
                sam_AB = _m_pxor (sam_AB, left_right);
                sam_AB = _m_psradi (sam_AB, 31);
                tmp1 = _m_psubd (fill, sam_AB);
                weight_AB = _m_pxor (weight_AB, sam_AB);
                weight_AB = _m_paddd (weight_AB, tmp1);
                weight_AB = _m_paddsw (weight_AB, tmp0);
                weight_AB = _m_psubd (weight_AB, tmp1);
                weight_AB = _m_pxor (weight_AB, sam_AB);

                buffer += 2;
            }
        }
    }

    dpp->weight_A = ((int32_t *) &weight_AB) [0];
    dpp->weight_B = ((int32_t *) &weight_AB) [1];

    for (k = 0; k < MAX_TERM; ++k) {
        dpp->samples_A [k] = ((int32_t *) samples_AB) [m * 2];
        dpp->samples_B [k] = ((int32_t *) samples_AB) [m * 2 + 1];
        m = (m + 1) & (MAX_TERM - 1);
    }

    _mm_empty ();
}

#else

static void decorr_stereo_pass (struct decorr_pass *dpp, int32_t *buffer, int32_t sample_count)
{
    int32_t *bptr, *eptr = buffer + (sample_count * 2);
    int m, k;

    switch (dpp->term) {
        case 17:
            for (bptr = buffer; bptr < eptr; bptr += 2) {
                int32_t sam, tmp;

                sam = 2 * dpp->samples_A [0] - dpp->samples_A [1];
                dpp->samples_A [1] = dpp->samples_A [0];
                bptr [0] = tmp = (dpp->samples_A [0] = bptr [0]) - apply_weight (dpp->weight_A, sam);
                update_weight (dpp->weight_A, dpp->delta, sam, tmp);

                sam = 2 * dpp->samples_B [0] - dpp->samples_B [1];
                dpp->samples_B [1] = dpp->samples_B [0];
                bptr [1] = tmp = (dpp->samples_B [0] = bptr [1]) - apply_weight (dpp->weight_B, sam);
                update_weight (dpp->weight_B, dpp->delta, sam, tmp);
            }

            break;

        case 18:
            for (bptr = buffer; bptr < eptr; bptr += 2) {
                int32_t sam, tmp;

                sam = dpp->samples_A [0] + ((dpp->samples_A [0] - dpp->samples_A [1]) >> 1);
                dpp->samples_A [1] = dpp->samples_A [0];
                bptr [0] = tmp = (dpp->samples_A [0] = bptr [0]) - apply_weight (dpp->weight_A, sam);
                update_weight (dpp->weight_A, dpp->delta, sam, tmp);

                sam = dpp->samples_B [0] + ((dpp->samples_B [0] - dpp->samples_B [1]) >> 1);
                dpp->samples_B [1] = dpp->samples_B [0];
                bptr [1] = tmp = (dpp->samples_B [0] = bptr [1]) - apply_weight (dpp->weight_B, sam);
                update_weight (dpp->weight_B, dpp->delta, sam, tmp);
            }

            break;

        default:
            for (m = 0, k = dpp->term & (MAX_TERM - 1), bptr = buffer; bptr < eptr; bptr += 2) {
                int32_t sam, tmp;

                sam = dpp->samples_A [m];
                bptr [0] = tmp = (dpp->samples_A [k] = bptr [0]) - apply_weight (dpp->weight_A, sam);
                update_weight (dpp->weight_A, dpp->delta, sam, tmp);

                sam = dpp->samples_B [m];
                bptr [1] = tmp = (dpp->samples_B [k] = bptr [1]) - apply_weight (dpp->weight_B, sam);
                update_weight (dpp->weight_B, dpp->delta, sam, tmp);

                m = (m + 1) & (MAX_TERM - 1);
                k = (k + 1) & (MAX_TERM - 1);
            }

            if (m) {
                int32_t temp_A [MAX_TERM], temp_B [MAX_TERM];

                memcpy (temp_A, dpp->samples_A, sizeof (dpp->samples_A));
                memcpy (temp_B, dpp->samples_B, sizeof (dpp->samples_B));

                for (k = 0; k < MAX_TERM; k++) {
                    dpp->samples_A [k] = temp_A [m];
                    dpp->samples_B [k] = temp_B [m];
                    m = (m + 1) & (MAX_TERM - 1);
                }
            }

            break;

        case -1:
            for (bptr = buffer; bptr < eptr; bptr += 2) {
                int32_t sam_A, sam_B, tmp;

                sam_A = dpp->samples_A [0];
                bptr [0] = tmp = (sam_B = bptr [0]) - apply_weight (dpp->weight_A, sam_A);
                update_weight_clip (dpp->weight_A, dpp->delta, sam_A, tmp);

                bptr [1] = tmp = (dpp->samples_A [0] = bptr [1]) - apply_weight (dpp->weight_B, sam_B);
                update_weight_clip (dpp->weight_B, dpp->delta, sam_B, tmp);
            }

            break;

        case -2:
            for (bptr = buffer; bptr < eptr; bptr += 2) {
                int32_t sam_A, sam_B, tmp;

                sam_B = dpp->samples_B [0];
                bptr [1] = tmp = (sam_A = bptr [1]) - apply_weight (dpp->weight_B, sam_B);
                update_weight_clip (dpp->weight_B, dpp->delta, sam_B, tmp);

                bptr [0] = tmp = (dpp->samples_B [0] = bptr [0]) - apply_weight (dpp->weight_A, sam_A);
                update_weight_clip (dpp->weight_A, dpp->delta, sam_A, tmp);
            }

            break;

        case -3:
            for (bptr = buffer; bptr < eptr; bptr += 2) {
                int32_t sam_A, sam_B, tmp;

                sam_A = dpp->samples_A [0];
                sam_B = dpp->samples_B [0];

                dpp->samples_A [0] = tmp = bptr [1];
                bptr [1] = tmp -= apply_weight (dpp->weight_B, sam_B);
                update_weight_clip (dpp->weight_B, dpp->delta, sam_B, tmp);

                dpp->samples_B [0] = tmp = bptr [0];
                bptr [0] = tmp -= apply_weight (dpp->weight_A, sam_A);
                update_weight_clip (dpp->weight_A, dpp->delta, sam_A, tmp);
            }

            break;
    }
}

#endif

//////////////////////////////////////////////////////////////////////////////
// This function returns the accumulated RMS noise as a double if the       //
// CALC_NOISE bit was set in the WavPack header. The peak noise can also be //
// returned if desired. See wavpack.c for the calculations required to      //
// convert this into decibels of noise below full scale.                    //
//////////////////////////////////////////////////////////////////////////////

double WavpackGetEncodedNoise (WavpackContext *wpc, double *peak)
{
    WavpackStream *wps = wpc->streams [wpc->current_stream];

    if (peak)
        *peak = wps->dc.noise_max;

    return wps->dc.noise_sum;
}
