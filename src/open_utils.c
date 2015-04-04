////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//              Copyright (c) 1998 - 2013 Conifer Software.               //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// open_utils.c

// This module provides all the code required to open an existing WavPack file
// for reading, either by filename or by using a reader callback mechanism. This
// includes the code required to find and parse WavPack blocks, process any
// included metadata, and queue up the bitstreams containing the encoded audio
// data. It does not the actual code to unpack audio data and this was done so
// that programs that just want to query WavPack files for information (like,
// for example, taggers) don't need to link in a lot of unnecessary code.

#include <stdlib.h>
#include <string.h>

#include "wavpack_local.h"

// This code provides an interface between the reader callback mechanism that
// WavPack uses internally and the standard fstream C library. This allows an
// "open" call for WavPack files that accepts a filename (which is located at
// the end of the #ifdef).

#ifndef NO_USE_FSTREAMS

#include <fcntl.h>
#include <sys/stat.h>

#if defined (WIN32) || defined (__OS2__)
#include <io.h>
#endif

#ifdef WIN32
#define fileno _fileno
#endif

static int32_t read_bytes (void *id, void *data, int32_t bcount)
{
    return (int32_t) fread (data, 1, bcount, (FILE*) id);
}

static uint32_t get_pos (void *id)
{
    return ftell ((FILE*) id);
}

static int set_pos_abs (void *id, uint32_t pos)
{
    return fseek (id, pos, SEEK_SET);
}

static int set_pos_rel (void *id, int32_t delta, int mode)
{
    return fseek (id, delta, mode);
}

static int push_back_byte (void *id, int c)
{
    return ungetc (c, id);
}

static uint32_t get_length (void *id)
{
    FILE *file = id;
    struct stat statbuf;

    if (!file || fstat (fileno (file), &statbuf) || !(statbuf.st_mode & S_IFREG))
        return 0;

    return statbuf.st_size;
}

static int can_seek (void *id)
{
    FILE *file = id;
    struct stat statbuf;

    return file && !fstat (fileno (file), &statbuf) && (statbuf.st_mode & S_IFREG);
}

static int32_t write_bytes (void *id, void *data, int32_t bcount)
{
    return (int32_t) fwrite (data, 1, bcount, (FILE*) id);
}

static WavpackStreamReader freader = {
    read_bytes, get_pos, set_pos_abs, set_pos_rel, push_back_byte, get_length, can_seek,
    write_bytes
};

// This function attempts to open the specified WavPack file for reading. If
// this fails for any reason then an appropriate message is copied to "error"
// (which must accept 80 characters) and NULL is returned, otherwise a
// pointer to a WavpackContext structure is returned (which is used to call
// all other functions in this module). A filename beginning with "-" is
// assumed to be stdin. The "flags" argument has the following bit mask
// values to specify details of the open operation:

// OPEN_WVC:  attempt to open/read "correction" file
// OPEN_TAGS:  attempt to read ID3v1 / APEv2 tags (requires seekable file)
// OPEN_WRAPPER:  make audio wrapper available (i.e. RIFF) to caller
// OPEN_2CH_MAX:  open only first stream of multichannel file (usually L/R)
// OPEN_NORMALIZE:  normalize floating point data to +/- 1.0 (w/ offset exp)
// OPEN_STREAMING:  blindly unpacks blocks w/o regard to header file position
// OPEN_EDIT_TAGS:  allow editing of tags (file must be writable)

// Version 4.2 of the WavPack library adds the OPEN_STREAMING flag. This is
// essentially a "raw" mode where the library will simply decode any blocks
// fed it through the reader callback, regardless of where those blocks came
// from in a stream. The only requirement is that complete WavPack blocks are
// fed to the decoder (and this may require multiple blocks in multichannel
// mode) and that complete blocks are decoded (even if all samples are not
// actually required). All the blocks must contain the same number of channels
// and bit resolution, and the correction data must be either present or not.
// All other parameters may change from block to block (like lossy/lossless).
// Obviously, in this mode any seeking must be performed by the application
// (and again, decoding must start at the beginning of the block containing
// the seek sample).

WavpackContext *WavpackOpenFileInput (const char *infilename, char *error, int flags, int norm_offset)
{
    char *file_mode = (flags & OPEN_EDIT_TAGS) ? "r+b" : "rb";
    FILE *wv_id, *wvc_id;
    WavpackContext *wpc;

    if (*infilename == '-') {
        wv_id = stdin;
#if defined(WIN32)
        _setmode (fileno (stdin), O_BINARY);
#endif
#if defined(__OS2__)
        setmode (fileno (stdin), O_BINARY);
#endif
    }
    else if ((wv_id = fopen (infilename, file_mode)) == NULL) {
        if (error) strcpy (error, (flags & OPEN_EDIT_TAGS) ? "can't open file for editing" : "can't open file");
        return NULL;
    }

    if (wv_id != stdin && (flags & OPEN_WVC)) {
        char *in2filename = malloc (strlen (infilename) + 10);

        strcpy (in2filename, infilename);
        strcat (in2filename, "c");
        wvc_id = fopen (in2filename, "rb");
        free (in2filename);
    }
    else
        wvc_id = NULL;

    wpc = WavpackOpenFileInputEx (&freader, wv_id, wvc_id, error, flags, norm_offset);

    if (!wpc) {
        if (wv_id)
            fclose (wv_id);

        if (wvc_id)
            fclose (wvc_id);
    }
    else
        wpc->close_files = TRUE;

    return wpc;
}

