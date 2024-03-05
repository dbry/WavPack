////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//              Copyright (c) 1998 - 2013 Conifer Software.               //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// pack_utils.c

// This module provides the high-level API for creating WavPack files from
// audio data. It manages the buffers used to deinterleave the data passed
// in from the application into the individual streams and it handles the
// generation of riff headers and the "fixup" on the first WavPack block
// header for the case where the number of samples was unknown (or wrong).
// The actual audio stream compression is handled in the pack.c module.

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "wavpack_local.h"

#ifdef ENABLE_THREADS
static void worker_threads_create (WavpackContext *wpc);
static void pack_samples_enqueue (WavpackStream *wps, int free_wps);
static int write_completed_blocks (WavpackContext *wpc, int write_all_blocks, int result);
static int worker_available (WavpackContext *wpc);
#endif

///////////////////////////// executable code ////////////////////////////////

// Open context for writing WavPack files. The returned context pointer is used
// in all following calls to the library. The "blockout" function will be used
// to store the actual completed WavPack blocks and will be called with the id
// pointers containing user defined data (one for the wv file and one for the
// wvc file). A return value of NULL indicates that memory could not be
// allocated for the context.

WavpackContext *WavpackOpenFileOutput (WavpackBlockOutput blockout, void *wv_id, void *wvc_id)
{
    WavpackContext *wpc = malloc (sizeof (WavpackContext));

    if (!wpc)
        return NULL;

    CLEAR (*wpc);
    wpc->total_samples = -1;
    wpc->stream_version = CUR_STREAM_VERS;
    wpc->blockout = blockout;
    wpc->wv_out = wv_id;
    wpc->wvc_out = wvc_id;
    return wpc;
}

static int add_to_metadata (WavpackContext *wpc, void *data, uint32_t bcount, unsigned char id);

// New for version 5.0, this function allows the application to store a file extension and a
// file_format identification. The extension would be used by the unpacker if the user had not
// specified the target filename, and specifically handles the case where the original file
// had the "wrong" extension for the file format (e.g., a Wave64 file having a "wav" extension)
// or an alternative (e.g., "bwf") or where the file format is not known. Specifying a file
// format besides the default WP_FORMAT_WAV will ensure that old decoders will not be able to
// see the non-wav wrapper provided with WavpackAddWrapper() (which they would end up putting
// on a file with a .wav extension).

void WavpackSetFileInformation (WavpackContext *wpc, char *file_extension, unsigned char file_format)
{
    if (file_extension && strlen (file_extension) < sizeof (wpc->file_extension)) {
        add_to_metadata (wpc, file_extension, (uint32_t) strlen (file_extension), ID_ALT_EXTENSION);
        strcpy (wpc->file_extension, file_extension);
    }

    wpc->file_format = file_format;
}

// Set configuration for writing WavPack files. This must be done before
// sending any actual samples, however it is okay to send wrapper or other
// metadata before calling this. The "config" structure contains the following
// required information:

// config->bytes_per_sample     see WavpackGetBytesPerSample() for info
// config->bits_per_sample      see WavpackGetBitsPerSample() for info
// config->channel_mask         Microsoft standard (mono = 4, stereo = 3)
// config->num_channels         self evident
// config->sample_rate          self evident

// In addition, the following fields and flags may be set:

// config->flags:
// --------------
// o CONFIG_HYBRID_FLAG         select hybrid mode (must set bitrate)
// o CONFIG_JOINT_STEREO        select joint stereo (must set override also)
// o CONFIG_JOINT_OVERRIDE      override default joint stereo selection
// o CONFIG_HYBRID_SHAPE        select hybrid noise shaping (set override &
//                                                      shaping_weight != 0.0)
// o CONFIG_SHAPE_OVERRIDE      override default hybrid noise shaping
//                               (set CONFIG_HYBRID_SHAPE and shaping_weight)
// o CONFIG_FAST_FLAG           "fast" compression mode
// o CONFIG_HIGH_FLAG           "high" compression mode
// o CONFIG_BITRATE_KBPS        hybrid bitrate is kbps, not bits / sample
// o CONFIG_CREATE_WVC          create correction file
// o CONFIG_OPTIMIZE_WVC        maximize bybrid compression (-cc option)
// o CONFIG_CALC_NOISE          calc noise in hybrid mode
// o CONFIG_EXTRA_MODE          extra processing mode (slow!)
// o CONFIG_SKIP_WVX            no wvx stream for floats & large ints
// o CONFIG_MD5_CHECKSUM        specify if you plan to store MD5 signature
// o CONFIG_CREATE_EXE          specify if you plan to prepend sfx module
// o CONFIG_OPTIMIZE_MONO       detect and optimize for mono files posing as
//                               stereo (uses a more recent stream format that
//                               is not compatible with decoders < 4.3)

// config->bitrate              hybrid bitrate in either bits/sample or kbps
// config->shaping_weight       hybrid noise shaping coefficient override
// config->block_samples        force samples per WavPack block (0 = use deflt)
// config->float_norm_exp       select floating-point data (127 for +/-1.0)
// config->xmode                extra mode processing value override
// config->worker_threads       enable multithreading by specifying number of
//                               worker threads (maximum 15), but be aware that
//                               this only benefits multichannel files

// If the number of samples to be written is known then it should be passed
// here. If the duration is not known then pass -1. In the case that the size
// is not known (or the writing is terminated early) then it is suggested that
// the application retrieve the first block written and let the library update
// the total samples indication. A function is provided to do this update and
// it should be done to the "correction" file also. If this cannot be done
// (because a pipe is being used, for instance) then a valid WavPack will still
// be created, but when applications want to access that file they will have
// to seek all the way to the end to determine the actual duration. Also, if
// a RIFF header has been included then it should be updated as well or the
// WavPack file will not be directly unpackable to a valid wav file (although
// it will still be usable by itself). A return of FALSE indicates an error.
//
// The enhanced version of this function now allows setting the identities of
// any channels that are NOT standard Microsoft channels and are therefore not
// represented in the channel mask. WavPack files require that all the Microsoft
// channels come first (and in Microsoft order) and these are followed by any
// other channels (which can be in any order).
//
// The identities are provided in a NULL-terminated string (0x00 is not an allowed
// channel ID). The Microsoft channels may be provided as well (and will be checked)
// but it is really only necessary to provide the "unknown" channels. Any truly
// unknown channels are indicated with a 0xFF.
//
// The channel IDs so far reserved are listed here:
//
// 0:           not allowed / terminator
// 1 - 18:      Microsoft standard channels
// 30, 31:      Stereo mix from RF64 (not really recommended, but RF64 specifies this)
// 33 - 44:     Core Audio channels (see Core Audio specification)
// 127 - 128:   Amio LeftHeight, Amio RightHeight
// 138 - 142:   Amio BottomFrontLeft/Center/Right, Amio ProximityLeft/Right
// 200 - 207:   Core Audio channels (see Core Audio specification)
// 221 - 224:   Core Audio channels 301 - 305 (offset by 80)
// 255:         Present but unknown or unused channel
//
// All other channel IDs are reserved. Ask if something you need is missing.

// Table of channels that will automatically "pair" into a single stereo stream

static const struct { unsigned char a, b; } stereo_pairs [] = {
    { 1, 2 },       // FL, FR
    { 5, 6 },       // BL, BR
    { 7, 8 },       // FLC, FRC
    { 10, 11 },     // SL, SR
    { 13, 15 },     // TFL, TFR
    { 16, 18 },     // TBL, TBR
    { 30, 31 },     // stereo mix L,R (RF64)
    { 33, 34 },     // Rls, Rrs
    { 35, 36 },     // Lw, Rw
    { 38, 39 },     // Lt, Rt
    { 127, 128 },   // Lh, Rh
    { 138, 140 },   // Bfl, Bfr
    { 141, 142 },   // Pl, Pr
    { 200, 201 },   // Amb_W, Amb_X
    { 202, 203 },   // Amb_Y, Amb_Z
    { 204, 205 },   // MS_Mid, MS_Side
    { 206, 207 },   // XY_X, XY_Y
    { 221, 222 },   // Hph_L, Hph_R
};

#define NUM_STEREO_PAIRS (sizeof (stereo_pairs) / sizeof (stereo_pairs [0]))

// Legacy version of this function for compatibility with existing applications. Note that this version
// also generates older streams to be compatible with all decoders back to 4.0, but of course cannot be
// used with > 2^32 samples or non-Microsoft channels. The older stream version only differs in that it
// does not support the "mono optimization" feature where stereo blocks containing identical audio data
// in both channels are encoded in mono for better efficiency.

int WavpackSetConfiguration (WavpackContext *wpc, WavpackConfig *config, uint32_t total_samples)
{
    config->flags |= CONFIG_COMPATIBLE_WRITE;       // write earlier version streams

    if (total_samples == (uint32_t) -1)
        return WavpackSetConfiguration64 (wpc, config, -1, NULL);
    else
        return WavpackSetConfiguration64 (wpc, config, total_samples, NULL);
}

