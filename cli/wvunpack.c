////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//              Copyright (c) 1998 - 2006 Conifer Software.               //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// wvunpack.c

// This is the main module for the WavPack command-line decompressor.

#if defined(WIN32)
#include <windows.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <sys/param.h>
#include <locale.h>
#include <iconv.h>
#if defined (__GNUC__)
#include <unistd.h>
#include <glob.h>
#endif
#endif

#if defined(__GNUC__) && !defined(WIN32)
#include <sys/time.h>
#else
#include <sys/timeb.h>
#endif

#include <math.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>

#include "wavpack.h"
#include "utils.h"
#include "md5.h"

#ifdef WIN32
#define fileno _fileno
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

///////////////////////////// local variable storage //////////////////////////

static const char *sign_on = "\n"
" WVUNPACK  Hybrid Lossless Audio Decompressor  %s Version %s\n"
" Copyright (c) 1998 - 2006 Conifer Software.  All Rights Reserved.\n\n";

static const char *usage =
#if defined (WIN32)
" Usage:   WVUNPACK [-options] [@]infile[.wv]|- [[@]outfile[.wav]|outpath|-]\n"
#else
" Usage:   WVUNPACK [-options] [@]infile[.wv]|- [...] [-o [@]outfile[.wav]|outpath|-]\n"
#endif
"             (infile may contain wildcards: ?,*)\n\n"
" Options: -b  = blindly decode all stream blocks & ignore length info\n"
"          -c  = extract cuesheet only to stdout (no audio decode)\n"
"          -cc = extract cuesheet file (.cue) in addition to audio file\n"
"          -d  = delete source file if successful (use with caution!)\n"
"          -i  = ignore .wvc file (forces hybrid lossy decompression)\n"
#if defined (WIN32)
"          -l  = run at low priority (for smoother multitasking)\n"
#endif
"          -m  = calculate and display MD5 signature; verify if lossless\n"
"          -q  = quiet (keep console output to a minimum)\n"
#if !defined (WIN32)
"          -o FILENAME | PATH = specify output filename or path\n"
#endif
"          -r  = force raw audio decode (results in .raw extension)\n"
"          -s  = display summary information only to stdout (no audio decode)\n"
"          -ss = display super summary (including tags) to stdout (no decode)\n"
"          -t  = copy input file's time stamp to output file(s)\n"
"          -v  = verify source data only (no output file created)\n"
"          -w  = regenerate .wav header (ignore RIFF data in file)\n"
"          -y  = yes to overwrite warning (use with caution!)\n\n"
" Web:     Visit www.wavpack.com for latest version and info\n";

// this global is used to indicate the special "debug" mode where extra debug messages
// are displayed and all messages are logged to the file \wavpack.log

int debug_logging_mode;

static char overwrite_all, delete_source, raw_decode, extract_cuesheet,
    summary, ignore_wvc, quiet_mode, calc_md5, copy_time, blind_decode, wav_decode;

static int num_files, file_index, outbuf_k;

/////////////////////////// local function declarations ///////////////////////

static int unpack_file (char *infilename, char *outfilename);
static void display_progress (double file_progress);

#define NO_ERROR 0L
#define SOFT_ERROR 1
#define HARD_ERROR 2

//////////////////////////////////////////////////////////////////////////////
// The "main" function for the command-line WavPack decompressor.           //
//////////////////////////////////////////////////////////////////////////////

