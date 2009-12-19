////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//              Copyright (c) 1998 - 2009 Conifer Software.               //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// riff.c

// This module is a helper to the WavPack command-line programs to support MS WAVE RIFF files.

#if defined(WIN32)
#include <windows.h>
#include <io.h>
#else
#if defined(__OS2__)
#define INCL_DOS
#include <io.h>
#endif
#include <sys/param.h>
#include <sys/stat.h>
#include <locale.h>
#include <iconv.h>
#endif

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <ctype.h>

#include "wavpack.h"
#include "utils.h"
#include "md5.h"

#if defined (__GNUC__) && !defined(WIN32)
#include <unistd.h>
#include <glob.h>
#include <sys/time.h>
#else
#include <sys/timeb.h>
#endif

#ifdef WIN32
#define stricmp(x,y) _stricmp(x,y)
#define strdup(x) _strdup(x)
#define fileno _fileno
#else
#define stricmp(x,y) strcasecmp(x,y)
#endif

#ifdef DEBUG_ALLOC
#define malloc malloc_db
#define realloc realloc_db
#define free free_db
void *malloc_db (uint32_t size);
void *realloc_db (void *ptr, uint32_t size);
void free_db (void *ptr);
int32_t dump_alloc (void);
static char *strdup (const char *s)
 { char *d = malloc (strlen (s) + 1); return strcpy (d, s); }
#endif

#define NO_ERROR 0L
#define SOFT_ERROR 1
#define HARD_ERROR 2

extern int debug_logging_mode;