int WavpackSetConfiguration64 (WavpackContext *wpc, WavpackConfig *config, int64_t total_samples, const unsigned char *chan_ids)
{
    uint32_t flags, bps = 0;
    uint32_t chan_mask = config->channel_mask;
    int num_chans = config->num_channels;
    int stream_index, i;

    if (config->sample_rate <= 0) {
        strcpy (wpc->error_message, "sample rate cannot be zero or negative!");
        return FALSE;
    }

    if (!total_samples || total_samples > MAX_WAVPACK_SAMPLES || total_samples < -1) {
        strcpy (wpc->error_message, "invalid total sample count!");
        return FALSE;
    }

    if (num_chans <= 0 || num_chans > WAVPACK_MAX_CHANS) {
        strcpy (wpc->error_message, "invalid channel count!");
        return FALSE;
    }

    if (config->block_samples && (config->block_samples < 16 || config->block_samples > 131072)) {
        strcpy (wpc->error_message, "invalid custom block samples!");
        return FALSE;
    }

    wpc->stream_version = (config->flags & CONFIG_COMPATIBLE_WRITE) ? CUR_STREAM_VERS : MAX_STREAM_VERS;

    if ((config->qmode & QMODE_DSD_AUDIO) && config->bytes_per_sample == 1 && config->bits_per_sample == 8) {
#ifdef ENABLE_DSD
        wpc->dsd_multiplier = 1;
        flags = DSD_FLAG;

        for (i = 14; i >= 0; --i)
            if (config->sample_rate % sample_rates [i] == 0) {
                int divisor = config->sample_rate / sample_rates [i];

                if (divisor && (divisor & (divisor - 1)) == 0) {
                    config->sample_rate /= divisor;
                    wpc->dsd_multiplier = divisor;
                    break;
                }
            }

        // most options that don't apply to DSD we can simply ignore for now, but NOT hybrid mode!
        if (config->flags & CONFIG_HYBRID_FLAG) {
            strcpy (wpc->error_message, "hybrid mode not available for DSD!");
            return FALSE;
        }

        // with DSD, very few PCM options work (or make sense), so only allow those that do
        config->flags &= (CONFIG_HIGH_FLAG | CONFIG_MD5_CHECKSUM | CONFIG_PAIR_UNDEF_CHANS);
        config->float_norm_exp = config->xmode = 0;
#else
        strcpy (wpc->error_message, "libwavpack not configured for DSD!");
        return FALSE;
#endif
    }
    else
        flags = config->bytes_per_sample - 1;

    wpc->total_samples = total_samples;
    wpc->config.sample_rate = config->sample_rate;
    wpc->config.num_channels = config->num_channels;
    wpc->config.channel_mask = config->channel_mask;
    wpc->config.bits_per_sample = config->bits_per_sample;
    wpc->config.bytes_per_sample = config->bytes_per_sample;
    wpc->config.worker_threads = config->worker_threads;
    wpc->config.block_samples = config->block_samples;
    wpc->config.flags = config->flags;
    wpc->config.qmode = config->qmode;

    if (config->flags & CONFIG_VERY_HIGH_FLAG)
        wpc->config.flags |= CONFIG_HIGH_FLAG;

    for (i = 0; i < 15; ++i)
        if (wpc->config.sample_rate == sample_rates [i])
            break;

    flags |= i << SRATE_LSB;

    // all of this stuff only applies to PCM

    if (!(flags & DSD_FLAG)) {
        if (config->float_norm_exp) {
            if (config->bytes_per_sample != 4 || config->bits_per_sample != 32) {
                strcpy (wpc->error_message, "incorrect bits/bytes configuration for float data!");
                return FALSE;
            }

            wpc->config.float_norm_exp = config->float_norm_exp;
            wpc->config.flags |= CONFIG_FLOAT_DATA;
            flags |= FLOAT_DATA;
        }
        else {
            if (config->bytes_per_sample < 1 || config->bytes_per_sample > 4) {
                strcpy (wpc->error_message, "invalid bytes per sample!");
                return FALSE;
            }

            if (config->bits_per_sample < 1 || config->bits_per_sample > config->bytes_per_sample * 8) {
                strcpy (wpc->error_message, "invalid bits per sample!");
                return FALSE;
            }

            flags |= ((config->bytes_per_sample * 8) - config->bits_per_sample) << SHIFT_LSB;
        }

        if (config->flags & CONFIG_HYBRID_FLAG) {
            flags |= HYBRID_FLAG | HYBRID_BITRATE | HYBRID_BALANCE;

            // the noise-shaping override is used to specify a fixed shaping value, or to select no shaping

            if (config->flags & CONFIG_SHAPE_OVERRIDE) {
                if ((config->flags & CONFIG_HYBRID_SHAPE) && config->shaping_weight) {
                    wpc->config.shaping_weight = config->shaping_weight;
                    flags |= HYBRID_SHAPE | NEW_SHAPING;
                }
                else
                    wpc->config.shaping_weight = 0.0;
            }
            else {
                wpc->config.flags |= 0x4000;    // FIXME: just make identical files

                if (!(config->flags & CONFIG_DYNAMIC_SHAPING)) {    // if nothing specified, use defaults
                    if (config->flags & CONFIG_OPTIMIZE_WVC)
                        wpc->config.shaping_weight = -0.5;
                    else if (config->sample_rate >= 64000)
                        wpc->config.shaping_weight = 1.0;
                    else
                        wpc->config.flags |= CONFIG_DYNAMIC_SHAPING;
                }

                flags |= HYBRID_SHAPE | NEW_SHAPING;
            }

            if (wpc->config.flags & (CONFIG_CROSS_DECORR | CONFIG_OPTIMIZE_WVC))
                flags |= CROSS_DECORR;

            if (config->flags & CONFIG_BITRATE_KBPS) {
                bps = (uint32_t) floor (config->bitrate * 256000.0 / config->sample_rate / config->num_channels + 0.5);

                if (bps > (64 << 8))
                    bps = 64 << 8;
            }
            else
                bps = (uint32_t) floor (config->bitrate * 256.0 + 0.5);
        }
        else
            flags |= CROSS_DECORR;

        if (!(config->flags & CONFIG_JOINT_OVERRIDE) || (config->flags & CONFIG_JOINT_STEREO))
            flags |= JOINT_STEREO;

        if (config->flags & CONFIG_CREATE_WVC)
            wpc->wvc_flag = TRUE;
    }

    // if a channel-identities string was specified, process that here, otherwise all channels
    // not present in the channel mask are considered "unassigned"

    if (chan_ids) {
        int lastchan = 0, mask_copy = chan_mask;

        if ((int) strlen ((char *) chan_ids) > num_chans) {          // can't be more than num channels!
            strcpy (wpc->error_message, "chan_ids longer than num channels!");
            return FALSE;
        }

        // skip past channels that are specified in the channel mask (no reason to store those)

        while (*chan_ids)
            if (*chan_ids <= 32 && *chan_ids > lastchan && (mask_copy & (1U << (*chan_ids-1)))) {
                mask_copy &= ~(1U << (*chan_ids-1));
                lastchan = *chan_ids++;
            }
            else
                break;

        // now scan the string for an actually defined channel (and don't store if there aren't any)

        for (i = 0; chan_ids [i]; i++)
            if (chan_ids [i] != 0xff) {
                wpc->channel_identities = (unsigned char *) strdup ((char *) chan_ids);
                break;
            }
    }

    // This loop goes through all the channels and creates the Wavpack "streams" for them to go in.
    // A stream can hold either one or two channels, so we have several rules to determine how many
    // channels will go in each stream.

    for (stream_index = 0; num_chans; stream_index++) {
        WavpackStream *wps = calloc (1, sizeof (WavpackStream));
        unsigned char left_chan_id = 0, right_chan_id = 0;
        int pos, chans = 1;

        // allocate the stream and initialize the pointer to it
        wpc->streams = realloc (wpc->streams, (stream_index + 1) * sizeof (wpc->streams [0]));
        wpc->streams [stream_index] = wps;
        wps->stream_index = stream_index;
        wps->wpc = wpc;

        // if there are any bits [still] set in the channel_mask, get the next one or two IDs from there
        if (chan_mask)
            for (pos = 0; pos < 32; ++pos)
                if (chan_mask & (1U << pos)) {
                    if (left_chan_id) {
                        right_chan_id = pos + 1;
                        break;
                    }
                    else {
                        chan_mask &= ~(1U << pos);
                        left_chan_id = pos + 1;
                    }
                }

        // next check for any channels identified in the channel-identities string
        while (!right_chan_id && chan_ids && *chan_ids)
            if (left_chan_id)
                right_chan_id = *chan_ids;
            else
                left_chan_id = *chan_ids++;

        // assume anything we did not get is "unassigned"
        if (!left_chan_id)
            left_chan_id = right_chan_id = 0xff;
        else if (!right_chan_id)
            right_chan_id = 0xff;

        // if we have 2 channels, this is where we decide if we can combine them into one stream:
        // 1. they are "unassigned" and we've been told to combine unassigned pairs, or
        // 2. they appear together in the valid "pairings" list
        if (num_chans >= 2) {
            if ((config->flags & CONFIG_PAIR_UNDEF_CHANS) && left_chan_id == 0xff && right_chan_id == 0xff)
                chans = 2;
            else
                for (i = 0; i < NUM_STEREO_PAIRS; ++i)
                    if ((left_chan_id == stereo_pairs [i].a && right_chan_id == stereo_pairs [i].b) ||
                        (left_chan_id == stereo_pairs [i].b && right_chan_id == stereo_pairs [i].a)) {
                            if (right_chan_id <= 32 && (chan_mask & (1U << (right_chan_id-1))))
                                chan_mask &= ~(1U << (right_chan_id-1));
                            else if (chan_ids && *chan_ids == right_chan_id)
                                chan_ids++;

                            chans = 2;
                            break;
                        }
        }

        num_chans -= chans;

        if (num_chans && stream_index == NEW_MAX_STREAMS - 1)
            break;

        memcpy (wps->wphdr.ckID, "wvpk", 4);
        wps->wphdr.ckSize = sizeof (WavpackHeader) - 8;
        SET_TOTAL_SAMPLES (wps->wphdr, wpc->total_samples);
        wps->wphdr.version = wpc->stream_version;
        wps->wphdr.flags = flags;
        wps->bits = bps;

        if (!stream_index)
            wps->wphdr.flags |= INITIAL_BLOCK;

        if (!num_chans)
            wps->wphdr.flags |= FINAL_BLOCK;

        if (chans == 1) {
            wps->wphdr.flags &= ~(JOINT_STEREO | CROSS_DECORR | HYBRID_BALANCE);
            wps->wphdr.flags |= MONO_FLAG;
        }
    }

    wpc->num_streams = stream_index;

    if (num_chans) {
        strcpy (wpc->error_message, "too many channels!");
        return FALSE;
    }

#ifdef SKIP_DECORRELATION
    wpc->config.xmode = 0;
#else
    if (config->flags & CONFIG_EXTRA_MODE)
        wpc->config.xmode = config->xmode ? config->xmode : 1;
    else
        wpc->config.xmode = 0;
#endif

    return TRUE;
}

