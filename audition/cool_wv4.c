////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//                Copyright (c) 1998 - 2025 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// cool_wv4.c

// This is the main module for the WavPack file filter for CoolEdit

// Version 1.0 - May 31, 2003
// Version 1.1 - June 5, 2003  (fixed bug in 20-bit headers)
// Version 2.0 - June 7, 2004  (WavPack 4.0)
// Version 2.1 - July 9, 2004  (fixed bug with 20 and 24-bit headers)
// Version 2.2 - Aug 17, 2004  (fixed bug with mono encode in extra1.c module)
// Version 2.3 - Aug 28, 2004  (enhanced RIFF / first block stuff, new libs)
// Version 2.4 - Apr 2, 2005   (fixed 2gig+ problem, new libs)
// Version 2.5 - Sept 1, 2005  (fixed encoder/decoder overflow on extra mode)
// Version 2.6 - Nov 1, 2005   (updated to library version 4.3)
// Version 2.7 - Dec 3, 2006   (updated to library version 4.40, added v.high mode)
// Version 2.8 - May 6, 2007   (library ver 4.41, read RIFF header, not just trailer)
// Version 2.9 - May 24, 2008  (library ver 4.50, add About, make "extra" into slider)
// Version 2.10 - Sept 25, 2009 (library ver 4.60)
// Version 2.11 - Nov 22, 2009 (library ver 4.60.1)
// Version 2.12 - May 10, 2015 (library ver 4.75.0)
// Version 2.13 - Sept 29, 2015 (library ver 4.75.2)
// Version 2.14 - Mar 28, 2016 (library ver 4.80.0)
// Version 2.15a - Aug 26, 2016 (library ver 5.0.0-alpha4, DSD read with 8x decimation)
// Version 2.15b - Sept 27, 2016 (library ver 5.0.0-alpha5, new "high" DSD, broken!)
// Version 3.0 - Dec 1, 2016 (library ver 5.0.0)
// Version 3.1 - Jan 18, 2017 (library ver 5.1.0)
// Version 4.0 - June 22, 2023 (library ver 5.6.6, multithreading)
// Version 4.1 - July 4, 2023 (fixed bug with handling of Type 3 "normalized" float setting)
// Version 4.2 - Feb 26, 2024 (library ver 5.7.0)
// Version 4.3 - April 13, 2024 (fixed bug with files > 24 chans, restored legacy support)
// Version 4.4 - January 23, 2025 (fixed bug with loading unpacked samples, library ver 5.8.0)
// Version 5.0 - August 25, 2026 Introduced lossless DSD editing (DXD-e project, library ver 5.9.0)

#include <windows.h>
#include <commctrl.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <io.h>

#include "filters.h"
#include "wavpack.h"
#include "resource.h"
#include "dsd-utils.h"
#include "decoder.h"

#define CHUNKSIZE 16384         // let CoolEdit decide chunk size

// DWORD configuration word used by CoolEdit / Audition defined here:

#define OPTIONS_VERSION         0x3     // 0=none, 1=current, 2&3=reserved
#define OPTIONS_UPDATED         0x4     // options word has been updated
#define OPTIONS_HYBRID          0x8     // hybrid mode
#define OPTIONS_WVC             0x10    // wvc file mode (hybrid only)
#define OPTIONS_HIGH            0x20    // "high" mode specified
#define OPTIONS_FAST            0x40    // "fast" mode specified
#define OPTIONS_FLOAT20         0x80    // floats stored as 20-bit ints
#define OPTIONS_FLOAT24         0x100   // floats stored as 24-bit ints
#define OPTIONS_NOISESHAPE      0x200   // floats noiseshaped to ints
#define OPTIONS_DITHER          0x400   // floats dithered to ints
#define OPTIONS_NORMALIZE       0x800   // floats normalized to type 3 std
#define OPTIONS_EXTRA           0x1000  // extra processing mode (version 2.9+, range = 0-6, else 0-1)
#define OPTIONS_VERY_HIGH       0x8000  // "very high" mode specified
#define OPTIONS_DSD             0x10000 // floats stored at DSD (default level 3, 3rd-order NS)
#define OPTIONS_PROOF           0x20000 // DSD "proof" quality mode (level 5, 4th-order NS)
#define OPTIONS_BITRATE         0xfff00000 // hybrid bits/sample (5.7 fixed pt)

#define WAV_IEEE_FLOAT_NORMAL   127         // +/- 1.0
#define COOL_EDIT_FLOAT_NORMAL  (127+15)    // +/- 32768.0

#define CLEAR(destin) memset (&destin, 0, sizeof (destin));

// This logging facility has only been tested under Wine on Linux (it writes to /tmp). It is disabled
// by default but could probably be made to work on Windows either by using a different file location
// or by opening a console window (which I actually did once).

// #define LOGGING "/tmp/cooledit.txt"

#ifdef LOGGING

// not threadsafe!
// don't use more than one instance in a single printf()!
static char *options_string (DWORD dwOptions)
{
    static char string [256] = {};

    if (!dwOptions)
        return "zero";

    sprintf (string, "%svers %d,", (dwOptions & OPTIONS_UPDATED) ? "* " : "", (int) dwOptions & OPTIONS_VERSION);
    if (dwOptions & OPTIONS_HYBRID) strcat (string, " HYB");
    if (dwOptions & OPTIONS_WVC) strcat (string, " WVC");
    if (dwOptions & OPTIONS_HIGH) strcat (string, " HIGH");
    if (dwOptions & OPTIONS_FAST) strcat (string, " FAST");
    if (dwOptions & OPTIONS_FLOAT20) strcat (string, " FLT20");
    if (dwOptions & OPTIONS_FLOAT24) strcat (string, " FLT24");
    if (dwOptions & OPTIONS_NOISESHAPE) strcat (string, " NS");
    if (dwOptions & OPTIONS_DITHER) strcat (string, " DTHR");
    if (dwOptions & OPTIONS_NORMALIZE) strcat (string, " NRML");
    if (dwOptions & (OPTIONS_EXTRA * 7)) sprintf (string+strlen (string), " XTRA-%d", (int)(dwOptions >> 12) & 0x7);
    if (dwOptions & OPTIONS_VERY_HIGH) strcat (string, " VHIGH");
    if (dwOptions & OPTIONS_DSD) strcat (string, " DSD");
    if (dwOptions & OPTIONS_PROOF) strcat (string, " PROOF");
    if (dwOptions & OPTIONS_BITRATE) sprintf (string+strlen (string), " BR-%.3f", ((dwOptions >> 20) & 0xfff) / 128.0);

    return string;
}

static void logprintf (char *fmt, ...)
{
    va_list argptr;
    FILE *logfile = fopen (LOGGING, "a");

    if (logfile) {
        va_start (argptr, fmt);
        vfprintf (logfile, fmt, argptr);
        va_end (argptr);
        fclose (logfile);
    }
}

#else
#define logprintf(...) ((void)0)
#endif

//////////////////////////////////////////////////////////////////////////////
// Let CoolEdit know who we are and what we can do.                         //
//////////////////////////////////////////////////////////////////////////////

short PASCAL QueryCoolFilter (COOLQUERY *lpcq)
{
    strcpy (lpcq->szName, "WavPack");
    strcpy (lpcq->szCopyright, "WavPack Hybrid Audio Compression");
    strcpy (lpcq->szExt, "WV");

    lpcq->lChunkSize = CHUNKSIZE;

    lpcq->dwFlags = QF_CANLOAD | QF_CANSAVE | QF_RATEADJUSTABLE | QF_CANDO32BITFLOATS |
        QF_HASOPTIONSBOX | QF_READSPECIALLAST | QF_WRITESPECIALLAST;

    lpcq->Mono8 = lpcq->Stereo8 = 0x3ff;
    lpcq->Mono12 = lpcq->Stereo12 = 0;
    lpcq->Mono16 = lpcq->Stereo16 = 0x3ff;
    lpcq->Mono24 = lpcq->Stereo24 = 0;
    lpcq->Mono32 = lpcq->Stereo32 = 0x3ff;

    lpcq->Quad32 = 0;
    lpcq->Quad16 = 0;
    lpcq->Quad8 = 0;

    return C_VALIDLIBRARY;
}


//////////////////////////////////////////////////////////////////////////////
// Since WavPack extensions are unique checking that is good enough for now //
//////////////////////////////////////////////////////////////////////////////

BOOL PASCAL FilterUnderstandsFormat (LPSTR lpszFilename)
{
    if (!lpszFilename || !*lpszFilename)
        return 0;

    return strstr (lpszFilename, ".WV") || strstr (lpszFilename, ".wv");
}


//////////////////////////////////////////////////////////////////////////////
// We can handle everything except 8-bit data or more than 2 channels.      //
//////////////////////////////////////////////////////////////////////////////

void PASCAL GetSuggestedSampleType (long *lplSamprate, WORD *lpwBitsPerSample, WORD *lpwChannels)
{
    *lplSamprate = 0;

    if (*lpwChannels > 2)
        *lpwChannels = 2;

//  *lpwBitsPerSample = (*lpwBitsPerSample > 16) ? 32 : 16;
}


//////////////////////////////////////////////////////////////////////////////
// Open file for output. Because normal WavPack files contain verbatim RIFF //
// headers, we must create them here from scratch.                          //
//////////////////////////////////////////////////////////////////////////////

typedef struct {
    uint32_t bytes_written, first_block_size;
    FILE *file;
    int error;
} write_id;

