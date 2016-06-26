////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//                Copyright (c) 1998 - 2016 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// dsf.c

// This module is a helper to the WavPack command-line programs to support DSF files.

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <ctype.h>

#include "wavpack.h"
#include "utils.h"
#include "md5.h"

#ifdef _WIN32
#define strdup(x) _strdup(x)
#endif

#define WAVPACK_NO_ERROR    0
#define WAVPACK_SOFT_ERROR  1
#define WAVPACK_HARD_ERROR  2

extern int debug_logging_mode;

#pragma pack(push,4)

typedef struct
{
    char ckID [4];
    int64_t ckSize;
} DSFChunkHeader;

typedef struct
{
    char ckID [4];
    int64_t ckSize;
    int64_t fileSize;
    int64_t metaOffset;
} DSFFileChunk;

typedef struct
{
    char ckID [4];
    int64_t ckSize;
    uint32_t formatVersion, formatID;
    uint32_t chanType, numChannels, sampleRate, bitsPerSample;
    int64_t sampleCount;
    uint32_t blockSize, reserved;
} DSFFormatChunk;

#pragma pack(pop)

#define DSFChunkHeaderFormat "4D"
#define DSFFileChunkFormat "4DDD"
#define DSFFormatChunkFormat "4DLLLLLLDL4"

int ParseDsfHeaderConfig (FILE *infile, char *infilename, char *fourcc, WavpackContext *wpc, WavpackConfig *config)
{
    int64_t infilesize, total_samples, total_blocks, leftover_samples;
    DSFFileChunk file_chunk;
    DSFFormatChunk format_chunk;
    DSFChunkHeader chunk_header;

    uint32_t bcount;
    int i;

    infilesize = DoGetFileSize (infile);
    memcpy (&file_chunk, fourcc, 4);

    if ((!DoReadFile (infile, ((char *) &file_chunk) + 4, sizeof (DSFFileChunk) - 4, &bcount) ||
        bcount != sizeof (DSFFileChunk) - 4)) {
            error_line ("%s is not a valid .DSF file!", infilename);
            return WAVPACK_SOFT_ERROR;
    }
    else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
        !WavpackAddWrapper (wpc, &file_chunk, sizeof (DSFFileChunk))) {
            error_line ("%s", WavpackGetErrorMessage (wpc));
            return WAVPACK_SOFT_ERROR;
    }

#if 1   // this might be a little too picky...
    WavpackLittleEndianToNative (&file_chunk, DSFFileChunkFormat);

    if (debug_logging_mode)
        error_line ("file header lengths = %lld, %lld, %lld", file_chunk.ckSize, file_chunk.fileSize, file_chunk.metaOffset);

    if (infilesize && !(config->qmode & QMODE_IGNORE_LENGTH) &&
        file_chunk.fileSize && file_chunk.fileSize + 1 && file_chunk.fileSize != infilesize) {
            error_line ("%s is not a valid .DSF file (by total size)!", infilename);
            return WAVPACK_SOFT_ERROR;
    }
#endif

    if (!DoReadFile (infile, ((char *) &format_chunk), sizeof (DSFFormatChunk), &bcount) ||
        bcount != sizeof (DSFFormatChunk) || strncmp (format_chunk.ckID, "fmt ", 4)) {
            error_line ("%s is not a valid .DSF file!", infilename);
            return WAVPACK_SOFT_ERROR;
    }
    else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
        !WavpackAddWrapper (wpc, &format_chunk, sizeof (DSFFormatChunk))) {
            error_line ("%s", WavpackGetErrorMessage (wpc));
            return WAVPACK_SOFT_ERROR;
    }

    WavpackLittleEndianToNative (&format_chunk, DSFFormatChunkFormat);

    if (format_chunk.ckSize != sizeof (DSFFormatChunk) || format_chunk.formatVersion != 1 ||
        format_chunk.formatID != 0) {
            error_line ("%s is not a valid .DSF file!", infilename);
            return WAVPACK_SOFT_ERROR;
    }

    if (debug_logging_mode) {
        error_line ("sampling rate = %d Hz", format_chunk.sampleRate);
        error_line ("channel type = %d, channel count = %d", format_chunk.chanType, format_chunk.numChannels);
        error_line ("block size = %d, bits per sample = %d", format_chunk.blockSize, format_chunk.bitsPerSample);
        error_line ("sample count = %lld", format_chunk.sampleCount);
    }

    if (!DoReadFile (infile, ((char *) &chunk_header), sizeof (DSFChunkHeader), &bcount) ||
        bcount != sizeof (DSFChunkHeader) || strncmp (chunk_header.ckID, "data", 4)) {
            error_line ("%s is not a valid .DSF file!", infilename);
            return WAVPACK_SOFT_ERROR;
    }
    else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
        !WavpackAddWrapper (wpc, &chunk_header, sizeof (DSFChunkHeader))) {
            error_line ("%s", WavpackGetErrorMessage (wpc));
            return WAVPACK_SOFT_ERROR;
    }

    WavpackLittleEndianToNative (&chunk_header, DSFChunkHeaderFormat);

    total_samples = format_chunk.sampleCount;
    total_blocks = total_samples / (format_chunk.blockSize * 8);
    leftover_samples = total_samples - (total_blocks * format_chunk.blockSize * 8);

    if (leftover_samples)
        total_blocks++;

    if (debug_logging_mode) {
        error_line ("data chunk size (fixed) = %lld", chunk_header.ckSize - 12);
        error_line ("alternate data chunk size = %lld", total_blocks * 4096 * format_chunk.numChannels);
    }

    config->bits_per_sample = 8;
    config->bytes_per_sample = 1;
    config->num_channels = format_chunk.numChannels;

    if (config->num_channels <= 2)
        config->channel_mask = 0x5 - config->num_channels;
    else
        config->channel_mask = (1 << config->num_channels) - 1;

    config->sample_rate = format_chunk.sampleRate / 8;

    if (!WavpackSetConfiguration64 (wpc, config, total_blocks * 4096)) {
        error_line ("%s: %s", infilename, WavpackGetErrorMessage (wpc));
        return WAVPACK_SOFT_ERROR;
    }

    return WAVPACK_NO_ERROR;
}

