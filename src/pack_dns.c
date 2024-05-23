////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//              Copyright (c) 1998 - 2013 Conifer Software.               //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// pack_dns.c

// This module handles the implementation of "dynamic noise shaping" which is
// designed to move the spectrum of the quantization noise introduced by lossy
// compression up or down in frequency so that it is more likely to be masked
// by the source material.

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "wavpack_local.h"

#define NEW_DNS_METHOD
// #define RMS_WINDOW_AVERAGE
// #define VERBOSE

#ifdef NEW_DNS_METHOD
static void generate_dns_values (const int32_t *samples, int sample_count, uint32_t flags, short *values, short min_value);
#endif

static void best_floating_line (short *values, int num_values, double *initial_y, double *final_y, short *max_error);

void dynamic_noise_shaping (WavpackStream *wps, const int32_t *buffer, int shortening_allowed)
{
    int32_t sample_count = wps->wphdr.block_samples;
    uint32_t flags = wps->wphdr.flags;
    short min_value = -512;
#ifndef NEW_DNS_METHOD
    const int32_t *bptr;
    short *swptr;
    int sc;
#endif

#ifdef NEW_DNS_METHOD
    if (wps->bits < 768) {
        min_value = -768 - ((768 - wps->bits) * 16 / 25);

        if (min_value < -896)   // -0.875 is minimum usable shaping
            min_value = -896;
    }
    else
        min_value = -768;       // at 3 bits/sample and up minimum shaping is -0.75
#endif

#ifndef NEW_DNS_METHOD
    if (!wps->num_terms && sample_count > 8) {
        struct decorr_pass *ap = &wps->analysis_pass;
        int32_t temp, sam;

        if (flags & MONO_DATA) {
            for (bptr = buffer + sample_count - 3, sc = sample_count - 2; sc--;) {
                sam = (3 * bptr [1] - bptr [2]) >> 1;
                temp = *bptr-- - apply_weight (ap->weight_A, sam);
                update_weight (ap->weight_A, 2, sam, temp);
            }
            // fprintf (stderr, "dynamic_noise_shaping(1): reverse scan, weight = %d\n", ap->weight_A);
            for (bptr = buffer + sample_count - 3, sc = sample_count - 2; sc--;) {
                sam = (3 * bptr [1] - bptr [2]) >> 1;
                temp = *bptr-- - apply_weight (ap->weight_A, sam);
                update_weight (ap->weight_A, 2, sam, temp);
            }
            // fprintf (stderr, "dynamic_noise_shaping(2): reverse scan, weight = %d\n", ap->weight_A);
        }
        else {
            for (bptr = buffer + (sample_count - 3) * 2 + 1, sc = sample_count - 2; sc--;) {
                sam = (3 * bptr [2] - bptr [4]) >> 1;
                temp = *bptr-- - apply_weight (ap->weight_B, sam);
                update_weight (ap->weight_B, 2, sam, temp);
                sam = (3 * bptr [2] - bptr [4]) >> 1;
                temp = *bptr-- - apply_weight (ap->weight_A, sam);
                update_weight (ap->weight_A, 2, sam, temp);
            }
            // fprintf (stderr, "dynamic_noise_shaping(): reverse scan, weights = %d, %d\n", ap->weight_A, ap->weight_B);
        }
    }
#endif

    if (sample_count > wps->dc.shaping_samples) {
#ifndef NEW_DNS_METHOD
        sc = sample_count - wps->dc.shaping_samples;
        swptr = wps->dc.shaping_data + wps->dc.shaping_samples;
        bptr = buffer + wps->dc.shaping_samples * ((flags & MONO_DATA) ? 1 : 2);
        struct decorr_pass *ap = &wps->analysis_pass;
        int32_t temp, sam;

        if (flags & MONO_DATA) {
            while (sc--) {
                sam = (3 * ap->samples_A [0] - ap->samples_A [1]) >> 1;
                temp = *bptr - apply_weight (ap->weight_A, sam);
                update_weight (ap->weight_A, 2, sam, temp);
                ap->samples_A [1] = ap->samples_A [0];
                ap->samples_A [0] = *bptr++;
                *swptr++ = (ap->weight_A < 256) ? 1024 : 1536 - ap->weight_A * 2;
            }
            // fprintf (stderr, "dynamic_noise_shaping(): forward scan, weight = %d\n", ap->weight_A);
        }
        else {
            while (sc--) {
                sam = (3 * ap->samples_A [0] - ap->samples_A [1]) >> 1;
                temp = *bptr - apply_weight (ap->weight_A, sam);
                update_weight (ap->weight_A, 2, sam, temp);
                ap->samples_A [1] = ap->samples_A [0];
                ap->samples_A [0] = *bptr++;

                sam = (3 * ap->samples_B [0] - ap->samples_B [1]) >> 1;
                temp = *bptr - apply_weight (ap->weight_B, sam);
                update_weight (ap->weight_B, 2, sam, temp);
                ap->samples_B [1] = ap->samples_B [0];
                ap->samples_B [0] = *bptr++;

                *swptr++ = (ap->weight_A + ap->weight_B < 512) ? 1024 : 1536 - ap->weight_A - ap->weight_B;
            }
            // fprintf (stderr, "dynamic_noise_shaping(): foward scan, weights = %d, %d\n", ap->weight_A, ap->weight_B);
        }
#else
        short *new_values = malloc (sample_count * sizeof (short));

        generate_dns_values (buffer, sample_count, flags, new_values, min_value);

        int existing_values_to_use = wps->dc.shaping_samples / 2;
        int new_values_to_use = sample_count - existing_values_to_use;
        memcpy (wps->dc.shaping_data + existing_values_to_use, new_values + existing_values_to_use, new_values_to_use * sizeof (short));

#ifdef VERBOSE
        int early_diffs = 0, late_diffs = 0;

        for (int k = 0; k < wps->dc.shaping_samples; ++k)
            if (wps->dc.shaping_data [k] != new_values [k]) {
                if (k < wps->dc.shaping_samples / 2)
                    ++early_diffs;
                else
                    ++late_diffs;
            }

        if (early_diffs || late_diffs)
            fprintf (stderr, "%d generated values, %d existing, %d early diffs, %d late diffs\n",
                sample_count, wps->dc.shaping_samples, early_diffs, late_diffs);

        int max_delta = -1, max_delta_index = 0;
        for (int k = 0; k < wps->dc.shaping_samples - 1; ++k) {
            int delta = abs (wps->dc.shaping_data [k] - wps->dc.shaping_data [k+1]);

            if (delta > max_delta) {
                max_delta_index = k;
                max_delta = delta;
            }
        }

        if (max_delta != -1)
            fprintf (stderr, "max delta = %d, data [%d] = %d, %d\n", max_delta, max_delta_index,
                wps->dc.shaping_data [max_delta_index], wps->dc.shaping_data [max_delta_index+1]);
#endif
        free (new_values);
#endif
        wps->dc.shaping_samples = sample_count;
    }

    if (wps->wpc->wvc_flag) {
        int max_allowed_error = 1000000 / wps->wpc->ave_block_samples;
        short max_error, trial_max_error;
        double initial_y, final_y;

        if (max_allowed_error < 128)
            max_allowed_error = 128;

#ifdef VERBOSE
        for (int k = 0; k < sample_count; ++k)
            if (k && wps->dc.shaping_data [k] == 0 && k < sample_count - 1) {
                fprintf (stderr, "shaping_data [%d] = (%d) %d (%d)\n", k, wps->dc.shaping_data [k-1], 0, wps->dc.shaping_data [k+1]);
                break;
            }
#endif

        best_floating_line (wps->dc.shaping_data, sample_count, &initial_y, &final_y, &max_error);

#define MIN_BLOCK_SAMPLES 16

        if (shortening_allowed && max_error > max_allowed_error && sample_count > MIN_BLOCK_SAMPLES) {
            int min_samples = 0, max_samples = sample_count, trial_count;
            double trial_initial_y, trial_final_y;

            // min_samples can only go up
            // max_samples can only go down
            // max_samples > min_samples

            while (1) {
                trial_count = (min_samples + max_samples) / 2;

                if (trial_count < MIN_BLOCK_SAMPLES)
                    trial_count = MIN_BLOCK_SAMPLES;

                best_floating_line (wps->dc.shaping_data, trial_count, &trial_initial_y,
                    &trial_final_y, &trial_max_error);

                if (trial_count == MIN_BLOCK_SAMPLES || trial_max_error < max_allowed_error) {
                    max_error = trial_max_error;
                    min_samples = trial_count;
                    initial_y = trial_initial_y;
                    final_y = trial_final_y;
                }
                else
                    max_samples = trial_count;

                if (min_samples > 10000 || max_samples - min_samples < 2)
                    break;
            }

            sample_count = min_samples;
        }

        if (initial_y < min_value) initial_y = min_value;
        else if (initial_y > 1024) initial_y = 1024;

        if (final_y < min_value) final_y = min_value;
        else if (final_y > 1024) final_y = 1024;

#ifdef VERBOSE
        fprintf (stderr, "%.5f sec, sample count = %5d / %5d, max error = %3d, range = %5d, %5d, actual = %5d, %5d\n",
            (double) wps->sample_index / wps->wpc->config.sample_rate, sample_count, wps->wphdr.block_samples, max_error,
            (int) floor (initial_y), (int) floor (final_y),
            wps->dc.shaping_data [0], wps->dc.shaping_data [sample_count-1]);
#endif
        if (sample_count != wps->wphdr.block_samples)
            wps->wphdr.block_samples = sample_count;

        if (wps->wpc->wvc_flag) {
            wps->dc.shaping_acc [0] = wps->dc.shaping_acc [1] = (int32_t) floor (initial_y * 65536.0 + 0.5);

            wps->dc.shaping_delta [0] = wps->dc.shaping_delta [1] =
                (int32_t) floor ((final_y - initial_y) / (sample_count - 1) * 65536.0 + 0.5);

#ifdef NEW_DNS_METHOD
            wps->dc.shaping_acc [0] -= wps->dc.shaping_delta [0];
            wps->dc.shaping_acc [1] -= wps->dc.shaping_delta [1];
#endif

            wps->dc.shaping_array = NULL;
        }
        else
            wps->dc.shaping_array = wps->dc.shaping_data;
    }
    else
        wps->dc.shaping_array = wps->dc.shaping_data;
}