typedef struct {
    write_id wv_file, wvc_file;
    int32_t random, special_bytes;
    RiffChunkHeader listhdr;
    WavpackContext *wpc;
    float error [2];
    DWORD dwOptions;
    char *listdata;

    // DSD-specific items
    int dsd_buffer_size, source_samples, written_samples;
    unsigned char *dsd_buffer;
    time_t start_time;
    Decoder *decoder;
} OUTPUT;

static int write_block (void *id, void *data, int32_t bcount)
{
    write_id *wid = (write_id *) id;

    if (wid->error)
        return FALSE;

    if (wid && wid->file && data && bcount) {
        if (fwrite (data, 1, bcount, wid->file) != bcount) {
            _chsize (_fileno (wid->file), 0);
            fclose (wid->file);
            wid->file = NULL;
            wid->error = 1;
            return FALSE;
        }
        else {
            wid->bytes_written += bcount;

            if (!wid->first_block_size)
                wid->first_block_size = bcount;
        }
    }

    return TRUE;
}

DWORD PASCAL FilterGetOptions (HWND, HINSTANCE, long, WORD, WORD, DWORD);

HANDLE PASCAL OpenFilterOutput (LPSTR lpszFilename, long lSamprate,
    WORD wBitsPerSample, WORD wChannels, unsigned long lSize, long *lplChunkSize, DWORD dwOptions)
{
    int wavhdrsize = 16, format = 1;
    RiffChunkHeader riffhdr;
    ChunkHeader datahdr, fmthdr;
    WaveHeader wavhdr;
    WavpackConfig config;
    WavpackContext *wpc;
    uint32_t total_samples;
    char wvc_name [512];
    OUTPUT *out;

    total_samples = lSize / wChannels / (wBitsPerSample / 8);

    logprintf ("OpenFilterOutput (%s), samples = %d, rate = %d, bits = %d, channels = %d, options = %s\n",
        lpszFilename, total_samples, (int) lSamprate, wBitsPerSample, wChannels, options_string (dwOptions));

    if (!total_samples)
        return 0;

    // set the chunk size large enough for multithreading to kick in

    if (wChannels > 2)
        *lplChunkSize = 262144;
    else
        *lplChunkSize = 240000 * wChannels * (wBitsPerSample / 8);

    CLEAR (config);

    if ((out = malloc (sizeof (OUTPUT))) == NULL)
        return 0;

    CLEAR (*out);

    // use FilterGetOptions() (without a dialog) to process the (possibly) raw
    // config word into something we can use directly.

    dwOptions = FilterGetOptions (NULL, NULL, lSamprate, wChannels, wBitsPerSample, dwOptions);
    wpc = out->wpc = WavpackOpenFileOutput (write_block, &out->wv_file, &out->wvc_file);
    out->start_time = time (NULL);
    out->dwOptions = dwOptions;
    out->random = 1234567890;

    logprintf ("OpenFilterOutput (%s), options to use = %s\n", lpszFilename, options_string (dwOptions));

    if (!wpc) {
        free (out);
        return 0;
    }

    if ((out->wv_file.file = fopen (lpszFilename, "w+b")) == NULL) {
        WavpackCloseFile (wpc);
        free (out);
        return 0;
    }

    if (dwOptions & OPTIONS_DSD) {
        if (dwOptions & OPTIONS_HIGH)
            config.flags |= CONFIG_HIGH_FLAG;

        config.sample_rate = lSamprate;
        config.num_channels = wChannels;
        config.bits_per_sample = 8;
        config.bytes_per_sample = 1;
        config.channel_mask = 0x5 - wChannels;
        config.worker_threads = 4;
        config.qmode = QMODE_DSD_MSB_FIRST;
        WavpackSetFileInformation (wpc, "dff", WP_FORMAT_DFF);
        out->decoder = decodeInit (wChannels, (dwOptions & OPTIONS_PROOF) ? 5 : 3, MODULATE_MULTITHREADED);
        out->dsd_buffer_size = *lplChunkSize / wChannels / 4;
        out->dsd_buffer = malloc (out->dsd_buffer_size * wChannels);
    }
    else {
        if (dwOptions & OPTIONS_VERY_HIGH)
            config.flags |= (CONFIG_VERY_HIGH_FLAG | CONFIG_HIGH_FLAG);
        else if (dwOptions & OPTIONS_HIGH)
            config.flags |= CONFIG_HIGH_FLAG;
        else if (dwOptions & OPTIONS_FAST)
            config.flags |= CONFIG_FAST_FLAG;

        if (dwOptions & (OPTIONS_EXTRA * 7)) {
            config.flags |= CONFIG_EXTRA_MODE;
            config.xmode = (dwOptions & (OPTIONS_EXTRA * 7)) / OPTIONS_EXTRA;
        }

        if (dwOptions & OPTIONS_HYBRID) {
            config.bitrate = (float)(((dwOptions & OPTIONS_BITRATE) >> 20) / 128.0);
            config.flags |= CONFIG_HYBRID_FLAG;

            if (dwOptions & OPTIONS_WVC) {

                strcpy (wvc_name, lpszFilename);
                strcat (wvc_name, "c");

                if ((out->wvc_file.file = fopen (wvc_name, "wb")) == NULL) {
                    fclose (out->wv_file.file);
                    remove (lpszFilename);
                    WavpackCloseFile (wpc);
                    free (out);
                    return 0;
                }

                config.flags |= CONFIG_CREATE_WVC;
            }
        }

        if (wBitsPerSample == 32) {
            if (dwOptions & OPTIONS_FLOAT20) {
                lSize = lSize / 4 * 3;
                wBitsPerSample = 20;
            }
            else if (dwOptions & OPTIONS_FLOAT24) {
                lSize = lSize / 4 * 3;
                wBitsPerSample = 24;
            }
            else if (dwOptions & OPTIONS_NORMALIZE)
                format = 3;
            else
                wavhdrsize += 4;
        }

        config.sample_rate = lSamprate;
        config.num_channels = wChannels;
        config.bits_per_sample = wBitsPerSample;
        config.bytes_per_sample = (wBitsPerSample + 7) / 8;
        config.channel_mask = 0x5 - wChannels;
        config.worker_threads = 4;

        if (wBitsPerSample == 32)
            config.float_norm_exp = (format == 3) ? WAV_IEEE_FLOAT_NORMAL : COOL_EDIT_FLOAT_NORMAL;
    }

    WavpackSetConfiguration64 (wpc, &config, total_samples, NULL);

    // For DSD files we do not create file wrappers for DSDIFF files, so cuelists and such are not
    // stored. We probably could store these in a DSDIFF file because they're just RIFF, but for now
    // the user needs to store intermediate DXD-e files as 32-bit float or 24-bit (without dither).

    if (!(dwOptions & OPTIONS_DSD)) {
        memcpy (riffhdr.ckID, "RIFF", sizeof (riffhdr.ckID));
        riffhdr.ckSize = sizeof (riffhdr) + wavhdrsize + sizeof (datahdr) + lSize;
        memcpy (riffhdr.formType, "WAVE", sizeof (riffhdr.formType));

        memcpy (fmthdr.ckID, "fmt ", sizeof (fmthdr.ckID));
        fmthdr.ckSize = wavhdrsize;

        wavhdr.FormatTag = format;
        wavhdr.NumChannels = wChannels;
        wavhdr.SampleRate = lSamprate;
        wavhdr.BytesPerSecond = lSamprate * wChannels * ((wBitsPerSample + 7) / 8);
        wavhdr.BlockAlign = ((wBitsPerSample + 7) / 8) * wChannels;
        wavhdr.BitsPerSample = wBitsPerSample;

        if (wavhdrsize == 20) {
            wavhdr.cbSize = 2;
            wavhdr.ValidBitsPerSample = 1;
        }

        memcpy (datahdr.ckID, "data", sizeof (datahdr.ckID));
        datahdr.ckSize = lSize;

        // write the RIFF chunks up to just before the data starts

        if (!WavpackAddWrapper (wpc, &riffhdr, sizeof (riffhdr)) ||
            !WavpackAddWrapper (wpc, &fmthdr, sizeof (fmthdr)) ||
            !WavpackAddWrapper (wpc, &wavhdr, fmthdr.ckSize) ||
            !WavpackAddWrapper (wpc, &datahdr, sizeof (datahdr))) {
                fclose (out->wv_file.file);
                remove (lpszFilename);

                if (out->wvc_file.file) {
                    fclose (out->wv_file.file);
                    remove (wvc_name);
                }

                WavpackCloseFile (wpc);
                free (out);
                return 0;
        }
    }

    WavpackPackInit (wpc);
    return out;
}


//////////////////////////////////////////////////////////////////////////////
// Write data to Wavpack file.                                              //
//////////////////////////////////////////////////////////////////////////////

// Write DSD audio through the DXD-e decoder. There is a latency introduced here
// because of the DXD-e pilot signal detection so setting lBytes to <= 0 is
// provided to flush the operation.

static DWORD PASCAL WriteFilterOutputDSD (HANDLE hOutput, BYTE *lpbData, long lBytes)
{
    int num_channels, result, samples, samples_decoded;
    OUTPUT *out = hOutput;
    WavpackContext *wpc;

    wpc = out->wpc;
    num_channels = WavpackGetNumChannels (wpc);
    samples = lBytes / 4 / num_channels;

    if (lpbData && lBytes > 0) {
        int32_t *ibuffer = (int32_t *) lpbData;
        float *fbuffer = (float *) lpbData;
        int scount = samples * num_channels;

        out->source_samples += samples;

        while (scount--) {
            float sample24 = *fbuffer++ * 256.0F;

            if (sample24 > 8388607)
                *ibuffer++ = 8388607;
            else if (sample24 < -8388608)
                *ibuffer++ = -8388608;
            else
                *ibuffer++ = (int32_t) sample24;
        }
    }
    else
        samples = -1;

    samples_decoded = decodeProcess (out->decoder, (int32_t *) lpbData, samples, out->dsd_buffer, out->dsd_buffer_size);

    if (samples_decoded) {
        int32_t *ibuffer = (int32_t *) lpbData, *iptr = ibuffer;
        int scount = samples_decoded * num_channels;
        unsigned char *dptr = out->dsd_buffer;

        if (samples == -1)
            iptr = ibuffer = malloc (scount * sizeof (int32_t));

        while (scount--)
            *iptr++ = *dptr++;

        result = WavpackPackSamples (wpc, ibuffer, samples_decoded);

        if (samples == -1)
            free (ibuffer);

        out->written_samples += samples_decoded;
        return result ? lBytes : 0;
    }

    return 0;
}

