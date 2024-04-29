////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//                Copyright (c) 1998 - 2024 David Bryant.                 //
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
#include <ctype.h>
#ifdef _WIN32
#include <io.h>
#endif

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
#define NEW_SHAPING     0x20000000      // use IIR filter for negative shaping

#define MONO_DATA (MONO_FLAG | FALSE_STEREO)

// Introduced in WavPack 5.0:
#define HAS_CHECKSUM    0x10000000      // block contains a trailing checksum
#define DSD_FLAG        0x80000000      // block is encoded DSD (1-bit PCM)

#define IGNORED_FLAGS   0x08000000      // reserved, but ignore if encountered
#define UNKNOWN_FLAGS   0x00000000      // we no longer have any of these spares

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

static const char *metadata_names [] = {
    "DUMMY", "ENCODER_INFO", "DECORR_TERMS", "DECORR_WEIGHTS", "DECORR_SAMPLES", "ENTROPY_VARS", "HYBRID_PROFILE", "SHAPING_WEIGHTS",
    "FLOAT_INFO", "INT32_INFO", "WV_BITSTREAM", "WVC_BITSTREAM", "WVX_BITSTREAM", "CHANNEL_INFO", "DSD_BLOCK", "UNASSIGNED",
    "UNASSIGNED", "RIFF_HEADER", "RIFF_TRAILER", "ALT_HEADER", "ALT_TRAILER", "CONFIG_BLOCK", "MD5_CHECKSUM", "SAMPLE_RATE",
    "ALT_EXTENSION", "ALT_MD5_CHECKSUM", "NEW_CONFIG", "CHANNEL_IDENTITIES", "UNASSIGNED", "UNASSIGNED", "UNASSIGNED", "BLOCK_CHECKSUM"
};

static int32_t read_bytes (void *buff, int32_t bcount);
static uint32_t read_next_header (read_stream infile, WavpackHeader *wphdr);
static void little_endian_to_native (void *data, char *format);
static void native_to_little_endian (void *data, char *format);
static void parse_wavpack_block (unsigned char *block_data);
static int verify_wavpack_block (unsigned char *buffer);

static const char *sign_on = "\n"
" WVPARSER  WavPack Audio File Parser Test Filter  Version 1.20\n"
" Copyright (c) 1998 - 2024 David Bryant.  All Rights Reserved.\n\n";

static const char *usage =
" Usage:     WVPARSER [-options] < infile.wv [> outfile.txt]\n\n"
" Operation: WavPack file at stdin is parsed and displayed to stdout\n\n"
" Options:  -h     = display this help message and exit\n"
"           -v0    = show basic frame information only\n"
"           -v1    = also list metadata blocks found (default)\n"
"           -v2    = also display up to 16 bytes of each metadata block\n\n"
" Web:      Visit www.github.com/dbry/WavPack for latest version and info\n\n";

static int verbosity = 1;

