////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//              Copyright (c) 1998 - 2013 Conifer Software.               //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// tag_utils.c

// This module provides the high-level API for creating, reading and editing
// APEv2 tags on WavPack files. Read-only support is also provided for ID3v1
// tags, but their use is not recommended.

#include <stdlib.h>
#include <string.h>

#include "wavpack_local.h"

#ifndef __WATCOMC__
#ifdef _WIN32
#define stricmp(x,y) _stricmp(x,y)
#else
#define stricmp strcasecmp
#endif
#endif

static int get_ape_tag_item (M_Tag *m_tag, const char *item, char *value, int size, int type);
static int get_id3_tag_item (M_Tag *m_tag, const char *item, char *value, int size);
static int get_ape_tag_item_indexed (M_Tag *m_tag, int index, char *item, int size, int type);
static int get_id3_tag_item_indexed (M_Tag *m_tag, int index, char *item, int size);
static int append_ape_tag_item (WavpackContext *wpc, const char *item, const char *value, int vsize, int type);
static int build_ape_tag_index (M_Tag *m_tag);
static unsigned char *find_ape_tag_item (M_Tag *m_tag, const char *item, int type);
static int compare_ape_tag_items (const void *p1, const void *p2);
static int get_ape_tag_type (const unsigned char *item);
static void clear_ape_tag_cache (M_Tag *m_tag);
static int write_tag_blockout (WavpackContext *wpc);
static int write_tag_reader (WavpackContext *wpc);
static void tagcpy (char *dest, char *src, int tag_size);
static int tagdata (char *src, int tag_size);

//////////////////// Global functions part of external API /////////////////////////

// Count and return the total number of tag items in the specified file.

int WavpackGetNumTagItems (WavpackContext *wpc)
{
    int i = 0;

    while (WavpackGetTagItemIndexed (wpc, i, NULL, 0))
        ++i;

    return i;
}

// Count and return the total number of binary tag items in the specified file. This applies
// only to APEv2 tags and was implemented as a separate function to avoid breaking the old API.

int WavpackGetNumBinaryTagItems (WavpackContext *wpc)
{
    int i = 0;

    while (WavpackGetBinaryTagItemIndexed (wpc, i, NULL, 0))
        ++i;

    return i;
}

// Attempt to get the specified item from the specified file's ID3v1 or APEv2
// tag. The "size" parameter specifies the amount of space available at "value",
// if the desired item will not fit in this space then ellipses (...) will
// be appended and the string terminated. Only text data are supported. The
// actual length of the string is returned (or 0 if no matching value found).
// Note that with APEv2 tags the length might not be the same as the number of
// characters because UTF-8 encoding is used. Also, APEv2 tags can have multiple
// (NULL separated) strings for a single value (this is why the length is
// returned). If this function is called with a NULL "value" pointer (or a
// zero "length") then only the actual length of the value data is returned
// (not counting the terminating NULL). This can be used to determine the
// actual memory to be allocated beforehand.

int WavpackGetTagItem (WavpackContext *wpc, const char *item, char *value, int size)
{
    M_Tag *m_tag = &wpc->m_tag;

    if (value && size)
        *value = 0;

    if (m_tag->ape_tag_hdr.ID [0] == 'A')
        return get_ape_tag_item (m_tag, item, value, size, APE_TAG_TYPE_TEXT);
    else if (m_tag->id3_tag.tag_id [0] == 'T')
        return get_id3_tag_item (m_tag, item, value, size);
    else
        return 0;
}

// Attempt to get the specified binary item from the specified file's APEv2
// tag. The "size" parameter specifies the amount of space available at "value".
// If the desired item will not fit in this space then nothing will be copied
// and 0 will be returned, otherwise the actual size will be returned. If this
// function is called with a NULL "value" pointer (or a zero "length") then only
// the actual length of the value data is returned and can be used to determine
// the actual memory to be allocated beforehand.

int WavpackGetBinaryTagItem (WavpackContext *wpc, const char *item, char *value, int size)
{
    M_Tag *m_tag = &wpc->m_tag;

    if (value && size)
        *value = 0;

    if (m_tag->ape_tag_hdr.ID [0] == 'A')
        return get_ape_tag_item (m_tag, item, value, size, APE_TAG_TYPE_BINARY);
    else
        return 0;
}