#endif

// This function is identical to WavpackOpenFileInput() except that instead
// of providing a filename to open, the caller provides a pointer to a set of
// reader callbacks and instances of up to two streams. The first of these
// streams is required and contains the regular WavPack data stream; the second
// contains the "correction" file if desired. Unlike the standard open
// function which handles the correction file transparently, in this case it
// is the responsibility of the caller to be aware of correction files.

static uint32_t seek_final_index (WavpackStreamReader *reader, void *id);

WavpackContext *WavpackOpenFileInputEx (WavpackStreamReader *reader, void *wv_id, void *wvc_id, char *error, int flags, int norm_offset)
{
    WavpackContext *wpc = malloc (sizeof (WavpackContext));
    WavpackStream *wps;
    int num_blocks = 0;
    unsigned char first_byte;
    uint32_t bcount;

    if (!wpc) {
        if (error) strcpy (error, "can't allocate memory");
        return NULL;
    }

    CLEAR (*wpc);
    wpc->wv_in = wv_id;
    wpc->wvc_in = wvc_id;
    wpc->reader = reader;
    wpc->total_samples = (uint32_t) -1;
    wpc->norm_offset = norm_offset;
    wpc->max_streams = OLD_MAX_STREAMS;     // use this until overwritten with actual number
    wpc->open_flags = flags;

    wpc->filelen = wpc->reader->get_length (wpc->wv_in);

#ifndef NO_TAGS
    if ((flags & (OPEN_TAGS | OPEN_EDIT_TAGS)) && wpc->reader->can_seek (wpc->wv_in)) {
        load_tag (wpc);
        wpc->reader->set_pos_abs (wpc->wv_in, 0);

        if ((flags & OPEN_EDIT_TAGS) && !editable_tag (&wpc->m_tag)) {
            if (error) strcpy (error, "can't edit tags located at the beginning of files!");
            return WavpackCloseFile (wpc);
        }
    }
#endif

#ifndef VER4_ONLY
    if (wpc->reader->read_bytes (wpc->wv_in, &first_byte, 1) != 1) {
        if (error) strcpy (error, "can't read all of WavPack file!");
        return WavpackCloseFile (wpc);
    }

    wpc->reader->push_back_byte (wpc->wv_in, first_byte);

    if (first_byte == 'R')
        return open_file3 (wpc, error);
#endif

    wpc->streams = malloc ((wpc->num_streams = 1) * sizeof (wpc->streams [0]));
    if (!wpc->streams) {
        if (error) strcpy (error, "can't allocate memory");
        return WavpackCloseFile (wpc);
    }

    wpc->streams [0] = wps = malloc (sizeof (WavpackStream));
    if (!wps) {
        if (error) strcpy (error, "can't allocate memory");
        return WavpackCloseFile (wpc);
    }
    CLEAR (*wps);

    while (!wps->wphdr.block_samples) {

        wpc->filepos = wpc->reader->get_pos (wpc->wv_in);
        bcount = read_next_header (wpc->reader, wpc->wv_in, &wps->wphdr);

        if (bcount == (uint32_t) -1 ||
            (!wps->wphdr.block_samples && num_blocks++ > 16)) {
                if (error) strcpy (error, "not compatible with this version of WavPack file!");
                return WavpackCloseFile (wpc);
        }

        wpc->filepos += bcount;
        wps->blockbuff = malloc (wps->wphdr.ckSize + 8);
        if (!wps->blockbuff) {
            if (error) strcpy (error, "can't allocate memory");
            return WavpackCloseFile (wpc);
        }
        memcpy (wps->blockbuff, &wps->wphdr, 32);

        if (wpc->reader->read_bytes (wpc->wv_in, wps->blockbuff + 32, wps->wphdr.ckSize - 24) != wps->wphdr.ckSize - 24) {
            if (error) strcpy (error, "can't read all of WavPack file!");
            return WavpackCloseFile (wpc);
        }

        wps->init_done = FALSE;

        if (wps->wphdr.block_samples && !(flags & OPEN_STREAMING)) {
            if (wps->wphdr.block_index || wps->wphdr.total_samples == (uint32_t) -1) {
                wpc->initial_index = wps->wphdr.block_index;
                wps->wphdr.block_index = 0;

                if (wpc->reader->can_seek (wpc->wv_in)) {
                    uint32_t pos_save = wpc->reader->get_pos (wpc->wv_in);
                    uint32_t final_index = seek_final_index (wpc->reader, wpc->wv_in);

                    if (final_index != (uint32_t) -1)
                        wpc->total_samples = final_index - wpc->initial_index;

                    wpc->reader->set_pos_abs (wpc->wv_in, pos_save);
                }
            }
            else
                wpc->total_samples = wps->wphdr.total_samples;
        }

        if (wpc->wvc_in && wps->wphdr.block_samples && (wps->wphdr.flags & HYBRID_FLAG)) {
            wpc->file2len = wpc->reader->get_length (wpc->wvc_in);
            wpc->wvc_flag = TRUE;
        }

        if (wpc->wvc_flag && !read_wvc_block (wpc)) {
            if (error) strcpy (error, "not compatible with this version of correction file!");
            return WavpackCloseFile (wpc);
        }

        if (!wps->init_done && !unpack_init (wpc)) {
            if (error) strcpy (error, wpc->error_message [0] ? wpc->error_message :
                "not compatible with this version of WavPack file!");

            return WavpackCloseFile (wpc);
        }

        wps->init_done = TRUE;
    }

    wpc->config.flags &= ~0xff;
    wpc->config.flags |= wps->wphdr.flags & 0xff;
    wpc->config.bytes_per_sample = (wps->wphdr.flags & BYTES_STORED) + 1;
    wpc->config.float_norm_exp = wps->float_norm_exp;

    wpc->config.bits_per_sample = (wpc->config.bytes_per_sample * 8) -
        ((wps->wphdr.flags & SHIFT_MASK) >> SHIFT_LSB);

    if (!wpc->config.sample_rate) {
        if (!wps->wphdr.block_samples || (wps->wphdr.flags & SRATE_MASK) == SRATE_MASK)
            wpc->config.sample_rate = 44100;
        else
            wpc->config.sample_rate = sample_rates [(wps->wphdr.flags & SRATE_MASK) >> SRATE_LSB];
    }

    if (!wpc->config.num_channels) {
        wpc->config.num_channels = (wps->wphdr.flags & MONO_FLAG) ? 1 : 2;
        wpc->config.channel_mask = 0x5 - wpc->config.num_channels;
    }

    if ((flags & OPEN_2CH_MAX) && !(wps->wphdr.flags & FINAL_BLOCK))
        wpc->reduced_channels = (wps->wphdr.flags & MONO_FLAG) ? 1 : 2;

    return wpc;
}

