////////////////////////////////////////////////////////////////////////////
//                           **** DSD UTILS ****                          //
//                         Various DSD/DXD Helpers                        //
//                     Copyright (c) 2026 David Bryant                    //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// dsd-utils.h

#ifndef DSD_UTILS_H
#define DSD_UTILS_H

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <float.h>
#include <time.h>
#include <math.h>

#include "biquad.h"

// ******************** DSD Decimator ********************

#define NUM_FILTER_TERMS    104     // must be odd multiple of eight, currently just 56 or 104

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

#ifdef __cplusplus
extern "C" {
#endif

DecimateDSD *decimateDSDinit (int num_channels, int flags);
void decimateDSDreset (DecimateDSD *cxt);
int decimateDSDrun (DecimateDSD *cxt, const unsigned char *in_samples, int numInputFrames, int32_t *out_samples);
int32_t decimateSingleDSDsample (DecimateDSD *cxt, const unsigned char in_samples [HISTORY_BYTES]);
void decimateDSDdestroy (DecimateDSD *cxt);

#ifdef __cplusplus
}
#endif

// ******************** DSD Embedding & Detection ********************

#define PILOT_SEQUENCE 0xf123456789abcde0

#define EMBED_PILOT_SIGNAL  0x1
#define EMBED_PILOT_UNIQUE  0x2

typedef struct {
    uint32_t *parity_shifters;
    Biquad *noise_shapers;
    float *noise_feedback;
    int64_t sample_index;
    int nchans, flags;
} EmbedDSD;

typedef struct {
    uint64_t channel_shifter, sample_index;
    uint32_t parity_shifter;
    int samples_to_skip;
    char locked;
} PilotDetectChannel;

typedef struct {
    uint64_t parity_masks [64];
    PilotDetectChannel *chans;
    int nchans;
} PilotDetect;

#ifdef __cplusplus
extern "C" {
#endif

EmbedDSD *embedDSDinit (int nchans, int flags);
void embedDSDrun (EmbedDSD *embed_context, int32_t *dst_buffer, unsigned char *src_buffer, int nsamples);
void embedDSDdestroy (EmbedDSD *embed_context);

PilotDetect *pilotDetectInit (int nchans);
int pilotDetectChannelRun (PilotDetect *context, const int32_t *src_buffer, int chan, int nsamples);
void pilotDetectDestroy (PilotDetect *context);

#ifdef __cplusplus
}
#endif

// ******************** DSD Transitioning ********************

#ifdef __cplusplus
extern "C" {
#endif

void transitionDSDstreams (DecimateDSD *decimator, int64_t samples, unsigned char *initial_dsd, const unsigned char *final_dsd, int byte_count);
void transitionDSDdumpstats (FILE *stream);

#ifdef __cplusplus
}
#endif

#endif