// This function allows setting the Core Audio File channel layout, many of which do not
// conform to the Microsoft ordering standard that Wavpack requires internally (at least for
// those channels present in the "channel mask"). In addition to the layout tag, this function
// allows a reordering string to be stored in the file to allow the unpacker to reorder the
// channels back to the specified layout (if it is aware of this feature and wants to restore
// the CAF order). The number of channels in the layout is specified in the lower nybble of
// the layout word, and if a reorder string is specified it must be that long. Note that all
// the reordering is actually done outside of this library, and that if reordering is done
// then the appropriate qmode bit must be set to ensure that any MD5 sum is stored with a new
// ID so that old decoders don't try to verify it (and to let the decoder know that a reorder
// might be required).
//
// Note: This function should only be used to encode Core Audio files in such a way that a
// verbatim archive can be created. Applications can just include the chan_ids parameter in
// the call to WavpackSetConfiguration64() if there are non-Microsoft channels to specify,
// or do nothing special if only Microsoft channels are present (the vast majority of cases).

int WavpackSetChannelLayout (WavpackContext *wpc, uint32_t layout_tag, const unsigned char *reorder)
{
    int nchans = layout_tag & 0xff;

    if ((layout_tag & 0xff00ff00) || nchans > wpc->config.num_channels)
        return FALSE;

    wpc->channel_layout = layout_tag;

    if (wpc->channel_reordering) {
        free (wpc->channel_reordering);
        wpc->channel_reordering = NULL;
    }

    if (nchans && reorder) {
        int min_index = 256, i;

        for (i = 0; i < nchans; ++i)
            if (reorder [i] < min_index)
                min_index = reorder [i];

        wpc->channel_reordering = malloc (nchans);

        if (wpc->channel_reordering)
            for (i = 0; i < nchans; ++i)
                wpc->channel_reordering [i] = reorder [i] - min_index;
    }

    return TRUE;
}

// Prepare to actually pack samples by determining the size of the WavPack
// blocks and allocating sample buffers and initializing each stream. Call
// after WavpackSetConfiguration() and before WavpackPackSamples(). A return
// of FALSE indicates an error.

static int write_metadata_block (WavpackContext *wpc);

int WavpackPackInit (WavpackContext *wpc)
{
    int stream_index;

    if (wpc->metabytes > 16384)             // 16384 bytes still leaves plenty of room for audio
        write_metadata_block (wpc);         //  in this block (otherwise write a special one)

#ifdef ENABLE_THREADS
    // if multithreading is enabled and requested, configure and start the workers here

    if (wpc->config.worker_threads) {
        // for multichannel files, we use one less worker thread than there are streams
        if (wpc->num_streams > 1 && wpc->config.worker_threads > wpc->num_streams - 1)
            wpc->num_workers = wpc->num_streams - 1;
        else
            wpc->num_workers = wpc->config.worker_threads;

        if (wpc->num_workers > 15)
            wpc->num_workers = 15;

        // FIXME: There are some situations where the number of samples in a block is truncated during the packing
        // of the first stream. This can be either because of dynamic noise shaping used with correction files or with
        // the "merge blocks" feature. Since this would not work with multithreaded compression, we'll prohibit that
        // for now, but in the future a better solution would be to simply calculate the truncation before starting
        // the compression threads.

        if (!(wpc->streams [0]->wphdr.flags & FLOAT_DATA) && wpc->config.bytes_per_sample <= 3)
            if ((wpc->wvc_flag && (wpc->config.flags & CONFIG_DYNAMIC_SHAPING) && !wpc->config.block_samples) ||
                wpc->config.flags & CONFIG_MERGE_BLOCKS)
                    wpc->num_workers = 0;

        // Because of noise-shaping discontinuities between blocks and complications in measuring quantization noise,
        // we don't allow temporal multithreading in hybrid mode. In the future we might be able to loosen this up some.

        if (wpc->num_workers && wpc->num_streams == 1 && (wpc->streams [0]->wphdr.flags & HYBRID_FLAG))
            wpc->num_workers = 0;

        // DSD "high" mode performs much better with discontinuities if it has some "pre samples" to chew on first.
        // DSD "fast" mode doesn't care about discontinuities, and the PCM modes do a pack_init() to deal with them.

        if (wpc->num_workers && wpc->dsd_multiplier && wpc->num_streams == 1 && (wpc->config.flags & CONFIG_HIGH_FLAG))
            wpc->max_pre_samples = 512;

        if (wpc->num_workers)
            worker_threads_create (wpc);
    }
#endif

    // The default block size is a compromise. Longer blocks provide better encoding efficiency,
    // but longer blocks adversely affect memory requirements and seeking performance. For WavPack
    // version 5.0, the default block sizes have been reduced by half from the previous version,
    // but the difference in encoding efficiency will generally be less than 0.1 percent.

    if (wpc->dsd_multiplier) {
        wpc->block_samples = (wpc->config.sample_rate % 7) ? 48000 : 44100;

        if (wpc->config.flags & CONFIG_HIGH_FLAG)
            wpc->block_samples /= 2;

        if (wpc->config.num_channels == 1)
            wpc->block_samples *= 2;

        while (wpc->block_samples > 12000 && (int64_t) wpc->block_samples * wpc->config.num_channels > 300000)
            wpc->block_samples /= 2;
    }
    else {
        int divisor = (wpc->config.flags & CONFIG_HIGH_FLAG) ? 2 : 4;

        while (wpc->config.sample_rate % divisor)
            divisor--;

        wpc->block_samples = wpc->config.sample_rate / divisor;

        while (wpc->block_samples > 12000 && (int64_t) wpc->block_samples * wpc->config.num_channels > 75000)
            wpc->block_samples /= 2;

        while ((int64_t) wpc->block_samples * wpc->config.num_channels < 20000)
            wpc->block_samples *= 2;
    }

    if (wpc->config.block_samples) {
        if ((wpc->config.flags & CONFIG_MERGE_BLOCKS) &&
            wpc->block_samples > (uint32_t) wpc->config.block_samples) {
                wpc->block_boundary = wpc->config.block_samples;
                wpc->block_samples /= wpc->config.block_samples;
                wpc->block_samples *= wpc->config.block_samples;
        }
        else
            wpc->block_samples = wpc->config.block_samples;
    }

    wpc->ave_block_samples = wpc->block_samples;
    wpc->max_samples = wpc->block_samples + (wpc->block_samples >> 1);

    for (stream_index = 0; stream_index < wpc->num_streams; stream_index++) {
        WavpackStream *wps = wpc->streams [stream_index];

        wps->sample_buffer = malloc (wpc->max_samples * (wps->wphdr.flags & MONO_FLAG ? 4 : 8));

#ifdef ENABLE_DSD
        if (wps->wphdr.flags & DSD_FLAG)
            pack_dsd_init (wps);
        else
#endif
            pack_init (wps);
    }

    return TRUE;
}

