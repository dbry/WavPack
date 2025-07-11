////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//                Copyright (c) 1998 - 2024 David Bryant                  //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// import_id3.c

// This module provides limited support for importing existing ID3 tags
// (from DSF files, for example) into WavPack files

#ifndef ENABLE_LIBICONV
#define LIBICONV_PLUG 1
#endif

#include <sys/stat.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "wavpack.h"
#include "utils.h"

static const struct {
    char *id3_item, *ape_item;
} text_tag_table [] = {
    { "TALB", "Album" },
    { "TPE1", "Artist" },
    { "TPE2", "AlbumArtist" },
    { "TPE3", "Conductor" },
    { "TIT1", "Grouping" },
    { "TIT2", "Title" },
    { "TIT3", "Subtitle" },
    { "TSST", "DiscSubtitle" },
    { "TSOA", "AlbumSort" },
    { "TSOT", "TitleSort" },
    { "TSO2", "AlbumArtistSort" },
    { "TSOP", "ArtistSort" },
    { "TSOC", "ComposerSort" },
    { "TSST", "DiscSubtitle" },
    { "TPOS", "Disc" },
    { "TRCK", "Track" },
    { "TCON", "Genre" },
    { "TYER", "Year" },
    { "TDRC", "Year" },
    { "TMOO", "Mood" },
    { "TCOM", "Composer" },
    { "TPUB", "Publisher" },
    { "TCMP", "Compilation" },
    { "TENC", "EncodedBy" },
    { "TSSE", "Encoder" },
    { "TEXT", "Lyricist" },
    { "TCOP", "Copyright" },
    { "TLAN", "Language" },
    { "TSRC", "ISRC" },
    { "TMED", "Media" },
    { "TMOO", "Mood" },
    { "TBPM", "BPM" }
};

#define NUM_TEXT_TAG_ITEMS (sizeof (text_tag_table) / sizeof (text_tag_table [0]))

static const char *picture_types [] = {
    "Other", "Png Icon", "Icon", "Front", "Back", "Leaflet", "Media",
    "Lead Artist", "Artist", "Conductor", "Band", "Composer", "Lyricist",
    "Recording Location", "During Recording", "During Performance", "Video Capture",
    "Phish", "Illustration", "Band Logotype", "Publisher Logotype" };

#define NUM_PICTURE_TYPES (sizeof (picture_types) / sizeof (picture_types [0]))

typedef enum { ISO_8859_1, UTF_16, UTF_16BE, UTF_8 } ID3v2TextEncoding;

static int ID3v2StringsToUTF8 (ID3v2TextEncoding encoding, unsigned char *src, int src_length, unsigned char *dst);
static int WavpackAppendTagItemNoDups (WavpackContext *wpc, const char *item, const char *value, int vsize);
static int WideCharToUTF8 (const uint16_t *Wide, unsigned char *pUTF8, int len);
static int strlen_segments (const char *string, int num_segments);
static int strlen_limit (const char *string, int num_bytes);
static void Latin1ToUTF8 (void *string, int len);

// Similar to ImportID3v2() described below, but has an additional parameter "syncsafe"
// that specifies whether the frame sizes are "synchsafe" (i.e., the MSBs of each byte
// are not used). This feature was inexplicably missing for the frame sizes in ID3v2.3,
// however some tag writers (e.g., early versions of sacd_extract) would incorrectly
// apply synchsafe to frame sizes causing parsing to fail. By making this selectable
// for ID3v2.3 tags it's possible to correctly parse these non-compliant tags if the
// first attempt fails.

