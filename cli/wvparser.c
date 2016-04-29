////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//                Copyright (c) 1998 - 2016 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// wvparser.c

// This is a completely self-contained utility to parse complete Wavpack files
// into their component blocks and display some informative details about them
// (including all the "metadata" sub-blocks contained in each block). This can
// be very handy for visualizing how WavPack files are structured or debugging.
// Some sanity checking is performed, but no thorough verification is done
// (and, of course, no decoding takes place). APEv2 tags are ignored as is any
// other garbage between blocks.

// Acts as a filter: WavPack file to stdin, output to stdout.

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#endif
#include <stdint.h>

typedef int32_t (*read_stream)(void *, int32_t);

////////////////////////////// WavPack Header /////////////////////////////////

// Note that this is the ONLY structure that is written to (or read from)
// WavPack 4.0 files, and is the preamble to every block in both the .wv
// and .wvc files.

typedef struct {
    char ckID [4];
    uint32_t ckSize;
    int16_t version;
    unsigned char track_no, index_no;
    uint32_t total_samples, block_index, block_samples, flags, crc;
} WavpackHeader;

#define WavpackHeaderFormat "4LS2LLLLL"

// or-values for "flags"

#define BYTES_STORED	3	// 1-4 bytes/sample
#define MONO_FLAG	4	// not stereo
#define HYBRID_FLAG	8	// hybrid mode
#define JOINT_STEREO	0x10	// joint stereo
#define CROSS_DECORR	0x20	// no-delay cross decorrelation
#define HYBRID_SHAPE	0x40	// noise shape (hybrid mode only)
#define FLOAT_DATA	0x80	// ieee 32-bit floating point data

#define INT32_DATA	0x100	// special extended int handling
#define HYBRID_BITRATE	0x200	// bitrate noise (hybrid mode only)
#define HYBRID_BALANCE	0x400	// balance noise (hybrid stereo mode only)

#define INITIAL_BLOCK	0x800	// initial block of multichannel segment
#define FINAL_BLOCK	0x1000	// final block of multichannel segment

#define SHIFT_LSB	13
#define SHIFT_MASK	(0x1fL << SHIFT_LSB)

#define MAG_LSB		18
#define MAG_MASK	(0x1fL << MAG_LSB)

#define SRATE_LSB	23
#define SRATE_MASK	(0xfL << SRATE_LSB)

#define FALSE_STEREO    0x40000000      // block is stereo, but data is mono

#define IGNORED_FLAGS   0x18000000      // reserved, but ignore if encountered
#define NEW_SHAPING     0x20000000      // use IIR filter for negative shaping
#define UNKNOWN_FLAGS   0x80000000      // also reserved, but refuse decode if
                                        //  encountered

#define MONO_DATA (MONO_FLAG | FALSE_STEREO)

#define MIN_STREAM_VERS     0x402       // lowest stream version we'll decode
#define MAX_STREAM_VERS     0x410       // highest stream version we'll decode or encode

static const uint32_t sample_rates [] = { 6000, 8000, 9600, 11025, 12000, 16000, 22050,
    24000, 32000, 44100, 48000, 64000, 88200, 96000, 192000 };

//////////////////////////// WavPack Metadata /////////////////////////////////

// This is an internal representation of metadata.

typedef struct {
    int32_t byte_length;
    void *data;
    unsigned char id;
} WavpackMetadata;

#define ID_UNIQUE               0x3f
#define ID_OPTIONAL_DATA        0x20
#define ID_ODD_SIZE             0x40
#define ID_LARGE                0x80

#define ID_DUMMY                0x0
#define ID_ENCODER_INFO         0x1
#define ID_DECORR_TERMS         0x2
#define ID_DECORR_WEIGHTS       0x3
#define ID_DECORR_SAMPLES       0x4
#define ID_ENTROPY_VARS         0x5
#define ID_HYBRID_PROFILE       0x6
#define ID_SHAPING_WEIGHTS      0x7
#define ID_FLOAT_INFO           0x8
#define ID_INT32_INFO           0x9
#define ID_WV_BITSTREAM         0xa
#define ID_WVC_BITSTREAM        0xb
#define ID_WVX_BITSTREAM        0xc
#define ID_CHANNEL_INFO         0xd

