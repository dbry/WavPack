////////////////////////////////////////////////////////////////////////////
//                           **** BIQUAD ****                             //
//                     Simple Biquad Filter Library                       //
//                Copyright (c) 2021 - 2024 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// biquad.c

#include "biquad.h"

// This module implements biquad-like filters from first-order to fourth-order.
// Of course, technically, "biquads" are second-order, but they're all similar.

// Second-order Lowpass

void biquad_lowpass (BiquadCoefficients *filter, double frequency)
{
    double Q = sqrt (0.5), K = tan (M_PI * frequency);
    double norm = 1.0 / (1.0 + K / Q + K * K);

    memset (filter, 0, sizeof (BiquadCoefficients));

    filter->a0 = K * K * norm;
    filter->a1 = 2 * filter->a0;
    filter->a2 = filter->a0;
    filter->b1 = 2.0 * (K * K - 1.0) * norm;
    filter->b2 = (1.0 - K / Q + K * K) * norm;
}

// Second-order Highpass

void biquad_highpass (BiquadCoefficients *filter, double frequency)
{
    double Q = sqrt (0.5), K = tan (M_PI * frequency);
    double norm = 1.0 / (1.0 + K / Q + K * K);

    memset (filter, 0, sizeof (BiquadCoefficients));

    filter->a0 = norm;
    filter->a1 = -2.0 * norm;
    filter->a2 = filter->a0;
    filter->b1 = 2.0 * (K * K - 1.0) * norm;
    filter->b2 = (1.0 - K / Q + K * K) * norm;
}

// Initialize the specified biquad filter with the given parameters. Note that the "gain" parameter is supplied here
// to save a multiply every time the filter in applied.

void biquad_init (Biquad *f, const BiquadCoefficients *coeffs, double gain)
{
    memset (f, 0, sizeof (Biquad));

    f->a[0] = coeffs->a0 * gain;
    f->a[1] = coeffs->a1 * gain;
    f->a[2] = coeffs->a2 * gain;
    f->a[3] = coeffs->a3 * gain;
    f->a[4] = coeffs->a4 * gain;

    f->b[1] = coeffs->b1;
    f->b[2] = coeffs->b2;
    f->b[3] = coeffs->b3;
    f->b[4] = coeffs->b4;

    if (coeffs->a4 != 0.0F || coeffs->b4 != 0.0F)
        f->order = 4;
    else if (coeffs->a3 != 0.0F || coeffs->b3 != 0.0F)
        f->order = 3;
    else if (coeffs->a2 != 0.0F || coeffs->b2 != 0.0F)
        f->order = 2;
    else
        f->order = 1;
}

// Apply the supplied sample to the specified biquad filter, which must have been initialized with biquad_init().

artsample_t biquad_apply_sample (Biquad *f, artsample_t input)
{
    artsample_t sum = input * f->a[0];
    int i = f->index & 3;

    switch (f->order) {
        case 4:
            sum += (f->x[(i-3)&3] * f->a[4]) - (f->b[4] * f->y[(i-3)&3]);

        case 3:
            sum += (f->x[(i-2)&3] * f->a[3]) - (f->b[3] * f->y[(i-2)&3]);

        case 2:
            sum += (f->x[(i-1)&3] * f->a[2]) - (f->b[2] * f->y[(i-1)&3]);

        case 1:
            sum += (f->x[i]       * f->a[1]) - (f->b[1] * f->y[i]);
    }

    f->index = i = (i + 1) & 3;
    f->x [i] = input;
    f->y [i] = sum;

    return sum;
}

// Apply the supplied buffer to the specified biquad filter, which must have been initialized with biquad_init().

void biquad_apply_buffer (Biquad *f, artsample_t *buffer, int num_samples, int stride)
{
    int i = f->index;

    switch (f->order) {
        case 4:
            while (num_samples--) {
                artsample_t sum = (*buffer * f->a[0])
                    + (f->x[i&3]     * f->a[1]) - (f->b[1] * f->y[i&3])
                    + (f->x[(i-1)&3] * f->a[2]) - (f->b[2] * f->y[(i-1)&3])
                    + (f->x[(i-2)&3] * f->a[3]) - (f->b[3] * f->y[(i-2)&3])
                    + (f->x[(i-3)&3] * f->a[4]) - (f->b[4] * f->y[(i-3)&3]);
                f->x[++i&3] = *buffer;
                *buffer = f->y[i&3] = sum;
                buffer += stride;
            }

            break;

        case 3:
            while (num_samples--) {
                artsample_t sum = (*buffer * f->a[0])
                    + (f->x[i&3]     * f->a[1]) - (f->b[1] * f->y[i&3])
                    + (f->x[(i-1)&3] * f->a[2]) - (f->b[2] * f->y[(i-1)&3])
                    + (f->x[(i-2)&3] * f->a[3]) - (f->b[3] * f->y[(i-2)&3]);
                f->x[++i&3] = *buffer;
                *buffer = f->y[i&3] = sum;
                buffer += stride;
            }

            break;

        case 2:
            while (num_samples--) {
                artsample_t sum = (*buffer * f->a[0])
                    + (f->x[i&3]     * f->a[1]) - (f->b[1] * f->y[i&3])
                    + (f->x[(i-1)&3] * f->a[2]) - (f->b[2] * f->y[(i-1)&3]);
                f->x[++i&3] = *buffer;
                *buffer = f->y[i&3] = sum;
                buffer += stride;
            }

            break;

        case 1:
            while (num_samples--) {
                artsample_t sum = (*buffer * f->a[0])
                    + (f->x[i&3] * f->a[1]) - (f->b[1] * f->y[i&3]);
                f->x[++i&3] = *buffer;
                *buffer = f->y[i&3] = sum;
                buffer += stride;
            }

            break;
    }

    f->index = i;
}
