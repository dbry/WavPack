////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//              Copyright (c) 1998 - 2013 Conifer Software.               //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// unpack_utils.c

// This module provides the high-level API for unpacking audio data from
// WavPack files. It manages the buffers used to interleave the data passed
// back to the application from the individual streams. The actual audio
// stream decompression is handled in the unpack.c module.

#include <stdlib.h>
#include <string.h>

#include "wavpack_local.h"

#ifdef ENABLE_THREADS
static void unpack_samples_enqueue (WavpackStream *wps, int32_t *outbuf, int offset, uint32_t samcnt);
static void worker_threads_create (WavpackContext *wpc);
#endif

///////////////////////////// executable code ////////////////////////////////

// This function unpacks the specified number of samples from the given stream (which must be
// completely loaded and initialized). The samples are written (interleaved) into the given
// buffer at the specified offset. This function is threadsafe across streams, so it may be
// called directly from the main unpack code or from the worker threads.

static void unpack_samples_interleave (WavpackStream *wps, int32_t *outbuf, int offset, int32_t *tmpbuf, uint32_t samcnt)
{
    int num_channels = wps->wpc->config.num_channels;
    int32_t *src = tmpbuf, *dst = outbuf + offset;

#ifdef ENABLE_DSD
    if (wps->wphdr.flags & DSD_FLAG)
        unpack_dsd_samples (wps, tmpbuf, samcnt);
    else
#endif
        unpack_samples (wps, tmpbuf, samcnt);

    // if the block is mono, copy the samples from the single channel into the destination
    // using num_channels as the stride

    if (wps->wphdr.flags & MONO_FLAG) {
        while (samcnt--) {
            dst [0] = *src++;
            dst += num_channels;
        }
    }

    // if the block is stereo, and we don't have room for two more channels, just copy one

    else if (offset == num_channels - 1) {
        while (samcnt--) {
            dst [0] = src [0];
            dst += num_channels;
            src += 2;
        }
    }

    // otherwise copy the stereo samples into the destination

    else {
        while (samcnt--) {
            dst [0] = *src++;
            dst [1] = *src++;
            dst += num_channels;
        }
    }
}

// Unpack the specified number of samples from the current file position.
// Note that "samples" here refers to "complete" samples, which would be
// 2 longs for stereo files or even more for multichannel files, so the
// required memory at "buffer" is 4 * samples * num_channels bytes. The
// audio data is returned right-justified in 32-bit longs in the endian
// mode native to the executing processor. So, if the original data was
// 16-bit, then the values returned would be +/-32k. Floating point data
// can also be returned if the source was floating point data (and this
// can be optionally normalized to +/-1.0 by using the appropriate flag
// in the call to WavpackOpenFileInput ()). The actual number of samples
// unpacked is returned, which should be equal to the number requested unless
// the end of file is encountered or an error occurs. After all samples have
// been unpacked then 0 will be returned.

