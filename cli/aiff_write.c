////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//                Copyright (c) 1998 - 2024 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// aiff-write.c

// This module is a helper to the WavPack command-line programs to support AIFF files

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <ctype.h>

#include "wavpack.h"
#include "utils.h"
#include "md5.h"

#pragma pack(push,2)
typedef struct {
    uint16_t numChannels;
    uint32_t numSampleFrames;
    uint16_t sampleSize;
    uint16_t sampleRateExponent;
    uint64_t sampleRateMantissa;
    char compressionType [4];
    char compressionName [256-22];
} CommonChunk;
#pragma pack(pop)

#define CommonChunkFormat "SLSSD"

typedef struct {
    uint32_t offset;
    uint32_t blockSize;
} SoundChunk;

#define SoundChunkFormat "LL"

#define AIFCVersion1 0xA2805140 // Version 1 of AIFF-C
                                // this is 2726318400 in decimal

extern int debug_logging_mode;

static void put_extended (int value, uint16_t *exponent, uint64_t *mantissa)
{
    if (value == 0) {
        *exponent = 0;
        *mantissa = 0;
        return;
    }

    *exponent = value < 0 ? 16446 | 0x8000 : 16446;
    *mantissa = abs (value);

    while (!(*mantissa & 0x8000000000000000ULL)) {
        *mantissa <<= 1;
        (*exponent)--;
    }
}

int WriteAiffHeader (FILE *outfile, WavpackContext *wpc, int64_t total_samples, int qmode)
{
    ChunkHeader version_header, common_header, sound_header;
    uint32_t bcount, timestamp = AIFCVersion1;
    int aifc = 0, common_chunk_size = 18;
    RiffChunkHeader aiff_header;
    CommonChunk common_chunk;
    SoundChunk sound_chunk;

    int64_t total_data_bytes, total_aiff_bytes;
    int num_channels = WavpackGetNumChannels (wpc);
    uint32_t sample_rate = WavpackGetSampleRate (wpc);
    int bytes_per_sample = WavpackGetBytesPerSample (wpc);
    int bits_per_sample = WavpackGetBitsPerSample (wpc);
    int float_format = WavpackGetFloatNormExp (wpc);
    uint64_t mantissa;

    if (float_format) {
        if (float_format != 127 || !(qmode & QMODE_BIG_ENDIAN)) {
            error_line ("can't create valid AIF header for non-normalized or little-endian floating data!");
            return FALSE;
        }

        aifc = 1;
    }
    else if (bits_per_sample > 8 && !(qmode & QMODE_BIG_ENDIAN))
        aifc = 1;

    if (total_samples == -1)
        total_samples = 0x7ffff000 / (bytes_per_sample * num_channels);

    total_data_bytes = total_samples * bytes_per_sample * num_channels;

    if (total_data_bytes > 0xff000000) {
        error_line ("can't create valid AIF header for long file, total_data_bytes = %lld", total_data_bytes);
        return FALSE;
    }

    if (aifc) {
        char *compressionType, *compressionName;

        if (float_format) {
            compressionType = "fl32";
            compressionName = "IEEE 32-bit float";
        }
        else if (bits_per_sample > 8 && !(qmode & QMODE_BIG_ENDIAN)) {
            compressionType = "sowt";
            compressionName = "";
        }
        else {
            compressionType = "NONE";
            compressionName = "not compressed";
        }

        memcpy (common_chunk.compressionType, compressionType, sizeof (common_chunk.compressionType));
        common_chunk_size += 4;
        common_chunk.compressionName [0] = (char) strlen (compressionName);
        common_chunk_size++;
        strcpy (common_chunk.compressionName + 1, compressionName);
        common_chunk_size += (unsigned char) common_chunk.compressionName [0];
        common_chunk_size += common_chunk_size & 1;

        memcpy (version_header.ckID, "FVER", sizeof (version_header.ckID));
        version_header.ckSize = sizeof (timestamp);
        WavpackNativeToBigEndian (&version_header, ChunkHeaderFormat);
        WavpackNativeToBigEndian (&timestamp, "L");
    }

    total_aiff_bytes = sizeof (aiff_header) + sizeof (common_header) + common_chunk_size + sizeof (sound_header) + sizeof (sound_chunk);
    if (aifc) total_aiff_bytes += sizeof (version_header) + sizeof (timestamp);
    total_aiff_bytes += (total_data_bytes + 1) & ~(int64_t)1;

    memcpy (aiff_header.ckID, "FORM", sizeof (aiff_header.ckID));
    aiff_header.ckSize = (uint32_t)(total_aiff_bytes - sizeof (ChunkHeader));
    memcpy (aiff_header.formType, aifc ? "AIFC" : "AIFF", sizeof (aiff_header.formType));
    WavpackNativeToBigEndian (&aiff_header, ChunkHeaderFormat);

    memcpy (common_header.ckID, "COMM", sizeof (common_header.ckID));
    common_header.ckSize = common_chunk_size;
    WavpackNativeToBigEndian (&common_header, ChunkHeaderFormat);

    common_chunk.numChannels = num_channels;
    common_chunk.numSampleFrames = (uint32_t) total_samples;
    common_chunk.sampleSize = bits_per_sample;
    put_extended (sample_rate, &common_chunk.sampleRateExponent, &mantissa);    // mantissa is not properly aligned, so use local
    memcpy (&common_chunk.sampleRateMantissa, &mantissa, sizeof (mantissa));
    WavpackNativeToBigEndian (&common_chunk, CommonChunkFormat);

    memcpy (sound_header.ckID, "SSND", sizeof (sound_header.ckID));
    sound_header.ckSize = (uint32_t)(sizeof (sound_chunk) + total_data_bytes);
    WavpackNativeToBigEndian (&sound_header, ChunkHeaderFormat);

    sound_chunk.offset = sound_chunk.blockSize = 0;
    WavpackNativeToBigEndian (&sound_chunk, SoundChunkFormat);

    if (!DoWriteFile (outfile, &aiff_header, sizeof (aiff_header), &bcount) || bcount != sizeof (aiff_header)                      ||
        (aifc && (!DoWriteFile (outfile, &version_header, sizeof (version_header), &bcount) || bcount != sizeof (version_header))) ||
        (aifc && (!DoWriteFile (outfile, &timestamp, sizeof (timestamp), &bcount) || bcount != sizeof (timestamp)))                ||
        !DoWriteFile (outfile, &common_header, sizeof (common_header), &bcount) || bcount != sizeof (common_header)                ||
        !DoWriteFile (outfile, &common_chunk, common_chunk_size, &bcount) || bcount != common_chunk_size                           ||
        !DoWriteFile (outfile, &sound_header, sizeof (sound_header), &bcount) || bcount != sizeof (sound_header)                   ||
        !DoWriteFile (outfile, &sound_chunk, sizeof (sound_chunk), &bcount) || bcount != sizeof (sound_chunk)) {
            error_line ("can't write .AIF data, disk probably full!");
            return FALSE;
    }

    return TRUE;
}