static int ImportID3v2_syncsafe (WavpackContext *wpc, unsigned char *tag_data, int tag_size, char *error, int32_t *bytes_used, int syncsafe)
{
    int tag_size_from_header, items_imported = 0;
    unsigned char id3_header [10];
    int32_t local_bytes_used = 0;
    char tag_type [16];

    if (bytes_used)
        *bytes_used = 0;
    else
        bytes_used = &local_bytes_used;

    if (tag_size < sizeof (id3_header)) {
        strcpy (error, "can't read tag header");
        return -1;
    }

    memcpy (id3_header, tag_data, sizeof (id3_header));
    snprintf (tag_type, sizeof (tag_type), "ID3v2.%d", id3_header [3]);
    tag_size -= sizeof (id3_header);
    tag_data += sizeof (id3_header);

    if (id3_header [4] == 0xFF || (id3_header [5] & 0x0F)) {
        sprintf (error, "unsupported %s tag (header flags)", tag_type);
        return -1;
    }

    if (id3_header [5] & 0x80) {
        sprintf (error, "unsupported %s tag (unsynchronization)", tag_type);
        return -1;
    }

    if (id3_header [5] & 0x40) {
        sprintf (error, "unsupported %s tag (extended header)", tag_type);
        return -1;
    }

    if (id3_header [5] & 0x20) {
        sprintf (error, "unsupported %s tag (experimental indicator)", tag_type);
        return -1;
    }

    if ((id3_header [6] | id3_header [7] | id3_header [8] | id3_header [9]) & 0x80) {
        sprintf (error, "invalid %s tag (bad size)", tag_type);
        return -1;
    }

    tag_size_from_header = id3_header [9] + (id3_header [8] << 7) + (id3_header [7] << 14) + (id3_header [6] << 21);

    if (tag_size_from_header > tag_size) {
        sprintf (error, "invalid %s tag (truncated)", tag_type);
        return -1;
    }
    else if (tag_size_from_header < tag_size)
        tag_size = tag_size_from_header;        // trust indicated tag size over what's passed in here

    while (1) {
        unsigned char frame_header [10], *frame_body;
        int frame_size, i;

        if (tag_size < sizeof (frame_header))
            break;

        memcpy (frame_header, tag_data, sizeof (frame_header));
        tag_size -= sizeof (frame_header);
        tag_data += sizeof (frame_header);

        if (!frame_header [0] && !frame_header [1] && !frame_header [2] && !frame_header [3])
            break;

        if ((id3_header [5] & 0x10) && !strncmp ((char *) frame_header, "3DI", 3))
           break;

        for (i = 0; i < 4; ++i)
            if (frame_header [i] < '0' ||
                (frame_header [i] > '9' && frame_header [i] < 'A') ||
                frame_header [i] > 'Z') {
                    sprintf (error, "invalid %s tag (bad frame identity)", tag_type);
                    return -1;
            }

        if (frame_header [9]) {
            sprintf (error, "unsupported %s tag (unknown frame_header flag set)", tag_type);
            return -1;
        }

        if (syncsafe)
            frame_size = frame_header [7] + (frame_header [6] << 7) + (frame_header [5] << 14) + (frame_header [4] << 21);
        else
            frame_size = frame_header [7] + (frame_header [6] << 8) + (frame_header [5] << 16) + (frame_header [4] << 24);

        if (!frame_size) {
            sprintf (error, "invalid %s tag (empty frame encountered)", tag_type);
            return -1;
        }

        if (frame_size > tag_size) {
            sprintf (error, "invalid %s tag (truncated)", tag_type);
            return -1;
        }

        frame_body = malloc (frame_size + 4);

        memcpy (frame_body, tag_data, frame_size);
        tag_size -= frame_size;
        tag_data += frame_size;

        if (frame_header [0] == 'T' && frame_size >= 2) {
            int txxx_mode = !strncmp ((char *) frame_header, "TXXX", 4), num_segments;
            unsigned char *utf8_string = malloc (frame_size * 2);

            num_segments = ID3v2StringsToUTF8 (frame_body [0], frame_body + 1, frame_size - 1, utf8_string);

            if (num_segments == -1) {
                sprintf (error, "invalid %s tag (undefined character encoding)", tag_type);
                return -1;
            }

            // if we got a text string (or a TXXX and two text strings) store them here

            if (txxx_mode && num_segments >= 2 && strlen ((char *) utf8_string)) {
                unsigned char *cptr = utf8_string;
                unsigned char *utf8_value;
                int value_length;

                // if all single-byte UTF8, format TXXX description to match case of regular APEv2 descriptions (e.g., Performer)

                while (*cptr)
                    if (*cptr & 0x80)
                        break;
                    else
                        cptr++;

                if (!*cptr && isupper (*utf8_string)) {
                    cptr = utf8_string;

                    while (*++cptr)
                        if (isupper (*cptr))
                            *cptr = tolower (*cptr);
                }

                utf8_value = utf8_string + strlen ((char *) utf8_string) + 1;
                value_length = strlen_segments ((char *) utf8_value, num_segments - 1);

                if (wpc && !WavpackAppendTagItemNoDups (wpc, (char *) utf8_string, (char *) utf8_value, value_length)) {
                    strcpy (error, WavpackGetErrorMessage (wpc));
                    return -1;
                }

                items_imported++;
                if (bytes_used) *bytes_used += (int) (strlen ((char *) utf8_string) + value_length + 1);
            }
            else if (!txxx_mode && num_segments >= 1 && strlen ((char *) utf8_string)) {    // if not TXXX, look up APEv2 item name
                for (i = 0; i < NUM_TEXT_TAG_ITEMS; ++i)
                    if (!strncmp ((char *) frame_header, text_tag_table [i].id3_item, 4)) {
                        int value_length = strlen_segments ((char *) utf8_string, num_segments);

                        if (wpc && !WavpackAppendTagItemNoDups (wpc, text_tag_table [i].ape_item, (char *) utf8_string, value_length)) {
                            strcpy (error, WavpackGetErrorMessage (wpc));
                            return -1;
                        }

                        items_imported++;
                        if (bytes_used) *bytes_used += (int) (value_length + strlen (text_tag_table [i].ape_item) + 1);
                        break;
                    }
            }

            free (utf8_string);
        }
        else if (!strncmp ((char *) frame_header, "COMM", 4) && frame_size >= 5 && isalpha (frame_body [1]) &&
            isalpha (frame_body [2]) && isalpha (frame_body [3])) {
                unsigned char *utf8_string = malloc (frame_size * 2);
                int num_segments = ID3v2StringsToUTF8 (frame_body [0], frame_body + 4, frame_size - 4, utf8_string);

                if (num_segments >= 2 && !utf8_string [0] && utf8_string [1]) {
                    int value_length = strlen_segments ((char *) utf8_string + 1, num_segments - 1);

                    if (wpc && !WavpackAppendTagItemNoDups (wpc, "Comment", (char *) utf8_string + 1, value_length)) {
                        strcpy (error, WavpackGetErrorMessage (wpc));
                        return -1;
                    }

                    items_imported++;
                    if (bytes_used) *bytes_used += (int) strlen ("Comment") + 1 + value_length;
                }

                free (utf8_string);
        }
        else if (!strncmp ((char *) frame_header, "APIC", 4) && frame_size >= 8) {
            char *mime_type, *extension, item [80] = "";
            ID3v2TextEncoding encoding = frame_body [0];
            unsigned char *frame_ptr = frame_body + 1;
            int frame_bytes = frame_size - 1;
            unsigned char picture_type;

            // first find the terminator for the mimetype string

            mime_type = (char *) frame_ptr;

            while (frame_bytes-- && *frame_ptr++);

            if (frame_bytes < 0) {
                sprintf (error, "invalid %s tag (unterminated picture mime type)", tag_type);
                return -1;
            }

            if (frame_bytes == 0) {
                sprintf (error, "invalid %s tag (no picture type)", tag_type);
                return -1;
            }

            picture_type = *frame_ptr++;
            frame_bytes--;

            // then, depending on text encoding, search for the 8-bit or 16-bit
            // terminator for the description string (which we ignore for now)

            if (encoding == ISO_8859_1 || encoding == UTF_8)
                while (frame_bytes-- && *frame_ptr++);
            else if (encoding == UTF_16 || encoding == UTF_16BE)
                while (frame_bytes-- && frame_bytes--) {
                   char got_term = !frame_ptr [0] && !frame_ptr [1];
                   frame_ptr += 2;
                   if (got_term)
                       break;
                }
            else {
                sprintf (error, "invalid %s tag (unknown APIC character encoding)", tag_type);
                return -1;
            }

            if (frame_bytes < 0) {
                sprintf (error, "invalid %s tag (unterminated picture description)", tag_type);
                return -1;
            }

            if (frame_bytes < 2) {
                sprintf (error, "invalid %s tag (no picture data)", tag_type);
                return -1;
            }

            if (strstr (mime_type, "jpeg") || strstr (mime_type, "JPEG"))
                extension = ".jpg";
            else if (strstr (mime_type, "png") || strstr (mime_type, "PNG"))
                extension = ".png";
            else if (frame_ptr [0] == 0xFF && frame_ptr [1] == 0xD8)
                extension = ".jpg";
            else if (frame_ptr [0] == 0x89 && frame_ptr [1] == 0x50)
                extension = ".png";
            else
                extension = "";

            if (picture_type < NUM_PICTURE_TYPES)
                sprintf (item, "Cover Art (%s)", picture_types [picture_type]);

            if (item [0]) {
                int binary_tag_size = (int) strlen (item) + (int) strlen (extension) + 1 + frame_bytes;
                char *binary_tag_image = malloc (binary_tag_size);

                strcpy (binary_tag_image, item);
                strcat (binary_tag_image, extension);
                memcpy (binary_tag_image + binary_tag_size - frame_bytes, frame_ptr, frame_bytes);

                if (wpc && !WavpackAppendBinaryTagItem (wpc, item, binary_tag_image, binary_tag_size)) {
                    strcpy (error, WavpackGetErrorMessage (wpc));
                    return -1;
                }

                items_imported++;
                if (bytes_used) *bytes_used += (int) strlen (item) + 1 + binary_tag_size;
                free (binary_tag_image);
            }
        }

        free (frame_body);
    }

    strcpy (error, tag_type);

    return items_imported;
}

