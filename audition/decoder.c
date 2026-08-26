////////////////////////////////////////////////////////////////////////////
//                            **** DECODER ****                           //
//                        Decode DXD-e Back To DSD                        //
//                     Copyright (c) 2026 David Bryant                    //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// decoder.c

#include "decoder.h"

#define BUFSAMPLES  4704

#define IDLE_LEVEL  0

Decoder *decodeInit (int num_channels, int dsd_encode_level, int flags)
{
    Decoder *decoder = calloc (1, sizeof (Decoder));

    decoder->nchans = num_channels;
    decoder->level = dsd_encode_level;
    decoder->channels = calloc (num_channels, sizeof (DecoderChannel));
    decoder->decimator = decimateDSDinit (0, 0);
    decoder->modulator = modulateInit (num_channels, IDLE_LEVEL, (flags & MODULATOR_SHARED_FLAGS) | MODULATOR_ALIGN_EMBEDDED);
    decoder->float_buffer = calloc (sizeof (float), BUFSAMPLES * num_channels);
    decoder->embedded_buffer = calloc (sizeof (char), BUFSAMPLES * num_channels);
    decoder->modulated_buffer = calloc (sizeof (char), BUFSAMPLES * num_channels);
    decoder->composite_buffer = calloc (sizeof (char), BUFSAMPLES * num_channels);
    decoder->source_buffer = calloc (sizeof (int32_t), NUM_BUFFERS * BUFSAMPLES * num_channels);

    if (!(flags & DECODER_EXTRACT_ALWAYS))
        decoder->pilot_detector = pilotDetectInit (num_channels);

    return decoder;
}