// This function returns the major version number of the WavPack program
// (or library) that created the open file. Currently, this can be 1 to 4.
// Minor versions are not recorded in WavPack files.

int WavpackGetVersion (WavpackContext *wpc)
{
    if (wpc) {
#ifndef VER4_ONLY
        if (wpc->stream3)
            return get_version3 (wpc);
#endif
        return 4;
    }

    return 0;
}

// This function is used to seek to end of a file to determine its actual
// length in samples by reading the last header block containing data.
// Currently, all WavPack files contain the sample length in the first block
// containing samples, however this might not always be the case. Obviously,
// this function requires a seekable file or stream and leaves the file
// pointer undefined. A return value of -1 indicates the length could not
// be determined.

static uint32_t seek_final_index (WavpackStreamReader *reader, void *id)
{
    uint32_t result = (uint32_t) -1, bcount;
    WavpackHeader wphdr;
    unsigned char *tempbuff;

    if (reader->get_length (id) > 1200000L)
        reader->set_pos_rel (id, -1048576L, SEEK_END);
    else
        reader->set_pos_abs (id, 0);

    while (1) {
        bcount = read_next_header (reader, id, &wphdr);

        if (bcount == (uint32_t) -1)
            return result;

        tempbuff = malloc (wphdr.ckSize + 8);
        if (!tempbuff)
            return result;
        memcpy (tempbuff, &wphdr, 32);

        if (reader->read_bytes (id, tempbuff + 32, wphdr.ckSize - 24) != wphdr.ckSize - 24) {
            free (tempbuff);
            return result;
        }

        free (tempbuff);

        if (wphdr.block_samples && (wphdr.flags & FINAL_BLOCK))
            result = wphdr.block_index + wphdr.block_samples;
    }
}

// This function initializes everything required to unpack a WavPack block
// and must be called before unpack_samples() is called to obtain audio data.
// It is assumed that the WavpackHeader has been read into the wps->wphdr
// (in the current WavpackStream) and that the entire block has been read at
// wps->blockbuff. If a correction file is available (wpc->wvc_flag = TRUE)
// then the corresponding correction block must be read into wps->block2buff
// and its WavpackHeader has overwritten the header at wps->wphdr. This is
// where all the metadata blocks are scanned including those that contain
// bitstream data.

static int read_metadata_buff (WavpackMetadata *wpmd, unsigned char *blockbuff, unsigned char **buffptr);
static int process_metadata (WavpackContext *wpc, WavpackMetadata *wpmd);
static void bs_open_read (Bitstream *bs, void *buffer_start, void *buffer_end);