// Import specified ID3v2 tag, either v2.3 or v2.4. The WavPack context accepts the tag
// items, and can be NULL for doing a dry-run through the tag. If errors occur then a
// description will be written to "error" (which must be 80 characters) and -1 will be
// returned. If no errors occur then the number of tag items successfully written will
// be returned, or zero in the case of no applicable tags. An optional integer pointer
// can be provided to accept the total number of bytes consumed by the tag (name and
// value), and if no error occurs then the "error" string will be set to the type of
// ID3v2 tag converted (ID3v2.3 or ID3v2.4).

int ImportID3v2 (WavpackContext *wpc, unsigned char *tag_data, int tag_size, char *error, int32_t *bytes_used)
{
    int tag_version = 0, res = 0, res_ss;

    if (bytes_used)
        *bytes_used = 0;

    // look for the ID3 tag in case it's not first thing in the wrapper (like in WAV or DSDIFF files)

    if (tag_size >= 10) {
        unsigned char *cp = tag_data, *ce = cp + tag_size;

        while (cp < ce - 10)
            if (cp [0] == 'I' && cp [1] == 'D' && cp [2] == '3' && (cp [3] == 3 || cp [3] == 4)) {
                tag_version = cp [3];
                tag_size = (int)(ce - cp);
                tag_data = cp;
                break;
            }
            else
                cp++;

        if (cp == ce - 10)      // no tag found is NOT an error
            return 0;
    }

    if (tag_version == 3) {
        // ID3v2.3 tags do not use syncsafe for the frame sizes, so try that first
        res = ImportID3v2_syncsafe (NULL, tag_data, tag_size, error, bytes_used, FALSE);

        if (res > 0)
            return wpc ? ImportID3v2_syncsafe (wpc, tag_data, tag_size, error, bytes_used, FALSE) : res;

        // some malformed ID3v2.3 tags use syncsafe for the frame sizes, so try that as fallback
        res_ss = ImportID3v2_syncsafe (NULL, tag_data, tag_size, error, bytes_used, TRUE);

        if (res_ss > 0)
            return wpc ? ImportID3v2_syncsafe (wpc, tag_data, tag_size, error, bytes_used, TRUE) : res_ss;
    }
    else if (tag_version == 4) {
        // ID3v2.4 tags always use syncsafe for the frame sizes
        res = ImportID3v2_syncsafe (NULL, tag_data, tag_size, error, bytes_used, TRUE);

        if (res > 0)
            return wpc ? ImportID3v2_syncsafe (wpc, tag_data, tag_size, error, bytes_used, TRUE) : res;
    }

    return res;
}

