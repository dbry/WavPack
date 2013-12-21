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

///////////////////////////// executable code ////////////////////////////////

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
// the end of fle is encountered or an error occurs. After all samples have
// been unpacked then 0 will be returned.

uint32_t WavpackUnpackSamples (WavpackContext *wpc, int32_t *buffer, uint32_t samples)
{
    WavpackStream *wps = wpc->streams ? wpc->streams [wpc->current_stream = 0] : NULL;
    uint32_t bcount, samples_unpacked = 0, samples_to_unpack;
    int num_channels = wpc->config.num_channels;
    int file_done = FALSE;

#ifndef VER4_ONLY
    if (wpc->stream3)
        return unpack_samples3 (wpc, buffer, samples);
#endif

    while (samples) {
        if (!wps->wphdr.block_samples || !(wps->wphdr.flags & INITIAL_BLOCK) ||
            wps->sample_index >= wps->wphdr.block_index + wps->wphdr.block_samples) {

                uint32_t nexthdrpos;

                if (wpc->wrapper_bytes >= MAX_WRAPPER_BYTES)
                    break;

                free_streams (wpc);
                nexthdrpos = wpc->reader->get_pos (wpc->wv_in);
                bcount = read_next_header (wpc->reader, wpc->wv_in, &wps->wphdr);

                if (bcount == (uint32_t) -1)
                    break;

                wpc->filepos = nexthdrpos;

                if (wpc->open_flags & OPEN_STREAMING)
                    wps->wphdr.block_index = wps->sample_index = 0;
                else
                    wps->wphdr.block_index -= wpc->initial_index;

                wpc->filepos += bcount;
                wps->blockbuff = malloc (wps->wphdr.ckSize + 8);
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

                wps->init_done = FALSE;

                if (wps->wphdr.block_samples && wps->sample_index != wps->wphdr.block_index)
                    wpc->crc_errors++;

                if (wps->wphdr.block_samples && wpc->wvc_flag)
                    read_wvc_block (wpc);

                if (!wps->wphdr.block_samples) {
                    if (!wps->init_done && !unpack_init (wpc))
                        wpc->crc_errors++;

                    wps->init_done = TRUE;
                }
        }

        if (!wps->wphdr.block_samples || !(wps->wphdr.flags & INITIAL_BLOCK) ||
            wps->sample_index >= wps->wphdr.block_index + wps->wphdr.block_samples)
                continue;

        if (wps->sample_index < wps->wphdr.block_index) {
            samples_to_unpack = wps->wphdr.block_index - wps->sample_index;

            if (samples_to_unpack > 262144) {
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

            if (wpc->reduced_channels)
                samples_to_unpack *= wpc->reduced_channels;
            else
                samples_to_unpack *= num_channels;

            while (samples_to_unpack--)
                *buffer++ = 0;

            continue;
        }

        samples_to_unpack = wps->wphdr.block_index + wps->wphdr.block_samples - wps->sample_index;

        if (samples_to_unpack > samples)
            samples_to_unpack = samples;

        if (!wps->init_done && !unpack_init (wpc))
            wpc->crc_errors++;

        wps->init_done = TRUE;

        if (!wpc->reduced_channels && !(wps->wphdr.flags & FINAL_BLOCK)) {
            int32_t *temp_buffer = malloc (samples_to_unpack * 8), *src, *dst;
            int offset = 0;
            uint32_t samcnt;

	    if (!temp_buffer)
		break;

            while (1) {
                if (wpc->current_stream == wpc->num_streams) {
                    wpc->streams = realloc (wpc->streams, (wpc->num_streams + 1) * sizeof (wpc->streams [0]));
                    if (!wpc->streams)
		        break;
                    wps = wpc->streams [wpc->num_streams++] = malloc (sizeof (WavpackStream));
                    if (!wps)
			break;
                    CLEAR (*wps);
                    bcount = read_next_header (wpc->reader, wpc->wv_in, &wps->wphdr);

                    if (bcount == (uint32_t) -1) {
                        wpc->streams [0]->wphdr.block_samples = 0;
                        wpc->streams [0]->wphdr.ckSize = 24;
                        file_done = TRUE;
                        break;
                    }

                    if (wpc->open_flags & OPEN_STREAMING)
                        wps->wphdr.block_index = wps->sample_index = 0;
                    else
                        wps->wphdr.block_index -= wpc->initial_index;

                    wps->blockbuff = malloc (wps->wphdr.ckSize + 8);
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

                    wps->init_done = FALSE;

                    if (wpc->wvc_flag)
                        read_wvc_block (wpc);

                    if (!wps->init_done && !unpack_init (wpc))
                        wpc->crc_errors++;

                    wps->init_done = TRUE;
                }
                else
                    wps = wpc->streams [wpc->current_stream];

                unpack_samples (wpc, src = temp_buffer, samples_to_unpack);
                samcnt = samples_to_unpack;
                dst = buffer + offset;

                if (wps->wphdr.flags & MONO_FLAG) {
                    while (samcnt--) {
                        dst [0] = *src++;
                        dst += num_channels;
                    }

                    offset++;
                }
                else if (offset == num_channels - 1) {
                    while (samcnt--) {
                        dst [0] = src [0];
                        dst += num_channels;
                        src += 2;
                    }

                    wpc->crc_errors++;
                    offset++;
                }
                else {
                    while (samcnt--) {
                        dst [0] = *src++;
                        dst [1] = *src++;
                        dst += num_channels;
                    }

                    offset += 2;
                }

                if ((wps->wphdr.flags & FINAL_BLOCK) || wpc->current_stream == wpc->max_streams - 1 || offset == num_channels)
                    break;
                else
                    wpc->current_stream++;
            }

            wps = wpc->streams [wpc->current_stream = 0];
            free (temp_buffer);
        }
        else
            unpack_samples (wpc, buffer, samples_to_unpack);

        if (file_done) {
            strcpy (wpc->error_message, "can't read all of last block!");
            break;
        }

        if (wpc->reduced_channels)
            buffer += samples_to_unpack * wpc->reduced_channels;
        else
            buffer += samples_to_unpack * num_channels;

        samples_unpacked += samples_to_unpack;
        samples -= samples_to_unpack;

        if (wps->sample_index == wps->wphdr.block_index + wps->wphdr.block_samples) {
            if (check_crc_error (wpc) && wps->blockbuff) {

                if (wpc->reader->can_seek (wpc->wv_in)) {
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

        if (wpc->total_samples != (uint32_t) -1 && wps->sample_index == wpc->total_samples)
            break;
    }

    return samples_unpacked;
}