DWORD PASCAL WriteFilterOutput (HANDLE hOutput, BYTE *lpbData, long lBytes)
{
    int bits_per_sample, bytes_per_sample, num_channels, result;
    int32_t *buffer = (int32_t *) lpbData, samples;
    OUTPUT *out = hOutput;
    WavpackContext *wpc;
    DWORD dwOptions;

    if (!out)
        return 0;

    if (out->dwOptions & OPTIONS_DSD)
        return WriteFilterOutputDSD (hOutput, lpbData, lBytes);
    else {
        wpc = out->wpc;
        dwOptions = out->dwOptions;
        num_channels = WavpackGetNumChannels (wpc);
        bits_per_sample = WavpackGetBitsPerSample (wpc);
        bytes_per_sample = WavpackGetBytesPerSample (wpc);

        if (bytes_per_sample == 3)
            samples = lBytes / 4 / num_channels;
        else
            samples = lBytes / bytes_per_sample / num_channels;

        if (bytes_per_sample < 3)
            buffer = malloc (samples * num_channels * 4);

        if (bytes_per_sample == 1) {
            int32_t samcnt = samples * num_channels, *out = buffer;
            unsigned char *inp = (unsigned char *) lpbData;

            while (samcnt--)
                *out++ = *inp++ - 128;
        }
        else if (bytes_per_sample == 2) {
            int32_t samcnt = samples * num_channels, *out = buffer;
            short *inp = (short *) lpbData;

            while (samcnt--)
                *out++ = *inp++;
        }
        else if (bytes_per_sample == 3) {
            float fMult, fMin, fMax, fDither = 0.5, *fPtr = (float *) lpbData;
            int32_t temp, random = out->random;
            int sc = samples;

            fMult = (float)((bits_per_sample == 24) ? 256.0 : 16.0);
            fMin = (float)((bits_per_sample == 24) ? -8388608.0 : -524288.0);
            fMax = (float)((bits_per_sample == 24) ? 8388607.0 : 524287.0);

            while (sc--) {
                * (float*) fPtr *= fMult;

                if (dwOptions & OPTIONS_NOISESHAPE)
                    * (float*) fPtr -= out->error [0];

                if (dwOptions & OPTIONS_DITHER) {
                    temp = (random = (((random << 4) - random) ^ 1)) >> 1;
                    temp += (random = (((random << 4) - random) ^ 1)) >> 1;
                    fDither = (float)(temp * (1/2147483648.0) + 0.5);
                }

                if (* (float*) fPtr > fMax)
                    out->error [0] = (float)((temp = (int32_t) fMax) - * (float*) fPtr);
                else if (* (float*) fPtr < fMin)
                    out->error [0] = (float)((temp = (int32_t) fMin) - * (float*) fPtr);
                else
                    out->error [0] = (temp = (int32_t) floor (* (float*) fPtr + fDither)) - * (float*) fPtr;

                * (long*) fPtr++ = (bits_per_sample == 24) ? temp : temp << 4;

                if (num_channels == 1)
                    continue;

                * (float*) fPtr *= fMult;

                if (dwOptions & OPTIONS_NOISESHAPE)
                    * (float*) fPtr -= out->error [1];

                if (dwOptions & OPTIONS_DITHER) {
                    temp = (random = (((random << 4) - random) ^ 1)) >> 1;
                    temp += (random = (((random << 4) - random) ^ 1)) >> 1;
                    fDither = (float)(temp * (1/2147483648.0) + 0.5);
                }

                if (* (float*) fPtr > fMax)
                    out->error [1] = (temp = (int32_t) fMax) - * (float*) fPtr;
                else if (* (float*) fPtr < fMin)
                    out->error [1] = (temp = (int32_t) fMin) - * (float*) fPtr;
                else
                    out->error [1] = (temp = (int32_t) floor (* (float*) fPtr + fDither)) - * (float*) fPtr;

                * (long*) fPtr++ = (bits_per_sample == 24) ? temp : temp << 4;
            }

            out->random = random;
        }
        else if (WavpackGetFloatNormExp (wpc) == WAV_IEEE_FLOAT_NORMAL)
            WavpackFloatNormalize ((int32_t *) lpbData, samples * num_channels,
                WAV_IEEE_FLOAT_NORMAL - COOL_EDIT_FLOAT_NORMAL);

        result = WavpackPackSamples (wpc, buffer, samples);

        if (bytes_per_sample < 3)
            free (buffer);

        return result ? lBytes : 0;
    }

    return 0;
}


//////////////////////////////////////////////////////////////////////////////
// Close the WavPack file and free the OUTPUT structure. We have to seek    //
// back to the beginning of the file to rewrite the WavpackHeader with the  //
// updated crc(s) and possibly rewrite the RIFF header if we wrote some     //
// extra RIFF data.                                                         //
//////////////////////////////////////////////////////////////////////////////

static DWORD dump_list_chunk (OUTPUT *out);

void PASCAL CloseFilterOutput (HANDLE hOutput)
{
    OUTPUT *out = hOutput;
    WavpackContext *wpc;

    if (out) {
        wpc = out->wpc;

        // if we're writing DSD, flush the final DSD data from the DXD-e decoder
        // otherwise generate the list chunks (which don't apply to DSD output)

        if (out->decoder)
            WriteFilterOutputDSD (hOutput, NULL, -1);
        else
            dump_list_chunk (out);

        WavpackFlushSamples (wpc);

        if (out->wv_file.error || out->wvc_file.error) {
            MessageBox (NULL, "Error writing file, disk probably full!", "WavPack", MB_OK);
            WavpackCloseFile (wpc);

            if (out->wvc_file.file)
                fclose (out->wv_file.file);

            if (out->wvc_file.file)
                fclose (out->wvc_file.file);

            free (out);
            return;
        }

        // Again, for DSD files we currently do not write any extra RIFF chunks

        if (!out->decoder && (WavpackGetSampleIndex (wpc) != WavpackGetNumSamples (wpc) || out->special_bytes)) {
            char *block_buff = malloc (out->wv_file.first_block_size);

            fseek (out->wv_file.file, 0, SEEK_SET);
            fread (block_buff, out->wv_file.first_block_size, 1, out->wv_file.file);

            if (WavpackGetWrapperLocation (block_buff, NULL)) {
                RiffChunkHeader *riffhdr = WavpackGetWrapperLocation (block_buff, NULL);
                ChunkHeader *fmthdr = (ChunkHeader *) (riffhdr + 1);
                ChunkHeader *datahdr = (ChunkHeader *)((char *) fmthdr + fmthdr->ckSize + 8);
                uint32_t datasize = WavpackGetSampleIndex (wpc) * WavpackGetNumChannels (wpc) *
                    WavpackGetBytesPerSample (wpc);

                if (!strncmp (riffhdr->ckID, "RIFF", 4))
                    riffhdr->ckSize = sizeof (*riffhdr) + fmthdr->ckSize +
                        sizeof (*datahdr) + datasize + out->special_bytes;

                if (!strncmp (datahdr->ckID, "data", 4))
                    datahdr->ckSize = datasize;
            }

            // this must be done last (and unconditionally) to ensure block checksum is updated correctly

            WavpackUpdateNumSamples (wpc, block_buff);

            fseek (out->wv_file.file, 0, SEEK_SET);
            fwrite (block_buff, out->wv_file.first_block_size, 1, out->wv_file.file);
            free (block_buff);

            if (out->wvc_file.file && WavpackGetSampleIndex (wpc) != WavpackGetNumSamples (wpc)) {
                block_buff = malloc (out->wvc_file.first_block_size);
                fseek (out->wvc_file.file, 0, SEEK_SET);
                fread (block_buff, out->wvc_file.first_block_size, 1, out->wvc_file.file);
                WavpackUpdateNumSamples (wpc, block_buff);
                fseek (out->wvc_file.file, 0, SEEK_SET);
                fwrite (block_buff, out->wvc_file.first_block_size, 1, out->wvc_file.file);
                free (block_buff);
            }
        }

        WavpackCloseFile (wpc);
        fclose (out->wv_file.file);

        if (out->wvc_file.file)
            fclose (out->wvc_file.file);

        if (out->decoder) {
            logprintf ("CloseFilterOutput(): source samples = %d, written samples = %d, embedded samples = %d (%.2f%%), %d seconds\n",
                out->source_samples * WavpackGetNumChannels (wpc), out->written_samples * WavpackGetNumChannels (wpc), decodeTotalEmbeddedSamples (out->decoder),
                (decodeTotalEmbeddedSamples (out->decoder) * 100.0) / (out->written_samples * WavpackGetNumChannels (wpc)), time (NULL) - out->start_time);

            decodeFree (out->decoder);
        }

        free (out->dsd_buffer);
        free (out);
    }

    logprintf ("CloseFilterOutput() done\n\n");
}


