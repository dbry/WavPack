////////////////////////////////////////////////////////////////////////////
//                           **** MODULATOR ****                          //
//                       Simple PCM to DSD Modulator                      //
//                     Copyright (c) 2026 David Bryant                    //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// modulator.c

#include "modulator.h"

#ifdef NO_PRUNING
#undef NO_PRUNING
#define NO_PRUNING  1
#else
#define NO_PRUNING  0
#endif

// #define TEST_RESYNCING   // regularly reset modulator to test re-syncing
// #define ALLOW_DRIFT      // allow drift even when tracking alignment (for testing)
#define ENABLE_CLIPPING
#define ENABLE_DITHER

#define DEPTH_TERM  2       // tree search is terminated at this depth

/////////////////////////////////////////////////////////////////////////////////////////

// Higher-order noise shaping filters (i.e., above 2nd-order) become unstable,
// especially with higher levels. Increasing look-ahead helps, but we still need
// to soft-clip and dynamically reduce the order of the noise-shaping filter above
// +3.1 dB SACD. This is the maximum allowed level, although some DSD files go
// beyond this, and of course this code is designed to produce reasonable results
// with arbitrary PCM input.

#define SOFT_CLIP   0.72F               // we soft-clip above +3.1 dB SACD level
#define HARD_CLIP   0.86F               // we hard clip full-scale PCM to here

static void init_filter (int numTaps, float *filter, double fraction);
static inline double apply_filter (float *A, float *B, int num_taps);
static inline double tpdf_dither (uint32_t *generator);

#ifdef STATISTICS
static double best_sample (ModulatorChannel *cxt, const float *samples, double order, double *error, int depth);
#else
static double best_sample (const float *samples, double order, double *error, int depth);
#endif

Modulate *modulateInit (int numChannels, int level, int flags)
{
    Modulate *cxt = calloc (1, sizeof (Modulate));
    uint32_t seed = 0x31415926;
    float **upsample_filters;
    void *decimator = NULL;

    if (flags & MODULATOR_ALIGN_EMBEDDED)
        decimator = decimateDSDinit (0, 0);

    cxt->numChannels = numChannels;
    upsample_filters = calloc (NUM_FILTERS, sizeof (float*));

    for (int f = 0; f < NUM_FILTERS; ++f) {
        upsample_filters [f] = calloc (US_TAPS, sizeof (float));
        init_filter (US_TAPS, upsample_filters [f], (f + 0.5) / NUM_FILTERS);
    }

    cxt->channels = calloc (numChannels, sizeof (ModulatorChannel));

    for (int c = 0; c < numChannels; ++c) {
        cxt->channels [c].chan = c;

        modulateSetLevel (cxt, c, level);
        cxt->channels [c].flags = flags;

        cxt->channels [c].upsample_filters = upsample_filters;

        cxt->channels [c].source_buffer = calloc (NUM_SAMPLES, sizeof (float));
        cxt->channels [c].source_buffer_head = US_TAPS / 2;
        cxt->channels [c].source_buffer_tail = 0;

        cxt->channels [c].upsample_buffer = calloc (NUM_SAMPLES, sizeof (float));
        cxt->channels [c].upsample_buffer_fill = cxt->channels [c].upsample_buffer_conv = 0;
        cxt->channels [c].upsample_buffer_tail = 4;

        cxt->channels [c].dsd_buffer = calloc (NUM_SAMPLES, sizeof (unsigned char));
        cxt->channels [c].decimator = decimator;

        cxt->channels [c].tpdf_generator = seed;
        seed = (seed << 1) | ((seed >> 31) & 1);
    }

    if ((flags & MODULATE_MULTITHREADED) && numChannels > 1)
        cxt->workers = workersInit (numChannels - 1);

    return cxt;
}

void modulateSetLevel (Modulate *cxt, int channel_number, int level)
{
    ModulatorChannel *cptr = cxt->channels + channel_number;

    if (channel_number < cxt->numChannels) {
        if (level > 5)
            level = 5;
        else if (level < 0)
            level = 0;

#ifdef ENABLE_DITHER
        if (level)
            cptr->dither_level = (32 >> level) / 262144.0;
        else
            cptr->dither_level = 0.0;
#endif

        if (cptr->level != level) {
            cptr->level = level;
            cptr->plus_error_count = cptr->minus_error_count = cptr->large_error_count = cptr->error_sum = 0;
            cptr->error_feedback [0] = cptr->error_feedback [1] = cptr->error_feedback [2] = 0.0;
#if NS_TAPS == 4
            cptr->error_feedback [3] = 0.0;
#endif
        }
    }
}

void modulateSetAlignment (Modulate *cxt, int channel_number, int enable)
{
    ModulatorChannel *cptr = cxt->channels + channel_number;

    if (channel_number < cxt->numChannels && cptr->decimator && enable == !(cptr->flags & MODULATOR_ALIGN_EMBEDDED)) {
        cptr->plus_error_count = cptr->minus_error_count = cptr->large_error_count = cptr->error_sum = 0;
        cptr->flags ^= MODULATOR_ALIGN_EMBEDDED;

        if (enable)
            cptr->delayed_alignment = DSD_DELAY;
    }
}

