////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//              Copyright (c) 1998 - 2013 Conifer Software.               //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// unpack_seek.c

// This module provides the high-level API for unpacking audio data from
// a specific sample index (i.e., seeking).

#include <stdlib.h>
#include <string.h>

#include "wavpack_local.h"

///////////////////////////// executable code ////////////////////////////////

// Seek to the specifed sample index, returning TRUE on success. Note that
// files generated with version 4.0 or newer will seek almost immediately.
// Older files can take quite long if required to seek through unplayed
// portions of the file, but will create a seek map so that reverse seeks
// (or forward seeks to already scanned areas) will be very fast. After a
// FALSE return the file should not be accessed again (other than to close
// it); this is a fatal error.

int WavpackSeekSample (WavpackContext *wpc, uint32_t sample)
{
    return WavpackSeekSample64 (wpc, sample);
}

int WavpackSeekSample64 (WavpackContext *wpc, int64_t sample)
{
    return FALSE;
}