// Pack the specified samples. Samples must be stored in longs in the native
// endian format of the executing processor. The number of samples specified
// indicates composite samples (sometimes called "frames"). So, the actual
// number of data points would be this "sample_count" times the number of
// channels. Note that samples are accumulated here until enough exist to
// create a complete WavPack block (or several blocks for multichannel audio).
// If an application wants to break a block at a specific sample, then it must
// simply call WavpackFlushSamples() to force an early termination. Completed
// WavPack blocks are send to the function provided in the initial call to
// WavpackOpenFileOutput(). A return of FALSE indicates an error.

static int pack_streams (WavpackContext *wpc, uint32_t block_samples, int last_block);
static int create_riff_header (WavpackContext *wpc, int64_t total_samples, void *outbuffer);

int WavpackPackSamples (WavpackContext *wpc, int32_t *sample_buffer, uint32_t sample_count)
{
    int nch = wpc->config.num_channels;

    while (sample_count) {
        int32_t *source_pointer = sample_buffer;
        unsigned int samples_to_copy;
        int stream_index;

        if (!wpc->riff_header_added && !wpc->riff_header_created && !wpc->file_format) {
            char riff_header [128];

            if (!add_to_metadata (wpc, riff_header, create_riff_header (wpc, wpc->total_samples, riff_header), ID_RIFF_HEADER))
                return FALSE;
        }

        if (wpc->acc_samples + sample_count > wpc->max_samples)
            samples_to_copy = wpc->max_samples - wpc->acc_samples;
        else
            samples_to_copy = sample_count;

        for (stream_index = 0; stream_index < wpc->num_streams; stream_index++) {
            WavpackStream *wps = wpc->streams [stream_index];
            int32_t *dptr, *sptr, cnt;

            dptr = wps->sample_buffer + wpc->acc_samples * (wps->wphdr.flags & MONO_FLAG ? 1 : 2);
            sptr = source_pointer;
            cnt = samples_to_copy;

            // This code used to just copy the 32-bit samples regardless of the actual size with the
            // assumption that the caller had properly sign-extended the values (if they were smaller
            // than 32 bits). However, several people have discovered that if the data isn't properly
            // sign extended then ugly things happen (e.g. CRC errors that show up only on decode).
            // To prevent this, we now explicitly sign-extend samples smaller than 32-bit when we
            // copy, and the performance hit from doing this is very small (generally < 1%).

            if (wps->wphdr.flags & MONO_FLAG) {
                switch (wpc->config.bytes_per_sample) {
                    case 1:
                        while (cnt--) {
                            *dptr++ = (signed char) *sptr;
                            sptr += nch;
                        }

                        break;

                    case 2:
                        while (cnt--) {
                            *dptr++ = (int16_t) *sptr;
                            sptr += nch;
                        }

                        break;

                    case 3:
                        while (cnt--) {
                            *dptr++ = (int32_t)((uint32_t)*sptr << 8) >> 8;
                            sptr += nch;
                        }

                        break;

                    default:
                        while (cnt--) {
                            *dptr++ = *sptr;
                            sptr += nch;
                        }
                }

                source_pointer++;
            }
            else {
                switch (wpc->config.bytes_per_sample) {
                    case 1:
                        while (cnt--) {
                            *dptr++ = (signed char) sptr [0];
                            *dptr++ = (signed char) sptr [1];
                            sptr += nch;
                        }

                        break;

                    case 2:
                        while (cnt--) {
                            *dptr++ = (int16_t) sptr [0];
                            *dptr++ = (int16_t) sptr [1];
                            sptr += nch;
                        }

                        break;

                    case 3:
                        while (cnt--) {
                            *dptr++ = (int32_t)((uint32_t)sptr [0] << 8) >> 8;
                            *dptr++ = (int32_t)((uint32_t)sptr [1] << 8) >> 8;
                            sptr += nch;
                        }

                        break;

                    default:
                        while (cnt--) {
                            *dptr++ = sptr [0];
                            *dptr++ = sptr [1];
                            sptr += nch;
                        }
                }

                source_pointer += 2;
            }
        }

        sample_buffer += samples_to_copy * nch;
        sample_count -= samples_to_copy;

        if ((wpc->acc_samples += samples_to_copy) == wpc->max_samples &&
            !pack_streams (wpc, wpc->block_samples,
                wpc->acc_samples - wpc->block_samples + sample_count < wpc->max_samples))
                    return FALSE;
    }

    return TRUE;
}

// Flush all accumulated samples into WavPack blocks. This is normally called
// after all samples have been sent to WavpackPackSamples(), but can also be
// called to terminate a WavPack block at a specific sample (in other words it
// is possible to continue after this operation). This is also called to
// dump non-audio blocks like those holding metadata for various purposes.
// A return of FALSE indicates an error.

int WavpackFlushSamples (WavpackContext *wpc)
{
    while (wpc->acc_samples) {
        uint32_t block_samples;

        if (wpc->acc_samples > wpc->block_samples)
            block_samples = wpc->acc_samples / 2;
        else
            block_samples = wpc->acc_samples;

        if (!pack_streams (wpc, block_samples, block_samples == wpc->acc_samples))
            return FALSE;
    }

    if (wpc->metacount)
        write_metadata_block (wpc);

    return TRUE;
}

// Note: The following function is no longer required because a proper wav
// header is now automatically generated for the application. However, if the
// application wants to generate its own header or wants to include additional
// chunks, then this function can still be used in which case the automatic
// wav header generation is suppressed.

// Add wrapper (currently RIFF only) to WavPack blocks. This should be called
// before sending any audio samples for the RIFF header or after all samples
// have been sent for any RIFF trailer. WavpackFlushSamples() should be called
// between sending the last samples and calling this for trailer data to make
// sure that headers and trailers don't get mixed up in very short files. If
// the exact contents of the RIFF header are not known because, for example,
// the file duration is uncertain or trailing chunks are possible, simply write
// a "dummy" header of the correct length. When all data has been written it
// will be possible to read the first block written and update the header
// directly. An example of this can be found in the Audition filter. A
// return of FALSE indicates an error.

int WavpackAddWrapper (WavpackContext *wpc, void *data, uint32_t bcount)
{
    int64_t index = WavpackGetSampleIndex64 (wpc);
    unsigned char meta_id;

    if (!index || index == -1) {
        wpc->riff_header_added = TRUE;
        meta_id = wpc->file_format ? ID_ALT_HEADER : ID_RIFF_HEADER;
    }
    else {
        wpc->riff_trailer_bytes += bcount;
        meta_id = wpc->file_format ? ID_ALT_TRAILER : ID_RIFF_TRAILER;
    }

    return add_to_metadata (wpc, data, bcount, meta_id);
}

// Store computed MD5 sum in WavPack metadata. Note that the user must compute
// the 16 byte sum; it is not done here. A return of FALSE indicates an error.
// If any of the lower 8 bits of qmode are set, then this MD5 is stored with
// a metadata ID that old decoders do not recognize (because they would not
// interpret the qmode and would therefore fail the verification).

int WavpackStoreMD5Sum (WavpackContext *wpc, unsigned char data [16])
{
    return add_to_metadata (wpc, data, 16, (wpc->config.qmode & 0xff) ? ID_ALT_MD5_CHECKSUM : ID_MD5_CHECKSUM);
}

#pragma pack(push,4)

typedef struct {
    char ckID [4];
    uint64_t chunkSize64;
} CS64Chunk;

typedef struct {
    uint64_t riffSize64, dataSize64, sampleCount64;
    uint32_t tableLength;
} DS64Chunk;

typedef struct {
    char ckID [4];
    uint32_t ckSize;
    char junk [28];
} JunkChunk;

#pragma pack(pop)

#define DS64ChunkFormat "DDDL"