#ifdef NEW_DNS_METHOD

#define HALF_WINDOW_WIDTH 50

static void win_average_buffer (float *samples, int sample_count)
{
    float *output = malloc (sample_count * sizeof (float));
    double sum = 0.0;
    int m = 0, n = 0;
    int i, j, k;

    for (i = 0; i < sample_count; ++i) {
        k = i + HALF_WINDOW_WIDTH + 1;
        j = i - HALF_WINDOW_WIDTH;

        if (k > sample_count) k = sample_count;
        if (j < 0) j = 0;

#ifdef RMS_WINDOW_AVERAGE
        while (m < j) {
            sum -= samples [m] * samples [m];
            m++;
        }

        while (n < k) {
            sum += samples [n] * samples [n];
            n++;
        }

        output [i] = sqrt (sum / (n - m));
#else
        while (m < j) {
            sum -= fabs (samples [m]);
            m++;
        }

        while (n < k) {
            sum += fabs (samples [n]);
            n++;
        }

        output [i] = sum / (n - m);
#endif
    }

    memcpy (samples, output, sample_count * sizeof (float));
    free (output);
}

// Lowpass filter at fs/6. Note that filter must contain an odd number of taps so
// that we can subtract the result from the source to get complimentary highpass.

float lp_filter [] = {
    0.00150031, 0.00000000, -0.01703392, -0.03449186, 0.00000000,
    0.11776257, 0.26543272, 0.33366033, 0.26543272, 0.11776259,
    0.00000001, -0.03449186, -0.01703392, 0.00000000, 0.00150031
};