// This function looks up the tag item name by index and is used when the
// application wants to access all the items in the file's ID3v1 or APEv2 tag.
// Note that this function accesses only the item's name; WavpackGetTagItem()
// still must be called to get the actual value. The "size" parameter specifies
// the amount of space available at "item", if the desired item will not fit in
// this space then ellipses (...) will be appended and the string terminated.
// The actual length of the string is returned (or 0 if no item exists for
// index). If this function is called with a NULL "value" pointer (or a
// zero "length") then only the actual length of the item name is returned
// (not counting the terminating NULL). This can be used to determine the
// actual memory to be allocated beforehand. For binary tag values use the
// otherwise identical WavpackGetBinaryTagItemIndexed ();

int WavpackGetTagItemIndexed (WavpackContext *wpc, int index, char *item, int size)
{
    M_Tag *m_tag = &wpc->m_tag;

    if (item && size)
        *item = 0;

    if (m_tag->ape_tag_hdr.ID [0] == 'A')
        return get_ape_tag_item_indexed (m_tag, index, item, size, APE_TAG_TYPE_TEXT);
    else if (m_tag->id3_tag.tag_id [0] == 'T')
        return get_id3_tag_item_indexed (m_tag, index, item, size);
    else
        return 0;
}

int WavpackGetBinaryTagItemIndexed (WavpackContext *wpc, int index, char *item, int size)
{
    M_Tag *m_tag = &wpc->m_tag;

    if (item && size)
        *item = 0;

    if (m_tag->ape_tag_hdr.ID [0] == 'A')
        return get_ape_tag_item_indexed (m_tag, index, item, size, APE_TAG_TYPE_BINARY);
    else
        return 0;
}

// These two functions are used to append APEv2 tags to WavPack files; one is
// for text values (UTF-8 encoded) and the other is for binary values. If no tag
// has been started, then an empty one will be allocated first. When finished,
// use WavpackWriteTag() to write the completed tag to the file. The purpose of
// the passed size parameter is obvious for binary values, but might not be for
// text values. Keep in mind that APEv2 text values can have multiple values
// that are NULL separated, so the size is required to know the extent of the
// value (although the final terminating NULL is not included in the passed
// size). If the specified item already exists, it will be replaced with the
// new value. ID3v1 tags are not supported.

int WavpackAppendTagItem (WavpackContext *wpc, const char *item, const char *value, int vsize)
{
    while (WavpackDeleteTagItem (wpc, item));
    return append_ape_tag_item (wpc, item, value, vsize, APE_TAG_TYPE_TEXT);
}

int WavpackAppendBinaryTagItem (WavpackContext *wpc, const char *item, const char *value, int vsize)
{
    while (WavpackDeleteTagItem (wpc, item));
    return append_ape_tag_item (wpc, item, value, vsize, APE_TAG_TYPE_BINARY);
}

// Delete the specified tag item from the APEv2 tag on the specified WavPack file
// (fields cannot be deleted from ID3v1 tags). A return value of TRUE indicates
// that the item was found and successfully deleted.

int WavpackDeleteTagItem (WavpackContext *wpc, const char *item)
{
    M_Tag *m_tag = &wpc->m_tag;

    if (m_tag->ape_tag_hdr.ID [0] == 'A') {
        unsigned char *p = m_tag->ape_tag_data;
        unsigned char *q = p + m_tag->ape_tag_hdr.length - sizeof (APE_Tag_Hdr);
        int i;

        for (i = 0; i < m_tag->ape_tag_hdr.item_count && q - p > 8; ++i) {
            int vsize, isize;

            vsize = p[0] + (p[1] << 8) + (p[2] << 16) + ((uint32_t) p[3] << 24); p += 8;   // skip flags because we don't need them
            for (isize = 0; p + isize < q && p[isize]; ++isize);

            if (vsize < 0 || vsize > m_tag->ape_tag_hdr.length || p + isize + vsize + 1 > q)
                break;

            if (isize && vsize && !stricmp (item, (char *) p)) {
                unsigned char *d = p - 8;

                p += isize + vsize + 1;

                while (p < q)
                    *d++ = *p++;

                m_tag->ape_tag_hdr.length = (int32_t)(d - m_tag->ape_tag_data) + sizeof (APE_Tag_Hdr);
                m_tag->ape_tag_hdr.item_count--;
                clear_ape_tag_cache (m_tag);
                return 1;
            }
            else
                p += isize + vsize + 1;
        }
    }

    return 0;
}