static int create_riff_header (WavpackContext *wpc, int64_t total_samples, void *outbuffer)
{
    int do_rf64 = 0, write_junk = 1;
    ChunkHeader ds64hdr, datahdr, fmthdr;
    char *outptr = outbuffer;
    RiffChunkHeader riffhdr;
    DS64Chunk ds64_chunk;
    JunkChunk junkchunk;
    WaveHeader wavhdr;

    int64_t total_data_bytes, total_riff_bytes;
    int32_t channel_mask = wpc->config.channel_mask;
    int32_t sample_rate = wpc->config.sample_rate;
    int bytes_per_sample = wpc->config.bytes_per_sample;
    int bits_per_sample = wpc->config.bits_per_sample;
    int format = (wpc->config.float_norm_exp) ? 3 : 1;
    int num_channels = wpc->config.num_channels;
    int wavhdrsize = 16;

    wpc->riff_header_created = TRUE;

    if (format == 3 && wpc->config.float_norm_exp != 127) {
        strcpy (wpc->error_message, "can't create valid RIFF wav header for non-normalized floating data!");
        return FALSE;
    }

    if (total_samples == -1)
        total_samples = 0x7ffff000 / (bytes_per_sample * num_channels);

    total_data_bytes = total_samples * bytes_per_sample * num_channels;

    if (total_data_bytes > 0xff000000) {
        write_junk = 0;
        do_rf64 = 1;
    }

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

    memcpy (riffhdr.ckID, do_rf64 ? "RF64" : "RIFF", sizeof (riffhdr.ckID));
    memcpy (riffhdr.formType, "WAVE", sizeof (riffhdr.formType));
    total_riff_bytes = sizeof (riffhdr) + wavhdrsize + sizeof (datahdr) + total_data_bytes + wpc->riff_trailer_bytes;
    if (do_rf64) total_riff_bytes += sizeof (ds64hdr) + sizeof (ds64_chunk);
    if (write_junk) total_riff_bytes += sizeof (junkchunk);
    memcpy (fmthdr.ckID, "fmt ", sizeof (fmthdr.ckID));
    memcpy (datahdr.ckID, "data", sizeof (datahdr.ckID));
    fmthdr.ckSize = wavhdrsize;

    if (write_junk) {
        CLEAR (junkchunk);
        memcpy (junkchunk.ckID, "junk", sizeof (junkchunk.ckID));
        junkchunk.ckSize = sizeof (junkchunk) - 8;
        WavpackNativeToLittleEndian (&junkchunk, ChunkHeaderFormat);
    }

    if (do_rf64) {
        memcpy (ds64hdr.ckID, "ds64", sizeof (ds64hdr.ckID));
        ds64hdr.ckSize = sizeof (ds64_chunk);
        CLEAR (ds64_chunk);
        ds64_chunk.riffSize64 = total_riff_bytes;
        ds64_chunk.dataSize64 = total_data_bytes;
        ds64_chunk.sampleCount64 = total_samples;
        riffhdr.ckSize = (uint32_t) -1;
        datahdr.ckSize = (uint32_t) -1;
        WavpackNativeToLittleEndian (&ds64hdr, ChunkHeaderFormat);
        WavpackNativeToLittleEndian (&ds64_chunk, DS64ChunkFormat);
    }
    else {
        riffhdr.ckSize = (uint32_t) total_riff_bytes;
        datahdr.ckSize = (uint32_t) total_data_bytes;
    }

    WavpackNativeToLittleEndian (&riffhdr, ChunkHeaderFormat);
    WavpackNativeToLittleEndian (&fmthdr, ChunkHeaderFormat);
    WavpackNativeToLittleEndian (&wavhdr, WaveHeaderFormat);
    WavpackNativeToLittleEndian (&datahdr, ChunkHeaderFormat);

    // write the RIFF chunks up to just before the data starts

    outptr = (char *) memcpy (outptr, &riffhdr, sizeof (riffhdr)) + sizeof (riffhdr);

    if (do_rf64) {
        outptr = (char *) memcpy (outptr, &ds64hdr, sizeof (ds64hdr)) + sizeof (ds64hdr);
        outptr = (char *) memcpy (outptr, &ds64_chunk, sizeof (ds64_chunk)) + sizeof (ds64_chunk);
    }

    if (write_junk)
        outptr = (char *) memcpy (outptr, &junkchunk, sizeof (junkchunk)) + sizeof (junkchunk);

    outptr = (char *) memcpy (outptr, &fmthdr, sizeof (fmthdr)) + sizeof (fmthdr);
    outptr = (char *) memcpy (outptr, &wavhdr, wavhdrsize) + wavhdrsize;
    outptr = (char *) memcpy (outptr, &datahdr, sizeof (datahdr)) + sizeof (datahdr);

    return (int)(outptr - (char *) outbuffer);
}

static int block_add_checksum (unsigned char *buffer_start, unsigned char *buffer_end, int bytes);

// Pack the given stream into the allocated block buffers, including appending the block
// checksum. The only error possible is overflowing the buffers (which generally should
// be big enough to avoid this) and this is indicated with a FALSE return. This is
// threadsafe across streams, so it can be called directly from the main packing code
// or from the worker threads.

static int pack_stream_block (WavpackStream *wps)
{
    int result;

#ifdef ENABLE_DSD
    if (wps->wphdr.flags & DSD_FLAG)
        result = pack_dsd_block (wps, wps->sample_buffer);
    else
#endif
        result = pack_block (wps, wps->sample_buffer);

    if (result) {
        result = block_add_checksum (wps->blockbuff, wps->blockend, (wps->wphdr.flags & HYBRID_FLAG) ? 2 : 4);

        if (result && wps->block2buff)
            result = block_add_checksum (wps->block2buff, wps->block2end, 2);
    }

    return result;
}

// Write the packed data from the specified stream to the output file. This part is NOT threadsafe
// and must be performed in the stream order. This step includes potentially converting the WavPack
// header to little-endian and freeing the block buffers. If the block output function fails then
// this is flagged here and we retain that status through potentially multiple calls.

static int write_stream_block (WavpackStream *wps, int result)
{
    WavpackContext *wpc = (WavpackContext *) wps->wpc;  // safe because this is called single-threaded
    int bcount;

    if (result) {
        bcount = ((WavpackHeader *) wps->blockbuff)->ckSize + 8;
        WavpackNativeToLittleEndian ((WavpackHeader *) wps->blockbuff, WavpackHeaderFormat);
        result = wpc->blockout (wpc->wv_out, wps->blockbuff, bcount);

        if (result)
            wpc->filelen += bcount;
        else
            strcpy (wpc->error_message, "can't write WavPack data, disk probably full!");
    }

    free (wps->blockbuff);
    wps->blockbuff = NULL;

    if (wps->block2buff) {
        if (result) {
            bcount = ((WavpackHeader *) wps->block2buff)->ckSize + 8;
            WavpackNativeToLittleEndian ((WavpackHeader *) wps->block2buff, WavpackHeaderFormat);
            result = wpc->blockout (wpc->wvc_out, wps->block2buff, bcount);

            if (result)
                wpc->file2len += bcount;
            else
                strcpy (wpc->error_message, "can't write WavPack data, disk probably full!");
        }

        free (wps->block2buff);
        wps->block2buff = NULL;
    }

    wps->sample_index += wps->wphdr.block_samples;  // now that block is written, this is safe to do
    return result;
}

// Pack all streams and write the completed WavPack blocks to the output. The number of samples
// to be processed is specified by "block_samples", although in some situations fewer samples
// may actually be processed (e.g., hybrid lossy mode with DNS). Unused data in the
// sample_buffer will be moved up if the entire buffer is not exhausted.
//
// Multithreading is handled here. For multichannel files (i.e., more than one stream) the
// multithreading is done spatially across the multiple streams and will therefore result in
// identical output files. For single-stream files (i.e., mono or stereo) the multithreading
// is done temporally, meaning we start blocks in the future before past blocks are done.
// Unfortunately most WavPack modes use some information from the previous block during
// encoding, and so some provisions must be made here for discontinuities. Also, this means
// that the resulting output files will generally NOT be identical (although they will be
// essentially equivalent).
//
// The "last_block" parameter indicates that this is the final call to this function in this
// sequence. Otherwise, this function could return while packing is still occurring on worker
// threads (in which case it must be called again to write the results to the output). The
// exception to this is that on an error return (FALSE) the workers will be finished.