#define FILTER_LENGTH (sizeof (lp_filter) / sizeof (lp_filter [0]))

static void generate_dns_values (const int32_t *samples, int sample_count, uint32_t flags, short *values, short min_value)
{
    int filtered_count = sample_count - FILTER_LENGTH + 1, i, j;
    float *low_freq, *high_freq;

    if (filtered_count <= 0) {
#ifdef VERBOSE
        fprintf (stderr, "generate_dns_values() generated %d zeros!\n", sample_count);
#endif
        memset (values, 0, sample_count * sizeof (values [0]));
        return;
    }

    low_freq = malloc (filtered_count * sizeof (float));
    high_freq = malloc (filtered_count * sizeof (float));

    if (flags & MONO_DATA)
        for (i = 0; i < filtered_count; ++i, ++samples) {
            float filter_sum = 0.0;

            for (j = 0; j < FILTER_LENGTH; ++j)
                filter_sum += samples [j] * lp_filter [j];

            high_freq [i] = samples [FILTER_LENGTH >> 1] - filter_sum;
            low_freq [i] = filter_sum;
        }
    else
        for (i = 0; i < filtered_count; ++i, samples += 2) {
            float filter_sum = 0.0;

            for (j = 0; j < FILTER_LENGTH; ++j)
                filter_sum += (samples [j * 2] + samples [j * 2 + 1]) * lp_filter [j];

            high_freq [i] = samples [FILTER_LENGTH & ~1] + samples [FILTER_LENGTH] - filter_sum;
            low_freq [i] = filter_sum;
        }

    for (i = filtered_count - 1; i; --i)
        low_freq [i] -= low_freq [i - 1];

    low_freq [0] = low_freq [1];

    win_average_buffer (low_freq, filtered_count);
    win_average_buffer (high_freq, filtered_count);

    for (i = 0; i < filtered_count; ++i) {
        float ratio = high_freq [i] / low_freq [i];

        if (ratio > 100.0)
            ratio = 40.0;
        else if (ratio < 0.01)
            ratio = -40.0;
        else
            ratio = log10 (ratio) * 20.0;

        ratio = floor (ratio * 100.0 + 0.5);

        if (ratio > 1024) ratio = 1024;
        else if (ratio < min_value) ratio = min_value;

        values [i + (FILTER_LENGTH >> 1)] = ratio;
    }

    for (i = 0; i < sample_count; ++i) {
        if (i < (FILTER_LENGTH >> 1))
            values [i] = values [FILTER_LENGTH >> 1];
        else if (i >= filtered_count)
            values [i] = values [filtered_count - 1];
    }

    free (low_freq);
    free (high_freq);
}