// Once a APEv2 tag has been created with WavpackAppendTag(), this function is
// used to write the completed tag to the end of the WavPack file. Note that
// this function uses the same "blockout" function that is used to write
// regular WavPack blocks, although that's where the similarity ends. It is also
// used to write tags that have been edited on existing files.

int WavpackWriteTag (WavpackContext *wpc)
{
    if (wpc->blockout)      // this is the case for creating fresh WavPack files
        return write_tag_blockout (wpc);
    else                    // otherwise we are editing existing tags (OPEN_EDIT_TAGS)
        return write_tag_reader (wpc);
}

////////////////////////// local static functions /////////////////////////////

static int get_ape_tag_item (M_Tag *m_tag, const char *item, char *value, int size, int type)
{
    unsigned char *p = m_tag->ape_tag_data;
    unsigned char *q = p + m_tag->ape_tag_hdr.length - sizeof (APE_Tag_Hdr);
    int i;

    if (type >= APE_TAG_TYPE_TEXT && type <= APE_TAG_TYPE_BINARY &&
        m_tag->ape_tag_cursor [type].item && build_ape_tag_index (m_tag)) {
        p = find_ape_tag_item (m_tag, item, type);

        if (!p)
            return 0;
    }

    for (i = 0; i < m_tag->ape_tag_hdr.item_count && q - p > 8; ++i) {
        int vsize, flags, isize;

        vsize = p[0] + (p[1] << 8) + (p[2] << 16) + ((uint32_t) p[3] << 24); p += 4;
        flags = p[0] + (p[1] << 8) + (p[2] << 16) + ((uint32_t) p[3] << 24); p += 4;
        for (isize = 0; p + isize < q && p[isize]; ++isize);

        if (vsize < 0 || vsize > m_tag->ape_tag_hdr.length || p + isize + vsize + 1 > q)
            break;

        if (isize && vsize && !stricmp (item, (char *) p) && ((flags & 6) >> 1) == type) {

            if (!value || !size)
                return vsize;

            if (type == APE_TAG_TYPE_BINARY) {
                if (vsize <= size) {
                    memcpy (value, p + isize + 1, vsize);
                    return vsize;
                }
                else
                    return 0;
            }
            else if (vsize < size) {
                memcpy (value, p + isize + 1, vsize);
                value [vsize] = 0;
                return vsize;
            }
            else if (size >= 4) {
                memcpy (value, p + isize + 1, size - 1);
                value [size - 4] = value [size - 3] = value [size - 2] = '.';
                value [size - 1] = 0;
                return size - 1;
            }
            else
                return 0;
        }
        else
            p += isize + vsize + 1;
    }

    return 0;
}

static int get_id3_tag_item (M_Tag *m_tag, const char *item, char *value, int size)
{
    char lvalue [64];
    int len;

    lvalue [0] = 0;

    if (!stricmp (item, "title"))
        tagcpy (lvalue, m_tag->id3_tag.title, sizeof (m_tag->id3_tag.title));
    else if (!stricmp (item, "artist"))
        tagcpy (lvalue, m_tag->id3_tag.artist, sizeof (m_tag->id3_tag.artist));
    else if (!stricmp (item, "album"))
        tagcpy (lvalue, m_tag->id3_tag.album, sizeof (m_tag->id3_tag.album));
    else if (!stricmp (item, "year"))
        tagcpy (lvalue, m_tag->id3_tag.year, sizeof (m_tag->id3_tag.year));
    else if (!stricmp (item, "comment"))
        tagcpy (lvalue, m_tag->id3_tag.comment, sizeof (m_tag->id3_tag.comment));
    else if (!stricmp (item, "track") && m_tag->id3_tag.comment [29] && !m_tag->id3_tag.comment [28])
        sprintf (lvalue, "%d", m_tag->id3_tag.comment [29]);
    else
        return 0;

    len = (int) strlen (lvalue);

    if (!value || !size)
        return len;

    if (len < size) {
        strcpy (value, lvalue);
        return len;
    }
    else if (size >= 4) {
        strncpy (value, lvalue, size - 1);
        value [size - 4] = value [size - 3] = value [size - 2] = '.';
        value [size - 1] = 0;
        return size - 1;
    }
    else
        return 0;
}

