////////////////////////////////////////////////////////////////////////////
//                           **** MODULATOR ****                          //
//                       Simple PCM to DSD Modulator                      //
//                     Copyright (c) 2026 David Bryant                    //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// modulator.h

#ifndef MODULATOR_H
#define MODULATOR_H

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <float.h>
#include <math.h>

#include "dsd-utils.h"
#include "workers.h"

#define NUM_FILTERS 8                   // upsample ratio
#define US_TAPS     16                  // upsample filter number of taps
#define NS_TAPS     4                   // taps in noise-shaping filter
#define NUM_SAMPLES 1024
#define MAX_DEPTH   24

#define DSD_DELAY   (HISTORY_BYTES * 2) - 1 // DSD byte delay line for analysis

#define MODULATE_MULTITHREADED      0x1
#define MODULATOR_PREFILLED         0x2     // do not set, internal use only
#define MODULATOR_FLUSHED           0x4     // do not set, internal use only
#define MODULATOR_ALIGN_EMBEDDED    0x10

typedef struct {
    int flags;
    int upsample_buffer_fill, upsample_buffer_conv, upsample_buffer_tail;
    int source_buffer_head, source_buffer_tail, level;
    float *source_buffer, *upsample_buffer;
    float last_sample, dither_level;
    double error_feedback [NS_TAPS];
    unsigned char *dsd_buffer;
    float **upsample_filters;
    uint32_t tpdf_generator;
    void *decimator;

    const float *input;
    int numOutputFrames, numInputFrames, stride, chan;
    unsigned char *mod_output, *emb_output;

    unsigned char dsd_embedded_buffer [DSD_DELAY], dsd_calculated_buffer [DSD_DELAY];
    int delayed_alignment, delayed_samples, plus_error_count, minus_error_count, large_error_count, error_sum;
    int64_t total_samples;

#ifdef STATISTICS
    int64_t called_best_sample, checked_alt_sample, used_alt_sample, leaves;
    float sample_min, sample_max, upsample_min, upsample_max, min_order, max_order;
    int max_run_count, run_count, max_depth_seen;
    double rms_filtered_error, rms_unfiltered_error;
    double max_filtered_error, max_unfiltered_error;
    double min_filtered_error, min_unfiltered_error;
    double last_sample_peak;
#endif
} ModulatorChannel;

typedef struct {
    int numChannels;
    Workers *workers;
    ModulatorChannel *channels;
} Modulate;

#ifdef __cplusplus
extern "C" {
#endif

Modulate *modulateInit (int numChannels, int level, int flags);
void modulateSetLevel (Modulate *cxt, int channel_number, int level);
void modulateSetAlignment (Modulate *cxt, int channel_number, int enable);
int modulateProcess (Modulate *cxt, const float *input, int numInputFrames, unsigned char *mod_output, unsigned char *emb_output);
void modulateFree (Modulate *cxt);

#ifdef __cplusplus
}
#endif

#endif
