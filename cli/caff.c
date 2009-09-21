////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//              Copyright (c) 1998 - 2009 Conifer Software.               //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// caff.c

// This module is a helper to the WavPack command-line programs to support CAF files.

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

typedef struct
{
    char ckID [4];
    uint16_t mFileVersion;
    uint16_t mFileFlags;
} CAFFileHeader;

#define CAFFFileHeaderFormat "4SS"

typedef struct
{
    char ckID [4];
    int64_t mChunkSize;
} CAFChunkHeader;

#define CAFChunkHeaderFormat "4D"

typedef struct
{
    double mSampleRate;
    char mFormatID [4];
    uint32_t mFormatFlags;
    uint32_t mBytesPerPacket;
    uint32_t mFramesPerPacket;
    uint32_t mChannelsPerFrame;
    uint32_t mBitsPerChannel;
} CAFAudioFormat;

#define CAFAudioFormatFormat "D4LLLLL"
#define CAF_FORMAT_FLOAT            0x1
#define CAF_FORMAT_LITTLE_ENDIAN    0x2

typedef struct
{
    uint32_t mChannelLayoutTag;
    uint32_t mChannelBitmap;
    uint32_t mNumberChannelDescriptions;
} CAFChannelLayout;

#define CAFChannelLayoutFormat "LLL"

static struct {
    uint32_t mChannelLayoutTag;
    uint32_t mChannelBitmap;
} layouts [] = {
    { (100<<16) | 1, 0x004 },   // C
    { (101<<16) | 2, 0x003 },   // FL, FR
    { (102<<16) | 2, 0x003 },   // FL, FR
    { (106<<16) | 2, 0x003 },   // FL, FR
    { (108<<16) | 4, 0x033 },   // FL, FR, BL, BR 
    { (113<<16) | 3, 0x007 },   // FL, FR, C
    { (115<<16) | 4, 0x017 },   // FL, FR, C, BC
    { (117<<16) | 5, 0x037 },   // FL, FR, C, BL, BR
    { (121<<16) | 6, 0x03f },   // FL, FR, C, LFE, BL, BR
    { (125<<16) | 7, 0x13f },   // FL, FR, C, LFE, BL, BR, BC
    { (126<<16) | 8, 0x0ff },   // FL, FR, C, LFE, BL, BR, LC, RC
    { (131<<16) | 3, 0x103 },   // FL, FR, BC
    { (132<<16) | 4, 0x033 },   // FL, FR, BL, BR 
    { (133<<16) | 3, 0x00B },   // FL, FR, LFE 
    { (134<<16) | 4, 0x10B },   // FL, FR, LFE, BC 
    { (135<<16) | 5, 0x03B },   // FL, FR, LFE, BL, BR 
    { (136<<16) | 4, 0x00F },   // FL, FR, LFE, C 
    { (137<<16) | 5, 0x01F },   // FL, FR, LFE, C, BC 
};

#define NUM_LAYOUTS (sizeof (layouts) / sizeof (layouts [0]))