int ParseRiffHeaderConfig (FILE *infile, char *infilename, char *fourcc, WavpackContext *wpc, WavpackConfig *config)
{
    uint32_t total_samples = 0, bcount;
    RiffChunkHeader riff_chunk_header;
    ChunkHeader chunk_header;
    WaveHeader WaveHeader;
    int64_t infilesize;

    CLEAR (WaveHeader);
    infilesize = DoGetFileSize (infile);

    if (infilesize >= 4294967296LL && !(config->qmode & QMODE_IGNORE_LENGTH)) {
        error_line ("can't handle .WAV files larger than 4 GB (non-standard)!");
        return SOFT_ERROR;
    }

    memcpy (&riff_chunk_header, fourcc, 4);

    if ((!DoReadFile (infile, ((char *) &riff_chunk_header) + 4, sizeof (RiffChunkHeader) - 4, &bcount) ||
        bcount != sizeof (RiffChunkHeader) - 4 || strncmp (riff_chunk_header.formType, "WAVE", 4))) {
            error_line ("%s is not a valid .WAV file!", infilename);
            return SOFT_ERROR;
    }
    else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
        !WavpackAddWrapper (wpc, &riff_chunk_header, sizeof (RiffChunkHeader))) {
            error_line ("%s", WavpackGetErrorMessage (wpc));
            return SOFT_ERROR;
    }

    // loop through all elements of the RIFF wav header
    // (until the data chuck) and copy them to the output file

    while (1) {
        if (!DoReadFile (infile, &chunk_header, sizeof (ChunkHeader), &bcount) ||
            bcount != sizeof (ChunkHeader)) {
                error_line ("%s is not a valid .WAV file!", infilename);
                return SOFT_ERROR;
        }
        else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
            !WavpackAddWrapper (wpc, &chunk_header, sizeof (ChunkHeader))) {
                error_line ("%s", WavpackGetErrorMessage (wpc));
                return SOFT_ERROR;
        }

        WavpackLittleEndianToNative (&chunk_header, ChunkHeaderFormat);

        // if it's the format chunk, we want to get some info out of there and
        // make sure it's a .wav file we can handle

        if (!strncmp (chunk_header.ckID, "fmt ", 4)) {
            int supported = TRUE, format;

            if (chunk_header.ckSize < 16 || chunk_header.ckSize > sizeof (WaveHeader) ||
                !DoReadFile (infile, &WaveHeader, chunk_header.ckSize, &bcount) ||
                bcount != chunk_header.ckSize) {
                    error_line ("%s is not a valid .WAV file!", infilename);
                    return SOFT_ERROR;
            }
            else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
                !WavpackAddWrapper (wpc, &WaveHeader, chunk_header.ckSize)) {
                    error_line ("%s", WavpackGetErrorMessage (wpc));
                    return SOFT_ERROR;
            }

            WavpackLittleEndianToNative (&WaveHeader, WaveHeaderFormat);

            if (debug_logging_mode) {
                error_line ("format tag size = %d", chunk_header.ckSize);
                error_line ("FormatTag = %x, NumChannels = %d, BitsPerSample = %d",
                    WaveHeader.FormatTag, WaveHeader.NumChannels, WaveHeader.BitsPerSample);
                error_line ("BlockAlign = %d, SampleRate = %d, BytesPerSecond = %d",
                    WaveHeader.BlockAlign, WaveHeader.SampleRate, WaveHeader.BytesPerSecond);

                if (chunk_header.ckSize > 16)
                    error_line ("cbSize = %d, ValidBitsPerSample = %d", WaveHeader.cbSize,
                        WaveHeader.ValidBitsPerSample);

                if (chunk_header.ckSize > 20)
                    error_line ("ChannelMask = %x, SubFormat = %d",
                        WaveHeader.ChannelMask, WaveHeader.SubFormat);
            }

            if (chunk_header.ckSize > 16 && WaveHeader.cbSize == 2)
                config->qmode |= QMODE_ADOBE_MODE;

            format = (WaveHeader.FormatTag == 0xfffe && chunk_header.ckSize == 40) ?
                WaveHeader.SubFormat : WaveHeader.FormatTag;

            config->bits_per_sample = chunk_header.ckSize == 40 ?
                WaveHeader.ValidBitsPerSample : WaveHeader.BitsPerSample;

            if (format != 1 && format != 3)
                supported = FALSE;

            if (!WaveHeader.NumChannels || WaveHeader.NumChannels > 256 ||
                WaveHeader.BlockAlign / WaveHeader.NumChannels < (config->bits_per_sample + 7) / 8 ||
                WaveHeader.BlockAlign / WaveHeader.NumChannels > 4 ||
                WaveHeader.BlockAlign % WaveHeader.NumChannels)
                    supported = FALSE;

            if (config->bits_per_sample < 1 || config->bits_per_sample > 32)
                supported = FALSE;

            if (!supported) {
                error_line ("%s is an unsupported .WAV format!", infilename);
                return SOFT_ERROR;
            }

            if (chunk_header.ckSize < 40) {
                if (!config->channel_mask && !(config->qmode & QMODE_CHANS_UNASSIGNED)) {
                    if (WaveHeader.NumChannels <= 2)
                        config->channel_mask = 0x5 - WaveHeader.NumChannels;
                    else if (WaveHeader.NumChannels <= 18)
                        config->channel_mask = (1 << WaveHeader.NumChannels) - 1;
                    else
                        config->channel_mask = 0x3ffff;
                }
            }
            else {
                if (config->channel_mask || (config->qmode & QMODE_CHANS_UNASSIGNED)) {
                    error_line ("this WAV file already has channel order information!");
                    return SOFT_ERROR;
                }

                config->channel_mask = WaveHeader.ChannelMask;
            }

            if (format == 3)
                config->float_norm_exp = 127;
            else if ((config->qmode & QMODE_ADOBE_MODE) &&
                WaveHeader.BlockAlign / WaveHeader.NumChannels == 4) {
                    if (WaveHeader.BitsPerSample == 24)
                        config->float_norm_exp = 127 + 23;
                    else if (WaveHeader.BitsPerSample == 32)
                        config->float_norm_exp = 127 + 15;
            }

            if (debug_logging_mode) {
                if (config->float_norm_exp == 127)
                    error_line ("data format: normalized 32-bit floating point");
                else if (config->float_norm_exp)
                    error_line ("data format: 32-bit floating point (Audition %d:%d float type 1)",
                        config->float_norm_exp - 126, 150 - config->float_norm_exp);
                else
                    error_line ("data format: %d-bit integers stored in %d byte(s)",
                        config->bits_per_sample, WaveHeader.BlockAlign / WaveHeader.NumChannels);
            }
        }
        else if (!strncmp (chunk_header.ckID, "data", 4)) {

            // on the data chunk, get size and exit loop

            if (!WaveHeader.NumChannels) {      // make sure we saw a "fmt" chunk...
                error_line ("%s is not a valid .WAV file!", infilename);
                return SOFT_ERROR;
            }

            if (infilesize && !(config->qmode & QMODE_IGNORE_LENGTH) && infilesize - chunk_header.ckSize > 16777216) {
                error_line ("this .WAV file has over 16 MB of extra RIFF data, probably is corrupt!");
                return SOFT_ERROR;
            }

            total_samples = chunk_header.ckSize / WaveHeader.BlockAlign;

            if (!total_samples && !(config->qmode & QMODE_IGNORE_LENGTH)) {
                error_line ("this .WAV file has no audio samples, probably is corrupt!");
                return SOFT_ERROR;
            }

            config->bytes_per_sample = WaveHeader.BlockAlign / WaveHeader.NumChannels;
            config->num_channels = WaveHeader.NumChannels;
            config->sample_rate = WaveHeader.SampleRate;
            break;
        }
        else {          // just copy unknown chunks to output file

            int bytes_to_copy = (chunk_header.ckSize + 1) & ~1L;
            char *buff = malloc (bytes_to_copy);

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
                    return SOFT_ERROR;
            }

            free (buff);
        }
    }

    if (!WavpackSetConfiguration (wpc, config, total_samples)) {
        error_line ("%s: %s", infilename, WavpackGetErrorMessage (wpc));
        return SOFT_ERROR;
    }

    return NO_ERROR;
}