static int pack_streams (WavpackContext *wpc, uint32_t block_samples, int last_block)
{
    uint32_t max_blocksize, max_chans = 1;
    int result = TRUE, stream_index, i;

    // for calculating output (block) buffer size, first see if any streams are stereo

    for (i = 0; i < wpc->num_streams; i++)
        if (!(wpc->streams [i]->wphdr.flags & MONO_FLAG)) {
            max_chans = 2;
            break;
        }

    // then calculate maximum size based on bytes / sample

    max_blocksize = block_samples * max_chans * ((wpc->streams [0]->wphdr.flags & BYTES_STORED) + 1);

    // add margin based on how much "negative" compression is possible with pathological audio

    if ((wpc->config.flags & CONFIG_FLOAT_DATA) && !(wpc->config.flags & CONFIG_SKIP_WVX))
        max_blocksize += max_blocksize;         // 100% margin for lossless float data
    else
        max_blocksize += max_blocksize >> 2;    // otherwise 25% margin for everything else

    max_blocksize += wpc->metabytes + 1024;     // finally, add metadata & another 1K margin
    max_blocksize += max_blocksize & 1;         // and make sure it's even so we detect overflow

    for (stream_index = 0; result && stream_index < wpc->num_streams; stream_index++) {
        WavpackStream *wps = wpc->streams [stream_index];
        uint32_t flags = wps->wphdr.flags;

        flags &= ~MAG_MASK;
        flags += (1U << MAG_LSB) * ((flags & BYTES_STORED) * 8 + 7);

        SET_BLOCK_INDEX (wps->wphdr, wps->sample_index);
        wps->wphdr.block_samples = block_samples;
        wps->wphdr.flags = flags;
        wps->block2buff = (wpc->wvc_flag) ? malloc (max_blocksize) : NULL;
        wps->block2end = (wpc->wvc_flag) ? wps->block2buff + max_blocksize : NULL;
        wps->blockbuff = malloc (max_blocksize);
        wps->blockend = wps->blockbuff + max_blocksize;

#ifdef ENABLE_THREADS
        // If there is a worker thread available, and we're not doing the final stream (which
        // implies we're doing multichannel) then we can start packing this stream on a worker
        // thread. In this case we pass the WavpackStream structure directly (i.e., not a copy).

        if (worker_available (wpc) && stream_index < wpc->num_streams - 1) {
            result = write_completed_blocks (wpc, FALSE, result);
            pack_samples_enqueue (wps, FALSE);
        }

        // If there is a worker available and we're doing a single stream (i.e., mono or stereo) and
        // it's not the very first block (which might have metadata which needs to be sent unthreaded)
        // and this is not the last block to be processed during this call to WavpackPackSamples(),
        // then we can start packing this block on a worker and move on to the next block. Note that
        // since we will continue with this stream before the packing is complete, we must make a
        // copy of the WavpackStream structure that will be freed once the block has been written.
        // This also implies that packing will continue in the background after pack_streams() has
        // returned, but we will not return to the user until they're all done, obviously.

        else if (worker_available (wpc) && wpc->num_streams == 1 && wps->sample_index && !last_block) {
            WavpackStream *wps_copy = malloc (sizeof (WavpackStream));

            memcpy (wps_copy, wps, sizeof (WavpackStream));

            // If there is a discontinuity (i.e., the previous block is not done, so we can't get any
            // continuation information from it) then we must essentially start fresh. For "high" mode
            // DSD we are able to use some number of previous samples to compensate for this, and the
            // "fast" DSD mode is always encoded independently, so that's a freebie.

            if (wps->discontinuous)
                pack_init (wps_copy);

            wps_copy->sample_buffer = malloc (block_samples * (wps->wphdr.flags & MONO_FLAG ? 4 : 8));
            memcpy (wps_copy->sample_buffer, wps->sample_buffer, block_samples * (wps->wphdr.flags & MONO_FLAG ? 4 : 8));

            if (wps->discontinuous && wps->pre_sample_buffer && wps->num_pre_samples) {
                wps_copy->pre_sample_buffer = malloc (wps->num_pre_samples * (wps->wphdr.flags & MONO_FLAG ? 4 : 8));
                memcpy (wps_copy->pre_sample_buffer, wps->pre_sample_buffer, wps->num_pre_samples * (wps->wphdr.flags & MONO_FLAG ? 4 : 8));
            }
            else {
                wps_copy->pre_sample_buffer = NULL;
                wps_copy->num_pre_samples = 0;
            }

            if (wps->dsd.ptable) {
                wps_copy->dsd.ptable = malloc (256 * sizeof (*wps->dsd.ptable));
                memcpy (wps_copy->dsd.ptable, wps->dsd.ptable, 256 * sizeof (*wps->dsd.ptable));
            }

            result = write_completed_blocks (wpc, FALSE, result);
            pack_samples_enqueue (wps_copy, TRUE);

            // Fix up the existing WavpackStream, and mark it as "discontinuous" because we will be continuing with
            // it before this operation is complete. The block buffers will be freed after the block is written.

            wps->sample_index += block_samples;
            wps->discontinuous = TRUE;
            wps->blockbuff = NULL;
            wps->block2buff = NULL;
        }
        else
#endif
        {
            // This code handles blocks compressed now and sent in the foreground,
            // although we might have to wait for all backgrounded blocks to send.

            if (wps->discontinuous)
                pack_init (wps);

            result = pack_stream_block (wps);

            if (wps->wphdr.block_samples != block_samples)
                block_samples = wps->wphdr.block_samples;

            if (!result) {
                strcpy (wpc->error_message, "output buffer overflowed!");
                break;
            }

            wpc->lossy_blocks |= wps->lossy_blocks;

#ifdef ENABLE_THREADS
            // all background blocks must be complete and sent before we send this one...
            if (wpc->num_workers)
                result = write_completed_blocks (wpc, TRUE, result);
#endif

            result = write_stream_block (wps, result);
            wps->discontinuous = FALSE;
        }

        // This handles maintaining the "pre sample buffer" if requested. Currently this is just used for DSD
        // "high" mode, but it could be used in the future for PCM data instead of starting over from scratch

        if (wps->discontinuous && wps->wpc->max_pre_samples) {
            if (!wps->pre_sample_buffer)
                wps->pre_sample_buffer = malloc (wps->wpc->max_pre_samples * (wps->wphdr.flags & MONO_FLAG ? 4 : 8));

            if (wps->wpc->block_samples > wps->wpc->max_pre_samples) {
                memcpy (wps->pre_sample_buffer,
                    wps->sample_buffer + (wps->wpc->block_samples - wps->wpc->max_pre_samples) * (wps->wphdr.flags & MONO_FLAG ? 1 : 2),
                    wps->wpc->max_pre_samples * (wps->wphdr.flags & MONO_FLAG ? 4 : 8));

                wps->num_pre_samples = wps->wpc->max_pre_samples;
            }
            else {
                memcpy (wps->pre_sample_buffer, wps->sample_buffer, wps->wpc->block_samples * (wps->wphdr.flags & MONO_FLAG ? 4 : 8));
                wps->num_pre_samples = wps->wpc->block_samples;
            }
        }
    }

#ifdef ENABLE_THREADS
    // All background blocks must be complete and sent before we return if this is the last
    // block of the WavpackPackSamples() call (or an error occurred). Of course, for
    // multichannel audio, this was already done above before we sent the last stream.

    if (wpc->num_workers && (last_block || !result))
        result = write_completed_blocks (wpc, TRUE, result);
#endif

    // If there's still audio in the sample buffer, move it up to the front for the next call

    if (wpc->acc_samples != block_samples)
        for (stream_index = 0; result && stream_index < wpc->num_streams; stream_index++) {
            WavpackStream *wps = wpc->streams [stream_index];
            memmove (wps->sample_buffer,
                     wps->sample_buffer + block_samples * (wps->wphdr.flags & MONO_FLAG ? 1 : 2),
                    (wpc->acc_samples - block_samples) * (wps->wphdr.flags & MONO_FLAG ? 4 : 8));
        }

    wpc->ave_block_samples = (wpc->ave_block_samples * 0x7 + block_samples + 0x4) >> 3;
    wpc->acc_samples -= block_samples;

    return result;
}

// Given the pointer to the first block written (to either a .wv or .wvc file),
// update the block with the actual number of samples written. If the wav
// header was generated by the library, then it is updated also. This should
// be done if WavpackSetConfiguration() was called with an incorrect number
// of samples (or -1). It is the responsibility of the application to read and
// rewrite the block. An example of this can be found in the Audition filter.

static void block_update_checksum (unsigned char *buffer_start);

void WavpackUpdateNumSamples (WavpackContext *wpc, void *first_block)
{
    uint32_t wrapper_size;

    if (wpc->riff_header_created && WavpackGetWrapperLocation (first_block, &wrapper_size)) {
        unsigned char riff_header [128];

        if (wrapper_size == create_riff_header (wpc, WavpackGetSampleIndex64 (wpc), riff_header))
            memcpy (WavpackGetWrapperLocation (first_block, NULL), riff_header, wrapper_size);
    }

    WavpackLittleEndianToNative (first_block, WavpackHeaderFormat);
    SET_TOTAL_SAMPLES (* (WavpackHeader *) first_block, WavpackGetSampleIndex64 (wpc));
    block_update_checksum (first_block);
    WavpackNativeToLittleEndian (first_block, WavpackHeaderFormat);
}

// Note: The following function is no longer required because the wav header
// automatically generated for the application will also be updated by
// WavpackUpdateNumSamples (). However, if the application wants to generate
// its own header or wants to include additional chunks, then this function
// still must be used to update the application generated header.

// Given the pointer to the first block written to a WavPack file, this
// function returns the location of the stored RIFF header that was originally
// written with WavpackAddWrapper(). This would normally be used to update
// the wav header to indicate that a different number of samples was actually
// written or if additional RIFF chunks are written at the end of the file.
// The "size" parameter can be set to non-NULL to obtain the exact size of the
// RIFF header, and the function will return FALSE if the header is not found
// in the block's metadata (or it is not a valid WavPack block). It is the
// responsibility of the application to read and rewrite the block. An example
// of this can be found in the Audition filter.

static void *find_metadata (void *wavpack_block, int desired_id, uint32_t *size);

void *WavpackGetWrapperLocation (void *first_block, uint32_t *size)
{
    void *loc;

    WavpackLittleEndianToNative (first_block, WavpackHeaderFormat);
    loc = find_metadata (first_block, ID_RIFF_HEADER, size);

    if (!loc)
        loc = find_metadata (first_block, ID_ALT_HEADER, size);

    WavpackNativeToLittleEndian (first_block, WavpackHeaderFormat);

    return loc;
}