int main (argc, argv) int argc; char **argv;
{
    int verify_only = 0, error_count = 0, add_extension = 0, output_spec = 0;
    char outpath, **matches = NULL, *outfilename = NULL;
    int result;

#if defined(WIN32)
    struct _finddata_t _finddata_t;
    char selfname [MAX_PATH];

    if (GetModuleFileName (NULL, selfname, sizeof (selfname)) && filespec_name (selfname) &&
	_strupr (filespec_name (selfname)) && strstr (filespec_name (selfname), "DEBUG")) {
	    char **argv_t = argv;
	    int argc_t = argc;

	    debug_logging_mode = TRUE;

	    while (--argc_t)
		error_line ("arg %d: %s", argc - argc_t, *++argv_t);
    }
#else
    if (filespec_name (*argv))
	if (strstr (filespec_name (*argv), "ebug") || strstr (filespec_name (*argv), "DEBUG")) {
	    char **argv_t = argv;
	    int argc_t = argc;

	    debug_logging_mode = TRUE;

	    while (--argc_t)
		error_line ("arg %d: %s", argc - argc_t, *++argv_t);
    }
#endif

    // loop through command-line arguments

    while (--argc) {
#if defined (WIN32)
	if ((**++argv == '-' || **argv == '/') && (*argv)[1])
#else
	if ((**++argv == '-') && (*argv)[1])
#endif
	    while (*++*argv)
		switch (**argv) {
		    case 'Y': case 'y':
			overwrite_all = 1;
			break;

		    case 'C': case 'c':
			++extract_cuesheet;
			break;

		    case 'D': case 'd':
			delete_source = 1;
			break;

#if defined (WIN32)
		    case 'L': case 'l':
			SetPriorityClass (GetCurrentProcess(), IDLE_PRIORITY_CLASS);
			break;
#else
		    case 'O': case 'o':
			output_spec = 1;
			break;
#endif
		    case 'T': case 't':
			copy_time = 1;
			break;

		    case 'V': case 'v':
			verify_only = 1;
			break;

		    case 'S': case 's':
			++summary;
			break;

		    case 'K': case 'k':
			outbuf_k = strtol (++*argv, argv, 10);
			--*argv;
			break;

		    case 'M': case 'm':
			calc_md5 = 1;
			break;

		    case 'B': case 'b':
			blind_decode = 1;
			break;

		    case 'R': case 'r':
			raw_decode = 1;
			break;

		    case 'W': case 'w':
			wav_decode = 1;
			break;

		    case 'Q': case 'q':
			quiet_mode = 1;
			break;

		    case 'I': case 'i':
			ignore_wvc = 1;
			break;

		    default:
			error_line ("illegal option: %c !", **argv);
			++error_count;
		}
	else {
#if defined (WIN32)
	    if (!num_files) {
		matches = realloc (matches, (num_files + 1) * sizeof (*matches));
		matches [num_files] = malloc (strlen (*argv) + 10);
		strcpy (matches [num_files], *argv);

		if (*(matches [num_files]) != '-' && *(matches [num_files]) != '@' &&
		    !filespec_ext (matches [num_files]))
			strcat (matches [num_files], ".wv");

		num_files++;
	    }
	    else if (!outfilename) {
		outfilename = malloc (strlen (*argv) + PATH_MAX);
		strcpy (outfilename, *argv);
	    }
	    else {
		error_line ("extra unknown argument: %s !", *argv);
		++error_count;
	    }
#else
            if (output_spec) {
		outfilename = malloc (strlen (*argv) + PATH_MAX);
		strcpy (outfilename, *argv);
                output_spec = 0;
            }
            else {
		matches = realloc (matches, (num_files + 1) * sizeof (*matches));
		matches [num_files] = malloc (strlen (*argv) + 10);
		strcpy (matches [num_files], *argv);

		if (*(matches [num_files]) != '-' && *(matches [num_files]) != '@' &&
		    !filespec_ext (matches [num_files]))
			strcat (matches [num_files], ".wv");

		num_files++;
            }
#endif
	}
    }

   // check for various command-line argument problems

    if (verify_only && delete_source) {
	error_line ("can't delete in verify mode!");
	delete_source = 0;
    }

    if (raw_decode && wav_decode) {
	error_line ("-r (raw decode) and -w (wav header) modes are incompatible!");
	++error_count;
    }

    if (verify_only && outfilename) {
	error_line ("outfile specification and verify mode are incompatible!");
	++error_count;
    }

    if (extract_cuesheet > 1 && outfilename && *outfilename == '-') {
	error_line ("can't extract cuesheet when unpacking to stdout!");
	++error_count;
    }

    if (!quiet_mode && !error_count)
	fprintf (stderr, sign_on, VERSION_OS, WavpackGetLibraryVersionString ());

    if (!num_files) {
	printf ("%s", usage);
	return 1;
    }

    if (error_count)
	return 1;

    setup_break ();

    for (file_index = 0; file_index < num_files; ++file_index) {
	char *infilename = matches [file_index];

	// If the single infile specification begins with a '@', then it
	// actually points to a file that contains the names of the files
	// to be converted. This was included for use by Wim Speekenbrink's
	// frontends, but could be used for other purposes.

	if (*infilename == '@') {
	    FILE *list = fopen (infilename+1, "rt");
	    int di, c;

	    for (di = file_index; di < num_files - 1; di++)
		matches [di] = matches [di + 1];

	    file_index--;
	    num_files--;

	    if (list == NULL) {
		error_line ("file %s not found!", infilename+1);
		free (infilename);
		return 1;
	    }

	    while ((c = getc (list)) != EOF) {

		while (c == '\n')
		    c = getc (list);

		if (c != EOF) {
		    char *fname = malloc (PATH_MAX);
		    int ci = 0;

		    do
			fname [ci++] = c;
		    while ((c = getc (list)) != '\n' && c != EOF && ci < PATH_MAX);

		    fname [ci++] = '\0';
		    fname = realloc (fname, ci);
		    matches = realloc (matches, ++num_files * sizeof (*matches));

		    for (di = num_files - 1; di > file_index + 1; di--)
			matches [di] = matches [di - 1];

		    matches [++file_index] = fname;
		}
	    }

	    fclose (list);
	    free (infilename);
	}
#if defined (WIN32)
	else if (filespec_wild (infilename)) {
	    FILE *list = fopen (infilename+1, "rt");
	    intptr_t file;
	    int di;

	    for (di = file_index; di < num_files - 1; di++)
		matches [di] = matches [di + 1];

	    file_index--;
	    num_files--;

	    if ((file = _findfirst (infilename, &_finddata_t)) != (intptr_t) -1) {
		do {
		    if (!(_finddata_t.attrib & _A_SUBDIR)) {
			matches = realloc (matches, ++num_files * sizeof (*matches));

			for (di = num_files - 1; di > file_index + 1; di--)
			    matches [di] = matches [di - 1];

			matches [++file_index] = malloc (strlen (infilename) + strlen (_finddata_t.name) + 10);
			strcpy (matches [file_index], infilename);
			*filespec_name (matches [file_index]) = '\0';
			strcat (matches [file_index], _finddata_t.name);
		    }
		} while (_findnext (file, &_finddata_t) == 0);

		_findclose (file);
	    }

	    free (infilename);
	}
#endif
    }

    // If the outfile specification begins with a '@', then it actually points
    // to a file that contains the output specification. This was included for
    // use by Wim Speekenbrink's frontends because certain filenames could not
    // be passed on the command-line, but could be used for other purposes.

    if (outfilename && outfilename [0] == '@') {
	FILE *list = fopen (outfilename+1, "rt");
	int c;

	if (list == NULL) {
	    error_line ("file %s not found!", outfilename+1);
	    free(outfilename);
	    return 1;
	}

	while ((c = getc (list)) == '\n');

	if (c != EOF) {
	    int ci = 0;

	    do
		outfilename [ci++] = c;
	    while ((c = getc (list)) != '\n' && c != EOF && ci < PATH_MAX);

	    outfilename [ci] = '\0';
	}
	else {
	    error_line ("output spec file is empty!");
	    free(outfilename);
	    fclose (list);
	    return 1;
	}

	fclose (list);
    }

    // if we found any files to process, this is where we start

    if (num_files) {
	if (outfilename && *outfilename != '-') {
	    outpath = (filespec_path (outfilename) != NULL);

	    if (num_files > 1 && !outpath) {
		error_line ("%s is not a valid output path", outfilename);
		free (outfilename);
		return 1;
	    }
	}
	else
	    outpath = 0;

	add_extension = !outfilename || outpath || !filespec_ext (outfilename);

	// loop through and process files in list

	for (file_index = 0; file_index < num_files; ++file_index) {
	    if (check_break ())
		break;

	    // generate output filename

	    if (outpath) {
		strcat (outfilename, filespec_name (matches [file_index]));

		if (filespec_ext (outfilename))
		    *filespec_ext (outfilename) = '\0';
	    }
	    else if (!outfilename) {
		outfilename = malloc (strlen (matches [file_index]) + 10);
		strcpy (outfilename, matches [file_index]);

		if (filespec_ext (outfilename))
		    *filespec_ext (outfilename) = '\0';
	    }

	    if (outfilename && *outfilename != '-' && add_extension)
		strcat (outfilename, raw_decode ? ".raw" : ".wav");

	    if (num_files > 1)
		fprintf (stderr, "\n%s:\n", matches [file_index]);

	    result = unpack_file (matches [file_index], verify_only ? NULL : outfilename);

	    if (result != NO_ERROR)
		++error_count;

	    if (result == HARD_ERROR)
		break;

	    // clean up in preparation for potentially another file

	    if (outpath)
		*filespec_name (outfilename) = '\0';
	    else if (*outfilename != '-') {
		free (outfilename);
		outfilename = NULL;
	    }

	    free (matches [file_index]);
	}

	if (num_files > 1) {
	    if (error_count)
		fprintf (stderr, "\n **** warning: errors occurred in %d of %d files! ****\n", error_count, num_files);
	    else if (!quiet_mode)
		fprintf (stderr, "\n **** %d files successfully processed ****\n", num_files);
	}

	free (matches);
    }
    else {
	error_line ("nothing to do!");
	++error_count;
    }

    if (outfilename)
	free (outfilename);

#ifdef DEBUG_ALLOC
    error_line ("malloc_count = %d", dump_alloc ());
#endif

    return error_count ? 1 : 0;
}

