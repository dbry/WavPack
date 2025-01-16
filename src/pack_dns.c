////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//                Copyright (c) 1998 - 2025 David Bryant                  //
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

#define FILTER_LENGTH 15
#define WINDOW_LENGTH 101
#define SETTLE_DISTANCE ((WINDOW_LENGTH >> 1) + (FILTER_LENGTH >> 1) + 1)
#define MIN_BLOCK_SAMPLES 16

static void generate_dns_values (const int32_t *samples, int sample_count, int num_chans, int sample_rate, short *values, short min_value);
static void best_floating_line (short *values, int num_values, double *initial_y, double *final_y, short *max_error);

void dynamic_noise_shaping (WavpackStream *wps, const int32_t *buffer, int shortening_allowed)
{
    int num_chans = (wps->wphdr.flags & MONO_DATA) ? 1 : 2;
    int32_t sample_count = wps->wphdr.block_samples;
    int sample_rate = wps->wpc->config.sample_rate;
    short min_value = -512;

    if (wps->bits < 768) {
        min_value = -768 - ((768 - wps->bits) * 16 / 25);

        if (min_value < -896)   // -0.875 is minimum usable shaping
            min_value = -896;
    }
    else
        min_value = -768;       // at 3 bits/sample and up minimum shaping is -0.75

    if (sample_count > wps->dc.shaping_samples) {
        int existing_values_to_use = wps->dc.shaping_samples > SETTLE_DISTANCE ? wps->dc.shaping_samples - SETTLE_DISTANCE : 0;
        int new_values_to_use = sample_count - existing_values_to_use, values_to_skip;
        int new_values_to_generate = new_values_to_use + SETTLE_DISTANCE;
        short *new_values = malloc (sample_count * sizeof (short));

        if (new_values_to_generate > sample_count)
            new_values_to_generate = sample_count;

        values_to_skip = sample_count - new_values_to_generate;

        generate_dns_values (buffer + values_to_skip * num_chans, new_values_to_generate, num_chans, sample_rate, new_values, min_value);
        memcpy (wps->dc.shaping_data + existing_values_to_use, new_values + (new_values_to_generate - new_values_to_use), new_values_to_use * sizeof (short));
        wps->dc.shaping_samples = sample_count;
        free (new_values);
    }

    if (wps->wpc->wvc_flag) {
        int max_allowed_error = 1000000 / wps->wpc->ave_block_samples;
        short max_error, trial_max_error;
        double initial_y, final_y;

        if (max_allowed_error < 128)
            max_allowed_error = 128;

        best_floating_line (wps->dc.shaping_data, sample_count, &initial_y, &final_y, &max_error);

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

        if (sample_count != wps->wphdr.block_samples)
            wps->wphdr.block_samples = sample_count;

        if (wps->wpc->wvc_flag) {
            wps->dc.shaping_acc [0] = wps->dc.shaping_acc [1] = (int32_t) floor (initial_y * 65536.0 + 0.5);

            wps->dc.shaping_delta [0] = wps->dc.shaping_delta [1] =
                (int32_t) floor ((final_y - initial_y) / (sample_count - 1) * 65536.0 + 0.5);

            wps->dc.shaping_acc [0] -= wps->dc.shaping_delta [0];
            wps->dc.shaping_acc [1] -= wps->dc.shaping_delta [1];
            wps->dc.shaping_array = NULL;
        }
        else
            wps->dc.shaping_array = wps->dc.shaping_data;
    }
    else
        wps->dc.shaping_array = wps->dc.shaping_data;
}

// Given a buffer of floating values, apply a simple box filter of specified half width
// (total filter width is always odd) to determine the averaged magnitude at each point.
// This is a true RMS average. For the ends, we use only the visible samples.

static void win_average_buffer (float *samples, int sample_count, int half_width)
{
    float *output = malloc (sample_count * sizeof (float));
    double sum = 0.0;
    int m = 0, n = 0;
    int i, j, k;

    for (i = 0; i < sample_count; ++i) {
        k = i + half_width + 1;
        j = i - half_width;

        if (k > sample_count) k = sample_count;
        if (j < 0) j = 0;

        while (m < j) {
            if ((sum -= samples [m] * samples [m]) < 0.0) sum = 0.0;
            m++;
        }

        while (n < k) {
            sum += samples [n] * samples [n];
            n++;
        }

        output [i] = (float) sqrt (sum / (n - m));
    }

    memcpy (samples, output, sample_count * sizeof (float));
    free (output);
}

// Generate the shaping values for the specified buffer of stereo or mono samples,
// one shaping value output for each sample (or stereo pair of samples). This is
// calculated by filtering the audio at fs/6 (7350 Hz at 44.1 kHz) and comparing
// the averaged levels above and below that frequency. The output shaping values
// are nominally in the range of +/-1024, with 1024 indicating first-order HF boost
// shaping and -1024 for similar LF boost. However, since -1024 would result in
// infinite DC boost (not useful) a "min_value" is passed in. A output value of
// zero represents no noise shaping. For stereo input data the channels are summed
// for the calculation and the output is still just mono. Note that at the ends of
// the buffer the values diverge from true because not all the required source
// samples are visible. Use this formula to calculate the number of samples
// required for this to "settle":
//
//  int settle_distance = (WINDOW_LENGTH >> 1) + (FILTER_LENGTH >> 1) + 1;