int main (int argc, char **argv)
{
    uint32_t bcount, total_bytes, sample_rate, first_sample, last_sample = -1L;
    int channel_count, block_count, asked_help = 0;
    char flags_list [256];
    WavpackHeader wphdr;

#ifdef _WIN32
    setmode (_fileno (stdin), O_BINARY);
#endif
    fprintf (stderr, "%s", sign_on);

    // loop through command-line arguments

    while (--argc) {
#if defined (_WIN32)
        if ((**++argv == '-' || **argv == '/') && (*argv)[1])
#else
        if ((**++argv == '-') && (*argv)[1])
#endif
            while (*++*argv)
                switch (**argv) {

                    case 'H': case 'h':
                        asked_help = 1;
                        break;

                    case 'V': case 'v':
                        verbosity = strtol (++*argv, argv, 10);

                        if (verbosity < 0 || verbosity > 2) {
                            fprintf (stderr, "\nverbosity  must be 0, 1, or 2!\n");
                            return -1;
                        }

                        --*argv;
                        break;

                    default:
                        fprintf (stderr, "\nillegal option: %c !\n", **argv);
                        return 1;
                }
        else {
            fprintf (stderr, "\nextra unknown argument: %s !\n", *argv);
            return 1;
        }
    }

    if (asked_help) {
        printf ("%s", usage);
        return 0;
    }

    while (1) {

	// read next WavPack header

	bcount = read_next_header (read_bytes, &wphdr);

	if (bcount == (uint32_t) -1) {
	    printf ("\nend of file\n\n");
	    break;
	}

	if (bcount)
	    printf ("\nunknown data skipped, %u bytes\n", bcount);

        if (((wphdr.flags & SRATE_MASK) >> SRATE_LSB) == 15) {
            if (sample_rate != 44100)
                printf ("\nwarning: unknown sample rate...using 44100 default\n");
            sample_rate = 44100;
        }
        else {
            sample_rate = sample_rates [(wphdr.flags & SRATE_MASK) >> SRATE_LSB];

            if (wphdr.flags & DSD_FLAG)
                sample_rate *= 4;       // most common multiplier (DSD64), but could be wrong
        }

        // basic summary of the block

        if ((wphdr.flags & INITIAL_BLOCK) || !wphdr.block_samples)
            printf ("\n");

	if (wphdr.block_samples) {
	    printf ("%s audio block, version 0x%03x, %u samples in %u bytes, time = %.2f-%.2f\n",
                (wphdr.flags & MONO_FLAG) ? "mono" : "stereo", wphdr.version, wphdr.block_samples, wphdr.ckSize + 8,
                (double) wphdr.block_index / sample_rate, (double) (wphdr.block_index + wphdr.block_samples - 1) / sample_rate);

            // now show information from the "flags" field of the header

            printf ("samples are %d bits in %d bytes, shifted %d bits, sample rate = %u Hz\n",
                (int)((wphdr.flags & MAG_MASK) >> MAG_LSB) + 1,
                (wphdr.flags & BYTES_STORED) + 1,
                (int)(wphdr.flags & SHIFT_MASK) >> SHIFT_LSB,
                sample_rate);

            flags_list [0] = 0;

            if (wphdr.flags) {
                if (wphdr.flags & INITIAL_BLOCK) strcat (flags_list, "INITIAL ");
                if (wphdr.flags & MONO_FLAG) strcat (flags_list, "MONO ");
                if (wphdr.flags & DSD_FLAG) strcat (flags_list, "DSD ");
                if (wphdr.flags & HYBRID_FLAG) strcat (flags_list, "HYBRID ");
                if (wphdr.flags & JOINT_STEREO) strcat (flags_list, "JOINT-STEREO ");
                if (wphdr.flags & CROSS_DECORR) strcat (flags_list, "CROSS-DECORR ");
                if (wphdr.flags & HYBRID_SHAPE) strcat (flags_list, "NOISE-SHAPING ");
                if (wphdr.flags & FLOAT_DATA) strcat (flags_list, "FLOAT ");
                if (wphdr.flags & INT32_DATA) strcat (flags_list, "INT32 ");
                if (wphdr.flags & HYBRID_BITRATE) strcat (flags_list, "HYBRID-BITRATE ");
                if (wphdr.flags & FALSE_STEREO) strcat (flags_list, "FALSE-STEREO ");
                if (wphdr.flags & NEW_SHAPING) strcat (flags_list, "NEW-SHAPING ");
                if (wphdr.flags & HAS_CHECKSUM) strcat (flags_list, "CHECKSUM ");
                if (wphdr.flags & (IGNORED_FLAGS | UNKNOWN_FLAGS)) strcat (flags_list, "UNKNOWN-FLAGS ");
                if (wphdr.flags & FINAL_BLOCK) strcat (flags_list, "FINAL");
            }
            else
                strcat (flags_list, "none");

            printf ("flags: %s\n", flags_list);
        }
        else
            printf ("non-audio block of %u bytes, version 0x%03x\n", wphdr.ckSize + 8, wphdr.version);

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
		    printf ("multichannel: %d channels in %d blocks, %u bytes total\n",
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

        if (verbosity >= 1) {
            if (wpmd.id & 0x10)
                printf ("  metadata: ID = 0x%02x (UNASSIGNED), size = %d bytes\n", wpmd.id, wpmd.byte_length);
            else if (wpmd.id & 0x20)
                printf ("  metadata: ID = 0x%02x (%s), size = %d bytes\n", wpmd.id, metadata_names [wpmd.id - 0x10], wpmd.byte_length);
            else
                printf ("  metadata: ID = 0x%02x (%s), size = %d bytes\n", wpmd.id, metadata_names [wpmd.id], wpmd.byte_length);

            if (verbosity >= 2 && wpmd.data && wpmd.byte_length) {
                int i;
                printf ("   0x0:");
                for (i = 0; i < 16 && i < wpmd.byte_length; ++i)
                    printf (" %02x", ((unsigned char *) wpmd.data) [i]);

                if (wpmd.byte_length > i) printf (" ...\n"); else printf ("\n");
            }
        }
    }

    if (blockptr != block_data + wphdr->ckSize + 8)
        printf ("error: garbage at end of WavPack block\n");

    if (!verify_wavpack_block (block_data))
        printf ("error: checksum failure on WavPack block\n");
}

// Quickly verify the referenced block. It is assumed that the WavPack header has been converted
// to native endian format. If a block checksum is performed, that is done in little-endian
// (file) format. It is also assumed that the caller has made sure that the block length
// indicated in the header is correct (we won't overflow the buffer). If a checksum is present,
// then it is checked, otherwise we just check that all the metadata blocks are formatted
// correctly (without looking at their contents). Returns FALSE for bad block.

#define ID_BLOCK_CHECKSUM       (ID_OPTIONAL_DATA | 0xf)

static int verify_wavpack_block (unsigned char *buffer)
{
    WavpackHeader *wphdr = (WavpackHeader *) buffer;
    uint32_t checksum_passed = 0, bcount, meta_bc;
    unsigned char *dp, meta_id, c1, c2;

    if (strncmp (wphdr->ckID, "wvpk", 4) || wphdr->ckSize + 8 < sizeof (WavpackHeader))
        return 0;

    bcount = wphdr->ckSize - sizeof (WavpackHeader) + 8;
    dp = (unsigned char *)(wphdr + 1);

    while (bcount >= 2) {
        meta_id = *dp++;
        c1 = *dp++;

        meta_bc = c1 << 1;
        bcount -= 2;

        if (meta_id & ID_LARGE) {
            if (bcount < 2)
                return 0;

            c1 = *dp++;
            c2 = *dp++;
            meta_bc += ((uint32_t) c1 << 9) + ((uint32_t) c2 << 17);
            bcount -= 2;
        }

        if (bcount < meta_bc)
            return 0;

        if ((meta_id & ID_UNIQUE) == ID_BLOCK_CHECKSUM) {
            unsigned char *csptr = buffer;

            int wcount = (int)(dp - 2 - buffer) >> 1;
            uint32_t csum = (uint32_t) -1;

            if ((meta_id & ID_ODD_SIZE) || meta_bc < 2 || meta_bc > 4)
                return 0;

            native_to_little_endian ((WavpackHeader *) buffer, WavpackHeaderFormat);

            while (wcount--) {
                csum = (csum * 3) + csptr [0] + (csptr [1] << 8);
                csptr += 2;
            }

            little_endian_to_native ((WavpackHeader *) buffer, WavpackHeaderFormat);

            if (meta_bc == 4) {
                if (*dp++ != (csum & 0xff) || *dp++ != ((csum >> 8) & 0xff) || *dp++ != ((csum >> 16) & 0xff) || *dp++ != ((csum >> 24) & 0xff))
                    return 0;
            }
            else {
                csum ^= csum >> 16;

                if (*dp++ != (csum & 0xff) || *dp++ != ((csum >> 8) & 0xff))
                    return 0;
            }

            checksum_passed++;
        }

        bcount -= meta_bc;
        dp += meta_bc;
    }

    return (bcount == 0) && (!(wphdr->flags & HAS_CHECKSUM) || checksum_passed);
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

static void native_to_little_endian (void *data, char *format)
{
    unsigned char *cp = (unsigned char *) data;
    int32_t temp;

    while (*format) {
        switch (*format) {
            case 'L':
                temp = * (int32_t *) cp;
                *cp++ = (unsigned char) temp;
                *cp++ = (unsigned char) (temp >> 8);
                *cp++ = (unsigned char) (temp >> 16);
                *cp++ = (unsigned char) (temp >> 24);
                break;

            case 'S':
                temp = * (int16_t *) cp;
                *cp++ = (unsigned char) temp;
                *cp++ = (unsigned char) (temp >> 8);
                break;

            default:
                if (isdigit (*format))
                    cp += *format - '0';

                break;
        }

        format++;
    }
}