#endif

// Given an array of integer data (in shorts), find the linear function that most closely
// represents it (based on minimum sum of absolute errors). This is returned as the double
// precision initial & final Y values of the best-fit line. The function can also optionally
// compute and return a maximum error value (as a short). Note that the ends of the resulting
// line may fall way outside the range of input values, so some sort of clipping may be
// needed.

static void best_floating_line (short *values, int num_values, double *initial_y, double *final_y, short *max_error)
{
    double left_sum = 0.0, right_sum = 0.0, center_x = (num_values - 1) / 2.0, center_y, m;
    int i;

    for (i = 0; i < num_values >> 1; ++i) {
        right_sum += values [num_values - i - 1];
        left_sum += values [i];
    }

    if (num_values & 1) {
        right_sum += values [num_values >> 1] * 0.5;
        left_sum += values [num_values >> 1] * 0.5;
    }

    center_y = (right_sum + left_sum) / num_values;
    m = (right_sum - left_sum) / ((double) num_values * num_values) * 4.0;

    if (initial_y)
        *initial_y = center_y - m * center_x;

    if (final_y)
        *final_y = center_y + m * center_x;

    if (max_error) {
        double max = 0.0;

        for (i = 0; i < num_values; ++i)
            if (fabs (values [i] - (center_y + (i - center_x) * m)) > max)
                max = fabs (values [i] - (center_y + (i - center_x) * m));

        *max_error = (short) floor (max + 0.5);
    }
}