// Unpack the specified WavPack input file into the specified output file name.
// This function uses the library routines provided in wputils.c to do all
// unpacking. This function takes care of reformatting the data (which is
// returned in native-endian longs) to the standard little-endian format. This
// function also handles optionally calculating and displaying the MD5 sum of
// the resulting audio data and verifying the sum if a sum was stored in the
// source and lossless compression is used.

static uchar *format_samples (int bps, uchar *dst, int32_t *src, uint32_t samcnt);
static void dump_summary (WavpackContext *wpc, char *name, FILE *dst);
static int write_riff_header (FILE *outfile, WavpackContext *wpc, uint32_t total_samples);
static int dump_cuesheet (WavpackContext *wpc, FILE *dst);

static int unpack_file (char *infilename, char *outfilename)
{
    int result = NO_ERROR, md5_diff = FALSE, created_riff_header = FALSE;
    int open_flags = 0, bytes_per_sample, num_channels, wvc_mode, bps;
    uint32_t outfile_length, output_buffer_size = 0, bcount, total_unpacked_samples = 0;
    uchar *output_buffer = NULL, *output_pointer = NULL;
    double dtime, progress = -1.0;
    MD5_CTX md5_context;
    WavpackContext *wpc;
    int32_t *temp_buffer;
    char error [80];
    FILE *outfile;

#if defined(WIN32)
    struct _timeb time1, time2;
#else
    struct timeval time1, time2;
    struct timezone timez;
#endif

    // use library to open WavPack file

    if ((outfilename && !raw_decode && !blind_decode && !wav_decode) || summary > 1)
	open_flags |= OPEN_WRAPPER;

    if (blind_decode)
	open_flags |= OPEN_STREAMING;

    if (!ignore_wvc)
	open_flags |= OPEN_WVC;

    if (summary > 1 || extract_cuesheet)
	open_flags |= OPEN_TAGS;

    wpc = WavpackOpenFileInput (infilename, error, open_flags, 0);

    if (!wpc) {
	error_line (error);
	return SOFT_ERROR;
    }

    if (calc_md5)
	MD5Init (&md5_context);

    wvc_mode = WavpackGetMode (wpc) & MODE_WVC;
    num_channels = WavpackGetNumChannels (wpc);
    bps = WavpackGetBytesPerSample (wpc);
    bytes_per_sample = num_channels * bps;

    if (summary) {
	dump_summary (wpc, infilename, stdout);
	WavpackCloseFile (wpc);
	return NO_ERROR;
    }

    if (extract_cuesheet == 1) {
        if (!dump_cuesheet (wpc, stdout)) {
            error_line ("cuesheet not found!");
            WavpackCloseFile (wpc);
            return SOFT_ERROR;
        }

	WavpackCloseFile (wpc);
	return NO_ERROR;
    }
    else if (extract_cuesheet > 1 && outfilename && *outfilename != '-' && dump_cuesheet (wpc, NULL)) {
	char *cuefilename = malloc (strlen (outfilename) + 10);

	strcpy (cuefilename, outfilename);

        if (filespec_ext (cuefilename))
            strcpy (filespec_ext (cuefilename), ".cue");
        else
            strcat (cuefilename, ".cue");

        if (!overwrite_all && (outfile = fopen (cuefilename, "r")) != NULL) {
            DoCloseHandle (outfile);
            fprintf (stderr, "overwrite %s (yes/no/all)? ", FN_FIT (cuefilename));
#if defined(WIN32)
            SetConsoleTitle ("overwrite?");
#endif
            switch (yna ()) {

                case 'n':
                    result = SOFT_ERROR;
                    break;

                case 'a':
                    overwrite_all = 1;
            }
        }

        // open output file for writing

        if (result == NO_ERROR) {
            if ((outfile = fopen (cuefilename, "wt")) == NULL) {
                error_line ("can't create file %s!", FN_FIT (cuefilename));
                result = SOFT_ERROR;
            }
            else {
                dump_cuesheet (wpc, outfile);

                if (!DoCloseHandle (outfile)) {
                    error_line ("can't close file %s!", FN_FIT (cuefilename));
                    result = SOFT_ERROR;
                }
                else if (!quiet_mode)
                    error_line ("extracted cuesheet file %s", FN_FIT (cuefilename));
            }
        }

	free (cuefilename);

	if (result != NO_ERROR) {
	    WavpackCloseFile (wpc);
	    return result;
	}
    }   

    if (outfilename) {
	if (*outfilename != '-') {

	    // check the output file for overwrite warning required

	    if (!overwrite_all && (outfile = fopen (outfilename, "rb")) != NULL) {
		DoCloseHandle (outfile);
		fprintf (stderr, "overwrite %s (yes/no/all)? ", FN_FIT (outfilename));
#if defined(WIN32)
		SetConsoleTitle ("overwrite?");
#endif
		switch (yna ()) {

		    case 'n':
			result = SOFT_ERROR;
			break;

		    case 'a':
			overwrite_all = 1;
		}

		if (result != NO_ERROR) {
		    WavpackCloseFile (wpc);
		    return result;
		}
	    }

	    // open output file for writing

	    if ((outfile = fopen (outfilename, "wb")) == NULL) {
		error_line ("can't create file %s!", FN_FIT (outfilename));
		WavpackCloseFile (wpc);
		return SOFT_ERROR;
	    }
	    else if (!quiet_mode)
		fprintf (stderr, "restoring %s,", FN_FIT (outfilename));
	}
	else {	// come here to open stdout as destination

	    outfile = stdout;
#if defined(WIN32)
	    _setmode (fileno (stdout), O_BINARY);
#endif

	    if (!quiet_mode)
		fprintf (stderr, "unpacking %s%s to stdout,", *infilename == '-' ?
		    "stdin" : FN_FIT (infilename), wvc_mode ? " (+.wvc)" : "");
	}

	if (outbuf_k)
	    output_buffer_size = outbuf_k * 1024;
	else
	    output_buffer_size = 1024 * 256;

	output_pointer = output_buffer = malloc (output_buffer_size);
    }
    else {	// in verify only mode we don't worry about headers
	outfile = NULL;

	if (!quiet_mode)
	    fprintf (stderr, "verifying %s%s,", *infilename == '-' ? "stdin" :
		FN_FIT (infilename), wvc_mode ? " (+.wvc)" : "");
    }

#if defined(WIN32)
    _ftime (&time1);
#else
    gettimeofday(&time1,&timez);
#endif

    if (outfile && !raw_decode) {
	if (WavpackGetWrapperBytes (wpc)) {
	    if (!DoWriteFile (outfile, WavpackGetWrapperData (wpc), WavpackGetWrapperBytes (wpc), &bcount) ||
		bcount != WavpackGetWrapperBytes (wpc)) {
		    error_line ("can't write .WAV data, disk probably full!");
		    DoTruncateFile (outfile);
		    result = HARD_ERROR;
	    }

	    WavpackFreeWrapper (wpc);
	}
	else if (!write_riff_header (outfile, wpc, WavpackGetNumSamples (wpc))) {
	    DoTruncateFile (outfile);
	    result = HARD_ERROR;
	}
	else
	    created_riff_header = TRUE;
    }

    temp_buffer = malloc (4096L * num_channels * 4);

    while (result == NO_ERROR) {
	uint32_t samples_to_unpack, samples_unpacked;

	if (output_buffer) {
	    samples_to_unpack = (output_buffer_size - (output_pointer - output_buffer)) / bytes_per_sample;

	    if (samples_to_unpack > 4096)
		samples_to_unpack = 4096;
	}
	else
	    samples_to_unpack = 4096;

	samples_unpacked = WavpackUnpackSamples (wpc, temp_buffer, samples_to_unpack);
	total_unpacked_samples += samples_unpacked;

	if (output_buffer) {
	    if (samples_unpacked)
		output_pointer = format_samples (bps, output_pointer, temp_buffer, samples_unpacked * num_channels);

	    if (!samples_unpacked || (output_buffer_size - (output_pointer - output_buffer)) < (uint32_t) bytes_per_sample) {
		if (!DoWriteFile (outfile, output_buffer, (uint32_t)(output_pointer - output_buffer), &bcount) ||
		    bcount != output_pointer - output_buffer) {
			error_line ("can't write .WAV data, disk probably full!");
			DoTruncateFile (outfile);
			result = HARD_ERROR;
			break;
		}

		output_pointer = output_buffer;
	    }
	}

	if (calc_md5 && samples_unpacked) {
	    format_samples (bps, (uchar *) temp_buffer, temp_buffer, samples_unpacked * num_channels);
	    MD5Update (&md5_context, (unsigned char *) temp_buffer, bps * samples_unpacked * num_channels);
	}

	if (!samples_unpacked)
	    break;

	if (check_break ()) {
	    fprintf (stderr, "^C\n");
	    DoTruncateFile (outfile);
	    result = SOFT_ERROR;
	    break;
	}

	if (WavpackGetProgress (wpc) != -1.0 &&
	    progress != floor (WavpackGetProgress (wpc) * 100.0 + 0.5)) {
		int nobs = progress == -1.0;

		progress = WavpackGetProgress (wpc);
		display_progress (progress);
		progress = floor (progress * 100.0 + 0.5);

		if (!quiet_mode)
		    fprintf (stderr, "%s%3d%% done...",
			nobs ? " " : "\b\b\b\b\b\b\b\b\b\b\b\b", (int) progress);
	}
    }

    free (temp_buffer);

    if (output_buffer)
	free (output_buffer);

    if (!check_break () && calc_md5) {
	char md5_string1 [] = "00000000000000000000000000000000";
	char md5_string2 [] = "00000000000000000000000000000000";
	uchar md5_original [16], md5_unpacked [16];
	int i;

	MD5Final (md5_unpacked, &md5_context);

	if (WavpackGetMD5Sum (wpc, md5_original)) {

	    for (i = 0; i < 16; ++i)
		sprintf (md5_string1 + (i * 2), "%02x", md5_original [i]);

	    error_line ("original md5:  %s", md5_string1);

	    if (memcmp (md5_unpacked, md5_original, 16))
		md5_diff = TRUE;
	}

	for (i = 0; i < 16; ++i)
	    sprintf (md5_string2 + (i * 2), "%02x", md5_unpacked [i]);

	error_line ("unpacked md5:  %s", md5_string2);
    }

    if (!created_riff_header && WavpackGetWrapperBytes (wpc)) {
	if (outfile && result == NO_ERROR &&
	    (!DoWriteFile (outfile, WavpackGetWrapperData (wpc), WavpackGetWrapperBytes (wpc), &bcount) ||
	    bcount != WavpackGetWrapperBytes (wpc))) {
		error_line ("can't write .WAV data, disk probably full!");
		DoTruncateFile (outfile);
		result = HARD_ERROR;
	}

	WavpackFreeWrapper (wpc);
    }

    if (result == NO_ERROR && outfile && created_riff_header &&
	(WavpackGetNumSamples (wpc) == (uint32_t) -1 ||
	 WavpackGetNumSamples (wpc) != total_unpacked_samples)) {
	    if (*outfilename == '-' || DoSetFilePositionAbsolute (outfile, 0))
		error_line ("can't update RIFF header with actual size");
	    else if (!write_riff_header (outfile, wpc, total_unpacked_samples)) {
		DoTruncateFile (outfile);
		result = HARD_ERROR;
	    }
    }

    // if we are not just in verify only mode, grab the size of the output
    // file and close the file

    if (outfile != NULL) {
	fflush (outfile);
	outfile_length = DoGetFileSize (outfile);

	if (!DoCloseHandle (outfile)) {
	    error_line ("can't close file %s!", FN_FIT (outfilename));
	    result = SOFT_ERROR;
	}

	if (outfilename && *outfilename != '-' && !outfile_length)
	    DoDeleteFile (outfilename);
    }

    if (result == NO_ERROR && copy_time && outfilename &&
	!copy_timestamp (infilename, outfilename))
	    error_line ("failure copying time stamp!");

    if (result == NO_ERROR) {
	if (WavpackGetNumSamples (wpc) != (uint32_t) -1) {
	    if (total_unpacked_samples < WavpackGetNumSamples (wpc)) {
		error_line ("file is missing %u samples!",
		    WavpackGetNumSamples (wpc) - total_unpacked_samples);
		result = SOFT_ERROR;
	    }
	    else if (total_unpacked_samples > WavpackGetNumSamples (wpc)) {
		error_line ("file has %u extra samples!",
		    total_unpacked_samples - WavpackGetNumSamples (wpc));
		result = SOFT_ERROR;
	    }
	}

	if (WavpackGetNumErrors (wpc)) {
	    error_line ("missing data or crc errors detected in %d block(s)!", WavpackGetNumErrors (wpc));
	    result = SOFT_ERROR;
	}
    }

    if (result == NO_ERROR && md5_diff && (WavpackGetMode (wpc) & MODE_LOSSLESS)) {
	error_line ("MD5 signatures should match, but do not!");
	result = SOFT_ERROR;
    }

    // Compute and display the time consumed along with some other details of
    // the unpacking operation (assuming there was no error).

#if defined(WIN32)
    _ftime (&time2);
    dtime = time2.time + time2.millitm / 1000.0;
    dtime -= time1.time + time1.millitm / 1000.0;
#else
    gettimeofday(&time2,&timez);
    dtime = time2.tv_sec + time2.tv_usec / 1000000.0;
    dtime -= time1.tv_sec + time1.tv_usec / 1000000.0;
#endif

    if (result == NO_ERROR && !quiet_mode) {
	char *file, *fext, *oper, *cmode, cratio [16] = "";

	if (outfilename && *outfilename != '-') {
	    file = FN_FIT (outfilename);
	    fext = "";
	    oper = "restored";
	}
	else {
	    file = (*infilename == '-') ? "stdin" : FN_FIT (infilename);
	    fext = wvc_mode ? " (+.wvc)" : "";
	    oper = outfilename ? "unpacked" : "verified";
	}

	if (WavpackGetMode (wpc) & MODE_LOSSLESS) {
	    cmode = "lossless";

	    if (WavpackGetRatio (wpc) != 0.0)
		sprintf (cratio, ", %.2f%%", 100.0 - WavpackGetRatio (wpc) * 100.0);
	}
	else {
	    cmode = "lossy";

	    if (WavpackGetAverageBitrate (wpc, TRUE) != 0.0)
		sprintf (cratio, ", %d kbps", (int) (WavpackGetAverageBitrate (wpc, TRUE) / 1000.0));
	}

	error_line ("%s %s%s in %.2f secs (%s%s)", oper, file, fext, dtime, cmode, cratio);
    }

    WavpackCloseFile (wpc);

    if (result == NO_ERROR && delete_source) {
	int res = DoDeleteFile (infilename);

	if (!quiet_mode || !res)
	    error_line ("%s source file %s", res ?
		"deleted" : "can't delete", infilename);

	if (wvc_mode) {
	    char in2filename [PATH_MAX];

	    strcpy (in2filename, infilename);
	    strcat (in2filename, "c");
	    res = DoDeleteFile (in2filename);

	    if (!quiet_mode || !res)
		error_line ("%s source file %s", res ?
		    "deleted" : "can't delete", in2filename);
	}
    }

    return result;
}

