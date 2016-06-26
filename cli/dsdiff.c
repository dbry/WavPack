////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//                Copyright (c) 1998 - 2016 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// dsdiff.c

// This module is a helper to the WavPack command-line programs to support DFF files.

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
    int64_t ckDataSize;
} DFFChunkHeader;

typedef struct
{
    char ckID [4];
    int64_t ckDataSize;
    char formType [4];
} DFFFileHeader;

#pragma pack(pop)

#define DFFChunkHeaderFormat "4D"
#define DFFFileHeaderFormat "4D4"

int ParseDsdiffHeaderConfig (FILE *infile, char *infilename, char *fourcc, WavpackContext *wpc, WavpackConfig *config)
{
    int64_t infilesize, total_samples;
    DFFFileHeader dff_file_header;
    DFFChunkHeader dff_chunk_header;
    uint32_t bcount;
    int i;

    infilesize = DoGetFileSize (infile);
    memcpy (&dff_file_header, fourcc, 4);

    if ((!DoReadFile (infile, ((char *) &dff_file_header) + 4, sizeof (DFFFileHeader) - 4, &bcount) ||
        bcount != sizeof (DFFFileHeader) - 4) || strncmp (dff_file_header.formType, "DSD ", 4)) {
            error_line ("%s is not a valid .DFF file!", infilename);
            return WAVPACK_SOFT_ERROR;
    }
    else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
        !WavpackAddWrapper (wpc, &dff_file_header, sizeof (DFFFileHeader))) {
            error_line ("%s", WavpackGetErrorMessage (wpc));
            return WAVPACK_SOFT_ERROR;
    }

#if 1   // this might be a little too picky...
    WavpackBigEndianToNative (&dff_file_header, DFFFileHeaderFormat);

    if (infilesize && !(config->qmode & QMODE_IGNORE_LENGTH) &&
        dff_file_header.ckDataSize && dff_file_header.ckDataSize + 1 && dff_file_header.ckDataSize + 12 != infilesize) {
            error_line ("%s is not a valid .DFF file (by total size)!", infilename);
            return WAVPACK_SOFT_ERROR;
    }