// Calculate the APETAG value length of the given UTF-8 string and the number of NULL-separated
// segments contained in the string. Note that the length will include any embedded NULLs but
// will NOT include the terminating NULL (which is how APETAG strings are stored.

static int strlen_segments (const char *string, int num_segments)
{
    int value_length = 0;

    while (num_segments--)
        value_length += (int) strlen (string + value_length) + 1;

    return value_length - 1;
}

// This is an implementation of POSIX strnlen() which is not universally available

static int strlen_limit (const char *string, int num_bytes)
{
    const char *endstr = memchr (string, 0, num_bytes);
    return endstr ? (int)(endstr - string) : num_bytes;
}

// This function is an extension to the libwavpack function WavpackAppendTagItem(). The library
// function deletes any tag items with the specified name before writing the new item. This
// function will instead read the existing tag value and append those NULL-separated items in
// specified tag that do not occur in the NULL-separated values in the existing value string.
// This prevents duplicate strings from occurring in tags and is required when using wvtag's
// --import-id3 option to avoid the tag values expanding indefinitely.

static int WavpackAppendTagItemNoDups (WavpackContext *wpc, const char *item, const char *value, int vsize)
{
    const int existing_vsize = WavpackGetTagItem (wpc, item, NULL, 0);
    const char *value_end = value + vsize;

    if (existing_vsize) {
        int new_vsize = existing_vsize, max_vsize = new_vsize + 1 + vsize, retval = TRUE;
        char *new_value = malloc (max_vsize), *new_value_end = new_value + new_vsize;

        WavpackGetTagItem (wpc, item, new_value, existing_vsize + 1);

        // loop through the value segments passed in and search for them in existing value

        while (value < value_end && *value) {
            int candidate_len = strlen_limit (value, (int)(value_end - value)), found = 0;
            char *search_value = new_value;

            while (search_value < new_value_end && *search_value) {
                int search_len = strlen_limit (search_value, (int)(new_value_end - search_value));

                if (candidate_len != search_len || memcmp (value, search_value, search_len))
                    search_value += search_len + 1;
                else {
                    found = 1;
                    break;
                }
            }

            // if candidate string wasn't found in existing strings, append it to the end

            if (!found) {
                *new_value_end++ = 0;
                memcpy (new_value_end, value, candidate_len);
                new_value_end += candidate_len;
                new_vsize += candidate_len + 1;
            }

            value += candidate_len + 1;
        }

        // if we appended anything, write the value back

        if (new_vsize != existing_vsize)
            retval = WavpackAppendTagItem (wpc, item, new_value, new_vsize);

        free (new_value);
        return retval;
    }
    else
        return WavpackAppendTagItem (wpc, item, value, vsize);
}