int WriteRiffHeader (FILE *outfile, WavpackContext *wpc, uint32_t total_samples)
{
    RiffChunkHeader riffhdr;
    ChunkHeader datahdr, fmthdr;
    WaveHeader wavhdr;
    uint32_t bcount;

    uint32_t total_data_bytes;
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

    if (total_samples == (uint32_t) -1)
        total_samples = 0x7ffff000 / (bytes_per_sample * num_channels);

    total_data_bytes = total_samples * bytes_per_sample * num_channels;

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

    strncpy (riffhdr.ckID, "RIFF", sizeof (riffhdr.ckID));
    strncpy (riffhdr.formType, "WAVE", sizeof (riffhdr.formType));
    riffhdr.ckSize = sizeof (riffhdr) + wavhdrsize + sizeof (datahdr) + total_data_bytes;
    strncpy (fmthdr.ckID, "fmt ", sizeof (fmthdr.ckID));
    fmthdr.ckSize = wavhdrsize;

    strncpy (datahdr.ckID, "data", sizeof (datahdr.ckID));
    datahdr.ckSize = total_data_bytes;

    // write the RIFF chunks up to just before the data starts

    WavpackNativeToLittleEndian (&riffhdr, ChunkHeaderFormat);
    WavpackNativeToLittleEndian (&fmthdr, ChunkHeaderFormat);
    WavpackNativeToLittleEndian (&wavhdr, WaveHeaderFormat);
    WavpackNativeToLittleEndian (&datahdr, ChunkHeaderFormat);

    if (!DoWriteFile (outfile, &riffhdr, sizeof (riffhdr), &bcount) || bcount != sizeof (riffhdr) ||
        !DoWriteFile (outfile, &fmthdr, sizeof (fmthdr), &bcount) || bcount != sizeof (fmthdr) ||
        !DoWriteFile (outfile, &wavhdr, wavhdrsize, &bcount) || bcount != wavhdrsize ||
        !DoWriteFile (outfile, &datahdr, sizeof (datahdr), &bcount) || bcount != sizeof (datahdr)) {
            error_line ("can't write .WAV data, disk probably full!");
            return FALSE;
    }

    return TRUE;
}