#endif

    if (debug_logging_mode)
        error_line ("file header indicated length = %lld", dff_file_header.ckDataSize);

    // loop through all elements of the DSDIFF header
    // (until the data chuck) and copy them to the output file

    while (1) {
        if (!DoReadFile (infile, &dff_chunk_header, sizeof (DFFChunkHeader), &bcount) ||
            bcount != sizeof (DFFChunkHeader)) {
                error_line ("%s is not a valid .DFF file!", infilename);
                return WAVPACK_SOFT_ERROR;
        }
        else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
            !WavpackAddWrapper (wpc, &dff_chunk_header, sizeof (DFFChunkHeader))) {
                error_line ("%s", WavpackGetErrorMessage (wpc));
                return WAVPACK_SOFT_ERROR;
        }

        WavpackBigEndianToNative (&dff_chunk_header, DFFChunkHeaderFormat);

        if (debug_logging_mode)
            error_line ("chunk header indicated length = %lld", dff_chunk_header.ckDataSize);

        if (!strncmp (dff_chunk_header.ckID, "FVER", 4)) {
            uint32_t version;

            if (dff_chunk_header.ckDataSize != sizeof (version) ||
                !DoReadFile (infile, &version, sizeof (version), &bcount) ||
                bcount != sizeof (version)) {
                    error_line ("%s is not a valid .DFF file!", infilename);
                    return WAVPACK_SOFT_ERROR;
            }
            else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
                !WavpackAddWrapper (wpc, &version, sizeof (version))) {
                    error_line ("%s", WavpackGetErrorMessage (wpc));
                    return WAVPACK_SOFT_ERROR;
            }

            WavpackBigEndianToNative (&version, "L");

            if (debug_logging_mode)
                error_line ("dsdiff file version = 0x%08x", version);
        }
        else if (!strncmp (dff_chunk_header.ckID, "PROP", 4)) {
            unsigned char *prop_chunk = malloc ((size_t) dff_chunk_header.ckDataSize);

            if (!DoReadFile (infile, prop_chunk, (uint32_t) dff_chunk_header.ckDataSize, &bcount) ||
                bcount != dff_chunk_header.ckDataSize) {
                    error_line ("%s is not a valid .DFF file!", infilename);
                    free (prop_chunk);
                    return WAVPACK_SOFT_ERROR;
            }
            else if (!(config->qmode & QMODE_NO_STORE_WRAPPER) &&
                !WavpackAddWrapper (wpc, prop_chunk, (uint32_t) dff_chunk_header.ckDataSize)) {
                    error_line ("%s", WavpackGetErrorMessage (wpc));
                    free (prop_chunk);
                    return WAVPACK_SOFT_ERROR;
            }

            if (!strncmp (prop_chunk, "SND ", 4)) {
                unsigned char *cptr = prop_chunk + 4, *eptr = prop_chunk + dff_chunk_header.ckDataSize;
                uint16_t numChannels, chansSpecified, chanMask = 0;
                uint32_t sampleRate;

                while (eptr - cptr >= sizeof (dff_chunk_header)) {
                    memcpy (&dff_chunk_header, cptr, sizeof (dff_chunk_header));
                    cptr += sizeof (dff_chunk_header);
                    WavpackBigEndianToNative (&dff_chunk_header, DFFChunkHeaderFormat);

                    if (eptr - cptr >= dff_chunk_header.ckDataSize) {
                        if (!strncmp (dff_chunk_header.ckID, "FS  ", 4) && dff_chunk_header.ckDataSize == 4) {
                            memcpy (&sampleRate, cptr, sizeof (sampleRate));
                            WavpackBigEndianToNative (&sampleRate, "L");
                            cptr += dff_chunk_header.ckDataSize;

                            if (debug_logging_mode)
                                error_line ("got sample rate of %u Hz", sampleRate);
                        }
                        else if (!strncmp (dff_chunk_header.ckID, "CHNL", 4) && dff_chunk_header.ckDataSize >= 2) {
                            memcpy (&numChannels, cptr, sizeof (numChannels));
                            WavpackBigEndianToNative (&numChannels, "S");
                            cptr += sizeof (numChannels);

                            chansSpecified = (dff_chunk_header.ckDataSize - sizeof (numChannels)) / 4;

                            while (chansSpecified--) {
                                if (!strncmp (cptr, "SLFT", 4) || !strncmp (cptr, "MLFT", 4))
                                    chanMask |= 0x1;
                                else if (!strncmp (cptr, "SRGT", 4) || !strncmp (cptr, "MRGT", 4))
                                    chanMask |= 0x2;
                                else if (!strncmp (cptr, "LS  ", 4))
                                    chanMask |= 0x10;
                                else if (!strncmp (cptr, "RS  ", 4))
                                    chanMask |= 0x20;
                                else if (!strncmp (cptr, "C   ", 4))
                                    chanMask |= 0x4;
                                else if (!strncmp (cptr, "LFE ", 4))
                                    chanMask |= 0x8;
                                else
                                    if (debug_logging_mode)
                                        error_line ("undefined channel ID %c%c%c%c", cptr [0], cptr [1], cptr [2], cptr [3]);

                                cptr += 4;
                            }

                            if (debug_logging_mode)
                                error_line ("%d channels, mask = 0x%08x", numChannels, chanMask);
                        }
                        else if (!strncmp (dff_chunk_header.ckID, "CMPR", 4) && dff_chunk_header.ckDataSize >= 4) {
                            if (strncmp (cptr, "DSD ", 4)) {
                                error_line ("DSDIFF files must be uncompressed, not \"%c%c%c%c\"!",
                                    cptr [0], cptr [1], cptr [2], cptr [3]);
                                free (prop_chunk);
                                return WAVPACK_SOFT_ERROR;
                            }

                            cptr += dff_chunk_header.ckDataSize;
                        }
                        else if (debug_logging_mode) {
                            error_line ("got PROP/SND chunk type \"%c%c%c%c\" of %d bytes", dff_chunk_header.ckID [0],
                                dff_chunk_header.ckID [1], dff_chunk_header.ckID [2], dff_chunk_header.ckID [3], dff_chunk_header.ckDataSize);

                            cptr += dff_chunk_header.ckDataSize;
                        }
                    }
                    else {
                        error_line ("%s is not a valid .DFF file!", infilename);
                        free (prop_chunk);
                        return WAVPACK_SOFT_ERROR;
                    }
                }

                config->bits_per_sample = 8;
                config->bytes_per_sample = 1;
                config->num_channels = numChannels;
                config->channel_mask = chanMask;
                config->sample_rate = sampleRate / 8;
            }
            else if (debug_logging_mode)
                error_line ("got unknown PROP chunk type \"%c%c%c%c\" of %d bytes",
                    prop_chunk [0], prop_chunk [1], prop_chunk [2], prop_chunk [3], dff_chunk_header.ckDataSize);

            free (prop_chunk);
        }
        else if (!strncmp (dff_chunk_header.ckID, "DSD ", 4)) {
            total_samples = dff_chunk_header.ckDataSize / config->num_channels;
            break;
        }
        else {          // just copy unknown chunks to output file

            int bytes_to_copy = (uint32_t) dff_chunk_header.ckDataSize;
            char *buff = malloc (bytes_to_copy);

            if (debug_logging_mode)
                error_line ("extra unknown chunk \"%c%c%c%c\" of %d bytes",
                    dff_chunk_header.ckID [0], dff_chunk_header.ckID [1], dff_chunk_header.ckID [2],
                    dff_chunk_header.ckID [3], dff_chunk_header.ckDataSize);

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

    if (!WavpackSetConfiguration64 (wpc, config, total_samples)) {
        error_line ("%s: %s", infilename, WavpackGetErrorMessage (wpc));
        return WAVPACK_SOFT_ERROR;
    }

    return WAVPACK_NO_ERROR;
}