#define ID_RIFF_HEADER          (ID_OPTIONAL_DATA | 0x1)
#define ID_RIFF_TRAILER         (ID_OPTIONAL_DATA | 0x2)
#define ID_ALT_HEADER           (ID_OPTIONAL_DATA | 0x3)
#define ID_ALT_TRAILER          (ID_OPTIONAL_DATA | 0x4)
#define ID_CONFIG_BLOCK         (ID_OPTIONAL_DATA | 0x5)
#define ID_MD5_CHECKSUM         (ID_OPTIONAL_DATA | 0x6)
#define ID_SAMPLE_RATE          (ID_OPTIONAL_DATA | 0x7)
#define ID_ALT_EXTENSION        (ID_OPTIONAL_DATA | 0x8)
#define ID_ALT_MD5_CHECKSUM     (ID_OPTIONAL_DATA | 0x9)

static const char *metadata_names [] = {
    "DUMMY", "ENCODER_INFO", "DECORR_TERMS", "DECORR_WEIGHTS", "DECORR_SAMPLES", "ENTROPY_VARS", "HYBRID_PROFILE", "SHAPING_WEIGHTS",
    "FLOAT_INFO", "INT32_INFO", "WV_BITSTREAM", "WVC_BITSTREAM", "WVX_BITSTREAM", "CHANNEL_INFO", "UNASSIGNED", "UNASSIGNED",
    "UNASSIGNED", "RIFF_HEADER", "RIFF_TRAILER", "ALT_HEADER", "ALT_TRAILER", "CONFIG_BLOCK", "MD5_CHECKSUM", "SAMPLE_RATE",
    "ALT_EXTENSION", "ALT_MD5_CHECKSUM", "UNASSIGNED", "UNASSIGNED", "UNASSIGNED", "UNASSIGNED", "UNASSIGNED", "UNASSIGNED"
};

static int32_t read_bytes (void *buff, int32_t bcount);
static uint32_t read_next_header (read_stream infile, WavpackHeader *wphdr);
static void little_endian_to_native (void *data, char *format);
static void parse_wavpack_block (unsigned char *block_data);

static const char *sign_on = "\n"
" WVPARSER  WavPack Audio File Parser Test Filter  Version 1.00\n"
" Copyright (c) 1998 - 2016 David Bryant.  All Rights Reserved.\n\n";