static int get_ape_tag_item_indexed (M_Tag *m_tag, int index, char *item, int size, int type)
{
    unsigned char *p = m_tag->ape_tag_data;
    unsigned char *q = p + m_tag->ape_tag_hdr.length - sizeof (APE_Tag_Hdr);
    int i, item_index = 0;

    if (index < 0 || type < APE_TAG_TYPE_TEXT || type > APE_TAG_TYPE_BINARY)
        return 0;

    if (m_tag->ape_tag_cursor [type].item && index >= m_tag->ape_tag_cursor [type].index) {
        p = m_tag->ape_tag_cursor [type].item;
        item_index = m_tag->ape_tag_cursor [type].index;
        i = m_tag->ape_tag_cursor [type].item_number;
    }
    else
        i = 0;

    for (; i < m_tag->ape_tag_hdr.item_count && q - p > 8; ++i) {
        unsigned char *item_start = p;
        int vsize, flags, isize;

        vsize = p[0] + (p[1] << 8) + (p[2] << 16) + ((uint32_t) p[3] << 24); p += 4;
        flags = p[0] + (p[1] << 8) + (p[2] << 16) + ((uint32_t) p[3] << 24); p += 4;
        for (isize = 0; p + isize < q && p[isize]; ++isize);

        if (vsize < 0 || vsize > m_tag->ape_tag_hdr.length || p + isize + vsize + 1 > q)
            break;

        if (isize && vsize && ((flags & 6) >> 1) == type && item_index++ == index) {
            m_tag->ape_tag_cursor [type].item = item_start;
            m_tag->ape_tag_cursor [type].index = index;
            m_tag->ape_tag_cursor [type].item_number = i;

            if (!item || !size)
                return isize;

            if (isize < size) {
                memcpy (item, p, isize);
                item [isize] = 0;
                return isize;
            }
            else if (size >= 4) {
                memcpy (item, p, size - 1);
                item [size - 4] = item [size - 3] = item [size - 2] = '.';
                item [size - 1] = 0;
                return size - 1;
            }
            else
                return 0;
        }
        else
            p += isize + vsize + 1;
    }

    return 0;
}

static int build_ape_tag_index (M_Tag *m_tag)
{
    unsigned char *p = m_tag->ape_tag_data;
    unsigned char *q = p + m_tag->ape_tag_hdr.length - sizeof (APE_Tag_Hdr);
    int data_length = m_tag->ape_tag_hdr.length - sizeof (APE_Tag_Hdr);
    int i, max_items, num_items = 0;

    if (m_tag->ape_tag_index_state)
        return m_tag->ape_tag_index_state > 0;

    max_items = m_tag->ape_tag_hdr.item_count;

    if (max_items < 0 || max_items > data_length / 11)
        max_items = data_length / 11;

    if (max_items) {
        m_tag->ape_tag_index = malloc ((size_t) max_items * sizeof (*m_tag->ape_tag_index));

        if (!m_tag->ape_tag_index) {
            m_tag->ape_tag_index_state = -1;
            return FALSE;
        }

        for (i = 0; i < m_tag->ape_tag_hdr.item_count && q - p > 8; ++i) {
            unsigned char *item_start = p;
            int vsize, flags, isize;

            vsize = p[0] + (p[1] << 8) + (p[2] << 16) + ((uint32_t) p[3] << 24); p += 4;
            flags = p[0] + (p[1] << 8) + (p[2] << 16) + ((uint32_t) p[3] << 24); p += 4;
            for (isize = 0; p + isize < q && p[isize]; ++isize);

            if (vsize < 0 || vsize > m_tag->ape_tag_hdr.length || p + isize + vsize + 1 > q)
                break;

            if (isize && vsize && ((flags & 6) >> 1) <= APE_TAG_TYPE_BINARY)
                m_tag->ape_tag_index [num_items++] = item_start;

            p += isize + vsize + 1;
        }

        qsort (m_tag->ape_tag_index, num_items, sizeof (*m_tag->ape_tag_index), compare_ape_tag_items);

        if (!num_items) {
            free (m_tag->ape_tag_index);
            m_tag->ape_tag_index = NULL;
        }
    }

    m_tag->ape_tag_index_count = num_items;
    m_tag->ape_tag_index_state = 1;
    return TRUE;
}

