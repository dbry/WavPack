////////////////////////////////////////////////////////////////////////////
//                            **** DECODER ****                           //
//                        Decode DXD-e Back To DSD                        //
//                     Copyright (c) 2026 David Bryant                    //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// decoder.h

#ifndef DECODER_H
#define DECODER_H

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "modulator.h"
#include "dsd-utils.h"

#define MODULATOR_SHARED_FLAGS  MODULATE_MULTITHREADED
#define DECODER_EXTRACT_ALWAYS  0x2

typedef enum { Init, Embedding, Generating, Syncing } DecoderState;

#define NUM_BUFFERS 3

typedef struct {
    char dsd_pilot_valid [NUM_BUFFERS];
    DecoderState state, next_state;
    int64_t valid_dsd_samples;
} DecoderChannel;

typedef struct {
    DecoderChannel *channels;
    PilotDetect *pilot_detector;
    DecimateDSD *decimator;
    Modulate *modulator;
    int nchans, level;

    int64_t total_dsd_samples, total_pcm_samples;
    int source_samples, composite_samples, flushing;
    unsigned char *embedded_buffer, *modulated_buffer, *composite_buffer;
    int32_t *source_buffer;
    float *float_buffer;
} Decoder;

#ifdef __cplusplus
extern "C" {
#endif

Decoder *decodeInit (int num_channels, int dsd_encode_level, int flags);
int decodeProcess (Decoder *cxt, const int32_t *source, int in_samples, unsigned char *destin, int out_samples);
int64_t decodeTotalEmbeddedSamples (Decoder *cxt);
void decodeFree (Decoder *cxt);

#ifdef __cplusplus
}
#endif

#endif