uint32_t WavpackUnpackSamples (WavpackContext *wpc, int32_t *buffer, uint32_t samples)
{
    int num_channels = wpc->config.num_channels, file_done = FALSE;
    uint32_t bcount, samples_unpacked = 0, samples_to_unpack;
    int32_t *bptr = buffer;

    memset (buffer, 0, num_channels * samples * sizeof (int32_t));

#ifdef ENABLE_LEGACY
    if (wpc->stream3)
        return unpack_samples3 (wpc, buffer, samples);
#endif

    while (samples) {
        WavpackStream *wps = wpc->streams [0];
        int stream_index = 0;

        // if the current block has no audio, or it's not the first block of a multichannel
        // sequence, or the sample we're on is past the last sample in this block...we need
        // to free up the streams and read the next block

        if (!wps->wphdr.block_samples || !(wps->wphdr.flags & INITIAL_BLOCK) ||
            wps->sample_index >= GET_BLOCK_INDEX (wps->wphdr) + wps->wphdr.block_samples) {
                int64_t nexthdrpos;

                if (wpc->wrapper_bytes >= MAX_WRAPPER_BYTES)
                    break;

                free_streams (wpc);
                nexthdrpos = wpc->reader->get_pos (wpc->wv_in);
                bcount = read_next_header (wpc->reader, wpc->wv_in, &wps->wphdr);

                if (bcount == (uint32_t) -1)
                    break;

                wpc->filepos = nexthdrpos + bcount;

                // allocate the memory for the entire raw block and read it in

                wps->blockbuff = (unsigned char *)malloc (wps->wphdr.ckSize + 8);

                if (!wps->blockbuff)
                    break;

                memcpy (wps->blockbuff, &wps->wphdr, 32);

                if (wpc->reader->read_bytes (wpc->wv_in, wps->blockbuff + 32, wps->wphdr.ckSize - 24) !=
                    wps->wphdr.ckSize - 24) {
                        strcpy (wpc->error_message, "can't read all of last block!");
                        wps->wphdr.block_samples = 0;
                        wps->wphdr.ckSize = 24;
                        break;
                }

                // render corrupt blocks harmless
                if (!WavpackVerifySingleBlock (wps->blockbuff, !(wpc->open_flags & OPEN_NO_CHECKSUM))) {
                    wps->wphdr.ckSize = sizeof (WavpackHeader) - 8;
                    wps->wphdr.block_samples = 0;
                    memcpy (wps->blockbuff, &wps->wphdr, 32);
                }

                // potentially adjusting block_index must be done AFTER verifying block

                if (wpc->open_flags & OPEN_STREAMING)
                    SET_BLOCK_INDEX (wps->wphdr, wps->sample_index = 0);
                else
                    SET_BLOCK_INDEX (wps->wphdr, GET_BLOCK_INDEX (wps->wphdr) - wpc->initial_index);

                memcpy (wps->blockbuff, &wps->wphdr, 32);
                wps->init_done = FALSE;     // we have not yet called unpack_init() for this block

                // if this block has audio, but not the sample index we were expecting, flag an error

                if (wps->wphdr.block_samples && wps->sample_index != GET_BLOCK_INDEX (wps->wphdr))
                    wpc->crc_errors++;

                // if this block has audio, and we're in hybrid lossless mode, read the matching wvc block

                if (wps->wphdr.block_samples && wpc->wvc_flag)
                    read_wvc_block (wpc, 0);

                // if the block does NOT have any audio, call unpack_init() to process non-audio stuff

                if (!wps->wphdr.block_samples) {
                    if (!wps->init_done && !unpack_init (wpc, 0))
                        wpc->crc_errors++;

                    wps->init_done = TRUE;
                }
        }

        // if the current block has no audio, or it's not the first block of a multichannel
        // sequence, or the sample we're on is past the last sample in this block...we need
        // to loop back and read the next block

        if (!wps->wphdr.block_samples || !(wps->wphdr.flags & INITIAL_BLOCK) ||
            wps->sample_index >= GET_BLOCK_INDEX (wps->wphdr) + wps->wphdr.block_samples)
                continue;

        // There seems to be some missing data, like a block was corrupted or something.
        // If it's not too much data, just fill in with silence here and loop back.

        if (wps->sample_index < GET_BLOCK_INDEX (wps->wphdr)) {
            int32_t zvalue = (wps->wphdr.flags & DSD_FLAG) ? 0x55 : 0;

            samples_to_unpack = (uint32_t) (GET_BLOCK_INDEX (wps->wphdr) - wps->sample_index);

            if (!samples_to_unpack || samples_to_unpack > 262144) {
                strcpy (wpc->error_message, "discontinuity found, aborting file!");
                wps->wphdr.block_samples = 0;
                wps->wphdr.ckSize = 24;
                break;
            }

            if (samples_to_unpack > samples)
                samples_to_unpack = samples;

            wps->sample_index += samples_to_unpack;
            samples_unpacked += samples_to_unpack;
            samples -= samples_to_unpack;

            samples_to_unpack *= (wpc->reduced_channels ? wpc->reduced_channels : num_channels);

            while (samples_to_unpack--)
                *bptr++ = zvalue;

            continue;
        }

        // calculate number of samples to process from this block, then initialize the decoder for
        // this block if we haven't already

        samples_to_unpack = (uint32_t) (GET_BLOCK_INDEX (wps->wphdr) + wps->wphdr.block_samples - wps->sample_index);

        if (samples_to_unpack > samples)
            samples_to_unpack = samples;

        if (!wps->init_done && !unpack_init (wpc, 0))
            wpc->crc_errors++;

        wps->init_done = TRUE;

        // if this block is not the final block of a multichannel sequence (and we're not truncating
        // to stereo), then enter this conditional block...otherwise we just unpack the samples directly

        if (!wpc->reduced_channels && !(wps->wphdr.flags & FINAL_BLOCK)) {
            int32_t *temp_buffer = NULL;
            uint32_t offset = 0;     // offset to next channel in sequence (0 to num_channels - 1)

#ifdef ENABLE_THREADS
            if (wpc->num_workers) {
                if (!wpc->workers)
                    worker_threads_create (wpc);
            }
            else
#endif
            {
                temp_buffer = (int32_t *)calloc (1, samples_to_unpack * 8);

                if (!temp_buffer)
                    break;
            }

            // loop through all the streams...

            while (1) {

                // if the stream has not been allocated and corresponding block read, do that here...

                if (stream_index == wpc->num_streams) {
                    wpc->streams = (WavpackStream **)realloc (wpc->streams, (wpc->num_streams + 1) * sizeof (wpc->streams [0]));

                    if (!wpc->streams)
                        break;

                    wps = wpc->streams [wpc->num_streams++] = (WavpackStream *)calloc (1, sizeof (WavpackStream));

                    if (!wps)
                        break;

                    wps->wpc = wpc;
                    wps->stream_index = stream_index;
                    bcount = read_next_header (wpc->reader, wpc->wv_in, &wps->wphdr);

                    if (bcount == (uint32_t) -1) {
                        wpc->streams [0]->wphdr.block_samples = 0;
                        wpc->streams [0]->wphdr.ckSize = 24;
                        file_done = TRUE;
                        break;
                    }

                    wps->blockbuff = (unsigned char *)malloc (wps->wphdr.ckSize + 8);

                    if (!wps->blockbuff)
                        break;

                    memcpy (wps->blockbuff, &wps->wphdr, 32);

                    if (wpc->reader->read_bytes (wpc->wv_in, wps->blockbuff + 32, wps->wphdr.ckSize - 24) !=
                        wps->wphdr.ckSize - 24) {
                            wpc->streams [0]->wphdr.block_samples = 0;
                            wpc->streams [0]->wphdr.ckSize = 24;
                            file_done = TRUE;
                            break;
                    }

                    // render corrupt blocks harmless
                    if (!WavpackVerifySingleBlock (wps->blockbuff, !(wpc->open_flags & OPEN_NO_CHECKSUM))) {
                        wps->wphdr.ckSize = sizeof (WavpackHeader) - 8;
                        wps->wphdr.block_samples = 0;
                        memcpy (wps->blockbuff, &wps->wphdr, 32);
                    }

                    // potentially adjusting block_index must be done AFTER verifying block

                    if (wpc->open_flags & OPEN_STREAMING)
                        SET_BLOCK_INDEX (wps->wphdr, wps->sample_index = 0);
                    else
                        SET_BLOCK_INDEX (wps->wphdr, GET_BLOCK_INDEX (wps->wphdr) - wpc->initial_index);

                    memcpy (wps->blockbuff, &wps->wphdr, 32);

                    // if this block has audio, and we're in hybrid lossless mode, read the matching wvc block

                    if (wpc->wvc_flag)
                        read_wvc_block (wpc, stream_index);

                    // initialize the unpacker for this block

                    if (!unpack_init (wpc, stream_index))
                        wpc->crc_errors++;

                    wps->init_done = TRUE;
                }
                else
                    wps = wpc->streams [stream_index];

#ifdef ENABLE_THREADS
                if (wpc->num_workers)
                    unpack_samples_enqueue (wps, bptr, offset, samples_to_unpack);
                else
#endif
                    unpack_samples_interleave (wps, bptr, offset, temp_buffer, samples_to_unpack);

                if (wps->wphdr.flags & MONO_FLAG)
                    offset++;
                else if (offset == num_channels - 1) {
                    wpc->crc_errors++;
                    offset++;
                }
                else
                    offset += 2;

                // check several clues that we're done with this set of blocks and exit if we are; else do next stream

                if ((wps->wphdr.flags & FINAL_BLOCK) || stream_index == wpc->max_streams - 1 || offset == num_channels)
                    break;
                else
                    stream_index++;
            }

#ifdef ENABLE_THREADS
            if (wpc->num_workers) {         // wait until all the worker threads are ready (i.e. done)
                wp_mutex_obtain (wpc->mutex);

                while (wpc->workers_ready < wpc->num_workers)
                    wp_condvar_wait (wpc->global_cond, wpc->mutex);

                wp_mutex_release (wpc->mutex);
            }
#endif

            free (temp_buffer);

            // if we didn't get all the channels we expected, mute the buffer and flag an error

            if (offset != num_channels) {
                if (wps->wphdr.flags & DSD_FLAG) {
                    int samples_to_zero = samples_to_unpack * num_channels;
                    int32_t *zptr = bptr;

                    while (samples_to_zero--)
                        *zptr++ = 0x55;
                }
                else
                    memset (bptr, 0, samples_to_unpack * num_channels * 4);

                wpc->crc_errors++;
            }

            // go back to the first stream (we're going to leave them all loaded for now because they might have more samples)

            wps = wpc->streams [stream_index = 0];
        }
        // catch the error situation where we have only one channel but run into a stereo block
        // (this avoids overwriting the caller's buffer)
        else if (!(wps->wphdr.flags & MONO_FLAG) && (num_channels == 1 || wpc->reduced_channels == 1)) {
            memset (bptr, 0, samples_to_unpack * sizeof (*bptr));
            wps->sample_index += samples_to_unpack;
            wpc->crc_errors++;
        }
#ifdef ENABLE_DSD
        else if (wps->wphdr.flags & DSD_FLAG)
            unpack_dsd_samples (wps, bptr, samples_to_unpack);
#endif
        else
            unpack_samples (wps, bptr, samples_to_unpack);

        if (file_done) {
            strcpy (wpc->error_message, "can't read all of last block!");
            break;
        }

        if (wpc->reduced_channels)
            bptr += samples_to_unpack * wpc->reduced_channels;
        else
            bptr += samples_to_unpack * num_channels;

        samples_unpacked += samples_to_unpack;
        samples -= samples_to_unpack;

        // if we just finished a block, check for a calculated crc error
        // (and back up the streams a little if possible in case we passed a header)

        if (wps->sample_index == GET_BLOCK_INDEX (wps->wphdr) + wps->wphdr.block_samples) {
            if (check_crc_error (wpc)) {
                int32_t *zptr = bptr, zvalue = (wps->wphdr.flags & DSD_FLAG) ? 0x55 : 0;
                uint32_t samples_to_zero = wps->wphdr.block_samples;

                if (samples_to_zero > samples_to_unpack)
                    samples_to_zero = samples_to_unpack;

                samples_to_zero *= (wpc->reduced_channels ? wpc->reduced_channels : num_channels);

                while (samples_to_zero--)
                    *--zptr = zvalue;

                if (wps->blockbuff && wpc->reader->can_seek (wpc->wv_in)) {
                    int32_t rseek = ((WavpackHeader *) wps->blockbuff)->ckSize / 3;
                    wpc->reader->set_pos_rel (wpc->wv_in, (rseek > 16384) ? -16384 : -rseek, SEEK_CUR);
                }

                if (wpc->wvc_flag && wps->block2buff && wpc->reader->can_seek (wpc->wvc_in)) {
                    int32_t rseek = ((WavpackHeader *) wps->block2buff)->ckSize / 3;
                    wpc->reader->set_pos_rel (wpc->wvc_in, (rseek > 16384) ? -16384 : -rseek, SEEK_CUR);
                }

                wpc->crc_errors++;
            }
        }

        if (wpc->total_samples != -1 && wps->sample_index == wpc->total_samples)
            break;
    }

#ifdef ENABLE_DSD
    if (wpc->decimation_context)
        decimate_dsd_run (wpc->decimation_context, buffer, samples_unpacked);
#endif

    return samples_unpacked;
}