// Convert ID3v2 text according to the specified encoding. ID3v2 text may contain embedded NULLs,
// and so the conversion continues until the source length is exhausted or an empty string is
// encountered (except the first string, which CAN be empty). The number of segments is returned
// and this value is one greater than the number of embedded NULLs. It is assumed that there is
// enough room available at "dst" to accommodate the longest possible string and the easiest
// way to ensure that is for the available space to be twice the input size (and that's what's
// assumed here).

static int ID3v2StringsToUTF8 (ID3v2TextEncoding encoding, unsigned char *src, int src_length, unsigned char *dst)
{
    unsigned char *dst_end = dst + src_length * 2;
    int num_segments = 0;

    if ((encoding == ISO_8859_1 || encoding == UTF_8) && src_length >= 1) {
        unsigned char *fp = src, *fe = src + src_length;

        while (fp < fe && (!num_segments || *fp) && (fe - fp < dst_end - dst)) {
            int i = 0;

            while (fp < fe)
                if (!(dst [i++] = *fp++))
                    break;

            if (fp == fe)
                dst [i] = 0;

            if (encoding == ISO_8859_1)
                Latin1ToUTF8 (dst, (int)(dst_end - dst));

            dst = dst + strlen ((char *) dst) + 1;
            num_segments++;
        }
    }
    else if ((encoding == UTF_16 && src_length >= 4) || (encoding == UTF_16BE && src_length >= 2)) {
        unsigned char *fp = src, *fe = src + src_length;
        char big_endian = 0, little_endian = 0;
        uint16_t *wide_string;

        if (encoding == UTF_16BE || (fp [0] == 0xFE && fp [1] == 0xFF))
            big_endian = 1;
        else if (fp [0] == 0xFF && fp [1] == 0xFE)
            little_endian = 1;

        if (!big_endian && !little_endian)
            return -1;

        wide_string = malloc (src_length + 2);

        while (fp <= fe - 2 && (!num_segments || fp [0] || fp [1])) {
            int i = 0;

            while (fp <= fe - 2) {
                uint16_t wchar = little_endian ? fp [0] | (fp [1] << 8) : fp [1] | (fp [0] << 8);

                fp += 2;

                if ((wchar != 0xFEFF) && !(wide_string [i++] = wchar))
                    break;
            }

            if (fp > fe - 2)
                wide_string [i] = 0;

            WideCharToUTF8 (wide_string, dst, (int)(dst_end - dst));
            dst = dst + strlen ((char *) dst) + 1;
            num_segments++;
        }

        free (wide_string);
    }
    else
        return -1;

    return num_segments;
}