static void *find_metadata (void *wavpack_block, int desired_id, uint32_t *size)
{
    WavpackHeader *wphdr = wavpack_block;
    unsigned char *dp, meta_id, c1, c2;
    int32_t bcount, meta_bc;

    if (strncmp (wphdr->ckID, "wvpk", 4))
        return NULL;

    bcount = wphdr->ckSize - sizeof (WavpackHeader) + 8;
    dp = (unsigned char *)(wphdr + 1);

    while (bcount >= 2) {
        meta_id = *dp++;
        c1 = *dp++;

        meta_bc = c1 << 1;
        bcount -= 2;

        if (meta_id & ID_LARGE) {
            if (bcount < 2)
                break;

            c1 = *dp++;
            c2 = *dp++;
            meta_bc += ((uint32_t) c1 << 9) + ((uint32_t) c2 << 17);
            bcount -= 2;
        }

        if ((meta_id & ID_UNIQUE) == desired_id) {
            if ((bcount - meta_bc) >= 0) {
                if (size)
                    *size = meta_bc - ((meta_id & ID_ODD_SIZE) ? 1 : 0);

                return dp;
            }
            else
                return NULL;
        }

        bcount -= meta_bc;
        dp += meta_bc;
    }

    return NULL;
}

int copy_metadata (WavpackMetadata *wpmd, unsigned char *buffer_start, unsigned char *buffer_end)
{
    uint32_t mdsize = wpmd->byte_length + (wpmd->byte_length & 1);
    WavpackHeader *wphdr = (WavpackHeader *) buffer_start;

    mdsize += (wpmd->byte_length > 510) ? 4 : 2;
    buffer_start += wphdr->ckSize + 8;

    if (buffer_start + mdsize >= buffer_end)
        return FALSE;

    buffer_start [0] = wpmd->id | (wpmd->byte_length & 1 ? ID_ODD_SIZE : 0);
    buffer_start [1] = (wpmd->byte_length + 1) >> 1;

    if (wpmd->byte_length > 510) {
        buffer_start [0] |= ID_LARGE;
        buffer_start [2] = (wpmd->byte_length + 1) >> 9;
        buffer_start [3] = (wpmd->byte_length + 1) >> 17;
    }

    if (wpmd->data && wpmd->byte_length) {
        memcpy (buffer_start + (wpmd->byte_length > 510 ? 4 : 2), wpmd->data, wpmd->byte_length);

        if (wpmd->byte_length & 1)          // if size is odd, make sure pad byte is a zero
            buffer_start [mdsize - 1] = 0;
    }

    wphdr->ckSize += mdsize;
    return TRUE;
}

static int add_to_metadata (WavpackContext *wpc, void *data, uint32_t bcount, unsigned char id)
{
    WavpackMetadata *mdp;
    unsigned char *src = data;

    while (bcount) {
        if (wpc->metacount) {
            uint32_t bc = bcount;

            mdp = wpc->metadata + wpc->metacount - 1;

            if (mdp->id == id) {
                if (wpc->metabytes + bcount > 1000000)
                    bc = 1000000 - wpc->metabytes;

                mdp->data = realloc (mdp->data, mdp->byte_length + bc);
                memcpy ((char *) mdp->data + mdp->byte_length, src, bc);
                mdp->byte_length += bc;
                wpc->metabytes += bc;
                bcount -= bc;
                src += bc;

                if (wpc->metabytes >= 1000000 && !write_metadata_block (wpc))
                    return FALSE;
            }
        }

        if (bcount) {
            wpc->metadata = realloc (wpc->metadata, (wpc->metacount + 1) * sizeof (WavpackMetadata));
            mdp = wpc->metadata + wpc->metacount++;
            mdp->byte_length = 0;
            mdp->data = NULL;
            mdp->id = id;
        }
    }

    return TRUE;
}

static char *write_metadata (WavpackMetadata *wpmd, char *outdata)
{
    unsigned char id = wpmd->id, wordlen [3];

    wordlen [0] = (wpmd->byte_length + 1) >> 1;
    wordlen [1] = (wpmd->byte_length + 1) >> 9;
    wordlen [2] = (wpmd->byte_length + 1) >> 17;

    if (wpmd->byte_length & 1)
        id |= ID_ODD_SIZE;

    if (wordlen [1] || wordlen [2])
        id |= ID_LARGE;

    *outdata++ = id;
    *outdata++ = wordlen [0];

    if (id & ID_LARGE) {
        *outdata++ = wordlen [1];
        *outdata++ = wordlen [2];
    }

    if (wpmd->data && wpmd->byte_length) {
        memcpy (outdata, wpmd->data, wpmd->byte_length);
        outdata += wpmd->byte_length;

        if (wpmd->byte_length & 1)
            *outdata++ = 0;
    }

    return outdata;
}

static int write_metadata_block (WavpackContext *wpc)
{
    char *block_buff, *block_ptr;
    WavpackHeader *wphdr;

    if (wpc->metacount) {
        int metacount = wpc->metacount, block_size = sizeof (WavpackHeader);
        WavpackMetadata *wpmdp = wpc->metadata;

        while (metacount--) {
            block_size += wpmdp->byte_length + (wpmdp->byte_length & 1);
            block_size += (wpmdp->byte_length > 510) ? 4 : 2;
            wpmdp++;
        }

        // allocate 6 extra bytes for 4-byte checksum (which we add last)
        wphdr = (WavpackHeader *) (block_buff = malloc (block_size + 6));

        CLEAR (*wphdr);
        memcpy (wphdr->ckID, "wvpk", 4);
        SET_TOTAL_SAMPLES (*wphdr, wpc->total_samples);
        wphdr->version = wpc->stream_version;
        wphdr->ckSize = block_size - 8;
        wphdr->block_samples = 0;

        block_ptr = (char *)(wphdr + 1);

        wpmdp = wpc->metadata;

        while (wpc->metacount) {
            block_ptr = write_metadata (wpmdp, block_ptr);
            wpc->metabytes -= wpmdp->byte_length;
            free_metadata (wpmdp++);
            wpc->metacount--;
        }

        free (wpc->metadata);
        wpc->metadata = NULL;
        // add a 4-byte checksum here (increases block size by 6)
        block_add_checksum ((unsigned char *) block_buff, (unsigned char *) block_buff + (block_size += 6), 4);
        WavpackNativeToLittleEndian ((WavpackHeader *) block_buff, WavpackHeaderFormat);

        if (!wpc->blockout (wpc->wv_out, block_buff, block_size)) {
            free (block_buff);
            strcpy (wpc->error_message, "can't write WavPack data, disk probably full!");
            return FALSE;
        }

        free (block_buff);
    }

    return TRUE;
}

void free_metadata (WavpackMetadata *wpmd)
{
    if (wpmd->data) {
        free (wpmd->data);
        wpmd->data = NULL;
    }
}

// These two functions add or update the block checksums that were introduced in WavPack 5.0.
// The presence of the checksum is indicated by a flag in the wavpack header (HAS_CHECKSUM)
// and the actual metadata item should be the last one in the block, and can be either 2 or 4
// bytes. Of course, older versions of the decoder will simply ignore both of these.

static int block_add_checksum (unsigned char *buffer_start, unsigned char *buffer_end, int bytes)
{
    WavpackHeader *wphdr = (WavpackHeader *) buffer_start;
#ifdef BITSTREAM_SHORTS
    uint16_t *csptr = (uint16_t*) buffer_start;
#else
    unsigned char *csptr = buffer_start;
#endif
    int bcount = wphdr->ckSize + 8, wcount;
    uint32_t csum = (uint32_t) -1;

    if (bytes != 2 && bytes != 4)
        return FALSE;

    if (bcount < sizeof (WavpackHeader) || (bcount & 1) || buffer_start + bcount + 2 + bytes > buffer_end)
        return FALSE;

    wphdr->flags |= HAS_CHECKSUM;
    wphdr->ckSize += 2 + bytes;
    wcount = bcount >> 1;

#ifdef BITSTREAM_SHORTS
    while (wcount--)
        csum = (csum * 3) + *csptr++;
#else
    WavpackNativeToLittleEndian ((WavpackHeader *) buffer_start, WavpackHeaderFormat);

    while (wcount--) {
        csum = (csum * 3) + csptr [0] + (csptr [1] << 8);
        csptr += 2;
    }

    WavpackLittleEndianToNative ((WavpackHeader *) buffer_start, WavpackHeaderFormat);
#endif

    buffer_start += bcount;
    *buffer_start++ = ID_BLOCK_CHECKSUM;
    *buffer_start++ = bytes >> 1;

    if (bytes == 4) {
        *buffer_start++ = csum;
        *buffer_start++ = csum >> 8;
        *buffer_start++ = csum >> 16;
        *buffer_start++ = csum >> 24;
    }
    else {
        csum ^= csum >> 16;
        *buffer_start++ = csum;
        *buffer_start++ = csum >> 8;
    }

    return TRUE;
}