static unsigned char *find_ape_tag_item (M_Tag *m_tag, const char *item, int type)
{
    int high = m_tag->ape_tag_index_count, low = 0;

    while (low < high) {
        unsigned char *p;
        int result;
        int mid = low + (high - low) / 2;

        p = m_tag->ape_tag_index [mid];
        result = stricmp ((char *) p + 8, item);

        if (result < 0 || (!result && get_ape_tag_type (p) < type))
            low = mid + 1;
        else
            high = mid;
    }

    if (low < m_tag->ape_tag_index_count) {
        unsigned char *p = m_tag->ape_tag_index [low];

        if (!stricmp ((char *) p + 8, item) && get_ape_tag_type (p) == type)
            return p;
    }

    return NULL;
}

static int compare_ape_tag_items (const void *p1, const void *p2)
{
    unsigned char *item1 = * (unsigned char **) p1;
    unsigned char *item2 = * (unsigned char **) p2;
    int type1, type2;
    int result = stricmp ((char *) item1 + 8, (char *) item2 + 8);

    if (result)
        return result;

    type1 = get_ape_tag_type (item1);
    type2 = get_ape_tag_type (item2);

    if (type1 != type2)
        return type1 - type2;
    else if (item1 < item2)
        return -1;
    else
        return item1 != item2;
}

static int get_ape_tag_type (const unsigned char *item)
{
    uint32_t flags = item[4] + (item[5] << 8) + (item[6] << 16) + ((uint32_t) item[7] << 24);

    return (flags & 6) >> 1;
}

static void clear_ape_tag_cache (M_Tag *m_tag)
{
    free (m_tag->ape_tag_index);
    m_tag->ape_tag_index = NULL;
    m_tag->ape_tag_index_count = m_tag->ape_tag_index_state = 0;
    memset (m_tag->ape_tag_cursor, 0, sizeof (m_tag->ape_tag_cursor));
}

static int get_id3_tag_item_indexed (M_Tag *m_tag, int index, char *item, int size)
{
    char lvalue [16];
    int len;

    lvalue [0] = 0;

    if (tagdata (m_tag->id3_tag.title, sizeof (m_tag->id3_tag.title)) && !index--)
        strcpy (lvalue, "Title");
    else if (tagdata (m_tag->id3_tag.artist, sizeof (m_tag->id3_tag.artist)) && !index--)
        strcpy (lvalue, "Artist");
    else if (tagdata (m_tag->id3_tag.album, sizeof (m_tag->id3_tag.album)) && !index--)
        strcpy (lvalue, "Album");
    else if (tagdata (m_tag->id3_tag.year, sizeof (m_tag->id3_tag.year)) && !index--)
        strcpy (lvalue, "Year");
    else if (tagdata (m_tag->id3_tag.comment, sizeof (m_tag->id3_tag.comment)) && !index--)
        strcpy (lvalue, "Comment");
    else if (m_tag->id3_tag.comment [29] && !m_tag->id3_tag.comment [28] && !index--)
        strcpy (lvalue, "Track");
    else
        return 0;

    len = (int) strlen (lvalue);

    if (!item || !size)
        return len;

    if (len < size) {
        strcpy (item, lvalue);
        return len;
    }
    else if (size >= 4) {
        strncpy (item, lvalue, size - 1);
        item [size - 4] = item [size - 3] = item [size - 2] = '.';
        item [size - 1] = 0;
        return size - 1;
    }
    else
        return 0;
}