int unpack_init (WavpackContext *wpc)
{
    WavpackStream *wps = wpc->streams [wpc->current_stream];
    unsigned char *blockptr, *block2ptr;
    WavpackMetadata wpmd;

    wps->mute_error = FALSE;
    wps->crc = wps->crc_x = 0xffffffff;
    CLEAR (wps->wvbits);
    CLEAR (wps->wvcbits);
    CLEAR (wps->wvxbits);
    CLEAR (wps->decorr_passes);
    CLEAR (wps->dc);
    CLEAR (wps->w);

    if (!(wps->wphdr.flags & MONO_FLAG) && wpc->config.num_channels && wps->wphdr.block_samples &&
        (wpc->reduced_channels == 1 || wpc->config.num_channels == 1)) {
            wps->mute_error = TRUE;
            return FALSE;
    }

    if ((wps->wphdr.flags & UNKNOWN_FLAGS) || (wps->wphdr.flags & MONO_DATA) == MONO_DATA) {
        wps->mute_error = TRUE;
        return FALSE;
    }

    blockptr = wps->blockbuff + sizeof (WavpackHeader);

    while (read_metadata_buff (&wpmd, wps->blockbuff, &blockptr))
        if (!process_metadata (wpc, &wpmd)) {
            wps->mute_error = TRUE;
            return FALSE;
        }

    if (wps->wphdr.block_samples && wpc->wvc_flag && wps->block2buff) {
        block2ptr = wps->block2buff + sizeof (WavpackHeader);

        while (read_metadata_buff (&wpmd, wps->block2buff, &block2ptr))
            if (!process_metadata (wpc, &wpmd)) {
                wps->mute_error = TRUE;
                return FALSE;
            }
    }

    if (wps->wphdr.block_samples && !bs_is_open (&wps->wvbits)) {
        if (bs_is_open (&wps->wvcbits))
            strcpy (wpc->error_message, "can't unpack correction files alone!");

        wps->mute_error = TRUE;
        return FALSE;
    }

    if (wps->wphdr.block_samples && !bs_is_open (&wps->wvxbits)) {
        if ((wps->wphdr.flags & INT32_DATA) && wps->int32_sent_bits)
            wpc->lossy_blocks = TRUE;

        if ((wps->wphdr.flags & FLOAT_DATA) &&
            wps->float_flags & (FLOAT_EXCEPTIONS | FLOAT_ZEROS_SENT | FLOAT_SHIFT_SENT | FLOAT_SHIFT_SAME))
                wpc->lossy_blocks = TRUE;
    }

    if (wps->wphdr.block_samples)
        wps->sample_index = wps->wphdr.block_index;

    return TRUE;
}

//////////////////////////////// matadata handlers ///////////////////////////////

// These functions handle specific metadata types and are called directly
// during WavPack block parsing by process_metadata() at the bottom.

// This function initialzes the main bitstream for audio samples, which must
// be in the "wv" file.

static int init_wv_bitstream (WavpackStream *wps, WavpackMetadata *wpmd)
{
    if (!wpmd->byte_length)
        return FALSE;

    bs_open_read (&wps->wvbits, wpmd->data, (unsigned char *) wpmd->data + wpmd->byte_length);
    return TRUE;
}

// This function initialzes the "correction" bitstream for audio samples,
// which currently must be in the "wvc" file.

static int init_wvc_bitstream (WavpackStream *wps, WavpackMetadata *wpmd)
{
    if (!wpmd->byte_length)
        return FALSE;

    bs_open_read (&wps->wvcbits, wpmd->data, (unsigned char *) wpmd->data + wpmd->byte_length);
    return TRUE;
}

// This function initialzes the "extra" bitstream for audio samples which
// contains the information required to losslessly decompress 32-bit float data
// or integer data that exceeds 24 bits. This bitstream is in the "wv" file
// for pure lossless data or the "wvc" file for hybrid lossless. This data
// would not be used for hybrid lossy mode. There is also a 32-bit CRC stored
// in the first 4 bytes of these blocks.

static int init_wvx_bitstream (WavpackStream *wps, WavpackMetadata *wpmd)
{
    unsigned char *cp = wpmd->data;

    if (wpmd->byte_length <= 4)
        return FALSE;

    wps->crc_wvx = *cp++;
    wps->crc_wvx |= (int32_t) *cp++ << 8;
    wps->crc_wvx |= (int32_t) *cp++ << 16;
    wps->crc_wvx |= (int32_t) *cp++ << 24;

    bs_open_read (&wps->wvxbits, cp, (unsigned char *) wpmd->data + wpmd->byte_length);
    return TRUE;
}

// Read the int32 data from the specified metadata into the specified stream.
// This data is used for integer data that has more than 24 bits of magnitude
// or, in some cases, used to eliminate redundant bits from any audio stream.