// Convert the Unicode wide-format string into a UTF-8 string using no more
// than the specified buffer length. The wide-format string must be NULL
// terminated and the resulting string will be NULL terminated. The actual
// number of characters converted (not counting terminator) is returned, which
// may be less than the number of characters in the wide string if the buffer
// length is exceeded.

static int WideCharToUTF8 (const uint16_t *Wide, unsigned char *pUTF8, int len)
{
    const uint16_t *pWide = Wide;
    int outndx = 0;

    while (*pWide) {
        if (*pWide < 0x80 && outndx + 1 < len)
            pUTF8 [outndx++] = (unsigned char) *pWide++;
        else if (*pWide < 0x800 && outndx + 2 < len) {
            pUTF8 [outndx++] = (unsigned char) (0xc0 | ((*pWide >> 6) & 0x1f));
            pUTF8 [outndx++] = (unsigned char) (0x80 | (*pWide++ & 0x3f));
        }
        else if (outndx + 3 < len) {
            pUTF8 [outndx++] = (unsigned char) (0xe0 | ((*pWide >> 12) & 0xf));
            pUTF8 [outndx++] = (unsigned char) (0x80 | ((*pWide >> 6) & 0x3f));
            pUTF8 [outndx++] = (unsigned char) (0x80 | (*pWide++ & 0x3f));
        }
        else
            break;
    }

    pUTF8 [outndx] = 0;
    return (int)(pWide - Wide);
}

// Convert a Latin1 string into its Unicode UTF-8 format equivalent. The
// conversion is done in-place so the maximum length of the string buffer must
// be specified because the string may become longer or shorter. If the
// resulting string will not fit in the specified buffer size then it is
// truncated.

#ifdef _WIN32

#include <windows.h>

static void Latin1ToUTF8 (void *string, int len)
{
    int max_chars = (int) strlen (string);
    uint16_t *temp = (uint16_t *) malloc ((max_chars + 1) * sizeof (uint16_t));

    MultiByteToWideChar (28591, 0, string, -1, temp, max_chars + 1);
    WideCharToUTF8 (temp, (unsigned char *) string, len);
    free (temp);
}

#else

#include <iconv.h>

static void Latin1ToUTF8 (void *string, int len)
{
    char *temp = malloc (len);
    char *outp = temp;
    char *inp = string;
    size_t insize = 0;
    size_t outsize = len - 1;
    int err = 0;
    iconv_t converter;

    memset(temp, 0, len);

    insize = strlen (string);
    converter = iconv_open ("UTF-8", "ISO-8859-1");

    if (converter != (iconv_t) -1) {
        err = iconv (converter, &inp, &insize, &outp, &outsize);
        iconv_close (converter);
    }
    else
        err = -1;

    if (err == -1) {
        free(temp);
        return;
    }

    memmove (string, temp, len);
    free (temp);
}

#endif