static int append_ape_tag_item (WavpackContext *wpc, const char *item, const char *value, int vsize, int type)
{
    M_Tag *m_tag = &wpc->m_tag;
    int isize = (int) strlen (item);

    if (!m_tag->ape_tag_hdr.ID [0]) {
        memcpy (m_tag->ape_tag_hdr.ID, "APETAGEX", sizeof (m_tag->ape_tag_hdr.ID));
        m_tag->ape_tag_hdr.version = 2000;
        m_tag->ape_tag_hdr.length = sizeof (m_tag->ape_tag_hdr);
        m_tag->ape_tag_hdr.item_count = 0;
        m_tag->ape_tag_hdr.flags = APE_TAG_CONTAINS_HEADER;  // we will include header on tags we originate
    }

    if (m_tag->ape_tag_hdr.ID [0] == 'A') {
        int new_item_len = vsize + isize + 9, flags = type << 1;
        unsigned char *p;

        if (m_tag->ape_tag_hdr.length + new_item_len > APE_TAG_MAX_LENGTH) {
            strcpy (wpc->error_message, "APEv2 tag exceeds maximum allowed length!");
            return FALSE;
        }

        m_tag->ape_tag_hdr.item_count++;
        m_tag->ape_tag_hdr.length += new_item_len;
        p = m_tag->ape_tag_data = (unsigned char*)realloc (m_tag->ape_tag_data, m_tag->ape_tag_hdr.length);
        clear_ape_tag_cache (m_tag);
        p += m_tag->ape_tag_hdr.length - sizeof (APE_Tag_Hdr) - new_item_len;

        *p++ = (unsigned char) vsize;
        *p++ = (unsigned char) (vsize >> 8);
        *p++ = (unsigned char) (vsize >> 16);
        *p++ = (unsigned char) (vsize >> 24);

        *p++ = (unsigned char) flags;
        *p++ = (unsigned char) (flags >> 8);
        *p++ = (unsigned char) (flags >> 16);
        *p++ = (unsigned char) (flags >> 24);

        strcpy ((char *) p, item);
        p += isize + 1;
        memcpy (p, value, vsize);

        return TRUE;
    }
    else
        return FALSE;
}

// Append the stored APEv2 tag to the file being created using the "blockout" function callback.

static int write_tag_blockout (WavpackContext *wpc)
{
    M_Tag *m_tag = &wpc->m_tag;
    int result = TRUE;

    if (m_tag->ape_tag_hdr.ID [0] == 'A' && m_tag->ape_tag_hdr.item_count) {

        // only write header if it's specified in the flags

        if (m_tag->ape_tag_hdr.flags & APE_TAG_CONTAINS_HEADER) {
            m_tag->ape_tag_hdr.flags |= APE_TAG_THIS_IS_HEADER;
            WavpackNativeToLittleEndian (&m_tag->ape_tag_hdr, APE_Tag_Hdr_Format);
            result = wpc->blockout (wpc->wv_out, &m_tag->ape_tag_hdr, sizeof (m_tag->ape_tag_hdr));
            WavpackLittleEndianToNative (&m_tag->ape_tag_hdr, APE_Tag_Hdr_Format);
        }

        if (m_tag->ape_tag_hdr.length > sizeof (m_tag->ape_tag_hdr))
            result = wpc->blockout (wpc->wv_out, m_tag->ape_tag_data, m_tag->ape_tag_hdr.length - sizeof (m_tag->ape_tag_hdr));

        m_tag->ape_tag_hdr.flags &= ~APE_TAG_THIS_IS_HEADER;    // this is NOT header
        WavpackNativeToLittleEndian (&m_tag->ape_tag_hdr, APE_Tag_Hdr_Format);
        result = wpc->blockout (wpc->wv_out, &m_tag->ape_tag_hdr, sizeof (m_tag->ape_tag_hdr));
        WavpackLittleEndianToNative (&m_tag->ape_tag_hdr, APE_Tag_Hdr_Format);
    }

    if (!result)
        strcpy (wpc->error_message, "can't write WavPack data, disk probably full!");

    return result;
}

// Write the [potentially] edited tag to the existing WavPack file using the reader callback functions.