//////////////////////////////////////////////////////////////////////////////
// Open file for input and store information retrieved from file. For "raw" //
// WavPack files we don't know the sample rate so we set this to zero and   //
// CoolEdit will query the all-knowing user.                                //
//////////////////////////////////////////////////////////////////////////////

typedef struct {
    RiffChunkHeader listhdr;
    WavpackContext *wpc;
    uint32_t special_bytes;
    unsigned char *special_data;
    int legacy_warned;

    // DSD-specific items
    int dsd_buffer_size, dsd_buffer_samples;
    unsigned char *dsd_buffer;
    DecimateDSD *decimator;
    EmbedDSD *embedder;
} WavpackInput;

HANDLE PASCAL OpenFilterInput (LPSTR lpszFilename, long *lplSamprate,
    WORD *lpwBitsPerSample, WORD *lpwChannels, HWND hWnd, long *lplChunkSize)
{
    WavpackContext *wpc;
    char error [256];
    WavpackInput *in;

    logprintf ("OpenFilterInput() *** %s ***\n", lpszFilename);

    if ((in = malloc (sizeof (WavpackInput))) == NULL)
        return 0;

    CLEAR (*in);

    wpc = in->wpc = WavpackOpenFileInput (lpszFilename, error,
        OPEN_WVC | OPEN_WRAPPER | OPEN_DSD_NATIVE | (4 << OPEN_THREADS_SHFT), 0);

    if (!wpc) {
        logprintf ("OpenFilterInput (%s) failed\n\n", lpszFilename);
        free (in);
        return 0;
    }

    if (WavpackGetNumSamples (wpc) == (uint32_t) -1) {
        logprintf ("OpenFilterInput (%s) failed\n\n", lpszFilename);
        free (in);
        return WavpackCloseFile (wpc);
    }

    *lplSamprate = WavpackGetSampleRate (wpc);
    *lpwChannels = WavpackGetNumChannels (wpc);

    if (WavpackGetQualifyMode (wpc) & QMODE_DSD_AUDIO)
        *lpwBitsPerSample = 32;
    else
        *lpwBitsPerSample = WavpackGetBitsPerSample (wpc) > 16 ? 32 :
            (WavpackGetBitsPerSample (wpc) > 8 ? 16 : 8);

    // set the chunk size large enough for multithreading to kick in

    if (*lpwChannels > 2)
        *lplChunkSize = 262144;
    else
        *lplChunkSize = 240000 * *lpwChannels * (*lpwBitsPerSample / 8);

    // For DSD reading we use the WavPack library in native mode and do the decimation here using
    // the DXD-e library that provides high-quality sample-aligned decimation and the ability to
    // embed the DSD stream into the PCM (i.e., DXD-e).

    if (WavpackGetQualifyMode (wpc) & QMODE_DSD_AUDIO) {
        in->dsd_buffer_size = *lplChunkSize / *lpwChannels / 4 + 128;   // prevents having to double-read each call
        in->dsd_buffer = malloc (in->dsd_buffer_size * *lpwChannels);
        in->decimator = decimateDSDinit (*lpwChannels, 0);
        in->embedder = embedDSDinit (*lpwChannels, EMBED_PILOT_SIGNAL | EMBED_PILOT_UNIQUE);
        logprintf ("Initialized DSD Input successfully, dsd_buffer_size = %d\n", in->dsd_buffer_size);
    }

    logprintf ("OpenFilterInput (%s): %d,%d,%d\n",
        lpszFilename, (int) *lplSamprate, (int) *lpwBitsPerSample, (int) *lpwChannels);

    return in;
}


//////////////////////////////////////////////////////////////////////////////
// Return the audio data size of the open file, 20/24-bit files are floats. //
//////////////////////////////////////////////////////////////////////////////

DWORD PASCAL FilterGetFileSize (HANDLE hInput)
{
    WavpackInput *in = hInput;

    if (in && in->wpc && WavpackGetNumSamples (in->wpc) != (uint32_t) -1) {
        if (WavpackGetQualifyMode (in->wpc) & QMODE_DSD_AUDIO)
            return WavpackGetNumSamples (in->wpc) * WavpackGetNumChannels (in->wpc) * 4;
        else
            return WavpackGetNumSamples (in->wpc) * WavpackGetNumChannels (in->wpc) *
                (WavpackGetBitsPerSample (in->wpc) > 16 ? 4 :
                (WavpackGetBitsPerSample (in->wpc) > 8 ? 2 : 1));
    }

    return 0;
}


//////////////////////////////////////////////////////////////////////////////
// Read the specified number of audio data bytes from the open file.        //
//////////////////////////////////////////////////////////////////////////////

DWORD PASCAL ReadFilterInput (HANDLE hInput, BYTE *lpbData, long lBytes)
{
    WavpackInput *in = hInput;
    WavpackContext *wpc = in->wpc;
    int num_channels = WavpackGetNumChannels (wpc);

    if (!in || !in->wpc)
        return 0;

    if (!in->legacy_warned && WavpackGetVersion (in->wpc) < 4) {
        in->legacy_warned++;
        MessageBox (NULL,
            "This legacy file is deprecated and its use is not recommended.\n"
            "Future versions of this plugin may not read this file. Use this\n"
            "plugin or the WavPack 4.80 command-line program to\n"
            "transcode files of this vintage to a more recent version.", "WavPack Legacy Notification", MB_OK);
    }

    // Read DSD files in native mode and use the DXD-e library to do the decimation and DSD embedding.
    // Note that this is slightly complicated by the fact that the decimation code has (currently) a
    // 6-sample latency (from the 104-tap sinc filter), so we have to keep some DSD samples around
    // between calls to embed them next time.

    if (in->dsd_buffer) {
        int32_t *buffer = (int32_t *) lpbData;
        float *out = (float *) lpbData;
        int bytes_returned = 0;

        while (lBytes) {
            int samples_to_read = lBytes / 4 / num_channels, samples_read, samples_decimated, scount;
            unsigned char *dsd_buffer_offset = in->dsd_buffer + in->dsd_buffer_samples * num_channels;

            if (samples_to_read + in->dsd_buffer_samples > in->dsd_buffer_size)
                samples_to_read = in->dsd_buffer_size - in->dsd_buffer_samples;

            if (WavpackGetSampleIndex (wpc) + samples_to_read > WavpackGetNumSamples (wpc))
                samples_to_read = WavpackGetNumSamples (wpc) - WavpackGetSampleIndex (wpc);

            samples_read = WavpackUnpackSamples (wpc, buffer, samples_to_read);
            in->dsd_buffer_samples += samples_read;
            scount = samples_read * num_channels;

            for (int i = 0; i < scount; ++i)
                dsd_buffer_offset [i] = buffer [i] & 0xff;

            samples_decimated = decimateDSDrun (in->decimator, dsd_buffer_offset, samples_read ? samples_read : -1, buffer);

            if (!samples_read && !samples_decimated)
                break;

            embedDSDrun (in->embedder, buffer, in->dsd_buffer, samples_decimated);

            in->dsd_buffer_samples -= samples_decimated;

            if (in->dsd_buffer_samples)
                memmove (in->dsd_buffer, in->dsd_buffer + samples_decimated * num_channels, in->dsd_buffer_samples * num_channels);

            scount = samples_decimated * num_channels;

            for (int i = 0; i < scount; ++i)
                *out++ = *buffer++ / 256.0F;

            bytes_returned += samples_decimated * num_channels * 4;
            lBytes -= samples_decimated * num_channels * 4;
        }

        return bytes_returned;
    }
    else {
        int bytes_per_sample = WavpackGetBitsPerSample (wpc) > 16 ? 4 :
            (WavpackGetBitsPerSample (wpc) > 8 ? 2 : 1);
        int padding_bits = (WavpackGetBytesPerSample (wpc) - (WavpackGetBitsPerSample (wpc) + 7) / 8) * 8;
        int32_t samples_to_read = lBytes / bytes_per_sample / num_channels;
        int32_t *buffer = (int32_t *) lpbData;

        if (WavpackGetSampleIndex (wpc) + samples_to_read > WavpackGetNumSamples (wpc))
            samples_to_read = WavpackGetNumSamples (wpc) - WavpackGetSampleIndex (wpc);

        if (bytes_per_sample != 4)
            buffer = malloc (samples_to_read * num_channels * 4);

        samples_to_read = WavpackUnpackSamples (wpc, buffer, samples_to_read);

        if (bytes_per_sample == 1) {
            int32_t samcnt = samples_to_read * num_channels, *inp = buffer;
            unsigned char *out = (unsigned char *) lpbData;

            if (padding_bits)
                while (samcnt--)
                    *out++ = (*inp++ >> padding_bits) + 128;
            else
                while (samcnt--)
                    *out++ = *inp++ + 128;
        }
        else if (bytes_per_sample == 2) {
            int32_t samcnt = samples_to_read * num_channels, *inp = buffer;
            short *out = (short *) lpbData;

            if (padding_bits)
                while (samcnt--)
                    *out++ = *inp++ >> padding_bits;
            else
                while (samcnt--)
                    *out++ = *inp++;
        }
        else if (!(WavpackGetMode (wpc) & MODE_FLOAT)) {
            int32_t samcnt = samples_to_read * num_channels;
            float *out = (float *) buffer, factor = 1.0 / 256.0;

            if (WavpackGetBitsPerSample (wpc) > 24)
                factor /= 256.0;

            if (padding_bits)
                while (samcnt--)
                    *out++ = (*buffer++ >> padding_bits) * factor;
            else
                while (samcnt--)
                    *out++ = *buffer++ * factor;
        }
        else if ((WavpackGetMode (wpc) & MODE_FLOAT) && WavpackGetFloatNormExp (wpc) != COOL_EDIT_FLOAT_NORMAL)
            WavpackFloatNormalize (buffer, samples_to_read * num_channels,
                COOL_EDIT_FLOAT_NORMAL - WavpackGetFloatNormExp (wpc));

        if (bytes_per_sample != 4)
            free (buffer);

        return samples_to_read * bytes_per_sample * num_channels;
    }
}