// Reformat samples from longs in processor's native endian mode to
// little-endian data with (possibly) less than 4 bytes / sample.

static uchar *format_samples (int bps, uchar *dst, int32_t *src, uint32_t samcnt)
{
    int32_t temp;

    switch (bps) {

	case 1:
	    while (samcnt--)
		*dst++ = *src++ + 128;

	    break;

	case 2:
	    while (samcnt--) {
		*dst++ = (uchar) (temp = *src++);
		*dst++ = (uchar) (temp >> 8);
	    }

	    break;

	case 3:
	    while (samcnt--) {
		*dst++ = (uchar) (temp = *src++);
		*dst++ = (uchar) (temp >> 8);
		*dst++ = (uchar) (temp >> 16);
	    }

	    break;

	case 4:
	    while (samcnt--) {
		*dst++ = (uchar) (temp = *src++);
		*dst++ = (uchar) (temp >> 8);
		*dst++ = (uchar) (temp >> 16);
		*dst++ = (uchar) (temp >> 24);
	    }

	    break;
    }

    return dst;
}

static int write_riff_header (FILE *outfile, WavpackContext *wpc, uint32_t total_samples)
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

static void dump_UTF8_string (char *string, FILE *dst);
static void UTF8ToAnsi (char *string, int len);
static const char *speakers [] = {
    "FL", "FR", "FC", "LFE", "BL", "BR", "FLC", "FRC", "BC",
    "SL", "SR", "TC", "TFL", "TFC", "TFR", "TBL", "TBC", "TBR"
};