int main ()
{
    uint32_t bcount, total_bytes, sample_rate, first_sample, last_sample = -1L;
    int channel_count, block_count;
    char flags_list [256];
    WavpackHeader wphdr;

#ifdef _WIN32
    setmode (fileno (stdin), O_BINARY);
#endif
    fprintf (stderr, "%s", sign_on);

    while (1) {

	// read next WavPack header

	bcount = read_next_header (read_bytes, &wphdr);

	if (bcount == (uint32_t) -1) {
	    printf ("end of file\n\n");
	    break;
	}

	if (bcount)
	    printf ("unknown data skipped, %d bytes\n", bcount);

        if (((wphdr.flags & SRATE_MASK) >> SRATE_LSB) == 15) {
            if (sample_rate != 44100)
                printf ("warning: unknown sample rate...using 44100 default\n");
            sample_rate = 44100;
        }
        else
            sample_rate = sample_rates [(wphdr.flags & SRATE_MASK) >> SRATE_LSB];

        // basic summary of the block

        if (wphdr.flags & INITIAL_BLOCK)
            printf ("\n");

	if (wphdr.block_samples) {
	    printf ("%s audio block, %d samples in %d bytes, time = %.2f-%.2f\n",
                (wphdr.flags & MONO_FLAG) ? "mono" : "stereo", wphdr.block_samples, wphdr.ckSize + 8,
                (double) wphdr.block_index / sample_rate, (double) (wphdr.block_index + wphdr.block_samples - 1) / sample_rate);

            // now show information from the "flags" field of the header

            printf ("samples are %d bits in %d bytes, shifted %d bits, sample rate = %d\n",
                (int)((wphdr.flags & MAG_MASK) >> MAG_LSB) + 1,
                (wphdr.flags & BYTES_STORED) + 1,
                (int)(wphdr.flags & SHIFT_MASK) >> SHIFT_LSB,
                sample_rate);

            flags_list [0] = 0;

            if (wphdr.flags) {
                if (wphdr.flags & INITIAL_BLOCK) strcat (flags_list, "INITIAL ");
                if (wphdr.flags & MONO_FLAG) strcat (flags_list, "MONO ");
                if (wphdr.flags & HYBRID_FLAG) strcat (flags_list, "HYBRID ");
                if (wphdr.flags & JOINT_STEREO) strcat (flags_list, "JOINT-STEREO ");
                if (wphdr.flags & CROSS_DECORR) strcat (flags_list, "CROSS-DECORR ");
                if (wphdr.flags & HYBRID_SHAPE) strcat (flags_list, "NOISE-SHAPING ");
                if (wphdr.flags & FLOAT_DATA) strcat (flags_list, "FLOAT ");
                if (wphdr.flags & INT32_DATA) strcat (flags_list, "INT32 ");
                if (wphdr.flags & HYBRID_BITRATE) strcat (flags_list, "HYBRID-BITRATE ");
                if (wphdr.flags & FALSE_STEREO) strcat (flags_list, "FALSE-STEREO ");
                if (wphdr.flags & NEW_SHAPING) strcat (flags_list, "NEW-SHAPING ");
                if (wphdr.flags & (IGNORED_FLAGS | UNKNOWN_FLAGS)) strcat (flags_list, "UNKNOWN-FLAGS ");
                if (wphdr.flags & FINAL_BLOCK) strcat (flags_list, "FINAL");
            }
            else
                strcat (flags_list, "none");

            printf ("flags: %s\n", flags_list);
        }
        else
            printf ("non-audio block of %d bytes\n", wphdr.ckSize + 8);

	// read and parse the actual block data (which is entirely composed of "meta" blocks)

	if (wphdr.ckSize > sizeof (WavpackHeader) - 8) {
	    uint32_t block_size = wphdr.ckSize + 8;
	    char *block_buff = malloc (block_size);

            memcpy (block_buff, &wphdr, sizeof (WavpackHeader));
	    read_bytes (block_buff + sizeof (WavpackHeader), block_size - sizeof (WavpackHeader));
            parse_wavpack_block (block_buff);
	    free (block_buff);
	}

	// if there's audio samples in there do some other sanity checks (especially for multichannel)

	if (wphdr.block_samples) {
	    if ((wphdr.flags & INITIAL_BLOCK) && wphdr.block_index != last_sample + 1)
		printf ("error: discontinuity detected!\n");

	    if (!(wphdr.flags & INITIAL_BLOCK))
		if (first_sample != wphdr.block_index ||
		    last_sample != wphdr.block_index + wphdr.block_samples - 1)
			printf ("error: multichannel block mismatch detected!\n");

	    last_sample = (first_sample = wphdr.block_index) + wphdr.block_samples - 1;

	    if (wphdr.flags & INITIAL_BLOCK) {
		channel_count = (wphdr.flags & MONO_FLAG) ? 1 : 2;
		total_bytes = wphdr.ckSize + 8;
		block_count = 1;
	    }
	    else {
		channel_count += (wphdr.flags & MONO_FLAG) ? 1 : 2;
		total_bytes += wphdr.ckSize + 8;
		block_count++;

		if (wphdr.flags & FINAL_BLOCK)
		    printf ("multichannel: %d channels in %d blocks, %d bytes total\n",
			channel_count, block_count, total_bytes);
	    }
	}
    }

    return 0;
}

// read the next metadata block, or return 0 if there aren't any more (or an error occurs)

static int read_metadata_buff (WavpackMetadata *wpmd, unsigned char *blockbuff, unsigned char **buffptr)
{
    WavpackHeader *wphdr = (WavpackHeader *) blockbuff;
    unsigned char *buffend = blockbuff + wphdr->ckSize + 8;

    if (buffend - *buffptr < 2)
        return 0;

    wpmd->id = *(*buffptr)++;
    wpmd->byte_length = *(*buffptr)++ << 1;

    if (wpmd->id & ID_LARGE) {
        wpmd->id &= ~ID_LARGE;

        if (buffend - *buffptr < 2)
            return 0;

        wpmd->byte_length += *(*buffptr)++ << 9;
        wpmd->byte_length += *(*buffptr)++ << 17;
    }

    if (wpmd->id & ID_ODD_SIZE) {
        if (!wpmd->byte_length)         // odd size and zero length makes no sense
            return 0;
        wpmd->id &= ~ID_ODD_SIZE;
        wpmd->byte_length--;
    }

    if (wpmd->byte_length) {
        if (buffend - *buffptr < wpmd->byte_length + (wpmd->byte_length & 1)) {
            wpmd->data = NULL;
            return 0;
        }

        wpmd->data = *buffptr;
        (*buffptr) += wpmd->byte_length + (wpmd->byte_length & 1);
    }
    else
        wpmd->data = NULL;

    return 1;
}