static int write_tag_reader (WavpackContext *wpc)
{
    M_Tag *m_tag = &wpc->m_tag;
    int32_t tag_size = 0;
    int result;

    // before we write an edited (or new) tag into an existing file, make sure it's safe and possible

    if (m_tag->tag_begins_file) {
        strcpy (wpc->error_message, "can't edit tags located at the beginning of files!");
        return FALSE;
    }

    if (!wpc->reader->can_seek (wpc->wv_in)) {
        strcpy (wpc->error_message, "can't edit tags on pipes or unseekable files!");
        return FALSE;
    }

    if (!(wpc->open_flags & OPEN_EDIT_TAGS)) {
        strcpy (wpc->error_message, "can't edit tags without OPEN_EDIT_TAGS flag!");
        return FALSE;
    }

    if (m_tag->ape_tag_hdr.ID [0] == 'A' && m_tag->ape_tag_hdr.item_count &&
        m_tag->ape_tag_hdr.length > sizeof (m_tag->ape_tag_hdr))
            tag_size = m_tag->ape_tag_hdr.length;

    // only write header if it's specified in the flags

    if (tag_size && (m_tag->ape_tag_hdr.flags & APE_TAG_CONTAINS_HEADER))
        tag_size += sizeof (m_tag->ape_tag_hdr);

    result = !wpc->reader->set_pos_rel (wpc->wv_in, m_tag->tag_file_pos, SEEK_END);

    if (result && tag_size < -m_tag->tag_file_pos && !wpc->reader->truncate_here) {
        int nullcnt = (int) (-m_tag->tag_file_pos - tag_size);
        char zero = 0;

        while (nullcnt--)
            wpc->reader->write_bytes (wpc->wv_in, &zero, 1);
    }

    if (result && tag_size) {
        if (m_tag->ape_tag_hdr.flags & APE_TAG_CONTAINS_HEADER) {
            m_tag->ape_tag_hdr.flags |= APE_TAG_THIS_IS_HEADER;
            WavpackNativeToLittleEndian (&m_tag->ape_tag_hdr, APE_Tag_Hdr_Format);
            result = (wpc->reader->write_bytes (wpc->wv_in, &m_tag->ape_tag_hdr, sizeof (m_tag->ape_tag_hdr)) == sizeof (m_tag->ape_tag_hdr));
            WavpackLittleEndianToNative (&m_tag->ape_tag_hdr, APE_Tag_Hdr_Format);
        }

        result = (wpc->reader->write_bytes (wpc->wv_in, m_tag->ape_tag_data, m_tag->ape_tag_hdr.length - sizeof (m_tag->ape_tag_hdr)) == sizeof (m_tag->ape_tag_hdr));
        m_tag->ape_tag_hdr.flags &= ~APE_TAG_THIS_IS_HEADER;    // this is NOT header
        WavpackNativeToLittleEndian (&m_tag->ape_tag_hdr, APE_Tag_Hdr_Format);
        result = (wpc->reader->write_bytes (wpc->wv_in, &m_tag->ape_tag_hdr, sizeof (m_tag->ape_tag_hdr)) == sizeof (m_tag->ape_tag_hdr));
        WavpackLittleEndianToNative (&m_tag->ape_tag_hdr, APE_Tag_Hdr_Format);
    }

    if (result && tag_size < -m_tag->tag_file_pos && wpc->reader->truncate_here)
        result = !wpc->reader->truncate_here (wpc->wv_in);

    if (!result)
        strcpy (wpc->error_message, "can't write WavPack data, disk probably full!");

    return result;
}

// Copy the specified ID3v1 tag value (with specified field size) from the
// source pointer to the destination, eliminating leading spaces and trailing
// spaces and nulls.

static void tagcpy (char *dest, char *src, int tag_size)
{
    char *s1 = src, *s2 = src + tag_size - 1;

    if (*s2 && !s2 [-1])
        s2--;

    while (s1 <= s2)
        if (*s1 == ' ')
            ++s1;
        else if (!*s2 || *s2 == ' ')
            --s2;
        else
            break;

    while (*s1 && s1 <= s2)
        *dest++ = *s1++;

    *dest = 0;
}

static int tagdata (char *src, int tag_size)
{
    char *s1 = src, *s2 = src + tag_size - 1;

    if (*s2 && !s2 [-1])
        s2--;

    while (s1 <= s2)
        if (*s1 == ' ')
            ++s1;
        else if (!*s2 || *s2 == ' ')
            --s2;
        else
            break;

    return (*s1 && s1 <= s2);
}