static void dump_summary (WavpackContext *wpc, char *name, FILE *dst)
{
    uint32_t channel_mask = (uint32_t) WavpackGetChannelMask (wpc);
    int num_channels = WavpackGetNumChannels (wpc);
    uchar md5_sum [16];
    char modes [80];

    fprintf (dst, "\n");

    if (name && *name != '-') {
	fprintf (dst, "file name:         %s%s\n", name, (WavpackGetMode (wpc) & MODE_WVC) ? " (+wvc)" : "");
	fprintf (dst, "file size:         %u bytes\n", WavpackGetFileSize (wpc));
    }

    fprintf (dst, "source:            %d-bit %s at %u Hz\n", WavpackGetBitsPerSample (wpc),
	(WavpackGetMode (wpc) & MODE_FLOAT) ? "floats" : "ints",
	WavpackGetSampleRate (wpc));

    if (!channel_mask)
	strcpy (modes, "undefined speakers");
    else if (num_channels == 1 && channel_mask == 0x4)
	strcpy (modes, "mono");
    else if (num_channels == 2 && channel_mask == 0x3)
	strcpy (modes, "stereo");
    else {
	int cc = num_channels, si = 0;
	uint32_t cm = channel_mask;

	modes [0] = 0;

	while (cc && cm) {
	    if (cm & 1) {
		strcat (modes, speakers [si]);
		if (--cc)
		    strcat (modes, ",");
	    }
	    cm >>= 1;
	    si++;
	}

	if (cc)
	    strcat (modes, "...");
    }

    fprintf (dst, "channels:          %d (%s)\n", num_channels, modes);

    if (WavpackGetNumSamples (wpc) != (uint32_t) -1) {
	double seconds = (double) WavpackGetNumSamples (wpc) / WavpackGetSampleRate (wpc);
	int minutes = (int) floor (seconds / 60.0);
	int hours = (int) floor (seconds / 3600.0);

	seconds -= minutes * 60.0;
	minutes -= (int)(hours * 60.0);

	fprintf (dst, "duration:          %d:%02d:%05.2f\n", hours, minutes, seconds);
    }

    modes [0] = 0;

    if (WavpackGetMode (wpc) & MODE_HYBRID)
	strcat (modes, "hybrid ");

    strcat (modes, (WavpackGetMode (wpc) & MODE_LOSSLESS) ? "lossless" : "lossy");

    if (WavpackGetMode (wpc) & MODE_FAST)
	strcat (modes, ", fast");
    else if (WavpackGetMode (wpc) & MODE_VERY_HIGH)
	strcat (modes, ", very high");
    else if (WavpackGetMode (wpc) & MODE_HIGH)
	strcat (modes, ", high");

    if (WavpackGetMode (wpc) & MODE_EXTRA)
	strcat (modes, ", extra");

    if (WavpackGetMode (wpc) & MODE_SFX)
	strcat (modes, ", sfx");

    fprintf (dst, "modalities:        %s\n", modes);

    if (WavpackGetRatio (wpc) != 0.0) {
	fprintf (dst, "compression:       %.2f%%\n", 100.0 - (100 * WavpackGetRatio (wpc)));
	fprintf (dst, "ave bitrate:       %d kbps\n", (int) ((WavpackGetAverageBitrate (wpc, TRUE) + 500.0) / 1000.0));

	if (WavpackGetMode (wpc) & MODE_WVC)
	    fprintf (dst, "ave lossy bitrate: %d kbps\n", (int) ((WavpackGetAverageBitrate (wpc, FALSE) + 500.0) / 1000.0));
    }

    if (WavpackGetVersion (wpc))
	fprintf (dst, "encoder version:   %d\n", WavpackGetVersion (wpc));

    if (WavpackGetMD5Sum (wpc, md5_sum)) {
	char md5_string [] = "00000000000000000000000000000000";
	int i;

	for (i = 0; i < 16; ++i)
	    sprintf (md5_string + (i * 2), "%02x", md5_sum [i]);

	fprintf (dst, "original md5:      %s\n", md5_string);
    }

    if (summary > 1) {
	uint32_t header_bytes = WavpackGetWrapperBytes (wpc), trailer_bytes, i;
	uchar *header_data = WavpackGetWrapperData (wpc);
	char header_name [5];

	strcpy (header_name, "????");

	for (i = 0; i < 4 && i < header_bytes; ++i)
	    if (header_data [i] >= 0x20 && header_data [i] <= 0x7f)
		header_name [i] = header_data [i];

	WavpackFreeWrapper (wpc);
	WavpackSeekTrailingWrapper (wpc);
	trailer_bytes = WavpackGetWrapperBytes (wpc);

	if (header_bytes && trailer_bytes)
	    fprintf (dst, "file wrapper:      %d + %d bytes (%s)\n",
		header_bytes, trailer_bytes, header_name);
	else if (header_bytes)
	    fprintf (dst, "file wrapper:      %d byte %s header\n",
		header_bytes, header_name);
	else if (trailer_bytes)
	    fprintf (dst, "file wrapper:      %d byte trailer only\n",
		trailer_bytes);
	else
	    fprintf (dst, "file wrapper:      none (raw audio)\n");
    }
	
    if (WavpackGetMode (wpc) & MODE_VALID_TAG) {
	int ape_tag = WavpackGetMode (wpc) & MODE_APETAG;
	int num_items = WavpackGetNumTagItems (wpc), i;

	fprintf (dst, "%s tag items:   %d\n\n", ape_tag ? "APEv2" : "ID3v1", num_items);

	for (i = 0; i < num_items; ++i) {
	    int item_len, value_len, j;
	    char *item, *value;

	    item_len = WavpackGetTagItemIndexed (wpc, i, NULL, 0);
	    item = malloc (item_len + 1);
	    WavpackGetTagItemIndexed (wpc, i, item, item_len + 1);
	    value_len = WavpackGetTagItem (wpc, item, NULL, 0);
	    value = malloc (value_len * 2 + 1);
	    WavpackGetTagItem (wpc, item, value, value_len + 1);

	    if (ape_tag) {
                for (j = 0; j < value_len; ++j)
                    if (!value [j])
                        value [j] = '\\';

                fprintf (dst, "%s =%c", item, strchr (value, '\n') ? '\n' : ' ');
                dump_UTF8_string (value, dst);
		fprintf (dst, "\n");
            }
	    else
		fprintf (dst, "%s = %s\n", item, value);

	    free (value);
	    free (item);
	}
    }
}