//////////////////////////////////////////////////////////////////////////////
// Close input file and release WavpackInput structure.                     //
//////////////////////////////////////////////////////////////////////////////

void PASCAL CloseFilterInput (HANDLE hInput)
{
    WavpackInput *in = hInput;

    if (in) {
        if (WavpackGetNumErrors (in->wpc)) {
            char message [80];
            sprintf (message, "CRC errors detected in %d block(s)!", WavpackGetNumErrors (in->wpc));
            MessageBox (NULL, message, "WavPack", MB_OK);
        }

        if (in->wpc)
            WavpackCloseFile (in->wpc);

        if (in->dsd_buffer) {
            free (in->dsd_buffer);
            decimateDSDdestroy (in->decimator);
            embedDSDdestroy (in->embedder);
        }

        free (in);
        logprintf ("CloseFilterInput()\n\n");
    }
}


static INT_PTR CALLBACK WavPackDlgProc (HWND, UINT, WPARAM, LPARAM);
static int std_bitrate (int bitrate);

// these variables are use to communicate with the dialog routines

static int iCurrentMode;        // 4 lossless modes + 4 hybrid modes
static int iCurrentFloatBits;   // 1, 20, 24, or 32 bit storage of floats
static int iCurrentFlags;       // misc options
static int iCurrentBitrate, iMinBitrate; // kbps

//////////////////////////////////////////////////////////////////////////////
// Query user for WavPack file writing options. This also has the function  //
// of converting the 32-bit configuration word that CoolEdit uses from a    //
// possibly "raw" form (read directly from an input file) into a form that  //
// we can use for writing. This is done because some WavPack file formats   //
// can no longer be written (like the old lossy mode) and because some      //
// things we are not supporting here (like hybrid joint stereo and very     //
// fast mode) and some things are simply not knowable from reading a file   //
// (like the dither and noiseshaping modes used when converting floats).    //
// This function can also be used just to convert from "raw" to "usable"    //
// configuration by just calling it with NULL window and instance args.     //
//////////////////////////////////////////////////////////////////////////////

DWORD PASCAL FilterGetOptions (HWND hWnd, HINSTANCE hInst, long lSamprate, WORD wChannels, WORD wBitsPerSample, DWORD dwOptions)
{
    int dxd_rate = lSamprate % 352800 == 0 || lSamprate % 384000 == 0;
    int float_data = wBitsPerSample == 32;
    int force_std_bitrate = FALSE;

    logprintf ("FilterGetOptions(), dialog = %s, rate = %d, channels = %d, bits = %d, options = %s\n",
        (hWnd && hInst) ? "yes" : "no", (int) lSamprate, wChannels, wBitsPerSample, options_string (dwOptions));

    if ((dwOptions & OPTIONS_VERSION) != 1)
        dwOptions = 0;

    // if this is the first time through with this options list (from the file), set some things...

    if (!(dwOptions & OPTIONS_UPDATED)) {
        dwOptions |= OPTIONS_NOISESHAPE;    // noise-shaping ON, dither OFF (we have no idea how the file was saved
                                            // and we don't want dither because that means lossy for 20/24-bit)

        dwOptions &= ~OPTIONS_DSD;          // re-save DSD as normalized 32-bit float, again because that's lossless
                                            // and we can store cuelists and other RIFF items (user can specify DSD
                                            // export as the last step after editing)

        force_std_bitrate = TRUE;
    }

    if (dwOptions & OPTIONS_HYBRID) {
        if (dwOptions & OPTIONS_FAST)
            iCurrentMode = IDC_HYBRID_FAST;
        else if (dwOptions & OPTIONS_VERY_HIGH)
            iCurrentMode = IDC_HYBRID_VHIGH;
        else if (dwOptions & OPTIONS_HIGH)
            iCurrentMode = IDC_HYBRID_HIGH;
        else
            iCurrentMode = IDC_HYBRID;
    }
    else {
        if (dwOptions & OPTIONS_FAST)
            iCurrentMode = IDC_LOSSLESS_FAST;
        else if (dwOptions & OPTIONS_VERY_HIGH)
            iCurrentMode = IDC_LOSSLESS_VHIGH;
        else if (dwOptions & OPTIONS_HIGH)
            iCurrentMode = IDC_LOSSLESS_HIGH;
        else
            iCurrentMode = IDC_LOSSLESS;
    }

    iCurrentFlags = dwOptions & (OPTIONS_NOISESHAPE | OPTIONS_DITHER | OPTIONS_WVC |
                OPTIONS_NORMALIZE | OPTIONS_PROOF | (OPTIONS_EXTRA * 7));

    if (dwOptions & OPTIONS_FLOAT20)
        iCurrentFloatBits = IDC_FLOAT20;
    else if (dwOptions & OPTIONS_FLOAT24)
        iCurrentFloatBits = IDC_FLOAT24;
    else if (dwOptions & OPTIONS_DSD)
        iCurrentFloatBits = IDC_DSD;
    else
        iCurrentFloatBits = IDC_FLOAT32;

    // generate the minimum and default bitrates for hybrid modes

    iMinBitrate = (int) floor (lSamprate / 1000.0 * 2.22 * wChannels + 0.5);

    if (dwOptions & OPTIONS_BITRATE)
        iCurrentBitrate = (int) floor (lSamprate / 1000.0 * ((dwOptions >> 20) / 128.0) * wChannels + 0.5);
    else {
        iCurrentBitrate = (int) floor (lSamprate / 1000.0 * 3.8 * wChannels + 0.5);
        force_std_bitrate = TRUE;
    }

    if (force_std_bitrate && !std_bitrate (iCurrentBitrate)) {
        int delta;

        for (delta = 1; ; delta++)
            if (iCurrentBitrate - delta >= iMinBitrate && std_bitrate (iCurrentBitrate - delta)) {
                iCurrentBitrate -= delta;
                break;
            }
            else if (std_bitrate (iCurrentBitrate + delta)) {
                iCurrentBitrate += delta;
                break;
            }
    }

    // If the instance and window args are not NULL, execute the dialog box
    // and return the unchanged configuration if the user hit "cancel". The
    // optional parameter that can be passed to the dialog is used to indicate
    // whether the float conversion options should be enabled.

    if (hInst && hWnd && !DialogBoxParam (hInst, "CoolEdit", hWnd, WavPackDlgProc, float_data ? float_data + dxd_rate : 0)) {
        logprintf ("FilterGetOptions() after dialog Cancel, options = %s\n", options_string (dwOptions));
        return dwOptions;
    }

    // now generate a configuration that is usable for writing from the options
    // that the user selected (or at least approved of)

    dwOptions = OPTIONS_UPDATED + 1;

    if (iCurrentMode >= IDC_HYBRID) {
        dwOptions |= (int) floor (iCurrentBitrate * 128000.0 / lSamprate / wChannels + 0.5) << 20;
        dwOptions |= OPTIONS_HYBRID;
    }

    if (iCurrentMode == IDC_LOSSLESS_VHIGH || iCurrentMode == IDC_HYBRID_VHIGH)
        dwOptions |= OPTIONS_HIGH | OPTIONS_VERY_HIGH;
    else if (iCurrentMode == IDC_LOSSLESS_HIGH || iCurrentMode == IDC_HYBRID_HIGH)
        dwOptions |= OPTIONS_HIGH;
    else if (iCurrentMode == IDC_LOSSLESS_FAST || iCurrentMode == IDC_HYBRID_FAST)
        dwOptions |= OPTIONS_FAST;

    if (iCurrentFloatBits == IDC_FLOAT20)
        dwOptions |= OPTIONS_FLOAT20;
    else if (iCurrentFloatBits == IDC_FLOAT24)
        dwOptions |= OPTIONS_FLOAT24;
    else if (iCurrentFloatBits == IDC_DSD)
        dwOptions |= OPTIONS_DSD;

    dwOptions |= iCurrentFlags & (OPTIONS_NOISESHAPE | OPTIONS_DITHER | OPTIONS_WVC |
                OPTIONS_NORMALIZE | OPTIONS_PROOF | (OPTIONS_EXTRA * 7));

    logprintf ("FilterGetOptions() after dialog OK, options = %s\n", options_string (dwOptions));
    return dwOptions;
}


//////////////////////////////////////////////////////////////////////////////
// Standard callback for handling dialogs. The optional parameter is used   //
// to indicate whether the float conversion options should be disabled and  //
// the return value indicates whether the user chose OK or CANCEL.          //
//////////////////////////////////////////////////////////////////////////////

// Helper function to handle the change back and forth from DSD output and
// the other float storage options. Note when outputting DSD the only available
// options are "high" lossless mode and the "proof" quality mode.