// given a pointer to a WavPack block, parse all the "meta" blocks and display something about them

static void parse_wavpack_block (unsigned char *block_data)
{
    unsigned char *blockptr = block_data + sizeof (WavpackHeader);
    WavpackHeader *wphdr = (WavpackHeader *) block_data;
    int metadata_count = 0;
    WavpackMetadata wpmd;

    while (read_metadata_buff (&wpmd, block_data, &blockptr)) {
        metadata_count++;
        if (wpmd.id & 0x10)
            printf ("  metadata: ID = 0x%02x (UNASSIGNED), size = %d bytes\n", wpmd.id, wpmd.byte_length);
        else if (wpmd.id & 0x20)
            printf ("  metadata: ID = 0x%02x (%s), size = %d bytes\n", wpmd.id, metadata_names [wpmd.id - 0x10], wpmd.byte_length);
        else
            printf ("  metadata: ID = 0x%02x (%s), size = %d bytes\n", wpmd.id, metadata_names [wpmd.id], wpmd.byte_length);
    }

    if (blockptr != block_data + wphdr->ckSize + 8)
        printf ("error: garbage at end of WavPack block\n");
}

static int32_t read_bytes (void *buff, int32_t bcount)
{
    return fread (buff, 1, bcount, stdin);
}

// Read from current file position until a valid 32-byte WavPack 4.0 header is
// found and read into the specified pointer. The number of bytes skipped is
// returned. If no WavPack header is found within 1 meg, then a -1 is returned
// to indicate the error. No additional bytes are read past the header and it
// is returned in the processor's native endian mode. Seeking is not required.

static uint32_t read_next_header (read_stream infile, WavpackHeader *wphdr)
{
    unsigned char buffer [sizeof (*wphdr)], *sp = buffer + sizeof (*wphdr), *ep = sp;
    uint32_t bytes_skipped = 0;
    int bleft;

    while (1) {
	if (sp < ep) {
	    bleft = ep - sp;
	    memmove (buffer, sp, bleft);
	}
	else
	    bleft = 0;

	if (infile (buffer + bleft, sizeof (*wphdr) - bleft) != (int32_t) sizeof (*wphdr) - bleft)
	    return -1;

	sp = buffer;

	if (*sp++ == 'w' && *sp == 'v' && *++sp == 'p' && *++sp == 'k' &&
            !(*++sp & 1) && sp [2] < 16 && !sp [3] && (sp [2] || sp [1] || *sp >= 24) && sp [5] == 4 &&
            sp [4] >= (MIN_STREAM_VERS & 0xff) && sp [4] <= (MAX_STREAM_VERS & 0xff) && sp [18] < 3 && !sp [19]) {
		memcpy (wphdr, buffer, sizeof (*wphdr));
		little_endian_to_native (wphdr, WavpackHeaderFormat);
		return bytes_skipped;
	    }

        // printf ("read_next_header() did not see valid block right away: %c %c %c %c\n", buffer [0], buffer [1], buffer [2], buffer [3]);

	while (sp < ep && *sp != 'w')
	    sp++;

	if ((bytes_skipped += sp - buffer) > 1024 * 1024)
	    return -1;
    }
}

// using the specified format string, convert the referenced structure from little-endian to native

static void little_endian_to_native (void *data, char *format)
{
    unsigned char *cp = (unsigned char *) data;
    int32_t temp;

    while (*format) {
	switch (*format) {
	    case 'L':
		temp = cp [0] + ((int32_t) cp [1] << 8) + ((int32_t) cp [2] << 16) + ((int32_t) cp [3] << 24);
		* (int32_t *) cp = temp;
		cp += 4;
		break;

	    case 'S':
		temp = cp [0] + (cp [1] << 8);
		* (int16_t *) cp = (int16_t) temp;
		cp += 2;
		break;

	    default:
		if (isdigit (*format))
		    cp += *format - '0';

		break;
	}

	format++;
    }
}
