////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//                Copyright (c) 1998 - 2022 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// aiff.c

// This module is a helper to the WavPack command-line programs to support AIFF files

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <ctype.h>
#include <limits.h>

#include "wavpack.h"
#include "utils.h"
#include "md5.h"

#define WAVPACK_NO_ERROR    0
#define WAVPACK_SOFT_ERROR  1
#define WAVPACK_HARD_ERROR  2

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

extern int debug_logging_mode;

static double get_extended (uint16_t exponent, uint64_t mantissa)
{
    double sign = (exponent & 0x8000) ? -1.0 : 1.0, value = (double) mantissa;
    double scaler = pow (2.0, (exponent & 0x7fff) - 16446);
    return value * scaler * sign;
}

int ParseAiffHeaderConfig (FILE *infile, char *infilename, char *fourcc, WavpackContext *wpc, WavpackConfig *config)
{
    int common_chunks = 0, version_chunks = 0;
    int64_t total_samples = 0, infilesize;
    RiffChunkHeader aiff_chunk_header;
    ChunkHeader chunk_header;
    CommonChunk common_chunk;
    SoundChunk sound_chunk;
    uint32_t bcount;

    CLEAR (common_chunk);
    CLEAR (sound_chunk);
    infilesize = DoGetFileSize (infile);

    if (infilesize >= 4294967296LL && !(config->qmode & QMODE_IGNORE_LENGTH)) {
        error_line ("can't handle .AIF files larger than 4 GB (non-standard)!");
        return WAVPACK_SOFT_ERROR;
    }

    memcpy (&aiff_chunk_header, fourcc, 4);

    if (!DoReadFile (infile, ((char *) &aiff_chunk_header) + 4, sizeof (RiffChunkHeader) - 4, &bcount)  ||
        bcount != sizeof (RiffChunkHeader) - 4                                                          ||
        (strncmp (aiff_chunk_header.formType, "AIFF", 4) && strncmp (aiff_chunk_header.formType, "AIFC", 4))) {
            error_line ("%s is not a valid .AIF file!", infilename);
            return WAVPACK_SOFT_ERROR;
    }
    else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
        !WavpackAddWrapper (wpc, &aiff_chunk_header, sizeof (RiffChunkHeader))) {
            error_line ("%s", WavpackGetErrorMessage (wpc));
            return WAVPACK_SOFT_ERROR;
    }

    if (debug_logging_mode) {
        WavpackBigEndianToNative (&aiff_chunk_header, ChunkHeaderFormat);
        error_line ("file size = %llu, chunk size in AIF%c header = %u", (unsigned long long) infilesize,
            aiff_chunk_header.formType [3], aiff_chunk_header.ckSize);
    }

    // loop through all elements of the AIFF header
    // (until the sound chunck) and copy them to the output file

    while (1) {
        int padded_chunk_size;

        if (!DoReadFile (infile, &chunk_header, sizeof (ChunkHeader), &bcount) ||
            bcount != sizeof (ChunkHeader)) {
                error_line ("%s is not a valid .AIF%c file!", infilename, aiff_chunk_header.formType [3]);
                return WAVPACK_SOFT_ERROR;
        }
        else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
            !WavpackAddWrapper (wpc, &chunk_header, sizeof (ChunkHeader))) {
                error_line ("%s", WavpackGetErrorMessage (wpc));
                return WAVPACK_SOFT_ERROR;
        }

        WavpackBigEndianToNative (&chunk_header, ChunkHeaderFormat);
        padded_chunk_size = (chunk_header.ckSize + 1) & ~1L;

        if (!strncmp (chunk_header.ckID, "FVER", 4)) {          // if it's the format version chunk, don't be too picky
            uint32_t timestamp;

            if (version_chunks++ || padded_chunk_size != sizeof (timestamp)  ||
                !DoReadFile (infile, &timestamp, padded_chunk_size, &bcount) ||
                bcount != sizeof (timestamp)) {
                    error_line ("%s is not a valid .AIF%c file!", infilename, aiff_chunk_header.formType [3]);
                    return WAVPACK_SOFT_ERROR;
            }
            else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
                !WavpackAddWrapper (wpc, &timestamp, padded_chunk_size)) {
                    error_line ("%s", WavpackGetErrorMessage (wpc));
                    return WAVPACK_SOFT_ERROR;
            }

            // WavpackBigEndianToNative (&timestamp, "L");
        }
        else if (!strncmp (chunk_header.ckID, "COMM", 4)) {     // if it's the common chunk, we want to get some info out of there and
            int supported = TRUE, floatData = FALSE;            // make sure it's a .aiff file we can handle
            double sampleRate;

            if (common_chunks++ || padded_chunk_size < 18 || padded_chunk_size > sizeof (common_chunk) ||
                (aiff_chunk_header.formType [3] == 'F' && padded_chunk_size != 18)                     ||
                !DoReadFile (infile, &common_chunk, padded_chunk_size, &bcount)                        ||
                bcount != padded_chunk_size) {
                    error_line ("%s is not a valid .AIF%c file!", infilename, aiff_chunk_header.formType [3]);
                    return WAVPACK_SOFT_ERROR;
            }
            else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
                !WavpackAddWrapper (wpc, &common_chunk, padded_chunk_size)) {
                    error_line ("%s", WavpackGetErrorMessage (wpc));
                    return WAVPACK_SOFT_ERROR;
            }

            WavpackBigEndianToNative (&common_chunk, CommonChunkFormat);
            sampleRate = get_extended (common_chunk.sampleRateExponent, common_chunk.sampleRateMantissa);

            if (debug_logging_mode) {
                error_line ("common tag size = %d", chunk_header.ckSize);
                error_line ("numChannels = %d, numSampleFrames = %u",
                    common_chunk.numChannels, common_chunk.numSampleFrames);
                error_line ("sampleSize = %d, sampleRate = %g", common_chunk.sampleSize, sampleRate);

                if (chunk_header.ckSize >= 22) {
                    error_line ("compressionType = %c%c%c%c",
                        common_chunk.compressionType [0],
                        common_chunk.compressionType [1],
                        common_chunk.compressionType [2],
                        common_chunk.compressionType [3]);

                    if (chunk_header.ckSize >= 24) {
                        int pstring_len = (unsigned char) common_chunk.compressionName [0];

                        if (pstring_len >= 1 && pstring_len <= (int)(chunk_header.ckSize - 23)) {
                            char compressionName [256];
                            int i, j;

                            for (j = i = 0; i < pstring_len; ++i)
                                if (common_chunk.compressionName [i + 1] >= ' ' && common_chunk.compressionName [i + 1] <= '~')
                                    compressionName [j++] = common_chunk.compressionName [i + 1];

                            compressionName [j] = 0;
                            error_line ("compressionName = \"%s\"", compressionName);
                        }
                    }
                }
            }

            if (chunk_header.ckSize < 22)
                config->qmode |= QMODE_BIG_ENDIAN;
            else if (!strncmp (common_chunk.compressionType, "NONE", 4) || !strncmp (common_chunk.compressionType, "none", 4))
                config->qmode |= QMODE_BIG_ENDIAN;
            else if (!strncmp (common_chunk.compressionType, "FL32", 4) || !strncmp (common_chunk.compressionType, "fl32", 4)) {
                config->qmode |= QMODE_BIG_ENDIAN;
                floatData = TRUE;
            }
            else if (strncmp (common_chunk.compressionType, "SOWT", 4) && strncmp (common_chunk.compressionType, "sowt", 4))
                supported = FALSE;

            if (sampleRate <= 0.0 || sampleRate > 16777215.0)
                supported = FALSE;

            if (floatData && common_chunk.sampleSize != 32)
                supported = FALSE;

            if (!common_chunk.numChannels || common_chunk.numChannels > WAVPACK_MAX_CLI_CHANS)
                supported = FALSE;

            if (common_chunk.sampleSize < 1 || common_chunk.sampleSize > 32)
                supported = FALSE;

            if (!supported) {
                error_line ("%s is an unsupported .AIF%c format!", infilename, aiff_chunk_header.formType [3]);
                return WAVPACK_SOFT_ERROR;
            }

            if (sampleRate != floor (sampleRate))
                error_line ("warning: the nonintegral sample rate of %s will be rounded", infilename);

            if (sampleRate < 1.0)
                config->sample_rate = 1;
            else
                config->sample_rate = (int) floor (sampleRate + 0.5);

            config->bytes_per_sample = (common_chunk.sampleSize + 7) / 8;
            config->bits_per_sample = common_chunk.sampleSize;
            config->num_channels = common_chunk.numChannels;

            if ((config->qmode & QMODE_EVEN_BYTE_DEPTH) && (config->bits_per_sample % 8))
                config->bits_per_sample += 8 - (config->bits_per_sample % 8);

            if (!config->channel_mask && !(config->qmode & QMODE_CHANS_UNASSIGNED)) {
                if (common_chunk.numChannels <= 2)
                    config->channel_mask = 0x5 - common_chunk.numChannels;
                else if (common_chunk.numChannels <= 18)
                    config->channel_mask = (1 << common_chunk.numChannels) - 1;
                else
                    config->channel_mask = 0x3ffff;
            }

            if (common_chunk.sampleSize <= 8)
                config->qmode |= QMODE_SIGNED_BYTES;

            if (floatData)
                config->float_norm_exp = 127;

            if (debug_logging_mode) {
                if (config->float_norm_exp == 127)
                    error_line ("data format: 32-bit big-endian floating point");
                else if (config->bytes_per_sample == 1)
                    error_line ("data format: %d-bit signed integers stored in %d byte",
                        config->bits_per_sample, config->bytes_per_sample);
                else
                    error_line ("data format: %d-bit %s-endian integers stored in %d byte(s)",
                        config->bits_per_sample, (config->qmode & QMODE_BIG_ENDIAN) ? "big" : "little", config->bytes_per_sample);
            }
        }
        else if (!strncmp (chunk_header.ckID, "SSND", 4)) {             // on the data chunk, get size and exit loop
            int64_t data_chunk_size;
            int bytes_per_frame;

            if (!common_chunks || chunk_header.ckSize < sizeof (sound_chunk)      ||
                (!version_chunks && aiff_chunk_header.formType [3] == 'C')        ||
                !DoReadFile (infile, &sound_chunk, sizeof (sound_chunk), &bcount) ||
                bcount != sizeof (sound_chunk)) {
                    error_line ("%s is not a valid .AIF%c file!", infilename, aiff_chunk_header.formType [3]);
                    return WAVPACK_SOFT_ERROR;
            }
            else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
                !WavpackAddWrapper (wpc, &sound_chunk, sizeof (sound_chunk))) {
                    error_line ("%s", WavpackGetErrorMessage (wpc));
                    return WAVPACK_SOFT_ERROR;
            }

            WavpackBigEndianToNative (&sound_chunk, SoundChunkFormat);

            if (sound_chunk.offset || sound_chunk.blockSize) {
                error_line ("%s is an unsupported .AIF%c format!", infilename, aiff_chunk_header.formType [3]);
                return WAVPACK_SOFT_ERROR;
            }

            data_chunk_size = chunk_header.ckSize - sizeof (sound_chunk);
            bytes_per_frame = config->bytes_per_sample * config->num_channels;

            if (infilesize && !(config->qmode & QMODE_IGNORE_LENGTH) && infilesize - data_chunk_size > 16777216) {
                error_line ("this .AIF file has over 16 MB of extra AIF data, probably is corrupt!");
                return WAVPACK_SOFT_ERROR;
            }

            if (config->qmode & QMODE_IGNORE_LENGTH) {
                if (infilesize && DoGetFilePosition (infile) != -1)
                    total_samples = (infilesize - DoGetFilePosition (infile)) / bytes_per_frame;
                else
                    total_samples = -1;
            }
            else {
                total_samples = data_chunk_size / bytes_per_frame;

                if (total_samples != common_chunk.numSampleFrames) {
                    total_samples = (data_chunk_size + sizeof (sound_chunk)) / bytes_per_frame;

                    if (total_samples != common_chunk.numSampleFrames) {
                        error_line ("%s is not a valid .AIF%c file!", infilename);
                        return WAVPACK_SOFT_ERROR;
                    }
                    else
                        error_line ("warning: %s has a malformed chunk size which will be ignored", infilename);
                }

                if (!total_samples) {
                    error_line ("%s has no audio samples, probably is corrupt!", infilename);
                    return WAVPACK_SOFT_ERROR;
                }

                if (total_samples > MAX_WAVPACK_SAMPLES) {
                    error_line ("%s has too many samples for WavPack!", infilename);
                    return WAVPACK_SOFT_ERROR;
                }
            }

            break;
        }
        else {          // just copy unknown chunks to output file
            char *buff;

            if (padded_chunk_size < 0 || padded_chunk_size > 4194304) {
                error_line ("%s is not a valid .AIF%c file!", infilename, aiff_chunk_header.formType [3]);
                return WAVPACK_SOFT_ERROR;
            }

            buff = malloc (padded_chunk_size);

            if (debug_logging_mode)
                error_line ("extra unknown chunk \"%c%c%c%c\" of %d bytes",
                    chunk_header.ckID [0], chunk_header.ckID [1], chunk_header.ckID [2],
                    chunk_header.ckID [3], chunk_header.ckSize);

            if (!DoReadFile (infile, buff, padded_chunk_size, &bcount) || bcount != padded_chunk_size ||
                (!(config->qmode & QMODE_NO_STORE_WRAPPER) && !WavpackAddWrapper (wpc, buff, padded_chunk_size))) {
                    error_line ("%s", WavpackGetErrorMessage (wpc));
                    free (buff);
                    return WAVPACK_SOFT_ERROR;
            }

            free (buff);
        }
    }

    if (!WavpackSetConfiguration64 (wpc, config, total_samples, NULL)) {
        error_line ("%s: %s", infilename, WavpackGetErrorMessage (wpc));
        return WAVPACK_SOFT_ERROR;
    }

    return WAVPACK_NO_ERROR;
}