///////////////////////////// multithreading code ////////////////////////////////

#ifdef ENABLE_THREADS

// This is the worker thread function for unpacking support, essentially allowing
// unpack_samples_interleave() to be running for multiple streams simultaneously.

#ifdef _WIN32
static DWORD WINAPI unpack_samples_worker_thread (LPVOID param)
#else
static void *unpack_samples_worker_thread (void *param)
#endif
{
    WorkerInfo *cxt = param;
    int32_t *temp_buffer = NULL;
    int temp_samples = 0;

    while (1) {
        wp_mutex_obtain (*cxt->mutex);
        cxt->state = Ready;
        (*cxt->workers_ready)++;
        wp_condvar_signal (*cxt->global_cond);      // signal that we're ready to work

        while (cxt->state == Ready)                 // wait for something to do
            wp_condvar_wait (cxt->worker_cond, *cxt->mutex);

        wp_mutex_release (*cxt->mutex);

        if (cxt->state == Quit)                     // break out if we're done
            break;

        if (cxt->samcnt > temp_samples)             // reallocate temp buffer if not big enough
            temp_buffer = (int32_t *) realloc (temp_buffer, (temp_samples = cxt->samcnt) * 8);

        // this is where the work is done and we can loop back to "ready" when we're done
        unpack_samples_interleave (cxt->wps, cxt->outbuf, cxt->offset, temp_buffer, cxt->samcnt);
    }

    free (temp_buffer);
    wp_thread_exit (0);
    return 0;
}

