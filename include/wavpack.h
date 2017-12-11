////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                     Short Blocks Audio Compressor                      //
//                Copyright (c) 1998 - 2017 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// wavpack.h

#ifndef WAVPACK_H
#define WAVPACK_H

// This header file contains all the definitions required to use the
// functions in "wputils.c" to read and write WavPack files and streams.

#include <sys/types.h>

#if defined(_MSC_VER) && _MSC_VER < 1600
typedef unsigned __int64 uint64_t;
typedef unsigned __int32 uint32_t;
typedef unsigned __int16 uint16_t;
typedef unsigned __int8 uint8_t;
typedef __int64 int64_t;
typedef __int32 int32_t;
typedef __int16 int16_t;
typedef __int8  int8_t;
#else
#include <stdint.h>
#endif

// RIFF / wav header formats (these occur at the beginning of both wav files
// and pre-4.0 WavPack files that are not in the "raw" mode). Generally, an
// application using the library to read or write WavPack files will not be
// concerned with any of these.

typedef struct {
    char ckID [4];
    uint32_t ckSize;
    char formType [4];
} RiffChunkHeader;

typedef struct {
    char ckID [4];
    uint32_t ckSize;
} ChunkHeader;

#define ChunkHeaderFormat "4L"

typedef struct {
    uint16_t FormatTag, NumChannels;
    uint32_t SampleRate, BytesPerSecond;
    uint16_t BlockAlign, BitsPerSample;
    uint16_t cbSize, ValidBitsPerSample;
    int32_t ChannelMask;
    uint16_t SubFormat;
    char GUID [14];
} WaveHeader;

#define WaveHeaderFormat "SSLLSSSSLS"

///////////////////////// WavPack Configuration ///////////////////////////////

// This external structure is used during encode to provide configuration to
// the encoding engine and during decoding to provide fle information back to
// the higher level functions. Not all fields are used in both modes.

typedef struct {
    float bitrate, shaping_weight;
    int bits_per_sample, bytes_per_sample;
    int qmode, flags, xmode, num_channels, float_norm_exp;
    int32_t block_samples, extra_flags, sample_rate, channel_mask;
    unsigned char md5_checksum [16], md5_read;
    int num_tag_strings;                // this field is not used
    char **tag_strings;                 // this field is not used
} WavpackConfig;

#define CONFIG_HYBRID_FLAG      8       // hybrid mode
#define CONFIG_JOINT_STEREO     0x10    // joint stereo
#define CONFIG_CROSS_DECORR     0x20    // no-delay cross decorrelation
#define CONFIG_HYBRID_SHAPE     0x40    // noise shape (hybrid mode only)
#define CONFIG_FAST_FLAG        0x200   // fast mode
#define CONFIG_HIGH_FLAG        0x800   // high quality mode
#define CONFIG_VERY_HIGH_FLAG   0x1000  // very high
#define CONFIG_BITRATE_KBPS     0x2000  // bitrate is kbps, not bits / sample
#define CONFIG_SHAPE_OVERRIDE   0x8000  // shaping mode specified
#define CONFIG_JOINT_OVERRIDE   0x10000 // joint-stereo mode specified
#define CONFIG_DYNAMIC_SHAPING  0x20000 // dynamic noise shaping
#define CONFIG_CREATE_EXE       0x40000 // create executable
#define CONFIG_CREATE_WVC       0x80000 // create correction file
#define CONFIG_OPTIMIZE_WVC     0x100000 // maximize bybrid compression
#define CONFIG_COMPATIBLE_WRITE 0x400000 // write files for decoders < 4.3
#define CONFIG_CALC_NOISE       0x800000 // calc noise in hybrid mode
#define CONFIG_EXTRA_MODE       0x2000000 // extra processing mode
#define CONFIG_SKIP_WVX         0x4000000 // no wvx stream w/ floats & big ints
#define CONFIG_MD5_CHECKSUM     0x8000000 // store MD5 signature
#define CONFIG_MERGE_BLOCKS     0x10000000 // merge blocks of equal redundancy (for lossyWAV)
#define CONFIG_PAIR_UNDEF_CHANS 0x20000000 // encode undefined channels in stereo pairs
#define CONFIG_OPTIMIZE_MONO    0x80000000 // optimize for mono streams posing as stereo

