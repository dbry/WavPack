////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//                Copyright (c) 1998 - 2019 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// open_memfile.c

// This code provides the ability to decode entire WavPack files directly
// from memory for use in a fuzzing application, for example.

#include <stdlib.h>
#include <string.h>

#include "wavpack_local.h"

typedef struct {
    unsigned char ungetc_char, ungetc_flag;
    unsigned char *sptr, *dptr, *eptr;
    int64_t total_bytes_read;
} WavpackRawContext;

static int32_t raw_read_bytes (void *id, void *data, int32_t bcount)
{
    WavpackRawContext *rcxt = id;
    unsigned char *outptr = data;

    while (bcount) {
        if (rcxt->ungetc_flag) {
            *outptr++ = rcxt->ungetc_char;
            rcxt->ungetc_flag = 0;
            bcount--;
        }
        else {
            size_t bytes_to_copy = rcxt->eptr - rcxt->dptr;

            if (!bytes_to_copy)
                break;

            if (bytes_to_copy > bcount)
                bytes_to_copy = bcount;

            memcpy (outptr, rcxt->dptr, bytes_to_copy);
            rcxt->total_bytes_read += bytes_to_copy;
            rcxt->dptr += bytes_to_copy;
            outptr += bytes_to_copy;
            bcount -= bytes_to_copy;
        }
    }

    return (int32_t)(outptr - (unsigned char *) data);
}

static int32_t raw_write_bytes (void *id, void *data, int32_t bcount)
{
    return 0;
}

static int64_t raw_get_pos (void *id)
{
    WavpackRawContext *rcxt = id;
    return rcxt->dptr - rcxt->sptr;
}

static int raw_set_pos_abs (void *id, int64_t pos)
{
    WavpackRawContext *rcxt = id;

    if (rcxt->sptr + pos < rcxt->sptr || rcxt->sptr + pos > rcxt->eptr)
        return 1;

    rcxt->dptr = rcxt->sptr + pos;
    return 0;
}

static int raw_set_pos_rel (void *id, int64_t delta, int mode)
{
    WavpackRawContext *rcxt = id;
    unsigned char *ref = NULL;

    if (mode == SEEK_SET)
        ref = rcxt->sptr;
    else if (mode == SEEK_CUR)
        ref = rcxt->dptr;
    else if (mode == SEEK_END)
        ref = rcxt->eptr;

    if (ref + delta < rcxt->sptr || ref + delta > rcxt->eptr)
        return 1;

    rcxt->dptr = ref + delta;
    return 0;
}

static int raw_push_back_byte (void *id, int c)
{
    WavpackRawContext *rcxt = id;
    rcxt->ungetc_char = c;
    rcxt->ungetc_flag = 1;
    return c;
}

static int64_t raw_get_length (void *id)
{
    WavpackRawContext *rcxt = id;
    return rcxt->eptr - rcxt->sptr;
}

static int raw_can_seek (void *id)
{
    return 1;
}

static int raw_close_stream (void *id)
{
    WavpackRawContext *rcxt = id;

    // fprintf (stderr, "\nmemfile: total bytes read = %lld\n", (long long) rcxt->total_bytes_read);

    if (rcxt)
        free (rcxt);

    return 0;
}

static WavpackStreamReader64 raw_reader = {
    raw_read_bytes, raw_write_bytes, raw_get_pos, raw_set_pos_abs, raw_set_pos_rel,
    raw_push_back_byte, raw_get_length, raw_can_seek, NULL, raw_close_stream
};

// This function is similar to WavpackOpenFileInput() except that instead of
// providing a filename to open, the caller provides pointers to buffered
// WavPack files (both standard and, optionally, correction data).

WavpackContext *WavpackOpenMemoryFile (
    void *main_data, size_t main_size,
    void *corr_data, size_t corr_size,
    char *error, int flags, int norm_offset)
{
    WavpackRawContext *raw_wv = NULL, *raw_wvc = NULL;

    if (main_data) {
        raw_wv = calloc (1, sizeof (WavpackRawContext));
        raw_wv->dptr = raw_wv->sptr = main_data;
        raw_wv->eptr = raw_wv->dptr + main_size;
    }

    if (corr_data && corr_size) {
        raw_wvc = calloc (1, sizeof (WavpackRawContext));
        raw_wvc->dptr = raw_wvc->sptr = corr_data;
        raw_wvc->eptr = raw_wvc->dptr + corr_size;
    }

    return WavpackOpenFileInputEx64 (&raw_reader, raw_wv, raw_wvc, error, flags, norm_offset);
}
