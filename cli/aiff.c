////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//                Copyright (c) 1998 - 2018 David Bryant.                 //
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

static int get_extended (uint16_t exponent, uint64_t mantissa)
{
    if (exponent & 0x8000)
        return -(mantissa >> (16446 - (exponent & 0x7fff)));
    else
        return mantissa >> (16446 - exponent);
}

static void put_extended (int value, uint16_t *exponent, uint64_t *mantissa)
{
    if (value == 0) {
        *exponent = 0;
        *mantissa = 0;
        return;
    }

    *exponent = value < 0 ? 16446 | 0x8000 : 16446;
    *mantissa = abs (value);

    while (!(*mantissa & 0x8000000000000000)) {
        *mantissa <<= 1;
        (*exponent)--;
    }
}

static void test_extended (void)
{
    uint64_t mantissa;
    uint16_t exponent;
    int value, result;

    for (value = -10000000; value < 10000000; ++value) {
        put_extended (value, &exponent, &mantissa);
        result = get_extended (exponent, mantissa);

        if (result != value) {
            printf ("value = %d, result = %d\n", value, result);
            break;
        }
    }
}

int ParseAiffHeaderConfig (FILE *infile, char *infilename, char *fourcc, WavpackContext *wpc, WavpackConfig *config)
{
    int format_chunk = 0;
    int64_t total_samples = 0, infilesize;
    RiffChunkHeader aiff_chunk_header;
    ChunkHeader chunk_header;
    CommonChunk common_chunk;
    SoundChunk sound_chunk;
    uint32_t bcount;

    test_extended ();

    CLEAR (common_chunk);
    CLEAR (sound_chunk);
    infilesize = DoGetFileSize (infile);

    if (infilesize >= 4294967296LL && !(config->qmode & QMODE_IGNORE_LENGTH)) {
        error_line ("can't handle .AIF files larger than 4 GB (non-standard)!");
        return WAVPACK_SOFT_ERROR;
    }

    memcpy (&aiff_chunk_header, fourcc, 4);

    if ((!DoReadFile (infile, ((char *) &aiff_chunk_header) + 4, sizeof (RiffChunkHeader) - 4, &bcount) ||
        bcount != sizeof (RiffChunkHeader) - 4 || strncmp (aiff_chunk_header.formType, "AIFF", 4))) {
            error_line ("%s is not a valid .AIFF file (1)!", infilename);
            return WAVPACK_SOFT_ERROR;
    }
    else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
        !WavpackAddWrapper (wpc, &aiff_chunk_header, sizeof (RiffChunkHeader))) {
            error_line ("%s", WavpackGetErrorMessage (wpc));
            return WAVPACK_SOFT_ERROR;
    }

    // loop through all elements of the AIFF header
    // (until the sound chuck) and copy them to the output file

    while (1) {
        if (!DoReadFile (infile, &chunk_header, sizeof (ChunkHeader), &bcount) ||
            bcount != sizeof (ChunkHeader)) {
                error_line ("%s is not a valid .AIFF file (2)!", infilename);
                return WAVPACK_SOFT_ERROR;
        }
        else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
            !WavpackAddWrapper (wpc, &chunk_header, sizeof (ChunkHeader))) {
                error_line ("%s", WavpackGetErrorMessage (wpc));
                return WAVPACK_SOFT_ERROR;
        }

        WavpackBigEndianToNative (&chunk_header, ChunkHeaderFormat);

        if (!strncmp (chunk_header.ckID, "COMM", 4)) {          // if it's the common chunk, we want to get some info out of there and
            int supported = TRUE, format;                       // make sure it's a .aiff file we can handle

            if (format_chunk++) {
                error_line ("%s is not a valid .AIFF file (3)!", infilename);
                return WAVPACK_SOFT_ERROR;
            }

            if (chunk_header.ckSize < 18 || chunk_header.ckSize > sizeof (common_chunk) ||
                !DoReadFile (infile, &common_chunk, chunk_header.ckSize, &bcount) ||
                bcount != chunk_header.ckSize) {
                    error_line ("%s is not a valid .WAV file (4)!", infilename);
                    return WAVPACK_SOFT_ERROR;
            }
            else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
                !WavpackAddWrapper (wpc, &common_chunk, chunk_header.ckSize)) {
                    error_line ("%s", WavpackGetErrorMessage (wpc));
                    return WAVPACK_SOFT_ERROR;
            }

            WavpackBigEndianToNative (&common_chunk, CommonChunkFormat);

            if (debug_logging_mode) {
                error_line ("common tag size = %d", chunk_header.ckSize);
                error_line ("numChannels = %d, numSampleFrames = %u",
                    common_chunk.numChannels, common_chunk.numSampleFrames);
                error_line ("sampleSize = %d, sampleRate = %d", common_chunk.sampleSize,
                    get_extended (common_chunk.sampleRateExponent, common_chunk.sampleRateMantissa));

                if (chunk_header.ckSize >= 22)
                    error_line ("compressionType = %c%c%c%c, compressionName = %s",
                        common_chunk.compressionType [0],
                        common_chunk.compressionType [1],
                        common_chunk.compressionType [2],
                        common_chunk.compressionType [3],
                        common_chunk.compressionName);
            }

            if (chunk_header.ckSize < 22) {
                config->qmode |= QMODE_BIG_ENDIAN;
                format = 1;
            }
            else if (!strncmp (common_chunk.compressionType, "NONE", 4) || !strncmp (common_chunk.compressionType, "none", 4)) {
                config->qmode |= QMODE_BIG_ENDIAN;
                format = 1;
            }
            else if (!strncmp (common_chunk.compressionType, "FL32", 4) || !strncmp (common_chunk.compressionType, "fl32", 4)) {
                config->qmode |= QMODE_BIG_ENDIAN;
                format = 3;
            }
            else if (!strncmp (common_chunk.compressionType, "SOWT", 4) || !strncmp (common_chunk.compressionType, "sowt", 4))
                format = 1;
            else
                supported = FALSE;

            config->bits_per_sample = common_chunk.sampleSize;

            if (format != 1 && format != 3)
                supported = FALSE;

            if (format == 3 && config->bits_per_sample != 32)
                supported = FALSE;

            if (!common_chunk.numChannels || common_chunk.numChannels > 256)
                supported = FALSE;

            if (config->bits_per_sample < 1 || config->bits_per_sample > 32)
                supported = FALSE;

            if (!supported) {
                error_line ("%s is an unsupported .AIFF format!", infilename);
                return WAVPACK_SOFT_ERROR;
            }

            config->num_channels = common_chunk.numChannels;
            config->channel_mask = 0x5 - common_chunk.numChannels;
            config->bytes_per_sample = (common_chunk.sampleSize + 7) / 8;
            config->sample_rate = get_extended (common_chunk.sampleRateExponent, common_chunk.sampleRateMantissa);

            if (format == 3)
                config->float_norm_exp = 127;

            if (debug_logging_mode) {
                if (config->float_norm_exp == 127)
                    error_line ("data format: 32-bit %s-endian floating point", (config->qmode & QMODE_BIG_ENDIAN) ? "big" : "little");
                else
                    error_line ("data format: %d-bit %s-endian integers stored in %d byte(s)",
                        config->bits_per_sample, (config->qmode & QMODE_BIG_ENDIAN) ? "big" : "little", config->bytes_per_sample);
                error_line ("sample rate = %d\n", config->sample_rate);
            }
        }
        else if (!strncmp (chunk_header.ckID, "SSND", 4)) {             // on the data chunk, get size and exit loop

            if (!format_chunk || chunk_header.ckSize < sizeof (sound_chunk) ||
                !DoReadFile (infile, &sound_chunk, sizeof (sound_chunk), &bcount) ||
                bcount != sizeof (sound_chunk)) {
                    error_line ("%s is not a valid .AIFF file (5)!", infilename);
                    return WAVPACK_SOFT_ERROR;
            }
            else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
                !WavpackAddWrapper (wpc, &sound_chunk, sizeof (sound_chunk))) {
                    error_line ("%s", WavpackGetErrorMessage (wpc));
                    return WAVPACK_SOFT_ERROR;
            }

            WavpackBigEndianToNative (&sound_chunk, SoundChunkFormat);

            if (debug_logging_mode)
                printf ("got sound chunk, offset = %u, blockSize = %u\n", sound_chunk.offset, sound_chunk.blockSize);

            int64_t data_chunk_size = chunk_header.ckSize - sizeof (sound_chunk);
            int bytes_per_frame = config->bytes_per_sample * config->num_channels;

            if (infilesize && !(config->qmode & QMODE_IGNORE_LENGTH) && infilesize - data_chunk_size > 16777216) {
                error_line ("this .AIFF file has over 16 MB of extra AIFF data, probably is corrupt!");
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
                    error_line ("%s is not a valid .AIFF file (6)!", infilename);
                    return WAVPACK_SOFT_ERROR;
                }

                if (!total_samples) {
                    error_line ("this .AIFF file has no audio samples, probably is corrupt!");
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

            int bytes_to_copy = (chunk_header.ckSize + 1) & ~1L;
            char *buff;

            if (bytes_to_copy < 0 || bytes_to_copy > 4194304) {
                error_line ("%s is not a valid .AIFF file (7)!", infilename);
                return WAVPACK_SOFT_ERROR;
            }

            buff = malloc (bytes_to_copy);

            if (debug_logging_mode)
                error_line ("extra unknown chunk \"%c%c%c%c\" of %d bytes",
                    chunk_header.ckID [0], chunk_header.ckID [1], chunk_header.ckID [2],
                    chunk_header.ckID [3], chunk_header.ckSize);

            if (!DoReadFile (infile, buff, bytes_to_copy, &bcount) ||
                bcount != bytes_to_copy ||
                (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
                !WavpackAddWrapper (wpc, buff, bytes_to_copy))) {
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
    else
        error_line ("configure with %lld samples seemed to go okay\n", total_samples);

    return WAVPACK_NO_ERROR;
}

int WriteAiffHeader (FILE *outfile, WavpackContext *wpc, int64_t total_samples, int qmode)
{
#if 0
    int do_rf64 = 0, write_junk = 1, table_length = 0;
    ChunkHeader ds64hdr, datahdr, fmthdr;
    RiffChunkHeader riffhdr;
    DS64Chunk ds64_chunk;
    CS64Chunk cs64_chunk;
    JunkChunk junkchunk;
    WaveHeader wavhdr;
    uint32_t bcount;

    int64_t total_data_bytes, total_riff_bytes;
    int num_channels = WavpackGetNumChannels (wpc);
    int32_t channel_mask = WavpackGetChannelMask (wpc);
    int32_t sample_rate = WavpackGetSampleRate (wpc);
    int bytes_per_sample = WavpackGetBytesPerSample (wpc);
    int bits_per_sample = WavpackGetBitsPerSample (wpc);
    int format = WavpackGetFloatNormExp (wpc) ? 3 : 1;
    int wavhdrsize = 16;

    if (format == 3 && WavpackGetFloatNormExp (wpc) != 127) {
        error_line ("can't create valid RIFF wav header for non-normalized floating data!");
        return FALSE;
    }

    if (total_samples == -1)
        total_samples = 0x7ffff000 / (bytes_per_sample * num_channels);

    total_data_bytes = total_samples * bytes_per_sample * num_channels;

    if (total_data_bytes > 0xff000000) {
        if (debug_logging_mode)
            error_line ("total_data_bytes = %lld, so rf64", total_data_bytes);
        write_junk = 0;
        do_rf64 = 1;
    }
    else if (debug_logging_mode)
        error_line ("total_data_bytes = %lld, so riff", total_data_bytes);

    CLEAR (wavhdr);

    wavhdr.FormatTag = format;
    wavhdr.NumChannels = num_channels;
    wavhdr.SampleRate = sample_rate;
    wavhdr.BytesPerSecond = sample_rate * num_channels * bytes_per_sample;
    wavhdr.BlockAlign = bytes_per_sample * num_channels;
    wavhdr.BitsPerSample = bits_per_sample;

    if (num_channels > 2 || channel_mask != 0x5 - num_channels) {
        wavhdrsize = sizeof (wavhdr);
        wavhdr.cbSize = 22;
        wavhdr.ValidBitsPerSample = bits_per_sample;
        wavhdr.SubFormat = format;
        wavhdr.ChannelMask = channel_mask;
        wavhdr.FormatTag = 0xfffe;
        wavhdr.BitsPerSample = bytes_per_sample * 8;
        wavhdr.GUID [4] = 0x10;
        wavhdr.GUID [6] = 0x80;
        wavhdr.GUID [9] = 0xaa;
        wavhdr.GUID [11] = 0x38;
        wavhdr.GUID [12] = 0x9b;
        wavhdr.GUID [13] = 0x71;
    }

    strncpy (riffhdr.ckID, do_rf64 ? "RF64" : "RIFF", sizeof (riffhdr.ckID));
    strncpy (riffhdr.formType, "WAVE", sizeof (riffhdr.formType));
    total_riff_bytes = sizeof (riffhdr) + wavhdrsize + sizeof (datahdr) + ((total_data_bytes + 1) & ~(int64_t)1);
    if (do_rf64) total_riff_bytes += sizeof (ds64hdr) + sizeof (ds64_chunk);
    total_riff_bytes += table_length * sizeof (CS64Chunk);
    if (write_junk) total_riff_bytes += sizeof (junkchunk);
    strncpy (fmthdr.ckID, "fmt ", sizeof (fmthdr.ckID));
    strncpy (datahdr.ckID, "data", sizeof (datahdr.ckID));
    fmthdr.ckSize = wavhdrsize;

    if (write_junk) {
        CLEAR (junkchunk);
        strncpy (junkchunk.ckID, "junk", sizeof (junkchunk.ckID));
        junkchunk.ckSize = sizeof (junkchunk) - 8;
        WavpackNativeToLittleEndian (&junkchunk, ChunkHeaderFormat);
    }

    if (do_rf64) {
        strncpy (ds64hdr.ckID, "ds64", sizeof (ds64hdr.ckID));
        ds64hdr.ckSize = sizeof (ds64_chunk) + (table_length * sizeof (CS64Chunk));
        CLEAR (ds64_chunk);
        ds64_chunk.riffSize64 = total_riff_bytes;
        ds64_chunk.dataSize64 = total_data_bytes;
        ds64_chunk.sampleCount64 = total_samples;
        ds64_chunk.tableLength = table_length;
        riffhdr.ckSize = (uint32_t) -1;
        datahdr.ckSize = (uint32_t) -1;
        WavpackNativeToLittleEndian (&ds64hdr, ChunkHeaderFormat);
        WavpackNativeToLittleEndian (&ds64_chunk, DS64ChunkFormat);
    }
    else {
        riffhdr.ckSize = (uint32_t) total_riff_bytes;
        datahdr.ckSize = (uint32_t) total_data_bytes;
    }

    // this "table" is just a dummy placeholder for testing (normally not written)

    if (table_length) {
        strncpy (cs64_chunk.ckID, "dmmy", sizeof (cs64_chunk.ckID));
        cs64_chunk.chunkSize64 = 12345678;
        WavpackNativeToLittleEndian (&cs64_chunk, CS64ChunkFormat);
    }

    // write the RIFF chunks up to just before the data starts

    WavpackNativeToLittleEndian (&riffhdr, ChunkHeaderFormat);
    WavpackNativeToLittleEndian (&fmthdr, ChunkHeaderFormat);
    WavpackNativeToLittleEndian (&wavhdr, WaveHeaderFormat);
    WavpackNativeToLittleEndian (&datahdr, ChunkHeaderFormat);

    if (!DoWriteFile (outfile, &riffhdr, sizeof (riffhdr), &bcount) || bcount != sizeof (riffhdr) ||
        (do_rf64 && (!DoWriteFile (outfile, &ds64hdr, sizeof (ds64hdr), &bcount) || bcount != sizeof (ds64hdr))) ||
        (do_rf64 && (!DoWriteFile (outfile, &ds64_chunk, sizeof (ds64_chunk), &bcount) || bcount != sizeof (ds64_chunk)))) {
            error_line ("can't write .WAV data, disk probably full!");
            return FALSE;
    }

    // again, this is normally not written except for testing

    while (table_length--)
        if (!DoWriteFile (outfile, &cs64_chunk, sizeof (cs64_chunk), &bcount) || bcount != sizeof (cs64_chunk)) {
            error_line ("can't write .WAV data, disk probably full!");
            return FALSE;
        }


    if ((write_junk && (!DoWriteFile (outfile, &junkchunk, sizeof (junkchunk), &bcount) || bcount != sizeof (junkchunk))) ||
        !DoWriteFile (outfile, &fmthdr, sizeof (fmthdr), &bcount) || bcount != sizeof (fmthdr) ||
        !DoWriteFile (outfile, &wavhdr, wavhdrsize, &bcount) || bcount != wavhdrsize ||
        !DoWriteFile (outfile, &datahdr, sizeof (datahdr), &bcount) || bcount != sizeof (datahdr)) {
            error_line ("can't write .WAV data, disk probably full!");
            return FALSE;
    }

#endif
    return TRUE;
}

