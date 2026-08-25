////////////////////////////////////////////////////////////////////////////
//                           **** BIQUAD ****                             //
//                     Simple Biquad Filter Library                       //
//                Copyright (c) 2021 - 2022 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// biquad.h

#ifndef BIQUAD_H
#define BIQUAD_H

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define _USE_MATH_DEFINES
#include <math.h>

#if defined(PATH_WIDTH) && (PATH_WIDTH==64)
typedef double artsample_t;
#else
typedef float artsample_t;
#endif

typedef struct {
    artsample_t a0, a1, a2, a3, a4, b1, b2, b3, b4;
} BiquadCoefficients;

typedef struct {
    artsample_t a[5], b[5];               // coefficients
    artsample_t x[4], y[4];               // delayed input/output
    int order, index;
} Biquad;

#ifdef __cplusplus
extern "C" {
#endif

void biquad_init (Biquad *f, const BiquadCoefficients *coeffs, double gain);

void biquad_lowpass (BiquadCoefficients *filter, double frequency);
void biquad_highpass (BiquadCoefficients *filter, double frequency);

void biquad_apply_buffer (Biquad *f, artsample_t *buffer, int num_samples, int stride);
artsample_t biquad_apply_sample (Biquad *f, artsample_t input);

#ifdef __cplusplus
}
#endif

#endif