static void block_update_checksum (unsigned char *buffer_start)
{
    WavpackHeader *wphdr = (WavpackHeader *) buffer_start;
    unsigned char *dp, meta_id, c1, c2;
    uint32_t bcount, meta_bc;

    if (!(wphdr->flags & HAS_CHECKSUM))
        return;

    bcount = wphdr->ckSize - sizeof (WavpackHeader) + 8;
    dp = (unsigned char *)(wphdr + 1);

    while (bcount >= 2) {
        meta_id = *dp++;
        c1 = *dp++;

        meta_bc = c1 << 1;
        bcount -= 2;

        if (meta_id & ID_LARGE) {
            if (bcount < 2)
                return;

            c1 = *dp++;
            c2 = *dp++;
            meta_bc += ((uint32_t) c1 << 9) + ((uint32_t) c2 << 17);
            bcount -= 2;
        }

        if (bcount < meta_bc)
            return;

        if ((meta_id & ID_UNIQUE) == ID_BLOCK_CHECKSUM) {
#ifdef BITSTREAM_SHORTS
            uint16_t *csptr = (uint16_t*) buffer_start;
#else
            unsigned char *csptr = buffer_start;
#endif
            int wcount = (int)(dp - 2 - buffer_start) >> 1;
            uint32_t csum = (uint32_t) -1;

            if ((meta_id & ID_ODD_SIZE) || meta_bc < 2 || meta_bc > 4)
                return;

#ifdef BITSTREAM_SHORTS
            while (wcount--)
                csum = (csum * 3) + *csptr++;
#else
            WavpackNativeToLittleEndian ((WavpackHeader *) buffer_start, WavpackHeaderFormat);

            while (wcount--) {
                csum = (csum * 3) + csptr [0] + (csptr [1] << 8);
                csptr += 2;
            }

            WavpackLittleEndianToNative ((WavpackHeader *) buffer_start, WavpackHeaderFormat);
#endif

            if (meta_bc == 4) {
                *dp++ = csum;
                *dp++ = csum >> 8;
                *dp++ = csum >> 16;
                *dp++ = csum >> 24;
                return;
            }
            else {
                csum ^= csum >> 16;
                *dp++ = csum;
                *dp++ = csum >> 8;
                return;
            }
        }

        bcount -= meta_bc;
        dp += meta_bc;
    }
}

///////////////////////////// multithreading code ////////////////////////////////

#ifdef ENABLE_THREADS

// This is the worker thread function for packing support, essentially allowing
// pack_stream_block() to be running for multiple streams simultaneously.

#ifdef _WIN32
static unsigned WINAPI pack_samples_worker_thread (LPVOID param)
#else
static void *pack_samples_worker_thread (void *param)
#endif
{
    WorkerInfo *cxt = param;

    while (1) {
        wp_mutex_obtain (*cxt->mutex);
        cxt->state = Ready;
        (*cxt->workers_ready)++;
        wp_condvar_signal (*cxt->global_cond);      // signal that we're ready to work

        while (cxt->state == Ready)                 // wait for something to do
            wp_condvar_wait (cxt->worker_cond, *cxt->mutex);

        wp_mutex_release (*cxt->mutex);

        if (cxt->state == Quit)                     // break out if we're done
            break;

        cxt->result = pack_stream_block (cxt->wps); // this is where the work is done

        wp_mutex_obtain (*cxt->mutex);
        cxt->state = Done;
        wp_condvar_signal (*cxt->global_cond);      // signal completion

        while (cxt->state == Done)                  // wait for output to be written
            wp_condvar_wait (cxt->worker_cond, *cxt->mutex);

        wp_mutex_release (*cxt->mutex);

        if (cxt->state == Quit)                     // should check for quit here too
            break;
    }

    wp_thread_exit (0);
    return 0;
}

// Send the given stream to an available worker thread. In the background, the specified
// stream will begin the packing operation. The "free_wps" flag indicates that the
// WavpackStream structure should be freed once the unpack operation is complete because
// it is a copy of the original created for this operation only.

static void pack_samples_enqueue (WavpackStream *wps, int free_wps)
{
    WavpackContext *wpc = (WavpackContext *) wps->wpc;  // this is safe here because single-threaded
    int i;

    wp_mutex_obtain (wpc->mutex);

    while (!wpc->workers_ready)                         // make sure a worker thread is ready
        wp_condvar_wait (wpc->global_cond, wpc->mutex);

    for (i = 0; i < wpc->num_workers; ++i)
        if (wpc->workers [i].state == Ready) {
            wpc->workers [i].wps = wps;
            wpc->workers [i].state = Running;
            wpc->workers [i].free_wps = free_wps;
            wp_condvar_signal (wpc->workers [i].worker_cond);
            wpc->workers_ready--;
            break;
        }

    wp_mutex_release (wpc->mutex);
}

// If there are streams that have completed the packing operation, send their packed
// data to the block output device. Depending on the "write_all_blocks" parameter, this
// will return either as soon as there is a single worker thread in the "ready" state
// or once they're all in the "ready" state. If there are workers available already,
// it will return without doing anything. The "result" status passes through here and
// indicates that one of the two possible errors occurred: the output buffer overflowed
// or the write callback (to the user) failed.

static int write_completed_blocks (WavpackContext *wpc, int write_all_blocks, int result)
{
    wp_mutex_obtain (wpc->mutex);

    while (!wpc->workers_ready || (write_all_blocks && wpc->workers_ready < wpc->num_workers)) {
        WorkerInfo *next_worker = NULL;
        int i;

        // loop through the worker threads to determine the lowest sample index
        // (or stream index if the sample index is the same) that is either running
        // or done (which will be the next one we send)

        for (i = 0; i < wpc->num_workers; ++i)
            if (wpc->workers [i].state == Done || wpc->workers [i].state == Running)
                if (!next_worker || wpc->workers [i].wps->sample_index < next_worker->wps->sample_index ||
                   (wpc->workers [i].wps->sample_index == next_worker->wps->sample_index &&
                    wpc->workers [i].wps->stream_index < next_worker->wps->stream_index))
                        next_worker = &wpc->workers [i];

        // if the lowest stream is done, then we can send it now, otherwise wait

        if (next_worker && next_worker->state == Done) {
            wp_mutex_release (wpc->mutex);
            wpc->lossy_blocks |= next_worker->wps->lossy_blocks;

            if (result && !next_worker->result) {
                strcpy (wpc->error_message, "output buffer overflowed!");
                result = next_worker->result;
            }

            result = write_stream_block (next_worker->wps, result);

            if (next_worker->free_wps) {
                free (next_worker->wps->pre_sample_buffer);
                free (next_worker->wps->sample_buffer);
                free (next_worker->wps->dsd.ptable);
                free (next_worker->wps);
            }

            wp_mutex_obtain (wpc->mutex);
            next_worker->state = Uninit;
            wp_condvar_signal (next_worker->worker_cond);   // signal the thread so it can go ready
        }
        else
            wp_condvar_wait (wpc->global_cond, wpc->mutex);
    }

    wp_mutex_release (wpc->mutex);

    return result;
}

// Create the worker thread contexts and start the threads
// (which should all quickly go to the ready state)

static void worker_threads_create (WavpackContext *wpc)
{
    if (!wpc->workers) {
        int i;

        wp_mutex_init (wpc->mutex);
        wp_condvar_init (wpc->global_cond);

        wpc->workers = calloc (wpc->num_workers, sizeof (WorkerInfo));

        for (i = 0; i < wpc->num_workers; ++i) {
            wpc->workers [i].mutex = &wpc->mutex;
            wpc->workers [i].global_cond = &wpc->global_cond;
            wpc->workers [i].workers_ready = &wpc->workers_ready;
            wp_condvar_init (wpc->workers [i].worker_cond);
            wp_thread_create (wpc->workers [i].thread, pack_samples_worker_thread, &wpc->workers [i]);

            // gracefully handle failures in creating worker threads

            if (!wpc->workers [i].thread) {
                wp_condvar_delete (wpc->workers [i].worker_cond);
                wpc->num_workers = i;
                break;
            }
        }

        if (!wpc->num_workers) {    // if we failed to start any workers, free the array
            free (wpc->workers);
            wpc->workers = NULL;
        }
    }
}

// Return TRUE if there is a worker thread available. Obviously this depends on
// whether there are worker threads enabled and running, but it also depends
// on whether there is actually a worker thread that's not busy (however we
// don't count this as a requirement if there are many workers).

static int worker_available (WavpackContext *wpc)
{
    int retval = FALSE;

    if (wpc->num_workers && wpc->workers) {
        if (wpc->num_workers < 4) {
            wp_mutex_obtain (wpc->mutex);
            retval = wpc->workers_ready;
            wp_mutex_release (wpc->mutex);
        }
        else
            retval = TRUE;
    }

    return retval;
}

#endif