static int dump_cuesheet (WavpackContext *wpc, FILE *dst)
{
    if ((WavpackGetMode (wpc) & MODE_VALID_TAG) &&
	(WavpackGetMode (wpc) & MODE_APETAG)) {

	    int value_len = WavpackGetTagItem (wpc, "cuesheet", NULL, 0);
            char *value;

            if (!value_len || !dst)
                return value_len;

	    value = malloc (value_len * 2 + 1);
	    WavpackGetTagItem (wpc, "cuesheet", value, value_len + 1);
            dump_UTF8_string (value, dst);
	    free (value);
            return value_len;
    }
    else
        return 0;
}

// Dump the specified null-terminated, possibly multi-line, UTF-8 string to
// the specified stream. To make sure that this works correctly on both
// Windows and Linux, all CR characters ('\r') are removed from the stream
// and it is assumed that the output FILE will be in "text" mode (on Windows).
// Lines are processed and transmitted one at a time.

static void dump_UTF8_string (char *string, FILE *dst)
{
    while (*string) {
        char *p = string, *temp;
        int len = 0;

        while (*p) {
            if (*p != '\r')
                ++len;

            if (*p++ == '\n')
                break;
        }

        if (!len)
            return;

        p = temp = malloc (len * 2 + 1);

        while (*string) {
            if (*string != '\r')
                *p++ = *string;

            if (*string++ == '\n')
                break;
        }

        *p = 0;
        UTF8ToAnsi (temp, len * 2);
        fputs (temp, dst);
        free (temp);
    }
}