// The lower 8 bits of qmode indicate the use of new features in version 5 that (presently)
// only apply to Core Audio Files (CAF) and DSD files, but could apply to other things too.
// These flags are stored in the file and can be retrieved by a decoder that is aware of
// them, but the individual bits are meaningless to the library. If ANY of these bits are
// set then the MD5 sum is written with a new ID so that old decoders will not see it
// (because these features will cause the MD5 sum to be different and fail).

#define QMODE_BIG_ENDIAN        0x1     // big-endian data format (opposite of WAV format)
#define QMODE_SIGNED_BYTES      0x2     // 8-bit audio data is signed (opposite of WAV format)
#define QMODE_UNSIGNED_WORDS    0x4     // audio data (other than 8-bit) is unsigned (opposite of WAV format)
#define QMODE_REORDERED_CHANS   0x8     // source channels were not Microsoft order, so they were reordered
#define QMODE_DSD_LSB_FIRST     0x10    // DSD bytes, LSB first (most Sony .dsf files)
#define QMODE_DSD_MSB_FIRST     0x20    // DSD bytes, MSB first (Philips .dff files)
#define QMODE_DSD_IN_BLOCKS     0x40    // DSD data is blocked by channels (Sony .dsf only)
#define QMODE_DSD_AUDIO         (QMODE_DSD_LSB_FIRST | QMODE_DSD_MSB_FIRST)

// The rest of the qmode word is reserved for the private use of the command-line programs
// and are ignored by the library (and not stored either). They really should not be defined
// here, but I thought it would be a good idea to have all the definitions together.

#define QMODE_ADOBE_MODE        0x100   // user specified Adobe mode
#define QMODE_NO_STORE_WRAPPER  0x200   // user specified to not store audio file wrapper (RIFF, CAFF, etc.)
#define QMODE_CHANS_UNASSIGNED  0x400   // user specified "..." in --channel-order option
#define QMODE_IGNORE_LENGTH     0x800   // user specified to ignore length in file header
#define QMODE_RAW_PCM           0x1000  // user specified raw PCM format (no header present)

////////////// Callbacks used for reading & writing WavPack streams //////////

typedef struct {
    int32_t (*read_bytes)(void *id, void *data, int32_t bcount);
    uint32_t (*get_pos)(void *id);
    int (*set_pos_abs)(void *id, uint32_t pos);
    int (*set_pos_rel)(void *id, int32_t delta, int mode);
    int (*push_back_byte)(void *id, int c);
    uint32_t (*get_length)(void *id);
    int (*can_seek)(void *id);

    // this callback is for writing edited tags only
    int32_t (*write_bytes)(void *id, void *data, int32_t bcount);
} WavpackStreamReader;

// Extended version of structure for handling large files and added
// functionality for truncating and closing files

typedef struct {
    int32_t (*read_bytes)(void *id, void *data, int32_t bcount);
    int32_t (*write_bytes)(void *id, void *data, int32_t bcount);
    int64_t (*get_pos)(void *id);                               // new signature for large files
    int (*set_pos_abs)(void *id, int64_t pos);                  // new signature for large files
    int (*set_pos_rel)(void *id, int64_t delta, int mode);      // new signature for large files
    int (*push_back_byte)(void *id, int c);
    int64_t (*get_length)(void *id);                            // new signature for large files
    int (*can_seek)(void *id);
    int (*truncate_here)(void *id);                             // new function to truncate file at current position
    int (*close)(void *id);                                     // new function to close file
} WavpackStreamReader64;

typedef int (*WavpackBlockOutput)(void *id, void *data, int32_t bcount);