static void dialog_helper (HWND hDlg, LPARAM lParam)
{
    EnableWindow (GetDlgItem (hDlg, IDC_NORMALIZE), iCurrentFloatBits != IDC_DSD && lParam && iCurrentFloatBits == IDC_FLOAT32);
    EnableWindow (GetDlgItem (hDlg, IDC_NOISESHAPE), iCurrentFloatBits != IDC_DSD && lParam && iCurrentFloatBits != IDC_FLOAT32);
    EnableWindow (GetDlgItem (hDlg, IDC_DITHER), iCurrentFloatBits != IDC_DSD && lParam && iCurrentFloatBits != IDC_FLOAT32);
    EnableWindow (GetDlgItem (hDlg, IDC_PROOF), iCurrentFloatBits == IDC_DSD && lParam > 1 && TRUE);

    EnableWindow (GetDlgItem (hDlg, IDC_HYBRID_GROUP), iCurrentFloatBits != IDC_DSD && iCurrentMode >= IDC_HYBRID);
    EnableWindow (GetDlgItem (hDlg, IDC_BITRATE), iCurrentFloatBits != IDC_DSD && iCurrentMode >= IDC_HYBRID);
    EnableWindow (GetDlgItem (hDlg, IDC_BITRATE_TEXT), iCurrentFloatBits != IDC_DSD && iCurrentMode >= IDC_HYBRID);
    EnableWindow (GetDlgItem (hDlg, IDC_CORRECTION), iCurrentFloatBits != IDC_DSD && iCurrentMode >= IDC_HYBRID);

    EnableWindow (GetDlgItem (hDlg, IDC_EXTRA_SLIDER), iCurrentFloatBits != IDC_DSD);
    EnableWindow (GetDlgItem (hDlg, IDC_EXTRA_GROUP), iCurrentFloatBits != IDC_DSD);

    EnableWindow (GetDlgItem (hDlg, IDC_MODE_GROUP), iCurrentFloatBits != IDC_DSD);
    EnableWindow (GetDlgItem (hDlg, IDC_LOSSLESS_VHIGH), iCurrentFloatBits != IDC_DSD);
    EnableWindow (GetDlgItem (hDlg, IDC_LOSSLESS_FAST), iCurrentFloatBits != IDC_DSD);
    EnableWindow (GetDlgItem (hDlg, IDC_HYBRID), iCurrentFloatBits != IDC_DSD);
    EnableWindow (GetDlgItem (hDlg, IDC_HYBRID_HIGH), iCurrentFloatBits != IDC_DSD);
    EnableWindow (GetDlgItem (hDlg, IDC_HYBRID_VHIGH), iCurrentFloatBits != IDC_DSD);
    EnableWindow (GetDlgItem (hDlg, IDC_HYBRID_FAST), iCurrentFloatBits != IDC_DSD);

    if (iCurrentFloatBits == IDC_DSD) {
        int high_selected = iCurrentMode == IDC_LOSSLESS_HIGH || iCurrentMode == IDC_LOSSLESS_VHIGH ||
            iCurrentMode == IDC_HYBRID_HIGH || iCurrentMode == IDC_HYBRID_VHIGH;

        CheckDlgButton (hDlg, IDC_LOSSLESS, !high_selected);
        CheckDlgButton (hDlg, IDC_LOSSLESS_HIGH, high_selected);
    }
    else
        CheckRadioButton (hDlg, IDC_LOSSLESS, IDC_HYBRID_FAST, iCurrentMode);
}

static INT_PTR CALLBACK WavPackDlgProc (HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    char str [160];
    int i;

    switch (message) {
        case WM_INITDIALOG:
            EnableWindow (GetDlgItem (hDlg, IDC_HYBRID_GROUP), iCurrentMode >= IDC_HYBRID);
            EnableWindow (GetDlgItem (hDlg, IDC_BITRATE), iCurrentMode >= IDC_HYBRID);
            EnableWindow (GetDlgItem (hDlg, IDC_BITRATE_TEXT), iCurrentMode >= IDC_HYBRID);
            EnableWindow (GetDlgItem (hDlg, IDC_CORRECTION), iCurrentMode >= IDC_HYBRID);

            EnableWindow (GetDlgItem (hDlg, IDC_FLOAT_GROUP), lParam);
            EnableWindow (GetDlgItem (hDlg, IDC_FLOAT20), lParam);
            EnableWindow (GetDlgItem (hDlg, IDC_FLOAT24), lParam);
            EnableWindow (GetDlgItem (hDlg, IDC_FLOAT32), lParam);
            EnableWindow (GetDlgItem (hDlg, IDC_DSD), lParam > 1);

            CheckRadioButton (hDlg, IDC_LOSSLESS, IDC_HYBRID_FAST, iCurrentMode);
            CheckRadioButton (hDlg, IDC_FLOAT20, IDC_DSD, iCurrentFloatBits);

            dialog_helper (hDlg, lParam);

            SendDlgItemMessage (hDlg, IDC_BITRATE, CB_LIMITTEXT, 4, 0);

            for (i = iMinBitrate; i <= iMinBitrate * 6; ++i)
                if (i == iMinBitrate || std_bitrate (i)) {
                    sprintf (str, "%d", i);
                    SendDlgItemMessage (hDlg, IDC_BITRATE, CB_ADDSTRING, 0, (LPARAM) str);
                }

            sprintf (str, "%d", iCurrentBitrate);
            SetDlgItemText (hDlg, IDC_BITRATE, str);

            CheckDlgButton (hDlg, IDC_CORRECTION, iCurrentFlags & OPTIONS_WVC);
            CheckDlgButton (hDlg, IDC_NOISESHAPE, iCurrentFlags & OPTIONS_NOISESHAPE);
            CheckDlgButton (hDlg, IDC_DITHER, iCurrentFlags & OPTIONS_DITHER);
            CheckDlgButton (hDlg, IDC_NORMALIZE, iCurrentFlags & OPTIONS_NORMALIZE);
            CheckDlgButton (hDlg, IDC_PROOF, iCurrentFlags & OPTIONS_PROOF);

            SendDlgItemMessage (hDlg, IDC_EXTRA_SLIDER, TBM_SETRANGE, 0, MAKELONG (0, 6));
            SendDlgItemMessage (hDlg, IDC_EXTRA_SLIDER, TBM_SETPOS, 1,
                                (iCurrentFlags & (OPTIONS_EXTRA * 7)) / OPTIONS_EXTRA);

            SetFocus (GetDlgItem (hDlg, iCurrentMode));

            return FALSE;

        case WM_COMMAND:
            switch (LOWORD (wParam)) {
                case IDC_LOSSLESS: case IDC_LOSSLESS_HIGH: case IDC_LOSSLESS_VHIGH: case IDC_LOSSLESS_FAST:
                case IDC_HYBRID: case IDC_HYBRID_HIGH: case IDC_HYBRID_VHIGH: case IDC_HYBRID_FAST:
                    iCurrentMode = LOWORD (wParam);
                    EnableWindow (GetDlgItem (hDlg, IDC_HYBRID_GROUP), iCurrentMode >= IDC_HYBRID);
                    EnableWindow (GetDlgItem (hDlg, IDC_BITRATE), iCurrentMode >= IDC_HYBRID);
                    EnableWindow (GetDlgItem (hDlg, IDC_BITRATE_TEXT), iCurrentMode >= IDC_HYBRID);
                    EnableWindow (GetDlgItem (hDlg, IDC_CORRECTION), iCurrentMode >= IDC_HYBRID);
                    break;

                case IDC_FLOAT20: case IDC_FLOAT24: case IDC_FLOAT32: case IDC_DSD:
                    iCurrentFloatBits = LOWORD (wParam);
                    dialog_helper (hDlg, lParam);
                    break;

                case IDOK:
                    iCurrentFlags = 0;

                    if (IsDlgButtonChecked (hDlg, IDC_CORRECTION))
                        iCurrentFlags |= OPTIONS_WVC;

                    if (IsDlgButtonChecked (hDlg, IDC_NOISESHAPE))
                        iCurrentFlags |= OPTIONS_NOISESHAPE;

                    if (IsDlgButtonChecked (hDlg, IDC_NORMALIZE))
                        iCurrentFlags |= OPTIONS_NORMALIZE;

                    if (IsDlgButtonChecked (hDlg, IDC_DITHER))
                        iCurrentFlags |= OPTIONS_DITHER;

                    if (IsDlgButtonChecked (hDlg, IDC_PROOF))
                        iCurrentFlags |= OPTIONS_PROOF;

                    i = SendDlgItemMessage (hDlg, IDC_EXTRA_SLIDER, TBM_GETPOS, 0, 0);

                    if (i >= 0 && i <= 6)
                        iCurrentFlags |= OPTIONS_EXTRA * i;

                    GetWindowText (GetDlgItem (hDlg, IDC_BITRATE), str, sizeof (str));

                    if (atol (str) && atol (str) <= 9999)
                        iCurrentBitrate = atol (str) < iMinBitrate ? iMinBitrate : atol (str);

                    logprintf ("GetWindowText = \"%s\", iCurrentBitrate = %d\n", str, iCurrentBitrate);
                    EndDialog (hDlg, TRUE);
                    return TRUE;

                case IDCANCEL:
                    EndDialog (hDlg, FALSE);
                    return TRUE;

                case IDABOUT:
#ifdef _MSC_VER
                    sprintf (str, "Cool Edit / Audition Filter Version 5.0\n" "MSVC build / libwavpack %s\n"
                        "Copyright (c) 2026 David Bryant", WavpackGetLibraryVersionString());
#elif defined __MINGW32__
                    sprintf (str, "Cool Edit / Audition Filter Version 5.0\n" "MinGW build / libwavpack %s\n"
                        "Copyright (c) 2026 David Bryant", WavpackGetLibraryVersionString());
#else
                    sprintf (str, "Cool Edit / Audition Filter Version 5.0\n" "WavPack Library Version %s\n"
                        "Copyright (c) 2026 David Bryant", WavpackGetLibraryVersionString());
#endif
                    MessageBox (hDlg, str, "About WavPack Filter", MB_OK);
                    break;
            }

            break;
    }

    return FALSE;
}


//////////////////////////////////////////////////////////////////////////////
// Function to determine whether a given bitrate is a "standard".           //
//////////////////////////////////////////////////////////////////////////////

