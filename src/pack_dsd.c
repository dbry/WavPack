////////////////////////////////////////////////////////////////////////////
//                           **** DSDPACK ****                            //
//         Lossless DSD (Direct Stream Digital) Audio Compressor          //
//                Copyright (c) 2013 - 2016 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// pack_dsd.c

// This module actually handles the compression of the DSD audio data.

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "wavpack_local.h"

///////////////////////////// executable code ////////////////////////////////

// This function initializes everything required to pack WavPack DSD bitstreams
// and must be called BEFORE any other function in this module.

void pack_dsd_init (WavpackContext *wpc)
{
    WavpackStream *wps = wpc->streams [wpc->current_stream];

    wps->sample_index = 0;
}

// Pack an entire block of samples (either mono or stereo) into a completed
// WavPack block. This function is actually a shell for pack_samples() and
// performs tasks like handling any shift required by the format, preprocessing
// of floating point data or integer data over 24 bits wide, and implementing
// the "extra" mode (via the extra?.c modules). It is assumed that there is
// sufficient space for the completed block at "wps->blockbuff" and that
// "wps->blockend" points to the end of the available space. A return value of
// FALSE indicates an error.

static int pack_dsd_samples (WavpackContext *wpc, int32_t *buffer);

int pack_dsd_block (WavpackContext *wpc, int32_t *buffer)
{
    return pack_dsd_samples (wpc, buffer);
}

// Pack an entire block of samples (either mono or stereo) into a completed
// WavPack block. It is assumed that there is sufficient space for the
// completed block at "wps->blockbuff" and that "wps->blockend" points to the
// end of the available space. A return value of FALSE indicates an error.
// Any unsent metadata is transmitted first, then required metadata for this
// block is sent, and finally the compressed integer data is sent. If a "wpx"
// stream is required for floating point data or large integer data, then this
// must be handled outside this function. To find out how much data was written
// the caller must look at the ckSize field of the written WavpackHeader, NOT
// the one in the WavpackStream.

static int pack_dsd_samples (WavpackContext *wpc, int32_t *buffer)
{
    WavpackStream *wps = wpc->streams [wpc->current_stream];
    uint32_t flags = wps->wphdr.flags, data_count, bc;
    uint32_t sample_count = wps->wphdr.block_samples;
    unsigned char *dsd_encoding;
    WavpackMetadata wpmd;

    wps->wphdr.ckSize = sizeof (WavpackHeader) - 8;
    memcpy (wps->blockbuff, &wps->wphdr, sizeof (WavpackHeader));

    if (wpc->metacount) {
        WavpackMetadata *wpmdp = wpc->metadata;

        while (wpc->metacount) {
            copy_metadata (wpmdp, wps->blockbuff, wps->blockend);
            wpc->metabytes -= wpmdp->byte_length;
            free_metadata (wpmdp++);
            wpc->metacount--;
        }

        free (wpc->metadata);
        wpc->metadata = NULL;
    }

    if (!sample_count)
        return TRUE;

    memcpy (&wps->wphdr, wps->blockbuff, sizeof (WavpackHeader));

    dsd_encoding = wps->blockbuff + ((WavpackHeader *) wps->blockbuff)->ckSize + 12;
    bc = data_count = sample_count * ((flags & MONO_FLAG) ? 1 : 2);

    while (bc--)
        *dsd_encoding++ = *buffer++;

    if (data_count) {
        unsigned char *cptr = wps->blockbuff + ((WavpackHeader *) wps->blockbuff)->ckSize + 8;

        if (data_count & 1)
            data_count++;

        *cptr++ = ID_DSD_BLOCK | ID_LARGE;
        *cptr++ = data_count >> 1;
        *cptr++ = data_count >> 9;
        *cptr++ = data_count >> 17;
        ((WavpackHeader *) wps->blockbuff)->ckSize += data_count + 4;
    }

    wps->sample_index += sample_count;
    return TRUE;
}