#if defined (WIN32)

// Convert Unicode UTF-8 string to wide format. UTF-8 string must be NULL
// terminated. Resulting wide string must be able to fit in provided space
// and will also be NULL terminated. The number of characters converted will
// be returned (not counting terminator).

static int UTF8ToWideChar (const unsigned char *pUTF8, unsigned short *pWide)
{
    int trail_bytes = 0;
    int chrcnt = 0;

    while (*pUTF8) {
	if (*pUTF8 & 0x80) {
	    if (*pUTF8 & 0x40) {
		if (trail_bytes) {
		    trail_bytes = 0;
		    chrcnt++;
		}
		else {
		    char temp = *pUTF8;

		    while (temp & 0x80) {
			trail_bytes++;
			temp <<= 1;
		    }

		    pWide [chrcnt] = temp >> trail_bytes--;
		}
	    }
	    else if (trail_bytes) {
		pWide [chrcnt] = (pWide [chrcnt] << 6) | (*pUTF8 & 0x3f);

		if (!--trail_bytes)
		    chrcnt++;
	    }
	}
	else
	    pWide [chrcnt++] = *pUTF8;

	pUTF8++;
    }

    pWide [chrcnt] = 0;
    return chrcnt;
}

#endif

// Convert a Unicode UTF-8 format string into its Ansi equivalent. The
// conversion is done in-place so the maximum length of the string buffer must
// be specified because the string may become longer or shorter. If the
// resulting string will not fit in the specified buffer size then it is
// truncated.