int decodeProcess (Decoder *cxt, const int32_t *source, int in_samples, unsigned char *destin, int out_samples)
{
    int samples_returned = 0;

    if (!source || in_samples < 0 || cxt->flushing) {
        cxt->flushing = 1;
        in_samples = 0;
    }

    while (1) {
        if (out_samples && cxt->composite_samples) {
            int samples_to_write = cxt->composite_samples;

            if (samples_to_write > out_samples)
                samples_to_write = out_samples;

            memcpy (destin, cxt->composite_buffer, samples_to_write * cxt->nchans);
            cxt->total_dsd_samples += samples_to_write * cxt->nchans;
            cxt->composite_samples -= samples_to_write;
            destin += samples_to_write * cxt->nchans;
            samples_returned += samples_to_write;
            out_samples -= samples_to_write;

            if (cxt->composite_samples)
                memmove (cxt->composite_buffer, cxt->composite_buffer + samples_to_write * cxt->nchans, cxt->composite_samples * cxt->nchans);
        }
        else if (in_samples && cxt->source_samples < BUFSAMPLES * NUM_BUFFERS) {
            int samples_to_consume = BUFSAMPLES * NUM_BUFFERS - cxt->source_samples;

            if (samples_to_consume > in_samples)
                samples_to_consume = in_samples;

            memcpy (cxt->source_buffer + cxt->source_samples * cxt->nchans, source, samples_to_consume * cxt->nchans * sizeof (int32_t));
            cxt->total_pcm_samples += samples_to_consume;
            cxt->source_samples += samples_to_consume;
            source += samples_to_consume * cxt->nchans;
            in_samples -= samples_to_consume;
        }
        else if (!cxt->composite_samples && (cxt->source_samples == BUFSAMPLES * NUM_BUFFERS || (/* cxt->source_samples && */ cxt->flushing))) {
            int samples_read = BUFSAMPLES;

            if (cxt->flushing)
                samples_read = cxt->source_samples > BUFSAMPLES * 2 ? cxt->source_samples - BUFSAMPLES * 2 : 0;

            for (int c = 0; c < cxt->nchans; ++c) {
                DecoderChannel *chan = cxt->channels + c;

                switch (chan->state) {
                    case Init:
                        chan->next_state = chan->state = Embedding;     // default to embedding until we see an invalid frame

                        for (int b = 0; b < NUM_BUFFERS; ++b)
                            if (cxt->source_samples > BUFSAMPLES * b) {
                                int scan_samples = BUFSAMPLES;

                                if (cxt->source_samples < BUFSAMPLES * (b + 1))
                                    scan_samples = cxt->source_samples - BUFSAMPLES * b;

                                if (cxt->pilot_detector)
                                    chan->dsd_pilot_valid [b] = pilotDetectChannelRun (cxt->pilot_detector, cxt->source_buffer + BUFSAMPLES * b * cxt->nchans, c, scan_samples);
                                else
                                    chan->dsd_pilot_valid [b] = 1;

                                if (!chan->dsd_pilot_valid [b])
                                    chan->next_state = chan->state = Generating;
                            }
                            else
                                break;

                        if (chan->state == Generating)
                            modulateSetLevel (cxt->modulator, c, cxt->level);
                        else
                            modulateSetLevel (cxt->modulator, c, IDLE_LEVEL);

                        modulateSetAlignment (cxt->modulator, c, 0);
                        break;

                    case Generating:
                        if (samples_read) {
                                if (cxt->pilot_detector)
                                    chan->dsd_pilot_valid [2] = pilotDetectChannelRun (cxt->pilot_detector, cxt->source_buffer + BUFSAMPLES * 2 * cxt->nchans, c, samples_read);
                                else
                                    chan->dsd_pilot_valid [2] = 1;

                            if (chan->dsd_pilot_valid [0] && chan->dsd_pilot_valid [1] && chan->dsd_pilot_valid [2]) {
                                modulateSetAlignment (cxt->modulator, c, 1);
                                chan->next_state = Syncing;
                            }
                        }

                        break;

                    case Syncing:
                        if (samples_read) {
                                if (cxt->pilot_detector)
                                    chan->dsd_pilot_valid [2] = pilotDetectChannelRun (cxt->pilot_detector, cxt->source_buffer + BUFSAMPLES * 2 * cxt->nchans, c, samples_read);
                                else
                                    chan->dsd_pilot_valid [2] = 1;

                            if (chan->dsd_pilot_valid [2])
                                chan->next_state = Embedding;
                            else {
                                modulateSetAlignment (cxt->modulator, c, 0);
                                chan->next_state = Generating;
                            }
                        }

                        break;

                    case Embedding:
                        if (samples_read) {
                            if (cxt->pilot_detector)
                                chan->dsd_pilot_valid [2] = pilotDetectChannelRun (cxt->pilot_detector, cxt->source_buffer + BUFSAMPLES * 2 * cxt->nchans, c, samples_read);
                            else
                                chan->dsd_pilot_valid [2] = 1;

                            if (!chan->dsd_pilot_valid [1])
                                chan->next_state = Generating;
                            else if (!chan->dsd_pilot_valid [2]) {
                                modulateSetLevel (cxt->modulator, c, cxt->level);
                                modulateSetAlignment (cxt->modulator, c, 1);
                            }
                        }

                        break;
                }
            }

            int samples_to_convert = BUFSAMPLES, samples_generated;

            if (samples_to_convert > cxt->source_samples)
                samples_to_convert = cxt->source_samples;

            for (int i = 0; i < samples_to_convert * cxt->nchans; ++i)
                cxt->float_buffer [i] = cxt->source_buffer [i] / 8388608.0F;

            if (cxt->source_samples > samples_to_convert) {
                int samples_to_shift = cxt->source_samples - samples_to_convert;

                memmove (cxt->source_buffer, cxt->source_buffer + samples_to_convert * cxt->nchans, samples_to_shift * cxt->nchans * sizeof (int32_t));
                cxt->source_samples = samples_to_shift;

                for (int c = 0; c < cxt->nchans; ++c) {
                    cxt->channels [c].dsd_pilot_valid [0] = cxt->channels [c].dsd_pilot_valid [1];
                    cxt->channels [c].dsd_pilot_valid [1] = cxt->channels [c].dsd_pilot_valid [2];
                }
            }
            else
                cxt->source_samples = 0;

            samples_generated = modulateProcess (cxt->modulator, cxt->float_buffer, samples_to_convert ? samples_to_convert : -1, cxt->modulated_buffer, cxt->embedded_buffer);

            if (samples_generated) {
                unsigned char *initial_buf = malloc (samples_generated), *final_buf = malloc (samples_generated);

                if (cxt->composite_samples + samples_generated > BUFSAMPLES) {
                    fprintf (stderr, "attempt to generate %d composite samples\n", cxt->composite_samples + samples_generated);
                    exit (1);
                }

                for (int c = 0; c < cxt->nchans; ++c) {
                    unsigned char *composite_buffer = cxt->composite_buffer + cxt->composite_samples * cxt->nchans;
                    DecoderChannel *chan = cxt->channels + c;

                    if (chan->next_state == Embedding && chan->state == Syncing) {           // transition from generated to embedded
                        for (int i = 0; i < samples_generated; ++i) {
                            initial_buf [i] = cxt->modulated_buffer [i * cxt->nchans + c];
                            final_buf [i] = cxt->embedded_buffer [i * cxt->nchans + c];
                        }

                        transitionDSDstreams (cxt->decimator, cxt->total_dsd_samples / cxt->nchans, initial_buf, final_buf, samples_generated);

                        for (int i = 0; i < samples_generated; ++i)
                            composite_buffer [i * cxt->nchans + c] = initial_buf [i];

                        modulateSetLevel (cxt->modulator, c, IDLE_LEVEL);
                        modulateSetAlignment (cxt->modulator, c, 0);
                    }
                    else if (chan->next_state == Generating && chan->state == Embedding) {    // transition from embedded to generated
                        for (int i = 0; i < samples_generated; ++i) {
                            initial_buf [i] = cxt->embedded_buffer [i * cxt->nchans + c];
                            final_buf [i] = cxt->modulated_buffer [i * cxt->nchans + c];
                        }

                        transitionDSDstreams (cxt->decimator, cxt->total_dsd_samples / cxt->nchans, initial_buf, final_buf, samples_generated);

                        for (int i = 0; i < samples_generated; ++i)
                            composite_buffer [i * cxt->nchans + c] = initial_buf [i];

                        modulateSetAlignment (cxt->modulator, c, 0);
                    }
                    else if (chan->state == Embedding) {
                        for (int i = 0; i < samples_generated; ++i)
                            composite_buffer [(i * cxt->nchans) + c] = cxt->embedded_buffer [(i * cxt->nchans) + c];

                        chan->valid_dsd_samples += samples_generated;
                    }
                    else if (chan->state == Generating || chan->state == Syncing)
                        for (int i = 0; i < samples_generated; ++i)
                            composite_buffer [(i * cxt->nchans) + c] = cxt->modulated_buffer [(i * cxt->nchans) + c];

                    chan->state = chan->next_state;
                }

                cxt->composite_samples += samples_generated;
                free (initial_buf);
                free (final_buf);
            }
            else
                break;
        }
        else
            break;
    }

    return samples_returned;
}

int64_t decodeTotalEmbeddedSamples (Decoder *cxt)
{
    int64_t total_embedded_samples = 0;

    for (int c = 0; c < cxt->nchans; ++c)
        total_embedded_samples += cxt->channels [c].valid_dsd_samples;

    return total_embedded_samples;
}

void decodeFree (Decoder *cxt)
{
    free (cxt->float_buffer);
    free (cxt->source_buffer);
    free (cxt->embedded_buffer);
    free (cxt->modulated_buffer);
    free (cxt->composite_buffer);
    modulateFree (cxt->modulator);
    decimateDSDdestroy (cxt->decimator);

    if (cxt->pilot_detector)
        pilotDetectDestroy (cxt->pilot_detector);

    free (cxt->channels);
    free (cxt);
}