static int read_int32_info (WavpackStream *wps, WavpackMetadata *wpmd)
{
    int bytecnt = wpmd->byte_length;
    char *byteptr = wpmd->data;

    if (bytecnt != 4)
        return FALSE;

    wps->int32_sent_bits = *byteptr++;
    wps->int32_zeros = *byteptr++;
    wps->int32_ones = *byteptr++;
    wps->int32_dups = *byteptr;

    return TRUE;
}

static int read_float_info (WavpackStream *wps, WavpackMetadata *wpmd)
{
    int bytecnt = wpmd->byte_length;
    char *byteptr = wpmd->data;

    if (bytecnt != 4)
        return FALSE;

    wps->float_flags = *byteptr++;
    wps->float_shift = *byteptr++;
    wps->float_max_exp = *byteptr++;
    wps->float_norm_exp = *byteptr;
    return TRUE;
}

// Read multichannel information from metadata. The first byte is the total
// number of channels and the following bytes represent the channel_mask
// as described for Microsoft WAVEFORMATEX.

static int read_channel_info (WavpackContext *wpc, WavpackMetadata *wpmd)
{
    int bytecnt = wpmd->byte_length, shift = 0;
    unsigned char *byteptr = wpmd->data;
    uint32_t mask = 0;

    if (!bytecnt || bytecnt > 6)
        return FALSE;

    if (!wpc->config.num_channels) {

        if (bytecnt == 6) {
            wpc->config.num_channels = (byteptr [0] | ((byteptr [2] & 0xf) << 8)) + 1;
            wpc->max_streams = (byteptr [1] | ((byteptr [2] & 0xf0) << 4)) + 1;

            if (wpc->config.num_channels < wpc->max_streams)
                return FALSE;
    
            byteptr += 3;
            mask = *byteptr++;
            mask |= (uint32_t) *byteptr++ << 8;
            mask |= (uint32_t) *byteptr << 16;
        }
        else {
            wpc->config.num_channels = *byteptr++;

            while (--bytecnt) {
                mask |= (uint32_t) *byteptr++ << shift;
                shift += 8;
            }
        }

        if (wpc->config.num_channels > wpc->max_streams * 2)
            return FALSE;

        wpc->config.channel_mask = mask;
    }

    return TRUE;
}

// Read configuration information from metadata.

static int read_config_info (WavpackContext *wpc, WavpackMetadata *wpmd)
{
    int bytecnt = wpmd->byte_length;
    unsigned char *byteptr = wpmd->data;

    if (bytecnt >= 3) {
        wpc->config.flags &= 0xff;
        wpc->config.flags |= (int32_t) *byteptr++ << 8;
        wpc->config.flags |= (int32_t) *byteptr++ << 16;
        wpc->config.flags |= (int32_t) *byteptr++ << 24;

        if (bytecnt >= 4 && (wpc->config.flags & CONFIG_EXTRA_MODE))
            wpc->config.xmode = *byteptr;
    }

    return TRUE;
}

// Read non-standard sampling rate from metadata.

static int read_sample_rate (WavpackContext *wpc, WavpackMetadata *wpmd)
{
    int bytecnt = wpmd->byte_length;
    unsigned char *byteptr = wpmd->data;

    if (bytecnt == 3) {
        wpc->config.sample_rate = (int32_t) *byteptr++;
        wpc->config.sample_rate |= (int32_t) *byteptr++ << 8;
        wpc->config.sample_rate |= (int32_t) *byteptr++ << 16;
    }

    return TRUE;
}

// Read wrapper data from metadata. Currently, this consists of the RIFF
// header and trailer that wav files contain around the audio data but could
// be used for other formats as well. Because WavPack files contain all the
// information required for decoding and playback, this data can probably
// be ignored except when an exact wavefile restoration is needed.

static int read_wrapper_data (WavpackContext *wpc, WavpackMetadata *wpmd)
{
    if ((wpc->open_flags & OPEN_WRAPPER) && wpc->wrapper_bytes < MAX_WRAPPER_BYTES && wpmd->byte_length) {
        wpc->wrapper_data = realloc (wpc->wrapper_data, wpc->wrapper_bytes + wpmd->byte_length);
	if (!wpc->wrapper_data)
	    return FALSE;
        memcpy (wpc->wrapper_data + wpc->wrapper_bytes, wpmd->data, wpmd->byte_length);
        wpc->wrapper_bytes += wpmd->byte_length;
    }

    return TRUE;
}