static void UTF8ToAnsi (char *string, int len)
{
    int max_chars = (int) strlen (string);
#if defined (WIN32)
    unsigned short *temp = malloc ((max_chars + 1) * 2);
    int act_chars = UTF8ToWideChar (string, temp);

    while (act_chars) {
	memset (string, 0, len);

	if (WideCharToMultiByte (CP_ACP, 0, temp, act_chars, string, len - 1, NULL, NULL))
	    break;
	else
	    act_chars--;
    }

    if (!act_chars)
	*string = 0;
#else
    char *temp = malloc (len);
    char *outp = temp;
    char *inp = string;
    size_t insize = max_chars;
    size_t outsize = len - 1;
    int err = 0;
    char *old_locale;

    memset(temp, 0, len);
    old_locale = setlocale (LC_CTYPE, "");
    iconv_t converter = iconv_open ("", "UTF-8");
    err = iconv (converter, &inp, &insize, &outp, &outsize);
    iconv_close (converter);
    setlocale (LC_CTYPE, old_locale);

    if (err == -1) {
	free(temp);
	return;
    }

    memmove (string, temp, len);
#endif
    free (temp);
}

//////////////////////////////////////////////////////////////////////////////
// This function displays the progress status on the title bar of the DOS   //
// window that WavPack is running in. The "file_progress" argument is for   //
// the current file only and ranges from 0 - 1; this function takes into    //
// account the total number of files to generate a batch progress number.   //
//////////////////////////////////////////////////////////////////////////////

void display_progress (double file_progress)
{
    char title [40];

    file_progress = (file_index + file_progress) / num_files;
    sprintf (title, "%d%% (WvUnpack)", (int) ((file_progress * 100.0) + 0.5));
#if defined(WIN32)
    SetConsoleTitle (title);
#endif
}