int ParseCaffHeaderConfig (FILE *infile, char *infilename, char *fourcc, WavpackContext *wpc, WavpackConfig *config)
{
    uint32_t total_samples = 0, bcount;
    CAFFileHeader caf_file_header;
    CAFChunkHeader caf_chunk_header;
    CAFAudioFormat caf_audio_format;
    int64_t infilesize;

    infilesize = DoGetFileSize (infile);

    if (infilesize >= 4294967296LL && !(config->qmode & QMODE_IGNORE_LENGTH)) {
        error_line ("can't handle .CAF files larger than 4 GB (yet)!");
        return SOFT_ERROR;
    }

    memcpy (&caf_file_header, fourcc, 4);

    if ((!DoReadFile (infile, ((char *) &caf_file_header) + 4, sizeof (CAFFileHeader) - 4, &bcount) ||
        bcount != sizeof (CAFFileHeader) - 4)) {
            error_line ("%s is not a valid .CAF file!", infilename);
            return SOFT_ERROR;
    }
    else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
        !WavpackAddWrapper (wpc, &caf_file_header, sizeof (CAFFileHeader))) {
            error_line ("%s", WavpackGetErrorMessage (wpc));
            return SOFT_ERROR;
    }

    WavpackBigEndianToNative (&caf_file_header, CAFFFileHeaderFormat);

    if (caf_file_header.mFileVersion != 1) {
        error_line ("%s: can't handle version %d .CAF files!", infilename, caf_file_header.mFileVersion);
        return SOFT_ERROR;
    }

    // loop through all elements of the RIFF wav header
    // (until the data chuck) and copy them to the output file

    while (1) {
        if (!DoReadFile (infile, &caf_chunk_header, sizeof (CAFChunkHeader), &bcount) ||
            bcount != sizeof (CAFChunkHeader)) {
                error_line ("%s is not a valid .CAF file!", infilename);
                return SOFT_ERROR;
        }
        else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
            !WavpackAddWrapper (wpc, &caf_chunk_header, sizeof (CAFChunkHeader))) {
                error_line ("%s", WavpackGetErrorMessage (wpc));
                return SOFT_ERROR;
        }

        WavpackBigEndianToNative (&caf_chunk_header, CAFChunkHeaderFormat);

        // if it's the format chunk, we want to get some info out of there and
        // make sure it's a .caf file we can handle

        if (!strncmp (caf_chunk_header.ckID, "desc", 4)) {
            int supported = TRUE;

            if (caf_chunk_header.mChunkSize != sizeof (CAFAudioFormat) ||
                !DoReadFile (infile, &caf_audio_format, caf_chunk_header.mChunkSize, &bcount) ||
                bcount != caf_chunk_header.mChunkSize) {
                    error_line ("%s is not a valid .CAF file!", infilename);
                    return SOFT_ERROR;
            }
            else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
                !WavpackAddWrapper (wpc, &caf_audio_format, caf_chunk_header.mChunkSize)) {
                    error_line ("%s", WavpackGetErrorMessage (wpc));
                    return SOFT_ERROR;
            }

            WavpackBigEndianToNative (&caf_audio_format, CAFAudioFormatFormat);

            if (debug_logging_mode) {
                char formatstr [5];

                memcpy (formatstr, caf_audio_format.mFormatID, 4);
                formatstr [4] = 0;
                error_line ("format = %s, flags = %x, sampling rate = %g",
                    formatstr, caf_audio_format.mFormatFlags, caf_audio_format.mSampleRate);
                error_line ("packet = %d bytes and %d frames",
                    caf_audio_format.mBytesPerPacket, caf_audio_format.mFramesPerPacket);
                error_line ("channels per frame = %d, bits per channel = %d",
                    caf_audio_format.mChannelsPerFrame, caf_audio_format.mBitsPerChannel);
            }

            if (strncmp (caf_audio_format.mFormatID, "lpcm", 4) || (caf_audio_format.mFormatFlags & ~3))
                supported = FALSE;
            else if (caf_audio_format.mSampleRate < 1.0 || caf_audio_format.mSampleRate > 16777215.0 ||
                caf_audio_format.mSampleRate != floor (caf_audio_format.mSampleRate))
                    supported = FALSE;
            else if (!caf_audio_format.mChannelsPerFrame || caf_audio_format.mChannelsPerFrame > 256)
                supported = FALSE;
            else if (caf_audio_format.mBitsPerChannel < 1 || caf_audio_format.mBitsPerChannel > 32 ||
                ((caf_audio_format.mFormatFlags & CAF_FORMAT_FLOAT) && caf_audio_format.mBitsPerChannel != 32))
                    supported = FALSE;
            else if (caf_audio_format.mFramesPerPacket != 1 ||
                caf_audio_format.mBytesPerPacket / caf_audio_format.mChannelsPerFrame < (caf_audio_format.mBitsPerChannel + 7) / 8 ||
                caf_audio_format.mBytesPerPacket / caf_audio_format.mChannelsPerFrame > 4 ||
                caf_audio_format.mBytesPerPacket % caf_audio_format.mChannelsPerFrame)
                    supported = FALSE;

            if (!supported) {
                error_line ("%s is an unsupported .CAF format!", infilename);
                return SOFT_ERROR;
            }

            config->bytes_per_sample = caf_audio_format.mBytesPerPacket / caf_audio_format.mChannelsPerFrame;
            config->float_norm_exp = (caf_audio_format.mFormatFlags & CAF_FORMAT_FLOAT) ? 127 : 0;
            config->bits_per_sample = caf_audio_format.mBitsPerChannel;
            config->num_channels = caf_audio_format.mChannelsPerFrame;
            config->sample_rate = (int) caf_audio_format.mSampleRate;

            if (debug_logging_mode) {
                if (config->float_norm_exp == 127)
                    error_line ("data format: normalized 32-bit floating point");
                else if (config->float_norm_exp)
                    error_line ("data format: 32-bit floating point (Audition %d:%d float type 1)",
                        config->float_norm_exp - 126, 150 - config->float_norm_exp);
                else
                    error_line ("data format: %d-bit integers stored in %d byte(s)",
                        config->bits_per_sample, config->bytes_per_sample);
            }
        }
        else if (!strncmp (caf_chunk_header.ckID, "chan", 4)) {
            CAFChannelLayout *caf_channel_layout = malloc (caf_chunk_header.mChunkSize);
            int supported = TRUE;

            if (caf_chunk_header.mChunkSize < sizeof (CAFAudioFormat) ||
                !DoReadFile (infile, &caf_channel_layout, caf_chunk_header.mChunkSize, &bcount) ||
                bcount != caf_chunk_header.mChunkSize) {
                    error_line ("%s is not a valid .CAF file!", infilename);
                    return SOFT_ERROR;
            }
            else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
                !WavpackAddWrapper (wpc, &caf_audio_format, caf_chunk_header.mChunkSize)) {
                    error_line ("%s", WavpackGetErrorMessage (wpc));
                    return SOFT_ERROR;
            }

            WavpackBigEndianToNative (&caf_channel_layout, CAFChannelLayoutFormat);

            if (config->channel_mask || (config->qmode & QMODE_CHANS_UNASSIGNED)) {
                error_line ("this CAF file already has channel order information!");
                return SOFT_ERROR;
            }

            if (caf_channel_layout->mChannelLayoutTag == 0x10000)
                config->channel_mask = caf_channel_layout->mChannelBitmap;
            else {
                int i;

                for (i = 0; i < NUM_LAYOUTS; ++i)
                    if (caf_channel_layout->mChannelLayoutTag == layouts [i].mChannelLayoutTag) {
                        config->channel_mask = layouts [i].mChannelBitmap;
                        break;
                    }
            }

            if (!config->channel_mask)
                config->qmode |= QMODE_CHANS_UNASSIGNED;
        }
        else if (!strncmp (caf_chunk_header.ckID, "data", 4)) {     // on the data chunk, get size and exit loop
            int64_t data_chunk_size = caf_chunk_header.mChunkSize;

            if (data_chunk_size == -1 || data_chunk_size >= 4) {
                uint32_t mEditCount;

                if (!DoReadFile (infile, &mEditCount, sizeof (mEditCount), &bcount) ||
                    bcount != sizeof (mEditCount)) {
                        error_line ("%s is not a valid .CAF file!", infilename);
                        return SOFT_ERROR;
                }
                else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
                    !WavpackAddWrapper (wpc, &mEditCount, sizeof (mEditCount))) {
                        error_line ("%s", WavpackGetErrorMessage (wpc));
                        return SOFT_ERROR;
                }

                WavpackBigEndianToNative (&mEditCount, "L");
            }

            if (data_chunk_size == -1) {
                config->qmode |= QMODE_IGNORE_LENGTH;
                total_samples = -1;
            }
            else {
                if (infilesize && !(config->qmode & QMODE_IGNORE_LENGTH) && infilesize - caf_chunk_header.mChunkSize > 16777216) {
                    error_line (".CAF file %s has over 16 MB of extra CAFF data, probably is corrupt!", infilename);
                    return SOFT_ERROR;
                }

                if ((caf_chunk_header.mChunkSize - 4) % caf_audio_format.mBytesPerPacket) {
                    error_line (".CAF file %s has an invalid data chunk size, probably is corrupt!", infilename);
                    return SOFT_ERROR;
                }

                total_samples = (caf_chunk_header.mChunkSize - 4) / caf_audio_format.mBytesPerPacket;

                if (!total_samples && !(config->qmode & QMODE_IGNORE_LENGTH)) {
                    error_line ("this .CAF file has no audio samples, probably is corrupt!");
                    return SOFT_ERROR;
                }
            }

            break;
        }
        else {          // just copy unknown chunks to output file

            int bytes_to_copy = caf_chunk_header.mChunkSize;
            char *buff = malloc (bytes_to_copy);

            if (debug_logging_mode)
                error_line ("extra unknown chunk \"%c%c%c%c\" of %d bytes",
                    caf_chunk_header.ckID [0], caf_chunk_header.ckID [1], caf_chunk_header.ckID [2],
                    caf_chunk_header.ckID [3], caf_chunk_header.mChunkSize);

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

    if (!config->channel_mask && config->num_channels <= 2 && !(config->qmode & QMODE_CHANS_UNASSIGNED))
        config->channel_mask = 0x5 - config->num_channels;

    if (!WavpackSetConfiguration (wpc, config, total_samples)) {
        error_line ("%s", WavpackGetErrorMessage (wpc));
        return SOFT_ERROR;
    }

    return NO_ERROR;
}
