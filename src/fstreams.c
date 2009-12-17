////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//              Copyright (c) 1998 - 2006 Conifer Software.               //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// fstreams.c

// WavPack input files can be opened as standard "C" streams using a
// provided filename.

#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

#if defined (WIN32) || defined (__OS2__)
#include <io.h>
#endif

#include "wavpack_local.h"

#ifdef WIN32
#define stricmp(x,y) _stricmp(x,y)
#define fileno _fileno
#define stat64 __stat64
#define fstat64 _fstat64
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
#endif

///////////////////////////// executable code ////////////////////////////////

// This code provides an interface between the reader callback mechanism that
// WavPack uses internally and the standard fstream C library.

static int32_t read_bytes (void *id, void *data, int32_t bcount)
{
    return (int32_t) fread (data, 1, bcount, (FILE*) id);
}

static int64_t get_pos (void *id)
{
#ifdef WIN32
    return _ftelli64 ((FILE*) id);
#else
    return ftell ((FILE*) id);
#endif
}

static int set_pos_abs (void *id, int64_t pos)
{
#ifdef WIN32
    return _fseeki64 (id, pos, SEEK_SET);
#else
    return fseek (id, pos, SEEK_SET);
#endif
}

static int set_pos_rel (void *id, int64_t delta, int mode)
{
#ifdef WIN32
    return _fseeki64 (id, delta, mode);
#else
    return fseek (id, delta, mode);
#endif
}

static int push_back_byte (void *id, int c)
{
    return ungetc (c, id);
}

#ifdef WIN32

static int64_t get_length (void *id)
{
    FILE *file = id;
    struct stat64 statbuf;

    if (!file || fstat64 (fileno (file), &statbuf) || !(statbuf.st_mode & S_IFREG))
        return 0;

    return statbuf.st_size;
}

#else

static int64_t get_length (void *id)
{
    FILE *file = id;
    struct stat statbuf;

    if (!file || fstat (fileno (file), &statbuf) || !(statbuf.st_mode & S_IFREG))
        return 0;

    return statbuf.st_size;
}

#endif

static int can_seek (void *id)
{
    FILE *file = id;
    struct stat statbuf;

    return file && !fstat (fileno (file), &statbuf) && (statbuf.st_mode & S_IFREG);
}

static int close_file (void *id)
{
    return fclose ((FILE*) id);
}

static int32_t write_bytes (void *id, void *data, int32_t bcount)
{
    return (int32_t) fwrite (data, 1, bcount, (FILE*) id);
}

static WavpackStreamReader64 freader = {
    read_bytes, get_pos, set_pos_abs, set_pos_rel, push_back_byte, get_length, can_seek, close_file,
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
        strcpy (error, (flags & OPEN_EDIT_TAGS) ? "can't open file for editing" : "can't open file");
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

    return WavpackOpenFileInputEx64 (&freader, wv_id, wvc_id, error, flags, norm_offset);
}

