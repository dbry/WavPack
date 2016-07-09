////////////////////////////////////////////////////////////////////////////
//                           **** DSDPACK ****                            //
//         Lossless DSD (Direct Stream Digital) Audio Compressor          //
//                Copyright (c) 2013 - 2016 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// unpack_dsd.c

// This module actually handles the uncompression of the DSD audio data.

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "wavpack_local.h"

///////////////////////////// executable code ////////////////////////////////

// This function initialzes the main range-encoded data for DSD audio samples

int init_dsd_block (WavpackStream *wps, WavpackMetadata *wpmd)
{
    if (!wpmd->byte_length)
        return FALSE;

    wps->dsd_data_ptr = wpmd->data;
    wps->dsd_data_bcount = wpmd->byte_length;
    wps->dsd_data_index = 0;

    return TRUE;
}

int32_t unpack_dsd_samples (WavpackContext *wpc, int32_t *buffer, uint32_t sample_count)
{
    WavpackStream *wps = wpc->streams [wpc->current_stream];
    uint32_t flags = wps->wphdr.flags, crc = wps->crc;
    int bytes_to_copy;

    // don't attempt to decode past the end of the block, but watch out for overflow!

    if (wps->sample_index + sample_count > GET_BLOCK_INDEX (wps->wphdr) + wps->wphdr.block_samples &&
        GET_BLOCK_INDEX (wps->wphdr) + wps->wphdr.block_samples - wps->sample_index < sample_count)
            sample_count = (uint32_t) (GET_BLOCK_INDEX (wps->wphdr) + wps->wphdr.block_samples - wps->sample_index);

    if (GET_BLOCK_INDEX (wps->wphdr) > wps->sample_index || wps->wphdr.block_samples < sample_count)
        wps->mute_error = TRUE;

    if (wps->mute_error) {
        if (wpc->reduced_channels == 1 || wpc->config.num_channels == 1 || (flags & MONO_FLAG))
            memset (buffer, 0, sample_count * 4);
        else
            memset (buffer, 0, sample_count * 8);

        wps->sample_index += sample_count;
        return sample_count;
    }

    bytes_to_copy = sample_count * ((flags & MONO_FLAG) ? 1 : 2);

    while (bytes_to_copy-- && wps->dsd_data_ptr && wps->dsd_data_index < wps->dsd_data_bcount)
        *buffer++ = wps->dsd_data_ptr [wps->dsd_data_index++];

    wps->sample_index += sample_count;
    wps->crc = wps->wphdr.crc;

    return sample_count;
}