static int std_bitrate (int bitrate)
{
    int sd, sc = 0;

    for (sd = bitrate; sd > 7; sd >>= 1)
        sc++;

    return bitrate == sd << sc;
}


//////////////////////////////////////////////////////////////////////////////
// Return a "raw" configuration value for the open file. Unfortunately,     //
// zero would be a valid value but we can't return that so we have to fudge //
// that case. Having the CONFIG_UPDATED bit clear indicates that this has   //
// not passed through a config dialog and is NOT suitable for writing.      //
//////////////////////////////////////////////////////////////////////////////

DWORD PASCAL FilterOptions (HANDLE hInput)
{
    WavpackInput *in = hInput;
    DWORD dwOptions = 0;

    if (in && in->wpc) {
        WavpackContext *wpc = in->wpc;
        int bps = (int) floor ((WavpackGetAverageBitrate (wpc, FALSE) / WavpackGetSampleRate (wpc) /
            WavpackGetNumChannels (wpc)) * 128.0 + 0.5);
        int mode = WavpackGetMode (wpc);

        if ((mode & MODE_HYBRID) || !(mode & MODE_LOSSLESS))
            dwOptions = (bps << 20) | OPTIONS_HYBRID | 1;
        else
            dwOptions = 1;

        if (mode & MODE_WVC)
            dwOptions |= OPTIONS_WVC;

        if (mode & MODE_HIGH)
            dwOptions |= OPTIONS_HIGH;

        if (mode & MODE_VERY_HIGH)
            dwOptions |= OPTIONS_VERY_HIGH;

        if (mode & MODE_FAST)
            dwOptions |= OPTIONS_FAST;

        if (WavpackGetQualifyMode (wpc) & QMODE_DSD_AUDIO)
            dwOptions |= OPTIONS_DSD;
        else if (WavpackGetBitsPerSample (wpc) == 20)
            dwOptions |= OPTIONS_FLOAT20;
        else if (WavpackGetBitsPerSample (wpc) == 24)
            dwOptions |= OPTIONS_FLOAT24;

        if (!(mode & MODE_FLOAT) || WavpackGetFloatNormExp (wpc) == WAV_IEEE_FLOAT_NORMAL)
            dwOptions |= OPTIONS_NORMALIZE;
    }

    logprintf ("FilterOptions(): %s\n", options_string (dwOptions));
    return dwOptions;
}


//////////////////////////////////////////////////////////////////////////////
// Return a string for displaying the configuration of the open WavPack     //
// file. Note that we can display modes that we can no longer write like    //
// the old lossy mode. We return the "raw" configuration word.              //
//////////////////////////////////////////////////////////////////////////////

DWORD PASCAL FilterOptionsString (HANDLE hInput, LPSTR lpszString)
{
    WavpackInput *in = hInput;

    if (in && in->wpc && lpszString) {
        WavpackContext *wpc = in->wpc;
        int mode = WavpackGetMode (wpc);
        char *quality = "";

        if (mode & MODE_VERY_HIGH)
            quality = " Very High";
        else if (mode & MODE_HIGH)
            quality = " High";
        else if (mode & MODE_FAST)
            quality = " Fast";

        if (WavpackGetQualifyMode (wpc) & QMODE_DSD_AUDIO)
            sprintf (lpszString, "WavPack DSD%s Mode (%d kbps)\n", quality,
                (int) ((WavpackGetAverageBitrate (wpc, TRUE) + 500.0) / 1000.0));
        else
            sprintf (lpszString, "WavPack %d-bit %s%s%s Mode (%d kbps)\n",
                WavpackGetBitsPerSample (wpc), (mode & MODE_HYBRID) ? "Hybrid " : "",
                (mode & MODE_LOSSLESS) ? "Lossless" : "Lossy", quality,
                (int) ((WavpackGetAverageBitrate (wpc, TRUE) + 500.0) / 1000.0));

        logprintf ("FilterOptionsString(): %s", lpszString);
    }

    return 0;
}


//////////////////////////////////////////////////////////////////////////////
// Write special data into WavPack file. All special data types basically   //
// mirror the equivalent RIFF chunks, so all we really have to do is        //
// convert those that aren't exactly the same and then generate RIFF chucks //
// and write those to the file (because WavPack stores extra RIFF chunks    //
// verbatim). Because this is very CoolEdit specific, I will not comment it //
// in detail.                                                               //
//////////////////////////////////////////////////////////////////////////////

static DWORD write_special_riff_chunk (OUTPUT *out, LPCSTR szListType, ChunkHeader *ckHdr, char *pData);

DWORD PASCAL FilterWriteSpecialData (HANDLE hOutput, LPCSTR szListType,
    LPCSTR szType, char *pData, DWORD dwSize)
{
    ChunkHeader ChunkHeader;
    OUTPUT *out = hOutput;

    if (out) {
        if (!strncmp (szListType, "INFO", 4)) {

            memcpy (ChunkHeader.ckID, szType, 4);
            ChunkHeader.ckSize = dwSize;
            return write_special_riff_chunk (out, szListType, &ChunkHeader, pData);
        }
        else if (!strncmp (szListType, "adtl", 4)) {
            if (!strncmp (szType, "ltxt", 4)) {
                char *cp = pData, *ep = pData + dwSize;

                while (ep - cp > 4) {
                    DWORD length = * (DWORD *) cp;
                    char *pBuffer = malloc (length + 4);

                    memset (pBuffer, 0, length + 4);
                    memcpy (pBuffer, cp + 4, 12);

                    if (length > 16)
                        memcpy (pBuffer + 20, cp + 16, length - 16);

                    memcpy (ChunkHeader.ckID, szType, 4);
                    ChunkHeader.ckSize = length + 4;
                    write_special_riff_chunk (out, szListType, &ChunkHeader, pBuffer);
                    free (pBuffer);
                    cp += length;
                }
            }
            else if (!strncmp (szType, "labl", 4) || !strncmp (szType, "note", 4)) {
                char *cp = pData, *ep = pData + dwSize;

                while (ep - cp > 4) {
                    memcpy (ChunkHeader.ckID, szType, 4);
                    ChunkHeader.ckSize = strlen (cp + 4) + 5;
                    write_special_riff_chunk (out, szListType, &ChunkHeader, cp);
                    cp += ChunkHeader.ckSize;
                }
            }
            else
                return 0;
        }
        else if (!strncmp (szListType, "WAVE", 4)) {
            if (!strncmp (szType, "cue ", 4)) {
                DWORD num_cues = dwSize / 8, *dwPtr = (DWORD *) pData;
                struct cue_type *pCue;
                char *pBuffer;

                memcpy (ChunkHeader.ckID, szType, 4);
                ChunkHeader.ckSize = num_cues * sizeof (*pCue) + sizeof (DWORD);
                pBuffer = malloc (ChunkHeader.ckSize);
                * (DWORD*) pBuffer = num_cues;
                pCue = (struct cue_type *) (pBuffer + sizeof (DWORD));

                while (num_cues--) {
                    CLEAR (*pCue);
                    memcpy ((char *) &pCue->fccChunk, "data", 4);
                    pCue->dwName = *dwPtr++;
                    pCue->dwPosition = pCue->dwSampleOffset = *dwPtr++;
                    pCue++;
                }

                write_special_riff_chunk (out, szListType, &ChunkHeader, pBuffer);
                free (pBuffer);
                return 1;
            }
            else if (!strncmp (szType, "plst", 4)) {
                DWORD num_plays = dwSize / 16, *dwPtr = (DWORD *) pData;
                struct play_type *pPlay;
                char *pBuffer;

                memcpy (ChunkHeader.ckID, szType, 4);
                ChunkHeader.ckSize = num_plays * sizeof (*pPlay) + sizeof (num_plays);
                pBuffer = malloc (ChunkHeader.ckSize);
                * (DWORD*) pBuffer = num_plays;
                pPlay = (struct play_type *) (pBuffer + sizeof (DWORD));

                while (num_plays--) {
                    CLEAR (*pPlay);
                    pPlay->dwName = *dwPtr++;
                    pPlay->dwLength = *dwPtr++;
                    pPlay->dwLoops = *dwPtr++;
                    dwPtr++;    // I don't know what to do with "dwMode" (?)
                    pPlay++;
                }

                write_special_riff_chunk (out, szListType, &ChunkHeader, pBuffer);
                free (pBuffer);
                return 1;
            }

            memcpy (ChunkHeader.ckID, szType, 4);
            ChunkHeader.ckSize = dwSize;
            return write_special_riff_chunk (out, szListType, &ChunkHeader, pData);
        }
        else
            return 0;
    }

    return 0;
}


//////////////////////////////////////////////////////////////////////////////
// Write the specified RIFF chunk to the end of the WavPack file. The only  //
// tricky part is that chunks that are not top level types should be        //
// combined into LIST chunks. When we are done we must also update the RIFF //
// chunk header at the very beginning of the file.                          //
//////////////////////////////////////////////////////////////////////////////