static int modulateProcessChannelJob (void *ptr, void *sync_not_used);

int modulateProcess (Modulate *cxt, const float *input, int numInputFrames, unsigned char *mod_output, unsigned char *emb_output)
{
    for (int c = 0; c < cxt->numChannels; ++c) {
        cxt->channels [c].input = input + c;
        cxt->channels [c].numInputFrames = numInputFrames;
        if (mod_output) cxt->channels [c].mod_output = mod_output + c;
        if (emb_output) cxt->channels [c].emb_output = emb_output + c;
        cxt->channels [c].stride = cxt->numChannels;

        if (cxt->workers)
            workersEnqueueJob (cxt->workers, modulateProcessChannelJob, cxt->channels + c,
                c < cxt->numChannels - 1 ? WaitForAvailableWorkerThread : DontUseWorkerThread);
        else
            modulateProcessChannelJob (cxt->channels + c, NULL);
    }

    if (cxt->workers)
        workersWaitAllJobs (cxt->workers);

    return cxt->channels [0].numOutputFrames;
}

static unsigned char xor_images [] = {
    0x00, 0x04, 0x08, 0x0C,
    0x10, 0x18,
    0x20,
    0x30,
};

#define NUM_XORS (sizeof (xor_images) / sizeof (xor_images [0]))

#ifdef ENABLE_CLIPPING
#define CLIP_VALUE(value) ((value) > 0.72 ? 1.0 - (0.0784 / ((value) - 0.44)) : (value))
#else
#define CLIP_VALUE(value) (value)
#endif

#define ORDER_TO_USABLE(order) (floor (order) + sqrt ((order) - floor (order)))