static int read_metadata_buff (WavpackMetadata *wpmd, unsigned char *blockbuff, unsigned char **buffptr)
{
    WavpackHeader *wphdr = (WavpackHeader *) blockbuff;
    unsigned char *buffend = blockbuff + wphdr->ckSize + 8;

    if (buffend - *buffptr < 2)
        return FALSE;

    wpmd->id = *(*buffptr)++;
    wpmd->byte_length = *(*buffptr)++ << 1;

    if (wpmd->id & ID_LARGE) {
        wpmd->id &= ~ID_LARGE;

        if (buffend - *buffptr < 2)
            return FALSE;

        wpmd->byte_length += *(*buffptr)++ << 9;
        wpmd->byte_length += *(*buffptr)++ << 17;
    }

    if (wpmd->id & ID_ODD_SIZE) {
        if (!wpmd->byte_length)         // odd size and zero length makes no sense
            return FALSE;
        wpmd->id &= ~ID_ODD_SIZE;
        wpmd->byte_length--;
    }

    if (wpmd->byte_length) {
        if (buffend - *buffptr < wpmd->byte_length + (wpmd->byte_length & 1)) {
            wpmd->data = NULL;
            return FALSE;
        }

        wpmd->data = *buffptr;
        (*buffptr) += wpmd->byte_length + (wpmd->byte_length & 1);
    }
    else
        wpmd->data = NULL;

    return TRUE;
}

static int process_metadata (WavpackContext *wpc, WavpackMetadata *wpmd)
{
    WavpackStream *wps = wpc->streams [wpc->current_stream];

    switch (wpmd->id) {
        case ID_DUMMY:
            return TRUE;

        case ID_DECORR_TERMS:
            return read_decorr_terms (wps, wpmd);

        case ID_DECORR_WEIGHTS:
            return read_decorr_weights (wps, wpmd);

        case ID_DECORR_SAMPLES:
            return read_decorr_samples (wps, wpmd);

        case ID_ENTROPY_VARS:
            return read_entropy_vars (wps, wpmd);

        case ID_HYBRID_PROFILE:
            return read_hybrid_profile (wps, wpmd);

        case ID_SHAPING_WEIGHTS:
            return read_shaping_info (wps, wpmd);

        case ID_FLOAT_INFO:
            return read_float_info (wps, wpmd);

        case ID_INT32_INFO:
            return read_int32_info (wps, wpmd);

        case ID_CHANNEL_INFO:
            return read_channel_info (wpc, wpmd);

        case ID_CONFIG_BLOCK:
            return read_config_info (wpc, wpmd);

        case ID_SAMPLE_RATE:
            return read_sample_rate (wpc, wpmd);

        case ID_WV_BITSTREAM:
            return init_wv_bitstream (wps, wpmd);

        case ID_WVC_BITSTREAM:
            return init_wvc_bitstream (wps, wpmd);

        case ID_WVX_BITSTREAM:
            return init_wvx_bitstream (wps, wpmd);

        case ID_RIFF_HEADER: case ID_RIFF_TRAILER:
            return read_wrapper_data (wpc, wpmd);

        case ID_MD5_CHECKSUM:
            if (wpmd->byte_length == 16) {
                memcpy (wpc->config.md5_checksum, wpmd->data, 16);
                wpc->config.flags |= CONFIG_MD5_CHECKSUM;
                wpc->config.md5_read = 1;
            }

            return TRUE;

        default:
            return (wpmd->id & ID_OPTIONAL_DATA) ? TRUE : FALSE;
    }
}

//////////////////////////////// bitstream management ///////////////////////////////

// Open the specified BitStream and associate with the specified buffer.

static void bs_read (Bitstream *bs);

static void bs_open_read (Bitstream *bs, void *buffer_start, void *buffer_end)
{
    bs->error = bs->sr = bs->bc = 0;
    bs->ptr = (bs->buf = buffer_start) - 1;
    bs->end = buffer_end;
    bs->wrap = bs_read;
}

// This function is only called from the getbit() and getbits() macros when
// the BitStream has been exhausted and more data is required. Sinve these
// bistreams no longer access files, this function simple sets an error and
// resets the buffer.

static void bs_read (Bitstream *bs)
{
    bs->ptr = bs->buf - 1;
    bs->error = 1;
}

// This function is called to close the bitstream. It returns the number of
// full bytes actually read as bits.

uint32_t bs_close_read (Bitstream *bs)
{
    uint32_t bytes_read;

    if (bs->bc < sizeof (*(bs->ptr)) * 8)
        bs->ptr++;

    bytes_read = (uint32_t)(bs->ptr - bs->buf) * sizeof (*(bs->ptr));

    if (!(bytes_read & 1))
        ++bytes_read;

    CLEAR (*bs);
    return bytes_read;
}

// Normally the trailing wrapper will not be available when a WavPack file is first
// opened for reading because it is stored in the final block of the file. This
// function forces a seek to the end of the file to pick up any trailing wrapper
// stored there (then use WavPackGetWrapper**() to obtain). This can obviously only
// be used for seekable files (not pipes) and is not available for pre-4.0 WavPack
// files.

static int seek_riff_trailer (WavpackContext *wpc);

void WavpackSeekTrailingWrapper (WavpackContext *wpc)
{
    if ((wpc->open_flags & OPEN_WRAPPER) &&
        wpc->reader->can_seek (wpc->wv_in) && !wpc->stream3) {
            uint32_t pos_save = wpc->reader->get_pos (wpc->wv_in);

            seek_riff_trailer (wpc);
            wpc->reader->set_pos_abs (wpc->wv_in, pos_save);
    }
}