//////////////////////////// function prototypes /////////////////////////////

typedef void WavpackContext;

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_WAVPACK_SAMPLES ((1LL << 40) - 257)

WavpackContext *WavpackStreamOpenRawDecoder (
    void *main_data, int32_t main_size,
    void *corr_data, int32_t corr_size,
    int16_t version, char *error, int flags, int norm_offset);

WavpackContext *WavpackStreamOpenFileInputEx64 (WavpackStreamReader64 *reader, void *wv_id, void *wvc_id, char *error, int flags, int norm_offset);
WavpackContext *WavpackStreamOpenFileInputEx (WavpackStreamReader *reader, void *wv_id, void *wvc_id, char *error, int flags, int norm_offset);
WavpackContext *WavpackStreamOpenFileInput (const char *infilename, char *error, int flags, int norm_offset);

#define OPEN_WVC        0x1     // open/read "correction" file
#define OPEN_WRAPPER    0x4     // make audio wrapper available (i.e. RIFF)
#define OPEN_2CH_MAX    0x8     // open multichannel as stereo (no downmix)
#define OPEN_NORMALIZE  0x10    // normalize floating point data to +/- 1.0
#define OPEN_FILE_UTF8  0x80    // assume filenames are UTF-8 encoded, not ANSI (Windows only)

// new for version 5

#define OPEN_DSD_NATIVE 0x100   // open DSD files as bitstreams
                                // (returned as 8-bit "samples" stored in 32-bit words)
#define OPEN_DSD_AS_PCM 0x200   // open DSD files as 24-bit PCM (decimated 8x)
#define OPEN_ALT_TYPES  0x400   // application is aware of alternate file types & qmode
                                // (just affects retrieving wrappers & MD5 checksums)
#define OPEN_NO_CHECKSUM 0x800  // don't verify block checksums before decoding

int WavpackStreamGetMode (WavpackContext *wpc);

#define MODE_WVC        0x1
#define MODE_LOSSLESS   0x2
#define MODE_HYBRID     0x4
#define MODE_FLOAT      0x8
#define MODE_HIGH       0x20
#define MODE_FAST       0x40
#define MODE_EXTRA      0x80    // extra mode used, see MODE_XMODE for possible level
#define MODE_SFX        0x200
#define MODE_VERY_HIGH  0x400
#define MODE_MD5        0x800
#define MODE_XMODE      0x7000  // mask for extra level (1-6, 0=unknown)
#define MODE_DNS        0x8000