static DWORD write_special_riff_chunk (OUTPUT *out, LPCSTR szListType, ChunkHeader *ckHdr, char *pData)
{
    WavpackContext *wpc = out->wpc;
    char *listdp;

    // if this is the first special RIFF chunk then we must flush out all the
    // audio data

    if (!out->special_bytes && !out->listdata)
        WavpackFlushSamples (wpc);

    if (strncmp (szListType, "WAVE", 4)) {
        if (out->listdata && strncmp (out->listhdr.formType, szListType, 4))
            dump_list_chunk (out);

        if (!out->listdata) {
            memcpy (out->listhdr.formType, szListType, 4);
            memcpy (out->listhdr.ckID, "LIST", 4);
            out->listhdr.ckSize = 4;
        }

        listdp = out->listdata = realloc (out->listdata, out->listhdr.ckSize + sizeof (ChunkHeader) + ((ckHdr->ckSize + 1) & ~1));
        memcpy (listdp += out->listhdr.ckSize - 4, ckHdr, sizeof (ChunkHeader));

        if (ckHdr->ckSize) {
            memcpy (listdp += sizeof (ChunkHeader), pData, ckHdr->ckSize);

            if (ckHdr->ckSize & 1)
                listdp [ckHdr->ckSize] = 0;
        }

        out->listhdr.ckSize += sizeof (ChunkHeader) + ((ckHdr->ckSize + 1) & ~1);
        return 1;
    }

    dump_list_chunk (out);
    WavpackAddWrapper (wpc, ckHdr, sizeof (ChunkHeader));

    if (ckHdr->ckSize) {
        WavpackAddWrapper (wpc, pData, ckHdr->ckSize);

        if (ckHdr->ckSize & 1)
            WavpackAddWrapper (wpc, "\0", 1);
    }

    out->special_bytes += sizeof (ChunkHeader) + ((ckHdr->ckSize + 1) & ~1);
    return 1;
}

static DWORD dump_list_chunk (OUTPUT *out)
{
    if (out->listdata) {
        WavpackAddWrapper (out->wpc, &out->listhdr, sizeof (out->listhdr));
        WavpackAddWrapper (out->wpc, out->listdata, out->listhdr.ckSize - 4);
        out->special_bytes += sizeof (out->listhdr) + out->listhdr.ckSize - 4;
        free (out->listdata);
        out->listdata = NULL;
    }

    return 1;
}

//////////////////////////////////////////////////////////////////////////////
// Read next special data from WavPack file. Special data types basically   //
// mirror the equivalent RIFF chunks, so all we really have to do is        //
// convert those that aren't exactly the same and then pass them back to    //
// CoolEdit. Because this is very CoolEdit specific, I will not comment it  //
// in detail.                                                               //
//////////////////////////////////////////////////////////////////////////////

DWORD PASCAL FilterGetNextSpecialData (HANDLE hInput, SPECIALDATA *psp)
{
    ChunkHeader ChunkHeader;
    WavpackInput *in = hInput;

    while (in && in->special_bytes) {
        char * pData;

        if (in->special_bytes < sizeof (ChunkHeader))
            return 0;

        memcpy (&ChunkHeader, in->special_data, sizeof (ChunkHeader));
        in->special_data += sizeof (ChunkHeader);
        in->special_bytes -= sizeof (ChunkHeader);

        if (!strncmp (ChunkHeader.ckID, "RIFF", 4)) {
            if (in->special_bytes < sizeof (in->listhdr.formType))
                return 0;

            in->special_bytes -= sizeof (in->listhdr.formType);
            in->special_data += sizeof (in->listhdr.formType);
            continue;
        }
        else if (!strncmp (ChunkHeader.ckID, "fmt ", 4)) {
            if (in->special_bytes < ((ChunkHeader.ckSize + 1) & ~1))
                return 0;

            in->special_data += (ChunkHeader.ckSize + 1) & ~1;
            in->special_bytes -= (ChunkHeader.ckSize + 1) & ~1;
            continue;
        }
        else if (!strncmp (ChunkHeader.ckID, "data", 4)) {
            continue;
        }
        else if (!strncmp (ChunkHeader.ckID, "LIST", 4)) {
            memcpy (&in->listhdr, &ChunkHeader, sizeof (ChunkHeader));

            if (in->special_bytes < sizeof (in->listhdr.formType))
                return 0;

            memcpy (&in->listhdr.formType, in->special_data, sizeof (in->listhdr.formType));
            in->special_bytes -= sizeof (in->listhdr.formType);
            in->special_data += sizeof (in->listhdr.formType);

            if (in->listhdr.ckSize >= sizeof (in->listhdr.formType))
                in->listhdr.ckSize -= sizeof (in->listhdr.formType);
            else
                in->listhdr.ckSize = 0;

            continue;
        }

        if (in->listhdr.ckSize) {
            if (in->listhdr.ckSize >= sizeof (ChunkHeader))
                in->listhdr.ckSize -= sizeof (ChunkHeader);
            else
                in->listhdr.ckSize = 0;

            memcpy (psp->szListType, in->listhdr.formType, 4);
            psp->szListType [4] = 0;
        }
        else
            strcpy (psp->szListType, "WAVE");

        memcpy (psp->szType, ChunkHeader.ckID, 4);
        psp->szType [4] = 0;
        psp->dwSize = ChunkHeader.ckSize;
        psp->dwExtra = 1;
        psp->hSpecialData = NULL;
        psp->hData = GlobalAlloc (GMEM_MOVEABLE | GMEM_ZEROINIT, psp->dwSize + 1);
        pData = (char *) GlobalLock (psp->hData);

        if (in->special_bytes < ((psp->dwSize + 1) & ~1)) {
            GlobalUnlock (pData);
            GlobalFree (psp->hData);
            return 0;
        }

        memcpy (pData, in->special_data, (psp->dwSize + 1) & ~1);
        in->special_data += (psp->dwSize + 1) & ~1;
        in->special_bytes -= (psp->dwSize + 1) & ~1;

        if (in->listhdr.ckSize) {
            if (in->listhdr.ckSize >= ((ChunkHeader.ckSize + 1) & ~1))
                in->listhdr.ckSize -= (ChunkHeader.ckSize + 1) & ~1;
            else
                in->listhdr.ckSize = 0;
        }

        if (!strncmp (ChunkHeader.ckID, "cue ", 4)) {
            int num_cues = psp->dwSize >= sizeof (DWORD) ? (psp->dwExtra = * (DWORD *) pData) : -1;
            struct cue_type *pCue = (struct cue_type *)(pData + sizeof (DWORD));
            HANDLE hxData;
            DWORD *pdwData;

            if (num_cues < 0 || (DWORD) num_cues > (psp->dwSize - sizeof (DWORD)) / sizeof (struct cue_type)) {
                GlobalUnlock (pData);
                GlobalFree (psp->hData);
                return 0;
            }

            hxData = GlobalAlloc (GMEM_MOVEABLE | GMEM_ZEROINIT, num_cues * 8);
            pdwData = (DWORD *) GlobalLock (hxData);

            while (num_cues--) {
                *pdwData++ = pCue->dwName;
                *pdwData++ = pCue->dwPosition;
                pCue++;
            }

            GlobalUnlock (pData);
            GlobalFree (psp->hData);
            GlobalUnlock (pdwData);
            psp->hData = hxData;
            return 1;
        }
        else if (!strncmp (ChunkHeader.ckID, "plst", 4)) {
            int num_plays = psp->dwSize >= sizeof (DWORD) ? (psp->dwExtra = * (DWORD *) pData) : -1;
            struct play_type *pCue = (struct play_type *)(pData + sizeof (DWORD));
            HANDLE hxData;
            DWORD *pdwData;

            if (num_plays < 0 || (DWORD) num_plays > (psp->dwSize - sizeof (DWORD)) / sizeof (struct play_type)) {
                GlobalUnlock (pData);
                GlobalFree (psp->hData);
                return 0;
            }

            hxData = GlobalAlloc (GMEM_MOVEABLE | GMEM_ZEROINIT, num_plays * 16);
            pdwData = (DWORD *) GlobalLock (hxData);

            while (num_plays--) {
                *pdwData++ = pCue->dwName;
                *pdwData++ = pCue->dwLength;
                *pdwData++ = pCue->dwLoops;
                *pdwData++ = 0;
                pCue++;
            }

            GlobalUnlock (pData);
            GlobalFree (psp->hData);
            GlobalUnlock (pdwData);
            psp->hData = hxData;
            return 1;
        }
        else if (!strncmp (ChunkHeader.ckID, "ltxt", 4)) {
            HANDLE hxData;
            char *pxData;

            if (psp->dwSize < 20) {
                GlobalUnlock (pData);
                GlobalFree (psp->hData);
                return 0;
            }

            hxData = GlobalAlloc (GMEM_MOVEABLE | GMEM_ZEROINIT, psp->dwSize - 4);
            pxData = GlobalLock (hxData);

            memset (pxData, 0, psp->dwSize - 4);
            * (DWORD *) pxData = psp->dwSize - 4;
            memcpy (pxData + 4, pData, 12);

            if (psp->dwSize > 20)
                memcpy (pxData + 16, pData + 20, psp->dwSize - 20);

            GlobalUnlock (pData);
            GlobalFree (psp->hData);
            GlobalUnlock (pxData);
            psp->hData = hxData;
            return 1;
        }

        GlobalUnlock (pData);
        return 1;
    }

    return 0;
}


//////////////////////////////////////////////////////////////////////////////
// Attempt to read special RIFF data from the open WavPack file. If the     //
// audio data was not completely read then we can't do this (cause we're    //
// lazy). Once we determine that extra RIFF chunks are available, we use    //
// FilterGetNextSpecialData() to read them and return them to CoolEdit.     //
//////////////////////////////////////////////////////////////////////////////

DWORD PASCAL FilterGetFirstSpecialData (HANDLE hInput, SPECIALDATA *psp)
{
    WavpackInput *in = hInput;

    if (in && in->wpc) {
        WavpackContext *wpc = in->wpc;

        WavpackSeekTrailingWrapper (wpc);
        in->special_bytes = WavpackGetWrapperBytes (wpc);
        in->special_data = WavpackGetWrapperData (wpc);

        if (in->special_bytes)
            return FilterGetNextSpecialData (hInput, psp);
    }

    return 0;
}