// Send the given stream to an available worker thread. In the background, the stream will be
// unpacked and written (interleaved) to the given buffer at the specified offset.

static void unpack_samples_enqueue (WavpackStream *wps, int32_t *outbuf, int offset, uint32_t samcnt)
{
    WavpackContext *wpc = (WavpackContext *) wps->wpc;  // this is safe here because single-threaded
    int i;

    wp_mutex_obtain (wpc->mutex);

    while (!wpc->workers_ready)
        wp_condvar_wait (wpc->global_cond, wpc->mutex);

    for (i = 0; i < wpc->num_workers; ++i)
        if (wpc->workers [i].state == Ready) {
            wpc->workers [i].wps = wps;
            wpc->workers [i].outbuf = outbuf;
            wpc->workers [i].offset = offset;
            wpc->workers [i].samcnt = samcnt;
            wpc->workers [i].state = Running;
            wp_condvar_signal (wpc->workers [i].worker_cond);
            wpc->workers_ready--;
            break;
        }

    wp_mutex_release (wpc->mutex);
}

// Create the worker thread contexts and start the threads
// (which should all quickly go to the ready state)

static void worker_threads_create (WavpackContext *wpc)
{
    if (!wpc->workers) {
        int i;

        wp_mutex_init (wpc->mutex);
        wp_condvar_init (wpc->global_cond);

        wpc->workers = calloc (wpc->num_workers, sizeof (WorkerInfo));

        for (i = 0; i < wpc->num_workers; ++i) {
            wpc->workers [i].mutex = &wpc->mutex;
            wpc->workers [i].global_cond = &wpc->global_cond;
            wpc->workers [i].workers_ready = &wpc->workers_ready;
            wp_condvar_init (wpc->workers [i].worker_cond);
            wp_thread_create (wpc->workers [i].thread, unpack_samples_worker_thread, &wpc->workers [i]);
        }
    }
}

#endif