int WavpackStreamVerifySingleBlock (unsigned char *buffer, int verify_checksum);
int WavpackStreamGetQualifyMode (WavpackContext *wpc);
char *WavpackStreamGetErrorMessage (WavpackContext *wpc);
int WavpackStreamGetVersion (WavpackContext *wpc);
char *WavpackStreamGetFileExtension (WavpackContext *wpc);
unsigned char WavpackStreamGetFileFormat (WavpackContext *wpc);
uint32_t WavpackStreamUnpackSamples (WavpackContext *wpc, int32_t *buffer, uint32_t samples);
uint32_t WavpackStreamGetNumSamples (WavpackContext *wpc);
int64_t WavpackStreamGetNumSamples64 (WavpackContext *wpc);
uint32_t WavpackStreamGetNumSamplesInFrame (WavpackContext *wpc);
uint32_t WavpackStreamGetSampleIndex (WavpackContext *wpc);
int64_t WavpackStreamGetSampleIndex64 (WavpackContext *wpc);
int WavpackStreamGetNumErrors (WavpackContext *wpc);
int WavpackStreamLossyBlocks (WavpackContext *wpc);
int WavpackStreamSeekSample (WavpackContext *wpc, uint32_t sample);
int WavpackStreamSeekSample64 (WavpackContext *wpc, int64_t sample);
WavpackContext *WavpackStreamCloseFile (WavpackContext *wpc);
uint32_t WavpackStreamGetSampleRate (WavpackContext *wpc);
uint32_t WavpackStreamGetNativeSampleRate (WavpackContext *wpc);
int WavpackStreamGetBitsPerSample (WavpackContext *wpc);
int WavpackStreamGetBytesPerSample (WavpackContext *wpc);
int WavpackStreamGetNumChannels (WavpackContext *wpc);
int WavpackStreamGetChannelMask (WavpackContext *wpc);
int WavpackStreamGetReducedChannels (WavpackContext *wpc);
int WavpackStreamGetFloatNormExp (WavpackContext *wpc);
int WavpackStreamGetMD5Sum (WavpackContext *wpc, unsigned char data [16]);
void WavpackStreamGetChannelIdentities (WavpackContext *wpc, unsigned char *identities);
uint32_t WavpackStreamGetChannelLayout (WavpackContext *wpc, unsigned char *reorder);
uint32_t WavpackStreamGetWrapperBytes (WavpackContext *wpc);
unsigned char *WavpackStreamGetWrapperData (WavpackContext *wpc);
void WavpackStreamFreeWrapper (WavpackContext *wpc);
void WavpackStreamSeekTrailingWrapper (WavpackContext *wpc);
double WavpackStreamGetProgress (WavpackContext *wpc);
uint32_t WavpackGetFileSize (WavpackContext *wpc);
int64_t WavpackGetFileSize64 (WavpackContext *wpc);
double WavpackGetRatio (WavpackContext *wpc);
double WavpackGetAverageBitrate (WavpackContext *wpc, int count_wvc);
double WavpackGetInstantBitrate (WavpackContext *wpc);

WavpackContext *WavpackStreamOpenFileOutput (WavpackBlockOutput blockout, void *wv_id, void *wvc_id);
void WavpackStreamSetFileInformation (WavpackContext *wpc, char *file_extension, unsigned char file_format);

#define WP_FORMAT_WAV   0       // Microsoft RIFF, including BWF and RF64 varients
#define WP_FORMAT_W64   1       // Sony Wave64
#define WP_FORMAT_CAF   2       // Apple CoreAudio
#define WP_FORMAT_DFF   3       // Philips DSDIFF
#define WP_FORMAT_DSF   4       // Sony DSD Format

int WavpackStreamSetConfiguration (WavpackContext *wpc, WavpackConfig *config, uint32_t total_samples);
int WavpackStreamSetConfiguration64 (WavpackContext *wpc, WavpackConfig *config, int64_t total_samples, const unsigned char *chan_ids);
int WavpackStreamSetChannelLayout (WavpackContext *wpc, uint32_t layout_tag, const unsigned char *reorder);
int WavpackStreamAddWrapper (WavpackContext *wpc, void *data, uint32_t bcount);
int WavpackStreamStoreMD5Sum (WavpackContext *wpc, unsigned char data [16]);
int WavpackStreamPackInit (WavpackContext *wpc);
int WavpackStreamPackSamples (WavpackContext *wpc, int32_t *sample_buffer, uint32_t sample_count);
int WavpackStreamFlushSamples (WavpackContext *wpc);
void WavpackStreamUpdateNumSamples (WavpackContext *wpc, void *first_block);
void *WavpackStreamGetWrapperLocation (void *first_block, uint32_t *size);
double WavpackStreamGetEncodedNoise (WavpackContext *wpc, double *peak);

void WavpackStreamFloatNormalize (int32_t *values, int32_t num_values, int delta_exp);

void WavpackStreamLittleEndianToNative (void *data, char *format);
void WavpackStreamNativeToLittleEndian (void *data, char *format);
void WavpackStreamBigEndianToNative (void *data, char *format);
void WavpackStreamNativeToBigEndian (void *data, char *format);

uint32_t WavpackStreamGetLibraryVersion (void);
const char *WavpackStreamGetLibraryVersionString (void);

#ifdef __cplusplus
}
#endif

#endif