// Get any MD5 checksum stored in the metadata (should be called after reading
// last sample or an extra seek will occur). A return value of FALSE indicates
// that no MD5 checksum was stored.

static int seek_md5 (WavpackStreamReader *reader, void *id, unsigned char data [16]);

int WavpackGetMD5Sum (WavpackContext *wpc, unsigned char data [16])
{
    if (wpc->config.flags & CONFIG_MD5_CHECKSUM) {
        if (wpc->config.md5_read) {
            memcpy (data, wpc->config.md5_checksum, 16);
            return TRUE;
        }
        else if (wpc->reader->can_seek (wpc->wv_in)) {
            uint32_t pos_save = wpc->reader->get_pos (wpc->wv_in);

            wpc->config.md5_read = seek_md5 (wpc->reader, wpc->wv_in, wpc->config.md5_checksum);
            wpc->reader->set_pos_abs (wpc->wv_in, pos_save);

            if (wpc->config.md5_read) {
                memcpy (data, wpc->config.md5_checksum, 16);
                return TRUE;
            }
            else
                return FALSE;
        }
    }

    return FALSE;
}

// Read from current file position until a valid 32-byte WavPack 4.0 header is
// found and read into the specified pointer. The number of bytes skipped is
// returned. If no WavPack header is found within 1 meg, then a -1 is returned
// to indicate the error. No additional bytes are read past the header and it
// is returned in the processor's native endian mode. Seeking is not required.

uint32_t read_next_header (WavpackStreamReader *reader, void *id, WavpackHeader *wphdr)
{
    unsigned char buffer [sizeof (*wphdr)], *sp = buffer + sizeof (*wphdr), *ep = sp;
    uint32_t bytes_skipped = 0;
    int bleft;

    while (1) {
        if (sp < ep) {
            bleft = (int)(ep - sp);
            memmove (buffer, sp, bleft);
        }
        else
            bleft = 0;

        if (reader->read_bytes (id, buffer + bleft, sizeof (*wphdr) - bleft) != sizeof (*wphdr) - bleft)
            return -1;

        sp = buffer;

        if (*sp++ == 'w' && *sp == 'v' && *++sp == 'p' && *++sp == 'k' &&
            !(*++sp & 1) && sp [2] < 16 && !sp [3] && (sp [2] || sp [1] || *sp >= 24) && sp [5] == 4 &&
            sp [4] >= (MIN_STREAM_VERS & 0xff) && sp [4] <= (MAX_STREAM_VERS & 0xff) && sp [18] < 3 && !sp [19]) {
                memcpy (wphdr, buffer, sizeof (*wphdr));
                WavpackLittleEndianToNative (wphdr, WavpackHeaderFormat);
                return bytes_skipped;
            }

        while (sp < ep && *sp != 'w')
            sp++;

        if ((bytes_skipped += (uint32_t)(sp - buffer)) > 1024 * 1024)
            return -1;
    }
}

// Compare the regular wv file block header to a potential matching wvc
// file block header and return action code based on analysis:
//
//   0 = use wvc block (assuming rest of block is readable)
//   1 = bad match; try to read next wvc block
//  -1 = bad match; ignore wvc file for this block and backup fp (if
//       possible) and try to use this block next time

static int match_wvc_header (WavpackHeader *wv_hdr, WavpackHeader *wvc_hdr)
{
    if (wv_hdr->block_index == wvc_hdr->block_index &&
        wv_hdr->block_samples == wvc_hdr->block_samples) {
            int wvi = 0, wvci = 0;

            if (wv_hdr->flags == wvc_hdr->flags)
                return 0;

            if (wv_hdr->flags & INITIAL_BLOCK)
                wvi -= 1;

            if (wv_hdr->flags & FINAL_BLOCK)
                wvi += 1;

            if (wvc_hdr->flags & INITIAL_BLOCK)
                wvci -= 1;

            if (wvc_hdr->flags & FINAL_BLOCK)
                wvci += 1;

            return (wvci - wvi < 0) ? 1 : -1;
        }

    if ((int32_t)(wvc_hdr->block_index - wv_hdr->block_index) < 0)
        return 1;
    else
        return -1;
}

// Read the wvc block that matches the regular wv block that has been
// read for the current stream. If an exact match is not found then
// we either keep reading or back up and (possibly) use the block
// later. The skip_wvc flag is set if not matching wvc block is found
// so that we can still decode using only the lossy version (although
// we flag this as an error). A return of FALSE indicates a serious
// error (not just that we missed one wvc block).