static int modulateProcessChannelJob (void *ptr, void *sync_not_used)
{
    ModulatorChannel *cxt = ptr;
    cxt->numOutputFrames = 0;

    if (cxt->flags & MODULATOR_FLUSHED)
        cxt->numInputFrames = 0;

    if (cxt->numInputFrames < 0) {
        int samples_to_add = US_TAPS / 2 + (MAX_DEPTH + 3) / 8 + cxt->delayed_samples;

        if (cxt->source_buffer_head + samples_to_add >= NUM_SAMPLES) {
            int samples_to_move = cxt->source_buffer_head - cxt->source_buffer_tail;

            memmove (cxt->source_buffer, cxt->source_buffer + cxt->source_buffer_tail, samples_to_move * sizeof (float));
            cxt->source_buffer_head -= cxt->source_buffer_tail;
            cxt->source_buffer_tail -= cxt->source_buffer_tail;
        }

        for (int i = 0; i < samples_to_add; ++i)
            cxt->source_buffer [cxt->source_buffer_head + i] = cxt->source_buffer [cxt->source_buffer_head - 1];

        cxt->source_buffer_head += samples_to_add;
        cxt->flags |= MODULATOR_FLUSHED;
    }

    while (1) {
        if (cxt->source_buffer_tail + US_TAPS > cxt->source_buffer_head) {      // if we don't have enough source data to do anything...
            if (cxt->numInputFrames > 0) {                                      // if we have source samples still...
                if (cxt->source_buffer_head == NUM_SAMPLES) {                   // if the source buffer is full, shift it left
                    int samples_to_move = cxt->source_buffer_head - cxt->source_buffer_tail;

                    memmove (cxt->source_buffer, cxt->source_buffer + cxt->source_buffer_tail, samples_to_move * sizeof (float));
                    cxt->source_buffer_head -= cxt->source_buffer_tail;
                    cxt->source_buffer_tail -= cxt->source_buffer_tail;
                }

#ifdef STATISTICS
                if (*cxt->input < cxt->sample_min) cxt->sample_min = *cxt->input;
                if (*cxt->input > cxt->sample_max) cxt->sample_max = *cxt->input;
#endif

                cxt->source_buffer [cxt->source_buffer_head] = *cxt->input;
                cxt->input += cxt->stride;
                cxt->source_buffer_head++;
                cxt->numInputFrames--;
            }
            else
                break;
        }
        else {  // else we do have enough source data to generate 8 upsamples

            if (!(cxt->flags & MODULATOR_PREFILLED)) {
                for (int i = 0; i < US_TAPS / 2; ++i)
                    cxt->source_buffer [i] = cxt->source_buffer [US_TAPS / 2];

                cxt->flags |= MODULATOR_PREFILLED;
            }

            if (cxt->upsample_buffer_fill + NUM_FILTERS >= NUM_SAMPLES) {       // if upsample buffer is full...
                int move_area_start = cxt->upsample_buffer_conv - MAX_DEPTH;
                int samples_to_move = cxt->upsample_buffer_fill - move_area_start;

                memmove (cxt->upsample_buffer, cxt->upsample_buffer + move_area_start, samples_to_move * sizeof (float));
                memmove (cxt->dsd_buffer, cxt->dsd_buffer + move_area_start, samples_to_move * sizeof (unsigned char));
                cxt->upsample_buffer_fill -= move_area_start;
                cxt->upsample_buffer_conv -= move_area_start;
                cxt->upsample_buffer_tail -= move_area_start;
            }

            // generate 8 upsamples, and soft-clip if over +3.1 dB SACD
            float *upsample_ptr = cxt->upsample_buffer + cxt->upsample_buffer_fill;
            unsigned char *dsd_ptr = cxt->dsd_buffer + cxt->upsample_buffer_fill;
            float *source_ptr = cxt->source_buffer + cxt->source_buffer_tail;
            int dsd_data = (((int32_t)(source_ptr [7] * 8388608.0) & 0xff) << 8) |
                ((int32_t)(source_ptr [8] * 8388608.0) & 0xff);

            for (int f = 0; f < NUM_FILTERS; ++f) {
                double result = apply_filter (source_ptr, cxt->upsample_filters [f], US_TAPS);

                if (cxt->dither_level != 0.0)
                    result += tpdf_dither (&cxt->tpdf_generator) * cxt->dither_level;

#ifdef ENABLE_CLIPPING
                if (fabs (result) > SOFT_CLIP) {
                    if (result >= 1.00)
                        *upsample_ptr++ = HARD_CLIP;
                    else if (result > 0.0)
                        *upsample_ptr++ = 1.0 - (0.0784 / (result - 0.44));
                    else if (result <= -1.00)
                        *upsample_ptr++ = -HARD_CLIP;
                    else
                        *upsample_ptr++ = -1.0 - (0.0784 / (result + 0.44));
                }
                else
#endif
                    *upsample_ptr++ = result;

                *dsd_ptr++ = (dsd_data >> (11 - f)) & 0x1;      // b.0 of dsd_buffer is embedded DSD

#ifdef STATISTICS
                if (upsample_ptr [-1] < cxt->upsample_min) cxt->upsample_min = upsample_ptr [-1];
                if (upsample_ptr [-1] > cxt->upsample_max) cxt->upsample_max = upsample_ptr [-1];
#endif
            }

            cxt->upsample_buffer_fill += NUM_FILTERS;
            cxt->source_buffer_tail++;
        }

        // do the actual SDM here, assuming we have sufficient samples for lookahead depth (and we're not idle)
        while (cxt->upsample_buffer_fill - cxt->upsample_buffer_conv > MAX_DEPTH) if (cxt->level) {
            float *sample_ptr = cxt->upsample_buffer + cxt->upsample_buffer_conv, sample_max = 0.0, order = 2.0;
            unsigned char *dsd_ptr = cxt->dsd_buffer + cxt->upsample_buffer_conv;
            static int max_depth_per_level [] = { 0, 2, 6, 9, 15, 19 };
            int max_depth = max_depth_per_level [cxt->level];
            int depth = 2;

            if (cxt->level > 1)
                for (int i = cxt->upsample_buffer_conv < max_depth ? -cxt->upsample_buffer_conv : -max_depth; i <= max_depth; ++i)
                    sample_max = fmax (sample_max, fabs (sample_ptr [i]));

            switch (cxt->level) {
                case 2:
                    order = 2.8;
                    depth = 4;                                  // 4

                    if (sample_max > 0.50) {
                        depth++;                                // 5

                        if (sample_max > 0.65) {
                            depth++;                            // 6

                            if (sample_max > 0.74)
                                order -= (sample_max - 0.74) * 3.0;
                        }
                    }

                    break;

                case 3:
                    order = 3.0;
                    depth = 5;                                  // 5

                    if (sample_max > 0.40) {
                        depth++;                                // 6

                        if (sample_max > 0.50) {
                            depth++;                            // 7

                            if (sample_max > 0.60) {
                                depth++;                        // 8

                                if (sample_max > 0.70) {
                                    depth++;                    // 9

                                    if (sample_max > 0.75)
                                        order -= (sample_max - 0.75) * 3.0;
                                }
                            }
                        }
                    }

                    break;

                case 4:
                    order = 3.8;
                    depth = 10;                                 // 10

                    if (sample_max > 0.33) {
                        depth++;                                // 11

                        if (sample_max > 0.50) {
                            depth++;                            // 12

                            if (sample_max > 0.56) {
                                depth++;                        // 13

                                if (sample_max > 0.64) {
                                    depth++;                    // 14

                                    if (sample_max > 0.69) {
                                        depth++;                // 15

                                        if (sample_max > 0.72)
                                            order -= (sample_max - 0.72) * 5.0;
                                    }
                                }
                            }
                        }
                    }

                    break;

                case 5:
                    order = 4.0;
                    depth = 12;                                         // 12

                    if (sample_max > 0.38) {
                        depth++;                                        // 13

                        if (sample_max > 0.47) {
                            depth++;                                    // 14

                            if (sample_max > 0.50) {
                                depth++;                                // 15

                                if (sample_max > 0.61) {
                                    depth++;                            // 16

                                    if (sample_max > 0.66) {
                                        depth++;                        // 17

                                        if (sample_max > 0.68) {
                                            depth++;                    // 18

                                            if (sample_max > 0.70) {
                                                depth++;                // 19

                                                if (sample_max > 0.72)
                                                    order -= (sample_max - 0.72) * 3.0;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    break;

                default:
                    ;
            }

#ifdef TEST_RESYNCING
            // test synch by periodically resetting DSD encoding
            double seconds = cxt->total_samples / 2822400.0;
            if ((seconds / 2.0) - floor (seconds / 2.0) == (double) cxt->chan / cxt->stride) {
                cxt->error_feedback [0] = cxt->error_feedback [1] = cxt->error_feedback [2] = 0.0;
#if NS_TAPS == 4
                cxt->error_feedback [3] = 0.0;
#endif
            }
#endif

#ifdef STATISTICS
            float outsample = best_sample (cxt, sample_ptr, ORDER_TO_USABLE (order), cxt->error_feedback, depth);
            double filtered_error = outsample - *sample_ptr, unfiltered_error = cxt->error_feedback [0];

            if (depth > cxt->max_depth_seen) {
                cxt->max_depth_seen = depth;
                fprintf (stderr, "ch %d: max depth = %d at %.6f secs, max sample = %.3f, order = %.3f\n",
                    cxt->chan, depth, cxt->total_samples / 2822400.0, sample_max, order);
            }

            *dsd_ptr |= ((outsample > 0.0) & 1) << 1;           // b.1 of dsd_buffer is calculated DSD sample

            if (cxt->min_order == 0.0 || order < cxt->min_order) cxt->min_order = order;
            if (order > cxt->max_order) cxt->max_order = order;

            if (outsample == cxt->last_sample) {
                if (++(cxt->run_count) == 100) {
                    fprintf (stderr, "\n*** ch %d: value %2g, terminating run of %d, ending at %.6f secs (%.3f), max sample = %.3f, order = %.3f, depth = %d ***\n\n",
                        cxt->chan, outsample, cxt->run_count, cxt->total_samples / 2822400.0, CLIP_VALUE (cxt->total_samples / 169344000.0), sample_max, order, depth);
                    exit (1);
                }

                if (sample_max > cxt->last_sample_peak)
                    cxt->last_sample_peak = sample_max;
            }
            else {
                if (cxt->run_count > cxt->max_run_count) {
                    cxt->max_run_count = cxt->run_count;
                    if (cxt->run_count >= 20)
                        fprintf (stderr, "ch %d: value %2g, terminating run of %d, ending at %.6f secs (%.3f), max sample = %.3f, order = %.3f, depth = %d\n",
                            cxt->chan, outsample, cxt->run_count, cxt->total_samples / 2822400.0, CLIP_VALUE (cxt->total_samples / 169344000.0), sample_max, order, depth);
                }

                cxt->last_sample_peak = sample_max;
                cxt->last_sample = outsample;
                cxt->run_count = 1;
            }

            cxt->rms_filtered_error += filtered_error * filtered_error;
            cxt->rms_unfiltered_error += unfiltered_error * unfiltered_error;
            if (unfiltered_error > cxt->max_unfiltered_error) cxt->max_unfiltered_error = unfiltered_error;
            if (unfiltered_error < cxt->min_unfiltered_error) cxt->min_unfiltered_error = unfiltered_error;
            if (filtered_error > cxt->max_filtered_error) cxt->max_filtered_error = filtered_error;
            if (filtered_error < cxt->min_filtered_error) cxt->min_filtered_error = filtered_error;
#else
            cxt->last_sample = best_sample (sample_ptr, ORDER_TO_USABLE (order), cxt->error_feedback, depth);
            *dsd_ptr |= ((cxt->last_sample > 0.0) & 1) << 1;           // b.1 of dsd_buffer is calculated DSD sample
#endif

            cxt->upsample_buffer_conv++;
            cxt->total_samples++;
        }
        else {
            unsigned char *dsd_ptr = cxt->dsd_buffer + cxt->upsample_buffer_conv++;
            *dsd_ptr |= (cxt->total_samples++ & 1) << 1;
        }

        // while we have whole bytes of DSD data ready, output it
        while (cxt->upsample_buffer_tail + 8 <= cxt->upsample_buffer_conv) {
            unsigned char dsd_embedded = 0, dsd_calculated = 0;
            int bcount = 8;

            while (bcount--) {
                dsd_calculated = (dsd_calculated << 1) | ((cxt->dsd_buffer [cxt->upsample_buffer_tail] & 0x2) >> 1);
                dsd_embedded = (dsd_embedded << 1) | (cxt->dsd_buffer [cxt->upsample_buffer_tail++] & 0x1);
            }

            cxt->dsd_calculated_buffer [cxt->delayed_samples] = dsd_calculated;
            cxt->dsd_embedded_buffer [cxt->delayed_samples++] = dsd_embedded;

            if (cxt->delayed_samples == DSD_DELAY) {
                if (cxt->mod_output) {
                    *cxt->mod_output = cxt->dsd_calculated_buffer [0];
                    cxt->mod_output += cxt->stride;
                }

                if (cxt->emb_output) {
                    *cxt->emb_output = cxt->dsd_embedded_buffer [0];
                    cxt->emb_output += cxt->stride;
                }

#ifndef WRITE_ERROR_CHAN
                cxt->numOutputFrames++;
#endif

                if ((cxt->flags & MODULATOR_ALIGN_EMBEDDED) && (!cxt->delayed_alignment || !cxt->delayed_alignment--)) {
                    int32_t calculated_sum = 0, embedded_sum = 0, average_sum = 0, closest_average = 0;
                    unsigned char dsd_merged_buffer [DSD_DELAY];
                    int transition_byte = (DSD_DELAY - 1) / 2;

                    for (int i = 0; i < HISTORY_BYTES; ++i) {
                        calculated_sum += decimateSingleDSDsample (cxt->decimator, cxt->dsd_calculated_buffer + i);
                        embedded_sum += decimateSingleDSDsample (cxt->decimator, cxt->dsd_embedded_buffer + i);
                    }

                    average_sum = (calculated_sum + embedded_sum + HISTORY_BYTES) / (HISTORY_BYTES * 2);
                    memcpy (dsd_merged_buffer, cxt->dsd_calculated_buffer, transition_byte);
                    memcpy (dsd_merged_buffer + transition_byte + 1, cxt->dsd_embedded_buffer + transition_byte + 1, transition_byte);

                    for (int xor_index = 0; xor_index < NUM_XORS; xor_index++) {
                        int32_t merged_sum = 0, merged_average;

                        dsd_merged_buffer [transition_byte] = (cxt->dsd_calculated_buffer [transition_byte] & 0xF0) | (cxt->dsd_embedded_buffer [transition_byte] & 0x0F);
                        dsd_merged_buffer [transition_byte] ^= xor_images [xor_index];

                        for (int i = 0; i < HISTORY_BYTES; ++i)
                            merged_sum += decimateSingleDSDsample (cxt->decimator, dsd_merged_buffer + i);

                        merged_average = (merged_sum + 3) / HISTORY_BYTES;

                        if (!xor_index || abs (merged_average - average_sum) < abs (closest_average - average_sum))
                            closest_average = merged_average;
                    }

                    cxt->error_sum += closest_average - average_sum;

                    if (closest_average < average_sum)
                        cxt->minus_error_count++;
                    else if (closest_average > average_sum)
                        cxt->plus_error_count++;

                    if (abs (closest_average - average_sum) > 83656)
                        cxt->large_error_count++;

                    // The target is to do a worst-case 180° adjustment during a single 1/75 second sector. This is 4704 DXD
                    // samples (assuming DSD64) and since we adjust every 64 DXD samples the adjustment amount is +/- 64 / 4704
                    // every itteration through here. Note that adding a total of 1.0 to the error feedback results in a phase
                    // shift of 180°. This amount is equally distributed among the available error terms and inversely scaled
                    // by the sum of how the filter coefficients apply over time to that term.

                    if (cxt->plus_error_count + cxt->minus_error_count >= 64 && cxt->plus_error_count != cxt->minus_error_count) {
#ifndef ALLOW_DRIFT
                        float average_error = (double) cxt->error_sum / (cxt->plus_error_count + cxt->minus_error_count);
                        double correction_scale = (cxt->plus_error_count > cxt->minus_error_count ? +64.0 : -64.0) / 4704.0;

                        // when we're close the center, take smaller steps (PID)

                        if (!cxt->large_error_count && fabs (average_error) < 25600)
                            correction_scale *= fabs (average_error) / 25600;

                        if (cxt->level == 1) {          // level 1 (2 terms)
                            cxt->error_feedback [0] += correction_scale / 2.0 / -1.0;
                            cxt->error_feedback [1] += correction_scale / 2.0 /  1.0;
                        }
                        else if (cxt->level <= 3) {     // levels 2 & 3 (3 terms)
                            cxt->error_feedback [0] += correction_scale / 3.0 / -1.0;
                            cxt->error_feedback [1] += correction_scale / 3.0 /  2.0;
                            cxt->error_feedback [2] += correction_scale / 3.0 / -1.0;
                        }
                        else {                          // levels 4 & 5 (4 terms)
                            cxt->error_feedback [0] += correction_scale / 4.0 / -1.0;
                            cxt->error_feedback [1] += correction_scale / 4.0 /  3.0;
                            cxt->error_feedback [2] += correction_scale / 4.0 / -3.0;
                            cxt->error_feedback [3] += correction_scale / 4.0 /  1.0;
                        }
#endif
                        cxt->plus_error_count = cxt->minus_error_count = cxt->large_error_count = cxt->error_sum = 0;
                    }

#ifdef WRITE_ERROR_CHAN
                    if (cxt->chan == WRITE_ERROR_CHAN) {
                        int32_t error = closest_average - average_sum;
                        putchar (error & 0xff);
                        putchar ((error >> 8) & 0xff);
                        putchar ((error >> 16) & 0xff);
                    }
#endif
                }

                memmove (cxt->dsd_calculated_buffer, cxt->dsd_calculated_buffer + 1, DSD_DELAY - 1);
                memmove (cxt->dsd_embedded_buffer, cxt->dsd_embedded_buffer + 1, DSD_DELAY - 1);
                cxt->delayed_samples--;
            }
        }
    }

    return 0;
}

void modulateFree (Modulate *cxt)
{
#ifdef STATISTICS
    fprintf (stderr, "%ld total samples, %.2f seconds\n\n", cxt->channels [0].total_samples, cxt->channels [0].total_samples / 2822400.0);

    for (int c = 0; c < cxt->numChannels; ++c) {
        ModulatorChannel *chan = cxt->channels + c;
        fprintf (stderr, "chan %d: RMS noise = %.2f dB unfiltered, %.2f dB filtered, max run %d, order %.3f to %.3f\n", c,
            log10 (chan->rms_unfiltered_error / chan->total_samples * 2.0) * 10.0,
            log10 (chan->rms_filtered_error / chan->total_samples * 2.0) * 10.0,
            chan->max_run_count, chan->min_order, chan->max_order);
        if (chan->called_best_sample)
            fprintf (stderr, "        alt checked %.1f%%, alt used %.1f%%, %.1f ave leaves\n",
                chan->checked_alt_sample * 100.0 / chan->called_best_sample,
                chan->used_alt_sample * 100.0 / chan->called_best_sample,
                (double) chan->leaves / chan->called_best_sample);
        fprintf (stderr, "        PCM input (unclipped) range = %.3f to %.3f\n", chan->sample_min, chan->sample_max);
        fprintf (stderr, "          upsampled (clipped) range = %.3f to %.3f\n", chan->upsample_min, chan->upsample_max);
        fprintf (stderr, "             unfiltered error range = %.3f to %.3f\n", chan->min_unfiltered_error, chan->max_unfiltered_error);
        fprintf (stderr, "               filtered error range = %.3f to %.3f\n", chan->min_filtered_error, chan->max_filtered_error);
    }
#endif

    if (cxt->workers)
        workersDeinit (cxt->workers);

    for (int c = 0; c < cxt->numChannels; ++c) {
        free (cxt->channels [c].dsd_buffer);
        free (cxt->channels [c].source_buffer);
        free (cxt->channels [c].upsample_buffer);
    }

    for (int f = 0; f < NUM_FILTERS; ++f)
        free (cxt->channels [0].upsample_filters [f]);

    decimateDSDdestroy (cxt->channels [0].decimator);
    free (cxt->channels [0].upsample_filters);
    free (cxt->channels);
    free (cxt);
}

#define MIN_RMS_ERROR(x)    ((x)*(x) - fabs (2.0*(x)) + 1.0)
#define SQUARE(x)           ((x)*(x))

#ifdef STATISTICS
static double min_error (ModulatorChannel *cxt, const float *samples, const double *filter, const double *error, int depth, double max_min)
#else
static double min_error (const float *samples, const double *filter, const double *error, int depth, double max_min)
#endif
{
#if NS_TAPS == 4
    double sample = samples [0] + (error [0] * filter [0]) + (error [1] * filter [1]) + (error [2] * filter [2]) + (error [3] * filter [3]);
#else
    double sample = samples [0] + (error [0] * filter [0]) + (error [1] * filter [1]) + (error [2] * filter [2]);
#endif

#if (DEPTH_TERM == 0)
    if (depth == 0) {
#ifdef STATISTICS
        cxt->leaves++;
#endif
        return MIN_RMS_ERROR (sample);
    }
#endif

#if (DEPTH_TERM == 1)
    if (depth == 1) {
#if NS_TAPS == 4
        double sample_0 = samples [1] + ((-1.0 - sample) * filter [0]) + (error [0] * filter [1]) + (error [1] * filter [2]) + (error [2] * filter [3]);
#else
        double sample_0 = samples [1] + ((-1.0 - sample) * filter [0]) + (error [0] * filter [1]) + (error [1] * filter [2]);
#endif
        double sample_1 = sample_0 + filter [0] * 2.0;

#ifdef STATISTICS
        cxt->leaves += 2;
#endif
        return fmin (SQUARE (-1.0 - sample) + MIN_RMS_ERROR (sample_0), SQUARE (1.0 - sample) + MIN_RMS_ERROR (sample_1));
    }
#endif

#if (DEPTH_TERM == 2)
    if (depth == 2) {
#if NS_TAPS == 4
        double sample_0 = samples [1] + ((-1.0 - sample) * filter [0]) + (error [0] * filter [1]) + (error [1] * filter [2]) + (error [2] * filter [3]);
#else
        double sample_0 = samples [1] + ((-1.0 - sample) * filter [0]) + (error [0] * filter [1]) + (error [1] * filter [2]);
#endif
        double sample_1 = sample_0 + filter [0] * 2.0;

#if NS_TAPS == 4
        double sample_0_0 = samples [2] + ((-1.0 - sample_0) * filter [0]) + ((-1.0 - sample) * filter [1]) + (error [0] * filter [2]) + (error [1] * filter [3]);
#else
        double sample_0_0 = samples [2] + ((-1.0 - sample_0) * filter [0]) + ((-1.0 - sample) * filter [1]) + (error [0] * filter [2]);
#endif
        double sample_0_1 = sample_0_0 + filter [0] * 2.0;
#if NS_TAPS == 4
        double sample_1_0 = samples [2] + ((-1.0 - sample_1) * filter [0]) + ((+1.0 - sample) * filter [1]) + (error [0] * filter [2]) + (error [1] * filter [3]);
#else
        double sample_1_0 = samples [2] + ((-1.0 - sample_1) * filter [0]) + ((+1.0 - sample) * filter [1]) + (error [0] * filter [2]);
#endif
        double sample_1_1 = sample_1_0 + filter [0] * 2.0;

        double error_0_0 = SQUARE (-1.0 - sample) + SQUARE (-1.0 - sample_0) + MIN_RMS_ERROR (sample_0_0);
        double error_0_1 = SQUARE (-1.0 - sample) + SQUARE (+1.0 - sample_0) + MIN_RMS_ERROR (sample_0_1);
        double error_1_0 = SQUARE (+1.0 - sample) + SQUARE (-1.0 - sample_1) + MIN_RMS_ERROR (sample_1_0);
        double error_1_1 = SQUARE (+1.0 - sample) + SQUARE (+1.0 - sample_1) + MIN_RMS_ERROR (sample_1_1);

#ifdef STATISTICS
        cxt->leaves += 4;
#endif

        return fmin (fmin (error_0_0, error_0_1), fmin (error_1_0, error_1_1));
    }
#endif

    {
        double first_sample = (sample < 0.0) ? -1.0 : 1.0;
        double error_sum = SQUARE (first_sample - sample);

        if (NO_PRUNING || error_sum < max_min) {
#if NS_TAPS == 4
            double loc_error [] = { first_sample - sample, error [0], error [1], error [2] };
#else
            double loc_error [] = { first_sample - sample, error [0], error [1] };
#endif
            double alt_error_sum = SQUARE (-first_sample - sample);

#ifdef STATISTICS
            error_sum += min_error (cxt, samples + 1, filter, loc_error, depth - 1, max_min - error_sum);
#else
            error_sum += min_error (samples + 1, filter, loc_error, depth - 1, max_min - error_sum);
#endif
            if (NO_PRUNING || alt_error_sum < error_sum) {
                loc_error [0] = -first_sample - sample;
#ifdef STATISTICS
                alt_error_sum += min_error (cxt, samples + 1, filter, loc_error, depth - 1, error_sum - alt_error_sum);
#else
                alt_error_sum += min_error (samples + 1, filter, loc_error, depth - 1, error_sum - alt_error_sum);
#endif
                if (alt_error_sum < error_sum)
                    error_sum = alt_error_sum;
            }
        }

        return error_sum;
    }
}

#ifdef STATISTICS
static double best_sample (ModulatorChannel *cxt, const float *samples, double order, double *error, int depth)
#else
static double best_sample (const float *samples, double order, double *error, int depth)
#endif
{
#if NS_TAPS == 4
    double filter [NS_TAPS] = { -order };
    double sample, best_sample;

    if (order > 3.0) {
        filter [1] = (order - 2.0) * 3.0;
        filter [2] = 8.0 - (order * 3.0);
        filter [3] = order - 3.0;
    }
    else {
        filter [1] = (2.0 * order) - 3.0;
        filter [2] = 2.0 - order;
    }

    sample = samples [0] + (error [0] * filter [0]) + (error [1] * filter [1]) + (error [2] * filter [2]) + (error [3] * filter [3]);
    best_sample = (sample < 0.0) ? -1.0 : 1.0;
    error [3] = error [2];
#else
    double filter [] = { -order, 2.0 * order - 3.0, 2.0 - order };
    double sample = samples [0] + (error [0] * filter [0]) + (error [1] * filter [1]) + (error [2] * filter [2]);
    double best_sample = (sample < 0.0) ? -1.0 : 1.0;
#endif

    error [2] = error [1];
    error [1] = error [0];

    if (depth)
        switch (depth) {

        case 1: {
#if NS_TAPS == 4
            double sample_0 = samples [1] + ((-1.0 - sample) * filter [0]) + (error [1] * filter [1]) + (error [2] * filter [2]) + (error [3] * filter [3]);
#else
            double sample_0 = samples [1] + ((-1.0 - sample) * filter [0]) + (error [1] * filter [1]) + (error [2] * filter [2]);
#endif
            double sample_1 = sample_0 + filter [0] * 2.0;

            best_sample = SQUARE (+1.0 - sample) + MIN_RMS_ERROR (sample_1) < SQUARE (-1.0 - sample) + MIN_RMS_ERROR (sample_0) ? 1.0 : -1.0;
            break;
        }

        case 2: {
#if NS_TAPS == 4
            double sample_0 = samples [1] + ((-1.0 - sample) * filter [0]) + (error [1] * filter [1]) + (error [2] * filter [2]) + (error [3] * filter [3]);
#else
            double sample_0 = samples [1] + ((-1.0 - sample) * filter [0]) + (error [1] * filter [1]) + (error [2] * filter [2]);
#endif
            double sample_1 = sample_0 + filter [0] * 2.0;

#if NS_TAPS == 4
            double sample_0_0 = samples [2] + ((-1.0 - sample_0) * filter [0]) + ((-1.0 - sample) * filter [1]) + (error [1] * filter [2]) + (error [2] * filter [3]);
#else
            double sample_0_0 = samples [2] + ((-1.0 - sample_0) * filter [0]) + ((-1.0 - sample) * filter [1]) + (error [1] * filter [2]);
#endif
            double sample_0_1 = sample_0_0 + filter [0] * 2.0;
            double sample_1_0 = sample_0_0 + (filter [1] - SQUARE (filter [0])) * 2.0;
            double sample_1_1 = sample_1_0 + filter [0] * 2.0;

            double error_0_0 = (+2.0 * sample) + SQUARE (-1.0 - sample_0) + MIN_RMS_ERROR (sample_0_0);
            double error_0_1 = (+2.0 * sample) + SQUARE (+1.0 - sample_0) + MIN_RMS_ERROR (sample_0_1);
            double error_1_0 = (-2.0 * sample) + SQUARE (-1.0 - sample_1) + MIN_RMS_ERROR (sample_1_0);
            double error_1_1 = (-2.0 * sample) + SQUARE (+1.0 - sample_1) + MIN_RMS_ERROR (sample_1_1);

            best_sample = fmin (error_1_0, error_1_1) < fmin (error_0_0, error_0_1) ? 1.0 : -1.0;
            break;
        }

        default: {
            double alt_error_sum = SQUARE (-best_sample - sample);
            double error_sum = SQUARE (best_sample - sample);

            error [0] = best_sample - sample;
#ifdef STATISTICS
            error_sum += min_error (cxt, samples + 1, filter, error, depth - 1, DBL_MAX);
            cxt->called_best_sample++;
#else
            error_sum += min_error (samples + 1, filter, error, depth - 1, DBL_MAX);
#endif
            if (NO_PRUNING || alt_error_sum < error_sum) {
                error [0] = -best_sample - sample;
#ifdef STATISTICS
                alt_error_sum += min_error (cxt, samples + 1, filter, error, depth - 1, error_sum - alt_error_sum);
                cxt->checked_alt_sample++;
#else
                alt_error_sum += min_error (samples + 1, filter, error, depth - 1, error_sum - alt_error_sum);
#endif
                if (alt_error_sum < error_sum) {
                    best_sample = -best_sample;
#ifdef STATISTICS
                    cxt->used_alt_sample++;
#endif
                }
            }
         }
    }

    error [0] = best_sample - sample;
    return best_sample;
}

#define BLACKMAN_HARRIS

static void init_filter (int numTaps, float *filter, double fraction)
{
    double filter_sum = 0.0, scaler, error;
    double *tempFilter = malloc (numTaps * sizeof (double));
    const double a0 = 0.35875;
    const double a1 = 0.48829;
    const double a2 = 0.14128;
    const double a3 = 0.01168;
    int i;

    // "dist" is the absolute distance from the sinc maximum to the filter tap to be calculated, in radians
    // "ratio" is that distance divided by half the tap count such that it reaches π at the window extremes

    // Note that with this scaling, the odd terms of the Blackman-Harris calculation appear to be negated
    // with respect to the reference formula version.

    for (i = 0; i < numTaps; ++i) {
        double dist = fabs ((numTaps / 2 - 1) + fraction - i) * M_PI;
        double ratio = dist / (numTaps / 2);
        double value;

        if (dist != 0.0) {
            value = sin (dist) / dist;

#ifdef BLACKMAN_HARRIS
                value *= a0 + a1 * cos (ratio) + a2 * cos (2 * ratio) + a3 * cos (3 * ratio);
#else
                value *= 0.5 * (1.0 + cos (ratio));     // Hann window
#endif
        }
        else
            value = 1.0;

        filter_sum += tempFilter [i] = value;
    }

    // filter should have unity DC gain

    scaler = 1.0 / filter_sum;
    error = 0.0;

    for (i = numTaps / 2; i < numTaps; i = numTaps - i - (i >= numTaps / 2)) {
        filter [i] = (tempFilter [i] *= scaler) - error;
        error += filter [i] - tempFilter [i];
    }

    free (tempFilter);
}

static inline double apply_filter (float *A, float *B, int num_taps)
{
    double sum = 0.0;

    do sum += (double) *A++ * *B++;
    while (--num_taps);

    return sum;
}

// Return an uncorrelated tpdf random value in the range: -1.0 <= n < 1.0

static inline double tpdf_dither (uint32_t *generator)
{
    uint32_t random = *generator, value;

    random = ((random << 4) - random) ^ 1;
    value = ~(random = ((random << 4) - random) ^ 1) >> 1;
    random = ((random << 4) - random) ^ 1;
    random = ((random << 4) - random) ^ 1;
    value += (*generator = ((random << 4) - random) ^ 1) >> 1;

    return (value / 2147483648.0) - 1.0;
}