static void generate_dns_values (const int32_t *samples, int sample_count, int num_chans, int sample_rate, short *values, short min_value)
{
    float dB_offset = 0.0, dB_scaler = 100.0, max_dB, min_dB, max_ratio, min_ratio;
    int filtered_count = sample_count - FILTER_LENGTH + 1, i;
    float *low_freq, *high_freq;

    memset (values, 0, sample_count * sizeof (values [0]));

    if (filtered_count <= 0)
        return;

    low_freq = malloc (filtered_count * sizeof (float));
    high_freq = malloc (filtered_count * sizeof (float));

    // First, directly calculate the lowpassed audio using the 15-tap filter. This is
    // a basic sinc with Hann windowing (for a fast transition) and because the filter
    // is set to exactly fs/6, some terms are zero (which we can skip). Also, because
    // it's linear-phase and has an odd number of terms, we can just subtract the LF
    // result from the original to get the HF values.

    if (num_chans == 1)
        for (i = 0; i < filtered_count; ++i, ++samples) {
            float filter_sum = (float) (
                (samples [0] + samples [14]) *  0.00150031 +
                (samples [2] + samples [12]) * -0.01703392 +
                (samples [3] + samples [11]) * -0.03449186 +
                (samples [5] + samples [ 9]) *  0.11776258 +
                (samples [6] + samples [ 8]) *  0.26543272 +
                         samples [7]         *  0.33366033
            );

            high_freq [i] = samples [FILTER_LENGTH >> 1] - filter_sum;
            low_freq [i] = filter_sum;
        }
    else
        for (i = 0; i < filtered_count; ++i, samples += 2) {
            float filter_sum = (float) (
                (samples [ 0] + samples [ 1] + samples [28] + samples [29]) *  0.00150031 +
                (samples [ 4] + samples [ 5] + samples [24] + samples [25]) * -0.01703392 +
                (samples [ 6] + samples [ 7] + samples [22] + samples [23]) * -0.03449186 +
                (samples [10] + samples [11] + samples [18] + samples [19]) *  0.11776258 +
                (samples [12] + samples [13] + samples [16] + samples [17]) *  0.26543272 +
                               (samples [14] + samples [15])                *  0.33366033
            );

            high_freq [i] = samples [FILTER_LENGTH & ~1] + samples [FILTER_LENGTH] - filter_sum;
            low_freq [i] = filter_sum;
        }

    // Apply a simple first-order "delta" filter to the lowpass because frequencies below fs/6
    // become progressively less important for our purposes as the decorrelation filters make
    // those frequencies less and less relevant. Note that after all this filtering, the
    // magnitude level of the high frequency array will be 8.7 dB greater than the low frequency
    // array when the filters are presented with pure white noise (determined empirically).

    for (i = filtered_count - 1; i; --i)
        low_freq [i] -= low_freq [i - 1];

    low_freq [0] = low_freq [1];    // simply duplicate for the "unknown" sample

    // Next we determine the averaged (absolute) levels for each sample using a box filter.

    win_average_buffer (low_freq, filtered_count, WINDOW_LENGTH >> 1);
    win_average_buffer (high_freq, filtered_count, WINDOW_LENGTH >> 1);

    // Use the sample rate to calculate the desired offset:
    //   <= 22,050 Hz: we use offset of -8.7 dB which means the noise-shaping matches
    //                 the analysis results (i.e., white noise input gives zero shaping)
    //   >= 44,100 Hz: we use offset of 0 dB which biases the shaping to the positive
    //                 side (i.e., white noise input gives positive shaping, -s0.87)
    //   sampling rate values between 22-kHz and 44-kHz use an interpolated offset

    if (sample_rate < 44100) {
        if (sample_rate > 22050)
            dB_offset = (sample_rate - 44100) / 2534.0F;
        else
            dB_offset = -8.7F;
    }

    // calculate the minimum and maximum ratios that won't be clipped so that we only
    // have to compute the logarithm when needed

    max_dB = 1024 / dB_scaler - dB_offset;
    min_dB = min_value / dB_scaler - dB_offset;
    max_ratio = (float) pow (10.0, max_dB / 20.0);
    min_ratio = (float) pow (10.0, min_dB / 20.0);

    for (i = 0; i < filtered_count; ++i)
        if (high_freq [i] > 1.0 && low_freq [i] > 1.0) {
            float ratio = high_freq [i] / low_freq [i];
            int shaping_value;

            if (ratio >= max_ratio)
                shaping_value = 1024;
            else if (ratio <= min_ratio)
                shaping_value = min_value;
            else
                shaping_value = (int) floor ((log10 (ratio) * 20.0 + dB_offset) * dB_scaler + 0.5);

            values [i + (FILTER_LENGTH >> 1)] = shaping_value;
        }

    // finally, copy the value at each end into the 7 outermost positions on each side

    for (i = 0; i < FILTER_LENGTH >> 1; ++i)
        values [i] = values [FILTER_LENGTH >> 1];

    for (i = filtered_count + (FILTER_LENGTH >> 1); i < sample_count; ++i)
        values [i] = values [(FILTER_LENGTH >> 1) + filtered_count - 1];

    free (low_freq);
    free (high_freq);
}

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