int read_wvc_block (WavpackContext *wpc)
{
    WavpackStream *wps = wpc->streams [wpc->current_stream];
    uint32_t bcount, file2pos;
    WavpackHeader wphdr;
    int compare_result;

    while (1) {
        file2pos = wpc->reader->get_pos (wpc->wvc_in);
        bcount = read_next_header (wpc->reader, wpc->wvc_in, &wphdr);

        if (bcount == (uint32_t) -1) {
            wps->wvc_skip = TRUE;
            wpc->crc_errors++;
            return FALSE;
        }

        if (wpc->open_flags & OPEN_STREAMING)
            wphdr.block_index = wps->sample_index = 0;
        else
            wphdr.block_index -= wpc->initial_index;

        if (wphdr.flags & INITIAL_BLOCK)
            wpc->file2pos = file2pos + bcount;

        compare_result = match_wvc_header (&wps->wphdr, &wphdr);

        if (!compare_result) {
            wps->block2buff = malloc (wphdr.ckSize + 8);
	    if (!wps->block2buff)
	        return FALSE;
            memcpy (wps->block2buff, &wphdr, 32);

            if (wpc->reader->read_bytes (wpc->wvc_in, wps->block2buff + 32, wphdr.ckSize - 24) !=
                wphdr.ckSize - 24 || (wphdr.flags & UNKNOWN_FLAGS)) {
                    free (wps->block2buff);
                    wps->block2buff = NULL;
                    wps->wvc_skip = TRUE;
                    wpc->crc_errors++;
                    return FALSE;
            }

            wps->wvc_skip = FALSE;
            memcpy (&wps->wphdr, &wphdr, 32);
            return TRUE;
        }
        else if (compare_result == -1) {
            wps->wvc_skip = TRUE;
            wpc->reader->set_pos_rel (wpc->wvc_in, -32, SEEK_CUR);
            wpc->crc_errors++;
            return TRUE;
        }
    }
}

static int seek_md5 (WavpackStreamReader *reader, void *id, unsigned char data [16])
{
    unsigned char meta_id, c1, c2;
    uint32_t bcount, meta_bc;
    WavpackHeader wphdr;

    if (reader->get_length (id) > 1200000L)
        reader->set_pos_rel (id, -1048576L, SEEK_END);

    while (1) {
        bcount = read_next_header (reader, id, &wphdr);

        if (bcount == (uint32_t) -1)
            return FALSE;

        bcount = wphdr.ckSize - sizeof (WavpackHeader) + 8;

        while (bcount >= 2) {
            if (reader->read_bytes (id, &meta_id, 1) != 1 ||
                reader->read_bytes (id, &c1, 1) != 1)
                    return FALSE;

            meta_bc = c1 << 1;
            bcount -= 2;

            if (meta_id & ID_LARGE) {
                if (bcount < 2 || reader->read_bytes (id, &c1, 1) != 1 ||
                    reader->read_bytes (id, &c2, 1) != 1)
                        return FALSE;

                meta_bc += ((uint32_t) c1 << 9) + ((uint32_t) c2 << 17);
                bcount -= 2;
            }

            if (meta_id == ID_MD5_CHECKSUM)
                return (meta_bc == 16 && bcount >= 16 &&
                    reader->read_bytes (id, data, 16) == 16);

            reader->set_pos_rel (id, meta_bc, SEEK_CUR);
            bcount -= meta_bc;
        }
    }
}

static int seek_riff_trailer (WavpackContext *wpc)
{
    WavpackStreamReader *reader = wpc->reader;
    void *id = wpc->wv_in;
    unsigned char meta_id, c1, c2;
    uint32_t bcount, meta_bc;
    WavpackHeader wphdr;

    if (reader->get_length (id) > 1200000L)
        reader->set_pos_rel (id, -1048576L, SEEK_END);

    while (1) {
        bcount = read_next_header (reader, id, &wphdr);

        if (bcount == (uint32_t) -1)
            return TRUE;

        bcount = wphdr.ckSize - sizeof (WavpackHeader) + 8;

        while (bcount >= 2) {
            if (reader->read_bytes (id, &meta_id, 1) != 1 ||
                reader->read_bytes (id, &c1, 1) != 1)
                    return TRUE;

            meta_bc = c1 << 1;
            bcount -= 2;

            if (meta_id & ID_LARGE) {
                if (bcount < 2 || reader->read_bytes (id, &c1, 1) != 1 ||
                    reader->read_bytes (id, &c2, 1) != 1)
                        return TRUE;

                meta_bc += ((uint32_t) c1 << 9) + ((uint32_t) c2 << 17);
                bcount -= 2;
            }

            if ((meta_id & ID_UNIQUE) == ID_RIFF_TRAILER && meta_bc) {
                wpc->wrapper_data = realloc (wpc->wrapper_data, wpc->wrapper_bytes + meta_bc);
		if (!wpc->wrapper_data)
		    return FALSE;

                if (reader->read_bytes (id, wpc->wrapper_data + wpc->wrapper_bytes, meta_bc) == meta_bc)
                    wpc->wrapper_bytes += meta_bc;
                else
                    return TRUE;
            }
            else
                reader->set_pos_rel (id, meta_bc, SEEK_CUR);

            bcount -= meta_bc;
        }
    }
}
